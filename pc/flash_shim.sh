#!/bin/sh
# Прошивка shim в boot_b и переключение на слот B. Телефон в fastboot (LK), загрузчик разблокирован.
# Использование: ./flash_shim.sh [образ]    VBMETA=1 ./flash_shim.sh   (если LK не пускает образ из-за AVB)
set -e
IMG=${1:-out/shim_b.img}
fastboot getvar current-slot 2>&1 | head -2
fastboot flash boot_b "$IMG"
if [ "$VBMETA" = "1" ]; then
  fastboot --disable-verity --disable-verification flash vbmeta_b vbmeta_a.img
fi
fastboot set_active b
fastboot reboot
