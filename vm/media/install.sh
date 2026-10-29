#!/bin/sh
# Stage 1 (run in macOS Recovery Terminal):
#   sh /Volumes/InstallAssistant/install.sh
# Erases the 100G target disk, installs macOS Sonoma 14.6 offline.
exec >/tmp/stage1.log 2>&1
set -x

ISO=/Volumes/InstallAssistant
PKG="$ISO/InstallAssistant.pkg"

[ -d "$ISO" ] || { echo "ERROR: ISO not mounted at $ISO"; exit 1; }
[ -f "$PKG" ] || { echo "ERROR: pkg missing"; exit 1; }

# --- find the big target disk (>50GB, not the ~3GB BaseSystem media) ---
TARGET=""
for d in /dev/disk0 /dev/disk1 /dev/disk2 /dev/disk3 /dev/disk4 /dev/disk5; do
  [ -e "$d" ] || continue
  bytes=$(diskutil info -plist "$d" 2>/dev/null | plutil -extract TotalSize raw -o - - 2>/dev/null)
  if [ -z "$bytes" ]; then
    bytes=$(diskutil info "$d" 2>/dev/null | sed -n 's/.*Disk Size:.*(\([0-9][0-9]*\) Bytes).*/\1/p')
  fi
  [ -n "$bytes" ] || continue
  echo "disk $d size=$bytes"
  if [ "$bytes" -ge 50000000000 ] 2>/dev/null; then TARGET="$d"; fi
done
if [ -z "$TARGET" ]; then echo "ERROR: target disk not found"; diskutil list; exit 1; fi
echo "TARGET=$TARGET"

diskutil eraseDisk APFS macOS "$TARGET"
i=0
while [ ! -d /Volumes/macOS ] && [ $i -lt 90 ]; do sleep 1; i=$((i+1)); done
if [ ! -d /Volumes/macOS ]; then echo "ERROR: /Volumes/macOS did not appear"; diskutil list; exit 1; fi

# --- place the installer app on the target volume (symlinks preserved via tar) ---
mkdir -p /Volumes/macOS/Applications
tar -xzf "$ISO/app.tar.gz" -C /Volumes/macOS/Applications
APP="/Volumes/macOS/Applications/Install macOS Sonoma.app"
chown -R root:wheel "$APP"
chmod -R u+rwX,go+rX "$APP"
mkdir -p "$APP/Contents/SharedSupport"
cp "$PKG" "$APP/Contents/SharedSupport/SharedSupport.dmg"
chown root:wheel "$APP/Contents/SharedSupport/SharedSupport.dmg"
chmod 0644 "$APP/Contents/SharedSupport/SharedSupport.dmg"

# --- headless install ---
"$APP/Contents/Resources/startosinstall" --volume /Volumes/macOS --agreetolicense
echo "STARTOSINSTALL_EXIT=$?"
