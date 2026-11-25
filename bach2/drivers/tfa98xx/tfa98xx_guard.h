/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Write guards of tfa98xx-bach2 (pure C, shared with the host test mainline/test-tfa98xx-guard.sh).
 * At stage 0 nothing may be written. At stage >= 1:
 *   - never 0xa2-0xaf (MTP copy/read triggers incl. CIMTP at 0xa3) or 0xf0-0xff (MTP shadow);
 *   - SYS_CTRL0 (0x00) never with AMPE (bit 3) or SBSL (bit 5) set at stage 1 (A5a: the amplifier stays off);
 *     from stage 2 (A5: tfa_dev_start through the NXP DSP path) they are allowed;
 *   - raw multi-byte writes only into the CoolFlux window 0x90-0x9f, at least reg + one word.
 */
#ifndef TFA98XX_GUARD_H
#define TFA98XX_GUARD_H

#define TFA_GUARD_SYS_CTRL0	0x00
#define TFA_GUARD_AMPE		(1u << 3)
#define TFA_GUARD_SBSL		(1u << 5)

static inline int tfa_guard_reg(int stage, unsigned int reg, unsigned int val)
{
	if (stage < 1)
		return 0;
	if ((reg >= 0xa2 && reg <= 0xaf) || reg >= 0xf0)
		return 0;
	if (stage < 2 && reg == TFA_GUARD_SYS_CTRL0 && (val & (TFA_GUARD_AMPE | TFA_GUARD_SBSL)))
		return 0;
	return 1;
}

static inline int tfa_guard_raw(int stage, const unsigned char *data, int len)
{
	return stage >= 1 && len >= 3 && data[0] >= 0x90 && data[0] <= 0x9f;
}

#endif
