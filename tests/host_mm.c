/* Host test for src/mm.c against the real KL7 DTB:  make hosttest */
#include <stdio.h>
#include <stdlib.h>
#include "../src/mm.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "fdt.dtb", "rb");
    if (!f) { perror("fdt"); return 2; }
    static u8 dtb[0x100000];
    size_t n = fread(dtb, 1, sizeof dtb, f); fclose(f);

    static struct mm_dtb_info di;
    int rc = mm_parse_dtb(dtb, &di);
    printf("parse rc=%d bytes=%zu ram=%u nomap=%u rsv_mapped=%u memrsv=%u cpus=%u\n",
           rc, n, di.nram, di.nnomap, di.nrsv_mapped, di.nrsvmap, di.ncpu);
    CHECK(rc == 0, "parse");
    CHECK(di.nram == 1 && di.ram[0].base == 0x40000000UL && di.ram[0].size == 0x200000000UL, "ram range");
    CHECK(di.nnomap == 30, "expected 30 no-map regions, got %u", di.nnomap);
    CHECK(di.ncpu == 8, "expected 8 cpus, got %u", di.ncpu);
    for (u32 i = 0; i < di.nnomap; i++) printf("  nomap %011lx +%08lx\n", di.nomap[i].base, di.nomap[i].size);
    for (u32 i = 0; i < di.ncpu; i++) printf("  cpu %lx\n", di.cpu_mpidr[i]);

    u64 pool_sz = 0x200000;
    u8 *pool = aligned_alloc(MM_GRAN, pool_sz);
    struct mm_range forced = { 0x48090000UL, 0xe0000UL };
    struct mm_range image = { 0x40000000UL, 0x14000UL };
    struct mm_tables t;
    rc = mm_build(&di, (u64)pool, pool_sz, &forced, &image, 2, &t);
    printf("build rc=%d tables=%u oom=%u tcr=%lx mair=%lx\n", rc, t.tables_used, t.oom, t.tcr, t.mair);
    CHECK(rc == 0, "build");

    struct { u64 va; int mapped; int exec; const char *what; } T[] = {
        { 0x00000000, 1, 0, "mmio low" },
        { 0x0c000000, 1, 0, "GICD" },
        { 0x10007000, 1, 0, "WDT" },
        { 0x3fffc000, 1, 0, "end of mmio" },
        { 0x40000000, 1, 1, "image base (exec)" },
        { 0x40014000, 1, 0, "after image (nx)" },
        { 0x47c80000, 1, 0, "DTB" },
        { 0x480EF000, 1, 0, "ramoops console (forced over no-map)" },
        { 0x48090000, 1, 0, "pstore start" },
        { 0x48170000, 0, 0, "minirdump no-map" },
        { 0x48080000, 0, 0, "aee_debug_kinfo no-map" },
        { 0x48200000, 0, 0, "BL31 no-map" },
        { 0x70000000, 0, 0, "TEE no-map" },
        { 0x66f00000, 1, 0, "initrd" },
        { 0xfdfff000, 1, 0, "framebuffer" },
        { 0xfe000000 + 1230UL * 4352, 1, 0, "fb visible" },
        { 0xffff7000, 0, 0, "tkv no-map" },
        { 0x13ffff000UL, 0, 0, "dramc-rk0" },
        { 0x23ffff000UL, 0, 0, "dramc-rk1" },
        { 0x200000000UL, 1, 0, "high RAM" },
        { 0x240000000UL, 0, 0, "above RAM" },
        { 0xbf000000, 0, 0, "wifi dma no-map" },
        { 0xc0000000, 1, 0, "plain RAM after reserved" },
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        u64 pa = 0, d = 0;
        int m = mm_translate(&t, T[i].va, &pa, &d);
        int x = m && !(d & (1UL << 53));
        printf("  %-38s va=%011lx mapped=%d pa=%011lx exec=%d\n", T[i].what, T[i].va, m, pa, x);
        CHECK(m == T[i].mapped, "%s: mapped=%d want %d", T[i].what, m, T[i].mapped);
        if (m) {
            CHECK(pa == T[i].va, "%s: not identity", T[i].what);
            CHECK(x == T[i].exec, "%s: exec=%d want %d", T[i].what, x, T[i].exec);
            int idx = (int)((d >> 2) & 7);
            CHECK(idx == (T[i].va < 0x40000000UL ? 0 : 1), "%s: attr idx %d", T[i].what, idx);
        }
    }
    /* sweep: every 16 KiB page of RAM must match "inside RAM and not inside a no-map (unless forced)" */
    u64 bad = 0, mapped_pages = 0;
    for (u64 a = 0x40000000UL; a < 0x240000000UL; a += MM_GRAN) {
        int want = 1;
        for (u32 k = 0; k < di.nnomap; k++) {
            u64 b = di.nomap[k].base & ~(MM_GRAN - 1), e = (di.nomap[k].base + di.nomap[k].size + MM_GRAN - 1) & ~(MM_GRAN - 1);
            if (a >= b && a < e) want = 0;
        }
        if (a >= forced.base && a < forced.base + forced.size) want = 1;
        u64 pa;
        int m = mm_translate(&t, a, &pa, 0);
        if (m != want) { if (bad++ < 5) printf("  sweep mismatch %lx got %d want %d\n", a, m, want); }
        mapped_pages += m;
    }
    printf("sweep: %lu mismatches, %lu pages mapped (%lu MiB)\n", bad, mapped_pages, mapped_pages * 16 / 1024);
    CHECK(bad == 0, "sweep");
    printf(fails ? "RESULT: %d FAILURES\n" : "RESULT: ok\n", fails);
    return fails != 0;
}
