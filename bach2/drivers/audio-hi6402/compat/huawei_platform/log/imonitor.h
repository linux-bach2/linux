/* bach2: Huawei imonitor event reporting, stubbed (events become a log line) */
#ifndef BACH2_IMONITOR_H
#define BACH2_IMONITOR_H
#include <linux/printk.h>
struct imonitor_eventobj { int id; };
static struct imonitor_eventobj bach2_imonitor_obj;
static inline struct imonitor_eventobj *imonitor_create_eventobj(unsigned int id)
{
	pr_err("bach2-audio: imonitor event %u (stubbed)\n", id);
	return &bach2_imonitor_obj;
}
static inline int imonitor_set_param(struct imonitor_eventobj *o, unsigned short k, long v) { return 0; }
static inline int imonitor_send_event(struct imonitor_eventobj *o) { return 0; }
static inline void imonitor_destroy_eventobj(struct imonitor_eventobj *o) { }
#endif
