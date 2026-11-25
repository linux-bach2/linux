/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shims for the vendor 4.4 audio code built out of tree on Linux 7.0 (docs/mainline-audio.md).
 * Huawei DSM/RDR reporting becomes a log line; Android wakelocks become wakeup sources.
 * Force-included by Kbuild; behaviour changes go here, never into the vendor files' logic.
 */
#ifndef BACH2_AUDIO_COMPAT_H
#define BACH2_AUDIO_COMPAT_H
#include <linux/printk.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/pinctrl/consumer.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/timekeeping.h>

/* vendor clk_get_enable_count(): only used in log lines */
#define clk_get_enable_count(c) ((void)(c), -1)	/* not exported on 7.0 */

/* 4.4 do_gettimeofday/struct timeval */
#define timeval __kernel_old_timeval
static inline void do_gettimeofday(struct __kernel_old_timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / 1000;
}
#include <linux/slab.h>

/* dsm/dsm_pub.h, dsm_audio/dsm_audio.h */
#define AUDIO_CODEC			0
#define DSM_HI6402_SLIMBUS_READ_ERR	1
#define DSM_HI6402_SLIMBUS_LOST_MS	2
#define DSM_HI6402_PLL_CANNOT_LOCK	3
#define audio_dsm_report_info(m, e, fmt, ...) \
	({ pr_err("bach2-audio: dsm %d/%d: " fmt, (int)(m), (int)(e), ##__VA_ARGS__); 0; })

/* rdr_hisi_audio_adapter.h: the vendor escalates to a system reset; here it is only logged. */
#define RDR_AUDIO_RUNTIME_SYNC_FAIL_MODID	0x84000001u
#define RDR_AUDIO_SLIMBUS_LOSTSYNC_MODID	0x84000002u
#define rdr_system_error(id, a, b) \
	pr_err("bach2-audio: rdr_system_error 0x%x (%u, %u) ignored\n", (unsigned int)(id), \
	       (unsigned int)(a), (unsigned int)(b))

/* linux/wakelock.h (Android) */
struct wake_lock {
	struct wakeup_source *ws;
};
#define WAKE_LOCK_SUSPEND 0
static inline void wake_lock_init(struct wake_lock *l, int type, const char *name)
{
	l->ws = wakeup_source_register(NULL, name);
}
static inline void wake_lock_destroy(struct wake_lock *l)
{
	wakeup_source_unregister(l->ws);
	l->ws = NULL;
}
static inline void wake_lock(struct wake_lock *l) { if (l->ws) __pm_stay_awake(l->ws); }
static inline void wake_unlock(struct wake_lock *l) { if (l->ws) __pm_relax(l->ws); }
static inline void wake_lock_timeout(struct wake_lock *l, long jiffies)
{
	if (l->ws)
		__pm_wakeup_event(l->ws, jiffies_to_msecs(jiffies));
}

/* 4.4 helpers removed by 7.0 */
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#ifndef BACH2_NO_OF_GPIO_FLAGS
enum of_gpio_flags { OF_GPIO_ACTIVE_LOW = 0x1 };
static inline int of_get_gpio_flags(struct device_node *np, int index, enum of_gpio_flags *flags)
{
	if (flags)
		*flags = 0;
	return of_get_named_gpio(np, "gpios", index);
}
#endif
/* 4.4 struct timespec / get_monotonic_boottime (register-op timestamps) */
#define timespec timespec64
#define get_monotonic_boottime(ts) ktime_get_boottime_ts64(ts)
/* vendor printk.h helper: monotonic ns */
#define hisi_getcurtime() ktime_get_ns()
/* 7.0 platform_driver.remove returns void: wrap a vendor int remove(). */
#define BACH2_VOID_REMOVE(fn) \
	static void fn##_void(struct platform_device *pdev) { (void)fn(pdev); }
#endif
