// SPDX-License-Identifier: GPL-2.0
/*
 * Stand-ins for the BT/GNSS/FM/NFC/IR-over-UART half of the vendor driver
 * (bfgx_dev.c, bfgx_data_parse.c, bfgx_low_power.c, bfgx_user_ctrl.c,
 * platform/driver/plat_uart.c), which is not built for Bach2 Wi-Fi.
 *
 * Contract relied on by plat_pm.c / bfgx_exception_rst.c:
 *   - ps_get_core_reference() yields no core (-ENODEV, *core = NULL), so every
 *     caller that dereferences the core bails out on its existing NULL check;
 *   - bfgx_is_shutdown() is therefore always "shut down", which makes
 *     wlan_power_on() take the cold BFGX_AND_WIFI_CFG path (the hardware-
 *     qualified one) and never the UART-assisted WIFI_CFG path;
 *   - every UART command fails loudly instead of pretending to succeed.
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/tty.h>

#include "plat_type.h"
#include "plat_debug.h"
#include "hw_bfg_ps.h"
#include "plat_pm.h"

int32 g_plat_loglevel = PLAT_LOG_INFO;
module_param(g_plat_loglevel, int, 0644);

uint32 g_bfgx_rx_queue[BFGX_BUTT] = {
	RX_BT_QUEUE, RX_FM_QUEUE, RX_GNSS_QUEUE, RX_IR_QUEUE, RX_NFC_QUEUE,
};

const uint8 *g_bfgx_subsys_name[BFGX_BUTT] = {
	"BT", "FM", "GNSS", "IR", "NFC",
};

#define BFGX_ABSENT() PS_PRINT_ERR("%s: BFGX/UART not built on Bach2\n", __func__)

int32 hw_ps_init(void)
{
	PS_PRINT_INFO("BFGX (BT/GNSS/FM/NFC/IR over UART) not built\n");
	return 0;
}

void hw_ps_exit(void)
{
}

int32 ps_get_core_reference(struct ps_core_s **core_data)
{
	*core_data = NULL;
	return -ENODEV;
}

int32 ps_get_plat_reference(struct ps_plat_s **plat_data)
{
	*plat_data = NULL;
	return -ENODEV;
}

bool ps_chk_bfg_active(struct ps_core_s *ps_core_d)
{
	return false;
}

bool ps_chk_only_gnss_and_cldslp(struct ps_core_s *ps_core_d)
{
	return false;
}

void bfg_timer_expire(uint64 data)
{
}

void host_allow_bfg_sleep(struct ps_core_s *ps_core_d)
{
}

int32 bfgx_open_cmd_send(uint32 subsys)
{
	BFGX_ABSENT();
	return -ENODEV;
}

int32 prepare_to_visit_node(struct ps_core_s *ps_core_d)
{
	return -ENODEV;
}

int32 post_to_visit_node(struct ps_core_s *ps_core_d)
{
	return -ENODEV;
}

int32 open_tty_drv(void *pm_data)
{
	BFGX_ABSENT();
	return -ENODEV;
}

int32 release_tty_drv(void *pm_data)
{
	return 0;
}

int32 ps_change_uart_baud_rate(int64 baud_rate, uint8 enable_flowctl)
{
	BFGX_ABSENT();
	return -ENODEV;
}

void ps_kfree_skb(struct ps_core_s *ps_core_d, uint8 type)
{
}

int32 ps_push_skb_queue(struct ps_core_s *ps_core_d, uint8 *buf_ptr, uint16 pkt_len, uint8 type)
{
	return -ENODEV;
}

int32 ps_tx_sys_cmd(struct ps_core_s *ps_core_d, uint8 type, uint8 content)
{
	BFGX_ABSENT();
	return -ENODEV;
}

int32 ps_write_tty(struct ps_core_s *ps_core_d, const uint8 *data, int32 count)
{
	BFGX_ABSENT();
	return -ENODEV;
}

void ps_uart_state_pre(struct tty_struct *tty)
{
}

void ps_uart_state_dump(struct tty_struct *tty)
{
}

void reset_uart_rx_buf(void)
{
}

int32 uart_bfgx_close_cmd(void)
{
	BFGX_ABSENT();
	return -ENODEV;
}

/* Only reached when BCPU is running (bfgx_is_shutdown() false): never here. */
int32 uart_wifi_open(void)
{
	BFGX_ABSENT();
	return -ENODEV;
}

int32 uart_wifi_close(void)
{
	BFGX_ABSENT();
	return -ENODEV;
}

int32 wifi_choose_bfgn_channel_send_log2sdt(uint8 *data, uint16 len)
{
	return -ENODEV;
}

/*
 * Android memdump-rotation handshake (bfgx_user_ctrl.c, used when
 * HI110X_HAL_MEMDUMP_ENABLE is unset, i.e. HI110X_ANDROID_VERSION=N as on
 * stock). No userspace rotator here; dumps are written directly.
 */
int32 plat_send_rotate_cmd_2_app(uint32 which_dump)
{
	return 0;
}

void plat_wait_last_rotate_finish(void)
{
}

void plat_exception_dump_file_rotate_init(void)
{
}
