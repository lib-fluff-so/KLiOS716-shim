# Roadmap — ios-kl7 v0.9.1

## Shim / loader

[x] Enter shim through the existing Android/LK boot path at EL1 with MMU off
[x] Parse live FDT RAM, `/reserved-memory`, `no-map`, and FDT reservation-map entries
[x] Locate raw `newc` CPIO in the initrd declared by `linux,initrd-start/end`
[x] Validate the `KL7PAY0` test payload and preserve the physical handoff regression path
[x] Parse the `SWEETXNU` container and preflight Mach-O segment placement
[x] Copy Mach-O segments and BSS; copy ADT; form `boot_args` rev. 2; pass its physical address in `x0`
[x] Transfer with MMU/caches disabled and invalidate I-cache before branching
[x] Replace the progress strip with a text framebuffer console
[x] Route one log stream to both framebuffer and ramoops
[x] Remove page-table construction, PSCI `CPU_ON`, GIC/timer probes, and emulator-only shim mode
[x] Reduce exception output to ESR/FAR/ELR/SPSR
[x] Host tests for DTB memory map, legacy payload, SWEETXNU parser, packaging and ADT generation

[ ] Verify the v0.9.1 framebuffer console on the physical KL7
[ ] Verify the v0.9.1 ramoops log after rebooting back into Android
[ ] Add an explicit hardware test payload that validates all fields of a v0.9 boot ABI and returns
[ ] Test a real (not synthetic) Mach-O through the complete CPIO → container → segment-copy path
[ ] Confirm the current boot_args / ADT placement contract against the exact XNU build being targeted

## Not shim work (kept out of this release)

- Port XNU's early console and PlatformExpert to MT6789
- Adapt XNU GIC/timer/SMP and interrupt initialization
- Determine whether UFS can be started from the state left by LK/TF-A; implement a KL7 storage path if needed
- Identify and support KL7's touch controller
- Bring up matching iOS userland after the kernel and storage work
