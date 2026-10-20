#!/usr/bin/env bash
# Run on the Linux host. Uses ADB + phone root to back up slot-B partitions
# DIRECTLY to /sdcard/bak_b on the phone; large partition images are not pulled
# onto the computer or uploaded anywhere.
set -Eeuo pipefail

ADB_BIN="${ADB:-adb}"
command -v "$ADB_BIN" >/dev/null 2>&1 || {
    printf 'ERROR: adb not found. Install Android platform-tools or set ADB=/path/to/adb.\n' >&2
    exit 1
}
command -v base64 >/dev/null 2>&1 || {
    printf 'ERROR: base64 utility not found on host.\n' >&2
    exit 1
}

printf 'Waiting for Android over ADB...\n'
"$ADB_BIN" wait-for-device
ROOT_ID="$("$ADB_BIN" shell su -c id 2>/dev/null | tr -d '\r' || true)"
case "$ROOT_ID" in
    *"uid=0"*) ;;
    *) printf 'ERROR: root unavailable through adb (su -c id returned: %s)\n' "${ROOT_ID:-no output}" >&2; exit 1 ;;
esac

REMOTE_SCRIPT=$(cat <<'REMOTE'
#!/system/bin/sh
D=/sdcard/bak_b
mkdir -p "$D" || { echo "ERROR: cannot create $D"; exit 1; }

if [ "$(id -u)" != 0 ]; then
    echo "ERROR: remote command is not running as root"
    exit 1
fi

getprop ro.boot.slot_suffix > "$D/slot.txt"
: > "$D/manifest.tsv"
echo "Backup directory: $D"
echo "Current slot: $(cat "$D/slot.txt")"
echo "Free space before backup:"
df -h /sdcard 2>&1

for p in boot_b vendor_boot_b init_boot_b dtbo_b vbmeta_b; do
    src=
    for root in /dev/block/by-name /dev/block/platform/soc/11270000.ufshci/by-name /dev/block/bootdevice/by-name; do
        if [ -e "$root/$p" ]; then
            src="$root/$p"
            break
        fi
    done

    if [ -z "$src" ]; then
        echo "SKIP $p: partition not found"
        printf '%s\tSKIP\t%s\n' "$p" "not found" >> "$D/manifest.tsv"
        continue
    fi

    echo "Backing up $p from $src ..."
    expected=$(blockdev --getsize64 "$src" 2>/dev/null | tr -d '\r' | grep '^[0-9][0-9]*$' | tail -n 1)
    tmp="$D/$p.img.partial"
    out="$D/$p.img"
    rm -f "$tmp"

    if ! dd if="$src" of="$tmp" bs=1048576 2>"$D/$p.dd.log"; then
        echo "FAIL $p: dd failed; see $D/$p.dd.log"
        rm -f "$tmp"
        printf '%s\tFAIL\t%s\n' "$p" "dd failed" >> "$D/manifest.tsv"
        continue
    fi

    actual=$(wc -c < "$tmp" | tr -d '[:space:]')
    if [ -z "$actual" ] || [ "$actual" -eq 0 ]; then
        echo "FAIL $p: empty image"
        rm -f "$tmp"
        printf '%s\tFAIL\t%s\n' "$p" "empty image" >> "$D/manifest.tsv"
        continue
    fi
    if [ -n "$expected" ] && [ "$actual" != "$expected" ]; then
        echo "FAIL $p: size mismatch; expected $expected, got $actual; kept as $D/$p.img.incomplete"
        mv "$tmp" "$D/$p.img.incomplete"
        printf '%s\tFAIL\tsize mismatch expected=%s actual=%s\n' "$p" "$expected" "$actual" >> "$D/manifest.tsv"
        continue
    fi

    mv "$tmp" "$out"
    sum=$(sha256sum "$out" 2>/dev/null | awk '{print $1}')
    echo "OK $p: $actual bytes SHA256=$sum"
    printf '%s\tOK\t%s\t%s\n' "$p" "$actual" "$sum" >> "$D/manifest.tsv"
done

echo
echo "=== BACKUPS ==="
ls -lh "$D"
echo "=== MANIFEST ==="
cat "$D/manifest.tsv"
echo "Done. Backups remain on the phone at $D."
REMOTE
)

ENCODED="$(printf '%s' "$REMOTE_SCRIPT" | base64 | tr -d '\r\n')"
printf 'Running backup on the phone; files will stay in /sdcard/bak_b...\n\n'
"$ADB_BIN" shell su -c "echo '$ENCODED' | base64 -d | sh"
