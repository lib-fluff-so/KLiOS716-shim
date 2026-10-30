# Mali-G57 / Valhall v9 triangle bring-up notes

Status: 2026-10-29. This is a research note for `gpu-going-forward`; it does
not claim that a triangle has been drawn by the KL7 shim.

## Target architecture

KL7 reports Mali-G57 MC2, GPU ID `0x90930010`, Valhall generation v9. The
GPU uses the older Job Manager interface with Valhall shader programs and
descriptors. For v9, the normal draw path uses one `MALLOC_VERTEX_JOB`
(job type 11) for position processing, varying setup, and tiler work, followed
by a fragment job (type 9). Do not substitute an older `IDVS Helper Job`
layout for the v9 `MALLOC_VERTEX_JOB` aggregate.

Useful references:

- [HelioDarwin (Infinix Note 12 2023 / X676C, MT6789): Mali JM user-client patch](https://github.com/squeeeezy/HelioDarwin/blob/x676c/xnu/patches/0189-x676c-malimetal-jm-gpu-userclient.patch)
- [Noysz/panvk-g99-jm: v9 draw porting log](https://github.com/Noysz/panvk-g99-jm/blob/main/docs/porting-log.md)
- [Noysz/panvk-g99-jm: triangle draw test](https://github.com/Noysz/panvk-g99-jm/blob/main/tests/graphics/triangle_draw_test_v2.c)
- [JimVulkan/mali-panvk: v9 Job Manager draw implementation](https://github.com/JimVulkan/mali-panvk/blob/main/src/panfrost/vulkan/jm/panvk_v9_cmd_draw.c)
- [Mesa JM job-chain dependencies](https://github.com/vortexgpgpu/mesa/blob/vortex_3.x/src/panfrost/lib/pan_jc.h)

These are reference implementations and experiments, not a ready-to-copy
working draw packet: the G99 reference log explicitly records a case where a
MALLOC_VERTEX job and a fragment job complete but the image contains only the
clear color.

## Descriptor checklist for a future shim test

The G99 porting log decodes a v9 `MALLOC_VERTEX_JOB` as 384 bytes, 64-byte
aligned. Its relevant byte offsets are:

| Offset | Contents |
|---:|---|
| 0 | Job header |
| 32 | Primitive descriptor |
| 48 | Instance count and allocation fields |
| 56 | Tiler context pointer |
| 104 | Scissor |
| 112 | Primitive size |
| 120 | Index descriptor |
| 128 | Draw state, including vertex array and shader environments |
| 192 | Fragment shader environment |
| 256 | Position shader environment |
| 320 | Varying shader environment |

The exact packed fields must be generated or checked against the matching v9
genxml definitions, not assembled by guessing offsets. The currently available
shim uses raw word arrays, so a first implementation should dump all 384 bytes
after construction and before submission.

HelioDarwin targets the Infinix X676C, also MT6789 + Mali-G57 r0p1, so
its power/register/descriptor notes are unusually close to KL7. Its patch
explicitly documents the expected JM layout as slot 0 for fragment and slot 1
for vertex/tiler/compute, and defines the v9 job type as 11. The patch itself
states hardware execution remains unvalidated, so treat it as a register and
layout reference, not proof of a working renderer.

The current branch now adds a deliberately limited `MALLOC_VERTEX_JOB`
baseline after the known-good fragment clear. It uses job type 11, JS feature
bit 12, a 384-byte descriptor, and a Vertex-stage (value 3) `NOP.end`
shader. It is not supposed to draw anything. Its purpose is to establish
whether the JM accepts the vertex descriptor and to report the job header plus
the tiler-heap occupancy. This probe has not yet been built or run on KL7.

The existing shim already has:
- AS0 with 4-level, 48-bit GPU page tables;
- a 256 KiB tiler heap buffer and its heap descriptor/context;
- a known-good 64x64 linear RGBA8 framebuffer mapping with write-back
  AttrIdx=4 (`0x8c`);
- fragment clear-only job and CPU readback;
- job submit/wait helpers and read-only MMU/GPU fault diagnostics.

Before a `MALLOC_VERTEX_JOB` test, verify that every referenced GPU VA is
mapped: position code and shader descriptor, vertex/attribute buffers,
thread-local storage, tiler context, tiler heap descriptor, tiler heap storage,
framebuffer descriptor, and render target.

## Most useful failure discriminators

The G99 experiment narrowed the “clear succeeds, triangle absent” case to
several possibilities after ruling out a few descriptor/header mistakes:

1. **Position shader output:** dump and verify that the actual machine code is
   the intended triangle position shader, and that the job points at the
   triangle-position variant rather than a point-position variant.
2. **Vertex-array packet metadata:** determine whether the packet pointer and
   stride fields are written by hardware for this exact mode or must be seeded
   by software. The G99 log marks this as unresolved in its test.
3. **Tiler heap activity:** inspect the heap after job completion. A completed
   job with no tiler allocation/write is not evidence that primitives entered
   the tiler.
4. **Post-completion job header:** after waiting, record exception status,
   first-incomplete-task, fault pointer, type, index, and next-job. Do not
   infer the exception meaning from the interrupt bit alone.
5. **Framebuffer readback:** initialise the buffer with a per-pixel sentinel,
   clear/render, then report clear/sentinel/other counts and a compact row/tile
   mask.

The current `gpu_job_status_dump()` decodes the GPU-written exception
status. The NOP vertex-shader attempt on the KL7 returned
`status=0x00120258`: source ID `0x12`, access `READ`, exception
`DATA_INVALID_FAULT` (`0x58`). The same log had `mraw=0`, `fstat=0` and
`faddr=0`, so this was not an AS MMU translation fault.

The original Vertex-stage `NOP.end` did not produce the required position
output and was a bad baseline. The branch has since replaced it with a
six-instruction position-store smoke shader:
four register initialisations, `LEA_ATTR` into `r4:r5`, and
`STORE.i128.pos` of `(0,0,0,1)`. Its code words and shader descriptor are
logged before job submission. All three invocations are expected to write the
same position, so a visible triangle is not expected.

This replacement has **not yet been built or run**. The next measurement is
whether the position-store job returns DONE and what happens to the tiler heap.
Do not treat the previous NOP fault as the result of this newer shader.

## Current measured milestone

The latest KL7 test supplied on 2026-10-29 returned:

```text
GPU: WVALATTR idx=4 attr=8c pass=000000000000ffff fail=0000000000000000
GPU: FRAGWB pixels clear=4096 sent=0 other=0
GPU: FRAGWB PASS clear=4096
```

That validates write-back mapping and clear-only fragment writeback for this
test configuration. It does not yet validate vertex shading, tiler-generated
primitive data, or a shader-coloured triangle.


## No-fragment position smoke packet — reference-matched fields (2026-10-30)

Compared the shim's MALLOC_VERTEX_JOB against PanVK's v9 draw emitter and v9 genxml from pinned commit d896b0a9c10b92f0f9d53ef3373006b79ed709e8.

For the no-secondary/no-fragment path, PanVK sets allocation to packet stride 16 and attribute stride 0. This is an internal position-packet stride, not the API vertex-buffer stride. Setting it to zero was an incorrect interpretation.

The packet now uses no-FS DCD flags 0x00810183 / 0x0000ffff, a no-depth-test descriptor with word 4 0xe4c00000, and a blend descriptor with Enable=false and Internal Blend Mode=Off. The LOCAL_STORAGE descriptor is initialized with the no-workgroup-memory WLS sentinel (word1 & 0x1f == 0x1f).

The six-instruction shader and its same-position output are unchanged. No vertex-input descriptors or vertex buffer have been introduced. The shader environment Resources pointer is still zero and remains an explicit verification point if the job faults.

These edits match the reference packet fields; they have not yet been built or validated on KL7.
