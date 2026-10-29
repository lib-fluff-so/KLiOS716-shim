# Exact VM build snapshot, 2026-10-27

This directory is a byte-for-byte snapshot of the patch/build inputs that
produced the last successful local macOS VM kernel:

```text
1bfe9cd7b07d8fd14ca6e068bf93f776bfafecf5dc0797bfd6c796ac003c86ff
  kernel.development.sweet
```

It is kept for provenance, not as the normal build directory. Normal builds
use `../patches/` plus the pinned blacktop checkout; see `../README.md`.

## What is here

| Path | Description |
| --- | --- |
| `patches/` | exact `~/kbuild/dxb/patches` directory: 105 `.patch` files + 3 `.sh` staging scripts, plus blacktop's `14.4/`, `15.0/`, `26.0/` directories and `README.md` |
| `build.sh` | the modified blacktop `build.sh` used for the build (the wrapper applied the local `ARCHS` edit) |
| `build.log.gz` | full build log, gzip-compressed; 22,942,181 bytes uncompressed, 1,407,070 bytes compressed |
| `out-SHA256SUMS` | kernel hash |
| `MANIFEST.sha256` | SHA-256 for every file in this snapshot |

The 105 patches are:

- 97 sweet/KL7 patches, copied from the VM's `ios-sweet/xnu/patches`;
- 8 blacktop patches from the pinned `blacktop/darwin-xnu-build` commit
  `663906528d1cd5ea8fde7de6f16de73db9511549`:
  `TrustCache.patch`, `iobuffermemd_monterey.patch`, `kas_info.patch`,
  `machine_routines.patch`, `skywalk_sonoma.patch`, `skywalk_ventura.patch`,
  `syntax_checker_sonoma.patch`, `syntax_checker_ventura.patch`.

`../patches/` is the 97 sweet/KL7 subset plus the three scripts; it does not
duplicate blacktop's eight patches. Do not copy the blacktop files over the
repo's default set.

## Build configuration

```text
MACOS_VERSION=13.5
KERNEL_CONFIG=DEVELOPMENT
MACHINE_CONFIG=SWEET
MAKEFLAGS=-k
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
```

The log records the XNU source checkout as
`1b191cb58250d0705d8a51287127505aa4bc0789`.

## Patch application in this build

- `0101-kl7-early-framebuffer-geometry.patch` was **skipped**:
  `error: corrupt patch at line 83`.
- `0102`-`0109` and `0111`-`0117` were applied.
- `0110` is not present in this `patches/` directory.
- `0082` is not present.
- blacktop's own patches reported expected skips because the pinned tree
  already contained their changes or their external headers already existed.

The skip messages are visible near the start of the log. The build completed
and produced the kernel hash above; a skip is not proof that a patch is
unnecessary, only that this run did not apply it.

## How this snapshot was captured

The VM's `~/kbuild/dxb/patches` and `~/kbuild/dxb/build.sh` were archived
with `tar`, copied to the host, and checksummed. The 23 MB plain build log
was gzipped; the original was named
`~/kbuild/build-v0127-battery-g3-no0110.log` on the VM.

## Reproduce or inspect

To inspect the applied series without building:

```sh
zgrep -nE 'Applying patch|Skipping patch|corrupt patch' build.log.gz
```

To restore the exact forensic tree:

1. Check out blacktop at `663906528d1cd5ea8fde7de6f16de73db9511549`.
2. Replace its `patches/` with this snapshot's `patches/`.
3. Run this snapshot's `build.sh` with the configuration above.

That is not the recommended normal workflow. The recommended workflow is
`../README.md`: sync `../patches/` to the VM, run `../build-xnu.sh`, and let
blacktop supply its own eight patches from the pinned SHA.

## Integrity

```sh
cd xnu/vm-build-2026-10-27
sha256sum -c MANIFEST.sha256
```
