# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is
Mainline Linux 7.0 for the **Huawei MediaPad M5 lite 10, BAH2-W19** ("bach2"): HiSilicon Kirin 659 (`hi6250`),
8x Cortex-A53, 3 GB RAM. It is based on [tildeknown/linux-hi6250](https://github.com/tildeknown/linux-hi6250)
(`master` = untouched base `6359ae585`). All bach2 work is on branch **`bach2`** (remote `bach2` =
`git@github.com:linux-bach2/linux.git`, pushed with the deploy key set in `core.sshCommand`).
`.github/README.md` is the public README: device, status, artifacts.

The device runs a stock Huawei 4.4 kernel + Alpine userspace; mainline is booted only as a test image (see "Testing").

## Build
- **Always build in CI, not on the tablet** (it's cached and incremental; a one-file change takes ~1.5 min). Push
  `bach2` → `.github/workflows/build.yml` builds `Image dtbs modules` with `bach2_defconfig` **in an `alpine:3.21`
  container, in-tree, at `/root/linux-extra/build/linux-hi6250`**: the same path and toolchain (gcc 14.2.0-r4,
  binutils 2.43.1-r3) as the tablet. That parity is what lets the tablet reuse CI objects; don't change the path,
  image or package list casually (the tablet checks every `pkg=` line of `toolchain.txt`).
- Artifacts: `bach2-kernel-<sha>` (Image.gz, DTB, in-tree + bach2 modules, config) and `hostloop-tree-<sha>` (pax tar of the build
  products; nanosecond mtimes matter, since GNU tar's default format makes make rebuild generated files).
- Local checks only: `make ARCH=arm64 bach2_defconfig && make ARCH=arm64 drivers/foo/bar.o` (one object) or
  `make hisilicon/hi6250-huawei-bach2.dtb`. Never run a full local kernel build.
- CI status/logs: `/root/linux-extra/ci-wait.sh <sha>` (uses the read-only token `~/.gh-token`).

## The two-repo invariant (read before changing kernel code)
The bring-up ("host") repo `/root/pmos-tablet` holds the same kernel changes as **patch files**:
`mainline/patches/*.patch` (always), `patches/emmc/*`, `patches/proposed/*` (only if listed in `proposed/READY`),
DTS patches against `mainline/hi6250-huawei-bach2.dts`, plus `patch-reboot.py` / `patch-crumbs.py`.
`build-on-tablet.sh` (`ci_verify`) applies that set to `6359ae585` in a scratch worktree and **refuses the CI tree
unless this branch equals it**, apart from `.github/`, `arch/arm64/configs/bach2_defconfig`, `CLAUDE.md` and `bach2/`.
So every kernel change goes into **both** places:
1. commit it here (one commit per patch, message ends with `Origin: bach2 bring-up <patch>`);
2. add/update the patch in the host repo (new DTS/driver work → `proposed/NNNN-*.patch` + a line in `READY`);
3. check: extract `patch_tree`/`ci_verify` from `build-on-tablet.sh` and run `ci_verify $(git rev-parse HEAD)`
   from `/root/linux-extra/build/linux-hi6250` (that tree is a git worktree of this repo).
Out-of-tree drivers (hisi110x Wi-Fi, HI6402 audio, TFA98xx amps) live in `bach2/` here (sources + the host repo's
build scripts) **and** in the host repo's `mainline/drivers/`. CI builds them after the kernel (artifacts:
`bach2-modules-<release>.tar.gz`, plus `modules-ext.tar.zst` + `bach2-src.sha256` for the tablet). The tablet uses the
prebuilt `.ko` only if `bach2-src.sha256` equals its own copy, so change driver code in **both** places.

## Where things are
- Board DT: `arch/arm64/boot/dts/hisilicon/hi6250-huawei-bach2.dts` (+ `-emmc.dtsi`, always included); SoC: `hi6250.dtsi`.
  Clock IDs: `include/dt-bindings/clock/hi6250-clock.h` (`HI6250_CRGCTRL_NR_CLKS` = 96, reserved blocks are noted there).
- bach2 drivers: `drivers/mmc/host/sdhci-hi6250.c` (eMMC, read-only), `drivers/mmc/core/pwrseq_hi1102.c`,
  `drivers/net/wireless/hi1102/` (SDIO bootstrap), `drivers/input/misc/hi6250-pmic-powerkey.c`,
  `drivers/regulator/hi6555v100-ldo.c`, `drivers/power/supply/{hi6523_charger,hi6555v100_battery}.c` (read-only),
  `drivers/clk/hisilicon/clk-hi6250.c`, `drivers/power/reset/hi6250-reboot.c`.
- `DEBUG:` commits paint boot checkpoints ("crumbs") on the bootloader framebuffer (`arch/arm64/kernel/setup.c`,
  `head.S`, `mm/mmu.c`, `init/main.c`, `kernel/panic.c`); they are kept identical to `patch-crumbs.py` output.

## Hard rules
- **The eMMC holds the only recovery path: mainline stays read-only on it.** The request filter in `sdhci-hi6250.c`
  (allow-list of commands, no data writes, CMD6 only to volatile EXT_CSD bytes) must never be relaxed. CMD5
  (sleep) is completed as success *without being sent*, so s2idle works; the card stays awake (no vmmc-supply).
- Never exceed stock electrical settings (regulator voltages, backlight cap, amp gain/ladder in the host repo's
  `docs/mainline-amps.md`).
- Device facts are tagged [V]erified / [I]nferred / [G]uessed in the host docs; record results there
  (`docs/mainline-*.md`), with observables pre-registered before each test cycle.

## Testing on hardware (self-cycle, no host needed)
`/root/linux-extra/selfcycle/run-cycle.sh <ci-sha> <s2-run-script>` (optionally `BUILD_ENV=BACH2_NO_WIFI=1`) syncs the
host repo, polls for CI's `hostfast-<sha>` artifact, builds the test image (`build-on-tablet.sh w11 fast`: CI's Image
unchanged, the initramfs as an external initrd via `/chosen linux,initrd-*`; no compile or link on the tablet) with
`BACH2_SELFCYCLE=1`, writes it to `erecovery_kernel` (p27) and reboots with `erecovery`. Start to back-in-stock is
about 2 min. Changes only to the initramfs or s2-run scripts need no CI: reuse the last CI sha. Stage 2 ends 5 s after the
s2-run script exits (`# S2RUN-HOLD` in the script keeps it up until the dead-man). `FAST=0` = the old relink path.
CI's built-in initramfs is only `bach2/fallback-init` (no initrd: it logs `BACH2ML-FALLBACK-INIT` and restarts into stock). The image runs `mainline/initramfs/s2-run/<name>.sh` in its
stage-2 rootfs and ends with a kernel restart (or a watchdog reset) into the untouched stock kernel. The boot hook
`/etc/local.d/bach2-selfcycle.start` then saves pstore to `selfcycle/runs/<ID>/` and restores p27 from
`/root/linux-extra/backup/`. **The session dies during a cycle**: write state to memory first. Read results from the
pmsg copy (`pstore/pmsg-ramoops-0`); the console copy gets bit flips on the restart path.

## Known hardware behaviour that shaped the code
- The bootloader passes its own DTB; a shim in the boot image replaces it (`/root/pmos-tablet/shim/`).
- Audio: the SLIMbus FIFOs carry 24-bit samples left-justified in bits 31..8, so the PCM offers `S32_LE`
  (`S24_LE` lost 48 dB); HI6402 S4 `PORT_CFG_H` stays at stock `0x70` (951d1d724, A19 PASS). The ASP DMA can't
  read the `hifi_data` reserved window, so it uses coherent buffers.
- SP805 watchdog counts `clkin_ref` (32764 Hz, CRG 0x140 bit 17 = 0); it fires at ~T/2, and SIGTERM to busybox
  `watchdog` doesn't stop it.
- Firmware PSCI 1.0 offers no SYSTEM_SUSPEND: deep sleep = s2idle. Wake source: the PL031 at `0xfff04000`.
  The Hi1102 Wi-Fi (`mmc2:0001:1`, out-of-tree driver) blocks suspend.
