#ifndef KLIOS716_ANDROID_LOADER_H
#define KLIOS716_ANDROID_LOADER_H

#include "../memmap.h"

/* Read-only slot-A Android chainloader. On success it does not return. */
int android_boot_run(const struct memmap_info *map,
                     unsigned long long live_dtb,
                     unsigned int live_dtb_size,
                     unsigned long long live_initrd_start,
                     unsigned long long live_initrd_end,
                     unsigned long long shim_base);

#endif
