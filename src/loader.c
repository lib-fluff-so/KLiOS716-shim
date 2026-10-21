/* Freestanding CPIO and payload validation; intentionally no libc dependency. */
#include "loader.h"

static int magic_eq(const loader_u8 *p, const char *s, loader_u32 n)
{
    for (loader_u32 i = 0; i < n; i++) if (p[i] != (loader_u8)s[i]) return 0;
    return 1;
}

static int hex32_field(const loader_u8 *p, loader_u32 *out)
{
    loader_u32 v = 0;
    for (loader_u32 i = 0; i < 8; i++) {
        loader_u8 c = p[i];
        loader_u32 d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

static loader_u32 le32(const loader_u8 *p)
{
    return (loader_u32)p[0] | ((loader_u32)p[1] << 8) |
           ((loader_u32)p[2] << 16) | ((loader_u32)p[3] << 24);
}

loader_u32 loader_crc32(const loader_u8 *p, loader_u32 len)
{
    loader_u32 crc = 0xffffffffu;
    for (loader_u32 i = 0; i < len; i++) {
        crc ^= p[i];
        for (loader_u32 b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (loader_u32)-(int)(crc & 1));
    }
    return ~crc;
}

static int cpio_name_eq(const loader_u8 *name, loader_u32 namesize, const char *want)
{
    loader_u32 n = 0;
    while (want[n]) n++;
    if (namesize != n + 1 || name[n] != 0) return 0;
    for (loader_u32 i = 0; i < n; i++) if (name[i] != (loader_u8)want[i]) return 0;
    return 1;
}

int loader_cpio_find_file(const loader_u8 *src, loader_u64 len,
                          const char *want, const loader_u8 **file_data,
                          loader_u32 *file_size)
{
    if (!src || !want || !file_data || !file_size || len < 110 || len > 0x10000000UL)
        return -1;

    /* CPIO streams can be concatenated. After a TRAILER!!! or padding, scan
     * forward for the next valid header instead of stopping at the first one. */
    for (loader_u64 off = 0; off + 110 <= len;) {
        const loader_u8 *h = src + off;
        if (!magic_eq(h, "070701", 6) && !magic_eq(h, "070702", 6)) {
            off++;
            continue;
        }
        loader_u32 fsize = 0, nsize = 0;
        if (!hex32_field(h + 54, &fsize) || !hex32_field(h + 94, &nsize) ||
            !nsize || (loader_u64)nsize > len - off - 110) {
            off++;
            continue;
        }
        loader_u64 name_off = off + 110;
        loader_u64 data_off = (name_off + nsize + 3) & ~3UL;
        if (data_off > len || (loader_u64)fsize > len - data_off) {
            off++;
            continue;
        }
        if (cpio_name_eq(src + name_off, nsize, want)) {
            *file_data = src + data_off;
            *file_size = fsize;
            return 0;
        }
        loader_u64 next = (data_off + fsize + 3) & ~3UL;
        if (next > off && next <= len) off = next;
        else off++;
    }
    return -2;
}

int loader_parse_payload(const loader_u8 *file, loader_u32 file_size,
                         const loader_u8 **payload, loader_u32 *payload_size,
                         loader_u32 *expected_crc)
{
    if (!file || !payload || !payload_size || !expected_crc ||
        file_size <= LOADER_PAYLOAD_HEADER_SIZE ||
        !magic_eq(file, LOADER_PAYLOAD_MAGIC, 8)) return -1;

    loader_u32 n = le32(file + 8);
    loader_u32 crc = le32(file + 12);
    if (!n || n > LOADER_PAYLOAD_MAX || n != file_size - LOADER_PAYLOAD_HEADER_SIZE)
        return -2;
    const loader_u8 *body = file + LOADER_PAYLOAD_HEADER_SIZE;
    if (loader_crc32(body, n) != crc) return -3;
    *payload = body;
    *payload_size = n;
    *expected_crc = crc;
    return 0;
}
