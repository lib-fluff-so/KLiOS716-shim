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
#include "console.h"
#include "ufs_test.h"

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
#define REG_UICARG	0x94
#define REG_UICARG1	0x98
#define REG_UICARG2	0x9c
#define REG_UICARG3	0xa0

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

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

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	u8 cmd[512];
	u8 resp[512];
	struct {
		u32 addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static u8 data[2 * BLOCK] __attribute__((aligned(4096)));
static u8 ents[128 * 128] __attribute__((aligned(4096)));

static u32 last_ocs, last_st, last_sense;

static u64 ticks(void)
{
	u64 t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static u64 timer_freq(void)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 19200000u;
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

static u32 le32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 le64(const u8 *p)
{
	u64 v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static void ascii8(char out[9], const u8 *p)
{
	for (int i = 0; i < 8; i++)
		out[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
	out[8] = 0;
}

static int name_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
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
		u64 buf_pa = (u64)(unsigned long)buf;
		ucd.prdt[0].addr_lo = (u32)buf_pa;
		ucd.prdt[0].addr_hi = (u32)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	u64 start = ticks();
	u64 timeout = timer_freq();		/* 1 second */
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > timeout) {
			ufs_wr(REG_UTRLCLR, 1);	/* clear doorbell bit 0 */
			return 1;		/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	last_ocs = utrl[0].dw2 & 0xff;
	last_st = ucd.resp[7];			/* SCSI status in the response UPIU */
	last_sense = (u32)ucd.resp[16] << 24 | (u32)ucd.resp[17] << 16 |
		     (u32)ucd.resp[18] << 8 | ucd.resp[19];
	if (last_ocs)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return last_st ? 4 : 0;
}

static u32 read_blocks(u32 lun, u32 lba, u32 count, void *buf)
{
	u8 cdb[10] = { 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16),
		       (u8)(lba >> 8), (u8)lba, 0, (u8)(count >> 8), (u8)count, 0 };
	return scsi(lun, cdb, sizeof(cdb), buf, count * BLOCK);
}

static u32 tur(u32 lun)
{
	static const u8 cdb[6] = { 0x00 };
	u32 rc = 0;
	/* The first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(lun, cdb, sizeof(cdb), 0, 0);
		if (!rc)
			break;
	}
	return rc;
}

static u32 read_capacity(u32 lun, u32 *last_lba, u32 *blk)
{
	static const u8 cdb[10] = { 0x25 };	/* READ CAPACITY(10) */
	u32 rc = scsi(lun, cdb, sizeof(cdb), data, 8);
	if (!rc) {
		*last_lba = be32(data);
		*blk = be32(data + 4);
	}
	return rc;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(u32 lun, u32 lba)
{
	if (read_blocks(lun, lba, 1, data))
		return "read error";
	const u8 *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry (4K LBA units, sizes in MiB). */
static void list_partitions(u32 lun, u64 entry_lba, u32 nent)
{
	if (!nent || nent > 128) {
		logf("GPT: nent=%x invalid", (u64)nent);
		return;
	}
	u32 blocks = (nent * 128u + BLOCK - 1u) / BLOCK;
	if (entry_lba > 0xffffffffu) {
		logf("GPT: entries lba=%w too high", entry_lba);
		return;
	}
	u32 rc = read_blocks(lun, (u32)entry_lba, blocks, ents);
	logf("GPT: table lba=%x n=%x blocks=%x rc=%x ocs=%x st=%x",
	     (u64)(u32)entry_lba, (u64)nent, (u64)blocks, (u64)rc,
	     (u64)last_ocs, (u64)last_st);
	if (rc)
		return;

	u32 shown = 0;
	for (u32 i = 0; i < nent; i++) {
		const u8 *e = ents + i * 128;
		u64 first = le64(e + 32), last = le64(e + 40);
		if (!first || last < first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			u8 ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? (char)ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		u64 mib = (last - first + 1) >> 8;	/* 4K blocks -> MiB */
		if (!mib)
			continue;
		logf("part %s lba=%x last=%x MiB=%d", name, first, last, mib);
		shown++;
		if (name_eq(name, "userdata") || name_eq(name, "cache") ||
		    name_eq(name, "cust"))
			logf("part %s fs=%s", name, fs_type(lun, (u32)first));
	}
	logf("GPT: %d partitions listed", (u64)shown);
}

static void dump_gpt(u32 lun)
{
	u32 rc = read_blocks(lun, 0, 2, data);
	logf("LUN%d READ0/1: rc=%x ocs=%x st=%x sense=%x", (u64)lun,
	     (u64)rc, (u64)last_ocs, (u64)last_st, (u64)last_sense);
	if (rc)
		return;
	logf("LUN%d MBR: sig=%x type=%x", (u64)lun,
	     (u64)(data[510] | (data[511] << 8)), (u64)data[450]);

	const u8 *gpt = data + BLOCK;
	char sig[9];
	ascii8(sig, gpt);
	u64 cur = le64(gpt + 24), alt = le64(gpt + 32);
	u64 first = le64(gpt + 40), last = le64(gpt + 48);
	u64 elba = le64(gpt + 72);
	u32 nent = le32(gpt + 80), esz = le32(gpt + 84);
	logf("LUN%d GPT: sig=%s cur=%x alt=%x first=%x last=%x", (u64)lun,
	     sig, cur, alt, first, last);
	logf("LUN%d GPT: entries=%x esz=%x nent=%x", (u64)lun, elba,
	     (u64)esz, (u64)nent);
	if (esz != 128 || !nent || nent > 128) {
		logf("LUN%d GPT: unsupported layout", (u64)lun);
		return;
	}
	list_partitions(lun, elba, nent);
}

static void probe_lun(u32 lun)
{
	logf("=== LUN %d ===", (u64)lun);
	u32 rc = tur(lun);
	logf("LUN%d TUR: rc=%x ocs=%x st=%x sense=%x", (u64)lun,
	     (u64)rc, (u64)last_ocs, (u64)last_st, (u64)last_sense);
	if (rc)
		return;

	u32 last_lba = 0, blk = 0;
	rc = read_capacity(lun, &last_lba, &blk);
	logf("LUN%d CAP: rc=%x ocs=%x st=%x last=%x blk=%x", (u64)lun,
	     (u64)rc, (u64)last_ocs, (u64)last_st, (u64)last_lba,
	     (u64)blk);
	if (rc)
		return;

	rc = read_blocks(lun, 0, 1, data);
	logf("LUN%d READ0: rc=%x ocs=%x st=%x", (u64)lun, (u64)rc,
	     (u64)last_ocs, (u64)last_st);
	if (rc)
		return;

	char a[9], b[9];
	ascii8(a, data);
	ascii8(b, data + 8);
	logf("LUN%d data0: '%s' '%s' %x %x %x %x", (u64)lun, a, b,
	     (u64)data[0], (u64)data[1], (u64)data[2], (u64)data[3]);
	if (name_eq(a, "UFS_BOOT"))
		logf("LUN%d: UFS_BOOT signature", (u64)lun);

	if (lun == 2)
		dump_gpt(lun);
}

void ufs_test(void)
{
	logf("UFS TEST: base=%x (mediatek,mt8183-ufshci)", (u64)UFS_BASE);
	logf("UFS: cntfrq=%d", timer_freq());

	u32 cap = ufs_rd(REG_CAP), ver = ufs_rd(REG_VER);
	u32 hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	u32 is = ufs_rd(REG_IS), ie = ufs_rd(REG_IE);

	logf("UFS: cap=%x ver=%x", (u64)cap, (u64)ver);
	logf("UFS: hce=%x hcs=%x is=%x ie=%x", (u64)hce, (u64)hcs,
	     (u64)is, (u64)ie);
	logf("UFS: dp=%d utrlrdy=%d ucrdy=%d nutrs=%d",
	     (u64)((hcs & HCS_DP) ? 1 : 0),
	     (u64)((hcs & HCS_UTRLRDY) ? 1 : 0),
	     (u64)((hcs & HCS_UCRDY) ? 1 : 0),
	     (u64)((cap & 0x1f) + 1));
	logf("UFS: utrlba=%x/%x dbr=%x rsr=%x", (u64)ufs_rd(REG_UTRLBA),
	     (u64)ufs_rd(REG_UTRLBAU), (u64)ufs_rd(REG_UTRLDBR),
	     (u64)ufs_rd(REG_UTRLRSR));
	logf("UFS: ucdlba=%x/%x ucddbr=%x", (u64)ufs_rd(REG_UCDLBA),
	     (u64)ufs_rd(REG_UCDLBAU), (u64)ufs_rd(REG_UCDDBR));
	logf("UFS: uiccmd=%x arg=%x a1=%x a2=%x a3=%x", (u64)ufs_rd(REG_UICCMD),
	     (u64)ufs_rd(REG_UICARG), (u64)ufs_rd(REG_UICARG1),
	     (u64)ufs_rd(REG_UICARG2), (u64)ufs_rd(REG_UICARG3));
	logf("UFS: ahit=%x uecpa=%x uecba=%x uecc=%x", (u64)ufs_rd(REG_AHIT),
	     (u64)ufs_rd(REG_UECPA), (u64)ufs_rd(REG_UECBA),
	     (u64)ufs_rd(REG_UECC));

	if (!(hce & 1u)) {
		logf("UFS: HCE=0 -> host disabled by LK; stop (read-only)");
		return;
	}
	if (!(hcs & HCS_DP)) {
		logf("UFS: DP=0 -> link down/hibernated; stop (read-only)");
		return;
	}
	if (!(hcs & HCS_UTRLRDY)) {
		logf("UFS: UTRLRDY=0 -> host not ready; stop (read-only)");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		logf("UFS: UTRLDBR=%x outstanding; stop (read-only)",
		     (u64)ufs_rd(REG_UTRLDBR));
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	u64 list = (u64)(unsigned long)utrl;
	ufs_wr(REG_UTRLBA, (u32)list);
	ufs_wr(REG_UTRLBAU, (u32)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	logf("UFS: own utrl=%x rsr=%x", list, (u64)ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever LK left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (u32)list ||
	    ufs_rd(REG_UTRLBAU) != (u32)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1u)) {
		logf("UFS: list take-over failed; stop (no doorbell rung)");
		return;
	}

	probe_lun(0);
	probe_lun(1);
	probe_lun(2);
	logf("UFS TEST: end");
}
