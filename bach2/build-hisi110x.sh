#!/bin/bash
# Build the vendor Hi110x connectivity driver (mainline/drivers/hisi110x) as
# external modules against a configured Linux tree.
#
#   build-hisi110x.sh plat|wifi|all|clean
#
# Halves are independent: `plat` never touches wifi/, and `wifi` builds even
# while plat.ko is broken (modpost errors become warnings when plat's
# Module.symvers is absent, and the script says so loudly).
#
# Environment:
#   KDIR=...      configured kernel tree   (default ../src/linux-hi6250)
#   OUT=...       object output root       (default mainline/out/hisi110x)
#   JOBS=$(nproc) make -j
#   WERROR=1      keep -Werror (default: strip it via ccflags-remove-y)
#   V=1           verbose kbuild
#
# Native aarch64 gcc only (the Pi is aarch64); refuses to run elsewhere.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DRV=$HERE/drivers/hisi110x
KDIR=${KDIR:-$HERE/../src/linux-hi6250}
KDIR=$(cd "$KDIR" && pwd)
OUT=${OUT:-$HERE/out/hisi110x}
JOBS=${JOBS:-$(nproc)}
WHAT=${1:-}

die() { echo "build-hisi110x: FAIL: $*" >&2; exit 1; }

case "$WHAT" in plat|wifi|all|clean) ;; *) die "usage: $0 plat|wifi|all|clean";; esac
[ "$(uname -m)" = aarch64 ] || die "native aarch64 build only (uname -m = $(uname -m))"
[ -f "$KDIR/.config" ] || die "$KDIR is not configured (.config missing)"
[ -f "$KDIR/Module.symvers" ] || die "$KDIR/Module.symvers missing: finish 'make Image modules' first"
grep -q '^CONFIG_MODULES=y' "$KDIR/.config" || die "CONFIG_MODULES is not y in $KDIR/.config"

# Kconfig symbols the stock 4.4 bach2 kernel had (HI1102_PLATFORM/HI1102_WIFI=m).
# Nothing in our 7.0 .config knows them, so they are passed as -D.
CONFIG_DEFS=(
	CONFIG_CONNECTIVITY_HI110X
	CONFIG_CONNECTIVITY_HI110X_HI6250
	CONFIG_HI110X_SDIO_STD_CARD_SUPPORT
	CONFIG_HI110X_SDIO_DETECT_FUNCTION
	CONFIG_HI1102_PLATFORM_MODULE
	CONFIG_HI1102_WIFI_MODULE
	CONFIG_HI1102_PLAT_HW_CHR
	CONFIG_HI1102_PLAT_OAL
	CONFIG_HI1102_PLAT_FRW
	CONFIG_HI1102_PLAT_OAM
	CONFIG_HI1102_PLAT_SDT
	CONFIG_HI1102_PLAT_PS
	CONFIG_HI1102_PLAT_PM
	CONFIG_HI1102_PLAT_MAIN
	CONFIG_HI110X_KERNEL_MODULES_BUILD_SUPPORT
)
KCF="${KCFLAGS:-}"
for d in "${CONFIG_DEFS[@]}"; do KCF+=" -D$d=1"; done

# Variables the vendor Makefiles expect (see platform/Makefile, wifi/Makefile:
# their KERNELRELEASE branch includes the *_defconfig files from these paths).
MAKEVARS=(
	HI1102_MAKE_FLAG=MODULES
	HI1102_DRIVER_SRC_PATH="$DRV"
	KERNEL_DIR="$KDIR"
	CONFIG_HI110X_KERNEL_MODULES_BUILD_SUPPORT=yes
	_PRE_PRODUCT_VERSION=1102
	# Stock: efuse cache /data/misc/wifi, dumps /data/log (docs/hi1102-stock-oracle.md)
	HI110X_ANDROID_VERSION=N
	HI1102_BOARD_VERSION=v8r2
	KCFLAGS="$KCF"
)
[ "${WERROR:-0}" = 1 ] || MAKEVARS+=(ccflags-remove-y=-Werror)
[ -n "${V:-}" ] && MAKEVARS+=(V="$V")

kmake() { # <subdir> <target> [extra vars...]
	local sub=$1 tgt=$2; shift 2
	local extra=()
	# Linux 7.0 kbuild ignores EXTRA_CFLAGS. Until a half's Makefile is
	# converted to ccflags-y, forward its EXTRA_CFLAGS through ccflags-y.
	grep -q 'ccflags-y' "$DRV/$sub/Makefile" || extra+=('ccflags-y=$(EXTRA_CFLAGS)')
	mkdir -p "$OUT/$sub"
	make -C "$KDIR" M="$DRV/$sub" MO="$OUT/$sub" -j"$JOBS" \
		"${MAKEVARS[@]}" "${extra[@]}" "$@" "$tgt"
}

build_plat() {
	echo "== plat.ko"
	kmake platform modules || die "plat.ko build failed"
	[ -f "$OUT/platform/plat.ko" ] || die "plat.ko not produced"
	echo "== plat.ko OK: $OUT/platform/plat.ko"
}

build_wifi() {
	echo "== wifi.ko"
	local sym=$OUT/platform/Module.symvers
	if [ -s "$sym" ]; then
		kmake wifi modules KBUILD_EXTRA_SYMBOLS="$sym" || die "wifi.ko build failed"
	else
		echo "build-hisi110x: WARNING: $sym missing; building wifi.ko with" \
		     "KBUILD_MODPOST_WARN=1 (unresolved plat symbols NOT checked)" >&2
		kmake wifi modules KBUILD_MODPOST_WARN=1 || die "wifi.ko build failed"
	fi
	[ -f "$OUT/wifi/wifi.ko" ] || die "wifi.ko not produced"
	echo "== wifi.ko OK: $OUT/wifi/wifi.ko"
}

case "$WHAT" in
	plat) build_plat ;;
	wifi) build_wifi ;;
	all) build_plat; build_wifi ;;
	clean) rm -rf "$OUT" ;;
esac
