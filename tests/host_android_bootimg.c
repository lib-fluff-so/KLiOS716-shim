#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "android_bootimg.h"

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void test_boot_v4(void)
{
    unsigned char h[4096] = {0};
    struct android_bootimg_info info;

    memcpy(h, "ANDROID!", 8);
    put32(h + 8, 8192);
    put32(h + 12, 4096);
    put32(h + 20, 1584);
    put32(h + 40, 4);
    put32(h + 1580, 16);

    expect(android_bootimg_parse(h, 1024u * 1024u, &info) == 0,
           "boot v4 parses");
    expect(info.kernel_offset == 4096, "boot v4 kernel offset");
    expect(info.ramdisk_offset == 12288, "boot v4 ramdisk offset");
    expect(info.signature_offset == 16384, "boot v4 signature offset");
    expect(info.image_size == 20480, "boot v4 total size");
    expect(info.signature_size == 16, "boot v4 signature size");
}

static void test_boot_v3(void)
{
    unsigned char h[4096] = {0};
    struct android_bootimg_info info;

    memcpy(h, "ANDROID!", 8);
    put32(h + 8, 1);
    put32(h + 12, 0);
    put32(h + 20, 1580);
    put32(h + 40, 3);

    expect(android_bootimg_parse(h, 16384, &info) == 0,
           "boot v3 parses");
    expect(info.ramdisk_offset == 8192, "boot v3 page alignment");
    expect(info.signature_size == 0, "boot v3 has no signature section");
}

static void test_vendor_v4(void)
{
    unsigned char h[4096] = {0};
    struct android_vendor_boot_info info;

    memcpy(h, "VNDRBOOT", 8);
    put32(h + 8, 4);
    put32(h + 12, 4096);
    put32(h + 24, 5000);
    put32(h + 2096, 2128);
    put32(h + 2100, 1000);
    put32(h + 2112, 216);
    put32(h + 2116, 2);
    put32(h + 2120, 108);
    put32(h + 2124, 25);

    expect(android_vendor_boot_parse(h, 65536, &info) == 0,
           "vendor boot v4 parses");
    expect(info.ramdisk_offset == 4096, "vendor v4 ramdisk offset");
    expect(info.dtb_offset == 12288, "vendor v4 DTB offset");
    expect(info.ramdisk_table_offset == 16384, "vendor v4 table offset");
    expect(info.bootconfig_offset == 20480, "vendor v4 bootconfig offset");
    expect(info.image_size == 24576, "vendor v4 total size");
}

static void test_vendor_v3(void)
{
    unsigned char h[4096] = {0};
    struct android_vendor_boot_info info;

    memcpy(h, "VNDRBOOT", 8);
    put32(h + 8, 3);
    put32(h + 12, 4096);
    put32(h + 24, 1);
    put32(h + 2096, 2112);
    put32(h + 2100, 2);

    expect(android_vendor_boot_parse(h, 16384, &info) == 0,
           "vendor boot v3 parses");
    expect(info.ramdisk_offset == 4096, "vendor v3 ramdisk offset");
    expect(info.dtb_offset == 8192, "vendor v3 DTB offset");
    expect(info.image_size == 12288, "vendor v3 total size");
}

static void test_rejections(void)
{
    unsigned char h[4096] = {0};
    struct android_bootimg_info boot;
    struct android_vendor_boot_info vendor;

    expect(android_bootimg_parse(h, 65536, &boot) < 0,
           "bad boot magic rejected");

    memcpy(h, "ANDROID!", 8);
    put32(h + 20, 1584);
    put32(h + 40, 4);
    put32(h + 8, 1024u * 1024u);
    expect(android_bootimg_parse(h, 4096, &boot) < 0,
           "boot image larger than partition rejected");

    memset(h, 0, sizeof(h));
    memcpy(h, "VNDRBOOT", 8);
    put32(h + 8, 4);
    put32(h + 12, 4096);
    put32(h + 2096, 2128);
    put32(h + 2112, 216);
    put32(h + 2116, 2);
    put32(h + 2120, 107); /* Invalid entry size / inconsistent table. */
    expect(android_vendor_boot_parse(h, 65536, &vendor) < 0,
           "invalid vendor ramdisk table rejected");
}

int main(void)
{
    test_boot_v4();
    test_boot_v3();
    test_vendor_v4();
    test_vendor_v3();
    test_rejections();
    puts("Android boot image parser tests: PASS");
    return 0;
}
