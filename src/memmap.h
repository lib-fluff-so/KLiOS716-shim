#ifndef IOS_KL7_MEMMAP_H
#define IOS_KL7_MEMMAP_H

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

#define MEMMAP_MAX_RAM       4u
#define MEMMAP_MAX_RESERVED 64u
#define MEMMAP_MAX_FDT_RSV  64u
#define MEMMAP_MAX_NOMAP    64u

struct mem_range { u64 base, size; };

struct memmap_info {
    struct mem_range ram[MEMMAP_MAX_RAM];
    u32 nram;
    struct mem_range reserved[MEMMAP_MAX_RESERVED];
    u32 nreserved;
    struct mem_range fdt_reserved[MEMMAP_MAX_FDT_RSV];
    u32 nfdt_reserved;
    struct mem_range nomap[MEMMAP_MAX_NOMAP];
    u32 nnomap;
};

/* Parses the live flattened device tree and records usable RAM plus every
 * reservation that the shim must avoid. Returns 0 on success. */
int memmap_parse_dtb(const u8 *dtb, struct memmap_info *out);

#endif
