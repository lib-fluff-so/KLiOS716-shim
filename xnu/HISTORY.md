# XNU patch and build history

This file records the XNU side of the KL7 port. The shim's version history is
separate; see `../SHIM-HISTORY.md`.

## 1. Base train, `0001`-`0081`

The base series comes from the `ios-sweet` project by dipa-studio. It is kept
as a patch set so the KL7 repository does not import the upstream git
history. The broad groups are:

| Range | Purpose |
| --- | --- |
| `0001`-`0018` | clang compatibility, `MACHINE_CONFIG` support, non-PAC ARM64, open-source pmap/config, private-header stubs, early framebuffer, physmap, debug console, RNG/crypto, device tree and boot CPU |
| `0019`-`0033` | bundled pthread/BearSSL/HFS, mockfs/ramdisk root, UFS root and writes, AMFI/trust-cache, AppleKeyStore, read-write root |
| `0034`-`0049` | UFS DMA, AMFI entitlements, bindfs, watchdog, crash logs, IOMFB/IOSurface, persistent console, devnull tee, IOKit tracing, sandbox policy |
| `0050`-`0068` | crash/sampler work, HFS rename, process sampler, framebuffer present, HID/iPhone identity, touch/power/battery, SMP/PSCI, console mute, UFS speed |
| `0069`-`0081` | UFS faster path, hibernation, touch edges, USB probe/device/ECM/live log, 8-core SMP, display power, net trace, USB link state |

The base series was authored for the sweet/SM7150 target. Many patches are
guarded by `ARM64_BOARD_CONFIG_SWEET`; the KL7 patches below reuse the SWEET
configuration while adding KL7-specific matching and devices. The host
`ios-sweet` branch later gained newer variants of several base patches
(`0004`, `0010`, `0012`, `0016`, `0018`); those are **not** the versions in
the verified VM build. See section 5.

## 2. KL7 delta, `0101`-`0117`

The real KL7 patch train starts at `0101`. There is no `0082` in this
repository.

| Patch | Subject | Status in the verified VM build |
| --- | --- | --- |
| `0101` | early framebuffer geometry | **known defect**: `build.sh` rejects it with `corrupt patch at line 83`; kept for the record, not applied |
| `0102` | match platform/CPU/fbcon nodes | applied |
| `0103` | ramoops console mirror | applied |
| `0104` | ramoops early-console replay | applied |
| `0105` | skip RTC wait | applied |
| `0106` | BSD init debug | applied |
| `0107` | MTK UFS | applied |
| `0108` | UFS delay us | applied |
| `0109` | match AppleCLCD display | applied |
| `0110` | arm-io display hierarchy | **absent** from the verified build set |
| `0111` | sampler trap number | applied |
| `0112` | FTS3519 touch | applied |
| `0113` | keys and vibrator | applied |
| `0114` | battery telemetry and gated slow charging | applied |
| `0115` | UFS I/O statistics | applied (experimental) |
| `0116` | UFS phase benchmark | applied (experimental) |
| `0117` | UFS HS-G3 | applied (experimental) |

The on-device work established that `0102`-`0109` and `0111`-`0114` are
useful; `0115`-`0117` are UFS measurement/performance work and remain
experimental. `0110` was intentionally left out of the last build. `0101`
is still corrupt; do not pretend it applies. The early panel console was
consequently not available from the kernel until the later display work,
and the shim's own framebuffer console was used for bring-up.

### `0082` is not real

`0082-kl7-machine-config.patch` was a ChatGPT-generated "KL7 machine config"
patch that does not build. It was removed on 2026-10-29 from the host
`ios-sweet` working tree, the VM source trees, the Downloads artifact, and
this consolidated repository. The old `ios-sweet` git history still contains
the commit that added it; that history is not imported here. The KL7 changes
that actually work are the `0101`+ patches, not a new `MACHINE_CONFIG=KL7`.

## 3. Build chronology

| Date | Build / kernel | Patches | Notes |
| --- | --- | --- | --- |
| 2026-10-25 | `6689ce2e...` (v0113/v0114) | `0101`-`0111`; `0101` skipped | VM build; early console still targeted sweet's framebuffer address, so the KL7 panel stayed blank; `0111` sampler format string verified in the binary |
| 2026-10-26 | `6bca36e9...` (v0121 touch) | adds `0112` | FTS3519 touch works on hardware; only `boot_b` flashed |
| 2026-10-26 | `50eb4e5e...` (v0123 keys) | adds `0113` | volume/power keys and boot vibration work on hardware |
| 2026-10-27 | `1bfe9cd7...` (last full VM build) | `0102`-`0109`, `0111`-`0117`; `0101` skipped; no `0110` | battery `0114` and UFS `0115`-`0117` included; build log `vm-build-2026-10-27/build.log.gz` |

The `8f3cb86d...` kernel in `../vm/out/SHA256SUMS` is an older host copy
from 2026-10-23; it is not the latest VM build. The authoritative current
hash is `1bfe9cd7...` in `out/SHA256SUMS`.

## 4. Build environment

The only supported build path is the local macOS VM:

- macOS Sonoma 14.6, QEMU/KVM, Skylake-Client CPU model.
- Xcode at `/Applications/Xcode.app/Contents/Developer`.
- `MACOS_VERSION=13.5`, `KERNEL_CONFIG=DEVELOPMENT`, `MACHINE_CONFIG=SWEET`.
- blacktop `darwin-xnu-build` pinned at
  `663906528d1cd5ea8fde7de6f16de73db9511549`.
- XNU source SHA in the last log: `1b191cb58250d0705d8a51287127505aa4bc0789`.
- Output: `~/kbuild/out/kernel.development.sweet`.

There are no GitHub Actions macOS runners. The old `xnu.yml` workflow
references in `NOTES.md` and `ios-sweet-README.md` are historical.

The wrapper `build-xnu.sh` does a fresh shallow `dxb` checkout, copies
`ios-sweet/xnu/patches/*.patch` and `*.sh`, applies the local `ARCHS` edit,
runs blacktop's `build.sh`, and copies the kernel artifacts. The VM's last
build used the exact set preserved here.

## 5. Host/VM divergence

The host `ios-sweet` working tree is **not** the verified VM build:

- Host HEAD: `661cc28` on `kl7-xnu-bringup`.
- Host base patches `0004`, `0010`, `0012`, `0016`, `0018` are newer
  KL7-guard variants. The VM build used the older pre-KL7-machine-config
  variants.
- Host has `0114` (battery) but not `0115`-`0117`.
- The VM has `0114`-`0117`, no `0110`, and no `0082`.
- The host working tree has the `0082` deletion; the host git history still
  contains the old commit.

The repo's default `xnu/patches/` is the VM set because that is the set that
builds. If the host variants are to replace it, first sync them to the VM,
build, inspect every patch-apply line, and test the result. Do not assume the
host series is a drop-in update.

## 6. iOS root and userland

The XNU kernel work is one half of the port. The root userland is prepared
offline with `ios-root/post-root.sh` and the `v18-tweaks` scripts:

- `mkiosroot.sh` builds a single HFS+ volume from the iOS OS and cryptex
  images, preserves owners/modes/hard links, creates the shared-cache links,
  logd stores, and the no-passcode system keybag.
- `post-root.sh` repeats the VM-side/offline fixups for
  `/Users/dev/ios/IOSRoot`.
- `sweet-root-tweaks.py` applies the iOS 16.7.16 bring-up edits: launchd
  panic behavior, containers, migration marker, audio/MediaToolbox workarounds,
  SpringBoard watchdogs, setup flags, and activation.
- `sweet_daemons.py` can inspect or restore the launchd jobs disabled during
  bring-up; it does not implement their hardware dependencies.
- `v18-container.py` seeds the `mobileactivationd` system container.
- `v19-migrate.py` forces a real DataMigration for the offline root.
- `make_sparse.py` converts a raw root image to Android sparse format.

The `ios-root/patch-v17/manifest.tsv` is only a manifest for nine 4 MiB
sparse chunks. The chunk blobs were not preserved; see
`ios-root/patch-v17/README.md`.

## 7. Validation and open work

- The last VM build completed; this proves compilation, not boot.
- `0101` is known-corrupt and skipped. `0110` is missing from the verified
  set. `0115`-`0117` are experimental.
- The next validation step is to sync `xnu/patches/` to the VM, rebuild,
  inspect the build log, and test the resulting kernel on `boot_b` only.
- The GPU/display path is outside the kernel patch train; GPU bring-up is
  tracked in the shim repository's `GPU-BRINGUP.md`.

## 8. References

- `README.md` - build and sync instructions
- `patches/README.md` - patch directory rules
- `vm-build-2026-10-27/README.md` - exact build snapshot
- `NOTES.md` - detailed lab notebook, including the 0101/0110 history
- `XNU-KL7-PORT-PLAN.md` - the original plan; historical
- `../SHIM-HISTORY.md` - shim versions and on-device results
- `../REPOSITORY.md` - repository map and safety rules
