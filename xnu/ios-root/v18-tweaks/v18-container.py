#!/usr/bin/env python3
"""Seed the mobileactivationd system data container in an offline iOS root."""
import os
import plistlib
import sys

BUILD = "20H392"
VERSION = "16.7.16"
MOBILE = 501
UUID = "D90C94C4-F94E-4FCF-8A28-A93BE3846D8E"

root = sys.argv[1]
base = os.path.join(root, "private/var/containers/Data/System")
container = os.path.join(base, UUID)
internal = os.path.join(container, "Library/internal")
os.makedirs(internal, exist_ok=True)

metadata = {
    "MCMMetadataVersion": "2",
    "MCMMetadataIdentifier": "com.apple.mobileactivationd",
    "MCMMetadataUUID": UUID,
    "MCMMetadataContentClass": 2,
    "MCMMetadataInfo": {},
    "MCMMetadataUserIdentity": {
        "posixUID": MOBILE,
        "posixGID": MOBILE,
        "type": 0,
        "version": "2",
    },
}
meta_path = os.path.join(container, ".com.apple.mobile_container_manager.metadata.plist")
with open(meta_path, "wb") as f:
    plistlib.dump(metadata, f, fmt=plistlib.FMT_BINARY)
os.chown(meta_path, 0, 0)
os.chmod(meta_path, 0o644)

data_ark = {
    "-ActivationState": "Activated",
    "-BrickState": False,
    "-BuildVersion": BUILD,
    "-ProductVersion": VERSION,
    "-DataMigrationCompleted": True,
    "-PreSydroDataMigrationCompleted": True,
}
ark_path = os.path.join(internal, "data_ark.plist")
with open(ark_path, "wb") as f:
    plistlib.dump(data_ark, f, fmt=plistlib.FMT_BINARY)
os.chown(ark_path, MOBILE, MOBILE)
os.chmod(ark_path, 0o600)

os.chown(container, MOBILE, MOBILE)
os.chmod(container, 0o755)
os.chown(base, 0, 0)
os.chmod(base, 0o755)

# The XPC service runs as mobile and creates/removes the migrate sentinel here.
dm = os.path.join(root, "private/var/datamigrator")
os.makedirs(dm, exist_ok=True)
os.chown(dm, MOBILE, MOBILE)
os.chmod(dm, 0o755)

print("container", container)
print("metadata", meta_path)
print("data_ark", ark_path)
