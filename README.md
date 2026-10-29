# ios-kl7 v0.10.0

Minimal ARM64 XNU launcher for the TECNO KL7 / MediaTek MT6789. This repository is only the shim: it reads the live DTB and initrd, prepares memory and `boot_args`, and hands control to XNU. It does not contain the XNU kernel, its patch series, the iOS rootfs, or the VM build.

## Repository split

The XNU side lives in the separate [KLiOS716](https://github.com/lib-fluff-so/KLiOS716) repository. That repository owns the XNU source, kernel patches, iOS rootfs, and the macOS VM/flash workflow, and it includes this shim as a git submodule.

This repository owns only the bootloader-facing shim:

- the ARM64 entry/handoff code and console;
- the FDT/initrd parser and payload placement checks;
- the test payload and host regression tests;
- packaging helpers for `SWEETXNU` and the Android boot image.

The split is intentional: the shim is the small component that launches XNU, while the companion repository builds and packages the kernel/rootfs around it.

## What v0.10.0 does

- Launches XNU from a `SWEETXNU` container: parses the payload, preflights Mach-O segment destinations, copies segments and BSS, copies the ADT, builds `boot_args` rev. 2 (736 bytes), and branches to the entry point with `x0 = physical boot_args`.
- Displays shim log lines directly on the existing 1080x2436 framebuffer.
- Sends the same lines to the KL7 ramoops console region.
- Parses RAM and reservations from the live FDT, including `reserved-memory`, `no-map`, and the FDT reservation map.
- Finds the initrd from `linux,initrd-start` / `linux,initrd-end` and looks for a payload in raw `newc` CPIO.
- Default test mode validates and launches `ios-kl7-test.bin`; the payload returns to the shim, which validates `HANDOFF PASS` and calls PSCI `SYSTEM_RESET` instead of halting. Any test-mode error also triggers an automatic reset, so a multi-try B-slot test does not require manual power-cycling.
- Keeps a compact exception report (`ESR`, `FAR`, `ELR`, `SPSR`) on the framebuffer and in ramoops.
- Starts a PSCI `CPU_ON` timer on cpu1; after 50 seconds it issues PSCI `SYSTEM_RESET`, so a test attempt returns through a warm reset and preserves ramoops.
- Reports WDT restart/arm state and disables the watchdog at handoff by default, leaving the PSCI timer as the reset source.
- Uses `VISIBLE_OFFSET 0` for the shim console and puts the XNU framebuffer base at the start of the region.

The shim does **not** initialize XNU's GIC, UFS, or other drivers, and it does not enable the MMU. It launches the XNU payload; it does not build XNU or provide the kernel/rootfs. The `XNU_PAYLOAD=1` loader path being compiled is not proof that a particular XNU build reaches userland on KL7.

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

The default build exercises the shim test path. XNU, the kernel patches, and the rootfs are built in the companion repository.

Artifacts:

- `out/shim_b.img`: Android boot image with the gzip-compressed shim `Image` and the test ramdisk.
- `out/shim_raw_b.img`: same test ramdisk, uncompressed `Image`.
- `out/Image.gz`: gzip-compressed shim image.

Artifact sizes depend on the selected build switches. Run `make sizes` after a build; its output is authoritative for the local toolchain and tree.

The test image is intended to verify the screen console and the test-payload handoff. It automatically requests PSCI SYSTEM_RESET after each result. The ramoops header is preserved across warm attempts when it has a valid signature and bounds, so logs from the seven B-slot retries can be read after the bootloader falls back to Android. Framebuffer behavior and PSCI reset availability still require confirmation on the physical device.

## Read ramoops logs

After the phone has returned to Android, before booting the shim again:

```sh
adb shell su -c 'ls -lh /sys/fs/pstore'
adb exec-out su -c 'cat /sys/fs/pstore/console-ramoops-0' > ~/kl7-shim.log
grep -nE 'ios-kl7|console:|HANDOFF|REBOOT:|ERROR:|PANIC:' ~/kl7-shim.log
```

The shim currently writes to the KL7 ramoops console zone at `0x480EF000` within the `0x48090000..0x4816ffff` reserved region. This is based on the supplied KL7 DTB/kernel layout and must be revalidated if that layout changes. If `console-ramoops-0` is absent, inspect `ls /sys/fs/pstore` instead of assuming a filename.

## Packaging an XNU payload

Packaging happens here; building XNU and the iOS rootfs happens in the companion repository. Supply a real ARM64 Mach-O kernelcache accepted by `tools/mkpayload_xnu.py`; ideally also supply the genuine D22AP Apple Device Tree extracted from an iPhone X IPSW:

```sh
make OUT=out-xnu XNU_PAYLOAD=1 \
  XNU_KERNEL=/path/to/kernelcache \
  D22AP_ADT=/path/to/d22ap.adt \
  all
make OUT=out-xnu sizes
```

If `D22AP_ADT` is missing, the generator warns and omits `/product`; suitable for an early bring-up experiment, not a complete iOS identity. Do **not** flash a Qualcomm-targeted `ios-sweet` kernel or a synthetic Mach-O image to KL7. The shim's loader/handoff side is implemented, but a successful build here does not prove that a given XNU kernel/rootfs combination boots; validate that on the hardware with the companion repository.

## Size and build switches

```sh
make sizes                         # report artifact sizes
make RAMOOPS_LOG=0 all              # disable the shim's ramoops writes
make OUT=out-xnu XNU_PAYLOAD=1 XNU_KERNEL=/path/to/kernelcache all
```

`XNU_PAYLOAD=0` is the safe default test-payload path. Build artifacts live under `OUT` (default `out`). `make clean` cleans that `OUT` directory only.

## Scope removed in v0.9.0

The firmware no longer includes the shim page-table/MMU builder, MMU-fault experiment, PSCI `CPU_ON` experiment, GIC/timer probes, emulator-only build path, progress strip, or verbose bootconfig/register dumps. The DTB memory/reservation parser stays because payload placement must avoid memory owned by the OS, reserved devices, ramoops, the DTB, the initrd, and the shim itself.

v0.10.0 keeps the reduced v0.9.0 loader scope and adds only the PSCI reset timer,
the watchdog backstop, and the framebuffer offset change described above. It
does not add CPU scaling.
