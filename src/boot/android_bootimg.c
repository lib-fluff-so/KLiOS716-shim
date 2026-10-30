#include "android_bootimg.h"

#define ANDROID_PAGE_SIZE 4096u
#define BOOT_V3_HEADER_SIZE 1580u
#define BOOT_V4_HEADER_SIZE 1584u
#define VENDOR_V3_HEADER_SIZE 2112u
#define VENDOR_V4_HEADER_SIZE 2128u

static ab_u32 read_le32(const unsigned char *p)
{
    return (ab_u32)p[0] | ((ab_u32)p[1] << 8) |
           ((ab_u32)p[2] << 16) | ((ab_u32)p[3] << 24);
}

static ab_u64 align_page(ab_u64 value, ab_u32 page_size, int *ok)
{
    ab_u64 mask = (ab_u64)page_size - 1u;
    if (value > ~(ab_u64)0 - mask) {
        *ok = 0;
        return 0;
    }
    return (value + mask) & ~mask;
}

static int add_u64(ab_u64 a, ab_u64 b, ab_u64 *out)
{
    if (b > ~(ab_u64)0 - a)
        return 0;
    *out = a + b;
    return 1;
}

static int has_magic(const unsigned char *p, const char *magic, ab_u32 len)
{
    for (ab_u32 i = 0; i < len; i++)
        if (p[i] != (unsigned char)magic[i])
            return 0;
    return 1;
}

int android_bootimg_parse(const unsigned char *h, ab_u64 partition_size,
                          struct android_bootimg_info *out)
{
    if (!h || !out || !has_magic(h, "ANDROID!", 8u))
        return -1;

    ab_u32 version = read_le32(h + 40u);
    ab_u32 header_size = read_le32(h + 20u);
    if (version != 3u && version != 4u)
        return -2;
    if ((version == 3u && header_size != BOOT_V3_HEADER_SIZE) ||
        (version == 4u && header_size != BOOT_V4_HEADER_SIZE))
        return -3;

    out->header_version = version;
    out->header_size = header_size;
    out->os_version = read_le32(h + 16u);
    out->kernel_size = read_le32(h + 8u);
    out->ramdisk_size = read_le32(h + 12u);
    out->signature_size = version == 4u ? read_le32(h + 1580u) : 0u;

    out->kernel_offset = ANDROID_PAGE_SIZE;
    int ok = 1;
    ab_u64 kernel_end;
    if (!add_u64(out->kernel_offset, out->kernel_size, &kernel_end))
        return -4;
    out->ramdisk_offset = align_page(kernel_end, ANDROID_PAGE_SIZE, &ok);
    if (!ok)
        return -4;

    ab_u64 ramdisk_end;
    if (!add_u64(out->ramdisk_offset, out->ramdisk_size, &ramdisk_end))
        return -4;
    out->signature_offset = align_page(ramdisk_end, ANDROID_PAGE_SIZE, &ok);
    if (!ok)
        return -4;

    ab_u64 image_end;
    if (!add_u64(out->signature_offset, out->signature_size, &image_end))
        return -4;
    out->image_size = align_page(image_end, ANDROID_PAGE_SIZE, &ok);
    if (!ok || out->image_size > partition_size)
        return -5;
    return 0;
}

int android_vendor_boot_parse(const unsigned char *h, ab_u64 partition_size,
                              struct android_vendor_boot_info *out)
{
    if (!h || !out || !has_magic(h, "VNDRBOOT", 8u))
        return -1;

    ab_u32 version = read_le32(h + 8u);
    ab_u32 page_size = read_le32(h + 12u);
    ab_u32 header_size = read_le32(h + 2096u);
    if (version != 3u && version != 4u)
        return -2;
    if (page_size != ANDROID_PAGE_SIZE)
        return -3;
    if ((version == 3u && header_size != VENDOR_V3_HEADER_SIZE) ||
        (version == 4u && header_size != VENDOR_V4_HEADER_SIZE))
        return -4;

    out->header_version = version;
    out->header_size = header_size;
    out->page_size = page_size;
    out->vendor_ramdisk_size = read_le32(h + 24u);
    out->dtb_size = read_le32(h + 2100u);
    out->ramdisk_table_size = version == 4u ? read_le32(h + 2112u) : 0u;
    out->ramdisk_table_entries = version == 4u ? read_le32(h + 2116u) : 0u;
    out->ramdisk_table_entry_size = version == 4u ? read_le32(h + 2120u) : 0u;
    out->bootconfig_size = version == 4u ? read_le32(h + 2124u) : 0u;

    if (version == 4u) {
        if (out->ramdisk_table_entries > 256u)
            return -5;
        if ((out->ramdisk_table_entries == 0u) !=
            (out->ramdisk_table_size == 0u))
            return -5;
        if (out->ramdisk_table_entries &&
            (out->ramdisk_table_entry_size < 108u ||
             (ab_u64)out->ramdisk_table_entries * out->ramdisk_table_entry_size !=
             out->ramdisk_table_size))
            return -5;
    }

    int ok = 1;
    out->ramdisk_offset = align_page(header_size, page_size, &ok);
    if (!ok)
        return -6;

    ab_u64 ramdisk_end;
    if (!add_u64(out->ramdisk_offset, out->vendor_ramdisk_size, &ramdisk_end))
        return -6;
    out->dtb_offset = align_page(ramdisk_end, page_size, &ok);
    if (!ok)
        return -6;

    ab_u64 dtb_end;
    if (!add_u64(out->dtb_offset, out->dtb_size, &dtb_end))
        return -6;

    if (version == 4u) {
        out->ramdisk_table_offset = align_page(dtb_end, page_size, &ok);
        if (!ok)
            return -6;
        ab_u64 table_end;
        if (!add_u64(out->ramdisk_table_offset, out->ramdisk_table_size,
                     &table_end))
            return -6;
        out->bootconfig_offset = align_page(table_end, page_size, &ok);
        if (!ok)
            return -6;
        ab_u64 bootconfig_end;
        if (!add_u64(out->bootconfig_offset, out->bootconfig_size,
                     &bootconfig_end))
            return -6;
        out->image_size = align_page(bootconfig_end, page_size, &ok);
    } else {
        out->ramdisk_table_offset = 0;
        out->bootconfig_offset = 0;
        out->image_size = align_page(dtb_end, page_size, &ok);
    }

    if (!ok || out->image_size > partition_size)
        return -7;
    return 0;
}

int android_vendor_boot_parse_table(const unsigned char *data,
                                    ab_u32 table_size, ab_u32 entry_count,
                                    ab_u32 entry_size, ab_u32 vendor_ramdisk_size,
                                    struct android_vendor_ramdisk_table *out)
{
    const ab_u32 minimum_entry_size = 108u;

    if (!data || !out || entry_count > ANDROID_VENDOR_RAMDISK_MAX_ENTRIES)
        return -1;
    if (!entry_count) {
        if (table_size)
            return -2;
        out->count = 0;
        return 0;
    }
    if (entry_size < minimum_entry_size ||
        (ab_u64)entry_count * entry_size != table_size)
        return -3;

    out->count = 0;
    for (ab_u32 i = 0; i < entry_count; i++) {
        const unsigned char *p = data + (ab_u64)i * entry_size;
        struct android_vendor_ramdisk_fragment *entry = &out->entries[i];

        entry->size = read_le32(p + 0u);
        entry->offset = read_le32(p + 4u);
        entry->type = read_le32(p + 8u);
        if (!entry->size || entry->offset > vendor_ramdisk_size ||
            entry->size > vendor_ramdisk_size - entry->offset)
            return -4;

        for (ab_u32 c = 0; c < 32u; c++) {
            unsigned char ch = p[12u + c];
            entry->name[c] = (ch >= 32u && ch < 127u) ? (char)ch : 0;
        }
        entry->name[32] = 0;
        for (ab_u32 j = 0; j < 16u; j++)
            entry->board_id[j] = read_le32(p + 44u + j * 4u);
        out->count++;
    }
    return 0;
}
