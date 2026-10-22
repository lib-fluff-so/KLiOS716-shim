#include "memmap.h"

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u
#define MAX_DTB_SIZE    0x800000u
#define MAX_CHILD_RANGES 8u

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

static u64 be_cells(const u8 *p, u32 cells)
{
    u64 v = 0;
    for (u32 i = 0; i < cells; i++) v = (v << 32) | be32(p + i * 4u);
    return v;
}

static int node_name_is(const u8 *name, u32 len, const char *want)
{
    u32 i = 0;
    while (want[i] && i < len && name[i] == (u8)want[i]) i++;
    if (want[i]) return 0;
    return i == len || name[i] == '@';
}

static int string_is(const u8 *s, u32 len, const char *want)
{
    u32 i = 0;
    while (want[i] && i < len && s[i] == (u8)want[i]) i++;
    return !want[i] && i < len && s[i] == 0;
}

static int add_range(struct mem_range *dst, u32 *count, u32 cap,
                     u64 base, u64 size)
{
    if (!size) return 0;
    if (*count >= cap || size > (u64)-1 - base) return -1;
    dst[*count].base = base;
    dst[*count].size = size;
    (*count)++;
    return 0;
}

int memmap_parse_dtb(const u8 *dtb, struct memmap_info *out)
{
    if (!dtb || !out || be32(dtb) != FDT_MAGIC) return -1;
    for (u32 i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;

    u32 total = be32(dtb + 4);
    u32 off_struct = be32(dtb + 8);
    u32 off_strings = be32(dtb + 12);
    u32 off_rsv = be32(dtb + 16);
    u32 size_strings = be32(dtb + 32);
    u32 size_struct = be32(dtb + 36);
    if (total < 40 || total > MAX_DTB_SIZE ||
        off_struct > total || size_struct > total - off_struct ||
        off_strings > total || size_strings > total - off_strings ||
        off_rsv > total || off_rsv < 40 || (off_rsv & 7u))
        return -2;

    const u8 *blob_end = dtb + total;
    const u8 *strings = dtb + off_strings;
    const u8 *strings_end = strings + size_strings;
    const u8 *r = dtb + off_rsv;
    int rsv_terminated = 0;
    while (r <= blob_end && (u64)(blob_end - r) >= 16u) {
        u64 base = ((u64)be32(r) << 32) | be32(r + 4);
        u64 size = ((u64)be32(r + 8) << 32) | be32(r + 12);
        r += 16;
        if (!base && !size) { rsv_terminated = 1; break; }
        if (add_range(out->fdt_reserved, &out->nfdt_reserved,
                      MEMMAP_MAX_FDT_RSV, base, size)) return -3;
    }
    if (!rsv_terminated) return -4;

    const u8 *p = dtb + off_struct;
    const u8 *struct_end = p + size_struct;
    int depth = 0;
    int in_memory = 0;
    int in_reserved_memory = 0;
    int in_reserved_child = 0;
    int child_no_map = 0;
    u32 root_addr_cells = 0, root_size_cells = 0;
    u32 reserved_addr_cells = 0, reserved_size_cells = 0;
    struct mem_range child_ranges[MAX_CHILD_RANGES];
    u32 child_range_count = 0;
    int saw_end = 0;

    while (p <= struct_end && (u64)(struct_end - p) >= 4u) {
        u32 token = be32(p);
        p += 4;
        if (token == FDT_BEGIN_NODE) {
            const u8 *nul = p;
            while (nul < struct_end && *nul) nul++;
            if (nul == struct_end) return -5;
            u32 name_len = (u32)(nul - p);
            u64 consumed = (u64)name_len + 1u;
            consumed = (consumed + 3u) & ~3u;
            if (consumed > (u64)(struct_end - p)) return -5;
            const u8 *name = p;
            p += consumed;
            depth++;
            if (depth == 2) {
                in_memory = node_name_is(name, name_len, "memory");
                in_reserved_memory = node_name_is(name, name_len, "reserved-memory");
                if (in_reserved_memory) {
                    reserved_addr_cells = root_addr_cells;
                    reserved_size_cells = root_size_cells;
                }
            } else if (depth == 3 && in_reserved_memory) {
                in_reserved_child = 1;
                child_no_map = 0;
                child_range_count = 0;
            }
        } else if (token == FDT_END_NODE) {
            if (depth <= 0) return -6;
            if (depth == 3 && in_reserved_memory && in_reserved_child) {
                for (u32 i = 0; i < child_range_count; i++) {
                    if (add_range(out->reserved, &out->nreserved,
                                  MEMMAP_MAX_RESERVED,
                                  child_ranges[i].base, child_ranges[i].size)) return -7;
                    if (child_no_map &&
                        add_range(out->nomap, &out->nnomap, MEMMAP_MAX_NOMAP,
                                  child_ranges[i].base, child_ranges[i].size)) return -7;
                }
                in_reserved_child = 0;
            }
            depth--;
            if (depth == 1) {
                in_memory = 0;
                in_reserved_memory = 0;
            }
        } else if (token == FDT_PROP) {
            if ((u64)(struct_end - p) < 8u) return -8;
            u32 len = be32(p);
            u32 nameoff = be32(p + 4);
            p += 8;
            if ((u64)len > (u64)(struct_end - p)) return -8;
            const u8 *value = p;
            u64 padded = ((u64)len + 3u) & ~3u;
            if (padded > (u64)(struct_end - p)) return -8;
            p += padded;
            if (nameoff >= size_strings) return -9;
            const u8 *prop = strings + nameoff;
            const u8 *prop_end = prop;
            while (prop_end < strings_end && *prop_end) prop_end++;
            if (prop_end == strings_end) return -9;
            u32 prop_len = (u32)(prop_end - prop);

            if (depth == 1) {
                if (string_is(prop, prop_len + 1u, "#address-cells") && len == 4)
                    root_addr_cells = be32(value);
                else if (string_is(prop, prop_len + 1u, "#size-cells") && len == 4)
                    root_size_cells = be32(value);
            } else if (depth == 2 && in_reserved_memory) {
                if (string_is(prop, prop_len + 1u, "#address-cells") && len == 4)
                    reserved_addr_cells = be32(value);
                else if (string_is(prop, prop_len + 1u, "#size-cells") && len == 4)
                    reserved_size_cells = be32(value);
            } else if (depth == 2 && in_memory &&
                       string_is(prop, prop_len + 1u, "reg")) {
                if ((root_addr_cells != 1 && root_addr_cells != 2) ||
                    (root_size_cells != 1 && root_size_cells != 2)) return -10;
                u32 tuple_cells = root_addr_cells + root_size_cells;
                u32 tuple_bytes = tuple_cells * 4u;
                if (!tuple_bytes || len % tuple_bytes) return -10;
                for (u32 off = 0; off < len; off += tuple_bytes) {
                    u64 base = be_cells(value + off, root_addr_cells);
                    u64 size = be_cells(value + off + root_addr_cells * 4u,
                                        root_size_cells);
                    if (add_range(out->ram, &out->nram, MEMMAP_MAX_RAM, base, size)) return -11;
                }
            } else if (depth == 3 && in_reserved_memory && in_reserved_child) {
                if (string_is(prop, prop_len + 1u, "no-map")) {
                    child_no_map = 1;
                } else if (string_is(prop, prop_len + 1u, "reg")) {
                    if ((reserved_addr_cells != 1 && reserved_addr_cells != 2) ||
                        (reserved_size_cells != 1 && reserved_size_cells != 2)) return -12;
                    u32 tuple_cells = reserved_addr_cells + reserved_size_cells;
                    u32 tuple_bytes = tuple_cells * 4u;
                    if (!tuple_bytes || len % tuple_bytes) return -12;
                    for (u32 off = 0; off < len; off += tuple_bytes) {
                        if (child_range_count >= MAX_CHILD_RANGES) return -12;
                        child_ranges[child_range_count].base =
                            be_cells(value + off, reserved_addr_cells);
                        child_ranges[child_range_count].size =
                            be_cells(value + off + reserved_addr_cells * 4u,
                                     reserved_size_cells);
                        child_range_count++;
                    }
                }
            }
        } else if (token == FDT_NOP) {
            continue;
        } else if (token == FDT_END) {
            saw_end = 1;
            break;
        } else {
            return -13;
        }
    }

    if (!saw_end || !out->nram) return -14;
    return 0;
}
