// SPDX-License-Identifier: GPL-2.0
/*
 * Huawei "oneimage" chip selection, reduced for mainline Bach2.
 *
 * The vendor version asks huawei,hw_connectivity (CONFIG_HWCONNECTIVITY)
 * whether the board carries a HiSilicon chip and exports DT strings to
 * /proc/connectivity for the Android HAL. Bach2 always carries a Hi1102, the
 * stock kernel has no CONFIG_HWCONNECTIVITY path through here either (the
 * #else branches return true), and nothing in our userspace reads the proc
 * files, so they are not created.
 */
#include <linux/module.h>
#include "oneimage.h"
#include "board.h"

bool is_my_chip(void)
{
	return true;
}

bool is_hisi_chiptype(int32 chip)
{
	return chip == BOARD_VERSION_HI1102;
}
EXPORT_SYMBOL(is_hisi_chiptype);

/* Bach2 (BAH2-W19) has no NFC. */
bool is_my_nfc_chip(void)
{
	return false;
}

int read_nfc_conf_name_from_dts(char *buf, int buf_len, char *node_name, char *property_name)
{
	return -ENODEV;
}

int hw_misc_connectivity_init(void)
{
	return 0;
}

void hw_misc_connectivity_exit(void)
{
}
