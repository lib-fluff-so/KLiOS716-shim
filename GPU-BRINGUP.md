# KL7 Mali-G57 GPU bring-up

KL7 is a TECNO Spark 30 Pro with a MediaTek MT6789 (Helio G99) and an
ARM Mali-G57 MC2 (Valhall-JM).  This is **not** an Adreno/freedreno target, so
the Poco F1 (sweet) and Beryllium GPU code cannot be copied.  Beryllium is only
an architectural reference: kernel-side GPU user client, Mesa backend, EAGL
shim, and QuartzCore/backboardd integration.

## Hardware map

| Block | Physical base | Notes |
| --- | --- | --- |
| Mali GPU | `0x13000000` | `GPU_ID` at `+0x0`; 4 KiB register window |
| MFG top/config | `0x13fbf000` | MFG clock gate at `+0x0`; status bit 0 (0 = enabled) |
| SCPSYS | `0x10006000` | AP-side MFG power domains |
| Topckgen | `0x10000000` | `CLK_CFG_4` at `+0x50` |
| PWRAP | `0x10026000` | MT6366 PMIC access; VGPU/VSRAM_GPU rails |

SCPSYS MFG data from `mtk-scpsys-mt6789.c`:

- MFG0..MFG3 control: `0x308`, `0x30c`, `0x310`, `0x314`.
- Power status: `0x16c`, second status: `0x170`; MFG0..3 are bits 2..5.
- Power-on bits: `PWR_ON` bit 2, `PWR_ON_2ND` bit 3; clear `PWR_CLK_DIS`
  bit 4, clear `PWR_ISO` bit 1, set `PWR_RST_B` bit 0.
- SRAM power-down bit 8, acknowledge bit 12.
- MFG1 has the `mfg` basic clock and the bus-protect table.

Topckgen `CLK_CFG_4`:

- `mfg_ref_sel` bits 16..17 (0 = 26 MHz reference).
- `mfg_pll_sel` bit 18.
- `mfg_ref_sel` gate/power-down bit 23 (0 = enabled).
- The probe uses the 26 MHz reference, not the GPU PLL.

MFG clock gate (`0x13fbf000`):

- Status at `+0x0`, bit 0: 0 = enabled, 1 = gated.
- Enable by writing bit 0 to `+0x8` (MTK write-1-to-clear gate).
- The GPU gate should only be touched after MFG0/MFG1 are on.

## Current probe

`src/gpu_test.c` is integrated with `GPU_TEST=1`:

```sh
make OUT=out-gpu-test GPU_TEST=1 GPU_POWER_TEST=0 AUTO_REBOOT_TEST=1
```

`AUTO_REBOOT_TEST=1` makes the shim finish with PSCI SYSTEM_RESET, which is a
warm reset: DRAM and the ramoops/pstore ring survive. A halt plus power-cycle
is a cold reset and loses the shim log.

The default build is read-only with respect to the GPU power/clock controls.
It does perform PWRAP PMIC read transactions to report the MT6366 rails.  It:

1. Logs SCPSYS status and MFG0..3 control/status.
2. Performs PWRAP reads of the MT6366 VGPU and VSRAM_GPU enable/voltage
   registers.
3. Stops unless all four MFG power domains are already on.
4. Verifies `mfg_ref_sel=0`, `mfg_pll_sel=0`, gate bit 23 clear.
5. Reads the MFG gate status; if it is gated, leaves it alone and stops.
6. Reads `GPU_ID`, L2/tiler/memory/MMU/AS/IRQ registers.

Expected successful output includes:

```text
GPU TEST: MT6789 Mali-G57 MC2 probe
GPU: SCP status=.../...
GPU: MFG0 ctl=... sta=1
GPU: MFG1 ctl=... sta=1
...
GPU: PMIC swcid=... rc=0
GPU: VGPU con0=... en=1 elr=... dbg1=...
GPU: VGPU vsel=... approx=...mV
GPU: VSRAM_GPU con0=... en=1 vsel=... dbg0=...
GPU: topcfg4=... ref=0 pll=0 pdn=0
GPU: MFG_CG status=0
GPU: ID=90930010 product=9093 rev=0.1 status=0
GPU: identified Mali-G57 / Valhall (TNAX, product 9093)
GPU: L2=... tiler=... mem=... mmu=... as=... irq=...
GPU TEST: done
```

The KL7 reports product `0x9093`, which is TNAX (Valhall 9, product
major 3), not the older TTRX (9,0) value.  The vendor `mali_kbase_gpu_id.h`
defines TNAX as `GPU_ID2_MODEL_MAKE(9, 3)`; Linux/panfrost treats `0x9093`
as a Mali-G57 variant (used on MT8188).  The probe accepts both TTRX and
TNAX and reports which one it saw.

If any MFG domain is off, the probe stops before touching the GPU register block:

```text
GPU: MFG2 is off; read-only probe stops here
```

That result means the next milestone is power sequencing, not a GPU bug.

## First read-only result (2026-10-27)

The warm-reset probe succeeded.  The shim log survived in
`/sys/fs/pstore/console-ramoops-0` and showed:

```text
GPU: SCP status=f83a1bc1/f83a1bc1
GPU: MFG0 ctl=1112 sta=0
GPU: MFG1 ctl=1112 sta=0
GPU: MFG2 ctl=1112 sta=0
GPU: MFG3 ctl=1112 sta=0
GPU: PMIC swcid=6610 rc=0
GPU: VGPU con0=1 en=1 elr=38 dbg1=43
GPU: VGPU vsel=56 approx=850mV
GPU: VSRAM_GPU con0=1 en=1 vsel=38 dbg0=3824
GPU: MFG0 is off; read-only probe stops here
```

So all four MFG power domains are off, while the VGPU and VSRAM_GPU rails
are already on at about 850 mV.  The next step is the experimental MTCMOS
power-on with those rails known good.

## Power test result (2026-10-27)

The MTCMOS/clock power probe ran with the VGPU and VSRAM_GPU rails already
confirmed on at about 850 mV.  The warm-reset log showed:

```text
GPU: power probe topcfg4=1000000
GPU: topcfg4=1000000 ref=0 pll=0 pdn=0
GPU: powering MFG0
GPU: MFG0 on ctl=d
GPU: powering MFG1
GPU: MFG1 on ctl=d
GPU: powering MFG2
GPU: MFG2 on ctl=d
GPU: powering MFG3
GPU: MFG3 on ctl=d
GPU: SCP status=f83a1bfd/f83a1bfd
GPU: MFG0 ctl=d sta=1
GPU: MFG1 ctl=d sta=1
GPU: MFG2 ctl=d sta=1
GPU: MFG3 ctl=d sta=1
GPU: MFG_CG status=0
GPU: ID=90930010 product=9093 rev=0.1 status=0
GPU: identified Mali-G57 / Valhall (TNAX, product 9093)
GPU: L2=8120206 tiler=809
GPU: mem=1 mmu=2830 as=ff irq=100
```

All four MFG domains powered (`ctl=d`, status 1), the MFG clock gate reads
enabled (`status=0`), and the GPU ID/L2/tiler/MMU registers are readable.
This proves the SCPSYS power sequence, 26 MHz reference clock, gate, and APB
register path.

The probe still did not set the VGPU/VSRAM_GPU voltages; it only verified
that the rails were already on.  It also has not disabled bus protection nor
touched the GPU MMU or any job queue.  It is a bring-up probe, not a
production power-on path.

The PSCI reset timer was armed as a backstop; the probe returned normally and
the direct PSCI reset fired first.  The pstore ring contains the repeated
slot-B attempts and the final Android boot preserved it.

## MMU / BO / job result (2026-10-28)

The experimental traffic test ran seven times from `boot_b`.  Every slot-B
attempt produced the same result, and the PSCI reset preserved the log.
Build it with:

```sh
make OUT=out-gpu-traffic GPU_TEST=1 GPU_POWER_TEST=1 \
  AUTO_REBOOT_TEST=1 PSCI_RESET_TIMER=1
```

```text
GPU TEST: releasing MFG1 bus protection
GPU: busprot before en=0/0/0
GPU: busprot before sta=e60c1806/11c70400/4
GPU: busprot after en=0/0/0
GPU: busprot after sta=e60c1806/11c70400/4
GPU: extcg after 20=3e00000 24=7 b0=281001 b4=10 f60=1
GPU: reset raw=100 status=100
GPU: internal power ready l2=1 shader=5 tiler=1
GPU: coherency feat=0 set=none
GPU: js=7 as=ff mmu=2830 va=48 pa=40
GPU: JS0 feature=20e
GPU: PT pa=40009000/4000a000/4000b000/4000c000 bo=40006000/40007000/40008000
GPU: PTE l0=4000a403 l1=4000b403 l2=4000c403 l3=40006757/40007757/40008757
GPU: AS0 enable tt=40009000 cfg=420001c6
GPU: AS0 tt=40009000/0 mm=4c4c4c4c cf=420001c6/0 st=0
GPU: JS0 submit head=40000000 af=5 cfg=83300
GPU: write-value rc=0 raw=1 js=0 state=0
GPU: write-value out=0123456789abcdef want=0123456789abcdef
GPU: WRITE VALUE PASS
GPU: JS0 submit head=40002000 af=5 cfg=83300
GPU: fault rc=18446744073709551612 jraw=0 mraw=1 js=8 fstat=7c0003c3
GPU: fault faddr=0000000040100000 gstat=0 gaddr=0000000000000000 gint=600
GPU: MMU FAULT TEST INCOMPLETE
```

`rc=18446744073709551612` is `(u64)-4`, the 5 s job-wait timeout; the
deliberate MMU fault prevented a normal job completion interrupt.

What the result proves:

- MFG1 bus protection reads 0/0/0 and remains released after the test.  The
  subsequent GPU traffic works, so the protection is not blocking the GPU.
- External CG/QChannel setup and the GPU soft reset complete (`raw=100`,
  `status=100`), and L2/shader/tiler internal power reports ready.
- AS0 uses AArch64-4K, 4-level, 48-bit page tables.  The lock/flush/update
  sequence completes with `st=0`.
- Three static in-shim pages are mapped as a minimal BO: job, output, and
  the second job page.  Dynamic BO allocation, remapping, and reuse are
  tested separately below.
- The Write Value job completes with `rc=0`, `raw=1`, and writes
  `0123456789abcdef` to the mapped output page.
- The second job targets the unmapped `0x40100000` VA and raises an AS0 MMU
  fault: `mraw=1`, `faddr=0x40100000`, `fstat=0x7c0003c3`.  The low byte
  `0xc3` is a level-3 translation fault; access type bits 9:8 = 3 (write).

The old image printed `MMU FAULT TEST INCOMPLETE` only because its criterion
also required `JOB_INT_RAWSTAT` (`jraw`) to be nonzero.  On this GPU the
fault leaves the job active (`js=8`, `ACTIVE`) and does not raise a job
interrupt.  The corrected criterion uses `MMU_INT_RAWSTAT` plus the AS fault
address.  `gint=0x600` is `POWER_CHANGED | POWER_CHANGED_ALL`, not a fault
bit.

The test leaves the GPU powered and bus protection released; it does not
power the GPU back down.  It ends with PSCI SYSTEM_RESET and is not a
production power-on/off path.

The first tested image was:

```text
out-gpu-traffic/shim_b.img
  size 20,480 bytes
  sha256 d1770e8377f2ec233f820636e3425ec6be8bad8418f693ec567f575c9dcddc99
```

The archived pstore capture is
`/tmp/opencode/ramoops-gpu-traffic-7run.txt` (262,132 bytes, sha256
`f102a25f31470ff79a35d45163bd088760db2e73ba257207e2364e1ccc449fb7`).
It contains seven identical attempts.

A second flash with the corrected criterion produced seven more attempts,
all ending in:

```text
GPU: MMU FAULT TEST PASS (unmapped VA faulted, js=8)
```

The re-test capture is `/tmp/opencode/ramoops-gpu-traffic.txt` (262,132
bytes, sha256 `ea02ed8fb87658a9430ea5d433853520e684d85f965195d4246f7fbd0673786f`).
It contains 7x `WRITE VALUE PASS` and 7x `MMU FAULT TEST PASS`.

The flashed re-test image was:

```text
out-gpu-traffic/shim_b.img
  size 20,480 bytes
  sha256 e90675ec7bfe22c8db748d618c84171771ee92b8f8b3c54075ff16bd27c9cd92
```

The final source build removes an unused secondary `JS_STATUS` fallback from
the criterion.  The primary `mraw` + fault-address path is unchanged, so the
hardware result applies to it as well.  Final build:

```text
out-gpu-traffic/shim_b.img
  size 20,480 bytes
  sha256 e3416f314b1fc9b3a36e674b673c308a389ac4b5868facf8635c86b347aa4042
```

### Dynamic BO allocator, remap, reuse (2026-10-28)

The next image adds a 16-page in-shim BO pool and tests allocator allocation,
GPU page-table mapping, unmap/remap, and allocator reuse.  Every BO job gets a
fresh job page and a fresh GPU VA from `GPU_VA_JOB_POOL`, so job descriptor
reuse cannot mask page-table bugs.

The test sequence is:

1. Allocate BO A (2 pages) and BO B (1 page).
2. Map A0/A1 at `0x40040000/0x40041000`, B at `0x40042000`.
3. Submit three Write Value jobs through AS0 and verify all three values.
4. Unmap A1, map a fresh BO C at the same VA, write C, and verify the old A1
   page still holds its original value.
5. Free A, allocate D (2 pages), and confirm first-fit reuse returns the same
   base address (`a=40010000 d=40010000`).
6. Map/write/verify D0 and D1, unmap the BO VAs, free the pool, then run the
   existing unmapped-VA fault test.

All seven slot-B attempts produced:

```text
GPU: BO alloc a=40010000 b=40012000
GPU: BO mapped a=40010000/40011000 b=40012000
GPU: BO a0 job=40060000 pa=40013000 rc=0 raw=1 got=1111111111111111 want=1111111111111111
GPU: BO a1 job=40061000 pa=40014000 rc=0 raw=1 got=2222222222222222 want=2222222222222222
GPU: BO b job=40062000 pa=40015000 rc=0 raw=1 got=3333333333333333 want=3333333333333333
GPU: BO c job=40063000 pa=40017000 rc=0 raw=1 got=4444444444444444 want=4444444444444444
GPU: BO remap old=2222222222222222 new=4444444444444444
GPU: BO REMAP PASS
GPU: BO reuse a=40010000 d=40010000
GPU: BO d0 job=40064000 pa=40018000 rc=0 raw=1 got=5555555555555555 want=5555555555555555
GPU: BO d1 job=40065000 pa=40019000 rc=0 raw=1 got=6666666666666666 want=6666666666666666
GPU: BO TEST PASS
GPU: MMU FAULT TEST PASS (unmapped VA faulted, js=8)
```

The first BO attempt failed at `a1` because `GPU_VA_BO_A + 0x1000` was exactly
equal to `GPU_VA_BO_B`; mapping B silently replaced A1.  Moving B to
`0x40042000` fixed it.  The test now proves:

- Dynamic 4 KiB page-table insertion/removal with `AS_COMMAND_LOCK` +
  `AS_COMMAND_FLUSH_PT` works while AS0 is active.
- Unmapping and remapping a VA changes the GPU-visible physical page without
  disturbing the old page.
- The first-fit allocator reuses a freed two-page range at the same base.
- Write Value jobs work from freshly mapped job pages and fresh job VAs, not
  just from the static startup pages.

The tested image is:

```text
out-gpu-traffic/shim_b.img
  size 20,480 bytes
  sha256 98131b7d19470286b5b1b560a86005fcc506b8c422272aa881586fdb51c24f5d
```

The archived pstore capture is `/tmp/opencode/ramoops-gpu-bo3.txt`
(262,132 bytes, sha256
`5310e9f737a52f1763e7d3fa33231af9731ac3cee483b54a172a33741cdaf0ac`).
It contains 7x `BO TEST PASS` and 7x `MMU FAULT TEST PASS`.

### Multi-slot concurrent Write Value jobs (2026-10-28)

The next image adds a second output page at `GPU_VA_OUT2 = 0x40003000`
(L3 index 3) and submits two Write Value jobs at once: slot 0 targets
`GPU_VA_OUT`, slot 1 targets `GPU_VA_OUT2`.  Both jobs use the same AS0 and
the same affinity mask, and the test waits for each slot independently.

The JS feature dump is:

```text
GPU: JS0 feature=20e
GPU: JS1 feature=1c9e
GPU: JS2 feature=1e
```

`0x20e` is null/set-value/cache-flush/fragment.  `0x1c9e` adds compute and
tiler.  `0x1e` is null/set-value/cache-flush/compute.  Slot 2 is therefore
the natural compute slot; slots 0 and 1 are used for the multi-slot test.

All seven slot-B attempts produced:

```text
GPU: MULTISLOT slots=0/1
GPU: JS0 submit head=40000000 af=5 cfg=83300
GPU: JS1 submit head=40002000 af=5 cfg=83300
GPU: MULTISLOT slot0 rc=0 raw=3
GPU: MULTISLOT slot1 rc=0 raw=2
GPU: MULTISLOT out0=aaaaaaaaaaaaaaaa out1=bbbbbbbbbbbbbbbb
GPU: MULTISLOT TEST PASS
```

`slot0 raw=3` contains both done bits because slot 1 completed before the
slot 0 wait observed the interrupt.  `wait_job()` clears only the bits for
the slot it waited on, so slot 1's bit remains for its own wait.  This proves
two job slots can execute and complete independently against the same AS0.

The tested image is:

```text
out-gpu-traffic/shim_b.img
  size 20,480 bytes
  sha256 0de9b9df5030876891f1562d8049f6b5c038ec4fad672722e39b8559e73cd263
```

The archived pstore capture is `/tmp/opencode/ramoops-gpu-ms.txt`
(262,132 bytes, sha256
`7ab2a3bc4956ee62cd1673c49689cab81e2a3d0fc292d0559fa5fb0e670d5676`).
It contains 7x `MULTISLOT TEST PASS` and 7x `MMU FAULT TEST PASS`.

### Compute job and Valhall shader descriptor (2026-10-28)

The next image adds a compute job on slot 2, the only slot whose JS feature
word has the compute bit (`JS2=1e`).  The job is a 128-byte Valhall compute
job:

- `h[0..7]`: job header; `h[4]=0x10008` means type 4 (compute), index 1.
- `h[8..31]`: compute payload.  `h[8]=0` is workgroup size 1x1x1 (stored
  minus one), `h[9]=0x8001` is task increment 1 and task axis Z,
  `h[10..12]=1` is workgroup count 1x1x1.
- `h[16..31]`: shader environment.  `h[24]` resources, `h[26]` shader
  descriptor, `h[28]` thread storage, `h[30]` FAU.

The critical finding is that `h[26]` must point to a 32-byte Valhall
**Shader Program** descriptor, not directly to the instruction stream.
The descriptor used here is:

```text
D0 = 0x80000018   Type=Shader(8), Stage=Compute(1), 32 regs/thread(2)
D1 = 0
D2 = code VA low 32
D3 = code VA high 32
D4..D7 = 0
```

The descriptor is 32-byte aligned and lives at `GPU_VA_SHADER_DESC =
0x40084000` (L3 index 0x84).  The NOP stage points its `Binary` field at
`GPU_VA_SHADER_NOP = 0x40083000` (L3 index 0x83); the store stage points it
at `GPU_VA_SHADER = 0x40080000` (L3 index 0x80).

Two earlier faults proved the descriptor requirement:

- Raw store code as the shader pointer: `faddr=0x0000c30000000080`,
  `fstat=0x1c0`.  The GPU read word 2 of the raw instruction stream
  (`0x0110c300000000c0`) as the 64-bit `Binary` address.
- Raw NOP code as the shader pointer: `faddr=0`, `fstat=0x1c1`, because the
  NOP page had no descriptor words and the `Binary` field was zero.

With the descriptor in place, both stages pass:

```text
GPU: COMPUTE NOP rc=0 raw=4 js=0 state=0
GPU: COMPUTE NOP PASS
GPU: COMPUTE STORE rc=0 raw=4 js=0 state=0
GPU: COMPUTE out=0123456789abcdef want=0123456789abcdef
GPU: COMPUTE STORE PASS
```

The store shader is seven Valhall instructions: `IADD_IMM.i32` r2=0x40003000,
r3=0, r0=0x89abcdef, r1=0x01234567; `STORE.i64.slot0`; `NOP.wait0`;
`NOP.end`.  The GPU writes the expected 64-bit value through the AS0 MMU to
`GPU_VA_OUT2=0x40003000`.

PTE mapping for the compute pages:

```text
pt_l3[0x80] = 0x40011757   shader code
pt_l3[0x81] = 0x40012757   compute job
pt_l3[0x82] = 0x40013757   TLS
pt_l3[0x83] = 0x40014757   NOP shader code
pt_l3[0x84] = 0x40015757   shader-program descriptor
```

The tested image is:

```text
out-gpu-traffic/shim_b.img
  size 24,576 bytes
  sha256 8100fe4c8d83bfe2ac10407c7698d36f0bc656903ef7f0a34760087398fd8066
```

The archived pstore capture is `/tmp/opencode/ramoops-gpu-compute3.txt`
(262,132 bytes, sha256
`129d2eec22cb4c4289f14e065168b77fe884214541aadee2045083be0c5db796`).
It contains `COMPUTE NOP PASS`, `COMPUTE STORE PASS`, `BO TEST PASS`,
`MULTISLOT TEST PASS`, and `MMU FAULT TEST PASS`.

This proves the full compute path: job submission, shader environment,
shader-program descriptor, Valhall instruction fetch, register writes, a
64-bit store through the GPU MMU, and job completion on slot 2.  It does not
yet prove thread/workgroup IDs, barriers, TLS, textures, tiler/fragment/
vertex jobs, or the device-side user client.

## Power test details

`GPU_POWER_TEST=1` follows the SCPSYS power-on sequence (MFG0 -> MFG1 ->
MFG2/MFG3), switches the GPU mux to the 26 MHz reference, and enables the MFG
gate.  It does **not** yet bring up the MT6366 VGPU and VSRAM_GPU rails first.
The vendor `gpufreq_mt6789.c` sequence is:

1. VGPU buck on.
2. MTCMOS (MFG domains) on.
3. GPU clock on.

The current power test refuses to power MTCMOS unless the read-only PMIC probe
confirms that both VGPU and VSRAM_GPU are already enabled.  It still does not
set their voltages, so it is a diagnostic, not a production power-on path.  It
is only to be used after the read-only result is known.

Relevant MT6366 registers (from `mt6358-regulator.c`):

- VGPU enable: `BUCK_VGPU_CON0` `0x1508`, enable bit 0.
- VGPU voltage: `BUCK_VGPU_ELR0` `0x1526`, mask `0x7f`, 500 mV + 6.25 mV/step.
- VGPU status: `BUCK_VGPU_DBG1` `0x1520`, bit 0.
- VSRAM_GPU enable: `LDO_VSRAM_GPU_CON0` `0x1bc8`.
- VSRAM_GPU voltage: `LDO_VSRAM_CON3` `0x1bf4` (verify on the device).
- PMIC is reached through PWRAP at `0x10026000` using the same WACS2
  sequence as `src/battery_test.c`.

## Flash / recovery contract

- Only `boot_b` is ever written.
- `boot_a` must not be touched.
- Preferred when Android is running:
  ```sh
  adb -s 13016374AF000229 reboot bootloader
  fastboot devices -l
  fastboot flash boot_b /path/to/shim_b.img
  fastboot --set-active=b
  fastboot getvar current-slot
  fastboot reboot
  ```
  Use fastboot only for `boot_b`.  Never `fastboot flash boot_a`.
- `fastboot reboot` is a cold reset: it drops ramoops.  It is fine for
  restore/flash, but never use it to end a shim test whose log you need.
- If Android/fastboot is unavailable, use Antumbra direct BROM flashing.
  Start Antumbra first and let it wait for the transient BROM port.
- The v0127 battery-native restore image is:

```text
/home/fluff_/kl7/ios-kl7-v0.10.0/out-v0127-battery/readonly/shim_b.img
sha256 aad8f6efdbe21134a73fa844806c130ee329566f18cea38b810f3d4ec5ae8dda
```

- Restore after a test with `/tmp/opencode/revert-v0127.sh`, or by the fastboot
  sequence above.
- Never use `/home/fluff_/Downloads/recovery.img` or
  `/home/fluff_/TWRP/recovery.img`.

## Next milestones

1. (done 2026-10-27) Flash the read-only probe and record SCPSYS, clock, gate,
   PMIC rails, and GPU ID output.
2. (done 2026-10-27) Use the PMIC readback to confirm VGPU/VSRAM_GPU are on,
   then power MFG0..3 and enable the clock/gate.  GPU ID `0x90930010` is TNAX
   (Mali-G57), not TTRX.
3. (done 2026-10-28) Disable MFG1 bus protection, enable the external
   CG/QChannels, reset the GPU, and power the internal L2/shader/tiler
   domains for real GPU traffic.
4. (done 2026-10-28) Bring up AS0 with AArch64-4K 4-level page tables,
   map static and dynamic buffer objects, submit Write Value and multi-slot
   jobs, observe a deliberate MMU fault, and execute a real Valhall compute
   shader through a Shader Program descriptor.
5. Add tiler/fragment/vertex jobs, a framebuffer, and interrupt/fence
   handling; then add the device-side `SweetMali` user client, a real BO
   allocator, and the Panfrost backend; only then start the
   EAGL/QuartzCore integration.

The architecture reference is Beryllium's `SweetAdreno630` split, but every
Qualcomm/freedreno-specific piece must be replaced with MediaTek SCPSYS/PMIC
power code and Mesa Panfrost.

See `GPU-ACCELERATION-PLAN.md` for the full stack roadmap: shim 3D pipeline,
XNU `SweetMali` user client, Mesa panfrost + libdrm stand-in, EAGL/GLES shim,
and the CoreAnimation renderer hook.
