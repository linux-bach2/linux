/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mainline has no HiSilicon NVE (linux/mtd/hisi_nve_interface.h). Keep the
 * vendor struct layout and make every access fail, so the callers take their
 * existing fallbacks: RF params from the ini nvram_params*, and a random MAC
 * with bytes [1..2] = 11:02 (hwifi_get_mac_addr).
 */
#ifndef __HISI_NVE_STUB_H__
#define __HISI_NVE_STUB_H__

#include <linux/errno.h>
#include <linux/types.h>

#define NV_NAME_LENGTH		8
#define NVE_NV_DATA_SIZE	104
#define NV_WRITE		0
#define NV_READ			1

struct hisi_nve_info_user {
	uint32_t nv_operation;
	uint32_t nv_number;
	char nv_name[NV_NAME_LENGTH];
	uint32_t valid_size;
	u_char nv_data[NVE_NV_DATA_SIZE];
};

static inline int hisi_nve_direct_access(struct hisi_nve_info_user *info)
{
	return -ENODEV;
}

#endif
