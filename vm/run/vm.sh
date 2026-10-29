#!/usr/bin/env bash
# macOS Sonoma 14.6 VM launcher (headless, QEMU/KVM, AMD Ryzen)
# Usage:
#   ./vm.sh                          # boot installed system
#   BOOT_MEDIA=/path/BaseSystem.img ISO=/path/InstallAssistant.iso ./vm.sh
set -euo pipefail

VM_DIR="/home/fluff_/macOS-VM"
RAM="${RAM:-8192}"
CPUS="${CPUS:-8}"          # -smp 8,cores=4,threads=2
VNC_DISPLAY="${VNC_DISPLAY:-1}"  # 127.0.0.1:5901
BOOT_MEDIA="${BOOT_MEDIA:-}"
ISO="${ISO:-}"
# Validated on this host: Haswell-noTSX never reached SSH; Skylake-Client does.
CPU_MODEL="${CPU_MODEL:-Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on}"
# Attach the installer media by default when present. The installed system
# still boots from MacHDD, but this is the exact layout that has been verified.
if [ -z "${INSTMEDIA+x}" ] && [ -f "$VM_DIR/img/instmedia.qcow2" ]; then
  INSTMEDIA="$VM_DIR/img/instmedia.qcow2"
fi
INSTMEDIA="${INSTMEDIA:-}"

if pgrep -x macOS-Sonoma >/dev/null 2>&1; then
  echo "VM already running" >&2
  exit 1
fi

rm -f "$VM_DIR/run/qmp.sock" "$VM_DIR/run/monitor.sock"

args=(
  -name macOS-Sonoma,process=macOS-Sonoma
  -enable-kvm -m "$RAM"
  -cpu "$CPU_MODEL"
  -machine q35,accel=kvm
  -smp "$CPUS",sockets=1,cores=4,threads=2
  -device qemu-xhci,id=xhci
  -device usb-kbd,bus=xhci.0
  -device usb-tablet,bus=xhci.0
  -device isa-applesmc,osk="ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
  -drive if=pflash,format=raw,readonly=on,file="$VM_DIR/OVMF_CODE_4M.fd"
  -drive if=pflash,format=raw,file="$VM_DIR/OVMF_VARS-1920x1080.fd"
  -smbios type=2
  -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off
  -device ich9-ahci,id=sata
  -drive id=OpenCoreBoot,if=none,snapshot=on,format=qcow2,file="$VM_DIR/OpenCore/OpenCore.qcow2"
  -device ide-hd,bus=sata.2,drive=OpenCoreBoot
  -drive id=MacHDD,if=none,file="$VM_DIR/img/mac_hdd.img",format=qcow2
  -device ide-hd,bus=sata.4,drive=MacHDD
  -netdev user,id=net0,hostfwd=tcp::2222-:22
  -device vmxnet3,netdev=net0,id=net0,mac=52:54:00:c9:18:27
  -device vmware-svga
  -display none
  -vnc "127.0.0.1:${VNC_DISPLAY}"
  -qmp "unix:$VM_DIR/run/qmp.sock,server=on,wait=off"
  -monitor "unix:$VM_DIR/run/monitor.sock,server=on,wait=off"
  -rtc base=utc,clock=host
)

if [ -n "$BOOT_MEDIA" ]; then
  args+=(
    -drive id=InstallMedia,if=none,file="$BOOT_MEDIA",format=raw
    -device ide-hd,bus=sata.3,drive=InstallMedia
  )
fi

if [ -n "$INSTMEDIA" ]; then
  args+=(
    -drive id=InstMedia,if=none,file="$INSTMEDIA",format=qcow2
    -device ide-hd,bus=sata.1,drive=InstMedia
  )
fi

if [ -n "$ISO" ]; then
  args+=(
    -drive id=MacDVD,if=none,file="$ISO",format=raw
    -device ide-hd,bus=sata.5,drive=MacDVD
  )
fi

exec qemu-system-x86_64 "${args[@]}"
