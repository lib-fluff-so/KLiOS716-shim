#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/boot/android_kernel.h"

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || ftell(f) < 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    unsigned char *p = malloc((size_t)n ? (size_t)n : 1u);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static int check_fixture(const char *input_path, const char *expected_path)
{
    size_t input_size = 0, expected_size = 0;
    unsigned char *input = read_file(input_path, &input_size);
    unsigned char *expected = read_file(expected_path, &expected_size);
    unsigned char *output = malloc(expected_size + 1u);
    unsigned int output_size = 0;
    if (!input || !expected || !output) {
        free(input); free(expected); free(output);
        return -1;
    }
    int rc = android_kernel_unpack(input, (unsigned int)input_size,
                                   output, (unsigned int)(expected_size + 1u),
                                   &output_size);
    int ok = !rc && output_size == expected_size &&
             !memcmp(output, expected, expected_size);
    free(input); free(expected); free(output);
    return ok ? 0 : -2;
}

int main(int argc, char **argv)
{
    unsigned char raw[64] = {0};
    unsigned char raw_out[64] = {0};
    unsigned int raw_size = 0;
    memcpy(raw + 56, "ARM\x64", 4);
    if (android_kernel_unpack(raw, sizeof(raw), raw_out, sizeof(raw_out),
                              &raw_size) || raw_size != sizeof(raw) ||
        memcmp(raw, raw_out, sizeof(raw))) {
        fprintf(stderr, "raw ARM64 Image test failed\n");
        return 1;
    }

    const unsigned char bad[] = {0xde, 0xad, 0xbe, 0xef};
    if (android_kernel_unpack(bad, sizeof(bad), raw_out, sizeof(raw_out),
                              &raw_size) >= 0) {
        fprintf(stderr, "unsupported format was accepted\n");
        return 1;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT EXPECTED\n", argv[0]);
        return 2;
    }
    if (check_fixture(argv[1], argv[2])) {
        fprintf(stderr, "compressed kernel fixture test failed: %s\n", argv[1]);
        return 1;
    }
    puts("android kernel unpack tests: PASS");
    return 0;
}
