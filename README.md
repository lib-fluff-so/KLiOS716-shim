# KL7 dualboot development

This branch is the modular dualboot work-in-progress. It does not change the parent repository's pinned shim or the normal main branch.

## Build the shim

    make clean
    make -j$(nproc) all
    make hosttest loadertest xnutest androidimgtest
    make sizes

## Hardware test modes

Each mode is opt-in and runs instead of the normal XNU payload path:

    make OUT=out-menu MENU_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-ufs UFS_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-android ANDROID_BOOT_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-touch TOUCH_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-battery BATTERY_TEST=1 AUTO_REBOOT_TEST=0 all

The menu test uses the FTS3519 touchscreen driver and framebuffer drawing. It verifies touch coordinates by returning either Android or iOS as a choice; it does not yet chainload either operating system.

The Android boot inspection test reads the GPT on UFS LUN 2 and inspects boot_a, vendor_boot_a, init_boot_a, and vendor_boot_b. It parses Android boot image headers v3/v4 and vendor boot headers v3/v4. It is read-only and does not attempt to write boot partitions.

## Current implementation state

- Battery, FTS3519 touch, and MTK UFS are separate drivers with thin test clients.
- GPT parsing includes primary-header and partition-array CRC validation.
- Android boot and vendor_boot header parsing has host regression tests.
- The framebuffer menu and touch selection are implemented as a testable component.
- Android kernel/ramdisk loading, kernel decompression, vendor ramdisk selection/concatenation, final DTB preparation, and ARM64 Linux handoff remain to be implemented and validated on the device. Therefore this branch is not yet a functional dualboot release.

## Device safety

- Keep Android on slot A as the fallback.
- Flash only boot_b; never write boot_a.
- Keep boot_b at or below 64 MiB.
- The UFS driver currently issues SCSI reads only. Do not add writes to Android partitions or misc while implementing boot selection.
