# v0.5.2 — ramoops console logging

## Confirmed KL7 layout

The rooted Android diagnostic reported:

- `mem_address = 0x48090000`
- `mem_size = 0xe0000`
- `record_size = 0x1000`
- `console_size = 0x40000`
- `ftrace_size = 0x1000`
- `pmsg_size = 0x40000`
- `ecc = 0`

`/proc/iomem` maps the 95 4-KiB dmesg zones at `0x48090000..0x480eefff`, the console at `0x480ef000..0x4812efff`, ftrace at `0x4812f000..0x4812ffff`, and pmsg at `0x48130000..0x4816ffff`. `/sys/fs/pstore/console-ramoops-0` was present and had length 262132, exactly `0x40000 - sizeof(persistent_ram_buffer)`.

## Implementation

The shim writes only the console zone. Its 12-byte header is three 32-bit fields: signature `0x43474244` (the Linux constant `0x43474244` XOR ramoops console type 0), write position, and valid byte count, followed by up to 262132 bytes of circular data. With ECC disabled, no parity area is required. Page text is appended before the display buffer is cleared; the fatal exception screen is appended before the handler parks. `DSB SY` orders data and header stores before reset.

References used for format validation:

- Android common Linux 5.10 `fs/pstore/ram_core.c`: `struct persistent_ram_buffer`, signature validation and old-log extraction: https://android.googlesource.com/kernel/common/%2B/refs/tags/android12-5.10-2022-03_r7/fs/pstore/ram_core.c
- Android common Linux `fs/pstore/ram.c`: console zone allocation and ramoops region ordering: https://android.googlesource.com/kernel/common/%2B/3cb5835d2c6a32059570e168e5e485f3913a2776/fs/pstore/ram.c

## Not yet validated

Compilation and linking passed for the normal hardware image, exception-test image, and Unicorn compatibility image. The emulator packages are unavailable in the current build environment, so the Unicorn run was not repeated here. The phone has not yet been tested with this ramoops-writing build. In particular, retention through the actual reset/slot-switch path is not yet confirmed.

The first device test should boot the normal image, return to Android, and extract `/sys/fs/pstore/console-ramoops-0`. This code deliberately replaces the prior ramoops console contents with a shim log; it does not write into dmesg, ftrace, or pmsg zones.
