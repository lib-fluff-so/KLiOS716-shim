# v0.9.1 notes

- Replaced the test-payload success halt with PSCI `SYSTEM_RESET` (`0x84000009`), using the `smc` conduit declared by the supplied KL7 DTB.
- In the default test build, failures and shim exceptions also request reset instead of leaving the device parked, so seven B-slot attempts can complete without manual power-cycling. If PSCI reset returns, the shim logs the status and parks rather than spinning through repeated SMC calls.
- Preserve a valid existing ramoops console ring across warm retries and append a boot separator instead of unconditionally clearing it at every shim entry.
- `XNU_PAYLOAD=1` keeps stop-on-error behavior; automatic reset is enabled by default only in the test-payload mode.
- The existing host-side payload, loader, DTB/memmap and XNU-container tests remain required. A physical reset + persistent ramoops sequence still needs validation on the KL7. The expected log markers per attempt are `HANDOFF PASS` and `REBOOT: test passed; PSCI SYSTEM_RESET`; the final Android boot should expose all preserved attempts under `/sys/fs/pstore` if the ramoops region survives the warm resets.
