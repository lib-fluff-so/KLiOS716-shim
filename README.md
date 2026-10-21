# ios-kl7

Bare-metal ARM64 bring-up shim for the TECNO KL7 / MediaTek MT6789, aimed at eventually understanding the requirements for a Darwin/XNU experiment. **v0.7.0 is a loader-handoff test build, not an XNU boot.**

## Current state

- The device has booted the shim through `boot_b`; ramoops logging has been confirmed on-device.
- v0.6.3 was confirmed on the phone: MMU with a 16 KiB translation granule works; all seven secondary cores entered the test code and enabled the shared identity map; GICD PIDR2 reports GICv3.
- In v0.7.0, `CPU_BRINGUP=0` by default. Secondary CPUs are left OFF so the future XNU entry can start them through its own platform code.
- The shim builds an identity map with caches off, verifies the GICv3 distributor and RAM access, locates a small test payload in the supplied initrd's raw newc CPIO, checks its size and CRC32, checks a high-RAM target at `0x140000000` against DTB RAM ranges and known live/reserved regions, copies and rechecks it, then jumps to it with `SCTLR_EL1.M/C/I=0` and `x0=&test_boot_args`.
- The payload returns to a shim trampoline and reports what it observed. Only after validating its execution address, return ABI, `x0`, EL and MMU-off state does the progress bar reach 100%. It then requests a reset after three seconds.
- **The v0.7.0 target handoff has not yet been verified on the phone.** A successful compile and host tests do not prove that the device bootloader exposes the injected CPIO at the DTB's initrd range or that the high-RAM branch succeeds on hardware.

## Display and logging

The old multi-page diagnostic screen is gone. Diagnostic details—including entry registers, DTB properties, bootargs, bootconfig, memory map, MMU verification, CPIO checks and handoff result—are streamed directly into the KL7 ramoops console. They are not painted on-screen. The display is only touched in the final 88-pixel strip: a percentage and progress bar. The rest of the bootloader screen is left alone.

Percentages track stages of work, not time: environment/DTB checks (7–20%), memory-map validation and page tables (30–58%), MMU/GIC/RAM checks (66–78%), initrd/payload validation (80–91%), copy/read-back (94%), boot-argument preparation (97%), branch to test payload (99%), then verified return (100%). Failures leave a red bar and are written to ramoops; the shim attempts a controlled reset after three seconds.

## Build

```sh
make clean
make
make hosttest loadertest
```

Images are written to `out/`:

- `out/shim_b.img` — Android boot image with a gzip-compressed ARM64 `Image` and the generated raw CPIO test ramdisk.
- `out/shim_raw_b.img` — same boot header and ramdisk, but the ARM64 `Image` is not gzip-compressed.
- `out/payload.bin` and `out/ramdisk.cpio` — 64-byte test payload and its `ios-kl7-test.bin` CPIO entry plus `KL7PAY0` length/CRC wrapper.

The loader tests exercise CPIO file discovery, payload size/header/CRC validation, bad-payload rejection, read-back data and the page-table/DTB parser. The normal output directory should be kept separate from experimental builds when changing compile-time switches.

Optional framebuffer-only emulator test (requires Python modules `unicorn`, `python-fdt`/`fdt` and `Pillow`):

```sh
make emutest
```

This compatibility run stops after the initial environment inspection and captures one screenshot at `out-emu/emu_progress.png`. It is **not** an EL1/MMU/PSCI/loader hardware simulation; the phone is still the authority for those stages.

Build the deliberate EL1 exception test separately, never over the regular output:

```sh
make OUT=out-exception clean
make OUT=out-exception TEST_EXCEPTION_VECTORS=1
```

Do not leave `TEST_EXCEPTION_VECTORS=1` in the build used for normal boot testing.

## Ramoops log

The current image uses the observed Linux 5.10 layout: region `0x48090000..0x4816ffff`, console zone `0x480EF000..0x4812EFFF`. The shim writes only the console zone. Extraction after reset was confirmed for the earlier version on this device; v0.7.0's longer loader log and reset path still need a fresh on-device check. If the device kernel/ramoops layout changes, re-check the addresses before relying on the log. `RAMOOPS_LOG=0` disables physical ramoops writes.

## Build switches

- `CPU_BRINGUP=1` runs the older PSCI secondary-core test. **Do not combine it with `LOADER_HANDOFF=1`**; make rejects that combination, because the loader test leaves secondary CPUs off for the XNU-style handoff.
- `LOADER_HANDOFF=0` skips the test-payload branch and resets after the MMU checks.
- `MMU_STAGE=0` skips MMU bring-up; the payload handoff requires `MMU_STAGE=1`.
- `MMU_FAULT_TEST=1` deliberately touches a no-map address after enabling the MMU; use only for the known exception test.
- `RAMOOPS_LOG=0` disables ramoops writes.

## Safety notes

- This is still hardware-reconnaissance and a test-payload handoff, not an XNU loader. There is no Apple Device Tree generation, real Apple `boot_args`, XNU payload, cache enablement or interrupt setup yet.
- A `/memory` range is not a free-memory map. The high-RAM copy is allowed only after checking known DTB reservations, the image, page-table pool, secondary-core context memory, ramoops, DTB and initrd.
- The loader searches the live initrd for a raw newc CPIO entry named `ios-kl7-test.bin`. If the device passes an absent, compressed, or otherwise unsupported initrd layout, it should fail before copying into the high-RAM target. Confirm the actual ramoops failure log before interpreting a stalled percentage.
- `pc/flash_shim.sh` writes to `boot_b` and switches the active slot; use only when that is intended. The one-shot helper and recovery script are documented below.

## One-shot boot of slot B (flash from the phone, fall back to A by itself)

`tools/oneshot_b.py` flashes the image to `boot_b`, makes B active and sets B's retry count to 1, so the bootloader returns to slot A after the shim has run once (AOSP A/B rule: unsuccessful slot loses a try per boot, at zero it is marked unbootable). Root + `bootctl` on the phone; it runs through adb, or on the phone itself with `--local` (Termux + python, as root).

```sh
python3 tools/oneshot_b.py probe                 # once: shows what bootctl changes in misc, restores slot A
python3 tools/oneshot_b.py arm out/shim_b.img --reboot
```

`arm` backs up `misc`, verifies `boot_b` by read-back, and only patches the retry count when the metadata parses as the AOSP `bootloader_control` block (magic `BCAB`, CRC matches); otherwise it writes nothing extra and prints the byte diff. Simulated in `tests/test_oneshot.py`; not yet run on the real phone. If slot B ever loops, `pc/back_to_a.sh` still works.
