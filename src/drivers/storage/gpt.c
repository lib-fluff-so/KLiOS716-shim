#include "gpt.h"
#include "mtk_ufs.h"

#define GPT_BLOCK_SIZE 4096u
#define GPT_HEADER_MIN 92u
#define GPT_MAX_ENTRY_BYTES (GPT_MAX_PARTITIONS * 128u)

static gpt_u8 header_block[GPT_BLOCK_SIZE] __attribute__((aligned(4096)));
static gpt_u8 entry_bytes[GPT_MAX_ENTRY_BYTES] __attribute__((aligned(4096)));
static gpt_u8 header_check[GPT_BLOCK_SIZE] __attribute__((aligned(4096)));

static gpt_u32 le32(const gpt_u8 *p)
{
    return (gpt_u32)p[0] | ((gpt_u32)p[1] << 8) |
           ((gpt_u32)p[2] << 16) | ((gpt_u32)p[3] << 24);
}

static gpt_u64 le64(const gpt_u8 *p)
{
    return (gpt_u64)le32(p) | ((gpt_u64)le32(p + 4) << 32);
}

static gpt_u32 crc32(const gpt_u8 *p, gpt_u32 len)
{
    gpt_u32 crc = 0xffffffffu;
    for (gpt_u32 i = 0; i < len; i++) {
        crc ^= p[i];
        for (gpt_u32 bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (gpt_u32)-(int)(crc & 1u));
    }
    return ~crc;
}

static int bytes_equal(const gpt_u8 *p, const char *s, gpt_u32 n)
{
    for (gpt_u32 i = 0; i < n; i++)
        if (p[i] != (gpt_u8)s[i])
            return 0;
    return 1;
}

int gpt_read_table(gpt_u32 lun, struct gpt_table *table)
{
    if (!table)
        return -1;
    for (gpt_u32 i = 0; i < sizeof(*table); i++)
        ((gpt_u8 *)table)[i] = 0;

    if (mtk_ufs_block_size() != GPT_BLOCK_SIZE)
        return -2;
    int rc = mtk_ufs_read_blocks(lun, 1u, 1u, header_block);
    if (rc)
        return -3;
    if (!bytes_equal(header_block, "EFI PART", 8u))
        return -4;

    gpt_u32 header_size = le32(header_block + 12u);
    gpt_u32 expected_header_crc = le32(header_block + 16u);
    if (header_size < GPT_HEADER_MIN || header_size > GPT_BLOCK_SIZE)
        return -5;

    for (gpt_u32 i = 0; i < header_size; i++)
        header_check[i] = header_block[i];
    header_check[16] = header_check[17] = 0;
    header_check[18] = header_check[19] = 0;
    if (crc32(header_check, header_size) != expected_header_crc)
        return -6;

    gpt_u64 current_lba = le64(header_block + 24u);
    gpt_u64 entry_lba = le64(header_block + 72u);
    gpt_u32 entry_count = le32(header_block + 80u);
    gpt_u32 entry_size = le32(header_block + 84u);
    gpt_u32 expected_entries_crc = le32(header_block + 88u);

    if (current_lba != 1u || !entry_count ||
        entry_count > GPT_MAX_PARTITIONS || entry_size != 128u)
        return -7;

    gpt_u32 bytes = entry_count * entry_size;
    gpt_u32 blocks = (bytes + GPT_BLOCK_SIZE - 1u) / GPT_BLOCK_SIZE;
    if (!blocks || blocks * GPT_BLOCK_SIZE > sizeof(entry_bytes) ||
        entry_lba > 0xffffffffu || entry_lba + blocks > 0x100000000ULL)
        return -8;

    rc = mtk_ufs_read_blocks(lun, (gpt_u32)entry_lba, blocks, entry_bytes);
    if (rc)
        return -9;
    if (crc32(entry_bytes, bytes) != expected_entries_crc)
        return -10;

    table->block_size = GPT_BLOCK_SIZE;
    table->first_usable_lba = le64(header_block + 40u);
    table->last_usable_lba = le64(header_block + 48u);

    for (gpt_u32 i = 0; i < entry_count; i++) {
        const gpt_u8 *entry = entry_bytes + i * entry_size;
        struct gpt_partition *part = &table->partitions[table->count];
        gpt_u64 first = le64(entry + 32u);
        gpt_u64 last = le64(entry + 40u);
        int has_type = 0;

        for (gpt_u32 j = 0; j < 16u; j++) {
            part->type_guid[j] = entry[j];
            part->unique_guid[j] = entry[16u + j];
            if (entry[j])
                has_type = 1;
        }
        if (!has_type)
            continue;
        if (last < first || first < table->first_usable_lba ||
            last > table->last_usable_lba)
            return -11;

        part->first_lba = first;
        part->last_lba = last;
        part->attributes = le64(entry + 48u);
        for (gpt_u32 c = 0; c < 36u; c++) {
            gpt_u8 low = entry[56u + c * 2u];
            gpt_u8 high = entry[57u + c * 2u];
            if (!low && !high) {
                part->name[c] = 0;
                break;
            }
            part->name[c] = high ? '?' :
                ((low >= 32u && low < 127u) ? (char)low : '?');
        }
        part->name[36] = 0;
        table->count++;
    }
    return 0;
}

const struct gpt_partition *gpt_find_partition(
    const struct gpt_table *table, const char *name)
{
    if (!table || !name)
        return 0;

    for (gpt_u32 i = 0; i < table->count; i++) {
        const char *a = table->partitions[i].name;
        const char *b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return &table->partitions[i];
    }
    return 0;
}
