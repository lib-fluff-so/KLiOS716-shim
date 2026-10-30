#include "../console.h"
#include "../drivers/storage/gpt.h"
#include "../drivers/storage/mtk_ufs.h"
#include "ufs_test.h"

static struct gpt_table table;

void ufs_test(void)
{
    int rc = mtk_ufs_init();
    if (rc) {
        logf("UFS TEST: init failed rc=%d", (unsigned long)(-rc));
        return;
    }

    rc = gpt_read_table(2, &table);
    if (rc) {
        logf("UFS TEST: GPT read/CRC failed rc=%d", (unsigned long)(-rc));
        return;
    }

    logf("UFS TEST: LUN2 GPT valid, %d partitions",
         (unsigned long)table.count);
    for (gpt_u32 i = 0; i < table.count; i++) {
        const struct gpt_partition *p = &table.partitions[i];
        logf("GPT: %s first=%x last=%x",
             p->name, (unsigned long)p->first_lba,
             (unsigned long)p->last_lba);
    }

    const struct gpt_partition *boot_a = gpt_find_partition(&table, "boot_a");
    const struct gpt_partition *vendor_a =
        gpt_find_partition(&table, "vendor_boot_a");
    const struct gpt_partition *init_a =
        gpt_find_partition(&table, "init_boot_a");
    logf("UFS TEST: boot_a=%d vendor_boot_a=%d init_boot_a=%d",
         (unsigned long)(boot_a != 0),
         (unsigned long)(vendor_a != 0),
         (unsigned long)(init_a != 0));
}
