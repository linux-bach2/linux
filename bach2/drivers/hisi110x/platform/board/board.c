// SPDX-License-Identifier: GPL-2.0
/*
 * Hi1102 board layer for mainline Linux on Huawei Bach2 (hi6250).
 *
 * The vendor board.c probed "hisilicon,hi1102" for the power GPIO, the 32 kHz
 * PMU clock, UART pinmux juggling and the two device->host wake GPIOs, and
 * then toggled GPIO35 itself. On mainline the chip power is owned by the MMC
 * core: mainline/drivers/pwrseq_hi1102.c runs the same sequence (32k on,
 * UART RX held high, reset 10 ms low / 30 ms high) whenever the host powers
 * the card up, including from mmc_hw_reset(). So here:
 *
 *   board_power_on()/board_power_off()  only record the requested state;
 *                                        the actual cold reset happens in
 *                                        sdio_reinit() -> mmc_hw_reset().
 *   wake GPIOs                           optional, from the SDIO function
 *                                        child node "hisilicon,hi1102-wifi":
 *                                          wlan-wakeup-host-gpios
 *                                          bfgn-wakeup-host-gpios (unused)
 *                                        Without wlan-wakeup-host the SDIO
 *                                        in-band interrupt is used instead
 *                                        (hisdio_intr_mode = 0).
 *
 * The vendor SSI-over-GPIO debug port and all Hi1103/IR/UART code are gone.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>

#include "board.h"
#include "plat_debug.h"
#include "board-hi1102.h"
#include "hw_bfg_ps.h"

#define HI1102_WIFI_COMPATIBLE "hisilicon,hi1102-wifi"

BOARD_INFO g_board_info = {0};
EXPORT_SYMBOL(g_board_info);

uint32 g_device_subchip_type = BOARD_VERSION_HI1102;

DOWNLOAD_MODE device_download_mode_list[MODE_DOWNLOAD_BUTT] = {
	{.index = MODE_SDIO, .name = DOWNlOAD_MODE_SDIO},
	{.index = MODE_PCIE, .name = DOWNlOAD_MODE_PCIE},
	{.index = MODE_UART, .name = DOWNlOAD_MODE_UART},
};

extern int hisdio_intr_mode;

static struct gpio_desc *wlan_wkup_gpio;
static struct gpio_desc *bfgn_wkup_gpio;
static bool board_powered;

BOARD_INFO *get_hi110x_board_info(void)
{
	return &g_board_info;
}

int32 get_uart_pclk_source(void)
{
	return g_board_info.uart_pclk;
}

int32 check_device_board_name(void)
{
	g_device_subchip_type = BOARD_VERSION_HI1102;
	return BOARD_SUCC;
}

int32 get_device_board_version(void)
{
	g_board_info.chip_type = BOARD_VERSION_NAME_HI1102;
	g_device_subchip_type = BOARD_VERSION_HI1102;
	return BOARD_SUCC;
}

int board_get_wlan_wkup_gpio_val(void)
{
	return wlan_wkup_gpio ? gpiod_get_raw_value(wlan_wkup_gpio) : 0;
}

int board_get_bwkup_gpio_val(void)
{
	return bfgn_wkup_gpio ? gpiod_get_raw_value(bfgn_wkup_gpio) : 0;
}

extern void hi1102_request_cold_reset(void);
extern void hi1102_power_off_chip(void);

/*
 * The chip is powered and reset by pwrseq_hi1102 when the MMC core powers the
 * card up (host probe, mmc_hw_reset). Vendor callers pair board_power_on()
 * with sdio_reinit(); mark the cold reset as pending so sdio_reinit() does
 * mmc_hw_reset() (the GPIO35 sequence) instead of a warm re-init.
 */
void board_power_on(uint32 subsystem)
{
	if (subsystem >= POWER_BUTT) {
		PS_PRINT_ERR("power input system:%d error\n", subsystem);
		return;
	}
	PS_PRINT_INFO("board_power_on(%u): deferred to mmc pwrseq (was %s)\n",
		      subsystem, board_powered ? "on" : "off");
	board_powered = true;
	hi1102_request_cold_reset();
}

/*
 * Hi1102 power cannot be held off without removing the card (the reset line
 * belongs to pwrseq_hi1102). Stop the firmware instead: cold-reset the chip
 * back into ROM through mmc_hw_reset() and drop the in-band IRQ
 * (hi1102_power_off_chip, plat_pm.c). The next power-on cold-resets again.
 */
void board_power_off(uint32 subsystem)
{
	if (subsystem >= POWER_BUTT) {
		PS_PRINT_ERR("power input system:%d error\n", subsystem);
		return;
	}
	PS_PRINT_INFO("board_power_off(%u): reset chip to ROM\n", subsystem);
	board_powered = false;
	hi1102_power_off_chip();
}

static struct gpio_desc *board_get_wake_gpio(struct device_node *np, const char *con,
					     const char *label)
{
	struct gpio_desc *desc;

	desc = fwnode_gpiod_get_index(of_fwnode_handle(np), con, 0, GPIOD_IN, label);
	if (IS_ERR(desc)) {
		if (PTR_ERR(desc) != -ENOENT)
			PS_PRINT_ERR("%s-gpios: %ld\n", con, PTR_ERR(desc));
		return NULL;
	}
	return desc;
}

int32 hi110x_board_init(void)
{
	struct device_node *np;
	int irq;

	g_board_info.chip_type = BOARD_VERSION_NAME_HI1102;
	g_device_subchip_type = BOARD_VERSION_HI1102;
	g_board_info.wlan_download_channel = MODE_SDIO;
	g_board_info.bfgn_download_channel = MODE_SDIO;
	/* Stock DT: hisilicon,hi1102 carries hi1102,asic_version. */
	g_board_info.is_asic = VERSION_ASIC;
	g_board_info.need_power_prepare = NO_NEED_POWER_PREPARE;
	g_board_info.power_on_enable = -1;
	g_board_info.wlan_wakeup_host = -1;
	g_board_info.bfgn_wakeup_host = -1;
	g_board_info.bfgn_ir_ctrl_gpio = -1;
	g_board_info.have_ir = false;

	np = of_find_compatible_node(NULL, NULL, HI1102_WIFI_COMPATIBLE);
	if (np) {
		wlan_wkup_gpio = board_get_wake_gpio(np, "wlan-wakeup-host",
						     PROC_NAME_GPIO_WLAN_WAKEUP_HOST);
		bfgn_wkup_gpio = board_get_wake_gpio(np, "bfgn-wakeup-host",
						     PROC_NAME_GPIO_BFGN_WAKEUP_HOST);
		of_node_put(np);
	}

	if (wlan_wkup_gpio) {
		irq = gpiod_to_irq(wlan_wkup_gpio);
		if (irq <= 0) {
			PS_PRINT_ERR("wlan wake GPIO has no IRQ (%d)\n", irq);
			gpiod_put(wlan_wkup_gpio);
			wlan_wkup_gpio = NULL;
		} else {
			g_board_info.wlan_irq = irq;
			g_board_info.wlan_wakeup_host = desc_to_gpio(wlan_wkup_gpio);
		}
	}

	if (wlan_wkup_gpio) {
		PS_PRINT_INFO("hi1102 board: wlan wake GPIO %d, irq %u, gpio interrupt mode\n",
			      g_board_info.wlan_wakeup_host, g_board_info.wlan_irq);
	} else {
		PS_PRINT_WARNING("hi1102 board: no %s wlan-wakeup-host-gpios; using SDIO in-band interrupt\n",
				 HI1102_WIFI_COMPATIBLE);
		hisdio_intr_mode = 0;
	}
	return BOARD_SUCC;
}

void hi110x_board_exit(void)
{
	if (wlan_wkup_gpio)
		gpiod_put(wlan_wkup_gpio);
	if (bfgn_wkup_gpio)
		gpiod_put(bfgn_wkup_gpio);
	wlan_wkup_gpio = NULL;
	bfgn_wkup_gpio = NULL;
}
