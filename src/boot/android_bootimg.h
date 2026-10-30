#ifndef KLIOS716_ANDROID_BOOTIMG_H
#define KLIOS716_ANDROID_BOOTIMG_H

typedef unsigned int ab_u32;
typedef unsigned long long ab_u64;

struct android_bootimg_info {
    ab_u32 header_version;
    ab_u32 header_size;
    ab_u32 os_version;
    ab_u32 kernel_size;
    ab_u32 ramdisk_size;
    ab_u32 signature_size;
    ab_u64 kernel_offset;
    ab_u64 ramdisk_offset;
    ab_u64 signature_offset;
    ab_u64 image_size;
};

#define ANDROID_VENDOR_RAMDISK_MAX_ENTRIES 256u

struct android_vendor_ramdisk_fragment {
    ab_u32 size;
    ab_u32 offset;
    ab_u32 type;
    char name[33];
    ab_u32 board_id[16];
};

struct android_vendor_ramdisk_table {
    ab_u32 count;
    struct android_vendor_ramdisk_fragment entries[ANDROID_VENDOR_RAMDISK_MAX_ENTRIES];
};

struct android_vendor_boot_info {
    ab_u32 header_version;
    ab_u32 header_size;
    ab_u32 page_size;
    ab_u32 vendor_ramdisk_size;
    ab_u32 dtb_size;
    ab_u32 ramdisk_table_size;
    ab_u32 ramdisk_table_entries;
    ab_u32 ramdisk_table_entry_size;
    ab_u32 bootconfig_size;
    ab_u64 ramdisk_offset;
    ab_u64 dtb_offset;
    ab_u64 ramdisk_table_offset;
    ab_u64 bootconfig_offset;
    ab_u64 image_size;
};

int android_bootimg_parse(const unsigned char *header, ab_u64 partition_size,
                          struct android_bootimg_info *out);
int android_vendor_boot_parse(const unsigned char *header, ab_u64 partition_size,
                              struct android_vendor_boot_info *out);
int android_vendor_boot_parse_table(const unsigned char *table_data,
                                    ab_u32 table_size, ab_u32 entry_count,
                                    ab_u32 entry_size, ab_u32 vendor_ramdisk_size,
                                    struct android_vendor_ramdisk_table *out);

#endif
