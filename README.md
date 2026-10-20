# ios-kl7

Early bring-up shim for investigating the TECNO KL7 (MediaTek MT6789) boot environment, as a first step toward an experimental XNU/Darwin port.

## Current status

- Boots through the device's existing Android boot path as an ARM64 Image.
- Uses a known-working framebuffer region to show CPU, DTB, bootargs and bootconfig diagnostics.
- Queries PSCI version and `PSCI_CPU_ON_AARCH64` support without starting another CPU.
- **v0.5.1:** installs a 2 KiB-aligned EL1 exception vector table and shows `ESR_EL1`, `FAR_EL1`, `ELR_EL1`, `SPSR_EL1`, `SP` and `VBAR_EL1` on a fatal synchronous exception. The stack is selected with `SPSel=1` and set via `MOV SP` (an explicit `MSR SP_EL1` is undefined at EL1). The handler parks instead of immediately resetting the device.
- **Not implemented:** ramoops/pstore writes, MMU setup, secondary CPU startup, XNU payload loading, Apple Device Tree generation, `boot_args`, or XNU handoff.

## Build

```sh
make clean
make
```

The repository's Unicorn harness is a lightweight framebuffer test, not a complete privileged-system emulator. Build its compatibility image separately; this skips EL1-only `SPSel`/`VBAR_EL1` writes only for the emulated image, while the hardware image retains the real exception-vector setup:

```sh
make OUT=out-emu clean
make OUT=out-emu EMULATOR_COMPAT=1
python3 tools/emutest.py out-emu/Image fdt.dtb
```

Open `out/emu_page1.png` through `out/emu_page4.png` to inspect each captured page. Captures start at the device-confirmed visible framebuffer offset (`FB_BASE + 1230 * 4352`) and honor the physical 4352-byte stride. The emulator requires the Python packages `unicorn`, `python-fdt` (module `fdt`) and `Pillow`.

Build the deliberate exception-vector test in a separate output directory so it cannot silently replace the normal build:

```sh
make OUT=out-exception clean
make OUT=out-exception TEST_EXCEPTION_VECTORS=1
```

The test image executes `UDF #0` immediately after entering `shim_main()`. On the phone, it should display the exception screen with `ESR_EL1`/`ELR_EL1` and remain there. Prefer temporary boot (`fastboot boot out-exception/shim_b.img`) where supported; `pc/flash_shim.sh` **flashes** `boot_b` and is not a temporary-boot command. Do not use the deliberate-test image as the everyday shim.

`tools/emutest.py` uses Unicorn to exercise the ordinary image and capture framebuffer snapshots. Unicorn's lightweight CPU emulation is not a substitute for a system emulator when checking real exception-level delivery; validate the `UDF #0` exception test on hardware.

Normal build products are written to `out/`; the deliberate exception test goes to `out-exception/`.

## Safety notes

- This is hardware-reconnaissance code, not an XNU loader.
- The full RAM range in the DTB is not a safe free-memory map; reserve all firmware/platform regions and live boot buffers.
- `ramoops.mem_address` is identified in bootargs, but this project does not write to it yet. Before that change, verify the exact Android kernel's `persistent_ram` format, ramoops zone layout, and reset-retention behavior.
- The exception screen is best-effort and is not a recovery mechanism for every possible memory/stack fault. The v0.5.1 fix corrects the early EL1 stack setup; an older v0.5 image can fault before `VBAR_EL1` is installed and show a black screen.
- The Unicorn compatibility build does not validate real EL1 exception delivery. Use the hardware exception-test image for that check.
