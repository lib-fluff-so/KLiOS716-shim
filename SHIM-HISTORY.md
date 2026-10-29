# Shim history

This is the history of the `ios-kl7` ARM64 shim, not the XNU patch train.
The shim boots from an Android boot image, talks to the live KL7 firmware
state, and hands off either a test payload or the `SWEETXNU` container.
The kernel-side history is in `xnu/HISTORY.md`.

## Reconstructed release history

The old repository had the release source only as a pile of Downloads
archives plus a separate `ufs-test` branch. The history was rebuilt from
those archives in order, then the existing 46 UFS/touch/battery commits were
replayed on top. The reconstructed v0.10.0 tree is byte-identical to the old
`main` at `19eb1bf`; the replayed tree was byte-identical to the old
`ufs-test` HEAD at `4405c9f` before this consolidation.

| Tag | Date (2026) | Commit | What it added |
| --- | --- | --- | --- |
| `v0.5` | 10-21 00:33 | `992646e` | initial EL1 bring-up: framebuffer diagnostics, CPU/DTB/bootargs/PSCI report, 2 KiB-aligned exception vectors, ESR/FAR/ELR/SPSR dump |
| `v0.5.1` | 10-21 00:48 | `d468eff` | fix SP_EL1 selection and exception vectors; emulator compatibility and capture offset |
| `v0.5.2` | 10-21 01:36 | `7134a29` | ramoops console mirror; source archive was not preserved, so this commit keeps the v0.5.1 tree plus the release note and both surviving images |
| `v0.6.0` | 10-21 17:43 | `77f0bf2` | identity-map live DTB with 16 KiB granule, parse memory/reservations/GIC/PSCI/8 CPUs, MMU on, PSCI secondaries, host tests |
| `v0.6.1` | 10-21 17:51 | `195c1c1` | secondary-core framebuffer markers and AFFINITY_INFO diagnostics |
| `v0.6.2` | 10-21 17:54 | `c88ff95` | clean/invalidate secondary context before `CPU_ON`; stage colours |
| `v0.6.3` | 10-21 17:59 | `7a78d72` | build per-core context from a PC-relative base; record received `x0` |
| `v0.7.0` | 10-21 21:12 | `2c55280` | ramoops-first UI, test-payload handoff, CPIO/DTB parsing, high-RAM preflight, result ABI |
| `v0.7.1` | 10-21 21:29 | `be1e9cd` | fix the handoff result marker (`0x5345`, not `0x4553`) |
| `v0.8.0` | 10-21 22:39 | `3e24606` | optional `SWEETXNU` loader: Mach-O segments/BSS, ADT, `boot_args` rev 2, host packer/tests |
| `v0.8.0-ctf-hotfix` | 10-21 22:57 | `42c3df9` | accept the non-mapped `__CTF` segment with `vmsize == 0` |
| `v0.9.2` | 10-22 17:56 | `ca16cc4` | v0.9.0 scope reduction and v0.9.1 loader contract; watchdog diagnostics/disable; aligned XNU copies. No separate v0.9.1 source archive was preserved |
| `v0.9.3` | 10-23 21:45 | `41d1962` | XNU framebuffer base `0xFE519E00`; add the early `patches-kl7` staging tree (0101-0103) |
| `v0.10.0` | 10-24 11:36 | `2994351` | PSCI `CPU_ON` timer on cpu1 with `SYSTEM_RESET` after 50 s, WDT backstop, `VISIBLE_OFFSET 0`; old `main` tree |

### Source gaps and image-only releases

- **v0.5.2** has no source archive. The commit preserves
  `FIX-NOTES-v0.5.2.md` and the two 16 KiB images under
  `releases/v0.5.2/`; the source tree is v0.5.1. v0.6.0 is the first source
  snapshot that contains the ramoops implementation.
- **v0.9.0 and v0.9.1** have no separate source archives. The v0.9.2 archive
  is the first compact-loader source snapshot and its message covers the
  v0.9.0/v0.9.1 scope.
- **v0.10.0** is the old `main` tree (`19eb1bf`) byte-for-byte. It does not
  include the separate v0.9.3 `patches-kl7` staging directory; the XNU work
  continued outside this repository until the consolidation.

## UFS/touch/battery line

The active shim line after v0.10.0 is `ufs-test`. The 46 replayed commits
cover:

- UFS probe and UFSHCI/GPT reads (`ufs-v1` `7871d27`)
- UCD layout, MTK HPM/H8 link resume (`ufs-v2` `4007d85`, `ufs-v3`
  `4244410`)
- MTK SiP reset/refclk/link startup (`ufs-v4` `c93c1f8`)
- HCI/UniPro reset and HCE init (`ufs-v5` `6f395c0`)
- response-UPIU sense handling (`ufs-v6` `c3b2a77`)
- `fDeviceInit` handshake (`ufs-v7` `88df426`)
- ADT UFS absolute address (`xnu-ufs-adt1` `5e100d3`)
- boot from `iosroot` (`xnu-ufs-iosroot1` `5f32b9f`)
- UFS writes and iOS root boot args (`xnu-ufs-iosroot-rw1` `e34dddf`)
- eight CPUs to XNU; disable CPU1 reset timer (`xnu-smp8-no-timer-wdt`
  `dbe4cc9`)
- HS-G1/throughput, PRDT reads, residual transfer accounting
- display canvas/`/product` identity, touch bring-up, FTS3519 report parsing,
  kernel-console mute, boot vibration
- read-only battery AUXADC and SGM41516D probes, then the guarded
  SGM41516D charge write test at `edc8ffd`

The charge-write test is deliberately gated: it is not the default battery
path, it refuses to write on a fault, it never enables OTG, and it leaves
the safety timer disabled only as an explicit test setting. iOS charging
must be boot-arg gated (`kl7slowchg=1`) and must pet the SGM watchdog.

## Current working tree

After the release history, the shim gained the UFS/touch/battery work above.
The GPU bring-up is the current line of work. Its hardware map and test
results are in `GPU-BRINGUP.md`; the code is in the stashed GPU change set
and is committed separately from this history consolidation. The current
half-clear investigation is about a physical-address parity/memory-path
effect, not a proven FBD descriptor mismatch.

The last hardware test image on `boot_b` is the unique-sentinel/vendor
`JS_CONFIG` image; Android remains on slot `_a`. No new shim image was
flashed during the history rebuild.

## History mechanics

- Old refs preserved as `backup/main-v0100` (`19eb1bf`) and
  `backup/ufs-test-4405c9f` (`4405c9f`), plus a full bundle at
  `refs-backup/ios-kl7-before-history.bundle`.
- The rebuilt history was made in a throwaway clone, not by rewriting the
  real repository in place.
- The 11 `ufs-*`/`xnu-*` tags were recreated by matching the original commit
  subjects. `xnu-ufs-iosroot1` is annotated again with the original message
  and tagger; the other tags were lightweight originally and remain so.
- The old `main` branch is kept as `backup/main-v0100`; the new `main` is the
  reconstructed v0.10.0 line. `ufs-test` is the active line.
- The `ios-sweet` git history is not imported. See `xnu/HISTORY.md` for the
  base-patch attribution and the removed `0082` hallucination.

## Safety rules

- Flash **only** `boot_b`. Never write `boot_a`.
- Keep `boot_b` at or below 64 MiB.
- Prefer fastboot while Android is running:
  `adb -s 13016374AF000229 reboot bootloader`, then
  `fastboot flash boot_b ...`, `fastboot --set-active=b`, `fastboot reboot`.
- From iOS, start Antumbra first and wait for the transient BROM port.
- End warm-reset log tests with the built-in PSCI timer, not a fastboot,
  power-key or watchdog reset. A cold reset loses the shim ramoops.
- Do not use the Downloads or TWRP `recovery.img` artifacts.
- Do not add CPU scaling or other changes that make the phone slower.

## Related files

- `README.md` - the v0.9.1-era shim description (historical)
- `REPOSITORY.md` - repository map and current operating rules
- `xnu/HISTORY.md` - XNU patch/build history
- `xnu/README.md` - XNU build and sync
- `NOTES.md` - detailed lab notebook
- `RECOVERY-NOTES.txt` - flashing, pstore and warm-reset procedure
- `GPU-BRINGUP.md` - GPU hardware map and test results
