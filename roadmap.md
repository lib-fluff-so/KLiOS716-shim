# Roadmap

[x] Boot the shim through the existing Android boot path and preserve the existing screen except for the bottom progress strip
[x] Read CPU state, DTB, bootargs and bootconfig; stream diagnostics to ramoops
[x] v0.5.1: EL1 stack setup and exception vectors; deliberate UDF test
[x] v0.5.2: identify ramoops/persistent_ram layout and confirm extraction after reset
[x] v0.6.0: derive RAM and reservation ranges from the live DTB; host-tested
[x] v0.6.3: 16 KiB translation tables + MMU on, caches off; confirmed on-device
[x] v0.6.3: PSCI_CPU_ON diagnostic for all seven secondary cores; confirmed on-device
[x] v0.7.0: ramoops-first logging, bottom progress strip, and raw newc test ramdisk
[x] v0.7.0: high-RAM test copy, read-back validation, and MMU-off handoff trampoline
[x] v0.7.1: corrected test-payload magic and confirmed physical test-payload handoff PASS (user-confirmed)
[x] v0.8.0: parse SWEETXNU v1 container in freestanding C; host adversarial tests
[x] v0.8.0: opt-in Mach-O packaging with chained-fixup handling and raw CPIO entry
[x] v0.8.0: generate KL7-oriented ADT from the checked-in DTB facts; host assertions
[x] v0.8.0: construct boot_args rev 2 / 736-byte ABI and I-cache invalidation at transfer
[ ] Obtain and validate a real D22AP Apple Device Tree including /product from the IPSW
[ ] Adapt/verify early framebuffer console, panic path, and keep-console-after-TTBR0 XNU patches for KL7
[ ] Boot an actual XNU kernelcache and capture the first XNU-side log on physical KL7
[ ] Audit and port target-specific GIC, timer, CPU_ON/SMP, framebuffer, and SoC configuration patches
[ ] Determine whether UFS can be initialized from the state left by LK/TF-A and implement a KL7 storage path
[ ] Identify the KL7 touch controller and port the input driver
[ ] Enable/validate SMP and cache/MMU setup under XNU once early single-core entry is stable
[ ] Bring up matching iOS userland only after the kernel and storage platform are usable
