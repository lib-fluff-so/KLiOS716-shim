[x] Boot the shim through the existing Android boot path and draw diagnostics on the framebuffer
[x] Read CPU state, DTB, bootargs and bootconfig; query PSCI version and CPU_ON support
[x] v0.5.1: fix EL1 stack setup and install EL1 exception vectors
[x] Verify the exception handler on-device with the deliberate UDF test
[x] Identify the exact Android ramoops/persistent_ram implementation and zone offsets
[x] v0.5.2: write shim diagnostics into the ramoops console and verify extraction after reset (confirmed on device)
[x] v0.6.0: derive the physical-memory map from the live DTB (/memory, /reserved-memory no-map, /cpus); host-tested against fdt.dtb
[~] v0.6.0: 16 KiB translation tables + MMU on (Normal Non-cacheable, caches off); built and host-verified, device test pending
[~] v0.6.0: PSCI_CPU_ON of every secondary core, each enabling the MMU on the same tables; device test pending
[ ] Enable caches (WB mappings + cache maintenance), then atomics/spinlocks
[ ] GICv3 bring-up (GICD 0x0c000000, GICR 0x0c040000) + generic timer interrupt
[ ] Build an XNU payload, prepare an Apple Device Tree and boot_args
[ ] Transfer control to XNU and confirm execution of the early kernel entry
[ ] Continue with platform bring-up, storage and Darwin userspace
