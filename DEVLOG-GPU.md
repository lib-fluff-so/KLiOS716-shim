# DEVLOG-GPU — KL7 Mali-G57 Valhall fragment clear/writeback defect

## Status (2026-10-29)

**Visual diagnostic is now visible.** The `console_visual_result()` card draws at
`FB_BASE + 0` (offset 0, full screen 1080×2436, stride 4352), matching the working
`main` branch console. Previously it drew page B at `FB_ALT_OFFSET = 1230 * stride`
(0xFE519E00) which is NOT the scanned page — nothing was visible.

Build: `make OUT=out-gpu-memattr GPU_TEST=1 GPU_POWER_TEST=1 AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0`

Each of the seven output-page MMU attribute modes is shown full-screen for one second with its pass/fail
mask. After mode 7, the shim immediately issues PSCI `SYSTEM_RESET` (warm, keeps ramoops).
This matrix-only path skips later BO/compute/fragment probes so they do not overwrite the
final result card. Flash **only `boot_b`**, never `boot_a`; then `fastboot reboot`
to return to Android and read `/sys/fs/pstore/console-ramoops-0`.

## What works (proven)

- SCPSYS/MFG power-on, 26 MHz reference clock, bus-protect release, GPU reset
- AS0 AArch64-4K 4-level 48-bit MMU page tables
- BO mapping/remap/reuse, multi-slot concurrent jobs
- Deliberate MMU fault detected correctly
- Compute job writes `0123456789abcdef` through GPU MMU (slot 2, JS2)
- Write Value job works
- Panel path works: 8 color bars draw correctly (white yellow cyan green pink red blue + near-black)
- Pixel format is RGBA8 as expected (no R/B swap)

## The defect (unresolved)

Fragment clear-only job writes only HALF the rows of the 64×64 render target.

Row mask for 64×64: `clear=6969696969696969`, `sent=9696969696969696`.
Clear rows (mod 8): 0, 3, 5, 6. Sent rows: 1, 2, 4, 7.

**Observed address-parity signature:** GPU writes a row iff `popcount(addr[8] ^ addr[9] ^ addr[10]) == 0`,
i.e. even parity of address bits 8,9,10. Since row stride = 256 = 2^8, row `y` starts at
byte address `y<<8`, so the rule is `popcount(y & 7) == 0` → write, odd → skip.

This holds for ALL observed sizes:
- 16×16 stride 64: clear=f00f → rows 0-3, 12-15 (popcount of y>>2 even? verified via script)
- 16×32 stride 64: clear=0ff0f00f
- 32×16 stride 128: clear=3cc3
- 32×32 stride 128: clear=3cc33cc3
- 64×64 stride 256: clear=6969696969696969

A Python script (`/tmp/opencode/analyze_mask.py`) confirms: `XOR(addr[8],addr[9],addr[10])==0`
predicts clear/sent exactly for all five sizes.

`clear` and `sent` are exact bitwise NOTs (`clear ^ sent == 0xFFFF...FFFF`). Every row is
fully written or fully not written — no mixed pixels, no permuted rows. This is a
writeback-drop, not a scrambling.

## What was tried (no effect on the 0x69 pattern)

| cfg | FBD change | Result |
|-----|-----------|--------|
| alloc2 | colour-buffer allocation 2 KiB | same 6969 |
| alloc4 | colour-buffer allocation 4 KiB | same 6969 |
| nodither | dithering off (d[33] bit 15) | same 6969 |
| noclean | clean-pixel write off (d[33] bit 31) | clear=0, sent=all (no writeback at all) |
| emptyw | empty-tile write on | same 6969 |
| tile7 | effective tile size 128 px | same 6969 |
| tile9 | effective tile size 512 px | clear=512/4096 (different pattern) |
| tiledwb | block format = Tiled U-Interleaved | clear=576/4096, partial |
| alloff | dither off + clean off + empty-tile on | clear=0, sent=all |
| msaa4 | sample count = 4x (d[11] bits 0-2 = 2) | same `6969/9696` row mask; 2048 clear / 2048 sentinel — MSAA4 does not fix it |

Key observation: `noclean` (clean-pixel write enable = 0) produces ZERO writeback. So the
clean-pixel/fast-clear path IS active and IS writing — but it drops half the rows. MSAA4
does not change that mask. The subsequent Write Value grid reproduces the same parity mask
on ordinary GPU jobs, so the defect is not isolated to the clear-only fragment/FBD path.
The next discriminator is the job-slot flush matrix.

## Write Value address grid result (2026-10-29)

The first 16-address test ran on KL7 and reproduced the fragment row mask using ordinary
Write Value jobs:

```text
GPU: WVALGRID pass=0000000000006969 fail=0000000000009696
GPU: WVALGRID first offset=100 raw=1 got=5a5a5a5a5a5a5a5a want=a57e000000000002
```

Every job completed with `raw=1`, yet stores at the failing offsets left the sentinel
untouched. This proves the parity signature is not exclusive to the fragment clear-only
path or its FBD. It points to a shared layer below the FBD, still to be identified.

## Output-page MMU attribute matrix (source updated; hardware result pending)

The prior JS_CONFIG flush-mode matrix was reported to show the same FAIL result in every
mode. The targeted test now varies only the output page's PTE AttrIdx and matching AS_MEMATTR
entry. Each of seven modes draws its pass/fail mask full-screen for one second; after mode 7,
main issues warm PSCI `SYSTEM_RESET`. Later BO/compute/fragment probes are skipped.

Modes 1-4 keep the output memory type non-cacheable (`0x4c`) while selecting different PTE
AttrIdx values. Modes 5-7 apply write-back attributes (`0x88`, `0x8c`, `0x8f`) only to the
output page. The job descriptor, job page, and other mapped buffers remain on the original
non-cacheable AttrIdx=5. Each run logs the selected AttrIdx, attribute byte, PTE value, full
AS_MEMATTR register pair, and the 16-address pass/fail masks.

Build:

```sh
make OUT=out-gpu-memattr GPU_TEST=1 GPU_POWER_TEST=1 AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0
```

Flash **only `boot_b`**, never `boot_a`. After Android returns, read
`/sys/fs/pstore/console-ramoops-0`. Hardware results are pending until this image is built
and run on KL7.


## FBD layout (from Mesa v9.xml, matches our code)

Word 11 (Framebuffer Parameters):
- bits 0-2: Sample Count (default 0 → 1 sample)
- bits 3-5: Sample Pattern (0)
- bits 6-8: Tie-Break Rule = 2 (MINUS_180_IN_0_OUT)
- bits 9-12: Effective Tile Size = 8 → 2^8 = 256 px = 16×16
- bit 15: Point sprite origin max Y = 1
- bit 24: Color Buffer Allocation = 1 (1 KiB)
- Render Target Count (bits 19-22) = 0 → minus(1) → 1 RT

Word 33 (Render Target 0):
- bit 0: Write Enable = 1
- bits 3-7: Writeback Format = 19 (R8G8B8A8)
- bits 8-11: Writeback Block Format = 2 (Linear)
- bit 15: Dithering Enable = 1
- bits 16-27: Swizzle = 0x688
- bit 31: Clean Pixel Write Enable = 1

Word 42: Row Stride = width*4
Word 40-41: Base address

## Tiler context (build_tiler_context)

- d[2] = 0xff (hierarchy mask, single-sample)
- d[3] = (w-1) \| ((h-1)<<16)
- d[6]/d[7] = heap descriptor VA

## Fragment job (build_fragment_job)

- h[4] = (9<<1) \| (1<<16) : Type=Fragment, Index=1
- h[8] = 0 : Bound Min X/Y = 0
- h[9] = tx \| (ty<<16) : Bound Max in tiles
- h[10]/h[11] = Framebuffer VA \| 1 (low/high 32 bits)

**No shader is bound** — the job uses the RT clear-color path (fast clear), not a fragment
shader. This may be relevant: a normal fragment shader render might take a different writeback
path that does not have the parity bug.

## Files

- `src/console.c` — panel diagnostics and one-second per-mode result card
- `src/console.h` — console visual-result declarations
- `src/gpu_test.c` — flush-mode matrix, fragment probes, row-mask diagnostics
- `src/gpu_test.h` — GPU test header
- `src/main.c` — GPU test integration and immediate PSCI reset after the matrix
- `Makefile` — GPU_TEST and GPU_POWER_TEST build switches
- `GPU-BRINGUP.md` — proven GPU stages, register map, safety contract
- `GPU-ACCELERATION-PLAN.md` — GPU stack roadmap
- `RECOVERY-NOTES.txt` — KL7 slot retry, PSCI reset, fastboot recovery rules

## Build/flash commands

```sh
# Build seven-mode flush matrix (one second per result, then warm reset)
make OUT=out-gpu-flush-matrix GPU_TEST=1 GPU_POWER_TEST=1 AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0

# Flash only boot_b (device in fastboot/LK)
fastboot flash boot_b out-gpu-flush-matrix/shim_b.img
fastboot --set-active=b
fastboot reboot

# After Android returns, pull the log
adb -s 13016374AF000229 shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > ramoops-shim.txt

# Safe read-only probe (no GPU register writes)
make OUT=out-gpu-test GPU_TEST=1 GPU_POWER_TEST=0 AUTO_REBOOT_TEST=0
```

## Safety contract

- GPU WIP stays private and uncommitted unless user requests publication → this branch is the exception (user requested push)
- Flash only `boot_b`, never `boot_a`; keep `boot_b` <= 64 MiB
- Warm-reset tests end with PSCI `SYSTEM_RESET`, not fastboot/power/watchdog
- Commit identity: `lib-fluff-so <furbythefurby123@gmail.com>`
- Device serial: `13016374AF000229`, product `KL7`
