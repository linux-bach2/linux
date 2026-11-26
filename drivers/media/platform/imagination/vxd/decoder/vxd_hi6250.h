/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HiSilicon Hi6250 (Kirin 659) power/clock glue for the PVDEC (D5500) video decoder.
 */
#ifndef _VXD_HI6250_H
#define _VXD_HI6250_H

#include <linux/device.h>

struct vxd_hi6250;

struct vxd_hi6250 *vxd_hi6250_probe(struct device *dev);
int vxd_hi6250_power_on(struct vxd_hi6250 *hp);
int vxd_hi6250_power_off(struct vxd_hi6250 *hp);
/* true when the CRG/PMC status says the decoder domain is up and its NOC port is open */
bool vxd_hi6250_is_up(struct vxd_hi6250 *hp);

#endif /* _VXD_HI6250_H */
