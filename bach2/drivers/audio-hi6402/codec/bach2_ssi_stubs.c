// SPDX-License-Identifier: GPL-2.0
/*
 * bach2: hi_cdc_ctrl links the SSI accessors of hi_cdc_ssi.c, which is not ported: bach2 uses
 * hisilicon,bus-sel = "slimbus" (stock DT), and C2 showed the SSI path unreachable with the pads as
 * the bootloader leaves them. Calling these is a configuration error; it is logged loudly.
 */
#include <linux/printk.h>

unsigned int ssi_reg_read8(unsigned int reg);
unsigned int ssi_reg_read32(unsigned int reg);
void ssi_reg_write8(unsigned int reg, unsigned int val);
void ssi_reg_write32(unsigned int reg, unsigned int val);

unsigned int ssi_reg_read8(unsigned int reg)
{
	pr_err_ratelimited("bach2-audio: SSI register bus not ported (read8 0x%x)\n", reg);
	return 0;
}

unsigned int ssi_reg_read32(unsigned int reg)
{
	pr_err_ratelimited("bach2-audio: SSI register bus not ported (read32 0x%x)\n", reg);
	return 0;
}

void ssi_reg_write8(unsigned int reg, unsigned int val)
{
	pr_err_ratelimited("bach2-audio: SSI register bus not ported (write8 0x%x)\n", reg);
}

void ssi_reg_write32(unsigned int reg, unsigned int val)
{
	pr_err_ratelimited("bach2-audio: SSI register bus not ported (write32 0x%x)\n", reg);
}
