# SPDX-License-Identifier: GPL-2.0
# Imagination VXD decoder (from TI ti-linux-6.1.y drivers/media/platform/img/vxe-vxd).
# Feature choices of TI's Makefile, fixed here: H.264 only (the HEVC/MJPEG parsers are not
# built: the Hi6250 PVDEC has no HEVC pipe, and only H.264 is ported to its firmware ABI),
# error concealment on, capture buffers scatter-gather through the PVDEC MMU (CMA is small).

vxd-dec-y += common/img_mem_man.o \
	common/img_mem_unified.o \
	common/imgmmu.o \
	common/pool_api.o \
	common/idgen_api.o \
	common/talmmu_api.o \
	common/pool.o \
	common/hash.o \
	common/ra.o \
	common/addr_alloc.o \
	common/work_queue.o \
	common/lst.o \
	common/dq.o \
	common/resource.o \
	common/rman_api.o

vxd-dec-y += decoder/vxd_core.o \
	decoder/vxd_pvdec.o \
	decoder/vxd_hi6250.o \
	decoder/dec_resources.o \
	decoder/pixel_api.o \
	decoder/vdecdd_utils_buf.o \
	decoder/vdecdd_utils.o \
	decoder/vdec_mmu_wrapper.o \
	decoder/hw_control.o \
	decoder/vxd_int.o \
	decoder/translation_api.o \
	decoder/decoder.o \
	decoder/core.o \
	decoder/swsr.o \
	decoder/h264_secure_parser.o \
	decoder/bspp.o \
	decoder/vxd_dec.o \
	decoder/vxd_v4l2.o

ccflags-y += -DERROR_CONCEALMENT
ccflags-y += -I$(src)/common

obj-$(CONFIG_VIDEO_IMG_VXD_DEC) += vxd-dec.o
