# XNU port plan: ios-sweet -> KL7 / MT6789 (v0.9.3)

Source of truth: github.com/dipa-studio/ios-sweet, patch series `xnu/patches/` applied to
`apple-oss-distributions/xnu` tag **xnu-8796.141.3** (macOS 13.5 release.json, what the sweet CI builds).
Checked: sweet 0001-0081 apply cleanly in order to that tag, except 0031/0041, which need the HFS tree staged by
`0026-stage-hfs.sh` (CI does this). Our delta `xnu/patches-kl7/0101,0102` applies on top of the full series.
**Nothing here has been compiled** (the build needs macOS runners) or booted.

## How to build
Fork ios-sweet, copy `xnu/patches-kl7/*.patch` into its `xnu/patches/`, run the `xnu` workflow with
`machine_config=SWEET`. Output `kernel.development.sweet` goes to `XNU_KERNEL=` of this shim's Makefile.
Local review of the patched tree: `tools/kl7-xnu-tree.sh <ios-sweet clone> <outdir>`.
First boot args: keep `cpus=1` (the shim's default `XNU_CMDLINE`), no `rd=ufsroot0` yet.

## Done in the delta
- **0101 early framebuffer.** sweet hard-codes 0x9c000000 / 1080x2400 / stride 4320 in `start.s`,
  `sweet_early.c` and `arm_vm_init.c` (0010, 0012). KL7: visible page 0xFE519E00, 1080x2436, stride 4352,
  geometry in `SWEET.h` (`SWEET_EARLY_FB_*`) with a static assert that the page sits in one 32 MiB L2 block
  (16K granule): block 0xFE000000. The bootstrap and final-alias mappings are **Device-nGnRnE instead of
  Normal-NC**: that 32 MiB block also contains `tkv-reserved` (0xffff7000, TEE), and Normal memory may be
  speculatively read; sweet's Qualcomm shim avoided this the same way ("nothing cacheable except our image").
  Fallback if Device writes prove too slow: exact-page L3 mapping.
- **0102 names.** Platform expert also matches `tecno,kl7`, CPU driver also matches `arm,cortex-a55/a76`
  (ADT cpu nodes do not say `qcom,kryo470`), fbcon accepts `kl7-fbcon`. Identity strings already come
  from the ADT (sweet 0059).

## Already generic (checked, no change needed)
- GIC: sweet 0004/0017 use Group 1 NS IRQ + `ICC_IAR1`, timer INTID read from the ADT gic node
  (`virtual-timer-intid`, KL7 = 27 = PPI 11). `GICR_PE_SIZE` 0x20000 is right for GICv3 (observed
  GICD_PIDR2 arch=3); the redistributor walk stops at `GICR_TYPER.Last` (region is 2 MiB, 8 PEs).
- CPU: Cortex-A55/A76, same family as sweet's Kryo 470: cache line 64, ARMv8.2, no PAC. MIDR 0x412fd050,
  MMFR0 PARange 40-bit, TGran16 supported. MPIDR = Aff1 core number as on sweet.
- Handoff: sweet's shim also enters XNU with MMU off at EL1, `x0 = boot_args` (rev 2), so `transfer_to_payload` matches.
- Timer 13 MHz comes from the ADT cpu `timebase-frequency` (`pe_identify_machine.c`; 24 MHz there is only a default).
- Memory: physBase 0x140000000 is 16K aligned and L2-indexes into the same first V=P L2 page as sweet's map.

## Risks to watch on the first boot (in order of the stage markers drawn by `sweet_early_mark`)
1. White 64x32 band at the bottom of the visible page: proves `start_first_cpu` ran and the FB base is right.
2. Marks 1-8: PE_init_platform, topology (needs a `running` cpu0 with `reg`=0 in the ADT), cpu_bootstrap, arm_vm_init.
3. Marks 9-14: kernel VA alias of the FB, TTBR0 teardown, pmap_bootstrap. `ARM64_PHYSMAP_ALIGNMENT_PADDING` (0011) matters here.
4. Marks 15-20: serial_init -> `kl7-fbcon` WC mapping (`reg` = visible page only, base not 16K aligned: `ml_io_map_wcomb` must handle the offset).
5. The MT6789 watchdog is disabled by the shim at handoff (`WDT_DISABLE_AT_HANDOFF`, verified in v0.9.2); sweet's `wdt=-1` boot-arg only affects XNU's own driver.

## Later phases (unchanged from sweet unless noted)
- SMP (0065/0078): PSCI CPU_ON with the MT6789 MPIDR map (Aff1 = core, cores 6-7 are A76); keep `cpus=1` until the boot CPU is stable.
- Storage (0028/0030/0034/0069): sweet's UFS driver is Qualcomm UFS (`qcom,ufshc`); KL7 is `mediatek,mt8183-ufshci`
  (UFSHCI regs at 0x11270000 + MTK glue: M-PHY/unipro power, vendor ops). Whether LK leaves it initialised is unknown; driver port needed.
  0021-0024 (mock/ramdisk root) are the way to test userland earlier.
- Display (0046/0061/0079) is 1080x2400 sweet panel logic; KL7 is 1080x2436 with a double-buffered page offset (see 0101): rework with the framebuffer driver.
- Touch/buttons/battery (0058-0063, 0071): Qualcomm GENI/SPMI/RPMh specific, KL7 needs its own drivers.
- USB (0072-0081): Qualcomm DWC3/QUSB; MTK uses MUSB/XHCI-MTK.
