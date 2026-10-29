#!/usr/bin/env python3
"""Force a real DataMigration on the offline KL7 iOS root."""
import glob
import os
import plistlib
import shutil
import sys

root = sys.argv[1]
prefs = os.path.join(root, "private/var/mobile/Library/Preferences")
byhost = os.path.join(prefs, "ByHost")
dm_dir = os.path.join(root, "private/var/mobile/Library/DataMigration")
sentinel_dir = os.path.join(root, "private/var/datamigrator")
sentinel = os.path.join(sentinel_dir, "migrate")
MOBILE = 501

# Remove every preference that can make DataMigration decide "not necessary".
for path in [os.path.join(prefs, "com.apple.migration.plist"),
             os.path.join(prefs, "com.apple.DataMigration.plist")]:
    try:
        os.remove(path)
        print("removed", path)
    except FileNotFoundError:
        pass
for path in glob.glob(os.path.join(byhost, "com.apple.migration.*")):
    os.remove(path)
    print("removed", path)

# Remove completion markers from the previous synthetic migration.
if os.path.isdir(dm_dir):
    shutil.rmtree(dm_dir)
    print("removed", dm_dir)

# Let CFPreferences create its ByHost state; the directory itself must exist.
os.makedirs(byhost, exist_ok=True)
os.chown(byhost, MOBILE, MOBILE)
os.chmod(byhost, 0o755)

# This is the explicit "migration necessary" sentinel used by datamigrator.
os.makedirs(sentinel_dir, exist_ok=True)
os.chown(sentinel_dir, MOBILE, MOBILE)
os.chmod(sentinel_dir, 0o755)
with open(sentinel, "wb"):
    pass
os.chown(sentinel, MOBILE, MOBILE)
os.chmod(sentinel, 0o600)
print("created", sentinel)

# mobileactivationd's data ark must not claim that migration already completed.
ark = os.path.join(
    root,
    "private/var/containers/Data/System/"
    "D90C94C4-F94E-4FCF-8A28-A93BE3846D8E/Library/internal/data_ark.plist",
)
if os.path.exists(ark):
    with open(ark, "rb") as f:
        data = plistlib.load(f)
    for key in ("-DataMigrationCompleted", "-PreSydroDataMigrationCompleted"):
        if key in data:
            del data[key]
            print("removed", key, "from", ark)
    with open(ark, "wb") as f:
        plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
    os.chown(ark, MOBILE, MOBILE)
    os.chmod(ark, 0o600)

# Keep the mobile home and its preferences writable by the migration service.
for path in (os.path.join(root, "private/var/mobile"),
             os.path.join(root, "private/var/mobile/Library"),
             prefs):
    os.chown(path, MOBILE, MOBILE)
    os.chmod(path, 0o711 if path.endswith("/mobile") or path.endswith("/Library") else 0o755)

print("migration forced")
