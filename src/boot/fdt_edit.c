#include "fdt_edit.h"

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u
#define FDT_HEADER_SIZE 40u
#define FDT_MAX_SIZE 0x200000u

static fe_u32 be32(const fe_u8 *p)
{
    return ((fe_u32)p[0] << 24) | ((fe_u32)p[1] << 16) |
           ((fe_u32)p[2] << 8) | (fe_u32)p[3];
}

static void put_be32(fe_u8 *p, fe_u32 v)
{
    p[0] = (fe_u8)(v >> 24);
    p[1] = (fe_u8)(v >> 16);
    p[2] = (fe_u8)(v >> 8);
    p[3] = (fe_u8)v;
}

static void put_be64(fe_u8 *p, fe_u64 v)
{
    put_be32(p, (fe_u32)(v >> 32));
    put_be32(p + 4, (fe_u32)v);
}

static void copy_bytes(fe_u8 *dst, const fe_u8 *src, fe_u32 n)
{
    for (fe_u32 i = 0; i < n; i++) dst[i] = src[i];
}

static int string_equal(const fe_u8 *strings, fe_u32 size,
                        fe_u32 offset, const char *want)
{
    if (offset >= size) return 0;
    fe_u32 i = 0;
    while (want[i] && offset + i < size && strings[offset + i] == (fe_u8)want[i])
        i++;
    return !want[i] && offset + i < size && strings[offset + i] == 0;
}

static int string_find_or_append(const fe_u8 *strings, fe_u32 size,
                                 fe_u8 *append, fe_u32 *append_size,
                                 fe_u32 append_capacity, const char *name,
                                 fe_u32 *offset)
{
    fe_u32 pos = 0;
    while (pos < size) {
        fe_u32 end = pos;
        while (end < size && strings[end]) end++;
        if (end == size) return -1;
        fe_u32 i = 0;
        while (name[i] && pos + i < end && strings[pos + i] == (fe_u8)name[i])
            i++;
        if (!name[i] && pos + i == end) {
            *offset = pos;
            return 0;
        }
        pos = end + 1u;
    }

    for (fe_u32 i = 0; i < *append_size; ) {
        fe_u32 end = i;
        while (end < *append_size && append[end]) end++;
        if (end == *append_size) return -2;
        fe_u32 j = 0;
        while (name[j] && i + j < end && append[i + j] == (fe_u8)name[j])
            j++;
        if (!name[j] && i + j == end) {
            *offset = size + i;
            return 0;
        }
        i = end + 1u;
    }

    fe_u32 n = 0;
    while (name[n]) n++;
    if (n + 1u > append_capacity - *append_size)
        return -3;
    *offset = size + *append_size;
    for (fe_u32 i = 0; i < n; i++) append[(*append_size)++] = (fe_u8)name[i];
    append[(*append_size)++] = 0;
    return 0;
}

static int write_property(fe_u8 *dst, fe_u32 capacity, fe_u32 *position,
                          fe_u32 name_offset, const fe_u8 *value,
                          fe_u32 value_size)
{
    fe_u32 padded = (value_size + 3u) & ~3u;
    if (*position > capacity || 12u + padded > capacity - *position)
        return -1;
    put_be32(dst + *position, FDT_PROP);
    put_be32(dst + *position + 4u, value_size);
    put_be32(dst + *position + 8u, name_offset);
    *position += 12u;
    copy_bytes(dst + *position, value, value_size);
    for (fe_u32 i = value_size; i < padded; i++)
        dst[*position + i] = 0;
    *position += padded;
    return 0;
}

static int write_android_properties(fe_u8 *dst, fe_u32 capacity,
                                    fe_u32 *position, fe_u32 bootargs_name,
                                    fe_u32 start_name, fe_u32 end_name,
                                    const char *bootargs,
                                    fe_u64 initrd_start, fe_u64 initrd_end)
{
    fe_u8 value[8];
    fe_u32 n = 0;
    while (bootargs[n]) {
        if (n >= 4095u) return -1;
        n++;
    }
    if (write_property(dst, capacity, position, bootargs_name,
                       (const fe_u8 *)bootargs, n + 1u))
        return -2;
    put_be64(value, initrd_start);
    if (write_property(dst, capacity, position, start_name, value, 8u))
        return -3;
    put_be64(value, initrd_end);
    if (write_property(dst, capacity, position, end_name, value, 8u))
        return -4;
    return 0;
}

int fdt_edit_android_chosen(const fe_u8 *src, fe_u32 source_capacity,
                            fe_u8 *dst, fe_u32 dst_capacity,
                            const char *bootargs, fe_u64 initrd_start,
                            fe_u64 initrd_end, fe_u32 *result_size)
{
    fe_u8 appended_strings[256];
    fe_u32 appended_size = 0;
    fe_u32 bootargs_name, start_name, end_name;
    fe_u32 total, off_struct, off_strings, off_rsv, strings_size, struct_size;
    fe_u32 reserve_end, out_struct, out_pos;
    fe_u32 depth = 0;
    fe_u32 chosen_depth = 0;
    int chosen_found = 0, properties_written = 0, saw_end = 0;

    if (!src || !dst || !bootargs || !result_size || source_capacity < FDT_HEADER_SIZE)
        return -1;
    if (initrd_end <= initrd_start || be32(src) != FDT_MAGIC)
        return -2;

    total = be32(src + 4u);
    off_struct = be32(src + 8u);
    off_strings = be32(src + 12u);
    off_rsv = be32(src + 16u);
    strings_size = be32(src + 32u);
    struct_size = be32(src + 36u);
    if (total < FDT_HEADER_SIZE || total > source_capacity || total > FDT_MAX_SIZE ||
        off_struct > total || struct_size > total - off_struct ||
        off_strings > total || strings_size > total - off_strings ||
        off_rsv < FDT_HEADER_SIZE || off_rsv > total || (off_rsv & 7u))
        return -3;

    const fe_u8 *strings = src + off_strings;
    if (string_find_or_append(strings, strings_size, appended_strings,
                              &appended_size, sizeof(appended_strings),
                              "bootargs", &bootargs_name) ||
        string_find_or_append(strings, strings_size, appended_strings,
                              &appended_size, sizeof(appended_strings),
                              "linux,initrd-start", &start_name) ||
        string_find_or_append(strings, strings_size, appended_strings,
                              &appended_size, sizeof(appended_strings),
                              "linux,initrd-end", &end_name))
        return -4;

    reserve_end = off_rsv;
    for (;;) {
        if (reserve_end > total || 16u > total - reserve_end)
            return -5;
        fe_u64 address = ((fe_u64)be32(src + reserve_end) << 32) |
                         be32(src + reserve_end + 4u);
        fe_u64 size = ((fe_u64)be32(src + reserve_end + 8u) << 32) |
                      be32(src + reserve_end + 12u);
        reserve_end += 16u;
        if (!address && !size) break;
    }

    if (dst_capacity > FDT_MAX_SIZE)
        dst_capacity = FDT_MAX_SIZE;
    if (dst_capacity < FDT_HEADER_SIZE || reserve_end > dst_capacity)
        return -6;
    copy_bytes(dst, src, FDT_HEADER_SIZE);
    copy_bytes(dst + FDT_HEADER_SIZE, src + off_rsv, reserve_end - off_rsv);

    out_struct = (reserve_end + 3u) & ~3u;
    out_pos = out_struct;
    if (out_pos > dst_capacity)
        return -7;

    const fe_u8 *p = src + off_struct;
    const fe_u8 *end = p + struct_size;
    while (p + 4u <= end) {
        fe_u32 token = be32(p);
        fe_u32 token_size = 4u;
        if (token == FDT_BEGIN_NODE) {
            const fe_u8 *nul = p + 4u;
            while (nul < end && *nul) nul++;
            if (nul == end) return -8;
            fe_u32 name_len = (fe_u32)(nul - (p + 4u));
            token_size += (name_len + 1u + 3u) & ~3u;
            if (token_size > (fe_u32)(end - p)) return -8;
            depth++;
            if (depth == 2u && name_len == 6u &&
                p[4] == 'c' && p[5] == 'h' && p[6] == 'o' &&
                p[7] == 's' && p[8] == 'e' && p[9] == 'n') {
                chosen_found = 1;
                chosen_depth = depth;
            }
        } else if (token == FDT_PROP) {
            if (p + 12u > end) return -9;
            fe_u32 len = be32(p + 4u);
            fe_u32 nameoff = be32(p + 8u);
            fe_u32 padded = (len + 3u) & ~3u;
            token_size = 12u + padded;
            if (token_size > (fe_u32)(end - p) ||
                nameoff >= strings_size)
                return -9;
            if (chosen_found && !properties_written && depth == chosen_depth &&
                (string_equal(strings, strings_size, nameoff, "bootargs") ||
                 string_equal(strings, strings_size, nameoff, "linux,initrd-start") ||
                 string_equal(strings, strings_size, nameoff, "linux,initrd-end"))) {
                p += token_size;
                continue;
            }
        } else if (token == FDT_END_NODE) {
            if (!depth) return -10;
            if (chosen_found && !properties_written && depth == chosen_depth) {
                int rc = write_android_properties(dst, dst_capacity, &out_pos,
                                                  bootargs_name, start_name,
                                                  end_name, bootargs,
                                                  initrd_start, initrd_end);
                if (rc) return -11 + rc;
                properties_written = 1;
            } else if (depth == 1u && !chosen_found) {
                static const fe_u8 chosen_name[8] = {
                    'c','h','o','s','e','n',0,0
                };
                if (out_pos > dst_capacity || 16u > dst_capacity - out_pos)
                    return -17;
                put_be32(dst + out_pos, FDT_BEGIN_NODE);
                out_pos += 4u;
                copy_bytes(dst + out_pos, chosen_name, sizeof(chosen_name));
                out_pos += sizeof(chosen_name);
                int rc = write_android_properties(dst, dst_capacity, &out_pos,
                                                  bootargs_name, start_name,
                                                  end_name, bootargs,
                                                  initrd_start, initrd_end);
                if (rc) return -18 + rc;
                put_be32(dst + out_pos, FDT_END_NODE);
                out_pos += 4u;
                chosen_found = 1;
                properties_written = 1;
            }
            depth--;
        } else if (token == FDT_NOP) {
            token_size = 4u;
        } else if (token == FDT_END) {
            token_size = 4u;
            saw_end = 1;
            if (!chosen_found) {
                return -12;
            }
        } else {
            return -13;
        }

        if (token_size > (fe_u32)(end - p) ||
            out_pos > dst_capacity || token_size > dst_capacity - out_pos)
            return -14;
        copy_bytes(dst + out_pos, p, token_size);
        out_pos += token_size;
        p += token_size;
        if (saw_end)
            break;
    }
    if (!saw_end || !chosen_found || !properties_written)
        return -15;

    fe_u32 new_struct_size = out_pos - out_struct;
    fe_u32 new_strings_off = (out_pos + 3u) & ~3u;
    fe_u32 new_strings_size = strings_size + appended_size;
    if (new_strings_off > dst_capacity ||
        new_strings_size > dst_capacity - new_strings_off)
        return -16;
    copy_bytes(dst + new_strings_off, strings, strings_size);
    copy_bytes(dst + new_strings_off + strings_size,
               appended_strings, appended_size);

    put_be32(dst + 4u, new_strings_off + new_strings_size);
    put_be32(dst + 8u, out_struct);
    put_be32(dst + 12u, new_strings_off);
    put_be32(dst + 16u, FDT_HEADER_SIZE);
    put_be32(dst + 32u, new_strings_size);
    put_be32(dst + 36u, new_struct_size);
    *result_size = new_strings_off + new_strings_size;
    return 0;
}
