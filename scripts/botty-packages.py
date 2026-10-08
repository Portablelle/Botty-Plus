#!/usr/bin/env python3
"""Index and verify the standalone Botty+ delivery contract."""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'homebrew/botty-native/tools'))
from release_version import ps5_version, release_version, sync

PACKAGES = {
    'botty': ('NOTICE.md', 'LICENSE', 'botty-source.tar.gz', 'game-compressor-source.tar.gz', 'game-compressor-NOTICE.md'),
    'botty-native': ('NOTICE.md', 'LICENSE', 'botty-native-source.tar.gz'),
    'rtorrent': ('README.md', 'LICENSE', 'rtorrent-source.tar.gz'),
}


# Runtime files required independently of the supplied component manifests.
REQUIRED_FILES = {
    'botty': ('botty-manager.elf', 'icon0.png', 'ui/index.html', 'ui/app.js',
              'ui/style.css', 'cacert.pem', 'game-compressor.elf'),
    'botty-native': ('assets/Manrope-OFL.txt', 'assets/build.txt', 'assets/courier.rgba',
                     'assets/extractor.rgba', 'assets/nebula.rgb', 'assets/ui-font.bin',
                     'assets/vault.rgba', 'eboot.bin', 'sce_module/libc.prx',
                     'sce_sys/icon0.png', 'sce_sys/param.json', 'sce_sys/pic0.dds', 'sce_sys/snd0.at9'),
    'rtorrent': ('rtorrent.elf', 'rtorrent.rc', 'cacert.pem'),
}

def regular_file(path):
    if any(p.is_symlink() for p in [path, *path.parents]) or not path.is_file():
        raise ValueError("Missing or symbolic package file: " + str(path))
    return path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compatible_versions(root):
    native = json.loads(regular_file(root / 'botty-native/manifest.json').read_text())
    required = native.get('requires')
    if required is None:
        return
    manager = json.loads(regular_file(root / 'botty/manifest.json').read_text())
    engine = json.loads(regular_file(root / 'rtorrent/manifest.json').read_text())
    if not isinstance(required, dict) or set(required) != {'manager', 'worker', 'rtorrent', 'apiVersion'}:
        raise ValueError('Invalid native compatibility contract')
    def version(value, engine=False):
        pattern = r'([0-9]+)\.([0-9]+)\.([0-9]+)(?:-botty([0-9]+))' if engine else r'([0-9]+)\.([0-9]+)\.([0-9]+)'
        match = re.fullmatch(pattern, value) if isinstance(value, str) and len(value) <= 64 else None
        if not match:
            raise ValueError('Invalid component version')
        result = tuple(int(part) for part in match.groups())
        if any(part > 999999 for part in result):
            raise ValueError('Component version exceeds updater bounds')
        return result
    if (type(required['apiVersion']) is not int or required['apiVersion'] != 1 or
            type(manager.get('apiVersion')) is not int or
            manager.get('apiVersion') != required['apiVersion'] or
            manager.get('workerApi') != 'library-1.3' or manager.get('updaterVersion') != '1.0.0' or
            version(manager.get('id')) < version(required['manager']) or
            version(manager.get('workerVersion')) < version(required['worker']) or
            version(engine.get('id'), True) < version(required['rtorrent'], True)):
        raise ValueError('Native and service packages are incompatible')


def inventory(root):
    result = {}
    for package, notices in PACKAGES.items():
        base = root / package
        manifest = json.loads(regular_file(base / 'manifest.json').read_text())
        if manifest.get('schema') != 1 or not isinstance(manifest.get('version') if package == 'botty-native' else manifest.get('id'), str):
            raise ValueError('Unsupported Botty package: ' + package)
        names = ['manifest.json', *notices]
        for entry in manifest['files']:
            name = entry['path']
            relative = Path(name)
            if (not name or relative.is_absolute() or '..' in relative.parts or '\\' in name
                    or relative.as_posix() != name or name in names):
                raise ValueError('Unsafe or duplicate package path: ' + name)
            names.append(name)
            path = regular_file(base / name)
            if path.stat().st_size != entry['size'] or digest(path) != entry['sha256']:
                raise ValueError('Package content mismatch: ' + package + '/' + name)
        missing = set(REQUIRED_FILES[package]) - {entry['path'] for entry in manifest['files']}
        if missing:
            raise ValueError('Missing required package files: ' + package + ': ' + ', '.join(sorted(missing)))
        for name in names:
            path = regular_file(base / name)
            result[package + '/' + name] = digest(path)
        actual = {p.relative_to(base).as_posix() for p in base.rglob('*') if not p.is_dir() or p.is_symlink()}
        extra = actual - set(names)
        if extra:
            raise ValueError('Unlisted package files: ' + package + ': ' + ', '.join(sorted(extra)))
    compatible_versions(root)
    return dict(sorted(result.items()))


def release_identity(root):
    native = json.loads(regular_file(root / 'botty-native/manifest.json').read_text())
    manager = json.loads(regular_file(root / 'botty/manifest.json').read_text())
    engine = json.loads(regular_file(root / 'rtorrent/manifest.json').read_text())
    for name, manifest, keys in [
        ('botty-native/manifest.json', native, ('releaseVersion', 'version')),
        ('botty/manifest.json', manager, ('id', 'workerVersion', 'apiVersion', 'workerApi', 'updaterVersion')),
        ('rtorrent/manifest.json', engine, ('id',)),
    ]:
        for key in keys:
            if manifest.get(key) is None:
                raise ValueError(f'Missing {key} in {name}')
    version = native.get('releaseVersion')
    if ps5_version(version) != native['version']:
        raise ValueError('Public release and native PS5 version disagree')
    param = json.loads(regular_file(root / 'botty-native/sce_sys/param.json').read_text())
    if param.get('contentVersion') is None:
        raise ValueError('Missing contentVersion in botty-native/sce_sys/param.json')
    if param.get('contentVersion') != native['version']:
        raise ValueError('Native metadata and release version disagree')
    return dict(version=version,
                components=dict(native=native['version'], manager=manager['id'],
                                worker=manager['workerVersion'], rtorrent=engine['id']),
                apis=dict(manager=manager['apiVersion'], worker=manager['workerApi'],
                          updater=manager['updaterVersion']))


def release_record(root):
    return dict(schema=1, **release_identity(root), sha256=inventory(root))


def verify(root):
    record = json.loads(regular_file(root / 'botty-release.json').read_text())
    if record != release_record(root):
        raise ValueError('Botty release manifest is stale')
    return record


def verify_public_version(record):
    sync(check=True)
    if record['version'] != release_version():
        raise ValueError('Packaged release does not match release.json; rebuild the native title')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1] / 'packages')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--tag', help='Require the GitHub release tag to be v<public version>')
    args = parser.parse_args()
    if args.check:
        record = verify(args.root)
    else:
        record = release_record(args.root)
    if args.check or args.tag is not None:
        verify_public_version(record)
    if args.tag is not None and args.tag != 'v' + record['version']:
        raise ValueError('GitHub release tag does not match the public Botty+ version')
    if not args.check:
        (args.root / 'botty-release.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Botty+ packages verified' if args.check else 'Botty+ packages indexed')


if __name__ == '__main__':
    main()
