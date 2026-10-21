/* Host-side adversarial tests for the exact freestanding SWEETXNU parser. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../src/xnu_payload.h"

#define BLOB_SIZE 0x2e8u
#define SEG_TABLE 0x2a8u
#define SEG_FILE  0x2d0u
#define ADT_OFF   0x2e0u

static void put32(unsigned char *p, uint32_t x)
{
    p[0] = (unsigned char)x; p[1] = (unsigned char)(x >> 8);
    p[2] = (unsigned char)(x >> 16); p[3] = (unsigned char)(x >> 24);
}

static void put64(unsigned char *p, uint64_t x)
{
    put32(p, (uint32_t)x); put32(p + 4, (uint32_t)(x >> 32));
}

#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static void make_blob(unsigned char *b, size_t size)
{
    memset(b, 0, size);
    memcpy(b, "SWEETXNU", 8);
    put32(b + 8, 1);
    put32(b + 12, 1);
    put64(b + 0x10, 0xfffffff000000000ULL);
    put64(b + 0x18, 0xfffffff000004100ULL);
    put64(b + 0x20, ADT_OFF);
    put64(b + 0x28, 8);
    put64(b + 0x30, 0xfffffff000008000ULL);
    put64(b + 0x38, 0x140000000ULL);
    put64(b + 0x40, 0xffeff000ULL);
    memcpy(b + 0x48, "debug=0xc cpus=1 keepsyms=1", 28);
    put64(b + SEG_TABLE + 0, 0xfffffff000004000ULL); /* vmaddr */
    put64(b + SEG_TABLE + 8, 0x4000);               /* vmsize */
    put64(b + SEG_TABLE + 16, SEG_FILE);            /* file_off */
    put64(b + SEG_TABLE + 24, 16);                  /* file_size */
    memset(b + SEG_FILE, 0xa5, 16);
    /* Empty ADT root node: two little-endian u32 counts. */
    memset(b + ADT_OFF, 0, 8);
}

static int test_external_payload(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long n = ftell(f);
    if (n < 0 || (unsigned long)n > 0x10000000UL || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); fprintf(stderr, "FAIL: invalid external fixture size\n"); return 1;
    }
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return 1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); fprintf(stderr, "FAIL: short fixture read\n"); return 1;
    }
    fclose(f);
    struct xnu_payload_view p;
    int rc = xnu_payload_parse(buf, (loader_u64)n, &p);
    if (rc) {
        fprintf(stderr, "FAIL: generated SWEETXNU rejected by C parser (rc=%d)\n", rc);
        free(buf); return 1;
    }
    for (loader_u32 i = 0; i < p.nsegs; i++) {
        struct xnu_payload_segment s;
        if (xnu_payload_get_segment(&p, i, &s) != 0) {
            fprintf(stderr, "FAIL: generated segment %u cannot be read\n", i);
            free(buf); return 1;
        }
    }
    printf("RESULT: generated SWEETXNU accepted by C parser; %u segments, %ld bytes\n",
           p.nsegs, n);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char b[BLOB_SIZE];
    struct xnu_payload_view p;
    struct xnu_payload_segment s;
    make_blob(b, sizeof b);

    CHECK(xnu_payload_parse(b, sizeof b, &p) == 0, "valid container accepted");
    CHECK(p.version == 1 && p.nsegs == 1, "version and segment count decoded");
    CHECK(p.phys_base == 0x140000000ULL && p.mem_size == 0xffeff000ULL,
          "KL7 physical window decoded");
    CHECK(p.entry_va == 0xfffffff000004100ULL, "entry decoded");
    CHECK(xnu_payload_get_segment(&p, 0, &s) == 0, "segment accessor succeeds");
    CHECK(s.vmaddr == p.entry_va - 0x100 && s.vmsize == 0x4000 &&
          s.file_off == SEG_FILE && s.file_size == 16, "segment fields decoded");
    CHECK(xnu_payload_get_segment(&p, 1, &s) < 0, "out-of-range segment index rejected");

    b[0] ^= 1;
    CHECK(xnu_payload_parse(b, sizeof b, &p) == -2, "bad magic rejected");
    make_blob(b, sizeof b);
    put32(b + 12, XNU_PAYLOAD_MAX_SEGS + 1);
    CHECK(xnu_payload_parse(b, sizeof b, &p) == -3, "unreasonable segment count rejected");
    make_blob(b, sizeof b);
    put64(b + 0x20, sizeof b - 4);
    CHECK(xnu_payload_parse(b, sizeof b, &p) == -5, "ADT range beyond container rejected");
    make_blob(b, sizeof b);
    put64(b + 0x18, 0xfffffff00000c100ULL);
    CHECK(xnu_payload_parse(b, sizeof b, &p) == -12, "entry outside every segment rejected");
    make_blob(b, sizeof b);
    put64(b + SEG_TABLE + 16, sizeof b - 8);
    CHECK(xnu_payload_parse(b, sizeof b, &p) < 0, "segment bytes past ADT/container rejected");
    CHECK(xnu_payload_parse(NULL, sizeof b, &p) < 0, "null input rejected");

    puts("RESULT: SWEETXNU container parser tests passed");
    if (argc == 2) return test_external_payload(argv[1]);
    if (argc != 1) {
        fprintf(stderr, "usage: %s [generated-xnu-payload.bin]\n", argv[0]);
        return 2;
    }
    return 0;
}
