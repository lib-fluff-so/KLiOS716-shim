/* Host tests for the exact freestanding CPIO/payload parser used by the shim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/loader.h"

static unsigned char *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    unsigned char *p = malloc((size_t)n ? (size_t)n : 1);
    if (!p) { fclose(f); return NULL; }
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f); *size = (size_t)n; return p;
}

static size_t make_trailer(unsigned char *out, size_t cap)
{
    static const unsigned values[13] = {
        0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0
    };
    const char name[] = "TRAILER!!!";
    char h[111];
    if (cap < 512) return 0;
    memcpy(h, "070701", 6);
    for (unsigned i = 0; i < 13; i++)
        snprintf(h + 6 + i * 8, 9, "%08X", values[i]);
    memcpy(out, h, 110);
    memcpy(out + 110, name, sizeof name);
    size_t n = 110 + sizeof name;
    n = (n + 3u) & ~3u;
    memset(out + n, 0, 512 - n);
    return 512;
}

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s ramdisk.cpio payload.bin\n", argv[0]); return 2; }
    size_t ramdisk_size = 0, expected_size = 0;
    unsigned char *ramdisk = read_all(argv[1], &ramdisk_size);
    unsigned char *expected = read_all(argv[2], &expected_size);
    if (!ramdisk || !expected) { free(ramdisk); free(expected); return 2; }

    const loader_u8 *file = NULL, *payload = NULL;
    loader_u32 file_size = 0, payload_size = 0, expected_crc = 0;
    int rc = loader_cpio_find_file(ramdisk, ramdisk_size, "ios-kl7-test.bin", &file, &file_size);
    CHECK(rc == 0 && file, "find payload in generated CPIO");
    rc = loader_parse_payload(file, file_size, &payload, &payload_size, &expected_crc);
    CHECK(rc == 0, "validate payload header and CRC32");
    CHECK(payload_size == expected_size, "payload size matches source binary");
    CHECK(memcmp(payload, expected, expected_size) == 0, "payload bytes match source binary");
    CHECK(loader_crc32(payload, payload_size) == expected_crc, "CRC32 helper result");

    /* A preceding valid CPIO archive ending in TRAILER!!! must not hide the
     * payload in the next concatenated archive. */
    size_t prefix_size = 512;
    unsigned char *combined = malloc(prefix_size + ramdisk_size);
    CHECK(combined != NULL, "allocate concatenated CPIO test");
    CHECK(make_trailer(combined, prefix_size) == prefix_size, "make CPIO trailer prefix");
    memcpy(combined + prefix_size, ramdisk, ramdisk_size);
    const loader_u8 *concat_file = NULL;
    loader_u32 concat_size = 0;
    rc = loader_cpio_find_file(combined, prefix_size + ramdisk_size,
                               "ios-kl7-test.bin", &concat_file, &concat_size);
    CHECK(rc == 0 && concat_file && concat_size == file_size,
          "find payload after a preceding CPIO trailer");
    free(combined);

    unsigned char *damaged = malloc(file_size);
    CHECK(damaged != NULL, "allocate CRC corruption test");
    memcpy(damaged, file, file_size);
    damaged[file_size - 1] ^= 0x80;
    const loader_u8 *bad_payload = NULL;
    loader_u32 bad_size = 0, bad_crc = 0;
    rc = loader_parse_payload(damaged, file_size, &bad_payload, &bad_size, &bad_crc);
    CHECK(rc == -3, "reject corrupted payload by CRC32");
    free(damaged);

    printf("RESULT: loader parser ok; CPIO=%zu bytes, payload=%zu bytes, CRC32=%08x\n",
           ramdisk_size, expected_size, expected_crc);
    free(ramdisk); free(expected);
    return 0;
}
