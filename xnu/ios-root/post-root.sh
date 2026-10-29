#!/bin/bash
# Post-copy SWEET root preparation, run on the macOS VM as root.
set -euo pipefail

ROOT=/Users/dev/ios/IOSRoot
DSC="$ROOT/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"

echo "=== shared-cache hard links ==="
mkdir -p "$ROOT/System/Library/Caches/com.apple.dyld"
find "$DSC" -maxdepth 1 -type f -print0 |
while IFS= read -r -d '' f; do
    ln -f "$f" "$ROOT/System/Library/Caches/com.apple.dyld/${f##*/}"
    echo "cache ${f##*/}"
done

echo "=== cryptex files missing from the OS root ==="
for c in OS App; do
    base="$ROOT/private/preboot/Cryptexes/$c"
    (cd "$base" && find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*' -print0) |
    while IFS= read -r -d '' f; do
        f=${f#./}
        if [ ! -e "$ROOT/$f" ] && [ ! -L "$ROOT/$f" ]; then
            mkdir -p "$ROOT/$(dirname "$f")"
            src="$base/$f"
            if [ -L "$src" ]; then
                cp -P "$src" "$ROOT/$f"
            else
                ln "$src" "$ROOT/$f"
            fi
            echo "linked $c:/$f"
        fi
    done
done

echo "=== logd stores ==="
db="$ROOT/private/var/db"
mkdir -p "$db/diagnostics" "$db/uuidtext" "$db/diagnostics/timesync"
chown 272:272 "$db/diagnostics" "$db/uuidtext" "$db/diagnostics/timesync"
chmod 0751 "$db/diagnostics"
chmod 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
    touch "$db/$d/.migrated"
    chown 272:272 "$db/$d/.migrated"
    chmod 0644 "$db/$d/.migrated"
done

echo "=== system keybag ==="
mkdir -p "$ROOT/private/var/keybags"
chmod 0700 "$ROOT/private/var/keybags"
chown 0:0 "$ROOT/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' \
    > "$ROOT/private/var/keybags/systembag.kb"
chmod 0600 "$ROOT/private/var/keybags/systembag.kb"
chown 0:0 "$ROOT/private/var/keybags/systembag.kb"

echo "=== sweet-root-tweaks.py ==="
python3 /Users/dev/ios/sweet-root-tweaks.py "$ROOT"

echo "=== done ==="
df -h "$ROOT"
