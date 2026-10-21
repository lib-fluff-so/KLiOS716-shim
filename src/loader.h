/* Freestanding newc CPIO and test-payload parser shared with host tests. */
#ifndef IOS_KL7_LOADER_H
#define IOS_KL7_LOADER_H

typedef unsigned char loader_u8;
typedef unsigned int loader_u32;
typedef unsigned long loader_u64;

#define LOADER_PAYLOAD_HEADER_SIZE 16u
#define LOADER_PAYLOAD_MAX         0x10000u
#define LOADER_PAYLOAD_MAGIC       "KL7PAY0\0"

/* Finds an exact filename in a raw/concatenated newc CPIO byte stream.
 * Returns 0 on success, a negative error if the archive is invalid or missing. */
int loader_cpio_find_file(const loader_u8 *archive, loader_u64 archive_size,
                          const char *name, const loader_u8 **file_data,
                          loader_u32 *file_size);

/* Validates [magic:8][payload_size:LE32][CRC32:LE32][payload bytes].
 * Returns 0 on success, negative values for invalid header/size/CRC. */
int loader_parse_payload(const loader_u8 *file, loader_u32 file_size,
                         const loader_u8 **payload, loader_u32 *payload_size,
                         loader_u32 *expected_crc);
loader_u32 loader_crc32(const loader_u8 *data, loader_u32 size);

#endif
