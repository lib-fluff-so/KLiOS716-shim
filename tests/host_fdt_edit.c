#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/boot/fdt_edit.h"

#define MAGIC 0xd00dfeedu
#define BEGIN_NODE 1u
#define END_NODE 2u
#define PROP 3u
#define END 9u

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned int get32(const unsigned char *p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | p[3];
}

static unsigned long long get64(const unsigned char *p)
{
    return ((unsigned long long)get32(p) << 32) | get32(p + 4);
}

static void put_token(unsigned char *p, unsigned int token)
{
    put32(p, token);
}

static unsigned int add_node(unsigned char *p, unsigned int off, const char *name)
{
    unsigned int n = (unsigned int)strlen(name) + 1u;
    put_token(p + off, BEGIN_NODE);
    off += 4;
    memcpy(p + off, name, n);
    off += (n + 3u) & ~3u;
    return off;
}

static unsigned int add_prop(unsigned char *p, unsigned int off,
                             unsigned int nameoff, const char *value)
{
    unsigned int n = (unsigned int)strlen(value) + 1u;
    put_token(p + off, PROP);
    put32(p + off + 4u, n);
    put32(p + off + 8u, nameoff);
    off += 12u;
    memcpy(p + off, value, n);
    off += (n + 3u) & ~3u;
    return off;
}

static unsigned int build_dtb(unsigned char *blob, int with_chosen)
{
    static const char strings[] =
        "bootargs\0linux,initrd-start\0linux,initrd-end\0";
    const unsigned int strings_size = sizeof(strings) - 1u;
    const unsigned int bootargs_off = 0u;
    unsigned int start_off = (unsigned int)strlen("bootargs") + 1u;
    unsigned int end_off = start_off + (unsigned int)strlen("linux,initrd-start") + 1u;
    unsigned int off = 56u;

    memset(blob, 0, 2048u);
    off = add_node(blob, off, "");
    if (with_chosen) {
        off = add_node(blob, off, "chosen");
        off = add_prop(blob, off, bootargs_off, "old=1");
        put_token(blob + off, END_NODE);
        off += 4u;
    }
    put_token(blob + off, END_NODE);
    off += 4u;
    put_token(blob + off, END);
    off += 4u;

    unsigned int strings_off = (off + 3u) & ~3u;
    memcpy(blob + strings_off, strings, strings_size);
    put32(blob, MAGIC);
    put32(blob + 4u, strings_off + strings_size);
    put32(blob + 8u, 56u);
    put32(blob + 12u, strings_off);
    put32(blob + 16u, 40u);
    put32(blob + 20u, 17u);
    put32(blob + 24u, 16u);
    put32(blob + 28u, 0u);
    put32(blob + 32u, strings_size);
    put32(blob + 36u, off - 56u);
    (void)start_off;
    (void)end_off;
    return strings_off + strings_size;
}

static int property_equals(const unsigned char *dtb, const char *property,
                           const char *expected)
{
    unsigned int strings_off = get32(dtb + 12u);
    unsigned int strings_size = get32(dtb + 32u);
    unsigned int struct_off = get32(dtb + 8u);
    unsigned int struct_size = get32(dtb + 36u);
    const unsigned char *strings = dtb + strings_off;
    const unsigned char *p = dtb + struct_off;
    const unsigned char *end = p + struct_size;
    unsigned int depth = 0;
    while (p + 4u <= end) {
        unsigned int token = get32(p);
        p += 4u;
        if (token == BEGIN_NODE) {
            while (p < end && *p) p++;
            if (p == end) return 0;
            p++;
            p = (const unsigned char *)(((unsigned long)p + 3u) & ~3ul);
            depth++;
        } else if (token == END_NODE) {
            if (depth) depth--;
        } else if (token == PROP) {
            if (p + 8u > end) return 0;
            unsigned int len = get32(p);
            unsigned int nameoff = get32(p + 4u);
            p += 8u;
            if (len > (unsigned int)(end - p) || nameoff >= strings_size)
                return 0;
            if (depth == 2u && !strcmp((const char *)(strings + nameoff), property))
                return len == strlen(expected) + 1u &&
                       !memcmp(p, expected, len);
            p = (const unsigned char *)(((unsigned long)(p + len) + 3u) & ~3ul);
        } else if (token == END) {
            break;
        } else {
            return 0;
        }
    }
    return 0;
}

static int property_u64(const unsigned char *dtb, const char *property,
                        unsigned long long expected)
{
    unsigned int strings_off = get32(dtb + 12u);
    unsigned int strings_size = get32(dtb + 32u);
    unsigned int struct_off = get32(dtb + 8u);
    unsigned int struct_size = get32(dtb + 36u);
    const unsigned char *strings = dtb + strings_off;
    const unsigned char *p = dtb + struct_off;
    const unsigned char *end = p + struct_size;
    unsigned int depth = 0;
    while (p + 4u <= end) {
        unsigned int token = get32(p);
        p += 4u;
        if (token == BEGIN_NODE) {
            while (p < end && *p) p++;
            if (p == end) return 0;
            p++;
            p = (const unsigned char *)(((unsigned long)p + 3u) & ~3ul);
            depth++;
        } else if (token == END_NODE) {
            if (depth) depth--;
        } else if (token == PROP) {
            if (p + 8u > end) return 0;
            unsigned int len = get32(p);
            unsigned int nameoff = get32(p + 4u);
            p += 8u;
            if (len > (unsigned int)(end - p) || nameoff >= strings_size)
                return 0;
            if (depth == 2u && !strcmp((const char *)(strings + nameoff), property))
                return len == 8u && get64(p) == expected;
            p = (const unsigned char *)(((unsigned long)(p + len) + 3u) & ~3ul);
        } else if (token == END) {
            break;
        } else {
            return 0;
        }
    }
    return 0;
}

static int run_case(int with_chosen)
{
    unsigned char source[2048], output[4096];
    unsigned int source_size = build_dtb(source, with_chosen);
    unsigned int output_size = 0;
    int rc = fdt_edit_android_chosen(source, source_size, output,
                                     sizeof(output),
                                     "console=ttyS0 androidboot.slot_suffix=_a",
                                     0x12345000ULL, 0x12367890ULL,
                                     &output_size);
    if (rc || output_size != get32(output + 4u) ||
        get32(output) != MAGIC ||
        !property_equals(output, "bootargs",
                         "console=ttyS0 androidboot.slot_suffix=_a") ||
        !property_u64(output, "linux,initrd-start", 0x12345000ULL) ||
        !property_u64(output, "linux,initrd-end", 0x12367890ULL)) {
        fprintf(stderr, "FDT edit case with_chosen=%d rc=%d size=%u\n",
                with_chosen, rc, output_size);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (run_case(1) || run_case(0))
        return 1;
    puts("FDT chosen edit tests: PASS");
    return 0;
}
