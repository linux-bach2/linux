// SPDX-License-Identifier: GPL-2.0-only
/*
 * Initial Hi1102 SDIO transport. Register/ROM protocol from Huawei's
 * platform/oal/oal_sdio.h and platform/{driver/plat_sdio,pm/plat_firmware}.c.
 * No wireless interface or host MAC implementation yet.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define HI1102_FIFO		0x00
#define HI1102_INT_STATUS	0x08
#define HI1102_INT_ENABLE	0x09
#define HI1102_XFER_COUNT		0x0c
#define HI1102_MSG_FROM_DEV	0x28
#define HI1102_MSG_HIGH		0x2b
#define HI1102_INT_DREADY		BIT(0)
#define HI1102_INT_MESSAGE	BIT(2)
#define HI1102_INT_MASK		GENMASK(2, 0)
#define HI1102_WAKE_CONTROL	0xf0
#define HI1102_ROM_MAX_REPLY	4096

enum hi1102_boot_type { HI_CMD, HI_FILE, HI_CALI, HI_QUIT };
struct hi1102_boot_op {
	enum hi1102_boot_type type;
	const char *command;
	u32 value;
	bool v100_only;
};
#include "hi1102_boot.h"

/* Called after host claim and successful 512-byte block-size setup. */
static unsigned int hi1102_transfer_limit(struct sdio_func *func)
{
	struct mmc_host *host = func->card->host;
	u64 limit, wire_cap;

	if (func->cur_blksize != 512 || !host->actual_clock ||
	    host->ios.bus_width > MMC_BUS_WIDTH_8)
		return 0;
	limit = min_t(u64, host->max_req_size,
		      min_t(u32, host->max_blk_count, 511) * 512);
	limit = min_t(u64, limit, (u64)host->max_seg_size * host->max_segs);
	limit = min_t(u64, limit, 20 * SZ_32K); /* ROM's descriptor capacity */
	limit = min_t(u64, limit, SZ_128K); /* bounded bulk-transfer experiment */
	/* Leave margin within the SDIO one-second access timeout budget. */
	wire_cap = (u64)host->actual_clock * (1U << host->ios.bus_width) * 3 / 32;
	limit = min(limit, wire_cap);
	return round_down(limit, 512U);
}

/* Caller holds MMC; allocation is at least max(TX limit,4096)+1 DMA-safe bytes. */
static int hi1102_rom_read(struct sdio_func *func, u8 *buf, u32 *count)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(1000);
	u8 status;
	int ret;

	for (;;) {
		status = sdio_readb(func, HI1102_INT_STATUS, &ret);
		if (ret)
			return ret;
		if (status & HI1102_INT_DREADY)
			break;
		if (time_after_eq(jiffies, deadline)) {
			dev_err(&func->dev, "ROM response timed out, status=%#x\n", status);
			return -ETIMEDOUT;
		}
		usleep_range(1000, 2000);
	}
	sdio_writeb(func, status & HI1102_INT_MASK, HI1102_INT_STATUS, &ret);
	if (ret)
		return ret;
	*count = sdio_readl(func, HI1102_XFER_COUNT, &ret);
	if (ret)
		return ret;
	if (!*count || *count > HI1102_ROM_MAX_REPLY) {
		dev_err(&func->dev, "invalid ROM response length %u\n", *count);
		return -EPROTO;
	}
	memset(buf, 0, HI1102_ROM_MAX_REPLY + 1);
	return sdio_readsb(func, buf, HI1102_FIFO, *count);
}

static int hi1102_rom_send(struct sdio_func *func, u8 *buf,
			   const void *data, size_t len, const char *expect)
{
	size_t padded = len < 512 ? ALIGN(len, 4) : ALIGN(len, 512);
	u32 count;
	int ret;

	if (!len || padded > hi1102_transfer_limit(func))
		return -EINVAL;
	memset(buf, 0, padded);
	memcpy(buf, data, len);
	ret = sdio_writesb(func, HI1102_FIFO, buf, padded);
	if (ret || !expect)
		return ret;
	ret = hi1102_rom_read(func, buf, &count);
	if (ret)
		return ret;
	if (count < strlen(expect) || memcmp(buf, expect, strlen(expect))) {
		dev_err(&func->dev, "expected %s, reply (%u bytes): %*ph\n",
			expect, count, min_t(u32, count, 32), buf);
		return -EPROTO;
	}
	return 0;
}

static int hi1102_upload_file(struct sdio_func *func, u8 *buf,
			      const struct firmware *fw, u32 base,
			      unsigned long deadline)
{
	size_t offset, len;
	unsigned int chunk = hi1102_transfer_limit(func);
	char command[48];
	int ret, n;

	if (chunk < 512)
		return -EINVAL;
	dev_info(&func->dev, "upload %zu bytes at %#x, chunks <= %u\n",
		 fw->size, base, chunk);
	for (offset = 0; offset < fw->size; offset += len) {
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		len = min_t(size_t, chunk, fw->size - offset);
		n = scnprintf(command, sizeof(command), "FILES 1 0x%x ",
			      base + (u32)offset);
		ret = hi1102_rom_send(func, buf, command, n, "READY");
		if (ret)
			goto fail;
		usleep_range(5000, 6000);
		ret = hi1102_rom_send(func, buf, fw->data + offset, len, "FILES OK");
		if (ret)
			goto fail;
	}
	dev_info(&func->dev, "firmware upload acknowledged: %zu bytes at %#x\n",
		 fw->size, base);
	return 0;
fail:
	dev_err(&func->dev, "firmware chunk at offset %zu failed: %d\n", offset, ret);
	return ret;
}

static int hi1102_receive_boot_data(struct sdio_func *func, u8 *buf)
{
	u8 lengths[64];
	u32 count, int_stat, msg_stat, credit, offset = 0;
	unsigned int i, valid = 0;
	int ret;

	/* Caller supplies the current 80-byte vendor extended snapshot. */
	int_stat = get_unaligned_le32(buf);
	msg_stat = get_unaligned_le32(buf + 4);
	count = get_unaligned_le32(buf + 8);
	credit = get_unaligned_le32(buf + 12);
	memcpy(lengths, buf + 16, sizeof(lengths));
	dev_info(&func->dev, "HCC snapshot int=%#x msg=%#x bytes=%u credit=%#x\n",
		 int_stat, msg_stat, count, credit);
	if (!(int_stat & HI1102_INT_DREADY) || !count ||
	    count > hi1102_transfer_limit(func)) {
		dev_warn(&func->dev, "HCC RX metadata unqualified; FIFO not read\n");
		return 0;
	}
	ret = sdio_readsb(func, buf, HI1102_FIFO, count);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(lengths) && lengths[i]; i++) {
		u32 packet = (u32)lengths[i] << 5;
		u16 flags;
		u32 payload, pad_payload;
		u8 main_type, sub_type, pad_hdr;

		if (packet < 64 || offset > count || packet > count - offset) {
			dev_warn(&func->dev, "HCC descriptor %u out of bounds\n", i);
			break;
		}
		main_type = buf[offset] >> 4;
		sub_type = buf[offset] & 15;
		pad_hdr = buf[offset + 2];
		flags = get_unaligned_le16(buf + offset + 3);
		payload = flags >> 4;
		pad_payload = (flags >> 2) & 3;
		if (main_type >= 4 || 5 + pad_hdr + pad_payload > 64 ||
		    64 + pad_payload + payload > packet) {
			dev_warn(&func->dev, "HCC header unqualified: %8ph\n", buf + offset);
		} else {
			dev_info(&func->dev, "HCC RX frame%u main=%u sub=%u seq=%u payload=%u packet=%u header=%8ph\n",
				 i, main_type, sub_type, buf[offset + 1], payload, packet, buf + offset);
			valid++;
		}
		offset += packet;
	}
	dev_info(&func->dev, "HCC RX aggregate read: %u bytes, %u qualified headers\n",
		 count, valid);
	return 0;
}

static int hi1102_wait_ready(struct sdio_func *func, u8 *buf)
{
	/* Same ready wait as vendor HOST_WAIT_BOTTOM_INIT_TIMEOUT. */
	unsigned long deadline = jiffies + msecs_to_jiffies(20000);
	u32 messages;
	u8 status;
	bool data_seen = false;
	bool ready_received = false;
	int ret;

	dev_info(&func->dev, "QUIT sent; awaiting firmware WLAN_READY (20s)\n");
	for (;;) {
		/* Runtime protocol uses the extended snapshot, which acknowledges
		 * status on read. Do not clear the legacy interrupt register here.
		 */
		ret = sdio_memcpy_fromio(func, buf, 0x30, 80);
		if (ret)
			return ret;
		status = get_unaligned_le32(buf) & HI1102_INT_MASK;
		if (status & HI1102_INT_MESSAGE) {
			messages = get_unaligned_le32(buf + 4);
			sdio_readb(func, HI1102_MSG_HIGH, &ret);
			if (ret)
				return ret;
			dev_info(&func->dev, "post-QUIT extended firmware message=%#x\n", messages);
			if (messages & BIT(0)) {
				dev_info(&func->dev, "WLAN_READY message received with fresh MSG interrupt\n");
				ready_received = true;
			}
		}
		if ((status & HI1102_INT_DREADY) && !data_seen) {
			ret = hi1102_receive_boot_data(func, buf);
			if (ret)
				return ret;
			data_seen = true;
		}
		if (ready_received)
			return 0;
		if (time_after_eq(jiffies, deadline))
			break;
		usleep_range(1000, 2000);
	}
	dev_info(&func->dev, "no WLAN_READY yet, status=%#x; runtime unqualified\n",
		 status);
	return 0;
}

static int hi1102_coldboot(struct sdio_func *func, u8 *buf,
			   const struct firmware *const *fws)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(45000);
	/* Vendor cali_data_buf_malloc starts zeroed, target band_5g_enable=1.
	 * This is the initial BFGX payload, not learned Wi-Fi RF calibration.
	 */
	u8 initial_bfgx_cali[520] = {};
	char command[48];
	u32 count;
	unsigned int i, file = 0;
	u8 ec;
	int ret;

	ret = hi1102_rom_send(func, buf, "READM 0x50000770 4 ", 19, NULL);
	if (ret)
		return ret;
	ret = hi1102_rom_read(func, buf, &count);
	if (ret)
		return ret;
	if (count != 4) {
		dev_err(&func->dev, "EC revision read returned %u bytes\n", count);
		return -EPROTO;
	}
	ec = buf[1] & 3;
	dev_info(&func->dev, "EC revision=%s; cold BFGX+WiFi sequence\n",
		 ec == 0 ? "V100" : "V120");
	for (i = 0; i < ARRAY_SIZE(hi1102_boot_ops); i++) {
		const struct hi1102_boot_op *op = &hi1102_boot_ops[i];

		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		if (op->v100_only && ec != 0) {
			dev_info(&func->dev, "skip V100-only NFC retention write\n");
			continue;
		}
		switch (op->type) {
		case HI_CMD:
			ret = hi1102_rom_send(func, buf, op->command,
				strlen(op->command),
				!strncmp(op->command, "SETPM ", 6) ? "SET OK" : "WRITEM OK");
			break;
		case HI_FILE:
			if (file >= ARRAY_SIZE(hi1102_fw_names) ||
			    strcmp(op->command, hi1102_fw_names[file]))
				return -EINVAL;
			ret = hi1102_upload_file(func, buf, fws[file++],
						  op->value, deadline);
			break;
		case HI_CALI:
			scnprintf(command, sizeof(command), "FILES 1 0x%x ", op->value);
			ret = hi1102_rom_send(func, buf, command, strlen(command), "READY");
			if (ret)
				break;
			usleep_range(5000, 6000);
			ret = hi1102_rom_send(func, buf, initial_bfgx_cali,
					      sizeof(initial_bfgx_cali), "FILES OK");
			if (!ret)
				dev_info(&func->dev, "initial cold BFGX payload acknowledged\n");
			break;
		case HI_QUIT:
			ret = hi1102_rom_send(func, buf, "QUIT ", 5, NULL);
			if (ret)
				break;
			return hi1102_wait_ready(func, buf);
		default:
			return -EINVAL;
		}
		if (ret) {
			dev_err(&func->dev, "cold boot op %u type %u failed: %d\n",
				i, op->type, ret);
			return ret;
		}
	}
	return -EINVAL;
}

static int hi1102_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct device *dev = &func->dev;
	const struct firmware *fws[ARRAY_SIZE(hi1102_fw_names)] = {};
	const char *stage = "function enable";
	unsigned int i;
	unsigned int tx_limit;
	u8 *buf;
	u8 wake_before, wake_after;
	bool enabled = false;
	int ret, ignored;

	if (func->num != 1)
		return -ENODEV;
	/* Request all files before claiming MMC; never use userspace fallback. */
	for (i = 0; i < ARRAY_SIZE(fws); i++) {
		ret = request_firmware_direct(&fws[i], hi1102_fw_names[i], dev);
		if (ret) {
			dev_err_probe(dev, ret, "stock firmware %s missing\n", hi1102_fw_names[i]);
			goto release_fw;
		}
		if (!fws[i]->size || fws[i]->size > SZ_1M) {
			ret = -EINVAL;
			goto release_fw;
		}
		dev_info(dev, "stock firmware available: %s %zu bytes\n",
			 hi1102_fw_names[i], fws[i]->size);
	}
	sdio_claim_host(func);
	func->enable_timeout = 1000;
	ret = sdio_enable_func(func);
	if (ret)
		goto out;
	enabled = true;
	stage = "block size";
	ret = sdio_set_block_size(func, 512);
	if (ret)
		goto out;
	stage = "bulk transfer preconditions";
	tx_limit = hi1102_transfer_limit(func);
	if (tx_limit < 512) {
		ret = -EINVAL;
		goto out;
	}
	buf = devm_kzalloc(dev, max_t(size_t, tx_limit, HI1102_ROM_MAX_REPLY) + 1,
			 GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}
	dev_info(dev, "TX limit=%u, actual clock=%u, bus width=%u\n",
		 tx_limit, func->card->host->actual_clock,
		 1U << func->card->host->ios.bus_width);
	stage = "interrupt clear";
	sdio_writeb(func, HI1102_INT_MASK, HI1102_INT_STATUS, &ret);
	if (ret)
		goto out;
	stage = "interrupt enable";
	sdio_writeb(func, HI1102_INT_MASK, HI1102_INT_ENABLE, &ret);
	if (ret)
		goto out;
	dev_info(dev, "function 1 enabled, block size 512\n");
	dev_info(dev, "host limits req=%u seg=%u segs=%u blocks=%u\n",
		 func->card->host->max_req_size, func->card->host->max_seg_size,
		 func->card->host->max_segs, func->card->host->max_blk_count);
	stage = "host wake control read";
	wake_before = sdio_f0_readb(func, HI1102_WAKE_CONTROL, &ret);
	if (ret)
		goto out;
	stage = "host wake request";
	sdio_f0_writeb(func, 0, HI1102_WAKE_CONTROL, &ret);
	if (ret)
		goto out;
	wake_after = sdio_f0_readb(func, HI1102_WAKE_CONTROL, &ret);
	if (ret)
		goto out;
	dev_info(dev, "host wake control %#x -> %#x (DISALLOW_TO_SLEEP)\n",
		 wake_before, wake_after);
	if (wake_after != 0) {
		ret = -EIO;
		goto out;
	}
	stage = "ROM VERSION CMD53 exchange";
	ret = hi1102_rom_send(func, buf, "VERSION ", 8, "Hi1102V100");
	if (ret)
		goto out;
	dev_info(dev, "ROM VERSION Hi1102V100 confirmed (CMD53 write/read)\n");
	stage = "cold BFGX+WiFi bootstrap";
	ret = hi1102_coldboot(func, buf, fws);
out:
	/* Polling bootstrap only; no SDIO interrupt handler is installed yet. */
	if (enabled) {
		sdio_writeb(func, 0, HI1102_INT_ENABLE, &ignored);
		if (ret)
			sdio_disable_func(func);
	}
	sdio_release_host(func);
	if (ret)
		dev_err(dev, "%s failed: %d\n", stage, ret);
release_fw:
	for (i = 0; i < ARRAY_SIZE(fws); i++)
		if (fws[i])
			release_firmware(fws[i]);
	return ret;
}

static void hi1102_remove(struct sdio_func *func)
{
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
}

static const struct sdio_device_id hi1102_ids[] = {
	{ SDIO_DEVICE(0x0296, 0x5347) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, hi1102_ids);

static struct sdio_driver hi1102_driver = {
	.name = "hi1102_sdio",
	.id_table = hi1102_ids,
	.probe = hi1102_probe,
	.remove = hi1102_remove,
};
module_sdio_driver(hi1102_driver);
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("hi1102/cr4_asic.bin");
MODULE_FIRMWARE("hi1102/CPU_RAM_SCHED.bin");
MODULE_FIRMWARE("hi1102/CPU_RAM_WBS.bin");
MODULE_DESCRIPTION("Hi1102 SDIO boot ROM and firmware upload bring-up");
