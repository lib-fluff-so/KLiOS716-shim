# XNU patch port plan: ios-sweet → KL7 / MT6789

> **Historical document.** The abandoned `0082` machine-config patch is not
> part of this repository; the real KL7 delta is `0101`-`0117`, and the
> verified VM build uses `MACHINE_CONFIG=SWEET`. See `xnu/README.md` and
> `xnu/HISTORY.md`.

This is a triage map of the patch series in the supplied `ios-sweet` clone. It is a static review aid, **not** proof that the patches apply to a specific XNU checkout or that a kernel built from them boots. The source tree/version and build workflow must be fixed before applying anything.

## Phase 1 — make one core image build

Start with build-system / open-source XNU compatibility work, then create a new `KL7` board config instead of renaming Qualcomm assumptions mechanically:

- `0001`, `0002`: compiler/kernel-source compatibility.
- `0004`, `0005`, `0006`, `0007`, `0008`, `0009`: machine config, non-PAC/build setup, open-source pmap/master config, private-header stubs, missing open-source symbols, and export cleanup. These are **not all SoC-neutral**. In particular, review the CPU feature flags, cache-line size, interrupt-entry path and GIC security-group handling in `0004` against MT6789.
- Use the correct XNU source revision and build host/CI from the sweet workflow. A successful compile of the shim says nothing about whether this kernel source revision is buildable.

Deliverable for this phase: a reproducible XNU build with a KL7-specific kernel configuration and no device drivers assumed to work yet.

## Phase 2 — first visible XNU instruction

Prioritize these before storage, touch, USB, or multi-core bring-up:

- `0010`: early framebuffer/debug output. Rework the framebuffer base, stride, height and pixel format for KL7 (`0xfdfff000`, stride `4352`, `1080×2436`), and verify the actual buffer format before relying on visible output.
- `0011`: physmap alignment and padding. Re-check against the KL7 16 KiB page size and the new `physBase`/`topOfKernelData` layout.
- `0012`: keep the debug console reachable after TTBR0 changes. Trace the exact TTBR and mapping assumptions rather than applying it as a blind hunk.
- `0016`: device-tree PlatformExpert. Replace the `xiaomi,sweet` match and product strings; validate the ADT properties and registry enumeration against the generated KL7 tree.
- `0017`: architected virtual timer / interrupt path. KL7's DT-derived virtual timer is PPI 11 / INTID 27, but the `GICD_CTLR`, security state, IRQ-vs-FIQ and acknowledge/EOI logic must be checked on MT6789 before using the SWEET implementation.
- `0018`: boot-CPU `IOCPU` driver, as a single-core milestone. Update logical/physical CPU identity and the platform-expert matching.

The generated ADT currently carries `/chosen`, eight CPU nodes, GIC, timer information, framebuffer, UFS register range and `/pram`; without a real D22AP ADT, `/product` is deliberately absent. Therefore this milestone should target **early kernel logging**, not full iOS boot.

## Phase 3 — memory and SMP, after single-core boot

- `0065` and `0078`: PSCI/SMP startup and 8-core topology. These require a KL7-specific mapping from real `MPIDR_EL1` values to ADT CPU nodes. Do not assume Qualcomm's `context_id` path or CPU-number encoding matches the MediaTek firmware handoff. Keep secondaries disabled until the boot CPU, panic path and memory map are stable.
- Revisit XNU pmap, cache attributes, physical-memory discovery, reserved/no-map ranges and the 1 MiB panic-log carve-out as one consistent memory contract. The shim's MMU-off handoff is not a substitute for XNU's own pmap setup.

## Phase 4 — storage and userspace

The sweet UFS code is a **driver port, not a generic UFS feature flag**:

- `0028`, `0030`, `0034`, `0069` cover the sweet UFS device/root path, writes, DMA-above-4-GiB handling and performance. Replace the controller-specific register programming, queue/descriptor assumptions, DMA constraints and any Qualcomm-only calls with an MT6789 implementation. The presence of `mediatek,mt8183-ufshci` in Linux DTB only identifies a compatible register interface; it does not prove that XNU can initialize the controller in the post-LK state.
- `0021`–`0024` are an alternate mock/ramdisk-root route, useful for a controlled early userspace test but not evidence that the KL7 UFS path works.
- `0029`, `0031` and the filesystem/AMFI patches are downstream of a working root device. Do not start there.

## Phase 5 — peripheral drivers and polish

Defer until kernel logs and root storage work:

- `0058`, `0060`, `0062`, `0063`, `0071`: HID, keys, touch, battery/power and Goodix GENI details. The sweet touch/power stack contains Qualcomm-specific dependencies; identify KL7's actual touch controller and MediaTek power-management interfaces first.
- `0040`, `0045`, `0046`, `0056`, `0061`, `0079`: watchdog/display/performance and iPhone-X display assumptions. Re-check each against the KL7 hardware rather than treating dimensions or driver names as proof of compatibility.
- `0072`–`0081`: USB, networking diagnostics, and later SMP/display improvements; these are not required to prove the initial kernel handoff.
- Patches for RNG, crypto, AMFI, HFS, tracing, crash logs and bundled libraries may be needed to build or run userland, but most are not the first-boot blocker. Triage build dependencies individually once the target XNU tree is pinned.

## Immediate next experiment

Build a single-core KL7 XNU with the Phase 1 build patches and only the early-console/platform pieces needed for Phase 2. Use the v0.9.1 minimal loader with a real arm64 kernelcache and real D22AP `/product` data. The first success criterion is a stable XNU-originated marker on the framebuffer or ramoops-compatible console; do **not** enable SMP or depend on UFS until that marker is repeatable.
