[x] Boot the shim through the existing Android boot path and draw diagnostics on the framebuffer
[x] Read CPU state, DTB, bootargs and bootconfig; query PSCI version and CPU_ON support
[x] v0.5.1: fix EL1 stack setup and install EL1 exception vectors
[ ] Verify the exception handler on-device with the deliberate UDF test
[ ] Identify the exact Android ramoops/persistent_ram implementation and zone offsets
[ ] Add pstore-compatible persistent shim logging and verify retrieval from slot A
[ ] Derive a conservative physical-memory map from /memory and /reserved-memory
[ ] Bring up 16 KiB translation tables and validate RAM/MMIO/framebuffer attributes
[ ] Test PSCI_CPU_ON on one secondary CPU with a dedicated entry point and stack
[ ] Build an XNU payload, prepare an Apple Device Tree and boot_args
[ ] Transfer control to XNU and confirm execution of the early kernel entry
[ ] Continue with platform bring-up, storage and Darwin userspace
