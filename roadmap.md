[x] Boot the shim through the existing Android boot path; retain the original screen except for a bottom progress strip
[x] Read CPU state, DTB, bootargs and bootconfig; write complete diagnostic logs to ramoops
[x] v0.5.1: fix EL1 stack setup and install EL1 exception vectors
[x] Verify the exception handler on-device with the deliberate UDF test
[x] Identify the exact Android ramoops/persistent_ram implementation and zone offsets
[x] v0.5.2: write shim diagnostics into ramoops and confirm extraction after reset
[x] v0.6.0: derive physical-memory information from the live DTB (/memory, /reserved-memory no-map, /cpus); host-tested
[x] v0.6.3: 16 KiB translation tables + MMU on (Normal Non-cacheable, caches off); confirmed on-device
[x] v0.6.3: PSCI_CPU_ON of all seven secondary cores; each entered the shim and enabled the shared MMU tables, confirmed on-device
[x] v0.7.0: move diagnostics off-screen, stream to ramoops, and draw only bottom progress strip
[x] v0.7.0: generate raw newc CPIO containing a CRC-checked test payload; host test parser and packaging
[x] v0.7.0: validate high-RAM destination, copy test payload, verify read-back, and prepare x0=boot_args/MMU-off trampoline
[x] v0.7.0: physical KL7 confirmed CPIO/initrd discovery and payload return; found expected-vs-written magic mismatch
[ ] v0.7.1: re-run on physical KL7 to verify corrected test-payload magic and handoff PASS
[ ] Add robust cache maintenance and enable caches only after defining safe memory attributes
[ ] GICv3 CPU interface/redistributor bring-up + generic timer interrupt
[ ] Define an Apple Device Tree and actual XNU-compatible boot_args for the selected XNU source tree
[ ] Build and load a test XNU payload; confirm execution at its early kernel entry
[ ] Continue platform bring-up, storage and Darwin userspace
