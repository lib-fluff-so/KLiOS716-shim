/* ios-kl7 memory map + 16 KiB-granule stage-1 identity map (v0.6.0).
 * mm.c is freestanding and also builds on a host (-DMM_HOST) so the DTB
 * parser and the table builder can be tested against the real fdt.dtb. */
#ifndef MM_H
#define MM_H

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

typedef void (*mm_log_fn)(const char *fmt, ...);

#define MM_MAX_RAM   4
#define MM_MAX_NOMAP 64
#define MM_MAX_CPU   8

#define MM_GRAN      0x4000UL       /* 16 KiB translation granule */
#define MM_BLOCK     0x2000000UL    /* 32 MiB level-2 block */
#define MM_DEV_END   0x40000000UL   /* [0, 1 GiB) is MMIO on MT6789 */

struct mm_range { u64 base, size; };

struct mm_dtb_info {
    struct mm_range ram[MM_MAX_RAM];
    u32 nram;
    struct mm_range nomap[MM_MAX_NOMAP];   /* /reserved-memory children with no-map */
    u32 nnomap;
    u32 nrsv_mapped;                       /* /reserved-memory children without no-map */
    u32 nrsvmap;                           /* FDT memory reservation block entries */
    u32 ncpu;
    u64 cpu_mpidr[MM_MAX_CPU];             /* /cpus/cpu@N reg values (Aff3..Aff0) */
};

/* 0 on success, negative on a structural problem (see mm.c). */
int mm_parse_dtb(const u8 *dtb, struct mm_dtb_info *out);

struct mm_tables {
    u64 ttbr0, tcr, mair;
    u32 tables_used;
    u32 oom;
};

/* Identity map: [0,1GiB) Device-nGnRnE, every /memory range Normal
 * Non-cacheable, minus every no-map reservation, plus `forced` (mapped even
 * if reserved, used for ramoops) and `image` (executable). */
int mm_build(const struct mm_dtb_info *i, u64 pool_base, u64 pool_size,
             const struct mm_range *forced, const struct mm_range *image,
             u64 parange, struct mm_tables *t);

/* Software walk of the tables built above. Returns 1 and the output address
 * and raw descriptor if `va` is mapped, else 0. */
int mm_translate(const struct mm_tables *t, u64 va, u64 *pa, u64 *desc);

#ifndef MM_HOST
void mm_enable(const struct mm_tables *t);
#endif

#endif
