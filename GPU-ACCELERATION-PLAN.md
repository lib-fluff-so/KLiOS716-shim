# KL7 GPU acceleration plan (Mali-G57 / MT6789, iOS 16)

Status: 2026-10-28.  The shim has just executed a real Valhall compute shader
and written `0123456789abcdef` through AS0.  That proves the hardware path,
but it is not yet GPU acceleration for iOS userland.  This file lists the
remaining layers in dependency order.

## What is already proven

- SCPSYS power: MFG0..3 on, 26 MHz reference, MFG gate.
- GPU ID `0x90930010`, product `0x9093` (TNAX, Mali-G57).
- Bus protection release, external CG/QChannels, GPU reset.
- Internal L2/shader/tiler power.
- AS0 with AArch64-4K, 4-level, 48-bit page tables, non-cacheable attributes.
- Static and dynamic BO mapping, unmap/remap, first-fit reuse.
- Job submission on slots 0/1/2, Write Value jobs, multi-slot concurrency.
- Deliberate MMU fault at an unmapped VA, with `AS_FAULTADDRESS` and
  `AS_FAULTSTATUS` diagnostics.
- Compute job on slot 2:
  - compute job header/payload/shader environment;
  - 32-byte Valhall `Shader Program` descriptor;
  - instruction fetch, register writes, 64-bit store, job completion.

The exact compute result and descriptor layout are in `GPU-BRINGUP.md`
(section "Compute job and Valhall shader descriptor") and
`RECOVERY-NOTES.txt`.

## Layer 1: finish the shim / 3D pipeline

This is the current work.  It is still bare-metal `boot_b` code, not an iOS
driver, but it is the hardware contract the driver will rely on.

1. Compute validation beyond a single invocation:
   - workgroup/thread IDs, multiple workgroups, barriers, TLS, FAU.
   - a 2D fill into a framebuffer-sized BO.
2. Tiler job:
   - `MALLOC_VERTEX_JOB` on Valhall (384 bytes), position/varying shader
     environments, tiler heap/context, primitive descriptor.
   - polygon list in GPU memory.
3. Fragment job:
   - framebuffer descriptor, render target, tile bounds.
   - fragment shader environment and shader-program descriptor.
   - clear and a full-screen triangle.
4. Vertex/index path:
   - attribute buffers, index buffer, viewport/scissor.
   - one triangle with a known colour.
5. Synchronization and robustness:
   - fences, job interrupts, timeout/recovery, cache clean/invalidate.
   - texture/sampler descriptors, tiling/AFBC, multiple render targets.

Success criterion for this layer: the shim renders a known image into a
linear BGRA buffer that the CPU can read back, then the same path runs from
an XNU user client.

## Layer 2: XNU kernel driver / user client for Mali-G57

The current `kernel.development.sweet` has `IOMobileFramebuffer` and
`IOSurfaceRootUserClient`, but no Mali GPU provider (`SweetMali`,
`IOAccelerator`, or equivalent).  The shim's code cannot accelerate iOS
userland because it runs as a bootloader payload and owns the GPU directly.

Needed IOKit driver (`SweetMali` or similar):

- GPU power on/off: MT6366 VGPU/VSRAM_GPU, SCPSYS MFG0..3, clocks, bus
  protection, internal L2/shader/tiler domains.
- BO manager: allocate/map/unmap, GPU VA, per-process address spaces,
  cache maintenance, IOSurface import/export.
- Command submission: job ring or direct job chain, per-slot queues,
  dependencies, fences, interrupts, timeout/reset.
- Contexts: per-process GPU context, MMU/AS lifetime, teardown.
- Integration with XNU tasking, IOKit memory descriptors, power management,
  and the existing display/IOSurface stack.

This is the largest and riskiest piece.  It is a port/replacement of the
Linux kbase/panfrost kernel interface, not a configuration switch.

## Layer 3: Mesa panfrost/Valhall userspace + libdrm stand-in

- Build Mesa panfrost for arm64e iOS with the Valhall compiler and GLES
  frontend (OSMesa is the smallest frontend; a custom surfaceless frontend
  is also possible).
- Provide a `libdrm`-like stand-in whose BO/map/submit/wait calls go to the
  `SweetMali` user client instead of `/dev/dri`.
- Decide the kernel ABI: upstream panfrost UAPI vs a kbase-shaped ABI.
  The stand-in must match whatever the kernel driver exposes.
- Keep a softpipe fallback so a missing/broken GPU driver still boots.

Reference: Bergamota's `gpu/freedreno-sweet/sweet_drm.c` and
`gpu/eagl/mesa/bergamota-mesa-25.0.7.patch` show the shape of the stand-in
and the OSMesa hardware-screen hook, but the Mesa driver is freedreno/a6xx.
KL7 needs panfrost/Valhall instead.

## Layer 4: EAGL/OpenGLES shim and CoreAnimation integration

Bergamota's `gpu/eagl` is the closest reference:

- `EAGLContext`/`EAGLSharegroup`, drawable/IOSurface glue.
- 457 `gl*` trampolines resolved from Mesa's glapi.
- macro-context block for QuartzCore's `EAGLMacroContext`.
- `attachImage:toCoreSurface:invertedRender:` and
  `texImageIOSurface:...` semantics, including orientation rules.

For KL7, the shim is driver-agnostic if OSMesa's screen is panfrost.  The
hard part is the render-server integration:

- On iOS 18 Bergamota found that the render server does not create an
  `EAGLContext`; it uses Metal if available, otherwise the software
  renderer.  They hooked the renderer-selection vtable (slot 49) to build
  `CA::OGL::GLESContext` over the shim.
- iOS 16 may differ.  hackios's `inject.c` is the iOS 16 reference for
  forcing the GLES/OGL renderer.  The iOS 16 QuartzCore renderer selection
  must be reverse-engineered and hooked, or the framework replaced.
- Then `backboardd`/SpringBoard draws through `CA::OGL::GLESContext` ->
  EAGL shim -> Mesa panfrost -> `SweetMali` -> Mali-G57, with the frame
  delivered through the existing IOMobileFramebuffer/IOSurface swap chain.

## Layer 5: validation and performance

- Fences, cache coherency, tiling/AFBC, texture formats, shader variants.
- Display swap chain, orientation, alpha, wide colour.
- Frame time, power/thermal, stability, recovery from GPU hangs.
- Compare against the software renderer and against Bergamota's numbers
  (their freedreno full-screen quad was ~18 ms/frame vs ~532 ms softpipe;
  Mali-G57 should be in the same order of magnitude once the pipeline is
  correct).

## Dependency order

```text
shim 3D proof
    -> XNU SweetMali user client (BO/MMU/submit/fences/power)
        -> Mesa panfrost + libdrm stand-in
            -> EAGL/OpenGLES shim
                -> CoreAnimation renderer hook
                    -> GPU-composited iOS 16 UI
```

The shim and the XNU driver can share the same register/power/MMU code, but
the driver must not assume the shim's identity-mapped, single-AS world.

## Realistic estimate

- Compute: done.
- First GPU-rendered triangle/clear from the shim: days to a few weeks of
  test cycles, depending on tiler/fragment descriptor surprises.
- Mesa panfrost on the shim without XNU: weeks, assuming the panfrost
  winsys/ABI can be adapted cleanly.
- Full GPU-composited SpringBoard: months.  The long poles are the XNU
  driver and the CoreAnimation renderer hook, not the shader compiler.

This is not "enable the GPU".  It is building the Mali equivalent of
Bergamota's Adreno stack.  The EAGL/CA concepts transfer; the Mesa driver
and kernel ABI do not.
