# DEVLOG-GPU — KL7 Mali-G57 Valhall GPU bring-up

## Current status (2026-10-29, branch `gpu-going-forward`)

**All-output write-back and the isolated fragment clear both pass on the KL7.**
The latest device log ends with:

```text
GPU: WVALATTR idx=4 attr=8c pass=000000000000ffff fail=0000000000000000
GPU: FRAGWB begin slot=0 size=64x64 attr=8c
GPU: JS0 submit head=40086000 af=5 cfg=81300
GPU: FRAGPROBE wb-all cacheclean rc=0 raw=20600
GPU: FRAGPROBE wb-all w=64 h=64 rc=0 raw=1 clear=4096 sent=0 other=0
GPU: FRAGPROBE wb-all rowmask clear=ffffffffffffffff sent=0000000000000000 mixed=0000000000000000
GPU: FRAGWB pixels clear=4096 sent=0 other=0
GPU: FRAGWB PASS clear=4096
```

Every one of the 16 Write Value addresses passed under output AttrIdx=4 /
`0x8c`. The fragment clear then wrote the expected color to all 4096 pixels
in a 64x64 linear test buffer. This is a real fragment job and write-back
result, but it is still the clear-only path: the job does not execute a vertex
shader or draw geometry.

The earlier rollback entry below is historical. The fragment test is enabled
again in `gpu-going-forward`, isolated after the known-good write-back grid.
A read-only job-header dump now runs after the fragment job wait. It logs
the GPU-written exception status, first-incomplete-task value, fault pointer,
job type/index, and next-job pointer.

The first type-11 job used a Vertex-stage `NOP.end`. The latest device log
from that build shows the JS slot was selected and the job ran far enough to
write its header, but it failed:

```text
GPU: MALLOCV slot=1 features=1c9e
GPU: JOBSTATUS MALLOCV-NOP status=120258 task=0 fault=0000000000000000
GPU: JOBSTATUS MALLOCV-NOP type=11 index=1 next=0000000000000000
GPU: MALLOCV rc=... raw=20000 js=58 state=0
GPU: MALLOCV fault mraw=0 fstat=0 faddr=0000000000000000
```

Decode of `0x00120258` per Mesa's Job Manager status layout:
- source ID: `0x12`
- access: `READ`
- exception: `0x58 = DATA_INVALID_FAULT`

This is a job exception, not an AS MMU translation fault: the log has
`mraw=0`, `fstat=0` and a zero AS fault address. The NOP shader did not emit
the position output required by the position-processing path, so that NOP test
was not a valid neutral test. The source has now replaced it with a minimal
position-store shader. Every invocation writes clip-space `(0,0,0,1)`; all
three vertices coincide, so a visible triangle is not expected from this smoke
test. Machine code and descriptor words are logged before submission.

**The replacement position-store test has not yet been built or run on the
phone.** The old log above does not describe the replacement shader.

## Immediate next milestone

Build and run the new `POS-SMOKE` test. Check its decoded job status:
1. If the job completes, check whether the tiler heap changes and the job
   reports DONE.
2. If it faults, inspect the printed exception/access/source ID and the exact
   six shader instruction words plus shader descriptor.
3. Once position output completes, make vertex positions depend on vertex ID
   to form a triangle, then test tiler allocation, and only then add the
   fragment-shader colour path.

This follows the v9 layout rather than the older IDVS-helper descriptor used
by early Mali examples.

The external [Noysz/panvk-g99-jm porting log](https://github.com/Noysz/panvk-g99-jm/blob/main/docs/porting-log.md)
is a highly relevant G57/G99 reference. It records a case where a v9
`MALLOC_VERTEX` and fragment job both report completion, yet the framebuffer
contains only the clear color. The remaining hypotheses there include the
position shader's actual machine code, selecting the triangle rather than
point position-shader descriptor, vertex-array packet fields, and tiler-heap
allocation. Do not count a job-completion bit alone as proof that geometry was
generated.

## Previous diagnostic history

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


## Position smoke packet correction — v9 reference comparison (2026-10-30)

The no-fragment MALLOC_VERTEX_JOB packet was compared against PanVK's v9
implementation at
https://github.com/JimVulkan/mali-panvk/blob/d896b0a9c10b92f0f9d53ef3373006b79ed709e8/src/panfrost/vulkan/jm/panvk_v9_cmd_draw.c
and its src/panfrost/genxml/v9.xml definitions.

Corrections committed to gpu-going-forward:

- Allocation: vertex_packet_stride=16, vertex_attribute_stride=0. The 16-byte packet stride is the internal position-packet layout for a draw without a secondary/varying shader. It is not the application vertex-buffer stride. The prior experiment that changed it to zero confused two distinct fields.
- No-FS DCD flags: word 32 = 0x00810183, word 33 = 0x0000ffff. This selects forward-kill both ways, force-early pixel/Z-S update, overdraw alpha, CCW front face, scissor-to-bounding-box, full sample mask, and zero fragment render-target mask.
- Depth/stencil descriptor word 4 = 0xe4c00000: depth cull/clip enabled, depth clamp [0,1], fixed-function depth source, ALWAYS comparison, no depth write.
- Blend descriptor: Enable=false and Internal Blend Mode=Off.
- Local-storage descriptor: WLS Instances must contain MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM; its encoded low field is 0x1f. The smoke setup now explicitly writes this sentinel and logs it.

The source now logs the local-storage sentinel, depth/stencil word 4 and blend word 2 before submitting the job. The shader still emits the same position for all three invocations. No vertex buffer has been added.

This is a reference-based packet correction, not a hardware result. The updated source has not yet been built or run on the KL7. The no-input position shader now supplies a four-entry v9 RESOURCE table. RESOURCE[0] points at a 17-entry (544-byte) VS driver set containing 16 NULL_DESCRIPTOR attribute slots and the mandatory dummy sampler; RESOURCE[1..3] are empty. The dummy sampler is packed with the v9 default wrap/filter state and clamp-integer-array disabled. The resource table and driver set are mapped at GPU VAs 0x400c9000 and 0x400ca000. This is a reference-derived descriptor layout, not yet a hardware result.
