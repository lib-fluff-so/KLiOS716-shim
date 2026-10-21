/* ios-kl7 mm.c: DTB memory-map parser and 16 KiB-granule identity map builder.
 *
 * 16 KiB granule, T0SZ=24 (40-bit VA): level 1 has 16 table-only entries
 * (64 GiB each), level 2 has 2048 entries (32 MiB blocks or level-3 tables),
 * level 3 has 2048 entries (16 KiB pages). All of KL7's DRAM and MMIO is below
 * 64 GiB, so a single level-2 table is enough.
 */
#include "mm.h"

#define ADDR_MASK   0x0000FFFFFFFFC000UL   /* output/table address bits [47:14] */
#define BLK_MASK    0x0000FFFFFE000000UL   /* level-2 block output address [47:25] */
#define D_BLOCK     1UL
#define D_TABLE     3UL                    /* also the level-3 page descriptor */

/* Attribute bits (lower: AttrIndx[4:2], AP[7:6], SH[9:8], AF[10]; upper: PXN 53, UXN 54).
 * MAIR: Attr0 = Device-nGnRnE, Attr1 = Normal Non-cacheable, Attr2 = Normal WB. */
#define AF          (1UL << 10)
#define SH_ISH      (3UL << 8)
#define SH_OSH      (2UL << 8)
#define PXN         (1UL << 53)
#define UXN         (1UL << 54)
#define ATTR_DEV    ((0UL << 2) | AF | SH_OSH | PXN | UXN)
#define ATTR_NC     ((1UL << 2) | AF | SH_ISH | PXN | UXN)
#define ATTR_NC_X   ((1UL << 2) | AF | SH_ISH | UXN)

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u64 be64(const u8 *p) { return ((u64)be32(p) << 32) | be32(p + 4); }

static int name_is(const u8 *name, const char *want)
{
    int i = 0;
    for (; want[i]; i++) if (name[i] != (u8)want[i]) return 0;
    return name[i] == 0 || name[i] == '@';
}
static int streq(const u8 *a, const char *b)
{
    int i = 0;
    for (; b[i]; i++) if (a[i] != (u8)b[i]) return 0;
    return a[i] == 0;
}

/* ---------------------------------------------------------------- DTB -- */

int mm_parse_dtb(const u8 *dtb, struct mm_dtb_info *o)
{
    u8 *z = (u8 *)o;
    for (u32 k = 0; k < sizeof(*o); k++) z[k] = 0;
    if (!dtb || be32(dtb) != 0xd00dfeed) return -1;

    u32 total = be32(dtb + 4), off_st = be32(dtb + 8), off_str = be32(dtb + 12), off_rsv = be32(dtb + 16);
    if (total < 64 || total > 0x800000) return -2;

    /* memory reservation block: pairs of be64 (addr,size), terminated by 0,0 */
    for (const u8 *r = dtb + off_rsv; r + 16 <= dtb + total; r += 16) {
        if (!be64(r) && !be64(r + 8)) break;
        o->nrsvmap++;
    }

    const u8 *p = dtb + off_st, *end = dtb + total;
    int depth = 0, in_rsv = 0, in_mem = 0, in_cpus = 0;
    int root_ac = 0, root_sc = 0;
    struct mm_range cr[4]; u32 ncr = 0; int cnomap = 0;     /* current reserved child */

    while (p + 4 <= end) {
        u32 tok = be32(p); p += 4;
        if (tok == 1) {                                     /* FDT_BEGIN_NODE */
            const u8 *nm = p; int n = 0;
            while (nm[n]) n++;
            p += (n + 4) & ~3;
            depth++;
            if (depth == 2) {
                in_rsv = name_is(nm, "reserved-memory");
                in_mem = name_is(nm, "memory");
                in_cpus = name_is(nm, "cpus");
            } else if (depth == 3 && in_rsv) {
                ncr = 0; cnomap = 0;
            }
        } else if (tok == 2) {                              /* FDT_END_NODE */
            if (depth == 3 && in_rsv) {
                if (cnomap) {
                    for (u32 k = 0; k < ncr; k++)
                        if (o->nnomap < MM_MAX_NOMAP) o->nomap[o->nnomap++] = cr[k];
                        else return -5;
                } else o->nrsv_mapped++;
            }
            if (depth == 2) in_rsv = in_mem = in_cpus = 0;
            depth--;
        } else if (tok == 3) {                              /* FDT_PROP */
            u32 len = be32(p), noff = be32(p + 4);
            const u8 *val = p + 8;
            p += 8 + ((len + 3) & ~3);
            const u8 *pn = dtb + off_str + noff;
            if (depth == 1) {
                if (streq(pn, "#address-cells")) root_ac = (int)be32(val);
                else if (streq(pn, "#size-cells")) root_sc = (int)be32(val);
            } else if (depth == 2 && in_mem && streq(pn, "reg")) {
                if (root_ac != 2 || root_sc != 2) return -3;
                for (u32 k = 0; k + 16 <= len && o->nram < MM_MAX_RAM; k += 16) {
                    o->ram[o->nram].base = be64(val + k);
                    o->ram[o->nram].size = be64(val + k + 8);
                    o->nram++;
                }
            } else if (depth == 3 && in_rsv) {
                if (streq(pn, "no-map")) cnomap = 1;
                else if (streq(pn, "reg")) {
                    if (root_ac != 2 || root_sc != 2) return -3;   /* reserved-memory inherits 2/2 here */
                    for (u32 k = 0; k + 16 <= len && ncr < 4; k += 16) {
                        cr[ncr].base = be64(val + k);
                        cr[ncr].size = be64(val + k + 8);
                        ncr++;
                    }
                }
            } else if (depth == 3 && in_cpus && streq(pn, "reg") && len >= 4) {
                /* /cpus has #address-cells = 1 on this SoC; accept 1 or 2 cells */
                if (o->ncpu < MM_MAX_CPU) o->cpu_mpidr[o->ncpu++] = len >= 8 ? be64(val) : be32(val);
            }
        } else if (tok == 4) {                              /* FDT_NOP */
        } else break;                                       /* FDT_END or garbage */
    }
    if (!o->nram) return -4;
    return 0;
}

/* ------------------------------------------------------- page tables -- */

static struct {
    u64 *l2;
    u64 pool, pool_end;
    u32 used, oom;
} g;

static u64 *pt_alloc(void)
{
    if (g.pool + MM_GRAN > g.pool_end) { g.oom = 1; return 0; }
    volatile u64 *t = (volatile u64 *)g.pool;
    g.pool += MM_GRAN;
    g.used++;
    for (u32 i = 0; i < MM_GRAN / 8; i++) t[i] = 0;
    return (u64 *)t;
}

/* level-3 table covering the 32 MiB block `i2`, splitting an existing block if needed */
static u64 *l3_for(u32 i2)
{
    u64 e = g.l2[i2];
    if ((e & 3) == D_TABLE) return (u64 *)(e & ADDR_MASK);
    u64 *l3 = pt_alloc();
    if (!l3) return 0;
    if ((e & 3) == D_BLOCK) {
        u64 base = e & BLK_MASK, attr = e & ~(ADDR_MASK | 3UL);
        for (u32 i = 0; i < 2048; i++) l3[i] = (base + (u64)i * MM_GRAN) | attr | D_TABLE;
    }
    g.l2[i2] = (u64)l3 | D_TABLE;
    return l3;
}

/* map [pa, pa+size) identity with `attr`, or unmap it when attr == 0 */
static void pt_set(u64 pa, u64 size, u64 attr)
{
    u64 lim = 1UL << 36;
    if (pa >= lim) return;
    if (size > lim - pa) size = lim - pa;
    while (size && !g.oom) {
        u32 i2 = (u32)((pa >> 25) & 0x7ff);
        u64 blk_base = pa & ~(MM_BLOCK - 1);
        if (!(pa & (MM_BLOCK - 1)) && size >= MM_BLOCK) {
            g.l2[i2] = attr ? (pa | attr | D_BLOCK) : 0;
            pa += MM_BLOCK; size -= MM_BLOCK;
            continue;
        }
        u64 *l3 = l3_for(i2);
        if (!l3) return;
        u64 n = blk_base + MM_BLOCK - pa;
        if (n > size) n = size;
        for (u64 a = pa; a < pa + n; a += MM_GRAN)
            l3[(a >> 14) & 0x7ff] = attr ? (a | attr | D_TABLE) : 0;
        pa += n; size -= n;
    }
}

static u64 down(u64 x) { return x & ~(MM_GRAN - 1); }
static u64 up(u64 x)   { return (x + MM_GRAN - 1) & ~(MM_GRAN - 1); }

int mm_build(const struct mm_dtb_info *i, u64 pool_base, u64 pool_size,
             const struct mm_range *forced, const struct mm_range *image,
             u64 parange, struct mm_tables *t)
{
    g.used = 0; g.oom = 0;
    g.pool = up(pool_base);
    g.pool_end = down(pool_base + pool_size);

    u64 *l1 = pt_alloc();
    g.l2 = pt_alloc();
    if (!l1 || !g.l2) return -1;
    l1[0] = (u64)g.l2 | D_TABLE;

    pt_set(0, MM_DEV_END, ATTR_DEV);
    for (u32 k = 0; k < i->nram; k++) pt_set(i->ram[k].base, i->ram[k].size, ATTR_NC);
    for (u32 k = 0; k < i->nnomap; k++)
        pt_set(down(i->nomap[k].base), up(i->nomap[k].base + i->nomap[k].size) - down(i->nomap[k].base), 0);
    if (forced && forced->size) pt_set(down(forced->base), up(forced->base + forced->size) - down(forced->base), ATTR_NC);
    if (image && image->size)  pt_set(down(image->base), up(image->base + image->size) - down(image->base), ATTR_NC_X);
    if (g.oom) return -2;

    if (parange > 5) parange = 5;
    t->ttbr0 = (u64)l1;
    /* T0SZ=24, table walks Non-cacheable + outer-shareable (so stale LK cache
     * lines can never be read as descriptors), TG0=16K, TTBR1 walks disabled,
     * TG1=16K to avoid a reserved encoding, IPS from ID_AA64MMFR0_EL1.PARange. */
    t->tcr = 24UL | (2UL << 12) | (2UL << 14) | (1UL << 23) | (1UL << 30) | (parange << 32);
    t->mair = 0x00UL | (0x44UL << 8) | (0xFFUL << 16);
    t->tables_used = g.used;
    t->oom = g.oom;
    return 0;
}

int mm_translate(const struct mm_tables *t, u64 va, u64 *pa, u64 *desc)
{
    if (va >> 40) return 0;
    u64 *l1 = (u64 *)(t->ttbr0 & ADDR_MASK);
    u64 e = l1[(va >> 36) & 0xf];
    if ((e & 3) != D_TABLE) return 0;
    u64 *l2 = (u64 *)(e & ADDR_MASK);
    e = l2[(va >> 25) & 0x7ff];
    if ((e & 3) == D_BLOCK) {
        if (pa) *pa = (e & BLK_MASK) | (va & (MM_BLOCK - 1));
        if (desc) *desc = e;
        return 1;
    }
    if ((e & 3) != D_TABLE) return 0;
    u64 *l3 = (u64 *)(e & ADDR_MASK);
    e = l3[(va >> 14) & 0x7ff];
    if ((e & 3) != D_TABLE) return 0;
    if (pa) *pa = (e & ADDR_MASK) | (va & (MM_GRAN - 1));
    if (desc) *desc = e;
    return 1;
}

#ifndef MM_HOST
/* Turn the MMU on with Non-cacheable Normal mappings. Caches stay off
 * (SCTLR.C = SCTLR.I = 0) so nothing depends on cache maintenance yet. The
 * running code, stack and vectors are identity-mapped, so execution continues
 * straight through the SCTLR write. */
void mm_enable(const struct mm_tables *t)
{
    u64 sctlr;
    __asm__ volatile(
        "dsb    sy\n"
        "msr    mair_el1, %1\n"
        "msr    tcr_el1, %2\n"
        "msr    ttbr0_el1, %3\n"
        "isb\n"
        "tlbi   vmalle1\n"
        "dsb    nsh\n"
        "isb\n"
        "ic     iallu\n"
        "dsb    nsh\n"
        "isb\n"
        "mrs    %0, sctlr_el1\n"
        "orr    %0, %0, #1\n"          /* M */
        "bic    %0, %0, #2\n"          /* A off */
        "bic    %0, %0, #4\n"          /* C off */
        "bic    %0, %0, #0x1000\n"     /* I off */
        "msr    sctlr_el1, %0\n"
        "isb\n"
        : "=&r"(sctlr)
        : "r"(t->mair), "r"(t->tcr), "r"(t->ttbr0)
        : "memory");
}
#endif
