/*
 * Read-only UFS probe for the KL7 (Tecno Spark 30 Pro, MT6789).
 *
 * The bootloader (LK) has just loaded this shim from the very same UFS, so the
 * host controller and the link may still be alive.  This code only observes
 * them and, if they are ready, issues READ-ONLY SCSI commands:
 *
 *   LUN 0/1 : the two UFS_BOOT LUNs (sda/sdb under Android)
 *   LUN 2   : the Android main LUN (sdc), 4K logical blocks, GPT at 4K-LBA 1
 *
 * There is no SCSI WRITE anywhere in this file.  The only writes are to the
 * host controller registers needed to take over the (already used) transfer
 * request list, exactly like the SWEET shim probe.
 *
 * Runs with the MMU and caches off, so the static DMA buffers below are used
 * at their physical addresses and no cache maintenance is required.
 */
#include "../../console.h"
#include "../../core/timer.h"
#include "mtk_ufs.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define UFS_BASE	0x11270000UL	/* ufshci@11270000, MT6789 live DT */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_AHIT	0x18
#define REG_IS		0x20
#define REG_IE		0x24
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UECPA	0x38
#define REG_UECBA	0x40
#define REG_UECC	0x48
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define REG_UCDLBA	0x70
#define REG_UCDLBAU	0x74
#define REG_UCDDBR	0x78
#define REG_UICCMD	0x90
#define REG_UIC_ARG1	0x94
#define REG_UIC_ARG2	0x98
#define REG_UIC_ARG3	0x9c

/* MediaTek vendor registers (ufs-mediatek.h). */
#define REG_UFS_REFCLK_CTRL	0x144
#define REG_UFS_EXTREG		0x2100
#define REG_UFS_MPHYCTRL	0x2200

#define REFCLK_REQUEST		0x1
#define REFCLK_ACK		0x2

/*
 * KL7 UFS resets live in infracfg_ao (syscon@10001000), child node
 * infracfg_rst (ti,syscon-reset).  Live DT:
 *   resets = <&infracfg_rst 1>, <&infracfg_rst 2>, <&infracfg_rst 3>;
 *   reset-names = "hci_rst", "unipro_rst", "crypto_rst";
 * ti,reset-bits entries: <assert_off assert_bit deassert_off deassert_bit
 *                         status_off status_bit flags>
 *   hci_rst:    0x130 bit 15 / 0x134 bit 15
 *   unipro_rst: 0x140 bit  7 / 0x144 bit  7
 * flags 0x1c = STATUS_NONE | ASSERT_SET | DEASSERT_SET.
 */
#define INFRACFG_AO_BASE	0x10001000UL
#define INFRACFG_HCI_RST_SET	0x130
#define INFRACFG_HCI_RST_CLR	0x134
#define INFRACFG_UNIPRO_RST_SET	0x140
#define INFRACFG_UNIPRO_RST_CLR	0x144
#define INFRACFG_HCI_RST_BIT	15
#define INFRACFG_UNIPRO_RST_BIT	7

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

#define IS_UIC_ERROR		(1u << 2)
#define IS_UIC_POWER_MODE	(1u << 4)
#define IS_UIC_COMMAND_COMPL	(1u << 10)
#define HCS_UPMCRS_PWR_LOCAL	1u

/* Conservative first bring-up used HS-G1; the device and MPHY advertise G3. */
#ifndef UFS_HS_GEAR
#define UFS_HS_GEAR 3u
#endif

/* UIC command opcodes and UniPro attributes used by the MTK resume path. */
#define UIC_CMD_DME_GET		0x01
#define UIC_CMD_DME_SET		0x02
#define UIC_CMD_DME_PEER_GET	0x03
#define UIC_CMD_DME_PEER_SET	0x04
#define UIC_CMD_DME_ENABLE	0x12
#define UIC_CMD_DME_RESET	0x14
#define UIC_CMD_DME_LINK_STARTUP 0x16
#define UIC_CMD_DME_HIBER_EXIT	0x18

/* UFS query UPIU (ufs.h / scsi_bsg_ufs.h). */
#define UPIU_TRANSACTION_QUERY_REQ	0x16
#define UPIU_TRANSACTION_QUERY_RSP	0x36
#define UPIU_QUERY_OPCODE_READ_FLAG	0x05
#define UPIU_QUERY_OPCODE_SET_FLAG	0x06
#define UPIU_QUERY_FUNC_STANDARD_READ	0x01
#define UPIU_QUERY_FUNC_STANDARD_WRITE	0x81
#define QUERY_FLAG_IDN_FDEVICEINIT	0x01

#define PA_LOCAL_TX_LCC_ENABLE	0x155E
#define PA_ACTIVETXDATALANES	0x1560
#define PA_CONNECTEDTXDATALANES	0x1561
#define PA_TXPWRSTATUS		0x1567
#define PA_TXGEAR		0x1568
#define PA_TXTERMINATION	0x1569
#define PA_HSSERIES		0x156A
#define PA_PWRMODE		0x1571
#define PA_ACTIVERXDATALANES	0x1580
#define PA_CONNECTEDRXDATALANES	0x1581
#define PA_RXPWRSTATUS		0x1582
#define PA_RXGEAR		0x1583
#define PA_RXTERMINATION	0x1584
#define PA_MAXRXHSGEAR		0x1587
#define PA_PWRMODEUSERDATA0	0x15B0
#define PA_PWRMODEUSERDATA1	0x15B1
#define PA_PWRMODEUSERDATA2	0x15B2
#define PA_PWRMODEUSERDATA3	0x15B3
#define PA_PWRMODEUSERDATA4	0x15B4
#define PA_PWRMODEUSERDATA5	0x15B5
#define PA_TXHSADAPTTYPE	0x15D4
#define PA_NO_ADAPT		3
#define PA_HS_MODE_B		2
#define VS_UNIPROPOWERDOWNCONTROL 0xD0A8
#define VS_SAVEPOWERCONTROL	0xD0A6

/* MediaTek SiP UFS control (ufs-mediatek.c/ufs-mediatek.h). */
#define MTK_SIP_UFS_CONTROL		0x82000276UL
#define UFS_MTK_SIP_DEVICE_RESET	2
#define UFS_MTK_SIP_REF_CLK_NOTIFICATION 8

static inline u32 ufs_rd(u32 off) { return *(volatile u32 *)(UFS_BASE + off); }
static inline void ufs_wr(u32 off, u32 v) { *(volatile u32 *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	u32 dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	u32 dw1;
	u32 dw2;		/* [7:0] OCS */
	u32 dw3;
	u32 ucdba;		/* command descriptor base, 128-byte aligned */
	u32 ucdbau;
	u32 resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	u32 prdt;		/* [31:16] PRDT offset, [15:0] entries */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT.
 * Linux ufshcd uses ALIGNED_UPIU_SIZE = 512 for each UPIU and puts the PRDT
 * at offset 1024; the UTRD response/PRDT offsets and lengths are in DWORDs. */
#define PRDT_CHUNK	16384u
#define PRDT_MAX	128u

struct ucd {
	u8 cmd[512];
	u8 resp[512];
	struct {
		u32 addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[PRDT_MAX];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));


static u32 last_ocs, last_st, last_upiu, last_resid;

/* v0117 HS result, repeated after the bench so it stays on screen. */
static u32 hs_local_gear, hs_peer_gear, hs_rxlanes, hs_txlanes;
static u32 hs_final_pwr, hs_final_gear, hs_upmcrs;
static int hs_active, hs_link_broken;
static const char *hs_fail = "not_run";







/*
 * MediaTek SiP UFS control, used by Linux ufs-mediatek.c.  The secure
 * monitor owns the UFS device reset line (RST_n) and the reference clock
 * gate; both are needed to revive a device left powered down by the LK.
 * x0 = MTK_SIP_SMC_CMD(0x276), x1 = command, x2 = value.
 */
static u64 ufs_smc(u64 cmd, u64 val)
{
	register u64 x0 __asm__("x0") = MTK_SIP_UFS_CONTROL;
	register u64 x1 __asm__("x1") = cmd;
	register u64 x2 __asm__("x2") = val;
	register u64 x3 __asm__("x3") = 0;
	register u64 x4 __asm__("x4") = 0;
	register u64 x5 __asm__("x5") = 0;
	register u64 x6 __asm__("x6") = 0;
	register u64 x7 __asm__("x7") = 0;

	__asm__ volatile("smc #0" : "+r"(x0)
			 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5),
			   "r"(x6), "r"(x7)
			 : "memory");
	return x0;
}

/*
 * Mirror ufs_mtk_setup_ref_clk(true): notify the secure monitor that the
 * reference clock is needed, then request it from the host controller and
 * wait for the ack.  If the LK already left it running, change nothing.
 */
static void ref_clk_on(void)
{
	u32 v = ufs_rd(REG_UFS_REFCLK_CTRL);
	logf("UFS: refclk ctrl=%x", (u64)v);
	if ((v & (REFCLK_REQUEST | REFCLK_ACK)) ==
	    (REFCLK_REQUEST | REFCLK_ACK))
		return;

	u64 s = ufs_smc(UFS_MTK_SIP_REF_CLK_NOTIFICATION, 1);
	logf("UFS: refclk smc=%x", s);
	timer_delay_us(10);
	ufs_wr(REG_UFS_REFCLK_CTRL, REFCLK_REQUEST);

	u64 start = timer_ticks();
	u64 timeout = timer_frequency() * 3 / 1000;	/* 3 ms */
	do {
		v = ufs_rd(REG_UFS_REFCLK_CTRL);
		if (((v & REFCLK_ACK) >> 1) == (v & REFCLK_REQUEST))
			break;
	} while (timer_ticks() - start < timeout);
	logf("UFS: refclk ctrl after=%x", (u64)v);
}

static u32 infracfg_rd(u32 off)
{
	return *(volatile u32 *)(INFRACFG_AO_BASE + off);
}

static void infracfg_set(u32 off, u32 bit)
{
	u32 v = infracfg_rd(off);

	*(volatile u32 *)(INFRACFG_AO_BASE + off) = v | (1u << bit);
}

/*
 * Mirror ufs_mtk_host_reset(): assert HCI and UniPro resets, then release
 * UniPro first and HCI last.  Without this the UniPro link startup state
 * machine stays in the LK's state and DME_LINK_STARTUP returns failure.
 */
static void mtk_host_reset(void)
{
	logf("UFS: icfg before hci=%x/%x uni=%x/%x",
	     (u64)infracfg_rd(INFRACFG_HCI_RST_SET),
	     (u64)infracfg_rd(INFRACFG_HCI_RST_CLR),
	     (u64)infracfg_rd(INFRACFG_UNIPRO_RST_SET),
	     (u64)infracfg_rd(INFRACFG_UNIPRO_RST_CLR));
	infracfg_set(INFRACFG_HCI_RST_SET, INFRACFG_HCI_RST_BIT);
	infracfg_set(INFRACFG_UNIPRO_RST_SET, INFRACFG_UNIPRO_RST_BIT);
	timer_delay_us(100);
	infracfg_set(INFRACFG_UNIPRO_RST_CLR, INFRACFG_UNIPRO_RST_BIT);
	infracfg_set(INFRACFG_HCI_RST_CLR, INFRACFG_HCI_RST_BIT);
	timer_delay_us(100);
	logf("UFS: icfg after hci=%x/%x uni=%x/%x",
	     (u64)infracfg_rd(INFRACFG_HCI_RST_SET),
	     (u64)infracfg_rd(INFRACFG_HCI_RST_CLR),
	     (u64)infracfg_rd(INFRACFG_UNIPRO_RST_SET),
	     (u64)infracfg_rd(INFRACFG_UNIPRO_RST_CLR));
}

/*
 * Mirror ufshcd_hba_execute_hce(): stop, MTK reset, ref clock, start HCE,
 * then wait for the controller to finish its 1->0->1 initialization.
 */
static void hce_cycle(void)
{
	if (ufs_rd(REG_HCE) & 1u) {
		ufs_wr(REG_HCE, 0);
		u64 start = timer_ticks();
		u64 timeout = timer_frequency() / 100;	/* 10 ms */
		while ((ufs_rd(REG_HCE) & 1u) && timer_ticks() - start < timeout)
			;
	}
	logf("UFS: hce stop=%x hcs=%x", (u64)ufs_rd(REG_HCE),
	     (u64)ufs_rd(REG_HCS));

	mtk_host_reset();
	ref_clk_on();

	ufs_wr(REG_HCE, 1);
	timer_delay_us(600);
	u64 start = timer_ticks();
	u64 timeout = timer_frequency();		/* 1 second */
	while (!(ufs_rd(REG_HCE) & 1u)) {
		if (timer_ticks() - start > timeout)
			break;
	}
	logf("UFS: hce start=%x hcs=%x is=%x", (u64)ufs_rd(REG_HCE),
	     (u64)ufs_rd(REG_HCS), (u64)ufs_rd(REG_IS));
}

/*
 * Send one UIC command and wait for completion.  UFSHCI signals UIC command
 * completion in IS bit 10 (0x400); the result code is in UICARG2 and the DME
 * attribute value in UICARG3 (Linux ufshcd_get_uic_cmd_result/get_dme_attr_val).
 */
static u32 uic(u32 opcode, u32 arg1, u32 arg2, u32 arg3, u32 *val)
{
	ufs_wr(REG_IS, IS_UIC_COMMAND_COMPL | IS_UIC_ERROR);
	ufs_wr(REG_UIC_ARG1, arg1);
	ufs_wr(REG_UIC_ARG2, arg2);
	ufs_wr(REG_UIC_ARG3, arg3);
	ufs_wr(REG_UICCMD, opcode & 0xff);

	u64 start = timer_ticks();
	u64 timeout = timer_frequency() / 2;		/* 500 ms */
	while (!(ufs_rd(REG_IS) & IS_UIC_COMMAND_COMPL)) {
		if (timer_ticks() - start > timeout)
			return 0x100;		/* timeout */
	}
	u32 result = ufs_rd(REG_UIC_ARG2) & 0xff;
	if (val)
		*val = ufs_rd(REG_UIC_ARG3);
	ufs_wr(REG_IS, IS_UIC_COMMAND_COMPL);
	return result;
}

/* Mirror ufs_mtk_pre_link(): HPM, host TX LCC off, disable deep stall. */
static void mtk_pre_link(void)
{
	u32 v = 0, r;

	r = uic(UIC_CMD_DME_SET, (u32)VS_UNIPROPOWERDOWNCONTROL << 16, 0, 0,
		&v);
	logf("UFS: HPM rc=%x", (u64)r);
	r = uic(UIC_CMD_DME_SET, (u32)PA_LOCAL_TX_LCC_ENABLE << 16, 0, 0, &v);
	logf("UFS: TXLCC rc=%x", (u64)r);
	r = uic(UIC_CMD_DME_GET, (u32)VS_SAVEPOWERCONTROL << 16, 0, 0, &v);
	logf("UFS: SAVEPWR get rc=%x val=%x", (u64)r, (u64)v);
	if (!r) {
		r = uic(UIC_CMD_DME_SET, (u32)VS_SAVEPOWERCONTROL << 16, 0,
			v & ~(1u << 6), &v);
		logf("UFS: SAVEPWR set rc=%x", (u64)r);
	}
}

/*
 * v0117: the KL7 bootloader leaves UniPro in PWM (PA_PWRMODE=0x55, i.e.
 * SLOWAUTO/SLOWAUTO).  The Android kernel then performs the standard HS
 * power-mode change; our driver historically skipped it, which is why the
 * link moved at PWM-G1 speed (~480 KiB/s).  This is the same sequence Linux
 * uses in ufshcd_dme_change_power_mode(), but kept read-only with respect to
 * the UFS device: only UniPro/PHY attributes change.
 */
static u32
hs_get(u32 opcode, u32 attr, const char *name, u32 *val)
{
	u32 v = 0;
	u32 r = uic(opcode, attr << 16, 0, 0, &v);
	logf("UFS HS: %s rc=%x val=%x", name, (u64)r, (u64)v);
	if (val)
		*val = v;
	return r;
}

static u32
hs_set(u32 attr, u32 val, const char *name)
{
	u32 v = 0;
	u32 r = uic(UIC_CMD_DME_SET, attr << 16, 0, val, &v);
	logf("UFS HS: set %s=%x rc=%x", name, (u64)val, (u64)r);
	return r;
}

/*
 * A DME_SET(PA_PWRMODE) completes in two stages: first the UIC command
 * completion, then the actual link power-mode change.  The latter is
 * signalled by IS.UIC_POWER_MODE and its result is HCS.UPMCRS.  Linux's
 * ufshcd_uic_pwr_ctrl() waits for both; polling only UIC_COMMAND_COMPL makes
 * the change look like a failure and then races the PWM restore.
 */
static u32
hs_pwr_change(u32 mode, const char *name)
{
	u32 v = 0;
	ufs_wr(REG_IS, IS_UIC_POWER_MODE);
	u32 r = uic(UIC_CMD_DME_SET, (u32)PA_PWRMODE << 16, 0, mode, &v);
	logf("UFS HS: set %s=%x rc=%x", name, (u64)mode, (u64)r);
	if (r)
		return r;

	u64 start = timer_ticks();
	u64 timeout = timer_frequency();		/* 1 second */
	while (!(ufs_rd(REG_IS) & IS_UIC_POWER_MODE)) {
		if (timer_ticks() - start > timeout) {
			logf("UFS HS: %s timeout is=%x hcs=%x", name,
			     (u64)ufs_rd(REG_IS), (u64)ufs_rd(REG_HCS));
			return 0x100;
		}
	}
	u32 is = ufs_rd(REG_IS);
	u32 hcs = ufs_rd(REG_HCS);
	hs_upmcrs = (hcs >> 8) & 7u;
	ufs_wr(REG_IS, IS_UIC_POWER_MODE);
	logf("UFS HS: %s done is=%x hcs=%x upmcrs=%x", name, (u64)is,
	     (u64)hcs, (u64)hs_upmcrs);
	return hs_upmcrs == HCS_UPMCRS_PWR_LOCAL ? 0 : (0x200u | hs_upmcrs);
}

static void
ufs_hs_enable(void)
{
	u32 rxlanes = 0, txlanes = 0, local = 0, peer = 0;
	u32 pwr = 0, v = 0, r;

	logf("UFS HS: enter");
	if (hs_get(UIC_CMD_DME_GET, PA_CONNECTEDRXDATALANES,
		   "connected_rx", &rxlanes) ||
	    hs_get(UIC_CMD_DME_GET, PA_CONNECTEDTXDATALANES,
		   "connected_tx", &txlanes)) {
		hs_fail = "connected_lanes";
		return;
	}
	hs_rxlanes = rxlanes;
	hs_txlanes = txlanes;
	if (hs_get(UIC_CMD_DME_GET, PA_MAXRXHSGEAR, "max_hs_local", &local) ||
	    hs_get(UIC_CMD_DME_PEER_GET, PA_MAXRXHSGEAR, "max_hs_peer",
		   &peer)) {
		hs_fail = "max_hs_gear";
		return;
	}
	hs_local_gear = local;
	hs_peer_gear = peer;
	if (!rxlanes || !txlanes || rxlanes > 2 || txlanes > 2) {
		logf("UFS HS: bad lanes rx=%x tx=%x", (u64)rxlanes,
		     (u64)txlanes);
		hs_fail = "bad_lanes";
		return;
	}
	if (!peer) {
		logf("UFS HS: peer has no HS gear");
		hs_fail = "peer_no_hs";
		return;
	}
	logf("UFS HS: caps lanes %x/%x, max gear local=%x peer=%x, target=%x",
	     (u64)rxlanes, (u64)txlanes, (u64)local, (u64)peer,
	     (u64)UFS_HS_GEAR);

	if (hs_set(PA_RXGEAR, UFS_HS_GEAR, "rxgear") ||
	    hs_set(PA_ACTIVERXDATALANES, rxlanes, "rxlanes") ||
	    hs_set(PA_RXTERMINATION, 1, "rxterm") ||
	    hs_set(PA_TXGEAR, UFS_HS_GEAR, "txgear") ||
	    hs_set(PA_ACTIVETXDATALANES, txlanes, "txlanes") ||
	    hs_set(PA_TXTERMINATION, 1, "txterm") ||
	    hs_set(PA_HSSERIES, PA_HS_MODE_B, "hsseries_b")) {
		hs_fail = "hs_set";
		return;
	}

	/*
	 * Linux supplies the UniPro timeout user-data with every power-mode
	 * change.  Failures here are logged but do not abort the change; the
	 * bootloader may already have usable values.
	 */
	(void)hs_set(PA_PWRMODEUSERDATA0, 8191, "ud0");
	(void)hs_set(PA_PWRMODEUSERDATA1, 65535, "ud1");
	(void)hs_set(PA_PWRMODEUSERDATA2, 32767, "ud2");
	(void)hs_set(PA_PWRMODEUSERDATA3, 8191, "ud3");
	(void)hs_set(PA_PWRMODEUSERDATA4, 65535, "ud4");
	(void)hs_set(PA_PWRMODEUSERDATA5, 32767, "ud5");

	/* FAST (0x1) for both directions: 0x11.  FASTAUTO would be 0x44. */
	r = hs_pwr_change(0x11, "pwr_0x11");
	if (r) {
		logf("UFS HS: power change failed rc=%x, restoring PWM",
		     (u64)r);
		hs_fail = "not_hs";
		if (hs_pwr_change(0x55, "pwm_restore")) {
			hs_link_broken = 1;
			logf("UFS HS: PWM restore failed; link unusable");
		}
		return;
	}
	if (hs_get(UIC_CMD_DME_GET, PA_PWRMODE, "pwr_readback", &pwr)) {
		logf("UFS HS: pwr readback failed");
		hs_fail = "pwr_readback";
		return;
	}
	hs_final_pwr = pwr;
	(void)hs_get(UIC_CMD_DME_GET, PA_TXGEAR, "txgear_readback",
		     &hs_final_gear);
	(void)hs_get(UIC_CMD_DME_GET, PA_RXGEAR, "rxgear_readback", &v);
	(void)hs_get(UIC_CMD_DME_GET, PA_TXPWRSTATUS, "txstatus", &v);
	(void)hs_get(UIC_CMD_DME_GET, PA_RXPWRSTATUS, "rxstatus", &v);
	logf("UFS HS: ACTIVE pwr=%x gear=%x upmcrs=%x", (u64)pwr,
	     (u64)hs_final_gear, (u64)hs_upmcrs);
	hs_active = 1;
	hs_fail = "none";
	(void)hs_set(PA_TXHSADAPTTYPE, PA_NO_ADAPT, "noadapt");
}

static void zero(void *p, unsigned n)
{
	u8 *b = (u8 *)p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static u32 be32(const u8 *p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}









/*
 * Issue one SCSI command (read direction or no data) in UTRL slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static u32 scsi(u32 lun, const u8 *cdb, u32 cdb_len, void *buf, u32 len)
{
	static u8 tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = (u8)lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = (u8)(len >> 24);
	ucd.cmd[13] = (u8)(len >> 16);
	ucd.cmd[14] = (u8)(len >> 8);
	ucd.cmd[15] = (u8)len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	u64 ucd_pa = (u64)(unsigned long)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (u32)ucd_pa;
	utrl[0].ucdbau = (u32)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		u32 entries = (len + PRDT_CHUNK - 1) / PRDT_CHUNK;
		if (entries > PRDT_MAX)
			return 0x105;
		u64 buf_pa = (u64)(unsigned long)buf;
		for (u32 i = 0; i < entries; i++) {
			u32 n = len - i * PRDT_CHUNK;
			if (n > PRDT_CHUNK)
				n = PRDT_CHUNK;
			u64 pa = buf_pa + (u64)i * PRDT_CHUNK;
			ucd.prdt[i].addr_lo = (u32)pa;
			ucd.prdt[i].addr_hi = (u32)(pa >> 32);
			ucd.prdt[i].dbc = n - 1;
		}
		utrl[0].prdt = (1024u / 4) << 16 | entries;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	u64 start = timer_ticks();
	u64 timeout = timer_frequency();		/* 1 second */
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (timer_ticks() - start > timeout) {
			ufs_wr(REG_UTRLCLR, 1);	/* clear doorbell bit 0 */
			return 1;		/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	last_ocs = utrl[0].dw2 & 0xff;
	last_upiu = ucd.resp[0];		/* 0x21 = RESPONSE UPIU */

	last_resid = be32(ucd.resp + 12);	/* residual transfer count */
	last_st = ucd.resp[7];			/* SCSI status */
	if (last_ocs)
		return 2;
	if (last_upiu != 0x21)			/* RESPONSE UPIU */
		return 3;
	return last_st ? 4 : 0;
}

/*
 * Send one UFS QUERY REQUEST (no data) in UTRL slot 0.  Used for the
 * fDeviceInit handshake: after a device reset the UFS device keeps all LUs
 * NOT READY (sense key 2, ASC 0x04, ASCQ 0x01) until the host sets this flag;
 * the device then initializes and clears it (ufshcd_complete_dev_init()).
 */
static u32 ufs_query_flag(u32 opcode, u32 idn, u32 index, u32 *val)
{
	static u8 qtag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	ucd.cmd[0] = UPIU_TRANSACTION_QUERY_REQ;
	ucd.cmd[1] = 0;
	ucd.cmd[2] = 0;				/* LUN 0: device management */
	ucd.cmd[3] = ++qtag;
	ucd.cmd[5] = (opcode == UPIU_QUERY_OPCODE_SET_FLAG) ?
		     UPIU_QUERY_FUNC_STANDARD_WRITE :
		     UPIU_QUERY_FUNC_STANDARD_READ;
	ucd.cmd[12] = (u8)opcode;
	ucd.cmd[13] = (u8)idn;
	ucd.cmd[14] = (u8)index;
	ucd.cmd[15] = 0;			/* selector */
	/* length and value stay zero for flag queries */

	u64 ucd_pa = (u64)(unsigned long)&ucd;
	utrl[0].dw0 = (1u << 28) | (1u << 24);	/* UFS storage, interrupt */
	utrl[0].dw1 = 0;
	utrl[0].dw2 = 0x0f;			/* OCS invalid until completion */
	utrl[0].dw3 = 0;
	utrl[0].ucdba = (u32)ucd_pa;
	utrl[0].ucdbau = (u32)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	utrl[0].prdt = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	u64 start = timer_ticks();
	u64 timeout = timer_frequency();		/* 1 second */
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (timer_ticks() - start > timeout) {
			ufs_wr(REG_UTRLCLR, 1);
			logf("UFS: query op=%x idn=%x timeout", (u64)opcode,
			     (u64)idn);
			return 2;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	u32 ocs = utrl[0].dw2 & 0xff;
	u32 upiu = ucd.resp[0];
	u32 rsp = ucd.resp[6];
	u32 v = be32(ucd.resp + 20);		/* query response value */
	if (val)
		*val = v;
	/* SET_FLAG and errors are interesting; quiet successful READ_FLAGs. */
	if (opcode == UPIU_QUERY_OPCODE_SET_FLAG || ocs ||
	    upiu != UPIU_TRANSACTION_QUERY_RSP || rsp)
		logf("UFS: query op=%x idn=%x ocs=%x upiu=%x rsp=%x val=%x",
		     (u64)opcode, (u64)idn, (u64)ocs, (u64)upiu, (u64)rsp,
		     (u64)v);
	if (ocs)
		return 3;
	if (upiu != UPIU_TRANSACTION_QUERY_RSP)
		return 4;
	if (rsp)
		return 5;
	return 0;
}

static u32 ufs_complete_dev_init(void)
{
	u32 val = 0, rc;
	rc = ufs_query_flag(UPIU_QUERY_OPCODE_SET_FLAG,
			    QUERY_FLAG_IDN_FDEVICEINIT, 0, &val);
	logf("UFS: fDeviceInit SET rc=%x", (u64)rc);
	if (rc)
		return rc;

	u64 start = timer_ticks();
	u64 timeout = timer_frequency() * 1500 / 1000;	/* 1.5 s */
	u32 tries = 0;
	do {
		rc = ufs_query_flag(UPIU_QUERY_OPCODE_READ_FLAG,
				    QUERY_FLAG_IDN_FDEVICEINIT, 0, &val);
		tries++;
		if (rc || !(val & 1u))
			break;
		timer_delay_us(5000);
	} while (timer_ticks() - start < timeout);
	logf("UFS: fDeviceInit poll rc=%x val=%x tries=%x", (u64)rc,
	     (u64)val, (u64)tries);
	return rc;
}

static u32 read_blocks_one(u32 lun, u32 lba, u32 count, void *buf)
{
	u8 cdb[10] = { 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16),
		       (u8)(lba >> 8), (u8)lba, 0, (u8)(count >> 8), (u8)count, 0 };
	return scsi(lun, cdb, sizeof(cdb), buf, count * BLOCK);
}























static int ufs_ready;

int mtk_ufs_init(void)
{
	u32 hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	u32 r, v = 0;

	if (ufs_ready)
		return 0;

	logf("UFS: MT6789 host init base=%x gear=%d",
	     (u64)UFS_BASE, (u64)UFS_HS_GEAR);
	if (!(hce & 1u))
		logf("UFS: LK left HCE=0");
	if (!(hcs & HCS_DP))
		logf("UFS: LK left DP=0; restarting link");

	hce_cycle();

	u64 smc = ufs_smc(UFS_MTK_SIP_DEVICE_RESET, 0);
	if (smc) {
		logf("UFS: device reset assert SMC rc=%x", smc);
		return -1;
	}
	timer_delay_us(20);
	smc = ufs_smc(UFS_MTK_SIP_DEVICE_RESET, 1);
	if (smc) {
		logf("UFS: device reset deassert SMC rc=%x", smc);
		return -2;
	}
	timer_delay_us(15000);

	mtk_pre_link();
	r = uic(UIC_CMD_DME_LINK_STARTUP, 0, 0, 0, &v);
	if (r) {
		logf("UFS: LINKSTART rc=%x; retrying UniPro reset", (u64)r);
		(void)uic(UIC_CMD_DME_RESET, 0, 0, 0, &v);
		(void)uic(UIC_CMD_DME_ENABLE, 0, 0, 0, &v);
		mtk_pre_link();
		r = uic(UIC_CMD_DME_LINK_STARTUP, 0, 0, 0, &v);
	}
	if (r) {
		logf("UFS: LINKSTART failed rc=%x", (u64)r);
		return -3;
	}

	r = uic(UIC_CMD_DME_GET, (u32)PA_PWRMODE << 16, 0, 0, &v);
	logf("UFS: PA_PWRMODE rc=%x val=%x", (u64)r, (u64)v);
	if (r)
		return -4;

	ufs_hs_enable();
	if (hs_link_broken || !hs_active) {
		logf("UFS: HS link recovery failed (%s)", hs_fail);
		return -5;
	}

	/* The HCI reset cleared the request-list registers. Take over UTRL now. */
	ufs_wr(REG_UTRLRSR, 0);
	u64 list = (u64)(unsigned long)&utrl[0];
	ufs_wr(REG_UTRLBA, (u32)list);
	ufs_wr(REG_UTRLBAU, (u32)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	if (ufs_rd(REG_UTRLBA) != (u32)list ||
	    ufs_rd(REG_UTRLBAU) != (u32)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1u)) {
		logf("UFS: UTRL ownership failed");
		return -6;
	}

	r = ufs_complete_dev_init();
	if (r) {
		logf("UFS: fDeviceInit failed rc=%x", (u64)r);
		return -7;
	}

	ufs_ready = 1;
	logf("UFS: ready HS gear=%d lanes=%d/%d",
	     (u64)hs_final_gear, (u64)hs_rxlanes, (u64)hs_txlanes);
	return 0;
}

int mtk_ufs_read_blocks(u32 lun, u32 lba, u32 count, void *buffer)
{
	const u32 max_blocks = (PRDT_CHUNK * PRDT_MAX) / BLOCK;
	u8 *out = (u8 *)buffer;

	if (!ufs_ready || !buffer || !count)
		return -1;
	if (lba > 0xffffffffu - count)
		return -2;

	while (count) {
		u32 n = count > max_blocks ? max_blocks : count;
		u32 rc = read_blocks_one(lun, lba, n, out);
		if (rc)
			return -(int)(10u + rc);
		if (last_resid != 0u)
			return -30;
		lba += n;
		count -= n;
		out += n * BLOCK;
	}
	return 0;
}

u32 mtk_ufs_block_size(void)
{
	return BLOCK;
}
