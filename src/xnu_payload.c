/* Freestanding parser for ios-sweet's SWEETXNU v1 payload container. */
#include "xnu_payload.h"

#define U64_MAX_VALUE ((loader_u64)~(loader_u64)0)
#define PAYLOAD_SEGMENT_SIZE 32u
#define PAYLOAD_CMDLINE_OFF 0x48u
#define PAYLOAD_SEGMENTS_OFF XNU_PAYLOAD_HEADER_SIZE
#define MAX_CONTAINER_SIZE 0x10000000UL

static loader_u32 rd32le(const loader_u8 *p)
{
    return (loader_u32)p[0] | ((loader_u32)p[1] << 8) |
           ((loader_u32)p[2] << 16) | ((loader_u32)p[3] << 24);
}

static loader_u64 rd64le(const loader_u8 *p)
{
    return (loader_u64)rd32le(p) | ((loader_u64)rd32le(p + 4) << 32);
}

static int bytes_equal(const loader_u8 *a, const char *b, loader_u32 n)
{
    for (loader_u32 i = 0; i < n; i++)
        if (a[i] != (loader_u8)b[i]) return 0;
    return 1;
}

static int add_overflows(loader_u64 a, loader_u64 b)
{
    return b > U64_MAX_VALUE - a;
}

static int round_up_8(loader_u64 n, loader_u64 *out)
{
    if (n > U64_MAX_VALUE - 7u) return 0;
    *out = (n + 7u) & ~(loader_u64)7u;
    return 1;
}

int xnu_payload_get_segment(const struct xnu_payload_view *p,
                            loader_u32 index, struct xnu_payload_segment *out)
{
    if (!p || !p->blob || !out || index >= p->nsegs) return -1;
    loader_u64 off = PAYLOAD_SEGMENTS_OFF + (loader_u64)index * PAYLOAD_SEGMENT_SIZE;
    if (off > p->blob_size || PAYLOAD_SEGMENT_SIZE > p->blob_size - off) return -2;
    const loader_u8 *s = p->blob + off;
    out->vmaddr = rd64le(s + 0);
    out->vmsize = rd64le(s + 8);
    out->file_off = rd64le(s + 16);
    out->file_size = rd64le(s + 24);
    return 0;
}

int xnu_payload_parse(const loader_u8 *blob, loader_u64 blob_size,
                      struct xnu_payload_view *out)
{
    if (!blob || !out || blob_size < XNU_PAYLOAD_HEADER_SIZE ||
        blob_size > MAX_CONTAINER_SIZE)
        return -1;
    if (!bytes_equal(blob, XNU_PAYLOAD_MAGIC, 8)) return -2;

    loader_u32 version = rd32le(blob + 8);
    loader_u32 nsegs = rd32le(blob + 12);
    if (version != XNU_PAYLOAD_VERSION || !nsegs || nsegs > XNU_PAYLOAD_MAX_SEGS)
        return -3;

    loader_u64 seg_table_end = PAYLOAD_SEGMENTS_OFF + (loader_u64)nsegs * PAYLOAD_SEGMENT_SIZE;
    if (seg_table_end > blob_size) return -4;

    struct xnu_payload_view p;
    p.blob = blob;
    p.blob_size = blob_size;
    p.version = version;
    p.nsegs = nsegs;
    p.virt_base = rd64le(blob + 0x10);
    p.entry_va = rd64le(blob + 0x18);
    p.adt_off = rd64le(blob + 0x20);
    p.adt_size = rd64le(blob + 0x28);
    p.kernel_end = rd64le(blob + 0x30);
    p.phys_base = rd64le(blob + 0x38);
    p.mem_size = rd64le(blob + 0x40);
    p.cmdline = blob + PAYLOAD_CMDLINE_OFF;

    if (!p.virt_base || !p.phys_base || !p.mem_size ||
        (p.phys_base & (XNU_PAGE_SIZE - 1u)) ||
        (p.kernel_end & (XNU_PAGE_SIZE - 1u)) ||
        p.adt_off < seg_table_end || (p.adt_off & 7u) || !p.adt_size ||
        p.adt_off > blob_size || p.adt_size > blob_size - p.adt_off ||
        add_overflows(p.phys_base, p.mem_size) ||
        add_overflows(p.virt_base, p.mem_size))
        return -5;

    /* The on-disk command line must be NUL terminated within its fixed field. */
    int cmdline_terminated = 0;
    for (loader_u32 i = 0; i < XNU_BOOT_LINE_LENGTH; i++) {
        if (!p.cmdline[i]) { cmdline_terminated = 1; break; }
    }
    if (!cmdline_terminated) return -6;

    loader_u64 max_end_va = p.virt_base;
    int entry_found = 0;
    for (loader_u32 i = 0; i < nsegs; i++) {
        struct xnu_payload_segment s;
        if (xnu_payload_get_segment(&p, i, &s)) return -7;
        loader_u64 copied_span = 0;
        if (!s.vmsize || s.vmaddr < p.virt_base ||
            s.file_size > s.vmsize || !round_up_8(s.file_size, &copied_span) ||
            copied_span > s.vmsize || (s.file_off & 7u) ||
            s.file_off < seg_table_end || s.file_off > p.adt_off ||
            s.file_size > p.adt_off - s.file_off ||
            copied_span > p.adt_off - s.file_off ||
            add_overflows(s.vmaddr, s.vmsize))
            return -8;

        loader_u64 end_va = s.vmaddr + s.vmsize;
        if (end_va > max_end_va) max_end_va = end_va;
        if (p.entry_va >= s.vmaddr && p.entry_va < end_va) entry_found = 1;

        /* Reject overlapping virtual segments; malformed overlaps can make a
         * later segment silently overwrite an earlier one during loading. */
        for (loader_u32 j = 0; j < i; j++) {
            struct xnu_payload_segment prior;
            if (xnu_payload_get_segment(&p, j, &prior)) return -7;
            loader_u64 prior_end = prior.vmaddr + prior.vmsize;
            if (s.vmaddr < prior_end && prior.vmaddr < end_va) return -9;
        }
    }

    loader_u64 adt_copy_span = 0;
    if (!round_up_8(p.adt_size, &adt_copy_span) ||
        adt_copy_span > blob_size - p.adt_off) return -11;

    if (!entry_found || p.kernel_end < max_end_va ||
        p.kernel_end > p.virt_base + p.mem_size)
        return -12;

    *out = p;
    return 0;
}
