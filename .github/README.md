# Mainline Linux for the Huawei MediaPad M5 lite 10 (bach2)

Linux 7.0 for the **Huawei MediaPad M5 lite 10, Wi-Fi model BAH2-W19** (codename *bach2*): HiSilicon
Kirin 659 (`hi6250`), 8x Cortex-A53, 3 GB RAM, 1200x1920 panel. It is based on the hi6250 port at
[tildeknown/linux-hi6250](https://github.com/tildeknown/linux-hi6250); the bach2 work is on branch **`bach2`**.

## What is on top of the hi6250 base
- Device tree `arch/arm64/boot/dts/hisilicon/hi6250-huawei-bach2.dts` (+ `-emmc.dtsi`).
- Clocks: SDIO0, eMMC0, AO GPIO, I2C3/I2C4, touch GPIOs, and the IOMCU CRG (sensor I2C0).
- Storage: Hi6250 SDHCI eMMC host, **read-only by design** (the eMMC holds the only recovery path).
- Wi-Fi: Hi1102 power sequence and SDIO bootstrap. The full driver is out of tree for now.
- PMIC: power key, HI6555V100 LDOs (touch rails), and read-only Hi6523 charger / coulomb-counter battery reporting.
- Reboot reasons (`reboot bootloader|recovery|erecovery`).
- Sensors: LIS3DH accelerometer node on the IOMCU I2C0 bus. The bus is not bound at boot yet; rotation is in progress.
- `DEBUG:` commits on top paint boot checkpoints on the bootloader framebuffer. Drop them for production.

## Status
| Area | State |
|---|---|
| Boot, display (simplefb), USB gadget, eMMC (ro), Wi-Fi | working in the bring-up images |
| Touch (Elan ekth5512c, i2c-hid) | working; survives s2idle (touchscreen kept powered as a wakeup source) |
| Battery / charger (HI6555V100 coulomb counter, Hi6523) | battery reporting; managed charging (CV 4100 mV, 700 mA) with a hardware watchdog |
| Audio (HI6402 codec over SLIMbus, TFA9894 amps, out-of-tree in `bach2/`) | playback audible on two of four speakers (codec S4 word length fix); the other two and the mic ADC are open |
| Deep sleep | s2idle works (PL031 alarm wake) without the Hi1102 Wi-Fi driver loaded; with it, the SDIO function blocks suspend |
| Rotation (LIS3DH) | blocked: the sensor bus belongs to the IOMCU (sensorhub firmware needed) |

## Build
CI builds every push (`.github/workflows/build.yml`) in an `alpine:3.21` container, with cached
incremental builds (a one-file change takes about 1.5 min). Locally:
```sh
make ARCH=arm64 bach2_defconfig
make ARCH=arm64 -j$(nproc) Image dtbs modules
```
Artifacts per run:
- **`bach2-kernel-<sha>`**: `Image.gz`, `hi6250-huawei-bach2.dtb`, `Image.gz-dtb`, a modules tarball,
  `config`, `System.map`, `warnings.log`, `toolchain.txt`.
- **`hostloop-tree-<sha>`**: the built tree (objects only), used by the bring-up test loop. The tablet
  takes it as-is and only embeds its private initramfs and relinks.

Booting needs the bootloader unlocked, plus a small shim that replaces the bootloader's DTB (the
stock bootloader passes its own board DTB from the `dts` partition). The shim and the test loop live
in the bring-up repository, not here.
