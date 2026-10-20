# ios-kl7

Early bring-up shim for investigating the TECNO KL7 (MediaTek MT6789) boot environment, as a first step toward an experimental XNU/Darwin port.

## Current status

- Boots through the device's existing Android boot path as an ARM64 Image.
- Uses a known-working framebuffer region to show CPU, DTB, bootargs and bootconfig diagnostics.
- Queries PSCI version and `PSCI_CPU_ON_AARCH64` support without starting another CPU.
- **v0.5:** installs a 2 KiB-aligned EL1 exception vector table and shows `ESR_EL1`, `FAR_EL1`, `ELR_EL1`, `SPSR_EL1`, `SP` and `VBAR_EL1` on a fatal synchronous exception. The handler parks instead of immediately resetting the device.
- **Not implemented:** ramoops/pstore writes, MMU setup, secondary CPU startup, XNU payload loading, Apple Device Tree generation, `boot_args`, or XNU handoff.

## Build

```sh
make clean
make
```

To build the deliberate exception-vector test image:

```sh
make clean
make TEST_EXCEPTION_VECTORS=1
```

The test image executes `UDF #0` immediately after entering `shim_main()`. It should display the exception screen with `ESR_EL1`/`ELR_EL1` and remain there. Prefer temporary boot (`fastboot boot out/shim_b.img`) where supported; `pc/flash_shim.sh` **flashes** `boot_b` and is not a temporary-boot command. Do not use the deliberate-test image as the everyday shim. Rebuild normally before the regular diagnostic/reset test:

```sh
make clean
make
```

Build products are written to `out/`.

## Safety notes

- This is hardware-reconnaissance code, not an XNU loader.
- The full RAM range in the DTB is not a safe free-memory map; reserve all firmware/platform regions and live boot buffers.
- `ramoops.mem_address` is identified in bootargs, but this project does not write to it yet. Before that change, verify the exact Android kernel's `persistent_ram` format, ramoops zone layout, and reset-retention behavior.
- The exception screen is best-effort and is not a recovery mechanism for every possible memory/stack fault.
