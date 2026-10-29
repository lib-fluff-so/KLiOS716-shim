# KL7 repository guide

This repository is the consolidated home of the TECNO Spark 30 Pro (KL7,
MediaTek MT6789) bring-up:

- the `ios-kl7` ARM64 shim and its on-device tests;
- the UFS, touch, keys and battery work;
- the XNU patch set and local macOS VM build tooling;
- the offline iOS-root preparation scripts;
- the GPU bring-up notes and experiments.

The shim and XNU are separate histories. The old `ios-sweet` git history is
not imported; its base patches are attributed in `xnu/HISTORY.md`. The
KL7-specific XNU delta is `0101`-`0117` only. `0082` was a hallucinated,
non-building patch and is not part of this repository.

## Start here

| Document | Contents |
| --- | --- |
| `SHIM-HISTORY.md` | reconstructed shim releases, UFS/touch/battery line, history mechanics |
| `xnu/README.md` | authoritative XNU patch set, VM build, sync/build commands |
| `xnu/HISTORY.md` | XNU patch chronology and host/VM divergence |
| `vm/README.md` | local macOS VM launcher, install flow, QMP tools |
| `xnu/ios-root/README.md` | offline iOS root and userland helpers |
| `RECOVERY-NOTES.txt` | flashing, slots, pstore and warm-reset procedure |
| `GPU-BRINGUP.md` | GPU hardware map, descriptor experiments and results |
| `NOTES.md` | original lab notebook; historical, includes abandoned 0082 work |

`README.md` is the v0.9.1-era shim description. It is kept for historical
context; `SHIM-HISTORY.md` and the source are authoritative for the current
tree.

## Branches and tags

- `main` - reconstructed v0.10.0 shim line. The old `main` is preserved as
  `backup/main-v0100` at `19eb1bf`.
- `ufs-test` - active shim line with UFS, display, touch, keys, battery and
  the guarded charger test. The consolidated XNU/VM files are added on top.
- `backup/ufs-test-4405c9f` - old pre-rebuild `ufs-test` tip.
- `v0.5`...`v0.10.0` - shim release tags.
- `ufs-v1`...`ufs-v7`, `xnu-smp8-no-timer-wdt`, `xnu-ufs-adt1`,
  `xnu-ufs-iosroot1`, `xnu-ufs-iosroot-rw1` - UFS/root milestones.
- The full pre-rewrite refs are also in
  `refs-backup/ios-kl7-before-history.bundle`.

The release tags are lightweight except `xnu-ufs-iosroot1`, which is
annotated with the original message and tagger.

## Layout

```
.
├── src/                 shim source (C/asm)
├── tests/               host regression tests
├── tools/               shim-side Python tooling
├── phone/, pc/          phone/PC boot helpers; large images are ignored
├── releases/v0.5.2/     surviving v0.5.2 images + checksums
├── fdt.dtb              live KL7 device tree used by tests/packers
├── xnu/                 XNU patch set, VM build wrapper, tools, iOS root
├── vm/                  local macOS VM launcher, OpenCore, installer helpers
├── SHIM-HISTORY.md
├── REPOSITORY.md
├── RECOVERY-NOTES.txt
└── GPU-BRINGUP.md
```

Large generated artifacts (`out*/`, VM disks, installer media, phone
dumps/images) are intentionally ignored. Keep permanent copies outside git
under `/home/fluff_/kl7/` or a backup disk.

## Shim build and test

Dependencies on Arch Linux:

```sh
sudo pacman -S --needed base-devel clang lld llvm python
```

Build the default test image and run host regression tests:

```sh
make clean
make -j"$(nproc)" all
make hosttest loadertest xnutest
make sizes
```

The main artifact is `out/shim_b.img`; `out/shim_raw_b.img` is the
uncompressed test variant. The shim's `make` targets are in the root
`Makefile`. Battery/charger and GPU test builds use `BATTERY_TEST=1`,
`CHARGER_WRITE_TEST=1`, `GPU_TEST=1`, etc.; see `xnu/NOTES.md` and
`GPU-BRINGUP.md` for the exact commands.

## XNU build

Only the local macOS VM is supported. There are no GitHub Actions macOS
runners. The verified patch set is `xnu/patches/` (97 sweet/KL7 patches and
3 staging scripts, no `0082`, no `0110`). The exact 105-patch VM snapshot is
in `xnu/vm-build-2026-10-27/` for provenance.

```sh
# start the VM (see vm/README.md for prerequisites)
/home/fluff_/macOS-VM/run/start-vm.sh

# sync the verified patch set and wrapper
rsync -a --delete xnu/patches/ \
  dev@127.0.0.1:~/kbuild/ios-sweet/xnu/patches/
scp xnu/build-xnu.sh dev@127.0.0.1:~/kbuild/build-xnu.sh

# build
sshpass -p 1602 ssh -p 2222 dev@127.0.0.1 \
  'cd ~/kbuild && env MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT \
   MACHINE_CONFIG=SWEET MAKEFLAGS=-k bash build-xnu.sh'
```

The last verified kernel is
`1bfe9cd7b07d8fd14ca6e068bf93f776bfafecf5dc0797bfd6c796ac003c86ff`.
`vm/out/SHA256SUMS` contains an older copy and is not the latest build.
`xnu/patches/0101` is known-corrupt and is skipped by `build.sh`; inspect the
log. `0115`-`0117` are experimental. Do not restore `0082`.

## Current device state

- Android lives on slot `_a`; it is the fallback.
- Shim/XNU test images go to `boot_b` only.
- The last tested `boot_b` image is the unique-sentinel/vendor `JS_CONFIG`
  image; no new shim image was flashed during the history rebuild.
- Fastboot flow: `adb -s 13016374AF000229 reboot bootloader`, then
  `fastboot flash boot_b .../shim_b.img`, `fastboot --set-active=b`,
  `fastboot reboot`. The slot-B retry window is about 90 seconds.
- From iOS, start Antumbra first and wait for the transient BROM port.
- Warm-reset log tests must end with the built-in PSCI timer; a fastboot,
  power-key or watchdog reset loses the shim ramoops.

### Hard safety rules

- **Never flash `boot_a`.** Only `boot_b`.
- **`boot_b` must stay at or below 64 MiB.**
- Do not use `/home/fluff_/Downloads/recovery.img` or
  `/home/fluff_/TWRP/recovery.img`.
- The battery/charger path is read-only by default; slow charging is gated
  behind `kl7slowchg=1`. Do not leave an unsafe charger setting on the
  device. The shim's guarded write test disables OTG, checks the fault
  register, and is not the default.
- Do not add CPU scaling or anything that makes the phone slower.
- Sudo on the host uses password `1602`:
  `printf '1602\n' | sudo -S -p '' ...`.

## GPU bring-up

The GPU work is the current line after the UFS/touch/battery line. The
hardware map, descriptor layout, test images and the half-clear result are
in `GPU-BRINGUP.md`. The working tree is stashed as
`gpu-wip-before-history-rebuild` and is restored and committed separately
from the consolidation.

Current descriptor decisions to preserve:

- FBD: 64-byte parameters + 64-byte padding + 64-byte render target at
  offset 128;
- RT base in RT words 8-9, row stride word 10;
- FBD pointer tag `fbd_va | 1`;
- tiler heap size in aligned bytes;
- color allocation `bytes >> 10`;
- linear block format 2, internal format R8G8B8A8 = 1, writeback format 19,
  swizzle `0x688`.

The half-clear is suspected to be a physical-address parity/memory-path
effect, not an FBD mismatch. Test that before changing descriptors.

## Source and artifact policy

- Shim release history was rebuilt from the Downloads source archives; the
  v0.5.2 release is image-only and v0.9.0/v0.9.1 had no separate source
  archives. See `SHIM-HISTORY.md`.
- The XNU base patches are from dipa-studio/ios-sweet; the KL7 delta is
  `0101`-`0117`. No `ios-sweet` git history is imported.
- Keep `/tmp` test artifacts that matter under `/home/fluff_/kl7/` or in
  `opencode-archive/`; `/tmp` is not permanent.
- Backups: `refs-backup/ios-kl7-before-history.bundle`,
  `backup/main-v0100`, `backup/ufs-test-4405c9f`, and the stash listed above.

## What is not in git

- VM disks and installer media (`/home/fluff_/macOS-VM/img/*`).
- Generated OpenCore image (`OpenCore/OpenCore.qcow2`).
- Phone partition dumps, boot images, pstore logs, and build outputs.
- The `patch-v17` chunk blobs; only the manifest survived.
- The original `ios-sweet` history and its abandoned `0082` commit.

## Verification checklist after a change

1. Shim: `make clean && make -j$(nproc) all && make hosttest loadertest xnutest`.
2. XNU: sync `xnu/patches/`, build in the VM, inspect every patch apply/skip,
   confirm the kernel link, record the hash.
3. Device: flash only `boot_b`, keep Android on `_a`, confirm the slot-B retry
   window, pull `console-ramoops-0` before the next boot, and record the
   exact image/hash.
4. Update `SHIM-HISTORY.md`, `xnu/HISTORY.md`, and `GPU-BRINGUP.md` with the
   result. Do not leave a successful test only in `/tmp`.
