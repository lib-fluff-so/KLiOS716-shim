# v0.5.1 correction notes

## Why v0.5 showed a black screen

The early assembly attempted `MSR SP_EL1, x9` while executing at EL1. On normal ARMv8-A implementations, an explicit write to `SP_EL1` from EL1 is UNDEFINED, so this faults before `VBAR_EL1` has been installed. No exception screen can be drawn at that point.

The hardware image now selects `SPSel=1` and sets the selected stack via `MOV SP, x9`, then installs the aligned exception vector table.

## Emulator fixes

- The Unicorn compatibility build skips privileged EL1 setup (`SPSel`/`VBAR_EL1`) that the lightweight harness does not model.
- It fakes the two PSCI query return values and uses a final `BRK` as a stop marker, so the harness can reach and capture the diagnostic pages.
- The harness captures from the known visible framebuffer offset `FB_BASE + 1230 * 4352`, uses stride 4352, and snapshots at 2, 6, 10 and 14 simulated seconds.
- The emulation compatibility build does not test real exception entry or PSCI behavior. Use the hardware exception-test image for the `UDF #0` test.

## Validation performed here

The normal hardware image, hardware exception-test image and emulator compatibility image all compile and link. Android boot-image headers, the embedded compressed/raw ARM64 Image header and `image_size` were validated. The Unicorn runtime itself was not run in this environment because the `unicorn` and `fdt` Python modules are unavailable and package installation has no network access. The physical device has not yet been retested with v0.5.1.
