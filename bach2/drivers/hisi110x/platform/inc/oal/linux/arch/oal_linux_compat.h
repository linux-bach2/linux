/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux 4.4 -> 7.0 compatibility for the vendor Hi110x driver.
 *
 * Only APIs that were removed outright, with a drop-in replacement whose
 * semantics are the same for the way this driver calls them. Anything whose
 * semantics changed (timers, kernel_read's offset argument, wakeup sources)
 * is converted at the call site instead; see oal_schedule.h and
 * hi110x_kernel_read_at() below.
 *
 * Included from oal_types.h, so it is visible to plat.ko and wifi.ko.
 */
#ifndef __OAL_LINUX_COMPAT_H__
#define __OAL_LINUX_COMPAT_H__

#include <linux/version.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/ktime.h>
#include <linux/wait.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/stdarg.h>
#include <linux/sched/clock.h>
#include <linux/panic_notifier.h>
#include <linux/rtc.h>

/* struct timeval / do_gettimeofday() were removed in 5.6. */
#define timeval __kernel_old_timeval
static inline void do_gettimeofday(struct __kernel_old_timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}

/* struct timespec / getnstimeofday() were removed in 5.6 / 5.0. */
#define timespec timespec64
#define getnstimeofday(ts) ktime_get_real_ts64(ts)

/* wait_queue_t was renamed in 4.13. */
typedef struct wait_queue_entry wait_queue_t;

/* random_ether_addr() was removed in 5.15. */
#define random_ether_addr(addr) eth_random_addr(addr)

/* IRQF_DISABLED was a no-op since 2.6.35 and removed in 4.1. */
#ifndef IRQF_DISABLED
#define IRQF_DISABLED 0
#endif

/*
 * set_fs()/get_fs() were removed (arm64: 5.11). Every caller in this driver
 * wraps vfs_read/vfs_write on a kernel buffer, which kernel_read/kernel_write
 * do directly with the same (file, buf, count, loff_t *pos) contract.
 */
#ifndef KERNEL_DS
typedef int hi110x_mm_segment_t;
#define mm_segment_t hi110x_mm_segment_t
#define KERNEL_DS 0
#define get_fs() 0
#define set_fs(fs) ((void)(fs))
#endif
#define vfs_read(f, b, c, p) kernel_read((f), (b), (c), (p))
#define vfs_write(f, b, c, p) kernel_write((f), (b), (c), (p))

/*
 * 4.4 kernel_read(file, offset, buf, count) read at @offset and did NOT move
 * file->f_pos; the vendor callers advance f_pos themselves. Keep that.
 */
static inline ssize_t hi110x_kernel_read_at(struct file *file, loff_t offset,
					    void *buf, size_t count)
{
	loff_t pos = offset;

	return kernel_read(file, buf, count, &pos);
}

/*
 * sched_setscheduler() is no longer exported (5.9). Same effect through the
 * exported sched_setattr_nocheck(), keeping the vendor's policy/priority.
 */
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
static inline int hi110x_sched_setscheduler(struct task_struct *p, int policy,
					    const struct sched_param *param)
{
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = policy,
		.sched_priority = param->sched_priority,
	};

	return sched_setattr_nocheck(p, &attr);
}
#define sched_setscheduler(p, pol, prm) hi110x_sched_setscheduler((p), (pol), (prm))

/* del_timer*() were renamed in 6.2 and the old names removed in 6.15. */
#define del_timer(t) timer_delete(t)
#define del_timer_sync(t) timer_delete_sync(t)

/* rtc_time_to_tm() was removed in 5.6; vendor passes seconds as unsigned long. */
#define rtc_time_to_tm(secs, tm) rtc_time64_to_tm((time64_t)(secs), (tm))

#endif /* __OAL_LINUX_COMPAT_H__ */
