#!/bin/bash
# Build the bach2 audio modules (mainline/drivers/audio-hi6402, docs/mainline-audio.md) as external
# modules against a configured Linux tree, objects outside the source (MO=), like build-hisi110x.sh.
#
#   build-audio-hi6402.sh e2|asp|slimbus|ver|clean
#
#   e2       hi6402_e2.ko: experiment E2 (ASP power request, HI6402 version over SSI)
#   asp      bach2_asp.ko: ASP power regulator + clk_pmuaudioclk/clk_asp_subsys (LPM3 mailbox)
#   slimbus  hi6402_slimbus.ko: vendor Cadence SLIMbus manager + HI6402 device (port in progress)
#   ver      hi6402_ver.ko: HI6402 version over SLIMbus (needs the slimbus build first: its Module.symvers)
#
# Environment: KDIR (default ../src/linux-hi6250), OUT (default mainline/out/audio-hi6402), JOBS, V=1.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DRV=$HERE/drivers/audio-hi6402
KDIR=${KDIR:-$HERE/../src/linux-hi6250}
KDIR=$(cd "$KDIR" && pwd)
OUT=${OUT:-$HERE/out/audio-hi6402}
JOBS=${JOBS:-$(nproc)}
WHAT=${1:-}
die() { echo "build-audio-hi6402: FAIL: $*" >&2; exit 1; }
case "$WHAT" in e2|asp|slimbus|ver|codec|pcm|clean) ;; *) die "usage: $0 e2|asp|slimbus|ver|codec|pcm|clean";; esac
[ "$WHAT" = clean ] && { rm -rf "$OUT"; exit 0; }
[ "$(uname -m)" = aarch64 ] || die "native aarch64 build only (uname -m = $(uname -m))"
[ -f "$KDIR/.config" ] || die "$KDIR is not configured (.config missing)"
[ -f "$KDIR/Module.symvers" ] || die "$KDIR/Module.symvers missing: finish 'make Image modules' first"
grep -q '^CONFIG_MODULES=y' "$KDIR/.config" || die "CONFIG_MODULES is not y in $KDIR/.config"
grep -q '^CONFIG_HI3660_MBOX=y' "$KDIR/.config" || die "CONFIG_HI3660_MBOX is not y (E2 stage 2 needs it)"
EXTRA=()
if [ "$WHAT" = ver ] || [ "$WHAT" = codec ] || [ "$WHAT" = pcm ]; then
	[ -s "$OUT/slimbus/Module.symvers" ] || die "$OUT/slimbus/Module.symvers missing: build slimbus first"
	EXTRA=(KBUILD_EXTRA_SYMBOLS="$OUT/slimbus/Module.symvers")
fi
mkdir -p "$OUT/$WHAT"
make -C "$KDIR" M="$DRV/$WHAT" MO="$OUT/$WHAT" -j"$JOBS" ${V:+V=$V} "${EXTRA[@]}" modules > "$OUT/$WHAT.log" 2>&1 || { tail -n 40 "$OUT/$WHAT.log"; die "$WHAT build failed"; }
cat "$OUT/$WHAT.log"
grep -nE 'modpost: .*undefined' "$OUT/$WHAT.log" && die "unresolved symbols in $WHAT"
ko=$(ls "$OUT/$WHAT"/*.ko 2>/dev/null) || die "no .ko produced in $OUT/$WHAT"
for k in $ko; do
	vm=$(readelf -p .modinfo "$k" | sed -n 's/^ *\[ *[0-9a-f]*\] *vermagic=//p'); kr=$(cat "$KDIR/include/config/kernel.release")
	case "$vm" in "$kr "*) ;; *) die "$k vermagic [$vm] does not match kernel.release [$kr]";; esac
	echo "== OK: $k ($vm)"
done
