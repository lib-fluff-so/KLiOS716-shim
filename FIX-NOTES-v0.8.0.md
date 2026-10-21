# ios-kl7 v0.8.0 — optional XNU payload loader

## What changed

- Preserved the v0.7.1 test-payload path as the default (`XNU_PAYLOAD=0`). The physical test handoff has since been confirmed by the user.
- Added an opt-in `SWEETXNU` v1 container parser to the freestanding shim. It checks header/version, segment bounds, file and virtual ranges, ADT bounds, entry point, command-line termination and basic overlap constraints before any high-RAM writes.
- Added the upstream-style Mach-O host packer with bounds checks for ARM64 load commands and chained-fixup tables. It resolves supported chained rebases into flat segments and rejects unsupported binds/formats.
- Added raw CPIO mode for `ios-kl7-xnu.bin`; unlike the test payload, this file has no `KL7PAY0` CRC wrapper.
- Added a KL7-oriented Apple Device Tree generator sourced from the checked-in live Linux DTB. It validates the upper DRAM bank, GICv3, 13 MHz architected timer, UFS range, framebuffer and 8-core A55/A76 topology.
- Added XNU `boot_args` revision/version 2, with compile-time assertions for the 736-byte structure and `CommandLine` offset 108. The MMU-off transition invalidates the instruction cache before branching.
- Added host tests for malformed payloads, a multi-segment synthetic ARM64 Mach-O, the optional root-ramdisk ADT property, raw CPIO packaging and KL7 ADT values.

## Build

The known-good default mode remains:

```sh
make clean
make -j2 all
make hosttest loadertest xnutest
```

The opt-in XNU loader path is built separately:

```sh
make OUT=out-xnu XNU_PAYLOAD=1 \
  XNU_KERNEL=/path/to/kernelcache \
  D22AP_ADT=/path/to/d22ap.adt \
  XNU_CMDLINE='debug=0xc serial=3 cpus=1 keepsyms=1' all
```

Optional `XNU_RAMDISK=/path/to/root.img` appends a root image to the payload and adds `/chosen/memory-map/RAMDisk` to the ADT. Use `rd=md0` only when that root-image path is intended.

## Known limitations

- Host builds/tests pass, but the XNU payload path has **not** been confirmed booting an actual XNU kernelcache on the physical KL7.
- The generated ADT omits `/product` if a real D22AP ADT was not supplied. This is suitable only for early kernel bring-up experiments, not a complete iOS identity.
- The XNU patch series is not yet adapted to MT6789. GIC/timer, SMP/CPU_ON, early console, framebuffer, UFS and touch remain kernel-side integration work.
- The container packer inherits `ios-sweet`'s supported chained-pointer formats; unsupported formats or chained binds fail instead of being silently ignored.
- A synthetic Mach-O is used only by host tests. It is not a bootable XNU kernel and must never be flashed as one.
