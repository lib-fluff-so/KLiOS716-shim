#ifndef KLIOS716_ANDROID_KERNEL_H
#define KLIOS716_ANDROID_KERNEL_H

typedef unsigned char ak_u8;
typedef unsigned int ak_u32;

/* Accept a raw arm64 Image, a gzip stream, or the Linux legacy LZ4 stream.
 * Returns zero and writes the unpacked byte count on success. */
int android_kernel_unpack(const ak_u8 *src, ak_u32 src_size,
                          ak_u8 *dst, ak_u32 dst_capacity,
                          ak_u32 *dst_size);

#endif
