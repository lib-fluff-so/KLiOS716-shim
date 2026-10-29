# KL7 XNU bring-up plan

> **Historical document (2026-10-22).** The `0082-kl7-machine-config.patch`
> described by this plan was a hallucinated, non-building patch and has been
> removed. The real KL7 delta is `0101`-`0117`, and the verified build uses
> `MACHINE_CONFIG=SWEET`. See `README.md` and `HISTORY.md` before using this
> plan.

Target: Tecno Spark 30 Pro (KL7), MediaTek MT6789, iOS 16-generation XNU (`xnu-8796`). This tree is based on the supplied `ios-sweet` project snapshot; the upstream XNU source itself is fetched by the GitHub Actions build workflow and is not included in the archive.

## Hardware facts verified by the KL7 DTB / shim

- EL1 handoff; MIDR `0x412fd050`; 16 KiB-granule identity MMU map works.
- 8 CPUs started through PSCI in the shim; the KL7 topology is six Cortex-A55 plus two Cortex-A76, MPIDR Aff1 values `0x000`–`0x700`.
- `CNTFRQ_EL0 = 13,000,000`; virtual architected timer PPI 11, GIC INTID 27.
- GICv3 at GICD `0x0c000000`, GICR `0x0c040000`.
- XNU-managed upper DRAM starts at `0x140000000`; the KL7 framebuffer is reserved at `0xfdfff000`, 1080×2436, stride 4352, and spans the `0xfe000000` 32 MiB boundary. Map both 32 MiB blocks for early framebuffer access.
- MT6789 TOPRGU watchdog at `0x10007000` is armed by LK; the shim's WDT disable has been readback-verified. Do not let a pre-XNU test halt be mistaken for a successful long boot.

## This patch layer does now

1. Adds a real XNU `MACHINE_CONFIG=KL7`, no-PAC ARM64 flags, board header and eight-CPU capacity.
2. Enables the existing SWEET-guarded compatibility code in compiler flags, but changes the SWEET board-config include guard so `SWEET.h` is skipped when `ARM64_BOARD_CONFIG_KL7` is selected. This prevents Qualcomm board constants from leaking into KL7 while keeping the legacy shared compatibility paths compiled. KL7 timer INTID, framebuffer mapping, CPU match, and platform identity stay KL7-specific. Split compatibility workarounds from hardware code after the first stable kernel entry.
3. Makes `tools/mkadt_kl7.py` available in this repo. It parses the supplied live FDT and verifies the key hardware ranges. `--with-ufs` is opt-in so the first kernel test does not attach the SWEET UFS driver.
4. Adds KL7 matching for the platform expert, Cortex-A55 boot CPU, and early framebuffer AppleCLCD personality, keeping the SWEET personalities for the reference target.
5. Changes the kernel workflow's default machine config to `KL7`.

## First test (single core only)

1. Run the kernel workflow with `machine_config=KL7`, `kernel_config=DEVELOPMENT`, `macos_version=13.5`. This selects xnu-8796.
2. Download the `xnu-kernel` artifact and `xnu-buildtree` artifact. The build must complete before producing a payload; a shim compile is not proof of XNU compatibility.
3. Generate an early ADT from the actual KL7 FDT, deliberately without UFS or `/product`:

   `python3 tools/mkadt_kl7.py out/kl7.adt --dtb /path/to/fdt.dtb`

4. Package the actual arm64 XNU Mach-O with the KL7 ADT and a tiny diagnostic ramdisk, using `tools/mkpayload_kl7.py`. Keep the boot arguments at `cpus=1` and `rd=md0`; do not enable SMP or `rd=ufsroot0` during the first run.
5. Boot through the existing shim with watchdog disable at handoff enabled. Acceptance criterion is a repeatable XNU-originated marker after entering `arm_init` / completing platform setup, plus a recoverable log or visible marker. Record the precise marker and any panic/ELR before changing the next subsystem.

## Explicitly deferred

- Enabling secondary XNU CPUs (shim-side SMP is already proven, but XNU's A55/A76 topology and PSCI path are not; the first ADT keeps a flat temporary topology and boot args request `cpus=1`).
- KL7 UFS host integration/root writes; the current SWEET UFS code is Qualcomm-specific and must not be considered portable because the register range or DT compatible resembles UFSHCI.
- Display IOKit / IOSurface, touch, keys, PMIC, networking and USB.
- iBoot-style mode, Android chainload and recovery UI.
- Full iOS userspace boot. No real Mach-O has yet completed handoff through the shim; that remains a prerequisite test.

## Validation status

The uploaded `ios-sweet` snapshot contains the patch series and build workflow, but not a checked-out XNU source tree. This preparation can validate patch-file syntax, KL7 FDT/ADT generation, Python helpers and the known memory/register values; it cannot honestly claim `git apply` or XNU compilation success until the GitHub Actions workflow runs against the pinned `blacktop/darwin-xnu-build` source checkout.
