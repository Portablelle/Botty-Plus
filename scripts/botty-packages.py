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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    result = {}
    for package, notices in PACKAGES.items():
        base = root / package
        manifest = json.loads((base / 'manifest.json').read_text())
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
            path = base / name
            if path.stat().st_size != entry['size'] or digest(path) != entry['sha256']:
                raise ValueError('Package content mismatch: ' + package + '/' + name)
        for name in names:
            path = base / name
            if not path.is_file() or any(p.is_symlink() for p in [path, *path.parents] if p != root.parent):
                raise ValueError('Missing or symbolic package file: ' + str(path))
            result[package + '/' + name] = digest(path)
    return dict(sorted(result.items()))


def verify(root):
    record = json.loads((root / 'botty-release.json').read_text())
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
