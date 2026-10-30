#ifndef KLIOS716_GPT_H
#define KLIOS716_GPT_H

typedef unsigned char gpt_u8;
typedef unsigned int gpt_u32;
typedef unsigned long long gpt_u64;

#define GPT_MAX_PARTITIONS 128u

struct gpt_partition {
    char name[37];
    gpt_u8 type_guid[16];
    gpt_u8 unique_guid[16];
    gpt_u64 first_lba;
    gpt_u64 last_lba;
    gpt_u64 attributes;
};

struct gpt_table {
    gpt_u32 count;
    gpt_u32 block_size;
    gpt_u64 first_usable_lba;
    gpt_u64 last_usable_lba;
    struct gpt_partition partitions[GPT_MAX_PARTITIONS];
};

/* Read and CRC-validate the primary GPT from a UFS logical unit. */
int gpt_read_table(gpt_u32 lun, struct gpt_table *table);
const struct gpt_partition *gpt_find_partition(
    const struct gpt_table *table, const char *name);

#endif
