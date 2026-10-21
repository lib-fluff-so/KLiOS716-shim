# ios-kl7

Bare-metal ARM64 bring-up shim for the TECNO KL7 / MediaTek MT6789, exploring a path toward booting XNU.

## State of the port

- The **v0.7.1 test-payload handoff is confirmed on the physical phone**: the shim copies a payload above 4 GiB, transfers with MMU/caches off and `x0` pointing to the handoff structure, and observes the test payload return. This is separate from XNU boot.
- The default build (`XNU_PAYLOAD=0`) preserves that known-good test path.
- The opt-in v0.8.0 XNU path reads an unwrapped `SWEETXNU` container from CPIO, validates its segment table, preflights destinations against live DTB RAM/reservations, copies Mach-O segments into high DRAM, copies an Apple Device Tree (ADT), constructs `boot_args` rev 2 (736 bytes), and jumps to the kernel entry with MMU/caches disabled.
- Host tests cover the container parser, a synthetic arm64 Mach-O, CPIO packaging, and KL7 ADT generation. **The v0.8.0 XNU path has not yet been demonstrated booting XNU on-device.**
- The current ADT uses KL7-derived SoC values with an iPhone X / D22AP-style root identity. If the real D22AP `device-tree` from an iPhone X IPSW is unavailable, `/product` is omitted; that is only an early-kernel experiment and is not a complete iOS device identity.

The shim keeps secondary CPUs off for the XNU entry. SMP startup, GIC CPU-interface handling, timer setup, early panic/framebuffer console, storage, and SoC-specific XNU patches still need kernel-side work. The shim itself does not enable the XNU MMU or initialize interrupts before handoff.

## Build the known-good test mode

```sh
make clean
make -j2 all
make hosttest loadertest xnutest
```

Images are written to `out/`. `out/shim_b.img` is the compressed-image Android boot image; `out/shim_raw_b.img` uses an uncompressed ARM64 `Image`. The default CPIO contains `ios-kl7-test.bin`, wrapped with the `KL7PAY0` size/CRC header.

## Build the XNU loader path

You need an arm64 Mach-O kernelcache compatible with the port, and ideally the real D22AP Apple Device Tree extracted from an iPhone X IPSW. From the project root:

```sh
make OUT=out-xnu XNU_PAYLOAD=1 \
  XNU_KERNEL=/path/to/kernelcache \
  D22AP_ADT=/path/to/d22ap.adt \
  XNU_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1' all
```

Optional `XNU_RAMDISK=/path/to/root.img` appends a ramdisk segment and writes its physical range into the ADT as `RAMDisk`; the kernel command line should then include `rd=md0` if that is the intended root path. Omit `XNU_RAMDISK` for initial early-kernel tests.

The build writes `out-xnu/shim_b.img` and `out-xnu/shim_raw_b.img`. **Do not flash an image built with a synthetic test Mach-O**: the synthetic kernel in the regression tests only exists to validate packaging and parser logic.

If `D22AP_ADT` is missing, the build warns and emits an ADT without `/product`. That build can be useful for bring-up but is not expected to boot full iOS userland. The host-side ADT generator reads and validates the checked-in `fdt.dtb` values for KL7 RAM, GICv3, timer, UFS, framebuffer, and CPU topology before emitting the Apple ADT.

## What the host tests cover

`make xnutest` tests the exact freestanding SWEETXNU C parser against both adversarial fixtures and a generated Mach-O container. The Python test constructs a synthetic ARM64 Mach-O, emits the SWEETXNU v1 container, adds it unchanged to a raw newc CPIO, and checks the generated ADT fields against the KL7 DTB. These are format and build tests, not an emulation of XNU or proof of hardware boot.

`make hosttest loadertest` retain regression coverage for the live DTB/page-table parser and the original test-payload CPIO path. `make OUT=out-emu EMULATOR_COMPAT=1 all` plus the optional `make emutest` checks the framebuffer-only emulator path; it is not an EL1/MMU/PSCI/XNU hardware simulation.

## Persistent log

The shim writes diagnostics to the currently observed Linux 5.10 ramoops console zone at `0x480EF000..0x4812EFFF` within region `0x48090000..0x4816ffff`; `RAMOOPS_LOG=0` disables these writes. Revalidate these addresses if the phone's kernel or ramoops layout changes. The log is useful for failures before the first XNU console, but it is not a replacement for a real XNU panic/framebuffer console.

## Relevant switches

- `XNU_PAYLOAD=0` (default): original confirmed test-payload handoff.
- `XNU_PAYLOAD=1`: physical KL7 `SWEETXNU` loader path; requires `XNU_KERNEL`.
- `CPU_BRINGUP=1`: legacy PSCI secondary-core diagnostic; do not combine with `LOADER_HANDOFF=1`.
- `LOADER_HANDOFF=0`: stop after initial diagnostic/MMU stages.
- `MMU_STAGE=0`: skip the MMU stage; incompatible with payload handoff.
- `MMU_FAULT_TEST=1`: deliberately touches a no-map address; diagnostics only.
- `RAMOOPS_LOG=0`: disable persistent ramoops writes.

## One-shot boot of slot B

`tools/oneshot_b.py` provides `probe` and `arm` helpers for the A/B metadata flow. `arm` writes `boot_b`, reads it back, and only changes retry metadata if the AOSP `bootloader_control` block parses and its CRC matches. Review `pc/back_to_a.sh` before using any slot-switching helper; use the one-shot flow only when deliberately testing a boot image.
