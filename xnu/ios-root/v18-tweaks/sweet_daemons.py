#!/usr/bin/env python3
"""Inspect and restore launchd jobs in an offline SWEET iOS root.

Restoring a job only restores its launchd definition. Its hardware and kernel
dependencies must be implemented before the corresponding service can work.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import plistlib
import struct
import tempfile


CACHE = 'System/Library/xpc/launchd.plist'
ARCHIVE = 'private/var/db/sweet-root-tweaks/launchd-original-jobs.plist'
DEPENDENCIES = {
    'com.apple.CommCenter': 'baseband transport and CoreTelephony modem interface',
    'com.apple.locationd': 'location providers, authorization database and sensor interfaces',
    'com.apple.wifid': 'wlan/marconi-wifi device tree and Wi-Fi driver interface',
    'com.apple.bluetoothd': 'Bluetooth controller transport; also hosts BTAudioHALPlugin.xpc',
    'com.apple.nfcd': 'NFC controller and secure-element interfaces',
    'com.apple.awdd': 'WirelessDiagnostics clients and persistent diagnostics storage',
    'com.apple.analyticsd': 'CoreAnalyticsMessenger matching and diagnostics storage',
    'com.apple.passd': 'keychain persistence; NFC/secure element required for payment paths',
    'com.apple.biometrickitd': 'biometric sensor and secure-enclave interface',
    'com.apple.ScreenTimeAgent': 'keybag first-unlock, ScreenTime databases and account services',
    'com.apple.nanotimekitcompaniond': 'Watch pairing, NanoRegistry and snapshot rendering',
    'com.apple.fairplayd.H2': 'FairPlay key services and protected-content hardware contracts',
    'com.apple.ptpd': 'IOUSBDeviceInterface with USBDeviceFunction=PTP',
    'com.apple.searchd': 'Spotlight index storage, LaunchServices and keybag lifecycle',
}


def read(path):
    with Path(path).open('rb') as stream:
        return plistlib.load(stream)


def write(path, value, *, mode=0o600, owner=(0, 0)):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            plistlib.dump(value, stream, fmt=plistlib.FMT_BINARY)
            stream.flush()
            os.fsync(stream.fileno())
        os.chown(temporary, *owner)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def preserve_and_disable(root, labels):
    """Keep original definitions so a workaround can be reversed precisely."""
    root = Path(root)
    path = root / CACHE
    state = path.stat()
    cache = read(path)
    archive_path = root / ARCHIVE
    archive = read(archive_path) if archive_path.exists() else {}
    changed = []
    for key, job in cache['LaunchDaemons'].items():
        if job.get('Label') in labels:
            archive.setdefault(key, copy.deepcopy(job))
            job['Disabled'] = True
            changed.append(job['Label'])
    # Save the recovery data before modifying the launchd cache.
    write(archive_path, archive)
    panic_keys = 0
    for job in cache['LaunchDaemons'].values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            panic_keys += 1
    write(path, cache, mode=state.st_mode & 0o7777,
          owner=(state.st_uid, state.st_gid))
    print(f'launchd.plist: {panic_keys} panic keys renamed; '
          f'{len(changed)} jobs disabled with definitions preserved')
    missing = set(labels) - set(changed)
    if missing:
        print('Previously removed jobs require explicit restoration: ' + ', '.join(sorted(missing)))


def restore(root, label, source_cache=None):
    root = Path(root)
    path = root / CACHE
    state = path.stat()
    cache = read(path)
    source = Path(source_cache) if source_cache else root / ARCHIVE
    if not source.exists():
        raise ValueError('No preserved jobs; provide --source-cache from the same iOS build')
    originals = read(source)
    originals = originals.get('LaunchDaemons', originals)
    matches = [(key, job) for key, job in originals.items()
               if isinstance(job, dict) and job.get('Label') == label]
    if len(matches) != 1:
        raise ValueError(f'Expected one original definition for {label}; found {len(matches)}')
    key, original = matches[0]
    if not isinstance(key, str) or not key.startswith('/System/Library/'):
        raise ValueError('Original job cache key must be an absolute system plist path')
    jobs = cache['LaunchDaemons']
    duplicates = [k for k, job in jobs.items() if job.get('Label') == label and k != key]
    if duplicates:
        raise ValueError(f'Conflicting cached definitions for {label}: {duplicates}')
    job = copy.deepcopy(original)
    if job.get('Disabled'):
        raise ValueError('The original definition is disabled; select a valid original source')
    if '_PanicOnCrash' in job:
        job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
    jobs[key] = job
    write(path, cache, mode=state.st_mode & 0o7777, owner=(state.st_uid, state.st_gid))
    print(f'Restored definition: {label}')
    print('Required dependencies: ' + DEPENDENCIES.get(label, 'inspect the original service contract'))


def prepare_posters(root, build):
    """Install explicit, UUID-gated readiness and real provider preparation."""
    root, build = Path(root), Path(build)
    version = read(root / 'System/Library/CoreServices/SystemVersion.plist')
    if version.get('ProductBuildVersion') != '20H392':
        raise ValueError('Poster preparation has only been inspected for iOS build 20H392')
    artifacts = []
    for stem, filename, kind, destination in (
        ('SweetPosterBoard', 'SweetPosterBoard.dylib', 6, 'usr/local/lib/sweet/SweetPosterBoard.dylib'),
        ('SweetRegisterPosters', 'SweetRegisterPosters', 2, 'usr/local/libexec/SweetRegisterPosters'),
    ):
        manifest = json.loads((build / (stem + '.build.json')).read_text(encoding='utf-8'))
        data = (build / filename).read_bytes()
        source = Path(__file__).with_name('posterboard') / (stem + '.c')
        if manifest.get('os_build') != '20H392' or hashlib.sha256(data).hexdigest() != manifest.get('binary_sha256'):
            raise ValueError(f'Invalid build manifest/binary: {filename}')
        if hashlib.sha256(source.read_bytes()).hexdigest() != manifest.get('source_sha256'):
            raise ValueError(f'Source changed since build: {source.name}')
        if len(data) < 32 or struct.unpack_from('<II', data) != (0xfeedfacf, 0x100000c) or struct.unpack_from('<I', data, 12)[0] != kind:
            raise ValueError(f'Invalid arm64 Mach-O kind: {filename}')
        command_count, command_bytes = struct.unpack_from('<II', data, 16)
        if command_bytes > len(data) - 32:
            raise ValueError(f'Truncated Mach-O commands: {filename}')
        offset, platform = 32, None
        for _ in range(command_count):
            if offset + 8 > 32 + command_bytes:
                raise ValueError(f'Truncated Mach-O command: {filename}')
            command, size = struct.unpack_from('<II', data, offset)
            if size < 8 or size > 32 + command_bytes - offset:
                raise ValueError(f'Invalid Mach-O command: {filename}')
            if command == 0x32 and size >= 24:
                platform = struct.unpack_from('<I', data, offset + 8)[0]
            offset += size
        if platform != 2:
            raise ValueError(f'Artifact does not target iOS: {filename}')
        artifacts.append((root / destination, data))

    cache_path = root / CACHE
    state = cache_path.stat()
    cache = read(cache_path)
    matches = [(key, job) for key, job in cache['LaunchDaemons'].items()
               if job.get('Label') == 'com.apple.PosterBoard']
    if len(matches) != 1:
        raise ValueError('Expected exactly one existing PosterBoard launchd job')
    poster_key, poster_job = matches[0]
    if poster_job.get('ProgramArguments') != ['/Applications/PosterBoard.app/PosterBoard'] or poster_job.get('UserName') != 'mobile':
        raise ValueError('Unexpected PosterBoard launchd contract')
    preparation_key = '/System/Library/LaunchDaemons/org.heliodarwin.prepare-posters.plist'
    existing = cache['LaunchDaemons'].get(preparation_key)
    if existing and existing.get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation job')
    preparation_file = root / preparation_key.lstrip('/')
    if preparation_file.exists() and read(preparation_file).get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation plist')
    archive_path = root / ARCHIVE
    archive = read(archive_path) if archive_path.exists() else {}
    archive.setdefault(poster_key, copy.deepcopy(poster_job))
    write(archive_path, archive)
    for path, data in artifacts:
        missing = []
        directory = path.parent
        while not directory.exists():
            missing.append(directory)
            directory = directory.parent
        path.parent.mkdir(parents=True, exist_ok=True)
        for directory in missing:
            os.chown(directory, 0, 0)
            os.chmod(directory, 0o755)
        descriptor, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
        try:
            with os.fdopen(descriptor, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.chown(temporary, 0, 0)
            os.chmod(temporary, 0o755)
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    environment = poster_job.setdefault('EnvironmentVariables', {})
    libraries = environment.get('DYLD_INSERT_LIBRARIES', '').split(':')
    library = '/usr/local/lib/sweet/SweetPosterBoard.dylib'
    if library not in libraries:
        libraries.append(library)
    environment['DYLD_INSERT_LIBRARIES'] = ':'.join(item for item in libraries if item)
    environment['SWEET_POSTERBOARD_READINESS'] = '1'
    log_directory = root / 'private/var/mobile/Library/Logs'
    if not log_directory.exists():
        log_directory.mkdir(parents=True)
        os.chown(log_directory, 501, 501)
        os.chmod(log_directory, 0o755)
    poster_job.setdefault('StandardErrorPath', '/private/var/mobile/Library/Logs/SweetPosterBoard.log')
    job = {
        'Label': 'org.heliodarwin.prepare-posters', 'UserName': 'mobile',
        'ProgramArguments': ['/usr/local/libexec/SweetRegisterPosters', '--register', '--prewarm'],
        'RunAtLoad': True, 'KeepAlive': False, 'ProcessType': 'Background',
        'StandardOutPath': '/private/var/mobile/Library/Logs/SweetRegisterPosters.log',
        'StandardErrorPath': '/private/var/mobile/Library/Logs/SweetRegisterPosters.log',
    }
    write(root / preparation_key.lstrip('/'), job, mode=0o644)
    cache['LaunchDaemons'][preparation_key] = job
    write(cache_path, cache, mode=state.st_mode & 0o7777, owner=(state.st_uid, state.st_gid))
    print('Poster preparation installed; provider visibility, prewarm and dylib loading still require runtime readback')


def remove_poster_preparation(root):
    root = Path(root)
    path = root / CACHE
    cache = read(path)
    key = '/System/Library/LaunchDaemons/org.heliodarwin.prepare-posters.plist'
    existing = cache['LaunchDaemons'].get(key)
    file = root / key.lstrip('/')
    if existing and existing.get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation job')
    if file.exists() and read(file).get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation plist')
    restore(root, 'com.apple.PosterBoard')
    state = path.stat()
    cache = read(path)
    cache['LaunchDaemons'].pop(key, None)
    write(path, cache, mode=state.st_mode & 0o7777, owner=(state.st_uid, state.st_gid))
    if file.exists():
        file.unlink()
    print('Poster startup job and readiness environment removed; tools, logs and provider data retained')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--restore', metavar='LABEL')
    parser.add_argument('--prepare-posters', type=Path, metavar='BUILD_DIR',
                        help='opt in to verified local poster tools and real registration/prewarm at next boot')
    parser.add_argument('--remove-poster-preparation', action='store_true',
                        help='restore the original PosterBoard job and remove the preparation job')
    parser.add_argument('--source-cache', type=Path,
                        help='original launchd cache from the same iOS build')
    args = parser.parse_args()
    if args.remove_poster_preparation:
        if args.prepare_posters or args.restore or args.source_cache:
            parser.error('--remove-poster-preparation cannot be combined with other write operations')
        remove_poster_preparation(args.root)
        return
    if args.prepare_posters:
        if args.restore or args.source_cache:
            parser.error('--prepare-posters cannot be combined with daemon restoration')
        prepare_posters(args.root, args.prepare_posters)
        return
    if args.restore:
        restore(args.root, args.restore, args.source_cache)
        return
    jobs = read(args.root / CACHE)['LaunchDaemons']
    overrides_path = args.root / 'private/var/db/com.apple.xpc.launchd/disabled.plist'
    overrides = read(overrides_path) if overrides_path.exists() else {}
    for label, dependency in DEPENDENCIES.items():
        matches = [job for job in jobs.values() if job.get('Label') == label]
        status = 'removed' if not matches else 'present'
        if matches and overrides.get(label, matches[0].get('Disabled', False)):
            status = 'disabled'
        print(f'{label}: {status}; requires {dependency}')


if __name__ == '__main__':
    main()
