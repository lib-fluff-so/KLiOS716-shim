#!/usr/bin/env python3
"""Apply the SWEET bring-up changes to an iOS root made by tools/mkiosroot.sh.

  sudo sweet-root-tweaks.py MOUNTED_ROOT

MOUNTED_ROOT is the HFS+ image mounted read-write (Linux: run
`fsck.hfsplus -fy` on the image, then `mount -t hfsplus -o loop,force`).
Run as root (it writes root/mobile owned files). Idempotent. This is what
configures iOS 16 userland for bare-metal execution on MT6789:

  launchd.plist   _PanicOnCrash -> _PanicOnCrasX (a crashing daemon must not
                  panic the system); bring-up job disables are reversible via
                  tools/sweet_daemons.py; original definitions are preserved
  containers      /private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}
                  (on iOS they live on the data volume)
  migration       LastSystemVersion so DataMigration does not run for an hour
                  on every boot
  audio           VirtualAudio HAL plug-in disabled (mediaserverd crash)
  MediaToolbox    StartRecorderServer=false (camera filter prewarm calls a
                  NULL OpenGL function without a GPU -> mediaserverd crash loop)
  SpringBoard     BKNoWatchdogs (one slow CPU path, no scene-create watchdog
                  kills), no cloud-configuration UI
  Setup           skipped: SetupDone, SetupFinishedAllSteps, SetupVersion 11,
                  mini-buddy flags off
  activation      mobileactivationd data ark metadata merged; the actual state
                  comes from its Hactivation branch, selected by the /chosen
                  security-status properties in tools/mkadt.py
"""
import argparse, glob, hashlib, json, os, plistlib, shutil, struct, sys, tempfile
from pathlib import Path, PurePosixPath

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def load(path):
    with open(path, 'rb') as f:
        return plistlib.load(f)


def save(path, data, uid, gid, mode, fmt=plistlib.FMT_BINARY):
    with open(path, 'wb') as f:
        plistlib.dump(data, f, fmt=fmt)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def merge_pref(root, name, values):
    """Merge keys into /var/mobile/Library/Preferences/NAME.plist (mobile, 0600)."""
    path = os.path.join(root, 'private/var/mobile/Library/Preferences', name + '.plist')
    data = load(path) if os.path.exists(path) else {}
    data.update(values)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('pref', name, sorted(values))


def launchd(root):
    from sweet_daemons import preserve_and_disable
    disabled_labels = {
        'com.apple.CommCenter',
        'com.apple.locationd',
        'com.apple.wifid',
        'com.apple.bluetoothd',
        'com.apple.nfcd',
        'com.apple.awdd',
        'com.apple.analyticsd',
        'com.apple.passd',
        'com.apple.biometrickitd',
        'com.apple.ScreenTimeAgent',
    }
    preserve_and_disable(root, disabled_labels)


def containers(root):
    base = os.path.join(root, 'private/var/containers')
    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        d = os.path.join(base, sub)
        if not os.path.isdir(d):
            os.makedirs(d)
            os.chown(d, uid, uid)
            os.chmod(d, 0o755)
            print('mkdir', d)


def find_container(root, kind, identifier):
    for meta in glob.glob(os.path.join(root, 'private/var/containers', kind, '*',
                                       '.com.apple.mobile_container_manager.metadata.plist')):
        if load(meta).get('MCMMetadataIdentifier') == identifier:
            return os.path.dirname(meta)
    return None


def activation(root):
    c = find_container(root, 'Data/System', 'com.apple.mobileactivationd')
    if c is None:
        print('warning: no mobileactivationd system container in the image; skipped data ark')
        return
    d = os.path.join(c, 'Library/internal')
    os.makedirs(d, exist_ok=True)
    os.chown(d, MOBILE, MOBILE)
    os.chmod(d, 0o755)
    path = os.path.join(d, 'data_ark.plist')
    data = load(path) if os.path.exists(path) else {}
    defaults = {
        '-BuildVersion': BUILD, '-ProductVersion': VERSION,
        '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True,
    }
    for key, value in defaults.items():
        data.setdefault(key, value)
    save(path, data, MOBILE, MOBILE, 0o600)
    print('data ark defaults merged', d)


def audio(root):
    src = os.path.join(root, 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
    dst_dir = os.path.join(root, 'Library/Audio/Plug-Ins/HAL.disabled')
    if os.path.exists(src):
        if os.path.exists(os.path.join(dst_dir, 'VirtualAudio.plugin')):
            raise FileExistsError('Both active and disabled VirtualAudio.plugin exist; resolve them before applying audio tweaks')
        os.makedirs(dst_dir, exist_ok=True)
        shutil.move(src, dst_dir)
        print('VirtualAudio.plugin disabled')
    for conf in glob.glob(os.path.join(root, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')):
        st = os.stat(conf)
        data = load(conf)
        data['StartRecorderServer'] = False
        save(conf, data, st.st_uid, st.st_gid, st.st_mode & 0o7777, plistlib.FMT_XML)
        print('StartRecorderServer off', os.path.relpath(conf, root))


def malimetal_artifact(build_dir):
    """Reject stale or wrong-platform binaries before modifying the root image."""
    bundle = os.path.join(build_dir, 'MaliMetal.bundle')
    with open(os.path.join(build_dir, 'build.json')) as f:
        manifest = json.load(f)
    source_dir = Path(__file__).resolve().parent.parent / 'MaliMetal'
    required = {'build.py', 'Info.plist', 'MaliMetalDevice.m', 'MaliMetalCommandQueue.m',
                'MaliMetalTypes.h', 'Shader/MaliMetalShader.c', 'Vulkan/MaliVulkanBackend.c'}
    if not required.issubset(manifest['inputs']):
        raise ValueError('incomplete MaliMetal source manifest')
    digest = hashlib.sha256(json.dumps(manifest['inputs'], sort_keys=True).encode()).hexdigest()
    if digest != manifest['source_digest']:
        raise ValueError('MaliMetal source digest differs from manifest inputs')
    for name, expected in manifest['inputs'].items():
        relative = PurePosixPath(name)
        candidate = (source_dir / name).resolve()
        if relative.is_absolute() or '..' in relative.parts or '\\' in name or not candidate.is_relative_to(source_dir):
            raise ValueError('invalid source name in MaliMetal manifest')
        with open(candidate, 'rb') as f:
            if hashlib.sha256(f.read()).hexdigest() != expected:
                raise ValueError(f'MaliMetal source changed since build: {name}')
    with open(os.path.join(bundle, 'MaliMetal'), 'rb') as f:
        binary = f.read()
    if hashlib.sha256(binary).hexdigest() != manifest['binary_sha256']:
        raise ValueError('MaliMetal binary differs from build manifest')
    magic, cpu, _, kind, count = struct.unpack_from('<IIIII', binary)
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c, 6):
        raise ValueError('MaliMetal must be an arm64 Mach-O library')
    offset, platform = 32, None
    for _ in range(count):
        command, size = struct.unpack_from('<II', binary, offset)
        if size < 8 or offset + size > len(binary):
            raise ValueError('invalid MaliMetal load command')
        if command == 0x32:
            platform, = struct.unpack_from('<I', binary, offset + 8)
        offset += size
    if platform != 2:
        raise ValueError('MaliMetal was not built for iOS')
    expected_info = load(os.path.join(source_dir, 'Info.plist'))
    expected_info['MaliMetalSourceDigest'] = manifest['source_digest']
    if load(os.path.join(bundle, 'Info.plist')) != expected_info:
        raise ValueError('MaliMetal Info.plist differs from build manifest')
    components = manifest.get('components', {})
    icd = 'libpanvk_sweetmali.dylib'
    if os.path.isfile(os.path.join(bundle, icd)) != (icd in components):
        raise ValueError('MaliMetal ICD presence differs from build manifest')
    for name, expected in components.items():
        relative = PurePosixPath(name)
        candidate = (Path(bundle) / name).resolve()
        safe = not relative.is_absolute() and '..' not in relative.parts and '\\' not in name
        known = name in ('libpanvk_sweetmali.dylib', 'panvk-sweetmali.json') or name.startswith('licenses/')
        if not safe or not known or not candidate.is_relative_to(Path(bundle).resolve()):
            raise ValueError('unknown MaliMetal bundle component')
        with open(candidate, 'rb') as f:
            if hashlib.sha256(f.read()).hexdigest() != expected:
                raise ValueError(f'MaliMetal component differs from build manifest: {name}')
    files = dict(components)
    files['MaliMetal'] = manifest['binary_sha256']
    files['Info.plist'] = hashlib.sha256(Path(bundle, 'Info.plist').read_bytes()).hexdigest()
    return {'bundle': bundle, 'files': files}


def install_malimetal(root, artifact):
    bundle = artifact['bundle']
    parent = os.path.join(root, 'System/Library/Extensions')
    destination = os.path.join(parent, 'MaliMetal.bundle')
    stage = tempfile.mkdtemp(prefix='.malimetal-', dir=parent)
    fresh, previous = os.path.join(stage, 'new'), os.path.join(stage, 'previous')
    os.mkdir(fresh, 0o755)
    os.chmod(fresh, 0o755)
    os.chown(fresh, 0, 0)
    try:
        for name, expected in artifact['files'].items():
            mode = 0o755 if name in ('MaliMetal', 'libpanvk_sweetmali.dylib') else 0o644
            path = os.path.join(fresh, name)
            os.makedirs(os.path.dirname(path), mode=0o755, exist_ok=True)
            shutil.copyfile(os.path.join(bundle, name), path)
            if hashlib.sha256(Path(path).read_bytes()).hexdigest() != expected:
                raise ValueError(f'MaliMetal file changed during installation: {name}')
            os.chown(path, 0, 0)
            os.chmod(path, mode)
        if os.path.exists(destination):
            os.rename(destination, previous)
        try:
            os.rename(fresh, destination)
        except BaseException:
            if os.path.exists(previous):
                os.rename(previous, destination)
            raise
    except BaseException:
        if os.path.exists(previous):
            print('MaliMetal previous bundle preserved at', previous, file=sys.stderr)
        else:
            shutil.rmtree(stage)
        raise
    else:
        shutil.rmtree(stage)
    print('MaliMetal.bundle installed from verified build:', bundle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    parser.add_argument('--malimetal-build', help='explicit MaliMetal build output directory')
    parser.add_argument('--malimetal-only', action='store_true',
                        help='install the verified driver without applying other root tweaks')
    args = parser.parse_args()
    if not os.path.isdir(os.path.join(args.root, 'System/Library/xpc')):
        sys.exit(__doc__)
    root = args.root
    bundle = malimetal_artifact(args.malimetal_build) if args.malimetal_build else None
    if args.malimetal_only:
        if bundle is None:
            parser.error('--malimetal-only requires --malimetal-build')
        install_malimetal(root, bundle)
        return
    launchd(root)
    containers(root)
    activation(root)
    audio(root)
    merge_pref(root, 'com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref(root, 'com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False,
        'SBShowInternalAssertionBanners': False,
        'SBDontShowInternalAssertionBanners': True,
        'SBShowInternalBanner': False,
        'SBShowNonFatalAssertionBanners': False,
        'SBEnableAssertionAlerts': False,
        'SBShowNonFatalAssertionAlerts': False})
    dm_dir = os.path.join(root, 'private/var/mobile/Library/DataMigration')
    os.makedirs(dm_dir, exist_ok=True)
    with open(os.path.join(dm_dir, 'BuildVersion'), 'w') as f:
        f.write(BUILD + '\n')
    with open(os.path.join(dm_dir, 'DataMigrationCompleted'), 'w') as f:
        f.write('')
    os.chown(dm_dir, MOBILE, MOBILE)
    os.chmod(dm_dir, 0o755)
    if bundle is not None:
        install_malimetal(root, bundle)
    else:
        print('MaliMetal.bundle unchanged; use --malimetal-build to install a verified build')
    merge_pref(root, 'com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref(root, 'com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref(root, 'com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})


if __name__ == '__main__':
    main()
