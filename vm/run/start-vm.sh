#!/usr/bin/env bash
# Canonical launch command for the KL7 XNU macOS VM.
# Validated: SSH becomes reachable on 127.0.0.1:2222 within ~15 s.
set -euo pipefail

VM_DIR="/home/fluff_/macOS-VM"
LOG="${LOG:-/tmp/vmrun.log}"

if pgrep -x macOS-Sonoma >/dev/null 2>&1; then
  echo "VM already running" >&2
  exit 1
fi

cd "$VM_DIR"
setsid nohup env \
  INSTMEDIA="$VM_DIR/img/instmedia.qcow2" \
  CPU_MODEL='Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on' \
  "$VM_DIR/run/vm.sh" >"$LOG" 2>&1 &

echo "started macOS VM, log: $LOG"
