#!/usr/bin/env python3
"""Index and verify the standalone Botty+ delivery contract."""
import argparse
import hashlib
import json
from pathlib import Path

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
    return dict(sorted(result.items()))


def verify(root):
    record = json.loads(regular_file(root / 'botty-release.json').read_text())
    if record.get('schema') != 1 or record.get('sha256') != inventory(root):
        raise ValueError('Botty release manifest is stale')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1] / 'packages')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if args.check:
        verify(args.root)
    else:
        record = dict(schema=1, sha256=inventory(args.root))
        (args.root / 'botty-release.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Botty+ packages verified' if args.check else 'Botty+ packages indexed')


if __name__ == '__main__':
    main()
