#ifndef KLIOS716_FDT_EDIT_H
#define KLIOS716_FDT_EDIT_H

typedef unsigned char fe_u8;
typedef unsigned int fe_u32;
typedef unsigned long long fe_u64;

/* Clone a DTB and replace /chosen/bootargs and initrd bounds. The output is
 * rebuilt with a compact structure/string layout and a valid FDT header. */
int fdt_get_chosen_bootargs(const fe_u8 *source, fe_u32 source_capacity,
                            char *destination, fe_u32 destination_capacity,
                            fe_u32 *result_size);

int fdt_edit_android_chosen(const fe_u8 *source, fe_u32 source_capacity,
                            fe_u8 *destination, fe_u32 destination_capacity,
                            const char *bootargs, fe_u64 initrd_start,
                            fe_u64 initrd_end, fe_u32 *result_size);

#endif
