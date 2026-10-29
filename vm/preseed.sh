#!/bin/sh
# Stage 2 (run in macOS Recovery Terminal after the install finished, before first boot):
#   sh /Volumes/InstallAssistant/preseed.sh
# Creates admin user dev/1602, marks setup assistant done, starts sshd via LaunchDaemon.
exec >/tmp/preseed.log 2>&1
set -x

SYS=""
DATA=""
for v in /Volumes/*; do
  case "$v" in /Volumes/InstallAssistant*) continue;; esac
  if [ -f "$v/System/Library/CoreServices/SystemVersion.plist" ]; then SYS="$v"; fi
  if [ -d "$v/private/var/db/dslocal" ] && [ ! -f "$v/System/Library/CoreServices/SystemVersion.plist" ]; then DATA="$v"; fi
done
echo "SYS=$SYS DATA=$DATA"
if [ -z "$DATA" ]; then echo "ERROR: data volume not found"; ls -la /Volumes; exit 1; fi

# make sure the data volume is writable
mount -uw "$DATA" 2>/dev/null || true
mkdir -p "$DATA/private/var/db"
touch "$DATA/private/var/db/.AppleSetupDone"

DS="$DATA/private/var/db/dslocal/nodes/Default"
mkdir -p "$DS/users" "$DS/groups"

# delete stale entry if any
dscl -f "$DS" localonly -delete /Local/Default/Users/dev 2>/dev/null

dscl -f "$DS" localonly -create /Local/Default/Users/dev
dscl -f "$DS" localonly -create /Local/Default/Users/dev RealName "dev"
dscl -f "$DS" localonly -create /Local/Default/Users/dev UniqueID 501
dscl -f "$DS" localonly -create /Local/Default/Users/dev PrimaryGroupID 20
dscl -f "$DS" localonly -create /Local/Default/Users/dev UserShell /bin/zsh
dscl -f "$DS" localonly -create /Local/Default/Users/dev NFSHomeDirectory /Users/dev
dscl -f "$DS" localonly -create /Local/Default/Users/dev IsHidden 0
dscl -f "$DS" localonly -passwd /Local/Default/Users/dev 1602
dscl -f "$DS" localonly -append /Local/Default/Groups/admin GroupMembership dev
dscl -f "$DS" localonly -append /Local/Default/Groups/admin GroupMembers dev

mkdir -p "$DATA/Users/dev"
cp -R "$SYS/System/Library/User Template/English.lproj/." "$DATA/Users/dev/" 2>/dev/null || true
chown -R 501:20 "$DATA/Users/dev"

# sshd LaunchDaemon on the data volume (/Library is a firmlink there)
mkdir -p "$DATA/Library/LaunchDaemons"
cat > "$DATA/Library/LaunchDaemons/local.sshd.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>local.sshd</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/sh</string>
        <string>-c</string>
        <string>/usr/bin/ssh-keygen -A; exec /usr/sbin/sshd -D -e</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/var/log/local.sshd.log</string>
    <key>StandardErrorPath</key>
    <string>/var/log/local.sshd.log</string>
</dict>
</plist>
PLIST
chown root:wheel "$DATA/Library/LaunchDaemons/local.sshd.plist"
chmod 0644 "$DATA/Library/LaunchDaemons/local.sshd.plist"

sync
echo "PRESEED_DONE"
