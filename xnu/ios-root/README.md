# KL7 iOS root preparation

These scripts prepare an **offline** iOS 16.7.16 root for the KL7 port. They
do not flash or modify a live phone, and they do not replace the kernel work.
Run them only on a mounted copy of the root image, as root on the macOS VM
(or on a Linux host for `mkiosroot.sh`).

## Layout

```
ios-root/
├── post-root.sh              VM-side fixups for /Users/dev/ios/IOSRoot
├── patch-v17/                sparse-overlay manifest; chunk blobs not preserved
└── v18-tweaks/
    ├── make_sparse.py        raw image -> Android sparse image
    ├── sweet-root-tweaks.py  offline iOS 16.7.16 bring-up edits
    ├── sweet_daemons.py      inspect/restore launchd jobs
    ├── v18-container.py      seed mobileactivationd container
    └── v19-migrate.py        force a real DataMigration
```

The main root-image builder is `../tools/mkiosroot.sh`. It takes the iOS OS,
SystemOS and AppOS images, builds a journaled HFS+ volume, preserves owners,
modes and hard links, creates the shared-cache and cryptex links, creates the
logd stores, and installs the no-passcode system keybag. It requires
`apfs-fuse`, `mkfs.hfsplus`, `rsync`, and root privileges.

## `post-root.sh`

`post-root.sh` is the macOS-VM side repeat of the root fixups. It expects an
already copied root at:

```text
/Users/dev/ios/IOSRoot
```

Run it once as root after the root has been copied:

```sh
sudo /Users/dev/ios/post-root.sh
```

It:

- hard-links the SystemOS shared cache into
  `System/Library/Caches/com.apple.dyld`;
- fills missing cryptex files (OS and App) into the root without copying
  existing files;
- creates the logd stores with `_logd` (272:272) ownership and `.migrated`
  markers;
- creates `private/var/keybags/systembag.kb`;
- runs `sweet-root-tweaks.py` on the root.

It is idempotent where the underlying operations are idempotent, but do not
run it against a mounted live data volume.

## `v18-tweaks`

`sweet-root-tweaks.py MOUNTED_ROOT` is the main script. It targets iOS
`16.7.16` / build `20H392` and expects a mounted, writable HFS+ root. It
changes launchd's panic-on-crash behavior, creates containers, sets migration
and setup markers, disables known-crashing audio/MediaToolbox paths, relaxes
SpringBoard watchdogs, and prepares activation metadata. It preserves the
original launchd job definitions in a cache so they can be restored.

`sweet_daemons.py` inspects or restores launchd jobs in an offline root. It
only restores the launchd definitions; a restored service still needs its
hardware and kernel dependencies. It is not a way to make Wi-Fi, Bluetooth,
NFC, baseband or biometrics work.

`v18-container.py MOUNTED_ROOT` seeds the `mobileactivationd` system data
container with a stable UUID and activation metadata. `v19-migrate.py
MOUNTED_ROOT` removes migration completion state and creates the
`private/var/datamigrator/migrate` sentinel to force a real DataMigration on
the next boot.

`make_sparse.py RAW OUT` converts a raw disk image to Android sparse format,
skipping all-zero 4 MiB chunks. It does not resize or align partitions.

## `patch-v17`

`patch-v17/manifest.tsv` lists nine 4 MiB sparse chunks for an iOS 17 root
experiment. The chunk files themselves were **not preserved**; only the
manifest is in this repository. Read `patch-v17/README.md` before using it.
Do not treat the manifest as a complete patch set, and do not apply it to a
different root without verifying the source image and chunk hashes.

## Safety

- Work on a copy. Keep the original OS/cryptex images and a known-good root.
- Do not run these scripts against a live device root or a mounted data
  volume that iOS is using.
- Root operations change ownership and modes; `rsync -aH --numeric-ids` and
  root are required.
- The scripts contain bring-up workarounds, not a claim of full iOS
  userspace support. Read the comments and `../../xnu/NOTES.md` before
  changing defaults.
- The KL7 root work is separate from the shim history. Never flash `boot_a`;
  see `../../REPOSITORY.md`.

## Related files

- `../../xnu/tools/mkiosroot.sh` - build the HFS+ root volume
- `../../xnu/tools/mkinit.py` - static `sweet-init` wrapper
- `../../xnu/NOTES.md` - userland/display/GPU history
- `../../SHIM-HISTORY.md` - shim and on-device chronology
