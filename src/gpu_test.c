/*
 * Read-only Mali GPU identification probe for KL7 (MT6789, Mali-G57 MC2).
 *
 * The GPU is the ARM Mali-G57 MC2 at 0x13000000.  The AP-side power and
 * clock controls are the MediaTek SCPSYS block at 0x10006000 and the MFG
 * top-config block at 0x13fbf000.  The register layouts here come from:
 *
 *   drivers/soc/mediatek/mtk-scpsys-mt6789.c
 *   drivers/soc/mediatek/mtk-scpsys.c
 *   drivers/clk/mediatek/clk-mt6789.c
 *   drivers/clk/mediatek/clk-mt6789-mfgcfg.c
 *   drivers/gpu/mediatek/gpu_mali/.../gpu/mali_kbase_gpu_regmap.h
 *
 * Default build is strictly read-only with respect to the GPU power/clock
 * controls: it never writes SCPSYS, topckgen, the MFG gate or the GPU.  It
 * only touches the GPU registers when all four MFG power domains are already
 * on and the MFG clock selection is the safe 26 MHz reference.  It also
 * performs PMIC read transactions through PWRAP to report the MT6366
 * VGPU/VSRAM_GPU rail state.  GPU_POWER_TEST=1 enables the experimental
 * power-on path documented below; that path is intentionally not part of the
 * default build.
 */
#include "console.h"
#include "gpu_test.h"

#ifndef GPU_POWER_TEST
#define GPU_POWER_TEST 0
#endif

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;

#define SCPSYS_BASE		0x10006000ULL
#define SCPSYS_PWR_STATUS	0x016cU
#define SCPSYS_PWR_STATUS_2ND	0x0170U

#define MFG0_CTL		0x0308U
#define MFG1_CTL		0x030cU
#define MFG2_CTL		0x0310U
#define MFG3_CTL		0x0314U

#define PWR_RST_B_BIT		(1U << 0)
#define PWR_ISO_BIT		(1U << 1)
#define PWR_ON_BIT		(1U << 2)
#define PWR_ON_2ND_BIT		(1U << 3)
#define PWR_CLK_DIS_BIT		(1U << 4)
#define PWR_SRAM_PDN_BIT	(1U << 8)
#define PWR_SRAM_PDN_ACK_BIT	(1U << 12)

#define MFG_TOP_BASE		0x13fbf000ULL
#define MFG_TOP_CG_STATUS	0x0000U
#define MFG_TOP_CG_SET		0x0004U
#define MFG_TOP_CG_CLR		0x0008U
#define MFG_TOP_CG_BIT		(1U << 0)

#define TOPCKGEN_BASE		0x10000000ULL
#define TOP_CLK_CFG_4		0x0050U
#define TOP_CLK_CFG_4_CLR	0x0058U
#define TOP_CLK_CFG_UPDATE	0x0004U
#define TOP_MFG_REF_SHIFT	16U
#define TOP_MFG_REF_MASK	(3U << TOP_MFG_REF_SHIFT)
#define TOP_MFG_PLL_SHIFT	18U
#define TOP_MFG_PLL_BIT		(1U << TOP_MFG_PLL_SHIFT)
#define TOP_MFG_REF_UPDATE_BIT	(1U << 18)
#define TOP_MFG_REF_GATE_BIT	(1U << 23)

#define GPU_BASE		0x13000000ULL
#define GPU_ID			0x0000U
#define GPU_L2_FEATURES		0x0004U
#define GPU_TILER_FEATURES	0x000cU
#define GPU_MEM_FEATURES	0x0010U
#define GPU_L2_CONFIG		0x0048U
#define GPU_MMU_FEATURES	0x0014U
#define GPU_AS_PRESENT		0x0018U
#define GPU_JS_PRESENT		0x001cU
#define GPU_IRQ_RAWSTAT		0x0020U
#define GPU_INT_CLEAR		0x0024U
#define GPU_INT_MASK		0x0028U
#define GPU_CMD			0x0030U
#define GPU_STATUS		0x0034U
#define LATEST_FLUSH		0x0038U
#define GPU_JM_CONFIG		0x0f00U
#define GPU_SHADER_CONFIG	0x0f04U
#define GPU_TILER_CONFIG	0x0f08U
#define GPU_L2_MMU_CONFIG	0x0f0cU
#define GPU_FAULT_STATUS	0x003cU
#define GPU_FAULT_ADDRESS_LO	0x0040U
#define GPU_FAULT_ADDRESS_HI	0x0044U
#define GPU_JS_FEATURES(n)	(0x00c0U + ((n) * 4U))
#define GPU_L2_PRESENT_LO	0x0120U
#define GPU_L2_PRESENT_HI	0x0124U
#define GPU_SHADER_PRESENT_LO	0x0100U
#define GPU_SHADER_PRESENT_HI	0x0104U
#define GPU_SHADER_READY_LO	0x0140U
#define GPU_SHADER_READY_HI	0x0144U
#define GPU_TILER_PRESENT_LO	0x0110U
#define GPU_TILER_PRESENT_HI	0x0114U
#define GPU_TILER_READY_LO	0x0150U
#define GPU_TILER_READY_HI	0x0154U
#define GPU_L2_READY_LO		0x0160U
#define GPU_L2_READY_HI		0x0164U
#define GPU_SHADER_PWRON_LO	0x0180U
#define GPU_SHADER_PWRON_HI	0x0184U
#define GPU_TILER_PWRON_LO	0x0190U
#define GPU_TILER_PWRON_HI	0x0194U
#define GPU_L2_PWRON_LO		0x01a0U
#define GPU_L2_PWRON_HI		0x01a4U
#define GPU_COHERENCY_FEATURES	0x0300U
#define GPU_COHERENCY_ENABLE	0x0304U

#define GPU_IRQ_FAULT		(1U << 0)
#define GPU_IRQ_MULTIPLE_FAULT	(1U << 7)
#define GPU_IRQ_RESET_COMPLETED	(1U << 8)
#define GPU_IRQ_CLEAN_CACHES_COMPLETED (1U << 17)
#define GPU_IRQ_MASK_ALL	0x00030781U
#define GPU_CMD_SOFT_RESET	1U
#define GPU_CMD_HARD_RESET	2U
#define GPU_CMD_CLEAN_INV_CACHES 8U

#define GPU_MMU_BASE		0x2000U
#define MMU_INT_RAWSTAT		(GPU_MMU_BASE + 0x000U)
#define MMU_INT_CLEAR		(GPU_MMU_BASE + 0x004U)
#define MMU_INT_MASK		(GPU_MMU_BASE + 0x008U)
#define MMU_AS0_BASE		(GPU_MMU_BASE + 0x400U)
#define AS_TRANSTAB_LO		(MMU_AS0_BASE + 0x00U)
#define AS_TRANSTAB_HI		(MMU_AS0_BASE + 0x04U)
#define AS_MEMATTR_LO		(MMU_AS0_BASE + 0x08U)
#define AS_MEMATTR_HI		(MMU_AS0_BASE + 0x0cU)
#define AS_LOCKADDR_LO		(MMU_AS0_BASE + 0x10U)
#define AS_LOCKADDR_HI		(MMU_AS0_BASE + 0x14U)
#define AS_COMMAND		(MMU_AS0_BASE + 0x18U)
#define AS_FAULTSTATUS		(MMU_AS0_BASE + 0x1cU)
#define AS_FAULTADDRESS_LO	(MMU_AS0_BASE + 0x20U)
#define AS_FAULTADDRESS_HI	(MMU_AS0_BASE + 0x24U)
#define AS_STATUS		(MMU_AS0_BASE + 0x28U)
#define AS_TRANSCFG_LO		(MMU_AS0_BASE + 0x30U)
#define AS_TRANSCFG_HI		(MMU_AS0_BASE + 0x34U)
#define AS_STATUS_AS_ACTIVE	1U
#define AS_COMMAND_UPDATE	1U
#define AS_COMMAND_LOCK		2U
#define AS_COMMAND_FLUSH_PT	4U
#define AS_COMMAND_FLUSH_MEM	5U

#define JS_BASE			0x1800U
#define JS_STRIDE		0x0080U
#define JS_HEAD_LO(n)		(JS_BASE + ((n) * JS_STRIDE) + 0x00U)
#define JS_HEAD_HI(n)		(JS_BASE + ((n) * JS_STRIDE) + 0x04U)
#define JS_TAIL_LO(n)		(JS_BASE + ((n) * JS_STRIDE) + 0x08U)
#define JS_TAIL_HI(n)		(JS_BASE + ((n) * JS_STRIDE) + 0x0cU)
#define JS_CONFIG(n)		(JS_BASE + ((n) * JS_STRIDE) + 0x18U)
#define JS_COMMAND(n)		(JS_BASE + ((n) * JS_STRIDE) + 0x20U)
#define JS_HEAD_NEXT_LO(n)	(JS_BASE + ((n) * JS_STRIDE) + 0x40U)
#define JS_HEAD_NEXT_HI(n)	(JS_BASE + ((n) * JS_STRIDE) + 0x44U)
#define JS_AFFINITY_NEXT_LO(n)	(JS_BASE + ((n) * JS_STRIDE) + 0x50U)
#define JS_AFFINITY_NEXT_HI(n)	(JS_BASE + ((n) * JS_STRIDE) + 0x54U)
#define JS_CONFIG_NEXT(n)	(JS_BASE + ((n) * JS_STRIDE) + 0x58U)
#define JS_COMMAND_NEXT(n)	(JS_BASE + ((n) * JS_STRIDE) + 0x60U)
#define JS_FLUSH_ID_NEXT(n)	(JS_BASE + ((n) * JS_STRIDE) + 0x70U)
#define JS_STATUS(n)		(JS_BASE + ((n) * JS_STRIDE) + 0x24U)
#define JS_COMMAND_START	1U
#define JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE	(3U << 8)
#define JS_CONFIG_JOB_CHAIN_FLAG		(1U << 11)
#define JS_CONFIG_END_FLUSH_CLEAN		(1U << 12)
#define JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE	(3U << 12)
#define JS_CONFIG_ENABLE_FLUSH_REDUCTION	(1U << 14)
#define JS_CONFIG_THREAD_PRI(n)	((u32)(n) << 16)
#define JS_FEATURE_SET_VALUE_JOB	(1U << 2)
#define JS_FEATURE_COMPUTE_JOB		(1U << 4)
#define JS_FEATURE_FRAGMENT_JOB		(1U << 9)

#define JOB_INT_RAWSTAT		0x1000U
#define JOB_INT_CLEAR		0x1004U
#define JOB_INT_MASK		0x1008U
#define JOB_INT_JS_STATE	0x1010U

#define INFRACFG_BASE		0x10001000ULL
#define INFRA_TOPAXI_PROTECTEN_SET	0x02a0U
#define INFRA_TOPAXI_PROTECTSTA1	0x0228U
#define INFRA_TOPAXI_PROTECTEN_CLR	0x02a4U
#define INFRA_TOPAXI_PROTECTEN_1_SET	0x02a8U
#define INFRA_TOPAXI_PROTECTSTA1_1	0x0258U
#define INFRA_TOPAXI_PROTECTEN_1_CLR	0x02acU
#define INFRA_TOPAXI_PROTECTEN_2_SET	0x0714U
#define INFRA_TOPAXI_PROTECTSTA1_2	0x0724U
#define INFRA_TOPAXI_PROTECTEN_2_CLR	0x0718U
#define MFG1_PROT_STEP2_1_MASK		(1U << 7)
#define MFG1_PROT_STEP2_0_MASK		((1U << 21) | (1U << 22))
#define MFG1_PROT_STEP1_1_MASK		((1U << 5) | (1U << 6))
#define MFG1_PROT_STEP1_0_MASK		(1U << 21)

#define MFG_GLOBAL_CON			0x0b0U
#define MFG_ASYNC_CON			0x0020U
#define MFG_ASYNC_CON_2			0x0024U
#define MFG_QCHANNEL_CON		0x0b4U
#define MFG_I2M_PROTECTOR_CFG_00	0x0f60U

#define PWRAP_BASE		0x10026000ULL
#define PWRAP_WACS2_CMD		0x0880U
#define PWRAP_SWINF_2_RDATA	0x0894U
#define PWRAP_WACS2_VLDCLR	0x08a4U
#define PWRAP_WACS2_RDATA	0x08a8U
#define PWRAP_TIMEOUT_US	10000U
#define PWRAP_FSM_READY		0U
#define PWRAP_FSM_READ_DONE	6U

#define MT6366_SWCID		0x000aU
#define MT6366_BUCK_VGPU_CON0	0x1508U
#define MT6366_BUCK_VGPU_DBG1	0x1520U
#define MT6366_BUCK_VGPU_ELR0	0x1526U
#define MT6366_LDO_VSRAM_GPU_CON0 0x1bc8U
#define MT6366_LDO_VSRAM_GPU_DBG0 0x1be2U
#define MT6366_LDO_VSRAM_CON3	0x1bf4U

#define DOMAIN_COUNT		4U

static const u32 domain_bit[DOMAIN_COUNT] = {
	1U << 2, 1U << 3, 1U << 4, 1U << 5
};

static const u32 domain_ctl[DOMAIN_COUNT] = {
	MFG0_CTL, MFG1_CTL, MFG2_CTL, MFG3_CTL
};

static u32
rd32(u64 addr)
{
	u32 v = *(volatile u32 *)addr;

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
wr32(u64 addr, u32 value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile u32 *)addr = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static u64
ticks(void)
{
	u64 t;

	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static u64
timer_freq(void)
{
	u64 f;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 19200000U;
}

static void
delay_us(u64 us)
{
	u64 start = ticks();
	u64 delta = us * timer_freq() / 1000000U;

	while (ticks() - start < delta)
		;
}

static u32
pwrap_rd(u32 off)
{
	u32 v = *(volatile u32 *)(PWRAP_BASE + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
pwrap_wr(u32 off, u32 value)
{
	wr32(PWRAP_BASE + off, value);
}

static int
pwrap_wait_state(u32 want)
{
	u64 start = ticks();
	u64 timeout = (u64)PWRAP_TIMEOUT_US * timer_freq() / 1000000U;

	for (;;) {
		u32 v = pwrap_rd(PWRAP_WACS2_RDATA);

		if (((v >> 1) & 7U) == want)
			return 0;
		if (ticks() - start >= timeout)
			return -1;
		delay_us(1);
	}
}

static int
pwrap_read16(u16 adr, u16 *val)
{
	if (pwrap_wait_state(PWRAP_FSM_READY))
		return -1;
	pwrap_wr(PWRAP_WACS2_CMD, adr);
	if (pwrap_wait_state(PWRAP_FSM_READ_DONE)) {
		pwrap_wr(PWRAP_WACS2_VLDCLR, 1);
		return -2;
	}
	*val = (u16)(pwrap_rd(PWRAP_SWINF_2_RDATA) & 0xffffU);
	pwrap_wr(PWRAP_WACS2_VLDCLR, 1);
	return 0;
}

static int
log_pmic(void)
{
	u16 swcid = 0, con0 = 0, elr = 0, dbg1 = 0;
	u16 ldo_con0 = 0, ldo_vsel = 0, ldo_dbg0 = 0;
	int vgpu_on = 0, vsram_on = 0;
	int rc;

	logf("GPU: PMIC rail probe via PWRAP");
	rc = pwrap_read16(MT6366_SWCID, &swcid);
	logf("GPU: PMIC swcid=%x rc=%d", (u64)swcid, (u64)(rc < 0 ? -rc : rc));
	if (rc != 0) {
		logf("GPU: PWRAP/PMIC not readable; rail state unknown");
		return 0;
	}

	if (pwrap_read16(MT6366_BUCK_VGPU_CON0, &con0) == 0 &&
	    pwrap_read16(MT6366_BUCK_VGPU_ELR0, &elr) == 0 &&
	    pwrap_read16(MT6366_BUCK_VGPU_DBG1, &dbg1) == 0) {
		u32 vsel = elr & 0x7fU;
		u32 mv = 500U + (vsel * 6250U) / 1000U;

		vgpu_on = (con0 & 1U) != 0;
		logf("GPU: VGPU con0=%x en=%d elr=%x dbg1=%x",
		     (u64)con0, (u64)vgpu_on, (u64)elr, (u64)dbg1);
		logf("GPU: VGPU vsel=%d approx=%dmV", (u64)vsel, (u64)mv);
	} else {
		logf("GPU: VGPU register read failed");
	}

	if (pwrap_read16(MT6366_LDO_VSRAM_GPU_CON0, &ldo_con0) == 0 &&
	    pwrap_read16(MT6366_LDO_VSRAM_CON3, &ldo_vsel) == 0 &&
	    pwrap_read16(MT6366_LDO_VSRAM_GPU_DBG0, &ldo_dbg0) == 0) {
		vsram_on = (ldo_con0 & 1U) != 0;
		logf("GPU: VSRAM_GPU con0=%x en=%d vsel=%x dbg0=%x",
		     (u64)ldo_con0, (u64)vsram_on, (u64)ldo_vsel,
		     (u64)ldo_dbg0);
	} else {
		logf("GPU: VSRAM_GPU register read failed");
	}

	return vgpu_on && vsram_on;
}

static int
domain_on(u32 bit)
{
	u32 status = rd32(SCPSYS_BASE + SCPSYS_PWR_STATUS);
	u32 status2 = rd32(SCPSYS_BASE + SCPSYS_PWR_STATUS_2ND);

	return (status & bit) != 0 && (status2 & bit) != 0;
}

static void
log_domains(void)
{
	u32 status = rd32(SCPSYS_BASE + SCPSYS_PWR_STATUS);
	u32 status2 = rd32(SCPSYS_BASE + SCPSYS_PWR_STATUS_2ND);

	logf("GPU: SCP status=%x/%x", (u64)status, (u64)status2);
	for (u32 i = 0; i < DOMAIN_COUNT; i++) {
		logf("GPU: MFG%d ctl=%x sta=%d", (u64)i,
		     (u64)rd32(SCPSYS_BASE + domain_ctl[i]),
		     (u64)domain_on(domain_bit[i]));
	}
}

/*
 * The read-only path must not write topckgen.  The known safe parent for a
 * probe is mfg_ref_sel=0 (tck_26m_mx9) with mfg_pll_sel=0.  If the bootloader
 * left the GPU on a PLL, bail out instead of touching the GPU clock tree.
 */
static int
clock_is_safe(void)
{
	u32 top = rd32(TOPCKGEN_BASE + TOP_CLK_CFG_4);
	u32 ref = (top >> TOP_MFG_REF_SHIFT) & 3U;
	u32 pll = (top >> TOP_MFG_PLL_SHIFT) & 1U;
	u32 pdn = (top >> 23) & 1U;

	logf("GPU: topcfg4=%x ref=%d pll=%d pdn=%d", (u64)top, (u64)ref,
	     (u64)pll, (u64)pdn);
	if (ref != 0U || pll != 0U || pdn != 0U) {
		logf("GPU: clock tree not in safe 26 MHz mode; skip GPU access");
		return 0;
	}
	return 1;
}

#if GPU_POWER_TEST
/*
 * Experimental power-on path, derived from scpsys_power_on().  This is not
 * used by the default read-only build.  It intentionally turns the GPU clock
 * back to the 26 MHz reference before touching MFG, so a probe cannot depend
 * on the GPU PLL or on gpufreq having run.
 */
static void
select_probe_clock(void)
{
	u32 top = rd32(TOPCKGEN_BASE + TOP_CLK_CFG_4);
	u32 ref = (top >> TOP_MFG_REF_SHIFT) & 3U;
	u32 pll = (top >> TOP_MFG_PLL_SHIFT) & 1U;

	if (ref != 0U || (top & TOP_MFG_REF_GATE_BIT)) {
		wr32(TOPCKGEN_BASE + TOP_CLK_CFG_4_CLR,
		     TOP_MFG_REF_MASK | TOP_MFG_REF_GATE_BIT);
		wr32(TOPCKGEN_BASE + TOP_CLK_CFG_UPDATE,
		     TOP_MFG_REF_UPDATE_BIT);
		delay_us(20);
	}
	if (pll != 0U) {
		wr32(TOPCKGEN_BASE + TOP_CLK_CFG_4_CLR, TOP_MFG_PLL_BIT);
		delay_us(20);
	}

	logf("GPU: power probe topcfg4=%x", (u64)rd32(TOPCKGEN_BASE + TOP_CLK_CFG_4));
}

static int
power_on_domain(u32 index)
{
	u32 ctl = SCPSYS_BASE + domain_ctl[index];
	u32 bit = domain_bit[index];
	u32 val;
	u32 i;

	if (domain_on(bit)) {
		logf("GPU: MFG%d already on", (u64)index);
		return 0;
	}

	logf("GPU: powering MFG%d", (u64)index);
	val = rd32(ctl);
	val |= PWR_ON_BIT;
	wr32(ctl, val);
	val |= PWR_ON_2ND_BIT;
	wr32(ctl, val);

	for (i = 0; i < 1000U; i++) {
		if (domain_on(bit))
			break;
		delay_us(10);
	}
	if (!domain_on(bit)) {
		logf("GPU: MFG%d power ack timeout", (u64)index);
		return -1;
	}

	delay_us(100);
	val = rd32(ctl);
	val &= ~PWR_CLK_DIS_BIT;
	wr32(ctl, val);
	val &= ~PWR_ISO_BIT;
	wr32(ctl, val);
	val |= PWR_RST_B_BIT;
	wr32(ctl, val);

	val = rd32(ctl);
	val &= ~PWR_SRAM_PDN_BIT;
	wr32(ctl, val);
	for (i = 0; i < 1000U; i++) {
		if (!(rd32(ctl) & PWR_SRAM_PDN_ACK_BIT))
			break;
		delay_us(10);
	}
	if (rd32(ctl) & PWR_SRAM_PDN_ACK_BIT) {
		logf("GPU: MFG%d SRAM ack timeout", (u64)index);
		return -2;
	}

	logf("GPU: MFG%d on ctl=%x", (u64)index, (u64)rd32(ctl));
	return 0;
}
#endif

#if !GPU_POWER_TEST
static int
mfg_gate_on(void)
{
	u32 cg = rd32(MFG_TOP_BASE + MFG_TOP_CG_STATUS);

	logf("GPU: MFG_CG status=%x", (u64)cg);
	if (cg & MFG_TOP_CG_BIT) {
		logf("GPU: MFG gate is off; read-only probe leaves it untouched");
		return 0;
	}
	return 1;
}
#else
static int
mfg_gate_on(void)
{
	u32 cg = rd32(MFG_TOP_BASE + MFG_TOP_CG_STATUS);

	logf("GPU: MFG_CG status=%x", (u64)cg);
	if (cg & MFG_TOP_CG_BIT) {
		/* MTK gate enable is a write-1-to-clear operation. */
		wr32(MFG_TOP_BASE + MFG_TOP_CG_CLR, MFG_TOP_CG_BIT);
		delay_us(20);
		cg = rd32(MFG_TOP_BASE + MFG_TOP_CG_STATUS);
		logf("GPU: MFG_CG after enable=%x", (u64)cg);
	}
	return (cg & MFG_TOP_CG_BIT) ? 0 : 1;
}
#endif

#if GPU_POWER_TEST
/*
 * The GPU test below is deliberately kept in the experimental GPU_POWER_TEST
 * build.  It releases the MFG1 bus protection, enables the external GPU
 * clocks/QChannels, resets the GPU, powers its internal L2/shader/tiler
 * domains, installs an AArch64-4K page table in AS0, and submits a single
 * Write Value job.  A second job then targets an unmapped GPU VA so the
 * fault path can be observed without involving another device.
 */

#define GPU_VA_BASE		0x40000000ULL
#define GPU_VA_JOB		GPU_VA_BASE
#define GPU_VA_OUT		(GPU_VA_BASE + 0x1000ULL)
#define GPU_VA_JOB2		(GPU_VA_BASE + 0x2000ULL)
#define GPU_VA_OUT2		(GPU_VA_BASE + 0x3000ULL)
#define GPU_VA_BO_A		(GPU_VA_BASE + 0x40000ULL)
#define GPU_VA_BO_B		(GPU_VA_BASE + 0x42000ULL)
#define GPU_VA_JOB_POOL		(GPU_VA_BASE + 0x60000ULL)
#define GPU_VA_SHADER		(GPU_VA_BASE + 0x80000ULL)
#define GPU_VA_COMPUTE_JOB	(GPU_VA_BASE + 0x81000ULL)
#define GPU_VA_TLS		(GPU_VA_BASE + 0x82000ULL)
#define GPU_VA_SHADER_NOP	(GPU_VA_BASE + 0x83000ULL)
#define GPU_VA_SHADER_DESC	(GPU_VA_BASE + 0x84000ULL)
#define GPU_VA_FBD		(GPU_VA_BASE + 0x85000ULL)
#define GPU_VA_FRAG_JOB		(GPU_VA_BASE + 0x86000ULL)
#define GPU_VA_TILER_CTX	(GPU_VA_BASE + 0x87000ULL)
#define GPU_VA_TILER_HEAP	(GPU_VA_BASE + 0x88000ULL)
#define GPU_VA_TILER_DATA	(GPU_VA_BASE + 0x89000ULL)
#define GPU_TILER_HEAP_SIZE	0x40000ULL	/* 256 KiB */
#define GPU_VA_FB		(GPU_VA_BASE + 0x4000ULL)
#define GPU_VA_FAULT		(GPU_VA_BASE + 0x100000ULL)
#define GPU_WAIT_SHORT_US	200000ULL
#define GPU_WAIT_JOB_US		5000000ULL
#define BO_POOL_PAGES		16

#define MMU_PTE_TABLE		(0x400ULL | 0x3ULL)
#define MMU_PTE_PAGE		(0x400ULL | 0x300ULL | 0x50ULL | 0x7ULL)
#define MMU_MEMATTR_NC_ALL	0x4c4c4c4c4c4c4c4cULL
#define MMU_TRANSCFG_AARCH64_4K	(6ULL | (2ULL << 24) | (1ULL << 30) | (7ULL << 6))

static u64 pt_l0[512] __attribute__((aligned(4096)));
static u64 pt_l1[512] __attribute__((aligned(4096)));
static u64 pt_l2[512] __attribute__((aligned(4096)));
static u64 pt_l3[512] __attribute__((aligned(4096)));
static u64 job_page[512] __attribute__((aligned(4096)));
static u64 out_page[512] __attribute__((aligned(4096)));
static u64 job_page2[512] __attribute__((aligned(4096)));
static u64 out_page2[512] __attribute__((aligned(4096)));
static u64 shader_page[512] __attribute__((aligned(4096)));
static u64 compute_job_page[512] __attribute__((aligned(4096)));
static u64 tls_page[512] __attribute__((aligned(4096)));
static u64 shader_nop_page[512] __attribute__((aligned(4096)));
static u64 shader_desc_page[512] __attribute__((aligned(4096)));
static u64 fbd_page[512] __attribute__((aligned(4096)));
static u64 frag_job_page[512] __attribute__((aligned(4096)));
static u64 tiler_ctx_page[512] __attribute__((aligned(4096)));
static u64 tiler_heap_page[512] __attribute__((aligned(4096)));
/* 256 KiB tiler heap, matching the descriptor's 256 KiB chunk size. */
static u64 tiler_data_page[32768] __attribute__((aligned(4096)));
/* 64x64 RGBA8 render target: four contiguous 4 KiB pages. */
static u64 fb_page[2048] __attribute__((aligned(4096)));
/* Set once the final clear-only fragment job has produced the readback that
 * the visual diagnostic should display. */
static u32 gpu_visual_valid;

/* Hand-assembled Valhall shader:
 *   r2:r3 = 0x40003000 (out_page2 GPU VA)
 *   r0:r1 = 0x0123456789abcdef
 *   STORE.i64 [r2:r3], r0:r1
 *   wait for slot 0, end
 */
static const u64 compute_shader_code[] = {
	0x0110c240003000c0ULL,
	0x0110c300000000c0ULL,
	0x0110c089abcdefc0ULL,
	0x0110c101234567c0ULL,
	0x0061400428000042ULL,
	0x0800c00000000000ULL,
	0x7800c00000000000ULL,
};

/* Smallest possible Valhall shader: NOP.end.  If this faults, the
 * compute descriptor/pointer is wrong, not the store sequence. */
static const u64 compute_nop_code[] = {
	0x7800c00000000000ULL,
};
static u64 bo_pool[BO_POOL_PAGES][512] __attribute__((aligned(4096)));
static u32 bo_used;
static u32 bo_job_index;
static u64 bo_job_pages[16];
/* Set for one fragment run to exercise the vendor r32p1 JS_CONFIG
 * flush-reduction and job-chain bits. */
static u32 gpu_js_vendor_cfg;
/* Nonzero only while the Write Value flush-mode matrix is running. */
static u32 gpu_js_config_override;

static void build_write_value_job(u64 *page, u64 target, u64 value);
static int run_write_value_job(int slot, u64 job_va, u32 js_present,
			       u32 *raw_out);

static u64
phys_addr(const void *p)
{
	return (u64)(unsigned long)p;
}

static void
dsb_sy(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static int
wait_reg_mask(u64 addr, u32 mask, u32 value, u64 timeout_us)
{
	u64 start = ticks();
	u64 timeout = timeout_us * timer_freq() / 1000000ULL;

	for (;;) {
		if ((rd32(addr) & mask) == value)
			return 0;
		if (ticks() - start >= timeout)
			return -1;
		delay_us(10);
	}
}

static int
wait_present64(u32 lo_reg, u32 hi_reg, u64 want, u64 timeout_us)
{
	u64 start = ticks();
	u64 timeout = timeout_us * timer_freq() / 1000000ULL;

	for (;;) {
		u64 val = (u64)rd32(lo_reg) | ((u64)rd32(hi_reg) << 32);

		if (val == want)
			return 0;
		if (ticks() - start >= timeout)
			return -1;
		delay_us(10);
	}
}

static void
log_bus_protect(const char *when)
{
	u32 en = rd32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_SET);
	u32 en1 = rd32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_1_SET);
	u32 en2 = rd32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_2_SET);
	u32 sta = rd32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTSTA1);
	u32 sta1 = rd32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTSTA1_1);
	u32 sta2 = rd32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTSTA1_2);

	logf("GPU: busprot %s en=%x/%x/%x", when, (u64)en, (u64)en1,
	     (u64)en2);
	logf("GPU: busprot %s sta=%x/%x/%x", when, (u64)sta, (u64)sta1,
	     (u64)sta2);
}

static void
release_mfg_bus_protect(void)
{
	log_bus_protect("before");

	/* Vendor gpufreq power-on order for MFG1 (mt6789).  The MTCMOS
	 * BUS_PROT_IGN policy means these clear writes do not need an ack;
	 * keep a short settle delay and log the status for diagnosis. */
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_2_CLR,
	     MFG1_PROT_STEP2_1_MASK);
	delay_us(100);
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_CLR,
	     MFG1_PROT_STEP2_0_MASK);
	delay_us(100);
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_2_CLR,
	     MFG1_PROT_STEP1_1_MASK);
	delay_us(100);
	wr32(INFRACFG_BASE + INFRA_TOPAXI_PROTECTEN_1_CLR,
	     MFG1_PROT_STEP1_0_MASK);
	delay_us(100);

	log_bus_protect("after");
}

static void
configure_external_cg(void)
{
	u32 v;

	logf("GPU: extcg before 20=%x 24=%x b0=%x b4=%x f60=%x",
	     (u64)rd32(MFG_TOP_BASE + MFG_ASYNC_CON),
	     (u64)rd32(MFG_TOP_BASE + MFG_ASYNC_CON_2),
	     (u64)rd32(MFG_TOP_BASE + MFG_GLOBAL_CON),
	     (u64)rd32(MFG_TOP_BASE + MFG_QCHANNEL_CON),
	     (u64)rd32(MFG_TOP_BASE + MFG_I2M_PROTECTOR_CFG_00));

	/* The exact vendor mt6789 external CG/QChannel sequence. */
	v = rd32(MFG_TOP_BASE + MFG_ASYNC_CON);
	v |= (1U << 22) | (1U << 23) | (1U << 24) | (1U << 25);
	wr32(MFG_TOP_BASE + MFG_ASYNC_CON, v);

	v = rd32(MFG_TOP_BASE + MFG_GLOBAL_CON);
	v &= ~((1U << 10) | (1U << 9));
	wr32(MFG_TOP_BASE + MFG_GLOBAL_CON, v);

	v = rd32(MFG_TOP_BASE + MFG_QCHANNEL_CON);
	v |= (1U << 4);
	wr32(MFG_TOP_BASE + MFG_QCHANNEL_CON, v);

	v = rd32(MFG_TOP_BASE + MFG_GLOBAL_CON);
	v &= ~(1U << 8);
	v |= (1U << 19);
	wr32(MFG_TOP_BASE + MFG_GLOBAL_CON, v);

	v = rd32(MFG_TOP_BASE + MFG_ASYNC_CON_2);
	v |= (1U << 0);
	wr32(MFG_TOP_BASE + MFG_ASYNC_CON_2, v);

	v = rd32(MFG_TOP_BASE + MFG_I2M_PROTECTOR_CFG_00);
	v |= (1U << 0);
	wr32(MFG_TOP_BASE + MFG_I2M_PROTECTOR_CFG_00, v);

	logf("GPU: extcg after 20=%x 24=%x b0=%x b4=%x f60=%x",
	     (u64)rd32(MFG_TOP_BASE + MFG_ASYNC_CON),
	     (u64)rd32(MFG_TOP_BASE + MFG_ASYNC_CON_2),
	     (u64)rd32(MFG_TOP_BASE + MFG_GLOBAL_CON),
	     (u64)rd32(MFG_TOP_BASE + MFG_QCHANNEL_CON),
	     (u64)rd32(MFG_TOP_BASE + MFG_I2M_PROTECTOR_CFG_00));
}

static int
gpu_soft_reset(void)
{
	int rc;

	logf("GPU: reset begin status=%x raw=%x",
	     (u64)rd32(GPU_BASE + GPU_STATUS),
	     (u64)rd32(GPU_BASE + GPU_IRQ_RAWSTAT));

	wr32(GPU_BASE + GPU_INT_MASK, 0);
	wr32(GPU_BASE + GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
	wr32(GPU_BASE + GPU_CMD, GPU_CMD_SOFT_RESET);
	rc = wait_reg_mask(GPU_BASE + GPU_IRQ_RAWSTAT,
			   GPU_IRQ_RESET_COMPLETED,
			   GPU_IRQ_RESET_COMPLETED, 100000ULL);
	if (rc != 0) {
		logf("GPU: soft reset timeout raw=%x; hard reset",
		     (u64)rd32(GPU_BASE + GPU_IRQ_RAWSTAT));
		wr32(GPU_BASE + GPU_CMD, GPU_CMD_HARD_RESET);
		rc = wait_reg_mask(GPU_BASE + GPU_IRQ_RAWSTAT,
				   GPU_IRQ_RESET_COMPLETED,
				   GPU_IRQ_RESET_COMPLETED, 500000ULL);
	}
	if (rc != 0) {
		logf("GPU: reset failed raw=%x", (u64)rd32(GPU_BASE + GPU_IRQ_RAWSTAT));
		return -1;
	}

	logf("GPU: reset raw=%x status=%x",
	     (u64)rd32(GPU_BASE + GPU_IRQ_RAWSTAT),
	     (u64)rd32(GPU_BASE + GPU_STATUS));
	wr32(GPU_BASE + GPU_INT_CLEAR, GPU_IRQ_MASK_ALL);
	wr32(GPU_BASE + GPU_INT_MASK, 0);
	return 0;
}

/* Vendor r32p1 uses GPU_COMMAND_CLEAN_INV_CACHES when it needs to flush
 * the L2 (mali_cci_flush_l2).  The job-slot flush bits do not
 * necessarily cover the L2 writeback path used by clean-pixel writes. */
static int
gpu_clean_inv_caches(void)
{
	wr32(GPU_BASE + GPU_INT_CLEAR, GPU_IRQ_CLEAN_CACHES_COMPLETED);
	wr32(GPU_BASE + GPU_CMD, GPU_CMD_CLEAN_INV_CACHES);
	return wait_reg_mask(GPU_BASE + GPU_IRQ_RAWSTAT,
			     GPU_IRQ_CLEAN_CACHES_COMPLETED,
			     GPU_IRQ_CLEAN_CACHES_COMPLETED, 100000ULL);
}

/* Vendor r32p1 applies these global GPU configuration values during
 * power-up.  The G57 (tBAx) has BASE_HW_FEATURE_IDVS_GROUP_SIZE and its
 * device tree does not override the default group size, so the vendor
 * programs 0xF.  The tBAx L2_MMU_CONFIG limits are zero (no read/write
 * throttle).  Leaving these at reset defaults can make the tiler/job
 * manager behave differently from the vendor driver. */
static void
gpu_apply_hw_config(void)
{
	u32 v;

	v = rd32(GPU_BASE + GPU_JM_CONFIG);
	v &= ~(0x3fU << 16);
	v |= (0x0fU << 16);
	wr32(GPU_BASE + GPU_JM_CONFIG, v);

	v = rd32(GPU_BASE + GPU_L2_MMU_CONFIG);
	v &= ~((0x3fU << 5) | (0x1fU << 12));
	wr32(GPU_BASE + GPU_L2_MMU_CONFIG, v);

	logf("GPU: hwcfg sc=%x tiler=%x jm=%x l2mmu=%x l2cfg=%x mem=%x l2feat=%x",
	     (u64)rd32(GPU_BASE + GPU_SHADER_CONFIG),
	     (u64)rd32(GPU_BASE + GPU_TILER_CONFIG),
	     (u64)rd32(GPU_BASE + GPU_JM_CONFIG),
	     (u64)rd32(GPU_BASE + GPU_L2_MMU_CONFIG),
	     (u64)rd32(GPU_BASE + GPU_L2_CONFIG),
	     (u64)rd32(GPU_BASE + GPU_MEM_FEATURES),
	     (u64)rd32(GPU_BASE + GPU_L2_FEATURES));
}

static int
gpu_internal_power_on(void)
{
	u64 l2 = (u64)rd32(GPU_BASE + GPU_L2_PRESENT_LO) |
		 ((u64)rd32(GPU_BASE + GPU_L2_PRESENT_HI) << 32);
	u64 shader = (u64)rd32(GPU_BASE + GPU_SHADER_PRESENT_LO) |
		     ((u64)rd32(GPU_BASE + GPU_SHADER_PRESENT_HI) << 32);
	u64 tiler = (u64)rd32(GPU_BASE + GPU_TILER_PRESENT_LO) |
		    ((u64)rd32(GPU_BASE + GPU_TILER_PRESENT_HI) << 32);
	u64 core_mask = ~0ULL;
	u64 l2_on, shader_on;
	int rc;

	if (l2 != 0 && (l2 & (l2 - 1)) != 0)
		core_mask = ~(l2 - 1) & (l2 - 2);
	l2_on = l2 & core_mask;
	shader_on = shader & core_mask;

	logf("GPU: present l2=%x shader=%x tiler=%x", l2, shader, tiler);
	if (l2_on == 0) {
		logf("GPU: no L2 core present; cannot power GPU");
		return -1;
	}

	wr32(GPU_BASE + GPU_L2_PWRON_LO, (u32)l2_on);
	wr32(GPU_BASE + GPU_L2_PWRON_HI, (u32)(l2_on >> 32));
	rc = wait_present64(GPU_BASE + GPU_L2_READY_LO,
			    GPU_BASE + GPU_L2_READY_HI, l2_on, 200000ULL);
	if (rc != 0) {
		logf("GPU: L2 power-on timeout ready=%x",
		     (u64)rd32(GPU_BASE + GPU_L2_READY_LO));
		return -2;
	}

	wr32(GPU_BASE + GPU_SHADER_PWRON_LO, (u32)shader_on);
	wr32(GPU_BASE + GPU_SHADER_PWRON_HI, (u32)(shader_on >> 32));
	rc = wait_present64(GPU_BASE + GPU_SHADER_READY_LO,
			    GPU_BASE + GPU_SHADER_READY_HI, shader_on,
			    200000ULL);
	if (rc != 0) {
		logf("GPU: shader power-on timeout ready=%x",
		     (u64)rd32(GPU_BASE + GPU_SHADER_READY_LO));
		return -3;
	}

	wr32(GPU_BASE + GPU_TILER_PWRON_LO, (u32)tiler);
	wr32(GPU_BASE + GPU_TILER_PWRON_HI, (u32)(tiler >> 32));
	rc = wait_present64(GPU_BASE + GPU_TILER_READY_LO,
			    GPU_BASE + GPU_TILER_READY_HI, tiler, 200000ULL);
	if (rc != 0) {
		logf("GPU: tiler power-on timeout ready=%x",
		     (u64)rd32(GPU_BASE + GPU_TILER_READY_LO));
		return -4;
	}

	gpu_apply_hw_config();

	logf("GPU: internal power ready l2=%x shader=%x tiler=%x",
	     (u64)rd32(GPU_BASE + GPU_L2_READY_LO),
	     (u64)rd32(GPU_BASE + GPU_SHADER_READY_LO),
	     (u64)rd32(GPU_BASE + GPU_TILER_READY_LO));
	return 0;
}

static void
gpu_set_coherency(void)
{
	u32 feat = rd32(GPU_BASE + GPU_COHERENCY_FEATURES);

	/* The system is non-coherent here: AXI memory attributes are
	 * non-cacheable and the CPU runs with its caches off. */
	wr32(GPU_BASE + GPU_COHERENCY_ENABLE, 31);
	logf("GPU: coherency feat=%x set=none", (u64)feat);
}

static int
build_page_tables(void)
{
	u64 l0 = phys_addr(pt_l0);
	u64 l1 = phys_addr(pt_l1);
	u64 l2 = phys_addr(pt_l2);
	u64 l3 = phys_addr(pt_l3);
	u64 job = phys_addr(job_page);
	u64 out = phys_addr(out_page);
	u64 job2 = phys_addr(job_page2);
	u64 out2 = phys_addr(out_page2);
	u64 shader = phys_addr(shader_page);
	u64 cjob = phys_addr(compute_job_page);
	u64 tls = phys_addr(tls_page);
	u64 snop = phys_addr(shader_nop_page);
	u64 sdesc = phys_addr(shader_desc_page);
	u64 fbd = phys_addr(fbd_page);
	u64 fjob = phys_addr(frag_job_page);
	u64 tctx = phys_addr(tiler_ctx_page);
	u64 theap = phys_addr(tiler_heap_page);
	u64 tdata = phys_addr(tiler_data_page);
	u64 fb = phys_addr(fb_page);

	if ((l0 | l1 | l2 | l3 | job | out | job2 | out2 | shader | cjob |
	     tls | snop | sdesc | fbd | fjob | tctx | theap | tdata | fb) &
	    0xfffULL) {
		logf("GPU: page table/BO is not 4K aligned: %x %x %x",
		     l0, job, out);
		return -1;
	}

	for (u32 i = 0; i < 512; i++) {
		pt_l0[i] = 0;
		pt_l1[i] = 0;
		pt_l2[i] = 0;
		pt_l3[i] = 0;
	}
	for (u32 i = 0; i < 512; i++) {
		job_page[i] = 0;
		out_page[i] = 0;
		job_page2[i] = 0;
		out_page2[i] = 0;
		shader_page[i] = 0;
		compute_job_page[i] = 0;
		tls_page[i] = 0;
		shader_nop_page[i] = 0;
		shader_desc_page[i] = 0;
		fbd_page[i] = 0;
		frag_job_page[i] = 0;
		tiler_ctx_page[i] = 0;
		tiler_heap_page[i] = 0;
	}
	for (u32 i = 0; i < 2048; i++)
		fb_page[i] = 0;
	for (u32 i = 0; i < 32768; i++)
		tiler_data_page[i] = 0;

	pt_l0[0] = l1 | MMU_PTE_TABLE;
	pt_l1[1] = l2 | MMU_PTE_TABLE;
	pt_l2[0] = l3 | MMU_PTE_TABLE;
	pt_l3[0] = job | MMU_PTE_PAGE;
	pt_l3[1] = out | MMU_PTE_PAGE;
	pt_l3[2] = job2 | MMU_PTE_PAGE;
	pt_l3[3] = out2 | MMU_PTE_PAGE;
	pt_l3[0x80] = shader | MMU_PTE_PAGE;
	pt_l3[0x81] = cjob | MMU_PTE_PAGE;
	pt_l3[0x82] = tls | MMU_PTE_PAGE;
	pt_l3[0x83] = snop | MMU_PTE_PAGE;
	pt_l3[0x84] = sdesc | MMU_PTE_PAGE;
	pt_l3[0x85] = fbd | MMU_PTE_PAGE;
	pt_l3[0x86] = fjob | MMU_PTE_PAGE;
	pt_l3[0x87] = tctx | MMU_PTE_PAGE;
	pt_l3[0x88] = theap | MMU_PTE_PAGE;
	for (u32 i = 0; i < 64; i++)
		pt_l3[0x89 + i] = (tdata + i * 0x1000ULL) | MMU_PTE_PAGE;
	pt_l3[4] = fb | MMU_PTE_PAGE;
	pt_l3[5] = (fb + 0x1000ULL) | MMU_PTE_PAGE;
	pt_l3[6] = (fb + 0x2000ULL) | MMU_PTE_PAGE;
	pt_l3[7] = (fb + 0x3000ULL) | MMU_PTE_PAGE;
	dsb_sy();

	logf("GPU: PT pa=%x/%x/%x/%x bo=%x/%x/%x/%x",
	     l0, l1, l2, l3, job, out, job2, out2);
	logf("GPU: PTE l0=%x l1=%x l2=%x l3=%x/%x/%x/%x",
	     pt_l0[0], pt_l1[1], pt_l2[0], pt_l3[0], pt_l3[1], pt_l3[2],
	     pt_l3[3]);
	logf("GPU: PTE shader=%x/%x/%x/%x/%x", pt_l3[0x80], pt_l3[0x81],
	     pt_l3[0x82], pt_l3[0x83], pt_l3[0x84]);
	logf("GPU: PTE fbd=%x fjob=%x fb=%x/%x/%x/%x", pt_l3[0x85],
	     pt_l3[0x86], pt_l3[4], pt_l3[5], pt_l3[6], pt_l3[7]);
	logf("GPU: PTE tiler ctx=%x heap=%x data=%x/%x", pt_l3[0x87],
	     pt_l3[0x88], pt_l3[0x89], pt_l3[0xc8]);
	return 0;
}

static int
as_wait_idle(u64 timeout_us)
{
	return wait_reg_mask(GPU_BASE + AS_STATUS, AS_STATUS_AS_ACTIVE, 0,
			     timeout_us);
}

static int
as_command(u32 cmd)
{
	if (as_wait_idle(GPU_WAIT_SHORT_US) != 0)
		return -1;
	wr32(GPU_BASE + AS_COMMAND, cmd);
	/*
	 * AS_STATUS can still read idle for a few GPU cycles after the
	 * command is written.  Let the command start before the next
	 * idle poll, otherwise a following MMU register write can be
	 * issued while the command is still in flight.
	 */
	delay_us(10);
	return 0;
}

static int
mmu_enable_as0(void)
{
	u64 tt = phys_addr(pt_l0);
	u32 cfg = (u32)MMU_TRANSCFG_AARCH64_4K;

	logf("GPU: AS0 enable tt=%x cfg=%x", tt, (u64)cfg);

	wr32(GPU_BASE + MMU_INT_MASK, 0);
	wr32(GPU_BASE + MMU_INT_CLEAR, 0xffffffffU);

	/*
	 * A Mali MMU operation applies to the region in AS_LOCKADDR.  Lock
	 * the whole VA range before the initial FLUSH_MEM.  This matches
	 * both panfrost_mmu_enable() and kbase_mmu_hw_do_operation().
	 */
	wr32(GPU_BASE + AS_LOCKADDR_LO, 0x3fU);
	wr32(GPU_BASE + AS_LOCKADDR_HI, 0);
	if (as_command(AS_COMMAND_LOCK) != 0) {
		logf("GPU: AS0 lock timeout status=%x",
		     (u64)rd32(GPU_BASE + AS_STATUS));
		return -1;
	}

	/* Flush the GPU L2/L1 before installing a new translation table. */
	if (as_command(AS_COMMAND_FLUSH_MEM) != 0) {
		logf("GPU: AS0 flush_mem timeout status=%x",
		     (u64)rd32(GPU_BASE + AS_STATUS));
		return -2;
	}
	if (as_wait_idle(GPU_WAIT_SHORT_US) != 0) {
		logf("GPU: AS0 flush_mem complete timeout status=%x",
		     (u64)rd32(GPU_BASE + AS_STATUS));
		return -2;
	}

	wr32(GPU_BASE + AS_TRANSTAB_LO, (u32)tt);
	wr32(GPU_BASE + AS_TRANSTAB_HI, (u32)(tt >> 32));
	wr32(GPU_BASE + AS_MEMATTR_LO, (u32)MMU_MEMATTR_NC_ALL);
	wr32(GPU_BASE + AS_MEMATTR_HI, (u32)(MMU_MEMATTR_NC_ALL >> 32));
	wr32(GPU_BASE + AS_TRANSCFG_LO, cfg);
	wr32(GPU_BASE + AS_TRANSCFG_HI, 0);
	dsb_sy();

	if (as_command(AS_COMMAND_UPDATE) != 0) {
		logf("GPU: AS0 update timeout status=%x",
		     (u64)rd32(GPU_BASE + AS_STATUS));
		return -3;
	}
	if (as_wait_idle(GPU_WAIT_SHORT_US) != 0) {
		logf("GPU: AS0 update complete timeout status=%x",
		     (u64)rd32(GPU_BASE + AS_STATUS));
		return -3;
	}

	logf("GPU: AS0 tt=%x/%x mm=%x cf=%x/%x st=%x",
	     (u64)rd32(GPU_BASE + AS_TRANSTAB_LO),
	     (u64)rd32(GPU_BASE + AS_TRANSTAB_HI),
	     (u64)rd32(GPU_BASE + AS_MEMATTR_LO),
	     (u64)rd32(GPU_BASE + AS_TRANSCFG_LO),
	     (u64)rd32(GPU_BASE + AS_TRANSCFG_HI),
	     (u64)rd32(GPU_BASE + AS_STATUS));
	return 0;
}

static u64
bo_page_pa(u32 index)
{
	return phys_addr(bo_pool[index]);
}

static void
bo_reset(void)
{
	bo_used = 0;
	bo_job_index = 0;
	for (u32 i = 0; i < BO_POOL_PAGES; i++)
		for (u32 j = 0; j < 512; j++)
			bo_pool[i][j] = 0;
	for (u32 i = 0; i < 16; i++)
		bo_job_pages[i] = 0;
	dsb_sy();
}

static u64
bo_alloc(u32 pages)
{
	if (!pages || pages > BO_POOL_PAGES)
		return 0;

	for (u32 start = 0; start + pages <= BO_POOL_PAGES; start++) {
		u32 mask = ((1U << pages) - 1U) << start;

		if (bo_used & mask)
			continue;
		bo_used |= mask;
		for (u32 i = 0; i < pages; i++)
			for (u32 j = 0; j < 512; j++)
				bo_pool[start + i][j] = 0;
		dsb_sy();
		return bo_page_pa(start);
	}
	return 0;
}

static void
bo_free(u64 pa, u32 pages)
{
	u64 base = bo_page_pa(0);
	u32 start;

	if (!pa || pa < base || ((pa - base) & 0xfffULL))
		return;
	start = (u32)((pa - base) >> 12);
	if (!pages || start + pages > BO_POOL_PAGES)
		return;

	for (u32 i = 0; i < pages; i++) {
		bo_used &= ~(1U << (start + i));
		for (u32 j = 0; j < 512; j++)
			bo_pool[start + i][j] = 0;
	}
	dsb_sy();
}

static u64 *
l3_entry(u64 va)
{
	if (va < GPU_VA_BASE || va >= GPU_VA_BASE + 0x200000ULL)
		return 0;
	return &pt_l3[(va - GPU_VA_BASE) >> 12];
}

static int
mmu_flush_pt(void)
{
	wr32(GPU_BASE + AS_LOCKADDR_LO, 0x3fU);
	wr32(GPU_BASE + AS_LOCKADDR_HI, 0);
	if (as_command(AS_COMMAND_LOCK) != 0)
		return -1;
	if (as_command(AS_COMMAND_FLUSH_PT) != 0)
		return -2;
	if (as_wait_idle(GPU_WAIT_SHORT_US) != 0)
		return -3;
	return 0;
}

static int
mmu_map_page(u64 va, u64 pa)
{
	u64 *entry = l3_entry(va);

	if (!entry || (va & 0xfffULL) || (pa & 0xfffULL))
		return -1;
	*entry = pa | MMU_PTE_PAGE;
	dsb_sy();
	return mmu_flush_pt();
}

static int
mmu_unmap_page(u64 va)
{
	u64 *entry = l3_entry(va);

	if (!entry || (va & 0xfffULL))
		return -1;
	*entry = 0;
	dsb_sy();
	return mmu_flush_pt();
}

static int
bo_write_check(int slot, u32 js_present, const char *tag,
	       u64 gpu_va, u64 phys, u64 value)
{
	u64 job_pa, job_va;
	u32 raw, job_index;
	u64 got;
	int rc;

	if (bo_job_index >= 16) {
		logf("GPU: BO %s job index exhausted", tag);
		return -1;
	}
	job_index = bo_job_index++;
	job_pa = bo_alloc(1);
	if (!job_pa) {
		logf("GPU: BO %s job alloc failed", tag);
		return -1;
	}
	job_va = GPU_VA_JOB_POOL + ((u64)job_index << 12);
	if (mmu_map_page(job_va, job_pa) != 0) {
		logf("GPU: BO %s job map failed va=%x pa=%x", tag, job_va,
		     job_pa);
		bo_free(job_pa, 1);
		return -1;
	}

	build_write_value_job((u64 *)job_pa, gpu_va, value);
	rc = run_write_value_job(slot, job_va, js_present, &raw);
	__asm__ volatile("dmb oshld" ::: "memory");
	got = *(volatile u64 *)phys;
	logf("GPU: BO %s job=%x pa=%x rc=%d raw=%x got=%w want=%w", tag,
	     job_va, job_pa, (u64)rc, (u64)raw, got, value);
	bo_job_pages[job_index] = job_pa;

	if (mmu_unmap_page(job_va) != 0)
		logf("GPU: BO %s job unmap failed", tag);

	if (rc != 0)
		return -1;
	if (got != value)
		return -2;
	return 0;
}

static int
gpu_bo_test(int slot, u32 js_present)
{
	u64 a, b, c, d, old;
	int rc;

	bo_reset();
	a = bo_alloc(2);
	b = bo_alloc(1);
	if (!a || !b) {
		logf("GPU: BO alloc failed");
		return -1;
	}
	logf("GPU: BO alloc a=%x b=%x", a, b);

	if (mmu_map_page(GPU_VA_BO_A, a) != 0)
		return -2;
	if (mmu_map_page(GPU_VA_BO_A + 0x1000ULL, a + 0x1000ULL) != 0)
		return -3;
	if (mmu_map_page(GPU_VA_BO_B, b) != 0)
		return -4;
	logf("GPU: BO mapped a=%x/%x b=%x", a, a + 0x1000ULL, b);

	rc = bo_write_check(slot, js_present, "a0", GPU_VA_BO_A, a,
			    0x1111111111111111ULL);
	if (rc != 0)
		return -5;
	rc = bo_write_check(slot, js_present, "a1", GPU_VA_BO_A + 0x1000ULL,
			    a + 0x1000ULL, 0x2222222222222222ULL);
	if (rc != 0)
		return -6;
	rc = bo_write_check(slot, js_present, "b", GPU_VA_BO_B, b,
			    0x3333333333333333ULL);
	if (rc != 0)
		return -7;

	c = bo_alloc(1);
	if (!c)
		return -8;
	if (mmu_unmap_page(GPU_VA_BO_A + 0x1000ULL) != 0)
		return -9;
	if (mmu_map_page(GPU_VA_BO_A + 0x1000ULL, c) != 0)
		return -10;
	rc = bo_write_check(slot, js_present, "c", GPU_VA_BO_A + 0x1000ULL,
			    c, 0x4444444444444444ULL);
	if (rc != 0)
		return -11;

	__asm__ volatile("dmb oshld" ::: "memory");
	old = *(volatile u64 *)(a + 0x1000ULL);
	logf("GPU: BO remap old=%w new=%w", old, *(volatile u64 *)c);
	if (old != 0x2222222222222222ULL)
		return -12;
	logf("GPU: BO REMAP PASS");

	bo_free(a, 2);
	d = bo_alloc(2);
	logf("GPU: BO reuse a=%x d=%x", a, d);
	if (!d)
		return -13;
	if (d != a)
		logf("GPU: BO reuse base differs");
	if (mmu_map_page(GPU_VA_BO_A, d) != 0)
		return -14;
	if (mmu_map_page(GPU_VA_BO_A + 0x1000ULL, d + 0x1000ULL) != 0)
		return -15;
	rc = bo_write_check(slot, js_present, "d0", GPU_VA_BO_A, d,
			    0x5555555555555555ULL);
	if (rc != 0)
		return -16;
	rc = bo_write_check(slot, js_present, "d1", GPU_VA_BO_A + 0x1000ULL,
			    d + 0x1000ULL, 0x6666666666666666ULL);
	if (rc != 0)
		return -17;

	if (mmu_unmap_page(GPU_VA_BO_A) != 0)
		return -18;
	if (mmu_unmap_page(GPU_VA_BO_A + 0x1000ULL) != 0)
		return -19;
	if (mmu_unmap_page(GPU_VA_BO_B) != 0)
		return -20;

	for (u32 i = 0; i < bo_job_index; i++)
		if (bo_job_pages[i])
			bo_free(bo_job_pages[i], 1);
	bo_free(d, 2);
	bo_free(b, 1);
	bo_free(c, 1);
	logf("GPU: BO TEST PASS");
	return 0;
}

static int
find_write_value_slot(u32 present)
{
	int found = -1;

	for (u32 slot = 0; slot < 16; slot++) {
		if (!(present & (1U << slot)))
			continue;
		u32 feat = rd32(GPU_BASE + GPU_JS_FEATURES(slot));

		logf("GPU: JS%d feature=%x", (u64)slot, (u64)feat);
		if (found < 0 && (feat & JS_FEATURE_SET_VALUE_JOB))
			found = (int)slot;
	}
	return found;
}

static void
build_write_value_job(u64 *page, u64 target, u64 value)
{
	volatile u32 *h = (volatile u32 *)page;

	for (u32 i = 0; i < 1024; i++)
		h[i] = 0;

	/* Job Header: Type=Write value, Index=1, no next job. */
	h[4] = (2U << 1) | (1U << 16);
	h[5] = 0;
	h[6] = 0;
	h[7] = 0;

	/* Write Value Payload. */
	h[8] = (u32)target;
	h[9] = (u32)(target >> 32);
	h[10] = 7;		/* Immediate 64 */
	h[12] = (u32)value;
	h[13] = (u32)(value >> 32);
	dsb_sy();
}

static void
build_compute_job(u64 *page, u64 shader_va, u64 tls_va)
{
	volatile u32 *h = (volatile u32 *)page;

	for (u32 i = 0; i < 1024; i++)
		h[i] = 0;

	/* Job Header: Type=Compute, Index=1, no next job. */
	h[4] = (4U << 1) | (1U << 16);
	h[5] = 0;
	h[6] = 0;
	h[7] = 0;

	/* Compute Payload. */
	h[8] = 0;		/* workgroup size 1x1x1 */
	h[9] = 1U | (2U << 14);	/* task increment 1, axis Z */
	h[10] = 1;		/* workgroup count X */
	h[11] = 1;		/* workgroup count Y */
	h[12] = 1;		/* workgroup count Z */
	h[13] = 0;		/* offset X */
	h[14] = 0;		/* offset Y */
	h[15] = 0;		/* offset Z */

	/* Shader Environment starts at payload word 8 (job word 16). */
	h[16] = 0;		/* attribute offset */
	h[17] = 0;		/* FAU count */
	h[24] = 0;		/* resources low */
	h[25] = 0;		/* resources high */
	h[26] = (u32)shader_va;
	h[27] = (u32)(shader_va >> 32);
	h[28] = (u32)tls_va;
	h[29] = (u32)(tls_va >> 32);
	h[30] = 0;		/* FAU low */
	h[31] = 0;		/* FAU high */
	dsb_sy();
}

/* Valhall Shader Program descriptor (32 bytes, 32-byte aligned).
 * The compute job's Shader Environment points here; the descriptor's
 * Binary field points at the instruction stream. */
static void
build_shader_desc(u64 *page, u64 code_va)
{
	volatile u32 *d = (volatile u32 *)page;

	for (u32 i = 0; i < 1024; i++)
		d[i] = 0;

	/* Type=Shader (8), Stage=Compute (1), 32 registers/thread (2). */
	d[0] = 8U | (1U << 4) | (2U << 30);
	d[1] = 0;		/* Preload r48-r63 */
	d[2] = (u32)code_va;
	d[3] = (u32)(code_va >> 32);
	dsb_sy();
}

/* Valhall v9 tiler heap descriptor (32 bytes): Buffer type 9, Tiler heap
 * type 2, 256 KiB chunks, dynamic partitioning, then size/base/bottom/top. */
static void
build_tiler_heap_desc(u64 *page, u64 heap_va, u64 heap_size)
{
	volatile u32 *d = (volatile u32 *)page;

	for (u32 i = 0; i < 512; i++)
		d[i] = 0;

	d[0] = 9U | (2U << 4) | (0U << 8) | (0U << 10);
	d[1] = (u32)heap_size;
	d[2] = (u32)heap_va;
	d[3] = (u32)(heap_va >> 32);
	d[4] = (u32)heap_va;
	d[5] = (u32)(heap_va >> 32);
	d[6] = (u32)(heap_va + heap_size);
	d[7] = (u32)((heap_va + heap_size) >> 32);
	dsb_sy();
}

/* Valhall v9 tiler context (192 bytes, 48 words).  Mesa always emits one,
 * even for a clear-only fragment job; hierarchy_mask 0xff matches its
 * "max_levels >= 8" path on G57. */
static void
build_tiler_context(u64 *page, u64 heap_desc_va, u32 width, u32 height)
{
	volatile u32 *d = (volatile u32 *)page;

	for (u32 i = 0; i < 512; i++)
		d[i] = 0;

	d[2] = 0xffU;		/* hierarchy mask, single-sample pattern */
	d[3] = (width - 1U) | ((height - 1U) << 16);
	d[6] = (u32)heap_desc_va;
	d[7] = (u32)(heap_desc_va >> 32);
	dsb_sy();
}

/* Valhall v9 framebuffer descriptor for one linear RGBA8 render target.
 * Layout (from Mesa v9.xml):
 *   offset   0: Framebuffer Parameters (16 words)
 *   offset  64: Framebuffer Padding (16 words, zero)
 *   offset 128: Render Target 0 (16 words)
 * The fragment job's Framebuffer field is the FBD VA with bit 0 set
 * (Type=1) and Render target count - 1 in bits 2..5. */
static void
build_framebuffer_desc(u64 *page, u64 fb_va, u64 tiler_va, u32 width,
		       u32 height, u32 clear_color)
{
	volatile u32 *d = (volatile u32 *)page;
	u32 wm1 = width - 1U;
	u32 hm1 = height - 1U;

	for (u32 i = 0; i < 1024; i++)
		d[i] = 0;

	d[8] = wm1 | (hm1 << 16);
	d[9] = 0;
	d[10] = wm1 | (hm1 << 16);
	/* 1 sample, tie-break MINUS_180_IN_0_OUT, 16x16 effective tiles
	 * (log2 of 256 pixels = 8), point-sprite origin max Y, one render
	 * target, 1 KiB colour-buffer allocation (4 bytes per pixel * 256
	 * pixels, rounded up to the required 1 KiB). */
	d[11] = (2U << 6) | (8U << 9) | (1U << 15) | (1U << 24);
	/* Default depth format used by Mesa even without a depth surface. */
	d[12] = 1U << 16;	/* z_internal_format = D24 */

	/* Tiler context pointer; Mesa emits one even for clear-only jobs. */
	d[14] = (u32)tiler_va;
	d[15] = (u32)(tiler_va >> 32);

	/* Render target 0. */
	d[32] = 1U << 26;	/* internal format R8G8B8A8 */
	d[33] = 1U | (19U << 3) | (2U << 8) | (1U << 15) |
		(0x688U << 16) | (1U << 31);
	d[40] = (u32)fb_va;
	d[41] = (u32)(fb_va >> 32);
	d[42] = width * 4U;	/* row stride */
	d[43] = 0;
	d[44] = clear_color;
	d[45] = clear_color;
	d[46] = clear_color;
	d[47] = clear_color;
	dsb_sy();
}

static void
build_fragment_job(u64 *page, u64 fbd_va, u32 width, u32 height)
{
	volatile u32 *h = (volatile u32 *)page;
	u32 tag = (u32)(fbd_va | 1ULL);
	u32 tx = (width - 1U) >> 4;
	u32 ty = (height - 1U) >> 4;

	for (u32 i = 0; i < 1024; i++)
		h[i] = 0;

	/* Job Header: Type=Fragment, Index=1, no next job. */
	h[4] = (9U << 1) | (1U << 16);

	/* Fragment Job Payload: bounds are in 16x16 tiles. */
	h[8] = 0;
	h[9] = tx | (ty << 16);
	h[10] = tag;
	h[11] = 0;
	dsb_sy();
}

static int
find_fragment_slot(u32 present)
{
	for (u32 slot = 0; slot < 16; slot++) {
		u32 feat;

		if (!(present & (1U << slot)))
			continue;
		feat = rd32(GPU_BASE + GPU_JS_FEATURES(slot));
		if (feat & JS_FEATURE_FRAGMENT_JOB)
			return (int)slot;
	}
	return -1;
}

static void
frag_dump(const char *tag, const u64 *page)
{
	const volatile u32 *d = (const volatile u32 *)page;

	for (u32 i = 0; i < 48; i++)
		logf("GPU: FRAG %s%d=%x", tag, (u64)i, (u64)d[i]);
}

static void
frag_fault_diag(int slot, u32 raw)
{
	u32 mraw = rd32(GPU_BASE + MMU_INT_RAWSTAT);
	u32 fstat = rd32(GPU_BASE + AS_FAULTSTATUS);
	u64 faddr = (u64)rd32(GPU_BASE + AS_FAULTADDRESS_LO) |
		    ((u64)rd32(GPU_BASE + AS_FAULTADDRESS_HI) << 32);
	u32 gstat = rd32(GPU_BASE + GPU_FAULT_STATUS);
	u64 gaddr = (u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_LO) |
		    ((u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_HI) << 32);
	u32 graw = rd32(GPU_BASE + GPU_IRQ_RAWSTAT);

	logf("GPU: FRAGMENT fault raw=%x mraw=%x fstat=%x faddr=%w",
	     (u64)raw, (u64)mraw, (u64)fstat, faddr);
	logf("GPU: FRAGMENT fault gstat=%x gaddr=%w gint=%x", (u64)gstat,
	     gaddr, (u64)graw);
	logf("GPU: FRAGMENT js cfg=%x cmd=%x status=%x astat=%x",
	     (u64)rd32(GPU_BASE + JS_CONFIG(slot)),
	     (u64)rd32(GPU_BASE + JS_COMMAND(slot)),
	     (u64)rd32(GPU_BASE + JS_STATUS(slot)),
	     (u64)rd32(GPU_BASE + AS_STATUS));

	wr32(GPU_BASE + MMU_INT_CLEAR, mraw);
	wr32(GPU_BASE + GPU_INT_CLEAR, graw & (GPU_IRQ_FAULT |
					       GPU_IRQ_MULTIPLE_FAULT));
	wr32(GPU_BASE + JOB_INT_CLEAR, raw);
}

/* Compact occupancy report for the 64x64 test target: total non-zero
 * pixels, first zero, last non-zero, and a 4x4 grid of per-16x16-tile
 * non-zero counts.  This distinguishes "one row", "one tile", and
 * "scattered/tiled layout" failures without dumping 4096 pixels. */
static void
frag_pattern(const volatile u32 *fb, u32 width, u32 height)
{
	u32 nz = 0;
	u32 first_zero = width * height;
	u32 last_nz = 0;
	u32 tiles[16];

	for (u32 i = 0; i < 16; i++)
		tiles[i] = 0;

	for (u32 y = 0; y < height; y++) {
		for (u32 x = 0; x < width; x++) {
			u32 i = y * width + x;

			if (fb[i] != 0) {
				nz++;
				last_nz = i;
				tiles[(y >> 4) * 4 + (x >> 4)]++;
			} else if (first_zero == width * height) {
				first_zero = i;
			}
		}
	}

	logf("GPU: FRAGMENT pattern nz=%d first_zero=%d last_nz=%d",
	     (u64)nz, (u64)first_zero, (u64)last_nz);
	logf("GPU: FRAGMENT tiles %d %d %d %d / %d %d %d %d",
	     (u64)tiles[0], (u64)tiles[1], (u64)tiles[2], (u64)tiles[3],
	     (u64)tiles[4], (u64)tiles[5], (u64)tiles[6], (u64)tiles[7]);
	logf("GPU: FRAGMENT tiles %d %d %d %d / %d %d %d %d",
	     (u64)tiles[8], (u64)tiles[9], (u64)tiles[10], (u64)tiles[11],
	     (u64)tiles[12], (u64)tiles[13], (u64)tiles[14], (u64)tiles[15]);
}

/* 64-row bitmap: bit y is set if row y is entirely clear, entirely
 * sentinel, or mixed.  This pins down a tiled/skipped writeback. */
static void
frag_rowmask(const volatile u32 *fb, u32 width, u32 height, u32 clear,
	     u32 sentinel)
{
	u64 cmask = 0, smask = 0, mmask = 0;

	for (u32 y = 0; y < height; y++) {
		u32 nc = 0, ns = 0;

		for (u32 x = 0; x < width; x++) {
			u32 v = fb[y * width + x];

			if (v == clear)
				nc++;
			else if (v == sentinel)
				ns++;
		}
		if (nc == width)
			cmask |= 1ULL << y;
		else if (ns == width)
			smask |= 1ULL << y;
		else
			mmask |= 1ULL << y;
	}
	logf("GPU: FRAGMENT rowmask clear=%w sent=%w mixed=%w", cmask,
	     smask, mmask);
}

/* Compact clear probe used to compare tile-size behaviour across
 * framebuffer sizes without dumping the whole descriptor. */
static int
gpu_fragment_run(int slot, u32 width, u32 height, const char *tag, u32 cfg)
{
	const u32 clear = 0xff0000ffU;
	const u32 sentinel = 0x11223344U;
	volatile u32 *fb = (volatile u32 *)fb_page;
	u32 raw;
	u32 nclear = 0, nsent = 0, nother = 0;
	u64 cmask = 0, smask = 0, mmask = 0;
	int rc;

	build_tiler_heap_desc(tiler_heap_page, GPU_VA_TILER_DATA,
			      GPU_TILER_HEAP_SIZE);
	build_tiler_context(tiler_ctx_page, GPU_VA_TILER_HEAP, width, height);
	for (u32 i = 0; i < width * height; i++)
		fb[i] = sentinel;
	for (u32 i = 0; i < 512; i++) {
		fbd_page[i] = 0;
		frag_job_page[i] = 0;
	}
	dsb_sy();
	build_framebuffer_desc(fbd_page, GPU_VA_FB, GPU_VA_TILER_CTX, width,
			       height, clear);
	build_fragment_job(frag_job_page, GPU_VA_FBD, width, height);
	{
		volatile u32 *d = (volatile u32 *)fbd_page;

		switch (cfg) {
		case 1:	/* 2 KiB colour-buffer allocation */
			d[11] = (d[11] & ~(0xffU << 24)) | (2U << 24);
			break;
		case 2:	/* 4 KiB colour-buffer allocation */
			d[11] = (d[11] & ~(0xffU << 24)) | (4U << 24);
			break;
		case 3:	/* dithering off */
			d[33] &= ~(1U << 15);
			break;
		case 4:	/* clean-pixel write off */
			d[33] &= ~(1U << 31);
			break;
		case 5:	/* empty-tile write on */
			d[12] |= 1U << 29;
			break;
		case 6:	/* effective tile size 128 pixels */
			d[11] = (d[11] & ~(0xfU << 9)) | (7U << 9);
			break;
		case 7:	/* effective tile size 512 pixels */
			d[11] = (d[11] & ~(0xfU << 9)) | (9U << 9);
			break;
		case 8:	/* tiled U-interleaved writeback */
			d[33] = (d[33] & ~(0xfU << 8)) | (1U << 8);
			break;
		case 9:	/* dither off, clean off, empty-tile write on */
			d[33] &= ~((1U << 15) | (1U << 31));
			d[12] |= 1U << 29;
			break;
		case 10:	/* sample count 2 (4x MSAA) */
			d[11] = (d[11] & ~0x7U) | (2U << 0);
			break;
		}
		dsb_sy();
	}
	wr32(GPU_BASE + MMU_INT_CLEAR, 0xffffffffU);
	rc = run_write_value_job(slot, GPU_VA_FRAG_JOB, 1U << slot, &raw);
	if (rc != 0)
		return rc;

	{
		u32 mraw = rd32(GPU_BASE + MMU_INT_RAWSTAT);

		if (mraw) {
			u32 fstat = rd32(GPU_BASE + AS_FAULTSTATUS);
			u64 faddr = (u64)rd32(GPU_BASE + AS_FAULTADDRESS_LO) |
				    ((u64)rd32(GPU_BASE + AS_FAULTADDRESS_HI) << 32);
			u32 gstat = rd32(GPU_BASE + GPU_FAULT_STATUS);
			u64 gaddr = (u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_LO) |
				    ((u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_HI) << 32);

			logf("GPU: FRAGPROBE %s MMUFAULT mraw=%x fstat=%x faddr=%w gstat=%x gaddr=%w",
			     tag, (u64)mraw, (u64)fstat, faddr, (u64)gstat, gaddr);
			wr32(GPU_BASE + MMU_INT_CLEAR, mraw);
		}
	}

	{
		int crc = gpu_clean_inv_caches();

		logf("GPU: FRAGPROBE %s cacheclean rc=%d raw=%x", tag,
		     (u64)crc, (u64)rd32(GPU_BASE + GPU_IRQ_RAWSTAT));
	}

	__asm__ volatile("dmb oshld" ::: "memory");
	for (u32 y = 0; y < height; y++) {
		u32 nc = 0, ns = 0;

		for (u32 x = 0; x < width; x++) {
			u32 v = fb[y * width + x];

			if (v == clear) {
				nc++;
				nclear++;
			} else if (v == sentinel) {
				ns++;
				nsent++;
			} else {
				nother++;
			}
		}
		if (nc == width)
			cmask |= 1ULL << y;
		else if (ns == width)
			smask |= 1ULL << y;
		else
			mmask |= 1ULL << y;
	}
	logf("GPU: FRAGPROBE %s w=%d h=%d rc=%d raw=%x clear=%d sent=%d other=%d",
	     tag, (u64)width, (u64)height, (u64)rc, (u64)raw, (u64)nclear,
	     (u64)nsent, (u64)nother);
	logf("GPU: FRAGPROBE %s rowmask clear=%w sent=%w mixed=%w", tag,
	     cmask, smask, mmask);
	return 0;
}

/* Unique-pixel sentinel.  A row that the GPU never wrote keeps its own
 * 0x80000000|(y<<8)|x value; a row written from a different source row
 * keeps a foreign y.  This distinguishes a skipped writeback from a
 * permuted preload/writeback, which a uniform sentinel cannot. */
static int
gpu_fragment_unique(int slot, u32 width, u32 height, const char *tag, u32 cfg)
{
	const u32 clear = 0xff0000ffU;
	volatile u32 *fb = (volatile u32 *)fb_page;
	u32 raw = 0;
	u32 clearrows = 0, ownrows = 0, foreignrows = 0, otherrows = 0;
	u64 ownmask = 0, foreignmask = 0, othermask = 0;
	int rc;

	build_tiler_heap_desc(tiler_heap_page, GPU_VA_TILER_DATA,
			      GPU_TILER_HEAP_SIZE);
	build_tiler_context(tiler_ctx_page, GPU_VA_TILER_HEAP, width, height);
	for (u32 y = 0; y < height; y++) {
		for (u32 x = 0; x < width; x++)
			fb[y * width + x] = 0x80000000U | (y << 8) | x;
	}
	for (u32 i = 0; i < 512; i++) {
		fbd_page[i] = 0;
		frag_job_page[i] = 0;
	}
	dsb_sy();
	build_framebuffer_desc(fbd_page, GPU_VA_FB, GPU_VA_TILER_CTX, width,
			       height, clear);
	build_fragment_job(frag_job_page, GPU_VA_FBD, width, height);
	{
		volatile u32 *d = (volatile u32 *)fbd_page;

		switch (cfg) {
		case 1:	/* writeback disabled, linear */
			d[33] &= ~1U;
			break;
		case 2:	/* Mesa discard RT: no write enable, tiled wb */
			d[33] &= ~1U;
			d[33] = (d[33] & ~(0x1fU << 3)) | (0U << 3);
			d[33] = (d[33] & ~(0xfU << 8)) | (1U << 8);
			d[33] &= ~(0xfffU << 16);
			break;
		case 3:	/* vendor JS_CONFIG: flush reduction + job chain */
			gpu_js_vendor_cfg = 1;
			break;
		}
		dsb_sy();
	}
	wr32(GPU_BASE + MMU_INT_CLEAR, 0xffffffffU);
	rc = run_write_value_job(slot, GPU_VA_FRAG_JOB, 1U << slot, &raw);
	gpu_js_vendor_cfg = 0;
	if (rc != 0) {
		logf("GPU: FRAGUNIQ %s submit failed rc=%d raw=%x", tag,
		     (u64)rc, (u64)raw);
		return rc;
	}
	{
		int crc = gpu_clean_inv_caches();

		logf("GPU: FRAGUNIQ %s cacheclean rc=%d raw=%x", tag,
		     (u64)crc, (u64)rd32(GPU_BASE + GPU_IRQ_RAWSTAT));
	}
	__asm__ volatile("dmb oshld" ::: "memory");
	for (u32 y = 0; y < height; y++) {
		u32 nc = 0;

		for (u32 x = 0; x < width; x++) {
			if (fb[y * width + x] == clear)
				nc++;
		}
		if (nc == width) {
			clearrows++;
			continue;
		}
		if (nc != 0) {
			otherrows++;
			othermask |= 1ULL << y;
			continue;
		}
		{
			u32 v0 = fb[y * width];

			if ((v0 & 0x80000000U) &&
			    (((v0 >> 8) & 0xffU) == y)) {
				ownrows++;
				ownmask |= 1ULL << y;
			} else if (v0 & 0x80000000U) {
				foreignrows++;
				foreignmask |= 1ULL << y;
			} else {
				otherrows++;
				othermask |= 1ULL << y;
			}
		}
	}
	logf("GPU: FRAGUNIQ %s w=%d h=%d rc=%d raw=%x clearrows=%d",
	     tag, (u64)width, (u64)height, (u64)rc, (u64)raw,
	     (u64)clearrows);
	logf("GPU: FRAGUNIQ %s ownrows=%d foreignrows=%d otherrows=%d",
	     tag, (u64)ownrows, (u64)foreignrows, (u64)otherrows);
	logf("GPU: FRAGUNIQ %s own=%w foreign=%w other=%w", tag, ownmask,
	     foreignmask, othermask);
	for (u32 y = 0; y < height; y++) {
		u32 v0 = fb[y * width];

		if (v0 != clear && v0 != (0x80000000U | (y << 8)))
			logf("GPU: FRAGUNIQ %s row=%d v0=%x sy=%d", tag,
			     (u64)y, (u64)v0, (u64)((v0 >> 8) & 0xffU));
	}
	return 0;
}

static int
gpu_fragment_test(u32 js_present)
{
	const u32 width = 64;
	const u32 height = 64;
	const u32 clear = 0xff0000ffU;	/* RGBA (1, 0, 0, 1) */
	const u32 sentinel = 0x11223344U;
	volatile u32 *fb = (volatile u32 *)fb_page;
	u32 raw;
	int slot, rc;

	slot = find_fragment_slot(js_present);
	if (slot < 0) {
		logf("GPU: FRAGMENT skipped (no fragment slot)");
		return 0;
	}
	logf("GPU: FRAGMENT slot=%d", (u64)slot);

	/* Size sweep: locate whether the missing rows are per 16x16 tile
	 * or a property of the whole framebuffer. */
	gpu_fragment_run(slot, 16, 16, "16x16", 0);
	gpu_fragment_run(slot, 16, 32, "16x32", 0);
	gpu_fragment_run(slot, 32, 16, "32x16", 0);
	gpu_fragment_run(slot, 32, 32, "32x32", 0);
	gpu_fragment_run(slot, 64, 64, "64x64", 0);
	gpu_fragment_run(slot, 64, 64, "alloc2", 1);
	gpu_fragment_run(slot, 64, 64, "alloc4", 2);
	gpu_fragment_run(slot, 64, 64, "nodither", 3);
	gpu_fragment_run(slot, 64, 64, "noclean", 4);
	gpu_fragment_run(slot, 64, 64, "emptyw", 5);
	gpu_fragment_run(slot, 64, 64, "tile7", 6);
	gpu_fragment_run(slot, 64, 64, "tile9", 7);
	gpu_fragment_run(slot, 64, 64, "tiledwb", 8);
	gpu_fragment_run(slot, 64, 64, "alloff", 9);
	gpu_fragment_run(slot, 64, 64, "msaa4", 10);

	gpu_fragment_unique(slot, 16, 16, "uniq16", 0);
	gpu_fragment_unique(slot, 64, 64, "uniq64", 0);
	gpu_fragment_unique(slot, 64, 64, "uniq-nowrite", 1);
	gpu_fragment_unique(slot, 64, 64, "uniq-discard", 2);
	gpu_fragment_unique(slot, 64, 64, "uniq-vendorjs", 3);

	build_tiler_heap_desc(tiler_heap_page, GPU_VA_TILER_DATA,
			      GPU_TILER_HEAP_SIZE);
	build_tiler_context(tiler_ctx_page, GPU_VA_TILER_HEAP, width, height);

	for (u32 i = 0; i < 4096; i++)
		fb[i] = sentinel;
	for (u32 i = 0; i < 512; i++) {
		fbd_page[i] = 0;
		frag_job_page[i] = 0;
	}
	dsb_sy();

	build_framebuffer_desc(fbd_page, GPU_VA_FB, GPU_VA_TILER_CTX, width,
			       height, clear);
	build_fragment_job(frag_job_page, GPU_VA_FBD, width, height);

	logf("GPU: FRAGMENT fb=%x fbd=%x job=%x %dx%d clear=%x",
	     GPU_VA_FB, GPU_VA_FBD, GPU_VA_FRAG_JOB, (u64)width,
	     (u64)height, (u64)clear);
	frag_dump("FBD", fbd_page);
	frag_dump("JOB", frag_job_page);

	rc = run_write_value_job(slot, GPU_VA_FRAG_JOB, js_present, &raw);
	logf("GPU: FRAGMENT rc=%d raw=%x js=%x state=%x", (u64)rc,
	     (u64)raw, (u64)rd32(GPU_BASE + JS_STATUS(slot)),
	     (u64)rd32(GPU_BASE + JOB_INT_JS_STATE));
	/* Preserve the final clear-only readback for the visual diagnostic even
	 * when the job failed: sentinel rows themselves are useful on screen. */
	gpu_visual_valid = 1;
	if (rc != 0) {
		frag_fault_diag(slot, raw);
		return -1;
	}

	__asm__ volatile("dmb oshld" ::: "memory");
	logf("GPU: FRAGMENT px0=%x px1=%x pxlast=%x", (u64)fb[0],
	     (u64)fb[1], (u64)fb[width * height - 1U]);
	logf("GPU: FRAGMENT row0=%x/%x/%x/%x row1=%x/%x/%x/%x",
	     (u64)fb[0], (u64)fb[1], (u64)fb[2], (u64)fb[3],
	     (u64)fb[width], (u64)fb[width + 1U], (u64)fb[width + 2U],
	     (u64)fb[width + 3U]);
	{
		u32 nclear = 0, nsent = 0, nzero = 0, nother = 0;
		u32 first_sent = width * height, last_sent = 0;

		for (u32 i = 0; i < width * height; i++) {
			u32 v = fb[i];

			if (v == clear) {
				nclear++;
			} else if (v == sentinel) {
				nsent++;
				if (first_sent == width * height)
					first_sent = i;
				last_sent = i;
			} else if (v == 0) {
				nzero++;
			} else {
				nother++;
			}
		}
		logf("GPU: FRAGMENT fill nclear=%d nsent=%d nzero=%d nother=%d",
		     (u64)nclear, (u64)nsent, (u64)nzero, (u64)nother);
		logf("GPU: FRAGMENT sent first=%d last=%d", (u64)first_sent,
		     (u64)last_sent);
	}
	logf("GPU: FRAGMENT rows r0=%x/%x r1=%x/%x r2=%x/%x r3=%x/%x",
	     (u64)fb[0], (u64)fb[1], (u64)fb[width], (u64)fb[width + 1U],
	     (u64)fb[2U * width], (u64)fb[2U * width + 1U],
	     (u64)fb[3U * width], (u64)fb[3U * width + 1U]);
	logf("GPU: FRAGMENT rows r31=%x r32=%x r62=%x r63=%x",
	     (u64)fb[31U * width], (u64)fb[32U * width],
	     (u64)fb[62U * width], (u64)fb[63U * width]);
	frag_rowmask(fb, width, height, clear, sentinel);
	frag_pattern(fb, width, height);
	for (u32 i = 0; i < width * height; i++) {
		if (fb[i] != clear) {
			logf("GPU: FRAGMENT mismatch i=%d got=%x want=%x",
			     (u64)i, (u64)fb[i], (u64)clear);
			return -2;
		}
	}
	logf("GPU: FRAGMENT CLEAR PASS");
	return 0;
}

static u64
job_affinity(void)
{
	return (u64)rd32(GPU_BASE + GPU_SHADER_PRESENT_LO) |
	       ((u64)rd32(GPU_BASE + GPU_SHADER_PRESENT_HI) << 32);
}

static void
clear_job_irq(u32 js_present)
{
	wr32(GPU_BASE + JOB_INT_CLEAR, js_present | (js_present << 16));
	wr32(GPU_BASE + JOB_INT_MASK, 0);
}

static int
prepare_job(int slot, u64 job_va)
{
	u64 affinity;
	u32 cfg;

	if (slot < 0 || slot > 15)
		return -1;
	if (rd32(GPU_BASE + JS_COMMAND_NEXT(slot)) != 0) {
		logf("GPU: JS%d command_next busy=%x", (u64)slot,
		     (u64)rd32(GPU_BASE + JS_COMMAND_NEXT(slot)));
		return -2;
	}

	affinity = job_affinity();
	wr32(GPU_BASE + JS_HEAD_NEXT_LO(slot), (u32)job_va);
	wr32(GPU_BASE + JS_HEAD_NEXT_HI(slot), (u32)(job_va >> 32));
	wr32(GPU_BASE + JS_AFFINITY_NEXT_LO(slot), (u32)affinity);
	wr32(GPU_BASE + JS_AFFINITY_NEXT_HI(slot), (u32)(affinity >> 32));

	/* AS0, medium priority.  Use the default vendor-like flush
	 * configuration unless a diagnostic explicitly overrides it.
	 * BASE_HW_FEATURE_CLEAN_ONLY_SAFE is set on this G57, so the normal
	 * configuration is clean+invalidate at start and clean at end. */
	if (gpu_js_config_override != 0) {
		cfg = gpu_js_config_override;
	} else {
		cfg = JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE |
		      JS_CONFIG_END_FLUSH_CLEAN |
		      JS_CONFIG_THREAD_PRI(8);
		if (gpu_js_vendor_cfg) {
			cfg |= JS_CONFIG_ENABLE_FLUSH_REDUCTION;
			cfg |= JS_CONFIG_JOB_CHAIN_FLAG;
		}
	}
	wr32(GPU_BASE + JS_CONFIG_NEXT(slot), cfg);
	if (gpu_js_vendor_cfg && gpu_js_config_override == 0)
		wr32(GPU_BASE + JS_FLUSH_ID_NEXT(slot),
		     rd32(GPU_BASE + LATEST_FLUSH));
	wr32(GPU_BASE + JS_COMMAND_NEXT(slot), JS_COMMAND_START);

	logf("GPU: JS%d submit head=%x af=%x cfg=%x", (u64)slot,
	     job_va, affinity, (u64)cfg);
	return 0;
}

static int
wait_job(int slot, u32 *raw_out)
{
	u32 done = 1U << slot;
	u32 err = 1U << (slot + 16);
	u64 start, timeout;

	*raw_out = 0;
	start = ticks();
	timeout = GPU_WAIT_JOB_US * timer_freq() / 1000000ULL;
	for (;;) {
		u32 raw = rd32(GPU_BASE + JOB_INT_RAWSTAT);

		if (raw & (done | err)) {
			*raw_out = raw;
			wr32(GPU_BASE + JOB_INT_CLEAR, raw & (done | err));
			return (raw & err) ? -3 : 0;
		}
		if (ticks() - start >= timeout) {
			*raw_out = rd32(GPU_BASE + JOB_INT_RAWSTAT);
			return -4;
		}
		delay_us(10);
	}
}

static int
run_write_value_job(int slot, u64 job_va, u32 js_present, u32 *raw_out)
{
	int rc;

	clear_job_irq(js_present);
	rc = prepare_job(slot, job_va);
	if (rc != 0)
		return rc;
	return wait_job(slot, raw_out);
}

/*
 * Discriminate a fragment-only writeback defect from an address-dependent
 * failure in the basic GPU job write path.  The 16 targets span all values
 * of address bits [10:8] twice while staying inside the mapped output page.
 */
static int
gpu_write_value_grid_once(int slot, u32 js_present, u32 config)
{
	const u32 count = 16;
	const u64 sentinel = 0x5a5a5a5a5a5a5a5aULL;
	volatile u64 *out = (volatile u64 *)out_page;
	u64 passmask = 0, failmask = 0;
	u64 first_off = 0, first_got = 0, first_want = 0;
	u32 first_raw = 0;
	int rc;

	gpu_js_config_override = config;
	logf("GPU: WVALGRID begin cfg=%x", (u64)config);
	for (u32 i = 0; i < count; i++)
		out[i * 32U] = sentinel;
	dsb_sy();

	for (u32 i = 0; i < count; i++) {
		u64 offset = (u64)i * 0x100ULL;
		u64 want = 0xa57e000000000000ULL | (u64)(i + 1U);
		u64 got;
		u32 raw = 0;

		build_write_value_job(job_page, GPU_VA_OUT + offset, want);
		rc = run_write_value_job(slot, GPU_VA_JOB, js_present, &raw);
		if (rc != 0) {
			logf("GPU: WVALGRID cfg=%x job offset=%x rc=%d raw=%x",
			     (u64)config, offset, (u64)rc, (u64)raw);
			gpu_js_config_override = 0;
			return -1;
		}

		__asm__ volatile("dmb oshld" ::: "memory");
		got = out[i * 32U];
		if (got == want) {
			passmask |= 1ULL << i;
		} else {
			failmask |= 1ULL << i;
			if (failmask == (1ULL << i)) {
				first_off = offset;
				first_got = got;
				first_want = want;
				first_raw = raw;
			}
		}
	}

	gpu_js_config_override = 0;
	logf("GPU: WVALGRID cfg=%x pass=%w fail=%w",
	     (u64)config, passmask, failmask);
	if (failmask != 0) {
		logf("GPU: WVALGRID cfg=%x first offset=%x raw=%x got=%w want=%w",
		     (u64)config, first_off, (u64)first_raw, first_got,
		     first_want);
		return 1;
	}

	logf("GPU: WVALGRID PASS cfg=%x n=%d stride=%x",
	     (u64)config, (u64)count, 0x100ULL);
	return 0;
}

static int
gpu_write_value_grid_test(int slot, u32 js_present)
{
	/*
	 * Start flush uses 0=no action, 1=clean, 2=INV_SHADER_OTHER,
	 * 3=clean+invalidate. End flush supports no action, clean, or
	 * clean+invalidate on this JM. Keep thread priority at 8 and vary
	 * only the flush controls.
	 */
	static const u32 configs[] = {
		0x81300U, /* baseline: start clean+invalidate, end clean */
		0x81000U, /* no start flush, end clean */
		0x81100U, /* start clean, end clean */
		0x81200U, /* start invalidate, end clean */
		0x80300U, /* start clean+invalidate, no end flush */
		0x83300U, /* start clean+invalidate, end clean+invalidate */
		0x80000U, /* no start or end flush */
	};
	for (u32 i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
		int rc = gpu_write_value_grid_once(slot, js_present, configs[i]);

		if (rc < 0) {
			logf("GPU: WVALGRID matrix stopped at cfg=%x",
			     (u64)configs[i]);
			return -1;
		}
	}
	return 0;
}

static int
gpu_multislot_test(int slot0, u32 js_present)
{
	int slot1 = -1;
	u32 raw0, raw1;
	u64 got0, got1;
	int rc;

	for (u32 slot = 0; slot < 16; slot++) {
		u32 feat;

		if (!(js_present & (1U << slot)))
			continue;
		if (slot == (u32)slot0)
			continue;
		feat = rd32(GPU_BASE + GPU_JS_FEATURES(slot));
		if ((feat & JS_FEATURE_SET_VALUE_JOB) && slot1 < 0)
			slot1 = (int)slot;
	}
	if (slot1 < 0) {
		logf("GPU: MULTISLOT skipped (no second Write Value slot)");
		return 0;
	}
	logf("GPU: MULTISLOT slots=%d/%d", (u64)slot0, (u64)slot1);

	for (u32 i = 0; i < 512; i++) {
		out_page[i] = 0;
		out_page2[i] = 0;
	}
	dsb_sy();

	build_write_value_job(job_page, GPU_VA_OUT, 0xaaaaaaaaaaaaaaaaULL);
	build_write_value_job(job_page2, GPU_VA_OUT2, 0xbbbbbbbbbbbbbbbbULL);

	clear_job_irq(js_present);
	rc = prepare_job(slot0, GPU_VA_JOB);
	if (rc != 0)
		return -1;
	rc = prepare_job(slot1, GPU_VA_JOB2);
	if (rc != 0)
		return -2;

	rc = wait_job(slot0, &raw0);
	logf("GPU: MULTISLOT slot%d rc=%d raw=%x", (u64)slot0, (u64)rc,
	     (u64)raw0);
	if (rc != 0)
		return -3;

	rc = wait_job(slot1, &raw1);
	logf("GPU: MULTISLOT slot%d rc=%d raw=%x", (u64)slot1, (u64)rc,
	     (u64)raw1);
	if (rc != 0)
		return -4;

	__asm__ volatile("dmb oshld" ::: "memory");
	got0 = *(volatile u64 *)out_page;
	got1 = *(volatile u64 *)out_page2;
	logf("GPU: MULTISLOT out0=%w out1=%w", got0, got1);
	if (got0 != 0xaaaaaaaaaaaaaaaaULL)
		return -5;
	if (got1 != 0xbbbbbbbbbbbbbbbbULL)
		return -6;

	logf("GPU: MULTISLOT TEST PASS");
	return 0;
}

static int
find_compute_slot(u32 present)
{
	/* Prefer the highest compute-capable slot.  Slot 2 is normally
	 * unused by the Write Value / multi-slot tests and has a clean
	 * job-slot state, which removes stale-slot effects from compute
	 * bring-up. */
	for (int slot = 15; slot >= 0; slot--) {
		u32 feat;

		if (!(present & (1U << slot)))
			continue;
		feat = rd32(GPU_BASE + GPU_JS_FEATURES(slot));
		if (feat & JS_FEATURE_COMPUTE_JOB)
			return slot;
	}
	return -1;
}

static void
compute_dump_job(const char *tag)
{
	const volatile u32 *h = (const volatile u32 *)compute_job_page;

	for (u32 i = 0; i < 32; i++)
		logf("GPU: %s J%d=%x", tag, (u64)i, (u64)h[i]);
}

static void
compute_dump_shader(const char *tag, const u64 *page)
{
	for (u32 i = 0; i < 8; i++)
		logf("GPU: %s S%d=%w", tag, (u64)i, page[i]);
}

static void
compute_dump_desc(const char *tag, const u64 *page)
{
	const volatile u32 *d = (const volatile u32 *)page;

	for (u32 i = 0; i < 8; i++)
		logf("GPU: %s D%d=%x", tag, (u64)i, (u64)d[i]);
}

static void
compute_fault_diag(int slot, u32 raw)
{
	const volatile u32 *h = (const volatile u32 *)compute_job_page;
	u32 mraw = rd32(GPU_BASE + MMU_INT_RAWSTAT);
	u32 fstat = rd32(GPU_BASE + AS_FAULTSTATUS);
	u64 faddr = (u64)rd32(GPU_BASE + AS_FAULTADDRESS_LO) |
		    ((u64)rd32(GPU_BASE + AS_FAULTADDRESS_HI) << 32);
	u32 gstat = rd32(GPU_BASE + GPU_FAULT_STATUS);
	u64 gaddr = (u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_LO) |
		    ((u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_HI) << 32);
	u32 graw = rd32(GPU_BASE + GPU_IRQ_RAWSTAT);

	logf("GPU: COMPUTE fault raw=%x mraw=%x fstat=%x faddr=%w",
	     (u64)raw, (u64)mraw, (u64)fstat, faddr);
	logf("GPU: COMPUTE fault gstat=%x gaddr=%w gint=%x", (u64)gstat,
	     gaddr, (u64)graw);
	logf("GPU: COMPUTE js head_next=%x/%x head=%x/%x tail=%x/%x",
	     (u64)rd32(GPU_BASE + JS_HEAD_NEXT_LO(slot)),
	     (u64)rd32(GPU_BASE + JS_HEAD_NEXT_HI(slot)),
	     (u64)rd32(GPU_BASE + JS_HEAD_LO(slot)),
	     (u64)rd32(GPU_BASE + JS_HEAD_HI(slot)),
	     (u64)rd32(GPU_BASE + JS_TAIL_LO(slot)),
	     (u64)rd32(GPU_BASE + JS_TAIL_HI(slot)));
	logf("GPU: COMPUTE js cfg=%x cmd=%x status=%x astat=%x",
	     (u64)rd32(GPU_BASE + JS_CONFIG(slot)),
	     (u64)rd32(GPU_BASE + JS_COMMAND(slot)),
	     (u64)rd32(GPU_BASE + JS_STATUS(slot)),
	     (u64)rd32(GPU_BASE + AS_STATUS));
	logf("GPU: COMPUTE jobhdr=%x/%x/%x/%x/%x/%x/%x/%x",
	     (u64)h[0], (u64)h[1], (u64)h[2], (u64)h[3],
	     (u64)h[4], (u64)h[5], (u64)h[6], (u64)h[7]);

	wr32(GPU_BASE + MMU_INT_CLEAR, mraw);
	wr32(GPU_BASE + GPU_INT_CLEAR, graw & (GPU_IRQ_FAULT |
					       GPU_IRQ_MULTIPLE_FAULT));
	wr32(GPU_BASE + JOB_INT_CLEAR, raw);
}

static int
gpu_compute_test(u32 js_present)
{
	u32 raw;
	u64 got;
	int slot, rc;

	slot = find_compute_slot(js_present);
	if (slot < 0) {
		logf("GPU: COMPUTE skipped (no compute slot)");
		return 0;
	}
	logf("GPU: COMPUTE slot=%d", (u64)slot);

	for (u32 i = 0; i < 512; i++) {
		shader_page[i] = 0;
		shader_nop_page[i] = 0;
		shader_desc_page[i] = 0;
		compute_job_page[i] = 0;
		tls_page[i] = 0;
		out_page2[i] = 0;
	}
	for (u32 i = 0; i < sizeof(compute_nop_code) /
			     sizeof(compute_nop_code[0]); i++)
		shader_nop_page[i] = compute_nop_code[i];
	dsb_sy();

	/* Stage 1: NOP.end only.  This proves the compute descriptor,
	 * shader-program descriptor, slot and MMU mappings before any
	 * store. */
	build_shader_desc(shader_desc_page, GPU_VA_SHADER_NOP);
	build_compute_job(compute_job_page, GPU_VA_SHADER_DESC, GPU_VA_TLS);
	logf("GPU: COMPUTE NOP start code=%x desc=%x job=%x tls=%x",
	     GPU_VA_SHADER_NOP, GPU_VA_SHADER_DESC, GPU_VA_COMPUTE_JOB,
	     GPU_VA_TLS);
	compute_dump_job("NOP");
	compute_dump_shader("NOP", shader_nop_page);
	compute_dump_desc("NOP", shader_desc_page);

	rc = run_write_value_job(slot, GPU_VA_COMPUTE_JOB, js_present, &raw);
	logf("GPU: COMPUTE NOP rc=%d raw=%x js=%x state=%x", (u64)rc,
	     (u64)raw, (u64)rd32(GPU_BASE + JS_STATUS(slot)),
	     (u64)rd32(GPU_BASE + JOB_INT_JS_STATE));
	if (rc != 0) {
		compute_fault_diag(slot, raw);
		return -1;
	}
	logf("GPU: COMPUTE NOP PASS");

	/* Stage 2: full store shader. */
	for (u32 i = 0; i < sizeof(compute_shader_code) /
			     sizeof(compute_shader_code[0]); i++)
		shader_page[i] = compute_shader_code[i];
	dsb_sy();

	build_shader_desc(shader_desc_page, GPU_VA_SHADER);
	build_compute_job(compute_job_page, GPU_VA_SHADER_DESC, GPU_VA_TLS);
	logf("GPU: COMPUTE STORE start code=%x desc=%x job=%x tls=%x out=%x",
	     GPU_VA_SHADER, GPU_VA_SHADER_DESC, GPU_VA_COMPUTE_JOB,
	     GPU_VA_TLS, GPU_VA_OUT2);
	compute_dump_job("STORE");
	compute_dump_shader("STORE", shader_page);
	compute_dump_desc("STORE", shader_desc_page);

	rc = run_write_value_job(slot, GPU_VA_COMPUTE_JOB, js_present, &raw);
	logf("GPU: COMPUTE STORE rc=%d raw=%x js=%x state=%x", (u64)rc,
	     (u64)raw, (u64)rd32(GPU_BASE + JS_STATUS(slot)),
	     (u64)rd32(GPU_BASE + JOB_INT_JS_STATE));
	if (rc != 0) {
		compute_fault_diag(slot, raw);
		return -2;
	}

	__asm__ volatile("dmb oshld" ::: "memory");
	got = *(volatile u64 *)out_page2;
	logf("GPU: COMPUTE out=%w want=%w", got, 0x0123456789abcdefULL);
	if (got != 0x0123456789abcdefULL)
		return -3;
	logf("GPU: COMPUTE STORE PASS");
	return 0;
}

static int
gpu_traffic_test(void)
{
	u32 js_present, as_present, mmu;
	u32 raw;
	u32 slot;
	u64 val = 0x0123456789abcdefULL;
	u64 got;
	int rc;

	logf("GPU TEST: releasing MFG1 bus protection");
	release_mfg_bus_protect();
	configure_external_cg();

	logf("GPU TEST: GPU reset and internal power");
	if (gpu_soft_reset() != 0)
		return -1;
	if (gpu_internal_power_on() != 0)
		return -2;
	gpu_set_coherency();

	js_present = rd32(GPU_BASE + GPU_JS_PRESENT);
	as_present = rd32(GPU_BASE + GPU_AS_PRESENT);
	mmu = rd32(GPU_BASE + GPU_MMU_FEATURES);
	logf("GPU: js=%x as=%x mmu=%x va=%d pa=%d", (u64)js_present,
	     (u64)as_present, (u64)mmu, (u64)(mmu & 0xff),
	     (u64)((mmu >> 8) & 0xff));
	logf("GPU: shader=%x/%x tiler=%x l2=%x", (u64)rd32(GPU_BASE + GPU_SHADER_PRESENT_LO),
	     (u64)rd32(GPU_BASE + GPU_SHADER_PRESENT_HI),
	     (u64)rd32(GPU_BASE + GPU_TILER_PRESENT_LO),
	     (u64)rd32(GPU_BASE + GPU_L2_PRESENT_LO));

	if (!(as_present & 1U)) {
		logf("GPU: AS0 is not present; stop");
		return -3;
	}
	slot = (u32)find_write_value_slot(js_present);
	if ((int)slot < 0) {
		logf("GPU: no job slot supports Write Value; stop");
		return -4;
	}

	if (build_page_tables() != 0)
		return -5;
	if (mmu_enable_as0() != 0)
		return -6;

	build_write_value_job(job_page, GPU_VA_OUT, val);
	rc = run_write_value_job((int)slot, GPU_VA_JOB, js_present, &raw);
	logf("GPU: write-value rc=%d raw=%x js=%x state=%x", (u64)rc,
	     (u64)raw, (u64)rd32(GPU_BASE + JS_STATUS(slot)),
	     (u64)rd32(GPU_BASE + JOB_INT_JS_STATE));
	if (rc != 0)
		return -7;

	got = *(volatile u64 *)out_page;
	__asm__ volatile("dmb oshld" ::: "memory");
	logf("GPU: write-value out=%w want=%w", got, val);
	if (got != val) {
		logf("GPU: WRITE VALUE MISMATCH; stop before fault test");
		return -8;
	}
	logf("GPU: WRITE VALUE PASS");

	rc = gpu_write_value_grid_test((int)slot, js_present);
	if (rc < 0) {
		logf("GPU: WVALGRID STOP after a job error");
		return -8;
	}

	rc = gpu_bo_test((int)slot, js_present);
	if (rc != 0) {
		logf("GPU: BO TEST FAIL rc=%d", (u64)rc);
		return -9;
	}

	rc = gpu_multislot_test((int)slot, js_present);
	if (rc != 0) {
		logf("GPU: MULTISLOT TEST FAIL rc=%d", (u64)rc);
		return -10;
	}

	rc = gpu_compute_test(js_present);
	if (rc != 0) {
		logf("GPU: COMPUTE TEST FAIL rc=%d", (u64)rc);
		return -11;
	}

	rc = gpu_fragment_test(js_present);
	if (rc != 0) {
		logf("GPU: FRAGMENT TEST FAIL rc=%d", (u64)rc);
		return -12;
	}

	/* Deliberate fault: the job page is mapped, the target VA is not. */
	build_write_value_job(job_page2, GPU_VA_FAULT, val);
	raw = 0;
	rc = run_write_value_job((int)slot, GPU_VA_JOB2, js_present, &raw);
	{
		u32 mraw = rd32(GPU_BASE + MMU_INT_RAWSTAT);
		u32 fstat = rd32(GPU_BASE + AS_FAULTSTATUS);
		u64 faddr = (u64)rd32(GPU_BASE + AS_FAULTADDRESS_LO) |
			    ((u64)rd32(GPU_BASE + AS_FAULTADDRESS_HI) << 32);
		u32 gstat = rd32(GPU_BASE + GPU_FAULT_STATUS);
		u64 gaddr = (u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_LO) |
			    ((u64)rd32(GPU_BASE + GPU_FAULT_ADDRESS_HI) << 32);
		u32 graw = rd32(GPU_BASE + GPU_IRQ_RAWSTAT);
		u32 js_status = rd32(GPU_BASE + JS_STATUS(slot));

		logf("GPU: fault rc=%d jraw=%x mraw=%x js=%x fstat=%x", (u64)rc,
		     (u64)raw, (u64)mraw, (u64)js_status, (u64)fstat);
		logf("GPU: fault faddr=%w gstat=%x gaddr=%w gint=%x",
		     faddr, (u64)gstat, gaddr, (u64)graw);

		wr32(GPU_BASE + MMU_INT_CLEAR, mraw);
		wr32(GPU_BASE + GPU_INT_CLEAR, graw & (GPU_IRQ_FAULT |
						       GPU_IRQ_MULTIPLE_FAULT));
		wr32(GPU_BASE + JOB_INT_CLEAR, raw);

		/* An MMU fault does not raise JOB_INT_RAWSTAT on this GPU,
		 * so jraw may be zero.  The fault is latched in
		 * MMU_INT_RAWSTAT and the AS fault address.  JS_STATUS=8 is
		 * ACTIVE on Valhall, not a fault code (faults are 0x40+). */
		if ((rc == -3 || rc == -4) && mraw &&
		    (faddr == GPU_VA_FAULT || gaddr == GPU_VA_FAULT)) {
			logf("GPU: MMU FAULT TEST PASS (unmapped VA faulted, js=%x)",
			     (u64)js_status);
		} else {
			logf("GPU: MMU FAULT TEST INCOMPLETE");
		}
	}

	return 0;
}
#endif

static void
log_gpu_id(void)
{
	u32 id = rd32(GPU_BASE + GPU_ID);
	u32 product = (id >> 16) & 0xffffU;
	u32 major = (id >> 12) & 0xfU;
	u32 minor = (id >> 4) & 0xffU;
	u32 status = id & 0xfU;

	logf("GPU: ID=%x product=%x rev=%d.%d status=%d",
	     (u64)id, (u64)product, (u64)major, (u64)minor, (u64)status);

	/* Valhall model is in bits 27:24 (architecture) and 19:16
	 * (product major).  KL7/MT6789 reports product 0x9093, i.e. TNAX
	 * (9,3), not the older TTRX (9,0) value.  Both are Mali-G57 family. */
	switch (id & 0xf00f0000U) {
	case 0x90000000U:
		logf("GPU: identified Mali-G57 / Valhall (TTRX)");
		break;
	case 0x90030000U:
		logf("GPU: identified Mali-G57 / Valhall (TNAX, product 9093)");
		break;
	default:
		logf("GPU: unexpected Mali product id (wanted G57 TTRX/TNAX)");
		break;
	}

	logf("GPU: L2=%x tiler=%x", (u64)rd32(GPU_BASE + GPU_L2_FEATURES),
	     (u64)rd32(GPU_BASE + GPU_TILER_FEATURES));
	logf("GPU: mem=%x mmu=%x as=%x irq=%x",
	     (u64)rd32(GPU_BASE + GPU_MEM_FEATURES),
	     (u64)rd32(GPU_BASE + GPU_MMU_FEATURES),
	     (u64)rd32(GPU_BASE + GPU_AS_PRESENT),
	     (u64)rd32(GPU_BASE + GPU_IRQ_RAWSTAT));
}

void
gpu_visual_result(void)
{
#if GPU_POWER_TEST
	console_visual_result((const u32 *)fb_page, 64, 64, gpu_visual_valid);
#else
	console_visual_result(0, 0, 0, 0);
#endif
}

void
gpu_test(void)
{
#if GPU_POWER_TEST
	int rails;
#endif

	/* Put the CPU-generated card on both candidate panel pages before any
	 * GPU register access.  If the GPU test hangs, the card remains visible
	 * on the scanned page; the ramoops log still records the last stage. */
	gpu_visual_result();

	logf("GPU TEST: MT6789 Mali-G57 MC2 probe");
	log_domains();
#if GPU_POWER_TEST
	rails = log_pmic();
#else
	log_pmic();
#endif

#if GPU_POWER_TEST
	logf("GPU TEST: experimental power path enabled");
	if (!rails) {
		logf("GPU: VGPU/VSRAM_GPU are not both on; refusing MTCMOS power-on");
		return;
	}
	select_probe_clock();
	if (!clock_is_safe())
		return;
	for (u32 i = 0; i < DOMAIN_COUNT; i++) {
		int rc = power_on_domain(i);

		if (rc != 0) {
			logf("GPU: MFG%d power failed rc=%d", (u64)i,
			     (u64)(rc < 0 ? -rc : rc));
			return;
		}
	}
	log_domains();
	for (u32 i = 0; i < DOMAIN_COUNT; i++) {
		if (!domain_on(domain_bit[i])) {
			logf("GPU: MFG%d not on after power-on; stop", (u64)i);
			return;
		}
	}
#else
	for (u32 i = 0; i < DOMAIN_COUNT; i++) {
		if (!domain_on(domain_bit[i])) {
			logf("GPU: MFG%d is off; read-only probe stops here", (u64)i);
			return;
		}
	}
	if (!clock_is_safe())
		return;
#endif

	if (!mfg_gate_on()) {
		logf("GPU: MFG gate is not running; probe stops here");
		return;
	}

	log_gpu_id();
#if GPU_POWER_TEST
	if (gpu_traffic_test() != 0)
		logf("GPU: traffic test stopped before completion");
#endif
	logf("GPU TEST: done");
}
