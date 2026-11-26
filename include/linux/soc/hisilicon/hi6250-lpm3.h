/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HiSilicon Hi6250 (Kirin 659) LPM3 IPC mailbox 14, shared through the LPM3 power-domain driver
 * (drivers/pmdomain/hisilicon/hi6250-lpm3-pd.c).
 *
 * The mailbox framework gives a channel to one client only. The power-domain driver holds mailbox
 * 14; other LPM3 users of that mailbox (the ASP audio power and clock votes) send their stock words
 * through it instead of requesting the channel themselves. A consumer node points at the provider
 * with "hisilicon,lpm3 = <&lpm3_pd>".
 */
#ifndef __SOC_HISILICON_HI6250_LPM3_H
#define __SOC_HISILICON_HI6250_LPM3_H

#include <linux/err.h>
#include <linux/types.h>

struct device;
struct hi6250_lpm3;

#if IS_ENABLED(CONFIG_HI6250_LPM3_PD)
/*
 * Look up the provider named by @dev's "hisilicon,lpm3" phandle.
 * -ENOENT: no such property; -ENODEV: the provider node is disabled (in both cases the caller may
 * use its own mailbox); -EPROBE_DEFER: the provider has not probed (yet).
 */
struct hi6250_lpm3 *hi6250_lpm3_get(struct device *dev);
/*
 * Send one 8-word message. @msg must stay valid after the call (static storage): the mailbox core
 * queues the pointer, and a send that failed is retried from it later. Returns 0 once queued.
 */
int hi6250_lpm3_send(struct hi6250_lpm3 *lpm3, u32 *msg);
#else
static inline struct hi6250_lpm3 *hi6250_lpm3_get(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}

static inline int hi6250_lpm3_send(struct hi6250_lpm3 *lpm3, u32 *msg)
{
	return -ENODEV;
}
#endif

#endif /* __SOC_HISILICON_HI6250_LPM3_H */
