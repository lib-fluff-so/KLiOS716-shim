#include "../console.h"
#include "../memmap.h"
#include "../drivers/storage/gpt.h"
#include "../drivers/storage/mtk_ufs.h"
#include "android_bootimg.h"
#include "android_kernel.h"
#include "fdt_edit.h"

typedef unsigned long long al_u64;
typedef unsigned int al_u32;
typedef unsigned char al_u8;

#define ANDROID_BLOCK_SIZE       4096u
#define ANDROID_READ_CHUNK       512u
#define ANDROID_KERNEL_OUT_MAX   0x08000000u
#define ANDROID_KERNEL_IN_MAX    0x04000000u
#define ANDROID_RAMDISK_MAX      0x10000000u
#define ANDROID_DTB_MAX          0x00200000u
#define ANDROID_ALIGN_2M         0x00200000ULL
#define ANDROID_WINDOW_1G        0x40000000ULL
#define ANDROID_ALLOC_MAX        12u
#define RAMOOPS_BASE             0x48090000ULL
#define RAMOOPS_SIZE             0x000E0000ULL

struct allocation {
    al_u64 base;
    al_u64 size;
};

struct android_context {
    const struct memmap_info *map;
    al_u64 live_dtb;
    al_u64 live_dtb_size;
    al_u64 live_initrd_start;
    al_u64 live_initrd_end;
    al_u64 shim_base;
    al_u64 shim_size;
    struct allocation allocations[ANDROID_ALLOC_MAX];
    al_u32 allocation_count;
};

static struct gpt_table android_gpt;
static al_u8 boot_header[ANDROID_BLOCK_SIZE] __attribute__((aligned(4096)));
static al_u8 vendor_header[ANDROID_BLOCK_SIZE] __attribute__((aligned(4096)));
static al_u8 init_header[ANDROID_BLOCK_SIZE] __attribute__((aligned(4096)));

extern char _start[], _end[];
extern void transfer_to_linux(al_u64 kernel_entry, al_u64 dtb_address)
    __attribute__((noreturn));

static al_u64 read_le64(const al_u8 *p)
{
    return (al_u64)p[0] | ((al_u64)p[1] << 8) |
           ((al_u64)p[2] << 16) | ((al_u64)p[3] << 24) |
           ((al_u64)p[4] << 32) | ((al_u64)p[5] << 40) |
           ((al_u64)p[6] << 48) | ((al_u64)p[7] << 56);
}

static al_u32 read_le32(const al_u8 *p)
{
    return (al_u32)p[0] | ((al_u32)p[1] << 8) |
           ((al_u32)p[2] << 16) | ((al_u32)p[3] << 24);
}

static void write_le32(al_u8 *p, al_u32 v)
{
    p[0] = (al_u8)v;
    p[1] = (al_u8)(v >> 8);
    p[2] = (al_u8)(v >> 16);
    p[3] = (al_u8)(v >> 24);
}

static al_u64 align_up(al_u64 value, al_u64 alignment)
{
    if (!alignment || value > ~(al_u64)0 - (alignment - 1u))
        return 0;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static al_u64 align_down(al_u64 value, al_u64 alignment)
{
    return value & ~(alignment - 1u);
}

static int overlaps(al_u64 a, al_u64 as, al_u64 b, al_u64 bs)
{
    if (!as || !bs)
        return 0;
    if (as > ~(al_u64)0 - a || bs > ~(al_u64)0 - b)
        return 1;
    return a < b + bs && b < a + as;
}

static int in_ram(const struct android_context *ctx, al_u64 base, al_u64 size)
{
    if (!size || size > ~(al_u64)0 - base)
        return 0;
    for (al_u32 i = 0; i < ctx->map->nram; i++) {
        al_u64 rb = ctx->map->ram[i].base;
        al_u64 re = rb + ctx->map->ram[i].size;
        if (ctx->map->ram[i].size <= ~(al_u64)0 - rb &&
            base >= rb && base < re && size <= re - base)
            return 1;
    }
    return 0;
}

static int range_free(const struct android_context *ctx,
                      al_u64 base, al_u64 size)
{
    if (!in_ram(ctx, base, size))
        return 0;
    if (overlaps(base, size, ctx->live_dtb, ctx->live_dtb_size) ||
        overlaps(base, size, ctx->live_initrd_start,
                 ctx->live_initrd_end - ctx->live_initrd_start) ||
        overlaps(base, size, ctx->shim_base, ctx->shim_size) ||
        overlaps(base, size, RAMOOPS_BASE, RAMOOPS_SIZE))
        return 0;

    for (al_u32 i = 0; i < ctx->map->nreserved; i++)
        if (overlaps(base, size, ctx->map->reserved[i].base,
                     ctx->map->reserved[i].size))
            return 0;
    for (al_u32 i = 0; i < ctx->map->nfdt_reserved; i++)
        if (overlaps(base, size, ctx->map->fdt_reserved[i].base,
                     ctx->map->fdt_reserved[i].size))
            return 0;
    for (al_u32 i = 0; i < ctx->map->nnomap; i++)
        if (overlaps(base, size, ctx->map->nomap[i].base,
                     ctx->map->nomap[i].size))
            return 0;
    for (al_u32 i = 0; i < ctx->allocation_count; i++)
        if (overlaps(base, size, ctx->allocations[i].base,
                     ctx->allocations[i].size))
            return 0;
    return 1;
}

static int remember_allocation(struct android_context *ctx,
                               al_u64 base, al_u64 size)
{
    if (!size || ctx->allocation_count >= ANDROID_ALLOC_MAX)
        return -1;
    ctx->allocations[ctx->allocation_count].base = base;
    ctx->allocations[ctx->allocation_count].size = size;
    ctx->allocation_count++;
    return 0;
}

static int allocate_range(struct android_context *ctx, al_u64 size,
                          al_u64 alignment, al_u64 window_start,
                          al_u64 window_size, int prefer_high, al_u64 *result)
{
    if (!size || !alignment || !result)
        return -1;
    al_u64 window_end = window_size ? window_start + window_size : ~(al_u64)0;
    if (window_size && window_end < window_start)
        return -2;

    for (al_u32 i = 0; i < ctx->map->nram; i++) {
        al_u64 ram_start = ctx->map->ram[i].base;
        al_u64 ram_size = ctx->map->ram[i].size;
        if (!ram_size || ram_size > ~(al_u64)0 - ram_start)
            continue;
        al_u64 ram_end = ram_start + ram_size;
        al_u64 low = ram_start > window_start ? ram_start : window_start;
        al_u64 high = ram_end < window_end ? ram_end : window_end;
        if (high <= low || size > high - low)
            continue;

        if (prefer_high) {
            al_u64 candidate = align_down(high - size, alignment);
            while (candidate >= low && candidate <= high - size) {
                if (range_free(ctx, candidate, size)) {
                    if (remember_allocation(ctx, candidate, size))
                        return -3;
                    *result = candidate;
                    return 0;
                }
                if (candidate < low + alignment)
                    break;
                candidate -= alignment;
            }
        } else {
            al_u64 candidate = align_up(low, alignment);
            while (candidate && candidate <= high - size) {
                if (range_free(ctx, candidate, size)) {
                    if (remember_allocation(ctx, candidate, size))
                        return -3;
                    *result = candidate;
                    return 0;
                }
                if (candidate > ~(al_u64)0 - alignment)
                    break;
                candidate += alignment;
            }
        }
    }
    return -4;
}

static int allocate_fixed(struct android_context *ctx, al_u64 base,
                          al_u64 size, al_u64 *result)
{
    if (!range_free(ctx, base, size))
        return -1;
    if (remember_allocation(ctx, base, size))
        return -2;
    *result = base;
    return 0;
}

static int allocate_kernel(struct android_context *ctx, al_u64 requested,
                           al_u64 text_offset, al_u64 image_size,
                           al_u64 *kernel_address,
                           al_u64 *reservation_base,
                           al_u64 *reservation_size)
{
    if (!kernel_address || !reservation_base || !reservation_size ||
        image_size > ~(al_u64)0 - text_offset)
        return -1;
    al_u64 span = text_offset + image_size;

    if (requested) {
        if (requested < text_offset ||
            ((requested - text_offset) & (ANDROID_ALIGN_2M - 1u)))
            return -2;
        al_u64 base = requested - text_offset;
        al_u64 ignored;
        int rc = allocate_fixed(ctx, base, span, &ignored);
        if (rc)
            return -3 + rc;
        *kernel_address = requested;
        *reservation_base = base;
        *reservation_size = span;
        return 0;
    }

    for (al_u32 i = 0; i < ctx->map->nram; i++) {
        al_u64 start = align_up(ctx->map->ram[i].base, ANDROID_ALIGN_2M);
        al_u64 size = ctx->map->ram[i].size;
        if (!size || size > ~(al_u64)0 - ctx->map->ram[i].base)
            continue;
        al_u64 end = ctx->map->ram[i].base + size;
        while (start && start <= end && span <= end - start) {
            if (range_free(ctx, start, span)) {
                if (remember_allocation(ctx, start, span))
                    return -4;
                *kernel_address = start + text_offset;
                *reservation_base = start;
                *reservation_size = span;
                return 0;
            }
            if (start > ~(al_u64)0 - ANDROID_ALIGN_2M)
                break;
            start += ANDROID_ALIGN_2M;
        }
    }
    return -5;
}

static const struct gpt_partition *partition(const char *name)
{
    return gpt_find_partition(&android_gpt, name);
}

static int partition_bytes(const struct gpt_partition *p, al_u64 *size)
{
    if (!p || !size || p->last_lba < p->first_lba)
        return -1;
    al_u64 blocks = p->last_lba - p->first_lba + 1u;
    if (blocks > (~(al_u64)0) / ANDROID_BLOCK_SIZE)
        return -2;
    *size = blocks * ANDROID_BLOCK_SIZE;
    return 0;
}

static void copy_bytes(al_u8 *dst, const al_u8 *src, al_u64 n);

static al_u8 read_tail_block[ANDROID_BLOCK_SIZE]
    __attribute__((aligned(4096)));

static int read_partition(const struct gpt_partition *p, al_u64 offset,
                          al_u32 length, void *destination)
{
    al_u64 part_size, lba;
    al_u8 *out = (al_u8 *)destination;
    if (!p || !destination || partition_bytes(p, &part_size) ||
        (offset & (ANDROID_BLOCK_SIZE - 1u)) ||
        offset > part_size || length > part_size - offset)
        return -1;

    if (!length)
        return 0;
    lba = p->first_lba + offset / ANDROID_BLOCK_SIZE;
    if (lba > 0xffffffffULL)
        return -2;

    al_u32 full_blocks = length / ANDROID_BLOCK_SIZE;
    al_u32 tail_bytes = length % ANDROID_BLOCK_SIZE;
    while (full_blocks) {
        al_u32 count = full_blocks > ANDROID_READ_CHUNK ?
                       ANDROID_READ_CHUNK : full_blocks;
        if (lba + count > 0x100000000ULL)
            return -2;
        int rc = mtk_ufs_read_blocks(2u, (al_u32)lba, count, out);
        if (rc)
            return -3;
        lba += count;
        full_blocks -= count;
        out += (al_u64)count * ANDROID_BLOCK_SIZE;
    }
    if (tail_bytes) {
        if (lba > 0xffffffffULL)
            return -2;
        int rc = mtk_ufs_read_blocks(2u, (al_u32)lba, 1u, read_tail_block);
        if (rc)
            return -4;
        copy_bytes(out, read_tail_block, tail_bytes);
    }
    return 0;
}

static void copy_bytes(al_u8 *dst, const al_u8 *src, al_u64 n)
{
    for (al_u64 i = 0; i < n; i++)
        dst[i] = src[i];
}

static void zero_bytes(al_u8 *dst, al_u64 n)
{
    for (al_u64 i = 0; i < n; i++)
        dst[i] = 0;
}

static al_u32 bounded_string_length(const al_u8 *s, al_u32 cap)
{
    al_u32 n = 0;
    while (n < cap && s[n])
        n++;
    return n;
}

static int append_text(char *dst, al_u32 capacity, al_u32 *used,
                       const al_u8 *src, al_u32 src_capacity)
{
    al_u32 n = bounded_string_length(src, src_capacity);
    if (!n)
        return 0;
    if (*used && *used + 1u < capacity)
        dst[(*used)++] = ' ';
    if (n > capacity - *used - 1u)
        return -1;
    for (al_u32 i = 0; i < n; i++)
        dst[(*used)++] = (char)src[i];
    dst[*used] = 0;
    return 0;
}

static int contains_text(const char *haystack, const char *needle)
{
    for (al_u32 i = 0; haystack[i]; i++) {
        al_u32 j = 0;
        while (needle[j] && haystack[i + j] == needle[j])
            j++;
        if (!needle[j])
            return 1;
    }
    return 0;
}

static int ramdisk_format(const al_u8 *p, al_u32 n)
{
    if (!p || n < 4u)
        return 0;
    if ((p[0] == '0' && p[1] == '7' && p[2] == '0' &&
         (p[3] == '1' || p[3] == '2')))
        return 1; /* newc CPIO */
    if (p[0] == 0x1f && p[1] == 0x8b)
        return 2; /* gzip */
    if (read_le32(p) == 0x184c2102u)
        return 3; /* Linux legacy LZ4 */
    if (read_le32(p) == 0x184d2204u)
        return 4; /* LZ4 frame */
    if (p[0] == 0xfd && p[1] == 0x37 && p[2] == 0x7a && p[3] == 0x58)
        return 5; /* XZ */
    if (read_le32(p) == 0xfd2fb528u)
        return 6; /* Zstandard */
    return 0;
}

static al_u32 bootconfig_checksum(const al_u8 *p, al_u32 size)
{
    al_u32 sum = 0;
    for (al_u32 i = 0; i < size; i++)
        sum += p[i];
    return sum;
}

int android_boot_run(const struct memmap_info *map,
                     al_u64 live_dtb, al_u32 live_dtb_size,
                     al_u64 live_initrd_start, al_u64 live_initrd_end,
                     al_u64 shim_base)
{
    struct android_context ctx;
    const struct gpt_partition *boot_part, *vendor_part, *init_part;
    struct android_bootimg_info boot_info, init_info;
    struct android_vendor_boot_info vendor_info;
    al_u64 boot_part_size, vendor_part_size, init_part_size = 0;
    al_u64 kernel_input_addr, kernel_scratch_addr;
    al_u64 kernel_address, kernel_image_size, kernel_reserved_size;
    al_u64 kernel_reservation_base;
    al_u64 ramdisk_address, ramdisk_size, ramdisk_alloc_size;
    al_u64 dtb_source_addr, dtb_address;
    al_u64 kernel_window;
    al_u32 kernel_output_size = 0;
    al_u32 kernel_output_capacity;
    al_u32 generic_ramdisk_size = 0;
    al_u32 bootconfig_trailer = 0;
    al_u32 dtb_output_size = 0;
    al_u32 dtb_capacity;
    al_u32 cmdline_used = 0;
    char cmdline[4096];
    int rc;

    if (!map || live_initrd_end <= live_initrd_start)
        return -1;
    ctx.map = map;
    ctx.live_dtb = live_dtb;
    ctx.live_dtb_size = live_dtb_size;
    ctx.live_initrd_start = live_initrd_start;
    ctx.live_initrd_end = live_initrd_end;
    ctx.shim_base = shim_base;
    ctx.shim_size = (al_u64)(_end - _start);
    ctx.allocation_count = 0;
    for (al_u32 i = 0; i < ANDROID_ALLOC_MAX; i++) {
        ctx.allocations[i].base = 0;
        ctx.allocations[i].size = 0;
    }

    logf("ANDROID BOOT: starting read-only slot A loader");
    rc = mtk_ufs_init();
    if (rc) return -10 + rc;
    rc = gpt_read_table(2u, &android_gpt);
    if (rc) return -20 + rc;

    boot_part = partition("boot_a");
    vendor_part = partition("vendor_boot_a");
    init_part = partition("init_boot_a");
    if (!boot_part || !vendor_part)
        return -30;
    if (partition_bytes(boot_part, &boot_part_size) ||
        partition_bytes(vendor_part, &vendor_part_size))
        return -31;

    if (read_partition(boot_part, 0, sizeof(boot_header), boot_header))
        return -32;
    rc = android_bootimg_parse(boot_header, boot_part_size, &boot_info);
    if (rc) return -40 + rc;
    if (!boot_info.kernel_size ||
        boot_info.kernel_size > ANDROID_KERNEL_IN_MAX)
        return -50;
    logf("ANDROID BOOT: boot_a v=%d kernel=%d ramdisk=%d",
         (al_u64)boot_info.header_version, (al_u64)boot_info.kernel_size,
         (al_u64)boot_info.ramdisk_size);

    if (read_partition(vendor_part, 0, sizeof(vendor_header), vendor_header))
        return -51;
    rc = android_vendor_boot_parse(vendor_header, vendor_part_size, &vendor_info);
    if (rc) return -60 + rc;
    if (!vendor_info.vendor_ramdisk_size || !vendor_info.dtb_size ||
        vendor_info.dtb_size > ANDROID_DTB_MAX)
        return -70;
    logf("ANDROID BOOT: vendor_boot_a v=%d ramdisk=%d dtb=%d bootconfig=%d",
         (al_u64)vendor_info.header_version,
         (al_u64)vendor_info.vendor_ramdisk_size,
         (al_u64)vendor_info.dtb_size, (al_u64)vendor_info.bootconfig_size);

    if (init_part) {
        if (partition_bytes(init_part, &init_part_size))
            return -71;
        if (read_partition(init_part, 0, sizeof(init_header), init_header))
            return -72;
        rc = android_bootimg_parse(init_header, init_part_size, &init_info);
        if (rc) return -73 + rc;
        if (init_info.ramdisk_size)
            generic_ramdisk_size = init_info.ramdisk_size;
        else
            init_part = 0;
    }
    if (!init_part) {
        generic_ramdisk_size = boot_info.ramdisk_size;
        init_info = boot_info;
    }
    if (!generic_ramdisk_size)
        return -80;
    logf("ANDROID BOOT: generic ramdisk=%d source=%s",
         (al_u64)generic_ramdisk_size,
         init_part ? "init_boot_a" : "boot_a");

    /* Stage the compressed kernel away from the live LK initrd and shim. */
    al_u64 kernel_input_size = align_up(boot_info.kernel_size, ANDROID_BLOCK_SIZE);
    if (!kernel_input_size ||
        allocate_range(&ctx, kernel_input_size, ANDROID_BLOCK_SIZE,
                       0, 0, 1, &kernel_input_addr))
        return -90;
    kernel_output_capacity = boot_info.kernel_size * 4u;
    if (kernel_output_capacity < 0x02000000u)
        kernel_output_capacity = 0x02000000u;
    if (kernel_output_capacity > ANDROID_KERNEL_OUT_MAX)
        kernel_output_capacity = ANDROID_KERNEL_OUT_MAX;
    kernel_output_capacity = (al_u32)align_up(kernel_output_capacity,
                                               ANDROID_BLOCK_SIZE);
    if (allocate_range(&ctx, kernel_output_capacity, ANDROID_BLOCK_SIZE,
                       0, 0, 1, &kernel_scratch_addr))
        return -91;
    rc = read_partition(boot_part, boot_info.kernel_offset,
                        boot_info.kernel_size, (void *)kernel_input_addr);
    if (rc) return -92 + rc;
    rc = android_kernel_unpack((const al_u8 *)kernel_input_addr,
                               boot_info.kernel_size,
                               (al_u8 *)kernel_scratch_addr,
                               kernel_output_capacity, &kernel_output_size);
    if (rc) return -100 + rc;
    if (kernel_output_size < 64u ||
        ((const al_u8 *)kernel_scratch_addr)[56] != 'A' ||
        ((const al_u8 *)kernel_scratch_addr)[57] != 'R' ||
        ((const al_u8 *)kernel_scratch_addr)[58] != 'M' ||
        ((const al_u8 *)kernel_scratch_addr)[59] != 0x64)
        return -110;

    const al_u8 *image = (const al_u8 *)kernel_scratch_addr;
    al_u64 text_offset = read_le64(image + 8u);
    al_u64 image_size = read_le64(image + 16u);
    al_u64 image_flags = read_le64(image + 24u);
    if (text_offset >= ANDROID_ALIGN_2M ||
        (image_flags & 1u) || ((image_flags >> 1) & 3u) == 3u)
        return -111;
    if (!image_size)
        image_size = align_up(kernel_output_size, ANDROID_ALIGN_2M);
    if (image_size < kernel_output_size ||
        image_size > ANDROID_KERNEL_OUT_MAX)
        return -112;
    kernel_image_size = image_size;

    rc = allocate_kernel(&ctx, vendor_info.kernel_addr, text_offset,
                         kernel_image_size, &kernel_address,
                         &kernel_reservation_base, &kernel_reserved_size);
    if (rc) return -120 + rc;
    copy_bytes((al_u8 *)kernel_address, image, kernel_output_size);
    if (kernel_image_size > kernel_output_size)
        zero_bytes((al_u8 *)(kernel_address + kernel_output_size),
                   kernel_image_size - kernel_output_size);
    __asm__ volatile("dsb sy" ::: "memory");
    logf("ANDROID BOOT: kernel PA=%x size=%x flags=%x",
         kernel_address, kernel_output_size, image_flags);

    /* The staging pages are dead now; make them available to later placement. */
    ctx.allocation_count = 0;
    if (remember_allocation(&ctx, kernel_reservation_base, kernel_reserved_size))
        return -121;
    if (kernel_reserved_size &&
        align_down(kernel_reservation_base, ANDROID_WINDOW_1G) !=
        align_down(kernel_reservation_base + kernel_reserved_size - 1u,
                   ANDROID_WINDOW_1G))
        return -122;
    kernel_window = align_down(kernel_address, ANDROID_WINDOW_1G);

    al_u64 vendor_ramdisk_size = vendor_info.vendor_ramdisk_size;
    al_u64 generic_size = generic_ramdisk_size;
    al_u64 bootconfig_size = vendor_info.bootconfig_size;
    al_u64 ramdisk_payload_size = vendor_ramdisk_size + generic_size;
    if (ramdisk_payload_size < vendor_ramdisk_size ||
        ramdisk_payload_size > ANDROID_RAMDISK_MAX ||
        bootconfig_size > ANDROID_RAMDISK_MAX - ramdisk_payload_size)
        return -130;
    ramdisk_size = ramdisk_payload_size + bootconfig_size;
    if (bootconfig_size) {
        al_u64 padded = align_up(bootconfig_size, 4u);
        if (!padded || padded > ANDROID_RAMDISK_MAX - ramdisk_size - 16u)
            return -131;
        ramdisk_size += padded - bootconfig_size + 16u;
        bootconfig_trailer = (al_u32)(padded - bootconfig_size + 16u);
    }
    if (ramdisk_size > ANDROID_RAMDISK_MAX)
        return -132;
    ramdisk_alloc_size = align_up(ramdisk_size, ANDROID_BLOCK_SIZE);
    if (!ramdisk_alloc_size || ramdisk_alloc_size > ANDROID_RAMDISK_MAX)
        return -133;

    if (vendor_info.ramdisk_addr) {
        rc = allocate_fixed(&ctx, vendor_info.ramdisk_addr,
                            ramdisk_alloc_size, &ramdisk_address);
    } else {
        rc = allocate_range(&ctx, ramdisk_alloc_size, ANDROID_ALIGN_2M,
                            kernel_window, ANDROID_WINDOW_1G, 0,
                            &ramdisk_address);
    }
    if (rc) return -140 + rc;
    if (align_down(ramdisk_address, ANDROID_WINDOW_1G) != kernel_window ||
        ramdisk_alloc_size > kernel_window + ANDROID_WINDOW_1G - ramdisk_address)
        return -141;
    rc = read_partition(vendor_part, vendor_info.ramdisk_offset,
                        vendor_info.vendor_ramdisk_size,
                        (void *)ramdisk_address);
    if (rc) return -150 + rc;

    al_u64 generic_partition_offset = init_part ?
        init_info.ramdisk_offset : boot_info.ramdisk_offset;
    const struct gpt_partition *generic_part = init_part ? init_part : boot_part;
    rc = read_partition(generic_part, generic_partition_offset,
                        generic_ramdisk_size,
                        (void *)(ramdisk_address + vendor_ramdisk_size));
    if (rc) return -151 + rc;

    int vendor_format = ramdisk_format((const al_u8 *)ramdisk_address,
                                       vendor_info.vendor_ramdisk_size);
    int generic_format = ramdisk_format(
        (const al_u8 *)(ramdisk_address + vendor_ramdisk_size),
        generic_ramdisk_size);
    if (!vendor_format || !generic_format || vendor_format != generic_format)
        return -152;
    logf("ANDROID BOOT: ramdisk format=%d vendor=%d generic=%d",
         (al_u64)vendor_format, vendor_ramdisk_size, generic_size);

    if (bootconfig_size) {
        al_u64 bootconfig_offset = vendor_info.bootconfig_offset;
        al_u64 bootconfig_address = ramdisk_address + ramdisk_payload_size;
        rc = read_partition(vendor_part, bootconfig_offset,
                            vendor_info.bootconfig_size,
                            (void *)bootconfig_address);
        if (rc) return -153 + rc;
        al_u32 padded_size = (al_u32)align_up(bootconfig_size, 4u);
        zero_bytes((al_u8 *)(bootconfig_address + bootconfig_size),
                   padded_size - (al_u32)bootconfig_size);
        al_u8 *footer = (al_u8 *)(bootconfig_address + padded_size);
        write_le32(footer, padded_size);
        write_le32(footer + 4u,
                   bootconfig_checksum((const al_u8 *)bootconfig_address,
                                       padded_size));
        static const al_u8 magic[12] = {
            '#','B','O','O','T','C','O','N','F','I','G','\n'
        };
        copy_bytes(footer + 8u, magic, sizeof(magic));
        __asm__ volatile("dsb sy" ::: "memory");
        logf("ANDROID BOOT: bootconfig=%d trailer=%d",
             bootconfig_size, (al_u64)bootconfig_trailer);
    }

    /* Use the DTB embedded in vendor_boot, never the shim's live LK DTB. */
    if (allocate_range(&ctx, ANDROID_DTB_MAX, ANDROID_ALIGN_2M,
                       kernel_window, ANDROID_WINDOW_1G, 1,
                       &dtb_source_addr))
        return -160;
    rc = read_partition(vendor_part, vendor_info.dtb_offset,
                        vendor_info.dtb_size, (void *)dtb_source_addr);
    if (rc) return -161 + rc;

    dtb_capacity = ANDROID_DTB_MAX;
    if (vendor_info.dtb_addr) {
        rc = allocate_fixed(&ctx, vendor_info.dtb_addr, dtb_capacity,
                            &dtb_address);
    } else {
        rc = allocate_range(&ctx, dtb_capacity, ANDROID_ALIGN_2M,
                            kernel_window, ANDROID_WINDOW_1G, 0,
                            &dtb_address);
    }
    if (rc) return -163 + rc;
    if ((dtb_address & 7u) ||
        (vendor_info.dtb_addr &&
         overlaps(dtb_address, dtb_capacity, dtb_source_addr, ANDROID_DTB_MAX)))
        return -164;

    cmdline[0] = 0;
    if (append_text(cmdline, sizeof(cmdline), &cmdline_used,
                    vendor_header + 28u, 2048u) ||
        append_text(cmdline, sizeof(cmdline), &cmdline_used,
                    boot_header + 44u, 1536u))
        return -165;
    if (!contains_text(cmdline, "androidboot.slot_suffix=")) {
        static const al_u8 slot_suffix[] = "androidboot.slot_suffix=_a";
        if (append_text(cmdline, sizeof(cmdline), &cmdline_used,
                        slot_suffix, sizeof(slot_suffix) - 1u))
            return -166;
    }
    if (!contains_text(cmdline, "androidboot.slot=")) {
        static const al_u8 slot[] = "androidboot.slot=a";
        if (append_text(cmdline, sizeof(cmdline), &cmdline_used,
                        slot, sizeof(slot) - 1u))
            return -167;
    }

    rc = fdt_edit_android_chosen((const al_u8 *)dtb_source_addr,
                                 vendor_info.dtb_size,
                                 (al_u8 *)dtb_address, dtb_capacity,
                                 cmdline, ramdisk_address,
                                 ramdisk_address + ramdisk_size,
                                 &dtb_output_size);
    if (rc) return -180 + rc;

    __asm__ volatile("dsb sy" ::: "memory");
    logf("ANDROID BOOT: DTB PA=%x size=%d", dtb_address,
         (al_u64)dtb_output_size);
    logf("ANDROID BOOT: initrd=%x..%x size=%x",
         ramdisk_address, ramdisk_address + ramdisk_size, ramdisk_size);
    logf("ANDROID BOOT: kernel entry=%x slot=_a", kernel_address);
    logf("ANDROID BOOT: Linux ARM64 handoff EL1, MMU off");
    transfer_to_linux(kernel_address, dtb_address);
}
