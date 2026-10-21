# v0.6.0 - memory map, 16 KiB MMU, secondary cores

## What the live DTB (fdt.dtb) says
- DRAM: one `/memory` range, `0x40000000 + 0x200000000` (8 GiB, two ranks; `dramc-rk0/rk1` markers at `0x13ffff000` and `0x23ffff000`).
- `/reserved-memory`: 36 children, 30 with `no-map` (TEE `0x70000000+0x6000000`, BL31 `0x48200000`, SSPM/MCUPM/SCP, modem, wifi, pstore, ...), 6 without (framebuffer `0xfdfff000+0x1ff8000`, aee_lk, log_store, tran_devmsg, gz-log, SSMR CMA pool). Linux maps the latter, so the shim does too.
- **pstore (ramoops) is itself `no-map`.** A naive "RAM minus no-map" map would unmap the shim's own log channel, so `0x48090000+0xe0000` is force-mapped.
- Interrupt controller `arm,gic-v3`: GICD `0x0c000000`, GICR `0x0c040000`. Timer 13 MHz. PSCI 1.0 via `smc`.
- 8 CPUs, `reg` = `0x0, 0x100, 0x200 ... 0x700` (one per Aff1, not Aff0). `CPU_ON` takes these values; they are read from the DTB, not guessed.

## Translation setup
16 KiB granule, T0SZ=24 (40-bit VA, start at level 1), 32 MiB level-2 blocks, 16 KiB level-3 pages only where a no-map region cuts a block (18 tables on this DTB). MAIR: Attr0 Device-nGnRnE, Attr1 Normal Non-cacheable, Attr2 Normal WB (unused so far). Table walks are Non-cacheable so stale lines left by LK can never be read as descriptors. Everything except the shim image is PXN+UXN. TTBR1 walks disabled. `SCTLR.C = SCTLR.I = 0`: the first MMU boot proves translation and attributes without involving cache maintenance.

The page tables, per-CPU contexts and secondary stacks live in a NOLOAD section behind the image (about 2.2 MiB). The file and `image_size` are unchanged (kept as on the proven v0.5.2 boot); at runtime the stage aborts before touching anything if that area overlaps the DTB, the initrd or a no-map region.

## Verified here
`make hosttest` parses the real `fdt.dtb`, builds the tables and walks them: 30 no-map regions, 8 CPUs, spot checks (GICD, WDT, image exec, DTB, initrd, framebuffer, ramoops forced, TEE/BL31/pstore neighbours unmapped, edges of RAM) and a sweep of every 16 KiB page of all 8 GiB (0 mismatches, 7902 MiB mapped). All build variants compile cleanly. **Not run on hardware**; no emulator here can model this SoC.

## First device test
Build, `pc/flash_shim.sh`, let the shim run (pages 1-4 take 16 s, then page 5 ~8 s plus CPU bring-up), return to slot A, then `cat /sys/fs/pstore/console-ramoops-0`. Read the last `mm:` / `cpu:` line:

| last line | meaning |
|---|---|
| `mm: parse FAILED code -N` | DTB structure not as expected (-3 cell sizes, -4 no /memory, -5 too many no-map) |
| `mm: ABORT image/pool overlaps ...` | the NOLOAD area collides with DTB/initrd/no-map; nothing was changed |
| `mm: VERIFY FAIL <name>` | software walk says a needed address is unmapped; MMU was not enabled |
| `mm: enabling MMU` (and nothing after) | the hardware faulted or hung right at the switch; an exception screen/log should follow if the vector path survived |
| `mm: MMU ON` then `GICD_PIDR2` with `arch=3` | translation, Device and Normal mappings all work |
| `cpu: CPU_ON <mpidr> ret=...` | PSCI return code: 0 ok, ...fe = INVALID_PARAMETERS, ...fd = DENIED, ...fc = ALREADY_ON, ...f9 = INVALID_ADDRESS; `state=2` means that core reached MMU-on |
| `cpu: N of M secondary cores reached MMU-on` | final result |

Use `make MMU_FAULT_TEST=1` once afterwards: it must end on an exception screen with a translation fault (EC `0x25`) at FAR `0x70000000`, which proves no-map regions are really unmapped. Use `CPU_BRINGUP=0` or `MMU_STAGE=0` to bisect if something hangs.

## v0.6.1 - secondary-core diagnostics (after the first device run: `CPUn <mpidr> R=0 ST=0` for all cores)
`R=0` = PSCI accepted CPU_ON, `ST=0` = no report from the core in shared memory within 100 ms. Three explanations: the core never reached `secondary_entry` (entry/EL), it reached it but the boot CPU read a stale cache line, or it was just slow. v0.6.1 separates them without relying on shared memory:
- every core paints a 32x32 square on the framebuffer in its very first instructions (column = Aff1): **orange = entered `secondary_entry`**, **green = its MMU is on**, then it keeps repainting (so the page render does not erase it). Squares sit at visible row 900, x = 40 + 72*Aff1.
- the boot CPU invalidates the context cache line before every poll, waits up to 300 ms, and asks PSCI `AFFINITY_INFO` (`A=` on screen/log: 0 ON, 1 OFF, 2 ON_PENDING).

Reading it: no square + `A=0` -> core runs but never at our entry (wrong entry address/EL, or firmware jumped elsewhere); no square + `A=1` -> core went off again; orange square only -> reached the entry but died before/at the MMU enable (look for an exception screen); green square but `ST=0` -> cores are fine, only the shared-memory report was stale; `ST=2` -> everything works.

## v0.6.2 - first v0.6.1 run: all 7 cores painted orange, none green
The cores do reach `secondary_entry` (entry address and EL are fine) but die silently before the MMU-on paint, with no exception screen. Most likely the context the boot CPU wrote (through a Non-cacheable mapping, after its own MMU was on) was not in DRAM when the secondary read it with its MMU off, so the secondary enabled its MMU with TTBR0/TCR = 0 and faulted without a usable vector. v0.6.2 cleans+invalidates the context lines before `CPU_ON`, and adds stage colours: **red + park** if the context reads as zeros (stack or TCR = 0), **blue** just before the MMU enable, then green. Red = the cache theory is right and something else must publish the context (or write it before the MMU is on); blue then nothing = the MMU enable itself faults on the secondary (look at SCTLR/TCR requirements per core); green = fixed.
