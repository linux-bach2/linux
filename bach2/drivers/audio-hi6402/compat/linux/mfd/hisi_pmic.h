/* bach2: only hisi_pmic_reg_read is used (hi_cdc_ctrl, optional pmu_clkcodec_addr, absent in our DT) */
#ifndef BACH2_HISI_PMIC_H
#define BACH2_HISI_PMIC_H
#include <linux/printk.h>
static inline unsigned int hisi_pmic_reg_read(int addr)
{
	pr_warn("bach2-audio: hisi_pmic_reg_read(0x%x) not ported, returning 0\n", addr);
	return 0;
}
#endif
