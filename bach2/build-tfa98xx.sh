#!/bin/bash
# TFA9894 amp module (mainline/drivers/tfa98xx, docs/mainline-amps.md section 10).
#
#   build-tfa98xx.sh stage-fw   (Pi) copy the stock containers from backup/odm.img
#                               (/etc/audio/nxp) into initramfs/firmware/tfa98xx/, hash-checked
#   build-tfa98xx.sh module     (tablet) build tfa98xx-bach2.ko against KDIR, objects in OUT (MO=)
#   build-tfa98xx.sh clean
#
# Environment: KDIR (default ../src/linux-hi6250), OUT (default mainline/out/tfa98xx), JOBS, V=1,
# ODM (default ../backup/odm.img).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DRV=$HERE/drivers/tfa98xx
OUT=${OUT:-$HERE/out/tfa98xx}
JOBS=${JOBS:-$(nproc)}
WHAT=${1:-}
die() { echo "build-tfa98xx: FAIL: $*" >&2; exit 1; }
case "$WHAT" in
stage-fw)
	ODM=${ODM:-$HERE/../backup/odm.img}
	[ -f "$ODM" ] || die "$ODM missing"
	command -v debugfs >/dev/null || die "debugfs (e2fsprogs) needed"
	mkdir -p "$HERE/initramfs/firmware/tfa98xx"
	for f in $(awk '{print $2}' "$HERE/tfa98xx-files.sha256"); do
		debugfs -R "dump /etc/audio/nxp/${f#tfa98xx/} $HERE/initramfs/firmware/$f" "$ODM" 2>/dev/null
		[ -s "$HERE/initramfs/firmware/$f" ] || die "debugfs did not produce $f"
	done
	(cd "$HERE/initramfs/firmware" && sha256sum -c "$HERE/tfa98xx-files.sha256") || die "container hash mismatch"
	exit 0;;
clean) rm -rf "$OUT"; exit 0;;
module) ;;
*) die "usage: $0 stage-fw|module|clean";;
esac
KDIR=${KDIR:-$HERE/../src/linux-hi6250}
KDIR=$(cd "$KDIR" && pwd)
[ "$(uname -m)" = aarch64 ] || die "native aarch64 build only (uname -m = $(uname -m))"
[ -f "$KDIR/.config" ] || die "$KDIR is not configured (.config missing)"
[ -f "$KDIR/Module.symvers" ] || die "$KDIR/Module.symvers missing: finish 'make Image modules' first"
for o in MODULES REGMAP_I2C CRC32 FW_LOADER; do
	grep -q "^CONFIG_$o=y" "$KDIR/.config" || die "CONFIG_$o is not y in $KDIR/.config"
done
mkdir -p "$OUT"
make -C "$KDIR" M="$DRV" MO="$OUT" -j"$JOBS" ${V:+V=$V} modules || die "build failed"
k=$OUT/tfa98xx-bach2.ko
[ -f "$k" ] || die "no $k"
vm=$(readelf -p .modinfo "$k" | sed -n 's/^ *\[ *[0-9a-f]*\] *vermagic=//p'); kr=$(cat "$KDIR/include/config/kernel.release")
case "$vm" in "$kr "*) ;; *) die "$k vermagic [$vm] does not match kernel.release [$kr]";; esac
echo "== OK: $k ($vm)"
