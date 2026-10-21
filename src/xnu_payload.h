/* Parser for the flat Mach-O segment container emitted by mkpayload_xnu.py. */
#ifndef IOS_KL7_XNU_PAYLOAD_H
#define IOS_KL7_XNU_PAYLOAD_H

#include "loader.h"

#define XNU_PAYLOAD_HEADER_SIZE 0x2a8u
#define XNU_PAYLOAD_VERSION     1u
#define XNU_PAYLOAD_MAX_SEGS    64u
#define XNU_BOOT_LINE_LENGTH    608u
#define XNU_PAGE_SIZE           0x4000u
#define XNU_PAYLOAD_MAGIC       "SWEETXNU"

struct xnu_payload_segment {
    loader_u64 vmaddr;
    loader_u64 vmsize;
    loader_u64 file_off;
    loader_u64 file_size;
};

struct xnu_payload_view {
    const loader_u8 *blob;
    loader_u64 blob_size;
    loader_u32 version;
    loader_u32 nsegs;
    loader_u64 virt_base;
    loader_u64 entry_va;
    loader_u64 adt_off;
    loader_u64 adt_size;
    loader_u64 kernel_end;
    loader_u64 phys_base;
    loader_u64 mem_size;
    const loader_u8 *cmdline;
};

/* Structural validation only: segment destinations must additionally be checked
 * against the live FDT/memory map by the platform loader before any writes. */
int xnu_payload_parse(const loader_u8 *blob, loader_u64 blob_size,
                      struct xnu_payload_view *out);
int xnu_payload_get_segment(const struct xnu_payload_view *payload,
                            loader_u32 index, struct xnu_payload_segment *out);

#endif
