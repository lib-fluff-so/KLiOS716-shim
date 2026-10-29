# DEVLOG-GPU — KL7 Mali-G57 Valhall fragment clear/writeback defect

## Status (2026-10-29)

**Text-console recovery build (2026-10-29).** The GPU test had a full-screen
visual-card call at its entry, then redrew the whole framebuffer after each MMU
attribute mode. That intentionally erased the early text log and could leave no
useful stage text on screen if a later register/job operation hung. The current
test path does not call those overlays: startup and per-address progress remain
in the regular framebuffer console, and the same lines are mirrored to ramoops.

Build: `make OUT=out-gpu-text GPU_TEST=1 GPU_POWER_TEST=1 AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0 WDT_DISABLE_AT_HANDOFF=0`

The seven output-page MMU attribute modes still run, with the pass/fail masks and
the GPU virtual offset printed as text. Each mode keeps a one-second pause so
the last progress lines can be seen. After mode 7, the shim issues PSCI
`SYSTEM_RESET` (warm, preserves ramoops). The matrix-only path skips later
BO/compute/fragment probes. Flash **only `boot_b`**, never `boot_a`; use the
project's Antumbra recovery route if the B-slot boot loop prevents Android from
starting, and retrieve `/sys/fs/pstore/console-ramoops-0` after recovery.

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

## WB output-mapping isolation build — source committed (2026-10-29)

The next test is now isolated in source. It sets AttrIdx=4 / AS_MEMATTR byte
`0x8c` for all six GPU-visible output mappings: `GPU_VA_OUT` (PTE 1),
`GPU_VA_OUT2` (PTE 3), and the four framebuffer pages (PTEs 4–7).
Job, shader, tiler, and descriptor pages retain AttrIdx=5 / `0x4c`.
The seven-mode loop is removed from this build; it runs one 16-address
Write Value grid with the all-output WB mapping, then returns to the existing
warm-reset path. BO/multislot/compute/fragment tests remain unreachable.

Relevant source commits:
- `109bd02562af65f291541e098db78c389b5b34ff` — change output PTEs and grid.
- `6fe000c181109780c621e61ed0aa4578068a94ef` — update isolated-test marker.
- `c4850a881f23a47cd73ba0fe62d36714f8fc2b32` — update reset comment.

Build:

```sh
make OUT=out-gpu-wb-all GPU_TEST=1 GPU_POWER_TEST=1 \
  AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0 WDT_DISABLE_AT_HANDOFF=0
```

Expected diagnostic line after setup:
`GPU: WVALATTR setup idx=4 attr=8c out=... out2=...`
followed by `GPU: WVALATTR FB PTE=... memattr=...` and the final
`GPU: WVALATTR idx=4 attr=8c pass=... fail=...`.

This build was subsequently run on KL7 and returned `pass=ffff fail=0`; see
“All-output write-back test” above. The framebuffer clear remains to be tested.
Install test builds only to `boot_b` through the existing Antumbra route, never
Fastboot and never `boot_a`. After recovery, collect `/sys/fs/pstore/console-ramoops-0`.

## All-output write-back test — completed on KL7 (2026-10-29)

Hardware result from the isolated all-output WB build:

```text
GPU: WVALATTR setup idx=4 attr=8c out=4000c753 out2=40012753
GPU: WVALATTR FB PTE=40007753/40008753/40009753/4000a753 memattr=4c4c4c4c/4c4c4c8c
GPU: WVALATTR idx=4 attr=8c pass=000000000000ffff fail=0000000000000000
```

All 16 Write Value stores completed and read back correctly using AttrIdx=4 /
`0x8c`. The logged PTEs show `GPU_VA_OUT`, `GPU_VA_OUT2`, and all four
framebuffer pages using AttrIdx=4; AS_MEMATTR index 4 is `0x8c`, while other
entries remain `0x4c`.

This confirms the complete mapping setup does not break Write Value output and
eliminates the prior `6969/9696` mask for this Write Value grid. The grid writes
to `GPU_VA_OUT`; it does **not** itself test fragment writeback to the framebuffer.

The next isolated source path has now been added in commit
`e758340417f633eb0750b3e471c4219e5b1f15aa`: after the 16-address grid passes,
it submits exactly one 64x64 clear-only fragment job tagged `wb-all`, scans all
4096 pixels, and reports `GPU: FRAGWB PASS clear=4096` only if every pixel
matches the clear color. It avoids the old size sweep and unique-pixel tests,
and still skips BO, multislot, and compute. This follow-up code has not yet been
built or run on the device.

Build:

```sh
make OUT=out-gpu-frag-wb GPU_TEST=1 GPU_POWER_TEST=1 \\
  AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0 WDT_DISABLE_AT_HANDOFF=0
```

Expected framebuffer result if the WB change fixes the clear-writeback defect:
`GPU: FRAGPROBE wb-all rowmask clear=ffffffffffffffff sent=0000000000000000 mixed=0000000000000000`
and `GPU: FRAGWB PASS clear=4096`.

## Output-page MMU attribute matrix — completed on KL7 (2026-10-29)

The text-console build completed all seven modes on-device. Every Write Value job in the
112-address/mode combinations completed; no job stalled during this matrix.

```text
GPU: WVALATTR mode=1 idx=5 attr=4c pass=0000000000006969 fail=0000000000009696
GPU: WVALATTR first idx=5 attr=4c offset=100 raw=1 got=5a5a5a5a5a5a5a5a want=a57e000000000002
GPU: WVALATTR mode=2 idx=4 attr=4c pass=0000000000006969 fail=0000000000009696
GPU: WVALATTR mode=3 idx=6 attr=4c pass=0000000000006969 fail=0000000000009696
GPU: WVALATTR mode=4 idx=7 attr=4c pass=0000000000006969 fail=0000000000009696
GPU: WVALATTR mode=5 idx=4 attr=88 pass=000000000000ffff fail=0000000000000000
GPU: WVALATTR mode=6 idx=4 attr=8c pass=000000000000ffff fail=0000000000000000
GPU: WVALATTR mode=7 idx=4 attr=8f pass=000000000000ffff fail=0000000000000000
```

Interpretation:

- Modes 1–4 keep the output memory type non-cacheable (`0x4c`) and vary PTE AttrIdx.
  The identical `6969/9696` mask means PTE AttrIdx alone does not fix the write-drop.
- Modes 5–7 all pass all 16 offsets with write-back encodings `0x88`, `0x8c`, and `0x8f`.
- The first non-cacheable failure is offset `0x100`; its sentinel remains unchanged.
- The text-console version emits `WVALATTR ... start` before each job and no longer calls the
  full-screen overlays from `gpu_test()` or the per-mode loop. This preserves useful stage
  text when a later operation stalls. The attached artifact is a ramoops log, not a panel
  photograph, so it records the test output rather than documenting the screen visually.

Build used:

```sh
make OUT=out-gpu-text GPU_TEST=1 GPU_POWER_TEST=1 AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0 WDT_DISABLE_AT_HANDOFF=0
```

The follow-up build above applies `0x8c` to every output/framebuffer mapping
while retaining only one 16-address grid. Do not re-enable BO/compute/fragment
probes until that hardware run has been inspected.

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

## Current build and recovery procedure

```sh
make OUT=out-gpu-text GPU_TEST=1 GPU_POWER_TEST=1 \
  AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=0 WDT_DISABLE_AT_HANDOFF=0
```

Install the image into `boot_b` using the project's existing Antumbra recovery route.
Do not attempt Fastboot recovery and never write `boot_a`. Keep `PSCI_RESET_TIMER=0`;
the hardware watchdog stays enabled as a fallback during the test. After recovery/Android
is available, retrieve `/sys/fs/pstore/console-ramoops-0`.

Safe read-only probe (no GPU register writes):

```sh
make OUT=out-gpu-test GPU_TEST=1 GPU_POWER_TEST=0 AUTO_REBOOT_TEST=0
```

## Safety contract

- GPU WIP stays private and uncommitted unless user requests publication → this branch is the exception (user requested push)
- Flash only `boot_b`, never `boot_a`; keep `boot_b` <= 64 MiB
- Warm-reset tests end with PSCI `SYSTEM_RESET`, not fastboot/power/watchdog
- Commit identity: `lib-fluff-so <furbythefurby123@gmail.com>`
- Device serial: `13016374AF000229`, product `KL7`
