# Verified sweet/KL7 patch set

This directory is the exact sweet/KL7 subset used by the last successful
local macOS VM build (`1bfe9cd7...`, 2026-10-27). It is not a complete copy
of `blacktop/darwin-xnu-build`; blacktop's eight patches and their version
directories are fetched with the pinned `dxb` checkout. The full 105-patch
VM snapshot is kept in `../vm-build-2026-10-27/` for provenance.

## Contents

- `0001`-`0081`: base `ios-sweet` train, attributed to dipa-studio.
- `0101`-`0117`: KL7 delta, this project.
- `0019-stage-libpthread.sh`, `0020-stage-bearssl.sh`,
  `0026-stage-hfs.sh`: source-staging scripts.
- `SHA256SUMS`: checksums for every file here.

There is **no `0082`**. `0082-kl7-machine-config.patch` was a hallucinated,
non-building patch and was removed. There is **no `0110`** in this set; the
last verified VM build omitted it. `0101` is present but known-corrupt.

## Apply order

Apply the three `.sh` staging scripts at their numbered positions, then the
`.patch` files in lexical order. `xnu/build-xnu.sh` copies both patterns into
the `dxb` patch directory; blacktop's `build.sh` iterates in sorted order and
logs every apply or skip.

Expected non-fatal skips in the 2026-10-27 build:

```text
0101-kl7-early-framebuffer-geometry.patch
  error: corrupt patch at line 83
TrustCache.patch (blacktop)
  already exists / already applied
other blacktop patches already present in the pinned tree
```

Do not "fix" the blacktop skips by deleting the patches from the snapshot;
they are part of the pinned build tree. Do not add `0082` back. If `0101` is
repaired later, remove the known-defect note here and in `HISTORY.md`, then
rebuild and test the panel output before trusting it.

## Patch status

| Patch | Status |
| --- | --- |
| `0001`-`0081` | base, used by the verified build |
| `0101` | corrupt; skipped by `build.sh` |
| `0102`-`0109` | applied; KL7 platform/display/UFS foundation |
| `0110` | not in this set |
| `0111`-`0114` | applied; sampler, touch, keys, battery |
| `0115`-`0117` | applied; experimental UFS stats/bench/HS-G3 |

The base patches are not a claim that every sweet driver is portable to
MT6789. In particular, UFS, touch, keys, battery and display required the
KL7-specific patches above; the `sweet` UFS driver is reused only after the
KL7 controller work in `0107`, `0108`, `0115`-`0117`.

## Verification

```sh
cd xnu/patches
sha256sum -c SHA256SUMS
```

The checksums cover the scripts and patches as preserved. They do not prove
that a future `dxb` checkout applies them cleanly; always inspect the VM
build log and confirm the intended patch lines. `0101` must appear as a
skip, not as a silent success.

## Exact VM snapshot

`../vm-build-2026-10-27/patches/` contains:

- the same 97 sweet/KL7 patches and 3 scripts;
- blacktop's eight patches;
- blacktop's `14.4/`, `15.0/`, `26.0/` directories and `README.md`;
- the modified `build.sh` used for the build;
- the full build log (gzipped) and a SHA-256 manifest.

Use the snapshot only for forensic reproduction or comparison. Normal builds
should use this directory plus the pinned blacktop SHA.
