// SPDX-License-Identifier: GPL-2.0
/*
 * HiSilicon Hi6250 (Kirin 659) IOMCU sensorhub: start the resident firmware and expose its
 * I2C0 relay as an I2C adapter.
 *
 * The bootloader (stock fastboot) loads the IOMCU firmware from the "sensorhub" partition into
 * DDR, powers and clocks the IOMCU, and leaves it waiting for the AP. This driver does what the
 * stock AP kernel's inputhub does up to MINI ready, and nothing after it:
 *  1. write the CONFIG_ON_DDR page (last 4 KiB of the sensorhub share memory): dump area and log
 *     level only; the charger, coulometer and USB short-circuit blocks stay zero, as on stock;
 *  2. send STARTUP_IOM3_CMD (one auto-ack word) on IPC mailbox 10 (AP -> IOMCU);
 *  3. wait for TAG_SYS STATUSCHANGE status MINSYSREADY on IPC mailbox 4 (IOMCU -> AP).
 * In that state the firmware services TAG_I2C transfer requests on its I2C0 bus (the sensor bus),
 * so the AP drives the sensors register by register (for example st_accel for the LIS3DH). The
 * dynamic module-load list is never sent: the firmware never loads its sensor or charger modules.
 *
 * Only IPC mailboxes 10 and 4 and the IPC lock register are touched. The IPC block itself
 * (0xe896b000) belongs to the hi3660-mbox driver (LPM3 channels), so its registers are mapped
 * here without claiming the region.
 *
 * Protocol (vendor drivers/huawei_platform/inputhub/hi6250 and drivers/hisi/mailbox, verified on
 * hardware by the userspace prototype shub.py): a packet is {tag, cmd, resp, partial_order,
 * tranid le16, length le16} + length bytes, at most 32 bytes per mailbox message; a response
 * has cmd + 1, the request's tranid and a le32 errno at the start of its payload.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/* HiIPCV230: mailbox n at n * 0x40 */
#define IPC_MBX_SIZE		0x40
#define MBX_SRC			0x00
#define MBX_DSET		0x04
#define MBX_DSTAT		0x0c
#define MBX_MODE		0x10
#define MBX_IMASK		0x14
#define MBX_ICLR		0x18
#define MBX_SEND		0x1c
#define MBX_DATA(i)		(0x20 + 4 * (i))
#define MBX_WORDS		8
#define IPC_LOCK		0xa00
#define IPC_UNLOCK_KEY		0x1acce551

#define MODE_AUTOACK		BIT(0)
#define ST_IDLE			BIT(4)
#define ST_DEST			BIT(6)
#define ST_ACK			BIT(7)

#define IPC_AP			0	/* A53, processor bit 0 */
#define IPC_IOM			2	/* IOM7, processor bit 2 */
#define MBX_TX			10	/* AP -> IOMCU (stock mailbox-10, tx-mailbox-fast) */
#define MBX_RX			4	/* IOMCU -> AP (stock mailbox-4, rx-mailbox-fast, SPI 201) */
#define ACK_TIMEOUT_US		300000	/* stock "timeout = <0x12c>" ms */

/* sensorhub share memory (vendor global_ddr_map.h / inputhub_bridge.h) */
#define SHMEM_SIZE		0x80000
#define SHMEM_CFG_OFF		(SHMEM_SIZE - 0x1000)		/* struct CONFIG_ON_DDR */
#define SHMEM_DUMP_SIZE		(SHMEM_SIZE - 0x1000)		/* 0x7f000 */
#define SHMEM_LOGBUF_SIZE	((SHMEM_SIZE - 0x3000) & ~0xfff)	/* 0x7d000 */
#define CFG_DUMP_ADDR		0x000	/* u64 */
#define CFG_DUMP_SIZE		0x008	/* u64 */
#define CFG_DUMP_FLAGS		0x01c	/* u8 enable, finish, reason */
#define CFG_LOGBUF		0x060	/* LogBuffCBBackup.mutex */
#define CFG_LOG_LEVEL		0x0e0
#define LOG_LEVEL_INFO		3

#define STARTUP_IOM3_CMD	0x00070001

#define TAG_I2C			0x82
#define TAG_SYS			0x88
#define TAG_END			0xff	/* continuation part of a long packet */
#define CMD_I2C_TRANS_REQ	0x1f
#define CMD_SYS_STATUSCHANGE	0x1f
#define ST_MINSYSREADY		2

#define PKT_HDR_LEN		8
#define PKT_MAX			(MBX_WORDS * 4)
#define I2C_REQ_LEN		12	/* busid u8, pad 3, addr u32, rx_len u16, tx_len u16 */
#define I2C_MAX_TX		(PKT_MAX - PKT_HDR_LEN - I2C_REQ_LEN)	/* 12 */
#define I2C_MAX_RX		(PKT_MAX - PKT_HDR_LEN - 4)		/* 20 */
#define REPLY_TIMEOUT_US	1000000
#define MINI_TIMEOUT_US		2000000

struct iomcu_shub {
	struct device *dev;
	void __iomem *tx;	/* mailbox 10 */
	void __iomem *rx;	/* mailbox 4 */
	void __iomem *lock;	/* IPC lock register */
	struct i2c_adapter adap;
	struct mutex xfer_lock;	/* one request in flight */
	spinlock_t rx_lock;	/* mailbox 4 drain + the fields below */

	/* request in flight (rx_lock) */
	bool want, done;
	u16 want_tid;
	u8 want_tag, want_cmd;
	u8 reply[PKT_MAX];
	unsigned int reply_len;

	bool skip_parts;	/* a long unsolicited packet: drop its continuation parts */
	int fw_status;		/* last STATUSCHANGE status, -1 none */
	u16 tranid;
	u8 porder;

	/* statistics (sysfs "stats") */
	unsigned int rx_irq, rx_poll, unsolicited, xfers, nak, errors, timeouts;
	unsigned int startup_ack_us, mini_us;
};

/* Started once per boot: the firmware is not restartable from here (stock uses an LPM3 reload). */
static bool iomcu_shub_started;

/* ------------------------------------------------------------------ IPC */

static int iomcu_ipc_unlock(struct iomcu_shub *s)
{
	int retry;

	for (retry = 0; retry < 3; retry++) {
		writel(IPC_UNLOCK_KEY, s->lock);
		if (!readl(s->lock))
			return 0;
		udelay(10);
	}
	return -EIO;
}

/* Called with rx_lock held. Returns true if a message was taken from mailbox 4. */
static bool iomcu_rx_one(struct iomcu_shub *s)
{
	u8 b[PKT_MAX];
	u32 mode, todo;
	unsigned int len;
	int i;

	mode = readl(s->rx + MBX_MODE);
	if (!(mode & ST_DEST) || !(readl(s->rx + MBX_DSTAT) & BIT(IPC_AP)))
		return false;

	for (i = 0; i < MBX_WORDS; i++)
		put_unaligned_le32(readl(s->rx + MBX_DATA(i)), b + 4 * i);
	for (i = 0; i < MBX_WORDS; i++)
		writel(0, s->rx + MBX_DATA(i));
	todo = 0x3 & ~readl(s->rx + MBX_IMASK);
	writel(todo, s->rx + MBX_ICLR);
	mode = readl(s->rx + MBX_MODE);
	if ((mode & ST_DEST) && !(mode & MODE_AUTOACK))
		writel(todo, s->rx + MBX_SEND);

	if (b[0] == TAG_END) {
		if (!s->skip_parts)
			s->unsolicited++;
		return true;
	}
	len = PKT_HDR_LEN + get_unaligned_le16(b + 6);
	s->skip_parts = len > PKT_MAX;

	if (b[0] == TAG_SYS && b[1] == CMD_SYS_STATUSCHANGE && len >= PKT_HDR_LEN + 2) {
		s->fw_status = get_unaligned_le16(b + PKT_HDR_LEN);
		return true;
	}
	if (s->want && !s->done && b[0] == s->want_tag && b[1] == s->want_cmd + 1 &&
	    get_unaligned_le16(b + 4) == s->want_tid && len <= PKT_MAX) {
		memcpy(s->reply, b, len);
		s->reply_len = len;
		s->done = true;
		return true;
	}
	s->unsolicited++;
	return true;
}

static void iomcu_rx_poll(struct iomcu_shub *s)
{
	unsigned long flags;

	spin_lock_irqsave(&s->rx_lock, flags);
	while (iomcu_rx_one(s))
		s->rx_poll++;
	spin_unlock_irqrestore(&s->rx_lock, flags);
}

static irqreturn_t iomcu_rx_irq(int irq, void *data)
{
	struct iomcu_shub *s = data;
	irqreturn_t ret = IRQ_NONE;

	spin_lock(&s->rx_lock);
	while (iomcu_rx_one(s)) {
		s->rx_irq++;
		ret = IRQ_HANDLED;
	}
	spin_unlock(&s->rx_lock);
	return ret;
}

/*
 * One message AP -> IOMCU on mailbox 10 (vendor hisi_mdev_send_msg: unlock, occupy, send, wait
 * for the ack state, clear, release). Mailbox 4 is drained while waiting: the IOMCU may talk
 * before it acks.
 */
static int iomcu_send(struct iomcu_shub *s, const u32 *words, unsigned int n, bool autoack,
		      unsigned int *ack_us)
{
	ktime_t t0;
	bool acked = false;
	u32 mode = 0;
	unsigned int i;
	int retry, ret;

	ret = iomcu_ipc_unlock(s);
	if (ret) {
		dev_err(s->dev, "IPC unlock failed\n");
		return ret;
	}
	for (retry = 0; retry < 10; retry++) {
		if (readl(s->tx + MBX_MODE) & ST_IDLE) {
			writel(BIT(IPC_AP), s->tx + MBX_SRC);
			if (readl(s->tx + MBX_SRC) & BIT(IPC_AP))
				break;
		}
		usleep_range(500, 600);
	}
	if (retry == 10) {
		dev_err(s->dev, "mailbox %d busy (mode %#x)\n", MBX_TX, readl(s->tx + MBX_MODE));
		return -EBUSY;
	}

	writel(~0U, s->tx + MBX_IMASK);
	writel(~(autoack ? BIT(IPC_IOM) : (BIT(IPC_AP) | BIT(IPC_IOM))), s->tx + MBX_IMASK);
	writel(BIT(IPC_IOM), s->tx + MBX_DSET);
	writel(autoack ? MODE_AUTOACK : 0, s->tx + MBX_MODE);
	for (i = 0; i < n && i < MBX_WORDS; i++)
		writel(words[i], s->tx + MBX_DATA(i));
	writel(BIT(IPC_AP), s->tx + MBX_SEND);

	t0 = ktime_get();
	do {
		mode = readl(s->tx + MBX_MODE);
		if (mode & ST_ACK) {
			acked = true;
			break;
		}
		iomcu_rx_poll(s);
		usleep_range(20, 50);
	} while (ktime_us_delta(ktime_get(), t0) < ACK_TIMEOUT_US);
	if (ack_us)
		*ack_us = ktime_us_delta(ktime_get(), t0);

	if (acked) {
		for (i = 0; i < MBX_WORDS; i++)
			writel(0, s->tx + MBX_DATA(i));
		writel(0x3 & ~readl(s->tx + MBX_IMASK), s->tx + MBX_ICLR);
	}
	/* release, also after a timeout */
	writel(~0U, s->tx + MBX_IMASK);
	writel(BIT(IPC_AP), s->tx + MBX_SRC);

	if (!acked) {
		dev_err(s->dev, "mailbox %d: no ack in %d ms (mode %#x)\n", MBX_TX,
			ACK_TIMEOUT_US / 1000, mode);
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * A request packet with RESP set (vendor write_customize_cmd with a read_info), sent with
 * manual ack; waits for the response (cmd + 1, same tranid). Caller holds xfer_lock.
 * Returns the response length (header included) or a negative errno.
 */
static int iomcu_request(struct iomcu_shub *s, u8 tag, u8 cmd, const u8 *payload,
			 unsigned int plen, u8 *reply)
{
	u8 pkt[PKT_MAX] = { 0 };
	u32 words[MBX_WORDS];
	unsigned long flags;
	unsigned int n, i;
	bool done = false;
	ktime_t t0;
	u16 tid;
	int ret;

	if (PKT_HDR_LEN + plen > PKT_MAX)
		return -EINVAL;

	tid = s->tranid++;
	pkt[0] = tag;
	pkt[1] = cmd;
	pkt[2] = 1;		/* RESP */
	pkt[3] = s->porder++;
	put_unaligned_le16(tid, pkt + 4);
	put_unaligned_le16(plen, pkt + 6);
	memcpy(pkt + PKT_HDR_LEN, payload, plen);
	n = DIV_ROUND_UP(PKT_HDR_LEN + plen, 4);
	for (i = 0; i < n; i++)
		words[i] = get_unaligned_le32(pkt + 4 * i);

	spin_lock_irqsave(&s->rx_lock, flags);
	s->want = true;
	s->done = false;
	s->want_tag = tag;
	s->want_cmd = cmd;
	s->want_tid = tid;
	spin_unlock_irqrestore(&s->rx_lock, flags);

	ret = iomcu_send(s, words, n, false, NULL);
	if (ret)
		goto out;

	t0 = ktime_get();
	do {
		iomcu_rx_poll(s);
		spin_lock_irqsave(&s->rx_lock, flags);
		done = s->done;
		spin_unlock_irqrestore(&s->rx_lock, flags);
		if (done)
			break;
		usleep_range(50, 100);
	} while (ktime_us_delta(ktime_get(), t0) < REPLY_TIMEOUT_US);

	if (!done) {
		s->timeouts++;
		dev_err_ratelimited(s->dev, "no response to tag %#x cmd %#x tranid %#x in %d ms\n",
				    tag, cmd, tid, REPLY_TIMEOUT_US / 1000);
		ret = -ETIMEDOUT;
		goto out;
	}
	memcpy(reply, s->reply, s->reply_len);
	ret = s->reply_len;
out:
	spin_lock_irqsave(&s->rx_lock, flags);
	s->want = false;
	spin_unlock_irqrestore(&s->rx_lock, flags);
	return ret;
}

/* ------------------------------------------------------------------ I2C relay */

/* {write}, {read} or {write, read} to one address (adapter quirks enforce the shape) */
static int iomcu_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct iomcu_shub *s = i2c_get_adapdata(adap);
	struct i2c_msg *wr = NULL, *rd = NULL;
	u8 req[I2C_REQ_LEN + I2C_MAX_TX] = { 0 };
	u8 reply[PKT_MAX];
	unsigned int tx_len, rx_len, plen;
	u32 fw_errno;
	int ret;

	if (num == 2) {
		wr = &msgs[0];
		rd = &msgs[1];
	} else if (msgs[0].flags & I2C_M_RD) {
		rd = &msgs[0];
	} else {
		wr = &msgs[0];
	}
	if ((wr && (wr->flags & (I2C_M_RD | I2C_M_TEN))) || (rd && (rd->flags & I2C_M_TEN)))
		return -EOPNOTSUPP;
	tx_len = wr ? wr->len : 0;
	rx_len = rd ? rd->len : 0;
	if (tx_len > I2C_MAX_TX || rx_len > I2C_MAX_RX)
		return -EOPNOTSUPP;

	req[0] = 0;				/* busid: I2C0 */
	put_unaligned_le32(msgs[0].addr, req + 4);
	put_unaligned_le16(rx_len, req + 8);
	put_unaligned_le16(tx_len, req + 10);
	if (wr)
		memcpy(req + I2C_REQ_LEN, wr->buf, tx_len);
	plen = I2C_REQ_LEN + tx_len;

	mutex_lock(&s->xfer_lock);
	s->xfers++;
	ret = iomcu_request(s, TAG_I2C, CMD_I2C_TRANS_REQ, req, plen, reply);
	if (ret < 0) {
		s->errors++;
		goto out;
	}
	if (ret < PKT_HDR_LEN + 4) {
		s->errors++;
		ret = -EPROTO;
		goto out;
	}
	fw_errno = get_unaligned_le32(reply + PKT_HDR_LEN);
	if (fw_errno) {
		/* the firmware's bus error: in practice an address NAK */
		s->nak++;
		dev_dbg(s->dev, "addr %#x: firmware errno %#x\n", msgs[0].addr, fw_errno);
		ret = -ENXIO;
		goto out;
	}
	if (rd) {
		if (ret < PKT_HDR_LEN + 4 + rx_len) {
			s->errors++;
			ret = -EPROTO;
			goto out;
		}
		memcpy(rd->buf, reply + PKT_HDR_LEN + 4, rx_len);
	}
	ret = num;
out:
	mutex_unlock(&s->xfer_lock);
	return ret;
}

static u32 iomcu_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm iomcu_i2c_algo = {
	.xfer = iomcu_i2c_xfer,
	.functionality = iomcu_i2c_func,
};

static const struct i2c_adapter_quirks iomcu_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN,
	.max_num_msgs = 2,
	.max_write_len = I2C_MAX_TX,
	.max_read_len = I2C_MAX_RX,
	.max_comb_1st_msg_len = I2C_MAX_TX,
	.max_comb_2nd_msg_len = I2C_MAX_RX,
};

/* ------------------------------------------------------------------ startup */

static int iomcu_write_config(struct iomcu_shub *s, const struct resource *res)
{
	void *mem;
	u8 *cfg;

	if (resource_size(res) != SHMEM_SIZE) {
		dev_err(s->dev, "share memory %pR: expected size %#x\n", res, SHMEM_SIZE);
		return -EINVAL;
	}
	mem = memremap(res->start, SHMEM_SIZE, MEMREMAP_WC);
	if (!mem) {
		dev_err(s->dev, "cannot map share memory %pR\n", res);
		return -ENOMEM;
	}
	cfg = mem + SHMEM_CFG_OFF;
	dev_info(s->dev, "config page %pa: stock layout %s (+000=%#x +008=%#x)\n",
		 &res->start,
		 (get_unaligned_le64(cfg + CFG_DUMP_ADDR) == res->start &&
		  get_unaligned_le64(cfg + CFG_DUMP_SIZE) == SHMEM_DUMP_SIZE) ? "found" : "not found",
		 get_unaligned_le32(cfg + CFG_DUMP_ADDR), get_unaligned_le32(cfg + CFG_DUMP_SIZE));

	/* the firmware log of this session starts from an empty buffer */
	memset(mem, 0, SHMEM_LOGBUF_SIZE);
	/*
	 * write_defualt_config_info_to_sharemem() + write_ramdump_info_to_sharemem(): everything
	 * else zero (charger/coulometer/USCP blocks as on stock, timestamps unused in MINI state)
	 */
	memset(cfg, 0, 0x1000);
	put_unaligned_le64(res->start, cfg + CFG_DUMP_ADDR);
	put_unaligned_le64(SHMEM_DUMP_SIZE, cfg + CFG_DUMP_SIZE);
	put_unaligned_le32(1, cfg + CFG_DUMP_FLAGS);	/* enable 1, finish SH_DUMP_INIT, reason 0 */
	put_unaligned_le32(0, cfg + CFG_LOGBUF);
	put_unaligned_le32(LOG_LEVEL_INFO, cfg + CFG_LOG_LEVEL);
	wmb();
	memunmap(mem);
	return 0;
}

static int iomcu_startup(struct iomcu_shub *s, const struct resource *shmem)
{
	u32 cmd = STARTUP_IOM3_CMD;
	unsigned long flags;
	ktime_t t0;
	int status = -1, ret;

	if (iomcu_shub_started) {
		dev_err(s->dev, "firmware already started in this boot; not sending STARTUP again\n");
		return -EBUSY;
	}
	if (!(readl(s->tx + MBX_MODE) & ST_IDLE)) {
		dev_err(s->dev, "mailbox %d not idle (mode %#x): not starting\n", MBX_TX,
			readl(s->tx + MBX_MODE));
		return -EBUSY;
	}
	iomcu_rx_poll(s);	/* stale message from an earlier session */

	ret = iomcu_write_config(s, shmem);
	if (ret)
		return ret;

	spin_lock_irqsave(&s->rx_lock, flags);
	s->fw_status = -1;
	spin_unlock_irqrestore(&s->rx_lock, flags);

	iomcu_shub_started = true;
	ret = iomcu_send(s, &cmd, 1, true, &s->startup_ack_us);
	if (ret) {
		dev_err(s->dev, "STARTUP %#010x not acked: the IOMCU is not listening on mailbox %d\n",
			cmd, MBX_TX);
		return ret;
	}

	t0 = ktime_get();
	do {
		iomcu_rx_poll(s);
		spin_lock_irqsave(&s->rx_lock, flags);
		status = s->fw_status;
		spin_unlock_irqrestore(&s->rx_lock, flags);
		if (status == ST_MINSYSREADY)
			break;
		usleep_range(100, 200);
	} while (ktime_us_delta(ktime_get(), t0) < MINI_TIMEOUT_US);
	s->mini_us = ktime_us_delta(ktime_get(), t0);

	if (status != ST_MINSYSREADY) {
		dev_err(s->dev, "STARTUP acked in %u us, but no MINI ready in %d ms (last status %d)\n",
			s->startup_ack_us, MINI_TIMEOUT_US / 1000, status);
		return -ETIMEDOUT;
	}
	dev_info(s->dev, "STARTUP acked in %u us, MINI ready %u us later\n",
		 s->startup_ack_us, s->mini_us);
	return 0;
}

/* ------------------------------------------------------------------ driver */

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct iomcu_shub *s = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "status=%d startup_ack_us=%u mini_us=%u xfers=%u nak=%u errors=%u timeouts=%u rx_irq=%u rx_poll=%u unsolicited=%u\n",
			  s->fw_status, s->startup_ack_us, s->mini_us, s->xfers, s->nak, s->errors,
			  s->timeouts, s->rx_irq, s->rx_poll, s->unsolicited);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *iomcu_shub_attrs[] = {
	&dev_attr_stats.attr,
	NULL
};
ATTRIBUTE_GROUPS(iomcu_shub);

static void __iomem *iomcu_map(struct device *dev, const struct resource *ipc, u32 off, u32 len)
{
	/* not devm_ioremap_resource(): hi3660-mbox owns the IPC region */
	return devm_ioremap(dev, ipc->start + off, len);
}

static int iomcu_shub_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *ipc_np;
	struct resource ipc, shmem;
	struct iomcu_shub *s;
	int irq, ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	s->fw_status = -1;
	s->tranid = 0x100;
	mutex_init(&s->xfer_lock);
	spin_lock_init(&s->rx_lock);
	platform_set_drvdata(pdev, s);

	ipc_np = of_parse_phandle(dev->of_node, "hisilicon,ipc", 0);
	if (!ipc_np)
		return dev_err_probe(dev, -EINVAL, "no hisilicon,ipc phandle\n");
	ret = of_address_to_resource(ipc_np, 0, &ipc);
	of_node_put(ipc_np);
	if (ret)
		return dev_err_probe(dev, ret, "IPC registers\n");
	if (resource_size(&ipc) < IPC_LOCK + 4)
		return dev_err_probe(dev, -EINVAL, "IPC region %pR too small\n", &ipc);
	s->tx = iomcu_map(dev, &ipc, MBX_TX * IPC_MBX_SIZE, IPC_MBX_SIZE);
	s->rx = iomcu_map(dev, &ipc, MBX_RX * IPC_MBX_SIZE, IPC_MBX_SIZE);
	s->lock = iomcu_map(dev, &ipc, IPC_LOCK, 4);
	if (!s->tx || !s->rx || !s->lock)
		return dev_err_probe(dev, -ENOMEM, "cannot map IPC mailboxes\n");

	ret = of_reserved_mem_region_to_resource(dev->of_node, 0, &shmem);
	if (ret)
		return dev_err_probe(dev, ret, "no memory-region (sensorhub share memory)\n");

	/* optional: without it (or if it never fires) mailbox 4 is polled */
	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		ret = devm_request_irq(dev, irq, iomcu_rx_irq, 0, dev_name(dev), s);
		if (ret)
			return dev_err_probe(dev, ret, "mailbox %d irq\n", MBX_RX);
	} else if (irq != -ENXIO) {
		return irq;
	}

	ret = iomcu_startup(s, &shmem);
	if (ret)
		return ret;

	s->adap.owner = THIS_MODULE;
	s->adap.algo = &iomcu_i2c_algo;
	s->adap.quirks = &iomcu_i2c_quirks;
	s->adap.dev.parent = dev;
	s->adap.dev.of_node = dev->of_node;
	s->adap.timeout = HZ;
	strscpy(s->adap.name, "IOMCU I2C0 relay", sizeof(s->adap.name));
	i2c_set_adapdata(&s->adap, s);
	ret = devm_i2c_add_adapter(dev, &s->adap);
	if (ret)
		return ret;
	dev_info(dev, "i2c-%d: IOMCU I2C0 relay (mailbox %d irq %d)\n", s->adap.nr, MBX_RX, irq);
	return 0;
}

/*
 * s2idle: the clients (st_accel) suspend first; the relay then refuses transfers until resume.
 * The firmware is left running in its MINI state, as it is between transfers.
 */
static int iomcu_shub_suspend_noirq(struct device *dev)
{
	struct iomcu_shub *s = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&s->adap);
	return 0;
}

static int iomcu_shub_resume_noirq(struct device *dev)
{
	struct iomcu_shub *s = dev_get_drvdata(dev);

	i2c_mark_adapter_resumed(&s->adap);
	return 0;
}

static const struct dev_pm_ops iomcu_shub_pm = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(iomcu_shub_suspend_noirq, iomcu_shub_resume_noirq)
};

static const struct of_device_id iomcu_shub_of_match[] = {
	{ .compatible = "hisilicon,hi6250-iomcu-i2c" },
	{ }
};

static struct platform_driver iomcu_shub_driver = {
	.probe = iomcu_shub_probe,
	.driver = {
		.name = "hi6250-iomcu-i2c",
		.of_match_table = iomcu_shub_of_match,
		.dev_groups = iomcu_shub_groups,
		.pm = pm_sleep_ptr(&iomcu_shub_pm),
		/* the firmware is started once per boot: no unbind/rebind */
		.suppress_bind_attrs = true,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
builtin_platform_driver(iomcu_shub_driver);
