# plat.ko for Hi1102 on Huawei Bach2 (hi6250), mainline Linux 7.0.
# Built out of tree only, by mainline/build-hisi110x.sh, which sets
# HI1102_DRIVER_SRC_PATH and passes the stock Kconfig symbols as -D.
#
# Dropped from the vendor object list (bach2 has no use for them):
#   oal_pci_if.o          PCIe (whole file is _PRE_PRODUCT_ID_HI1151 only)
#   board-hi1102.o, board-hi1103.o  vendor DT/GPIO board probing (board/board.c is ours)
#   plat_uart.o, bfgx/{bfgx_dev,bfgx_data_parse,bfgx_low_power,bfgx_user_ctrl}.o
#                         BT/GNSS/FM/NFC/IR over UART (replaced by bfgx/bfgx_stub.o)
#   gps_refclk_src_*.o    CONFIG_HI110X_GPS_REFCLK is not set on bach2

ifeq ($(HI1102_DRIVER_SRC_PATH),)
$(error HI1102_DRIVER_SRC_PATH unset: build with mainline/build-hisi110x.sh)
endif

_PRE_PRODUCT_VERSION ?= 1102
HI1102_BOARD_VERSION ?= v8r2
DRIVER_PATH := $(HI1102_DRIVER_SRC_PATH)/platform

# Vendor feature switches (COMM_COPTS, COPTS). Make syntax, kept verbatim.
include $(DRIVER_PATH)/hi$(_PRE_PRODUCT_VERSION)_comm_defconfig
include $(DRIVER_PATH)/plat_$(_PRE_PRODUCT_VERSION)_$(HI1102_BOARD_VERSION)_defconfig

oal-objs := oal_bus_if.o oal_main.o oal_mem.o oal_net.o oal_hardware.o oal_schedule.o \
	oal_aes.o oal_kernel_file.o oal_cfg80211.o oal_sdio_host.o oal_profiling.o \
	oal_hcc_host.o oal_hcc_test.o oal_softwdt.o oal_dft.o
frw-objs := frw_event_deploy.o frw_event_main.o frw_event_sched.o frw_ipc_msgqueue.o \
	frw_main.o frw_task.o frw_timer.o
oam-objs := oam_main.o oam_alarm.o oam_event.o oam_log.o oam_trace.o oam_statistics.o \
	oam_config.o oam_register.o oam_linux_netlink.o
pm-objs := plat_firmware.o plat_pm.o plat_pm_wlan.o plat_cali.o plat_efuse.o

plat-y := $(addprefix oal/,$(oal-objs)) \
	$(addprefix frw/,$(frw-objs)) \
	$(addprefix oam/,$(oam-objs)) \
	sdt/sdt_drv.o \
	$(addprefix pm/,$(pm-objs)) \
	main/plat_main.o \
	driver/plat_sdio.o \
	board/board.o \
	../bfgx/bfgx_exception_rst.o ../bfgx/bfgx_stub.o \
	../common/chr_log/chr_devs.o \
	../common/oneimage/oneimage.o \
	../common/customize/hisi_ini.o ../common/customize/hisi_customize_wifi.o

obj-m += plat.o

ccflags-y += -I$(DRIVER_PATH)/inc -I$(DRIVER_PATH)/inc/frw -I$(DRIVER_PATH)/inc/oal \
	-I$(DRIVER_PATH)/inc/oal/linux -I$(DRIVER_PATH)/inc/oam -I$(DRIVER_PATH)/inc/pm \
	-I$(DRIVER_PATH)/frw -I$(DRIVER_PATH)/oal -I$(DRIVER_PATH)/oam -I$(DRIVER_PATH)/pm \
	-I$(DRIVER_PATH)/sdt -I$(DRIVER_PATH)/board -I$(DRIVER_PATH)/driver \
	-I$(HI1102_DRIVER_SRC_PATH)/bfgx \
	-I$(HI1102_DRIVER_SRC_PATH)/common/oneimage \
	-I$(HI1102_DRIVER_SRC_PATH)/common/customize \
	-I$(HI1102_DRIVER_SRC_PATH)/common/inc \
	-I$(HI1102_DRIVER_SRC_PATH)/common/chr_log \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc/hal \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc/dmac \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc/wal \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/hmac/hmac_verify \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/dmac/dmac_verify

ccflags-y += -DPLATFORM_TYPE_FOR_HI110X=0 -DPLATFORM_HI110X_k3v2oem1=1 -DPLATFORM_HI110X_UEDGE=2
ccflags-y += -DPLATFORM_DEBUG_ENABLE
ccflags-y += $(COPTS) $(COMM_COPTS)
# Vendor warnings; -Werror is stripped by build-hisi110x.sh unless WERROR=1.
ccflags-y += -Werror -Wuninitialized -Wempty-body -Wclobbered -Wmissing-parameter-type \
	-Wold-style-declaration -Woverride-init -Wtype-limits
# Vendor style that gcc 14 rejects or floods on; not bugs we are fixing here.
ccflags-y += -Wno-missing-prototypes -Wno-missing-declarations -Wno-unused-function \
	-Wno-unused-variable -Wno-unused-but-set-variable -Wno-declaration-after-statement \
	-fno-strict-aliasing
