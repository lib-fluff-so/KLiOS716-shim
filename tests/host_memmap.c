#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/memmap.h"

static int range_contains(const struct mem_range *r, uint64_t base, uint64_t size)
{
    if (!r->size || size > UINT64_MAX - base || r->size > UINT64_MAX - r->base) return 0;
    return base >= r->base && base < r->base + r->size && size <= r->base + r->size - base;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s fdt.dtb\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    if (fseek(f, 0, SEEK_END) != 0) { perror("fseek"); fclose(f); return 2; }
    long n = ftell(f);
    if (n < 40 || n > 8 * 1024 * 1024) { fprintf(stderr, "invalid DTB size %ld\n", n); fclose(f); return 2; }
    rewind(f);
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return 2; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("fread"); free(buf); fclose(f); return 2; }
    fclose(f);

    struct memmap_info info;
    int rc = memmap_parse_dtb(buf, &info);
    if (rc) { fprintf(stderr, "memmap_parse_dtb failed: %d\n", rc); free(buf); return 1; }
    int target_in_ram = 0;
    for (uint32_t i = 0; i < info.nram; i++)
        if (range_contains(&info.ram[i], 0x140000000ULL, 0xffeff000ULL)) target_in_ram = 1;
    int ramoops_reserved = 0;
    for (uint32_t i = 0; i < info.nnomap; i++)
        if (range_contains(&info.nomap[i], 0x48090000ULL, 0xe0000ULL)) ramoops_reserved = 1;
    if (!target_in_ram || !info.nram || !info.nnomap || !ramoops_reserved) {
        fprintf(stderr, "expected KL7 RAM/reservation missing: ram=%u reserved=%u no-map=%u\n",
                info.nram, info.nreserved, info.nnomap);
        free(buf);
        return 1;
    }
    printf("RESULT: DTB %ld bytes; RAM=%u reserved=%u no-map=%u FDT reservations=%u; KL7 upper RAM and ramoops validated\n",
           n, info.nram, info.nreserved, info.nnomap, info.nfdt_reserved);

    uint8_t saved = buf[0];
    buf[0] = 0;
    rc = memmap_parse_dtb(buf, &info);
    buf[0] = saved;
    free(buf);
    if (!rc) { fprintf(stderr, "invalid FDT magic was accepted\n"); return 1; }
    puts("RESULT: invalid FDT magic rejected");
    return 0;
}
