# `patch-v17` sparse manifest

`manifest.tsv` describes nine 4 MiB chunks of an iOS 17 root experiment:

| Column | Meaning |
| --- | --- |
| 1 | chunk index (4 MiB units) |
| 2 | chunk size in bytes (`4194304`) |
| 3 | SHA-256 of the chunk |
| 4 | expected chunk filename |

The listed chunks are indices `0`, `1`, `2`, `179`, `182`, `201`, `202`,
`3887`, and `3888`. They cover the first 12 MiB and several isolated blocks;
this is a sparse overlay, not a full image.

## Important: the blobs are missing

The `chunk_*.bin` files referenced by the manifest were **not preserved** in
this repository or in the source archives available on the host at
consolidation time. Only `manifest.tsv` survives. Do not claim that this
directory contains a usable v17 patch, and do not invent chunk data from the
manifest.

To use the experiment you would need to recover the original chunks (or the
source image and the tool that produced them), verify each 4 MiB chunk's
SHA-256, and then apply them at the listed indices. The manifest gives only
the index, size and hash; it does not include a generator, a target path
inside the image, or a filesystem-aware patch description.

## Regeneration notes

If the source image is recovered:

1. Split it into 4 MiB chunks at the indices above.
2. Verify the exact SHA-256 from `manifest.tsv`.
3. Compare the non-zero chunks against the source; the manifest does not
   identify which file inside the image each chunk changes.
4. Keep the chunks outside git unless they are needed for a reproducible
   test; each chunk is 4 MiB and the full set would add 36 MiB.

## Status

This directory is provenance only. It is not part of the verified XNU build,
not part of the shim, and not a tested root patch.

## See also

- `../README.md` - offline root preparation
- `../../xnu/NOTES.md` - iOS root and userland history
- `../../SHIM-HISTORY.md` - on-device chronology
