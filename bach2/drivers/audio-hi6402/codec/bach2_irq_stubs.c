// SPDX-License-Identifier: GPL-2.0
/* bach2: hi64xx_irq calls the codec-DSP misc driver around suspend/resume; not ported (no codec DSP). */
int hi64xx_hifi_misc_suspend(void);
int hi64xx_hifi_misc_resume(void);
int hi64xx_hifi_misc_suspend(void) { return 0; }
int hi64xx_hifi_misc_resume(void) { return 0; }
