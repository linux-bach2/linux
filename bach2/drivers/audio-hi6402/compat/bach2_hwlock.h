/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bach2: the vendor ASP DMA driver serialises its DMAC register accesses against the HiFi DSP with
 * hardware spinlock 6. Mainline has no HiSilicon hwspinlock provider, and the HiFi DSP is never
 * started here (E1: ASP off, isolated and in reset at handoff; nothing loads DSP firmware), so the
 * lock degrades to local IRQ masking, which keeps the AP-side critical sections intact.
 */
#ifndef BACH2_HWLOCK_H
#define BACH2_HWLOCK_H
#include <linux/hwspinlock.h>
#include <linux/irqflags.h>

#define BACH2_HWLOCK_SENTINEL ((struct hwspinlock *)0x1)
#define hwspin_lock_request_specific(id)	({ (void)(id); BACH2_HWLOCK_SENTINEL; })
#define hwspin_lock_free(l)			({ (void)(l); 0; })
#define hwspin_lock_timeout_irqsave(l, to, f)	({ (void)(l); (void)(to); local_irq_save(*(f)); 0; })
#define hwspin_unlock_irqrestore(l, f)		do { (void)(l); local_irq_restore(*(f)); } while (0)
#endif
