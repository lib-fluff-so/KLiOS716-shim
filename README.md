# KL7 dualboot development

This branch is the modular dualboot work-in-progress. It does not change the parent repository's pinned shim or the normal main branch.

## Build the shim

    make clean
    make -j$(nproc) all
    make hosttest loadertest xnutest androidimgtest
    make sizes

## Dualboot image

The production image combines the touchscreen selector, the Android slot-A chainloader, and the existing XNU payload loader:

    make clean
    make -j$(nproc) DUALBOOT=1 XNU_PAYLOAD=1 \
      XNU_KERNEL=/path/to/arm64/kernelcache all

At boot, a fresh touchscreen tap selects Android or iOS. Android goes through the read-only slot-A loader. iOS continues into the normal XNU payload/ADT handoff.

The Android loader reads the primary GPT from UFS LUN 2 and uses `boot_a`, `vendor_boot_a`, and, when present, `init_boot_a`. It supports raw arm64 Image, gzip, and Linux legacy-LZ4 kernel payloads. It builds the ramdisk as vendor ramdisk followed immediately by the generic ramdisk, adds the vendor_boot v4 bootconfig trailer, clones the vendor DTB, and rewrites `/chosen/bootargs` plus initrd bounds. The Linux handoff passes the physical DTB in x0, clears x1-x3, masks interrupts, and enters with the MMU disabled.

For vendor_boot v4 the loader currently takes the complete vendor ramdisk section instead of filtering the fragment table by board ID. Unsupported kernel compression, invalid metadata, or unsafe memory placement causes a controlled halt/reset path; it does not attempt a partial boot.

This is an implemented chainloader path, but it is still **experimental and not hardware-validated on the KL7**. The exact stock partition headers, kernel compression, addresses, and DTB layout must be checked against device logs before treating an image as bootable.

## Hardware test modes

Each mode is opt-in and runs instead of the normal XNU payload path:

    make OUT=out-menu MENU_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-ufs UFS_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-android ANDROID_BOOT_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-touch TOUCH_TEST=1 AUTO_REBOOT_TEST=0 all
    make OUT=out-battery BATTERY_TEST=1 AUTO_REBOOT_TEST=0 all

The menu test uses the FTS3519 touchscreen driver and framebuffer drawing. The Android inspection test reads the partition headers and logs boot/vendor_boot metadata without writing to storage.

Host regression tests:

    make hosttest loadertest xnutest androidimgtest androidkerneltest fdttest

## Device safety

- Keep Android on slot A as the fallback.
- Flash only boot_b; never write boot_a.
- Keep boot_b at or below 64 MiB.
- The UFS driver currently issues SCSI reads only. Do not add writes to Android partitions or misc while implementing boot selection.
