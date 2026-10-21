# ios-kl7

Early bring-up shim for investigating the TECNO KL7 (MediaTek MT6789) boot environment, as a first step toward an experimental XNU/Darwin port.

## Current status

- Boots through the device's existing Android boot path as an ARM64 Image.
- Uses a known-working framebuffer region to show CPU, DTB, bootargs and bootconfig diagnostics.
- Queries PSCI version and `PSCI_CPU_ON_AARCH64` support without starting another CPU.
- **v0.5.1:** installs a 2 KiB-aligned EL1 exception vector table and shows `ESR_EL1`, `FAR_EL1`, `ELR_EL1`, `SPSR_EL1`, `SP` and `VBAR_EL1` on a fatal synchronous exception. The stack is selected with `SPSel=1` and set via `MOV SP` (an explicit `MSR SP_EL1` is undefined at EL1). The handler parks instead of immediately resetting the device.
- **v0.5.2:** writes each diagnostic page and the fatal exception report into the KL7 Linux 5.10 ramoops console buffer at `0x480EF000`. The buffer header matches `persistent_ram_buffer` (signature `0x43474244`, `start`, `size`, then 262132 bytes of data); logging uses `dsb sy` before publishing the header. This address is based on the observed KL7 bootargs and `/proc/iomem` and must be re-verified if the device kernel/ramoops layout changes.
- **v0.6.0:** a new page 5 / ramoops stage after the four diagnostic pages: parses `/memory`, `/reserved-memory` (30 `no-map` regions on this DTB) and `/cpus` from the live DTB (`src/mm.c`), builds a 16 KiB-granule identity map (`[0,1 GiB)` Device-nGnRnE, all DRAM Normal Non-cacheable, no-map regions unmapped, ramoops forced mapped, image executable), software-verifies that everything the shim touches is mapped, turns the MMU on (caches still off), reads the GICv3 ID registers through the Device mapping, then brings up every other core with `PSCI CPU_ON`; each core enables the MMU on the same tables and reports back. Details and failure reading guide: `FIX-NOTES-v0.6.0.md`.
- **Not implemented:** caches, interrupts, XNU payload loading, Apple Device Tree generation, `boot_args`, or XNU handoff.

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

The test image initializes the ramoops logger, then executes `UDF #0` near the start of `shim_main()`. On the phone, it should display the exception screen with `ESR_EL1`/`ELR_EL1` and remain there. On this MTK workflow, do not assume `fastboot boot` is supported. `pc/flash_shim.sh` **flashes** `boot_b`, sets slot B active, and reboots; use it only when that is intended. Do not use the deliberate-test image as the everyday shim.

`tools/emutest.py` uses Unicorn to exercise the ordinary image and capture framebuffer snapshots. Unicorn's lightweight CPU emulation is not a substitute for a system emulator when checking real exception-level delivery; validate the `UDF #0` exception test on hardware.

Host test of the DTB parser and page-table builder (needs only gcc): `make hosttest`.

Build switches: `MMU_STAGE=0` skips the whole v0.6 stage, `CPU_BRINGUP=0` skips only `CPU_ON`, `MMU_FAULT_TEST=1` reads a no-map address on purpose after the MMU is on (expect an exception screen with a translation fault), `RAMOOPS_LOG=0` disables physical ramoops writes.

Normal build products are written to `out/`; the deliberate exception test goes to `out-exception/`.

## Ramoops validation (v0.5.2)

The KL7 kernel reported `mem_address=0x48090000`, `mem_size=0xe0000`, `record_size=0x1000`, `console_size=0x40000`, `ftrace_size=0x1000`, `pmsg_size=0x40000`, and `ecc=0`. `/proc/iomem` showed 95 dmesg records through `0x480eefff`, console at `0x480ef000..0x4812efff`, ftrace at `0x4812f000..0x4812ffff`, and pmsg at `0x48130000..0x4816ffff`.

The shim writes only the console zone, not the dmesg/ftrace/pmsg zones. It creates a valid `persistent_ram_buffer` header and stores its own lines there. This is a hardware experiment: reset retention and delivery through `/sys/fs/pstore/console-ramoops-0` still need to be confirmed on the phone. To disable physical writes, build with `make RAMOOPS_LOG=0`.

## Safety notes

- This is hardware-reconnaissance code, not an XNU loader.
- The full RAM range in the DTB is not a safe free-memory map; reserve all firmware/platform regions and live boot buffers.
- The console-zone address/layout and Linux 5.10 `persistent_ram_buffer` format were verified against this KL7 kernel. The shim now writes the console zone; preservation across the particular reset path and retrieval from slot A remain unverified until the next device test.
- The exception screen is best-effort and is not a recovery mechanism for every possible memory/stack fault. The v0.5.1 fix corrects the early EL1 stack setup; an older v0.5 image can fault before `VBAR_EL1` is installed and show a black screen.
- The Unicorn compatibility build does not validate real EL1 exception delivery. Use the hardware exception-test image for that check.

## One-shot boot of slot B (flash from the phone, fall back to A by itself)

`tools/oneshot_b.py` flashes the image to `boot_b`, makes B active and sets B's retry count to 1, so the bootloader returns to slot A after the shim has run once (AOSP A/B rule: unsuccessful slot loses a try per boot, at zero it is marked unbootable). Root + `bootctl` on the phone; it runs through adb, or on the phone itself with `--local` (Termux + python, as root).

```sh
python3 tools/oneshot_b.py probe                 # once: shows what bootctl changes in misc, restores slot A
python3 tools/oneshot_b.py arm out/shim_b.img --reboot
```

`arm` backs up `misc`, verifies `boot_b` by read-back, and only patches the retry count when the metadata parses as the AOSP `bootloader_control` block (magic `BCAB`, CRC matches); otherwise it writes nothing extra and prints the byte diff. Simulated in `tests/test_oneshot.py`; not yet run on the real phone. If slot B ever loops, `pc/back_to_a.sh` still works.
