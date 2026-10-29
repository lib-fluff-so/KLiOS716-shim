# XNU for the KL7 / MediaTek MT6789

This directory contains the XNU patch set, the local macOS VM build tooling,
and the iOS-root helpers used by the KL7 port. It is **not** a copy of the
`ios-sweet` git history. The base patches are attributed to
[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet); the KL7
delta is `0101`-`0117` and is maintained here.

## Read this first

- **The verified build set is `xnu/patches/`.** It is the exact sweet/KL7
  subset that was copied into `blacktop/darwin-xnu-build` for the last
  successful local VM build (`1bfe9cd7...`, 2026-10-27).
- **`0082-kl7-machine-config.patch` does not exist and must not be recreated.**
  It was a hallucinated, non-building patch. The real KL7 work starts at
  `0101`. Any reference to `0082` or `MACHINE_CONFIG=KL7` in older notes is
  historical and superseded.
- **`0110` is absent from the verified build.** `0101` is present, but
  `build.sh` rejects it with `corrupt patch at line 83` and continues.
  `0102`-`0109` and `0111`-`0117` are applied.
- **There are no GitHub Actions macOS runners.** Builds happen only in the
  local macOS VM described in `../vm/README.md`.
- The full 105-patch `dxb/patches` tree, including blacktop's eight patches,
  is preserved in `vm-build-2026-10-27/` for exact provenance. Do not copy
  the blacktop patches into `xnu/patches/`.

## Verified build

| Item | Value |
| --- | --- |
| Date | 2026-10-27 |
| VM | macOS Sonoma 14.6, QEMU/KVM, local host |
| Xcode | `/Applications/Xcode.app/Contents/Developer` |
| `MACOS_VERSION` | `13.5` (xnu-8796.141.3) |
| `KERNEL_CONFIG` | `DEVELOPMENT` |
| `MACHINE_CONFIG` | `SWEET` |
| blacktop `dxb` SHA | `663906528d1cd5ea8fde7de6f16de73db9511549` |
| XNU source SHA from log | `1b191cb58250d0705d8a51287127505aa4bc0789` |
| Kernel | `kernel.development.sweet` |
| Kernel SHA-256 | `1bfe9cd7b07d8fd14ca6e068bf93f776bfafecf5dc0797bfd6c796ac003c86ff` |
| Build log | `vm-build-2026-10-27/build.log.gz` |

The `vm/out/SHA256SUMS` file contains `8f3cb86d...`, an older kernel copied
out on 2026-10-23. It is not the latest build. `xnu/out/SHA256SUMS` and
`xnu/vm-build-2026-10-27/out-SHA256SUMS` are the `1bfe9cd7...` result.

## Layout

```
xnu/
├── README.md                     this file
├── HISTORY.md                    patch/build chronology and host/VM divergence
├── NOTES.md                      the working lab notebook (historical)
├── XNU-KL7-PORT-PLAN.md          the original 2026-10-22 plan (historical)
├── build-xnu.sh                  local replica of the retired CI build
├── patches/                      verified sweet/KL7 patch set (97 + 3 scripts)
├── vm-build-2026-10-27/          exact dxb patch/build snapshot (105 + 3)
├── ios-root/                     iOS-root preparation scripts and sparse manifest
├── tools/                        ADT/payload/root/inspection utilities
└── out/SHA256SUMS                latest verified kernel hash
```

`patches/` contains:

- `0001`-`0081`: base `ios-sweet` patch train, attributed to dipa-studio.
- `0101`-`0117`: KL7 delta, this project. There is no `0082` and no `0110`
  in the verified set.
- `0019-stage-libpthread.sh`, `0020-stage-bearssl.sh`,
  `0026-stage-hfs.sh`: source-staging scripts, not patches.
- `SHA256SUMS`: checksums for the directory.

The exact VM snapshot additionally contains blacktop's
`TrustCache.patch`, `iobuffermemd_monterey.patch`, `kas_info.patch`,
`machine_routines.patch`, `skywalk_sonoma.patch`, `skywalk_ventura.patch`,
`syntax_checker_sonoma.patch`, `syntax_checker_ventura.patch`, their
`14.4/`, `15.0/`, `26.0/` directories, and `README.md`. Those belong to
blacktop and are fetched with the pinned `dxb` checkout; they are not part
of the repo's default patch directory.

## Build in the VM

Start the VM with the known-good CPU model and installer media layout:

```sh
/home/fluff_/macOS-VM/run/start-vm.sh
# or the explicit command in ../vm/README.md
sshpass -p 1602 ssh -p 2222 dev@127.0.0.1
```

The VM build tree is `~/kbuild/ios-sweet`; the wrapper is
`~/kbuild/build-xnu.sh`; the output is
`~/kbuild/out/kernel.development.sweet`.

### Sync the repo's patch set into the VM

This replaces the VM's `~/kbuild/ios-sweet/xnu/patches` with the verified
set. Back up any local VM-only experiments first.

```sh
rsync -a --delete xnu/patches/ \
  dev@127.0.0.1:~/kbuild/ios-sweet/xnu/patches/
scp xnu/build-xnu.sh dev@127.0.0.1:~/kbuild/build-xnu.sh
```

Then build:

```sh
sshpass -p 1602 ssh -p 2222 dev@127.0.0.1 \
  'cd ~/kbuild && env MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT \
   MACHINE_CONFIG=SWEET MAKEFLAGS=-k bash build-xnu.sh'
```

`build-xnu.sh` performs a fresh shallow checkout of the pinned blacktop
commit, copies the repo patches next to blacktop's patches, applies the
local `ARCHS` change, runs `build.sh`, and copies the kernel to
`~/kbuild/out/`.

Check the log for the intentional `0101` skip:

```text
Skipping patch .../0101-kl7-early-framebuffer-geometry.patch
  ... error: corrupt patch at line 83
```

The other skip messages from blacktop's own patches are expected when
those patches are already present in the pinned tree or when an external
header already exists. The build must still reach the kernel link and
produce `kernel.development.sweet`.

Record the resulting hash:

```sh
sshpass -p 1602 ssh -p 2222 dev@127.0.0.1 \
  'cat ~/kbuild/out/SHA256SUMS'
```

The last verified hash is `1bfe9cd7...`; do not assume a new build is
byte-identical, because XNU build products can contain timestamps and other
non-deterministic metadata. Verify that the build completes and that all
intended patches appear in the log.

## Reproducing the exact 2026-10-27 tree

`vm-build-2026-10-27/` is a byte-for-byte snapshot of the VM's
`~/kbuild/dxb/patches` plus the modified `build.sh` used for the build.
It contains 105 patches and 3 staging scripts. See
`vm-build-2026-10-27/README.md` and `MANIFEST.sha256`.

For a normal build, use the repo's `patches/` and the pinned blacktop SHA;
the eight blacktop patches are already in that SHA. For an exact forensic
restore, unpack the full `vm-build-2026-10-27/patches` over a `dxb` checkout
at the pinned SHA and run the snapshot `build.sh`. Do not use the full
snapshot as the repo's default patch directory.

## Patch provenance and divergence

- `0001`-`0081` are the `ios-sweet` base train. They are included as
  patches, not as imported git history.
- `0101`-`0117` are the KL7 changes. `0101` is a known-corrupt early
  framebuffer patch kept only for the record; `0102` and later carry the
  working KL7 platform, ramoops, UFS, display, touch, keys, battery and
  UFS-performance work.
- The host `ios-sweet` repository at `/home/fluff_/ios-sweet` is on branch
  `kl7-xnu-bringup` at `661cc28` and has **newer, unverified variants** of
  `0004`, `0010`, `0012`, `0016`, `0018`, plus `0114` but not `0115`-`0117`.
  The VM build used the older pre-KL7-machine-config base variants plus
  `0114`-`0117`. Do not sync the host working tree over the verified VM set
  without a full build test.
- `0082` was deleted from the host working tree on 2026-10-29. It still
  exists in the old `ios-sweet` git history; that history is not part of
  this repository.
- The host `ios-sweet` `NOTES.md` is copied here as `NOTES.md`; its
  references to `0082` and `MACHINE_CONFIG=KL7` describe the abandoned
  approach.

## iOS root and userland

`ios-root/` contains the offline-root preparation scripts: `post-root.sh`
for the VM-side `/Users/dev/ios/IOSRoot` tree, the `v18-tweaks` scripts for
iOS 16.7.16 userland, and the `patch-v17` sparse manifest. The v17 chunk
blobs were not preserved; see `ios-root/patch-v17/README.md` before using
that directory.

The XNU-side helpers in `tools/` generate the KL7 ADT, package the
`SWEETXNU` payload, build the static `sweet-init`, validate exports, and
inspect the build tree. They are host tools, not part of the kernel patch
series.

## Validation status

The 2026-10-27 VM build completed and produced `1bfe9cd7...`. The shim
work and the on-device results are documented in `../SHIM-HISTORY.md` and
`NOTES.md`. This directory does not claim that a newly built kernel boots;
the VM build only proves compilation. The next verification step is to
sync this patch set to the VM, rebuild, compare the build log, and test the
resulting image on `boot_b` only.

## Related files

- `../vm/README.md` - local macOS VM launcher and install media
- `../SHIM-HISTORY.md` - shim and on-device chronology
- `../REPOSITORY.md` - repository map and operating rules
- `HISTORY.md` - XNU patch/build history
- `NOTES.md` - detailed lab notebook
- `vm-build-2026-10-27/README.md` - exact build snapshot
