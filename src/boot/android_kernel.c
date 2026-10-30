#include "android_kernel.h"

typedef unsigned short ak_u16;

#define DEFLATE_MAX_BITS 15u
#define DEFLATE_MAX_SYMBOLS 288u

struct bit_reader {
    const ak_u8 *src;
    ak_u32 size;
    ak_u32 pos;
    ak_u32 bits;
    ak_u32 count;
};

struct huffman {
    ak_u16 count[DEFLATE_MAX_BITS + 1u];
    ak_u16 symbol[DEFLATE_MAX_SYMBOLS];
};

static ak_u32 le32(const ak_u8 *p)
{
    return (ak_u32)p[0] | ((ak_u32)p[1] << 8) |
           ((ak_u32)p[2] << 16) | ((ak_u32)p[3] << 24);
}

static ak_u32 crc32_update(ak_u32 crc, const ak_u8 *p, ak_u32 size)
{
    for (ak_u32 i = 0; i < size; i++) {
        crc ^= p[i];
        for (ak_u32 b = 0; b < 8u; b++)
            crc = (crc >> 1) ^ (0xedb88320u & (ak_u32)-(int)(crc & 1u));
    }
    return crc;
}

static int bits_read(struct bit_reader *r, ak_u32 n, ak_u32 *value)
{
    if (n > 24u || !value)
        return -1;
    while (r->count < n) {
        if (r->pos >= r->size)
            return -1;
        r->bits |= (ak_u32)r->src[r->pos++] << r->count;
        r->count += 8u;
    }
    *value = r->bits & ((1u << n) - 1u);
    r->bits >>= n;
    r->count -= n;
    return 0;
}

static int bits_align(struct bit_reader *r)
{
    ak_u32 discard = r->count & 7u;
    r->bits >>= discard;
    r->count -= discard;
    return 0;
}

static int huffman_build(struct huffman *h, const ak_u8 *lengths,
                         ak_u32 n, ak_u32 capacity)
{
    ak_u16 offsets[DEFLATE_MAX_BITS + 1u];
    ak_u32 left = 1u;

    if (!h || !lengths || n > capacity || capacity > DEFLATE_MAX_SYMBOLS)
        return -1;
    for (ak_u32 i = 0; i <= DEFLATE_MAX_BITS; i++)
        h->count[i] = 0;
    for (ak_u32 i = 0; i < n; i++) {
        if (lengths[i] > DEFLATE_MAX_BITS)
            return -2;
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;

    for (ak_u32 len = 1; len <= DEFLATE_MAX_BITS; len++) {
        left <<= 1;
        if (left < h->count[len])
            return -3;
        left -= h->count[len];
    }

    offsets[1] = 0;
    for (ak_u32 len = 1; len < DEFLATE_MAX_BITS; len++)
        offsets[len + 1u] = (ak_u16)(offsets[len] + h->count[len]);

    for (ak_u32 sym = 0; sym < n; sym++) {
        ak_u32 len = lengths[sym];
        if (len)
            h->symbol[offsets[len]++] = (ak_u16)sym;
    }
    return 0;
}

/* Canonical Huffman decoder. DEFLATE transmits each Huffman code most
 * significant bit first, although the surrounding fields are LSB-first. */
static int huffman_decode(struct bit_reader *r, const struct huffman *h,
                          ak_u32 *symbol)
{
    ak_u32 code = 0, first = 0, index = 0;
    for (ak_u32 len = 1; len <= DEFLATE_MAX_BITS; len++) {
        ak_u32 bit;
        if (bits_read(r, 1u, &bit))
            return -1;
        code |= bit;
        ak_u32 count = h->count[len];
        if (code >= first && code - first < count) {
            *symbol = h->symbol[index + code - first];
            return 0;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static int fixed_trees(struct huffman *lit, struct huffman *dist)
{
    ak_u8 ll[288], dd[32];
    for (ak_u32 i = 0; i <= 143u; i++) ll[i] = 8;
    for (ak_u32 i = 144u; i <= 255u; i++) ll[i] = 9;
    for (ak_u32 i = 256u; i <= 279u; i++) ll[i] = 7;
    for (ak_u32 i = 280u; i <= 287u; i++) ll[i] = 8;
    for (ak_u32 i = 0; i < 32u; i++) dd[i] = 5;
    if (huffman_build(lit, ll, 288u, 288u))
        return -1;
    if (huffman_build(dist, dd, 32u, 32u))
        return -2;
    return 0;
}

static int dynamic_trees(struct bit_reader *r, struct huffman *lit,
                         struct huffman *dist)
{
    static const ak_u8 order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    ak_u8 code_lengths[19];
    ak_u8 lengths[288 + 32];
    struct huffman code_tree;
    ak_u32 hlit, hdist, hclen, v;

    if (bits_read(r, 5u, &v)) return -1;
    hlit = v + 257u;
    if (bits_read(r, 5u, &v)) return -1;
    hdist = v + 1u;
    if (bits_read(r, 4u, &v)) return -1;
    hclen = v + 4u;
    if (hlit > 286u || hdist > 32u)
        return -2;

    for (ak_u32 i = 0; i < 19u; i++) code_lengths[i] = 0;
    for (ak_u32 i = 0; i < hclen; i++) {
        if (bits_read(r, 3u, &v)) return -1;
        code_lengths[order[i]] = (ak_u8)v;
    }
    if (huffman_build(&code_tree, code_lengths, 19u, 19u))
        return -3;

    ak_u32 total = hlit + hdist;
    ak_u32 used = 0;
    while (used < total) {
        ak_u32 sym;
        if (huffman_decode(r, &code_tree, &sym))
            return -4;
        if (sym <= 15u) {
            lengths[used++] = (ak_u8)sym;
        } else if (sym == 16u) {
            ak_u32 repeat;
            if (!used || bits_read(r, 2u, &v)) return -5;
            repeat = v + 3u;
            if (repeat > total - used) return -5;
            while (repeat--) lengths[used] = lengths[used - 1u], used++;
        } else if (sym == 17u) {
            ak_u32 repeat;
            if (bits_read(r, 3u, &v)) return -5;
            repeat = v + 3u;
            if (repeat > total - used) return -5;
            while (repeat--) lengths[used++] = 0;
        } else if (sym == 18u) {
            ak_u32 repeat;
            if (bits_read(r, 7u, &v)) return -5;
            repeat = v + 11u;
            if (repeat > total - used) return -5;
            while (repeat--) lengths[used++] = 0;
        } else {
            return -5;
        }
    }
    if (!lengths[256u])
        return -6;
    if (huffman_build(lit, lengths, hlit, 288u))
        return -7;
    if (huffman_build(dist, lengths + hlit, hdist, 32u))
        return -8;
    return 0;
}

static int inflate_codes(struct bit_reader *r, const struct huffman *lit,
                         const struct huffman *dist, ak_u8 *out,
                         ak_u32 cap, ak_u32 *out_pos)
{
    static const ak_u16 length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
        67,83,99,115,131,163,195,227,258
    };
    static const ak_u8 length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,
        4,4,4,4,5,5,5,5,0
    };
    static const ak_u16 distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
        513,769,1025,1537,2049,3073,4097,6145,8193,12289,
        16385,24577
    };
    static const ak_u8 distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
        9,9,10,10,11,11,12,12,13,13
    };

    for (;;) {
        ak_u32 sym;
        if (huffman_decode(r, lit, &sym))
            return -1;
        if (sym < 256u) {
            if (*out_pos >= cap) return -2;
            out[(*out_pos)++] = (ak_u8)sym;
            continue;
        }
        if (sym == 256u)
            return 0;
        if (sym < 257u || sym > 285u)
            return -3;

        ak_u32 li = sym - 257u;
        ak_u32 extra = 0;
        if (length_extra[li] && bits_read(r, length_extra[li], &extra))
            return -4;
        ak_u32 length = (ak_u32)length_base[li] + extra;
        ak_u32 dsym;
        if (huffman_decode(r, dist, &dsym) || dsym >= 30u)
            return -5;
        extra = 0;
        if (distance_extra[dsym] &&
            bits_read(r, distance_extra[dsym], &extra))
            return -6;
        ak_u32 distance = (ak_u32)distance_base[dsym] + extra;
        if (!distance || distance > *out_pos || length > cap - *out_pos)
            return -7;
        while (length--) {
            out[*out_pos] = out[*out_pos - distance];
            (*out_pos)++;
        }
    }
}

static int inflate_raw(const ak_u8 *src, ak_u32 src_size, ak_u32 offset,
                       ak_u8 *dst, ak_u32 dst_capacity, ak_u32 *dst_size,
                       ak_u32 *consumed)
{
    struct bit_reader r = { src, src_size, offset, 0, 0 };
    struct huffman lit, dist;
    ak_u32 out_pos = 0, final = 0, type;

    do {
        if (bits_read(&r, 1u, &final) || bits_read(&r, 2u, &type))
            return -1;
        if (type == 0u) {
            ak_u32 len, nlen;
            bits_align(&r);
            if (bits_read(&r, 16u, &len) || bits_read(&r, 16u, &nlen) ||
                ((len ^ 0xffffu) & 0xffffu) != nlen ||
                len > dst_capacity - out_pos)
                return -2;
            for (ak_u32 i = 0; i < len; i++) {
                ak_u32 b;
                if (bits_read(&r, 8u, &b)) return -3;
                dst[out_pos++] = (ak_u8)b;
            }
        } else if (type == 1u || type == 2u) {
            int rc = type == 1u ? fixed_trees(&lit, &dist) :
                                 dynamic_trees(&r, &lit, &dist);
            if (rc) return -4 + rc;
            rc = inflate_codes(&r, &lit, &dist, dst, dst_capacity, &out_pos);
            if (rc) return -8 + rc;
        } else {
            return -16;
        }
    } while (!final);

    bits_align(&r);
    *dst_size = out_pos;
    *consumed = r.pos - (r.count >> 3) - offset;
    return 0;
}

static int gzip_unpack(const ak_u8 *src, ak_u32 src_size, ak_u8 *dst,
                       ak_u32 cap, ak_u32 *out_size)
{
    if (src_size < 18u || src[0] != 0x1fu || src[1] != 0x8bu ||
        src[2] != 8u || (src[3] & 0xe0u))
        return -1;

    ak_u32 pos = 10u;
    ak_u8 flags = src[3];
    if (flags & 4u) {
        if (pos + 2u > src_size) return -2;
        ak_u32 extra = (ak_u32)src[pos] | ((ak_u32)src[pos + 1u] << 8);
        pos += 2u;
        if (extra > src_size - pos) return -2;
        pos += extra;
    }
    if (flags & 8u) {
        while (pos < src_size && src[pos++]) { }
        if (pos > src_size || src[pos - 1u] != 0) return -3;
    }
    if (flags & 16u) {
        while (pos < src_size && src[pos++]) { }
        if (pos > src_size || src[pos - 1u] != 0) return -3;
    }
    if (flags & 2u) {
        if (pos + 2u > src_size) return -4;
        pos += 2u;
    }

    ak_u32 produced = 0, consumed = 0;
    int rc = inflate_raw(src, src_size, pos, dst, cap, &produced, &consumed);
    if (rc) return -10 + rc;
    pos += consumed;
    if (pos + 8u > src_size)
        return -20;
    ak_u32 expected_crc = le32(src + pos);
    ak_u32 expected_size = le32(src + pos + 4u);
    ak_u32 actual_crc = ~crc32_update(0xffffffffu, dst, produced);
    if (actual_crc != expected_crc || expected_size != produced)
        return -21;
    *out_size = produced;
    return 0;
}

static int lz4_block(const ak_u8 *src, ak_u32 src_size, ak_u8 *dst,
                     ak_u32 dst_capacity, ak_u32 *written)
{
    ak_u32 ip = 0, op = 0;
    while (ip < src_size) {
        ak_u32 token = src[ip++];
        ak_u32 literal = token >> 4;
        if (literal == 15u) {
            ak_u32 v;
            do {
                if (ip >= src_size) return -1;
                v = src[ip++];
                if (literal > 0xffffffffu - v) return -1;
                literal += v;
            } while (v == 255u);
        }
        if (literal > src_size - ip || literal > dst_capacity - op)
            return -2;
        for (ak_u32 i = 0; i < literal; i++)
            dst[op++] = src[ip++];
        if (ip == src_size)
            break;
        if (ip + 2u > src_size)
            return -3;
        ak_u32 distance = (ak_u32)src[ip] | ((ak_u32)src[ip + 1u] << 8);
        ip += 2u;
        if (!distance || distance > op)
            return -4;
        ak_u32 match = token & 15u;
        if (match == 15u) {
            ak_u32 v;
            do {
                if (ip >= src_size) return -5;
                v = src[ip++];
                if (match > 0xffffffffu - v) return -5;
                match += v;
            } while (v == 255u);
        }
        match += 4u;
        if (match > dst_capacity - op)
            return -6;
        while (match--) {
            dst[op] = dst[op - distance];
            op++;
        }
    }
    *written = op;
    return 0;
}

static int lz4_legacy_unpack(const ak_u8 *src, ak_u32 src_size, ak_u8 *dst,
                             ak_u32 cap, ak_u32 *out_size)
{
    if (src_size < 8u || le32(src) != 0x184c2102u)
        return -1;
    ak_u32 ip = 4u, op = 0;
    while (ip + 4u <= src_size) {
        ak_u32 block = le32(src + ip);
        ip += 4u;
        if (!block)
            break;
        int raw = (block & 0x80000000u) != 0;
        block &= 0x7fffffffu;
        if (!block || block > src_size - ip || block > 8u * 1024u * 1024u)
            return -2;
        if (raw) {
            if (block > cap - op) return -3;
            for (ak_u32 i = 0; i < block; i++) dst[op++] = src[ip++];
        } else {
            ak_u32 produced = 0;
            int rc = lz4_block(src + ip, block, dst + op, cap - op, &produced);
            if (rc) return -4 + rc;
            op += produced;
            ip += block;
        }
    }
    if (!op)
        return -5;
    *out_size = op;
    return 0;
}

int android_kernel_unpack(const ak_u8 *src, ak_u32 src_size,
                          ak_u8 *dst, ak_u32 dst_capacity,
                          ak_u32 *dst_size)
{
    if (!src || !dst || !dst_size || !src_size || !dst_capacity)
        return -1;
    if (src_size >= 64u && src[56] == 'A' && src[57] == 'R' &&
        src[58] == 'M' && src[59] == 0x64) {
        if (src_size > dst_capacity)
            return -2;
        for (ak_u32 i = 0; i < src_size; i++) dst[i] = src[i];
        *dst_size = src_size;
        return 0;
    }
    if (src_size >= 2u && src[0] == 0x1fu && src[1] == 0x8bu)
        return gzip_unpack(src, src_size, dst, dst_capacity, dst_size);
    if (src_size >= 4u && le32(src) == 0x184c2102u)
        return lz4_legacy_unpack(src, src_size, dst, dst_capacity, dst_size);
    return -3;
}
