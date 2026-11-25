// SPDX-License-Identifier: GPL-2.0
/*
 * hi6402_ver: HI6402 identity check over SLIMbus (docs/mainline-audio-port.md, C4).
 * Needs hi6402_slimbus.ko probed (the vendor accessors return ENXIO otherwise). Reads the version
 * register (expected 0x11, HI6402_VERSION_VALUE) twice and the chip ID bytes through the stock
 * accessors, which write only the codec page pointer (0x1fd-0x1ff). The load always fails
 * (-EAGAIN on PASS) so nothing stays loaded and it can be rerun.
 */
#include <linux/module.h>

unsigned int slimbus_read_1byte(unsigned int reg);

#define TAG "BACH2ML: AUD: "

static int __init ver_init(void)
{
	unsigned int v0, v1, id[4], i;
	bool pass;

	v0 = slimbus_read_1byte(0x20007000);
	for (i = 0; i < 4; i++)
		id[i] = slimbus_read_1byte(0x20007092 + i);
	v1 = slimbus_read_1byte(0x20007000);
	pass = v0 == 0x11 && v1 == 0x11;
	pr_crit(TAG "HI6402_VER %s ver=0x%02x/0x%02x chipid=%02x%02x%02x%02x (expect 0x11/0x11)\n",
		pass ? "PASS" : "FAIL", v0, v1, id[0] & 0xff, id[1] & 0xff, id[2] & 0xff, id[3] & 0xff);
	return pass ? -EAGAIN : -EIO;
}
module_init(ver_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("bach2: HI6402 version read over SLIMbus");
