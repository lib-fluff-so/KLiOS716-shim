# ios-kl7 v0.9.3

Minimal ARM64 shim for the TECNO KL7 / MediaTek MT6789. This release focuses on the loader contract: read the live DTB and initrd, validate memory destinations, prepare either the test payload or a `SWEETXNU` container, then hand off with MMU/caches disabled. The shim no longer builds page tables, probes GIC/timers, or starts secondary CPUs.

## What v0.9.1 does

- Displays shim log lines directly on the existing 1080x2436 framebuffer.
- Sends the same lines to the KL7 ramoops console region.
- Parses RAM and reservations from the live FDT, including `reserved-memory`, `no-map`, and the FDT reservation map.
- Finds the initrd from `linux,initrd-start` / `linux,initrd-end` and looks for a payload in raw `newc` CPIO.
- Default test mode validates and launches `ios-kl7-test.bin`; the payload returns to the shim, which validates `HANDOFF PASS` and calls PSCI `SYSTEM_RESET` instead of halting. Any test-mode error also triggers an automatic reset, so a multi-try B-slot test does not require manual power-cycling.
- Optional `XNU_PAYLOAD=1` mode parses `ios-kl7-xnu.bin`, preflights Mach-O segment destinations, copies segments and BSS, copies the ADT, constructs `boot_args` rev. 2 (736 bytes), and branches to the payload entry with `x0 = physical boot_args`.
- Keeps a compact exception report (`ESR`, `FAR`, `ELR`, `SPSR`) on the framebuffer and in ramoops.

The shim does **not** initialize XNU's GIC, timer, SMP, UFS, or drivers. It also does not enable the MMU. The `XNU_PAYLOAD=1` loader path being compiled is not proof that a real XNU kernel boots on KL7.

## Build and test

Dependencies on Arch Linux:

```sh
sudo pacman -S --needed base-devel clang lld llvm python
```

Build the default test image and run host regression tests:

```sh
make clean
make -j"$(nproc)" all
make hosttest loadertest xnutest
make sizes
```

Artifacts:

- `out/shim_b.img`: Android boot image with the gzip-compressed shim `Image` and the test ramdisk.
- `out/shim_raw_b.img`: same test ramdisk, uncompressed `Image`.
- `out/Image.gz`: gzip-compressed shim image.

Measured v0.9.1 test sizes: `out/shim_b.img` is **20,480 bytes (20 KiB)** and `out/shim_raw_b.img` is **98,304 bytes (96 KiB)**. The v0.8.0 build was 28,672 bytes (28 KiB) / 106,496 bytes (104 KiB), respectively. `make sizes` is authoritative for a local build. The actual output of `make sizes` is authoritative for a local build.

The test image is intended to verify the screen console and the test-payload handoff. It automatically requests PSCI SYSTEM_RESET after each result. The ramoops header is preserved across warm attempts when it has a valid signature and bounds, so logs from the seven B-slot retries can be read after the bootloader falls back to Android. Framebuffer behavior and PSCI reset availability still require confirmation on the physical device.

## Read ramoops logs

After the phone has returned to Android, before booting the shim again:

```sh
adb shell su -c 'ls -lh /sys/fs/pstore'
adb exec-out su -c 'cat /sys/fs/pstore/console-ramoops-0' > ~/kl7-shim.log
grep -nE 'ios-kl7|console:|HANDOFF|REBOOT:|ERROR:|PANIC:' ~/kl7-shim.log
```

The shim currently writes to the KL7 ramoops console zone at `0x480EF000` within the `0x48090000..0x4816ffff` reserved region. This is based on the supplied KL7 DTB/kernel layout and must be revalidated if that layout changes. If `console-ramoops-0` is absent, inspect `ls /sys/fs/pstore` instead of assuming a filename.

## Optional XNU-container build

This mode is for host-side packaging/loader experiments. Supply a real ARM64 Mach-O kernelcache accepted by `tools/mkpayload_xnu.py`; ideally also supply the genuine D22AP Apple Device Tree extracted from an iPhone X IPSW:

```sh
make OUT=out-xnu XNU_PAYLOAD=1 \
  XNU_KERNEL=/path/to/kernelcache \
  D22AP_ADT=/path/to/d22ap.adt \
  all
make OUT=out-xnu sizes
```

If `D22AP_ADT` is missing, the generator warns and omits `/product`; suitable for an early bring-up experiment, not a complete iOS identity. Do **not** flash a Qualcomm-targeted `ios-sweet` kernel or a synthetic Mach-O image to KL7. The XNU payload path has not been demonstrated on-device.

## Size and build switches

```sh
make sizes                         # report artifact sizes
make RAMOOPS_LOG=0 all              # disable the shim's ramoops writes
make OUT=out-xnu XNU_PAYLOAD=1 XNU_KERNEL=/path/to/kernelcache all
```

`XNU_PAYLOAD=0` is the safe default test-payload path. Build artifacts live under `OUT` (default `out`). `make clean` cleans that `OUT` directory only.

## Scope removed in v0.9.0 and kept in v0.9.1

The firmware no longer includes the shim page-table/MMU builder, MMU-fault experiment, PSCI `CPU_ON` experiment, GIC/timer probes, emulator-only build path, progress strip, or verbose bootconfig/register dumps. The DTB memory/reservation parser stays because payload placement must avoid memory owned by the OS, reserved devices, ramoops, the DTB, the initrd, and the shim itself.

## v0.9.3 and the XNU kernel
v0.9.3 fixes the framebuffer base handed to XNU (visible page 0xFE519E00, not the region start). The kernel side lives in
`xnu/patches-kl7/` on top of ios-sweet's series; see `XNU-PATCH-PORT-PLAN.md` and `FIX-NOTES-v0.9.3.md`.
The README text above about v0.9.1 (no MMU/SMP in the shim) still holds for v0.9.3.
