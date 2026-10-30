#include "../console.h"
#include "../drivers/storage/gpt.h"
#include "../drivers/storage/mtk_ufs.h"
#include "android_boot_test.h"
#include "../boot/android_bootimg.h"

static struct gpt_table table;
static unsigned char image_header[4096] __attribute__((aligned(4096)));
static unsigned char kernel_prefix[4096] __attribute__((aligned(4096)));
static unsigned char vendor_dtb_header[4096] __attribute__((aligned(4096)));
static unsigned char ramdisk_table_data[65536] __attribute__((aligned(4096)));
static struct android_vendor_ramdisk_table ramdisk_table;

static const struct gpt_partition *find(const char *name)
{
    return gpt_find_partition(&table, name);
}

static void inspect_boot_partition(const char *name)
{
    const struct gpt_partition *p = find(name);
    if (!p) {
        logf("ANDROID: %s absent", name);
        return;
    }

    ab_u64 blocks = p->last_lba - p->first_lba + 1u;
    ab_u64 bytes = blocks * 4096u;
    int rc = mtk_ufs_read_blocks(2u, (unsigned int)p->first_lba,
                                 1u, image_header);
    if (rc) {
        logf("ANDROID: %s header read failed rc=%d",
             name, (unsigned long)(-rc));
        return;
    }

    struct android_bootimg_info info;
    rc = android_bootimg_parse(image_header, bytes, &info);
    if (rc) {
        logf("ANDROID: %s unsupported header rc=%d magic=%x",
             name, (unsigned long)(-rc),
             (unsigned long)((unsigned int)image_header[0] |
             ((unsigned int)image_header[1] << 8) |
             ((unsigned int)image_header[2] << 16) |
             ((unsigned int)image_header[3] << 24)));
        return;
    }

    logf("ANDROID: %s v=%d kernel=%d ramdisk=%d sig=%d image=%d",
         name, (unsigned long)info.header_version,
         (unsigned long)info.kernel_size, (unsigned long)info.ramdisk_size,
         (unsigned long)info.signature_size, (unsigned long)info.image_size);

    if (info.kernel_size) {
        unsigned int block = (unsigned int)(p->first_lba +
                                            info.kernel_offset / 4096u);
        rc = mtk_ufs_read_blocks(2u, block, 1u, kernel_prefix);
        if (!rc) {
            if (kernel_prefix[0] == 0x1fu && kernel_prefix[1] == 0x8bu)
                logf("ANDROID: %s kernel starts with gzip", name);
            else if (kernel_prefix[56] == 'A' && kernel_prefix[57] == 'R' &&
                     kernel_prefix[58] == 'M' && kernel_prefix[59] == 0x64)
                logf("ANDROID: %s kernel is raw ARM64 Image", name);
            else
                logf("ANDROID: %s kernel prefix=%x %x %x %x",
                     name, (unsigned long)kernel_prefix[0],
                     (unsigned long)kernel_prefix[1],
                     (unsigned long)kernel_prefix[2],
                     (unsigned long)kernel_prefix[3]);
        }
    }
}

static void inspect_vendor_partition(const char *name)
{
    const struct gpt_partition *p = find(name);
    if (!p) {
        logf("ANDROID: %s absent", name);
        return;
    }

    ab_u64 bytes = (p->last_lba - p->first_lba + 1u) * 4096u;
    int rc = mtk_ufs_read_blocks(2u, (unsigned int)p->first_lba,
                                 1u, image_header);
    if (rc) {
        logf("ANDROID: %s header read failed rc=%d",
             name, (unsigned long)(-rc));
        return;
    }

    struct android_vendor_boot_info info;
    rc = android_vendor_boot_parse(image_header, bytes, &info);
    if (rc) {
        logf("ANDROID: %s unsupported header rc=%d",
             name, (unsigned long)(-rc));
        return;
    }

    logf("ANDROID: %s v=%d page=%d ramdisk=%d dtb=%d table=%d/%d bootconfig=%d",
         name, (unsigned long)info.header_version,
         (unsigned long)info.page_size,
         (unsigned long)info.vendor_ramdisk_size,
         (unsigned long)info.dtb_size,
         (unsigned long)info.ramdisk_table_size,
         (unsigned long)info.ramdisk_table_entries,
         (unsigned long)info.bootconfig_size);
    logf("ANDROID: %s offsets ramdisk=%x dtb=%x table=%x config=%x total=%x",
         name, (unsigned long)info.ramdisk_offset, (unsigned long)info.dtb_offset,
         (unsigned long)info.ramdisk_table_offset, (unsigned long)info.bootconfig_offset,
         (unsigned long)info.image_size);

    if (info.dtb_size) {
        unsigned int dtb_lba = (unsigned int)(p->first_lba +
                                  info.dtb_offset / 4096u);
        rc = mtk_ufs_read_blocks(2u, dtb_lba, 1u, vendor_dtb_header);
        if (!rc) {
            unsigned int magic = ((unsigned int)vendor_dtb_header[0] << 24) |
                                 ((unsigned int)vendor_dtb_header[1] << 16) |
                                 ((unsigned int)vendor_dtb_header[2] << 8) |
                                 (unsigned int)vendor_dtb_header[3];
            unsigned int total = ((unsigned int)vendor_dtb_header[4] << 24) |
                                 ((unsigned int)vendor_dtb_header[5] << 16) |
                                 ((unsigned int)vendor_dtb_header[6] << 8) |
                                 (unsigned int)vendor_dtb_header[7];
            logf("ANDROID: %s DTB magic=%x total=%x declared=%x",
                 name, (unsigned long)magic, (unsigned long)total,
                 (unsigned long)info.dtb_size);
        }
    }

    if (info.ramdisk_table_size) {
        if (info.ramdisk_table_size > sizeof(ramdisk_table_data)) {
            logf("ANDROID: %s ramdisk table too large=%x",
                 name, (unsigned long)info.ramdisk_table_size);
        } else {
            unsigned int table_lba = (unsigned int)(p->first_lba +
                                       info.ramdisk_table_offset / 4096u);
            unsigned int blocks =
                (info.ramdisk_table_size + 4095u) / 4096u;
            rc = mtk_ufs_read_blocks(2u, table_lba, blocks,
                                     ramdisk_table_data);
            if (!rc) {
                rc = android_vendor_boot_parse_table(
                    ramdisk_table_data, info.ramdisk_table_size,
                    info.ramdisk_table_entries,
                    info.ramdisk_table_entry_size,
                    info.vendor_ramdisk_size, &ramdisk_table);
                if (rc) {
                    logf("ANDROID: %s ramdisk table invalid rc=%d",
                         name, (unsigned long)(-rc));
                } else {
                    for (unsigned int i = 0; i < ramdisk_table.count; i++) {
                        const struct android_vendor_ramdisk_fragment *entry =
                            &ramdisk_table.entries[i];
                        logf("ANDROID: %s ramdisk[%d] type=%d name=%s off=%x size=%x",
                             name, (unsigned long)i, (unsigned long)entry->type,
                             entry->name, (unsigned long)entry->offset,
                             (unsigned long)entry->size);
                    }
                }
            } else {
                logf("ANDROID: %s ramdisk table read failed rc=%d",
                     name, (unsigned long)(-rc));
            }
        }
    }
}

void android_boot_test(void)
{
    int rc = mtk_ufs_init();
    if (rc) {
        logf("ANDROID TEST: UFS init failed rc=%d", (unsigned long)(-rc));
        return;
    }

    rc = gpt_read_table(2u, &table);
    if (rc) {
        logf("ANDROID TEST: GPT read failed rc=%d", (unsigned long)(-rc));
        return;
    }

    logf("ANDROID TEST: LUN2 GPT has %d partitions",
         (unsigned long)table.count);
    inspect_boot_partition("boot_a");
    inspect_vendor_partition("vendor_boot_a");
    inspect_boot_partition("init_boot_a");
    inspect_vendor_partition("vendor_boot_b");
}
