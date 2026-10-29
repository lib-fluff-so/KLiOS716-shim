# Local macOS build VM

The KL7 XNU kernel is built in a local macOS Sonoma 14.6 VM. There are no
GitHub Actions macOS runners; this VM is the only supported XNU build host.
The scripts here are the launcher, OpenCore configuration, installer
helpers, and QMP control tools used to create and operate it.

## What is in this directory

| Path | Purpose |
| --- | --- |
| `run/vm.sh` | QEMU launcher for the installed VM; headless with VNC and QMP |
| `run/start-vm.sh` | canonical wrapper with the known-good CPU model and installer media |
| `run/vmctl.py` | QMP helper: screenshot, key, type, click, HMP |
| `run/ocr.py` | screenshot + Tesseract OCR for blind GUI control |
| `OpenCore/config.plist` | OpenCore configuration |
| `OpenCore/*.sh` | OpenCore image creation helpers (upstream-style) |
| `OVMF_CODE_4M.fd` | OVMF firmware code |
| `OVMF_VARS-1920x1080.fd` | OVMF variable store |
| `media/install.sh` | stage 1: erase the target disk and run `startosinstall` |
| `preseed.sh` | stage 2: create `dev`/`1602`, skip Setup Assistant, enable SSH |
| `pkg/PackageInfo` | metadata for the macOS Sonoma InstallAssistant package |
| `out/SHA256SUMS` | **older** kernel copy; not the latest VM build |

The large/generated files are intentionally not in git:

- `img/mac_hdd.img` (installed macOS disk)
- `img/instmedia.qcow2` (installer media)
- `OpenCore/OpenCore.qcow2` (generated OpenCore boot image)
- installer ISO/package images

The scripts assume the VM directory is `/home/fluff_/macOS-VM`. If the
repository is checked out elsewhere, adjust `VM_DIR` in `run/vm.sh` and the
paths in the helpers.

## Start the VM

The validated launch is:

```sh
/home/fluff_/macOS-VM/run/start-vm.sh
```

Equivalent explicit command:

```sh
cd /home/fluff_/macOS-VM
setsid nohup env \
  INSTMEDIA="$PWD/img/instmedia.qcow2" \
  CPU_MODEL='Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on' \
  ./run/vm.sh >/tmp/vmrun.log 2>&1 &
```

The VM exposes SSH on `127.0.0.1:2222`:

```sh
sshpass -p 1602 ssh -p 2222 dev@127.0.0.1
```

VNC is bound to `127.0.0.1:5901`; QMP is at `run/qmp.sock`, and the HMP
monitor is at `run/monitor.sock`.

The VM needs `INSTMEDIA` attached to reach SSH reliably on this host. A
Haswell-noTSX CPU model did not work; the Skylake-Client model above did.
Do not "simplify" the CPU model without testing SSH and a full XNU build.

## Install flow

This is a record of how the current VM was made, not a generic macOS-on-QEMU
guide.

1. Create/attach the target disk (`img/mac_hdd.img`), the installer media
   (`img/instmedia.qcow2`), and the generated OpenCore image.
2. Boot macOS Recovery and mount the installer volume at
   `/Volumes/InstallAssistant`.
3. Run `media/install.sh` from the Recovery Terminal. It finds the >50 GB
   target, erases it as APFS `macOS`, copies the InstallAssistant app, and
   runs `startosinstall --agreetolicense`.
4. After the install, run `preseed.sh` from Recovery. It finds the data
   volume, creates the admin user `dev` with password `1602`, marks Setup
   Assistant complete, and installs a `local.sshd` LaunchDaemon.
5. Boot the installed system and verify SSH on port 2222.

The OpenCore image can be rebuilt from `OpenCore/config.plist` with the
helper scripts in `OpenCore/`; see `OpenCore/README.md` for the upstream
notes. The current `OpenCore.qcow2` is not stored in git.

## QMP helpers

With the VM running:

```sh
python3 run/vmctl.py status
python3 run/vmctl.py shot /tmp/opencode/vm.png
python3 run/vmctl.py key meta_l+spc
python3 run/vmctl.py type "hello"
python3 run/vmctl.py click 640 480
python3 run/vmctl.py hmp "info status"
```

`ocr.py` takes a screenshot and prints recognized words with coordinates:

```sh
python3 run/ocr.py /tmp/opencode/vm.png
```

These are useful when a GUI installer or Recovery prompt must be driven
without a physical display.

## XNU build

The VM's build tree is `~/kbuild/ios-sweet`; the wrapper is
`~/kbuild/build-xnu.sh`; the output is
`~/kbuild/out/kernel.development.sweet`. The verified configuration is
`MACOS_VERSION=13.5`, `KERNEL_CONFIG=DEVELOPMENT`, `MACHINE_CONFIG=SWEET`,
with blacktop `dxb` pinned at `663906528d1cd5ea8fde7de6f16de73db9511549`.

See `../xnu/README.md` for the exact sync/build commands and the patch-set
rules. The last verified kernel is `1bfe9cd7...`; `vm/out/SHA256SUMS`
contains an older `8f3cb86d...` copy and must not be used as the latest
result.

## Safety and hygiene

- This VM is a build environment, not a place to keep the only copy of a
  patch set. Keep the repo's `xnu/patches/` and the VM tree in sync
  deliberately.
- Do not enable GitHub Actions as a fallback; the macOS runners are gone.
- The VM disk and installer media are large; keep them outside git.
- Shut the VM down cleanly before replacing `img/mac_hdd.img` or the
  OpenCore image.
- The phone-side safety rules are in `../REPOSITORY.md`: flash only
  `boot_b`, never `boot_a`, and keep `boot_b` at or below 64 MiB.

## Related files

- `../xnu/README.md` - XNU build and patch set
- `../xnu/HISTORY.md` - kernel chronology
- `../REPOSITORY.md` - repository map and phone safety rules
- `../SHIM-HISTORY.md` - shim/on-device history
