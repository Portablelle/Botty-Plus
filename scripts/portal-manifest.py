#!/usr/bin/env python3
"""Verify, index and export only the public portal files; never local backups."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

PUBLIC_FILES = ('index.html', 'portal.css', 'README.md', 'LICENSE')
PAYLOADS = ('ProsperoMgr.elf', 'elfldr-ps5-1360.elf', 'ftpsrv-ps5.elf',
            'a53_ppr_install.elf', 'kexp_2026_05_25.bin', 'kstuff.elf', 'shadowmountplus.elf')
PAYLOAD_NOTICES = ('shadowmountplus-source.tar.gz', 'shadowmountplus-LICENSE.txt',
                   'shadowmountplus-NOTICE.md', 'ppr-patch-source.tar.gz',
                   'ppr-patch-LICENSE.txt', 'ppr-patch-NOTICE.md')
PACKAGE_NOTICES = {
    'codex': ('NOTICE.md', 'LICENSE'),
    'cheatrunner': ('NOTICE.md', 'LICENSE', 'cheatrunner-source.tar.gz'),
    'rtorrent': ('README.md', 'LICENSE', 'rtorrent-source.tar.gz'),
    'botty': ('NOTICE.md', 'LICENSE', 'botty-source.tar.gz', 'game-compressor-source.tar.gz', 'game-compressor-NOTICE.md'),
    'botty-native': ('NOTICE.md', 'LICENSE', 'botty-native-source.tar.gz'),
    'transmission': ('NOTICE.txt', 'GPL-2.0.txt', 'GPL-3.0.txt',
                     'TRANSMISSION-COPYING.txt', 'WEBSRV-LICENSE.txt',
                     'public_html/transmission-app.js.LEGAL.txt'),
}


def public_files(root):
    names = list(PUBLIC_FILES) + ['payloads/' + name for name in (*PAYLOADS, *PAYLOAD_NOTICES)]
    for directory in ('src', 'offsets'):
        path = root / directory
        if not path.is_dir() or path.is_symlink():
            raise ValueError('Missing or symbolic public directory: ' + directory)
        for item in sorted(path.rglob('*')):
            if item.is_symlink():
                raise ValueError('Symbolic release path: ' + str(item.relative_to(root)))
            if item.is_file() and item.suffix == '.js' and not any(
                    part.startswith('.') for part in item.relative_to(root).parts):
                names.append(item.relative_to(root).as_posix())
    for package, notices in PACKAGE_NOTICES.items():
        prefix = 'apps/' + package + '/'
        manifest = json.loads((root / prefix / 'manifest.json').read_text())
        entries = manifest['files'] + ([manifest['helper']] if 'helper' in manifest else [])
        names.extend(prefix + name for name in ('manifest.json', *notices))
        for entry in entries:
            name = entry['path']
            rel = Path(name)
            if rel.is_absolute() or '..' in rel.parts or '\\' in name or rel.as_posix() != name:
                raise ValueError('Unsafe package path: ' + name)
            names.append(prefix + name)
    files = []
    for name in sorted(set(names)):
        path = root / name
        if not path.is_file() or path.is_symlink():
            raise ValueError('Missing or symbolic public path: ' + name)
        parent = path.parent
        while parent != root:
            if parent.is_symlink():
                raise ValueError('Symbolic public parent: ' + name)
            parent = parent.parent
        files.append(path)
    return files


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_packages(root):
    for package, installer, constant in (
        ('codex', 'codex-install.js', 'HASH'),
        ('cheatrunner', 'cheatrunner.js', 'HASH'),
        ('botty', 'botty-manager.js', 'HASH'),
        ('rtorrent', 'rtorrent.js', 'HASH'),
        ('botty-native', 'botty-native.js', 'HASH'),
        ('transmission', 'transmission.js', 'MANIFEST_HASH'),
    ):
        base = root / 'apps' / package
        manifest = base / 'manifest.json'
        code = (root / 'src' / installer).read_text()
        match = re.search(r'const\s+' + constant + r"\s*=\s*'([a-f0-9]{64})'", code)
        if not match or digest(manifest) != match[1]:
            raise ValueError('Installer manifest pin mismatch: ' + package)
        data = json.loads(manifest.read_text())
        entries = data['files'] + ([data['helper']] if 'helper' in data else [])
        seen = set()
        for entry in entries:
            name = entry['path']
            rel = Path(name)
            if (rel.is_absolute() or '..' in rel.parts or '\\' in name or
                    rel.as_posix() != name or name in seen):
                raise ValueError('Unsafe or duplicate package path: ' + name)
            seen.add(name)
            file = base / rel
            if (not file.is_file() or file.is_symlink() or
                    file.stat().st_size != entry['size'] or digest(file) != entry['sha256']):
                raise ValueError('Package content mismatch: ' + package + '/' + name)


def verify(root):
    files = public_files(root)
    verify_packages(root)
    hashes = {p.relative_to(root).as_posix(): digest(p) for p in files}
    manifest = json.loads((root / 'manifest.json').read_text())
    if manifest.get('sha256') != hashes:
        raise ValueError('Portal manifest is stale; regenerate it before exporting')
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'vps-site')
    parser.add_argument('--check', action='store_true', help='Verify without writing')
    parser.add_argument('--release', help='Release label when regenerating the manifest')
    parser.add_argument('--output', type=Path, help='Export verified files to a new directory')
    args = parser.parse_args()
    if args.release and (args.check or args.output):
        parser.error('--release is only for manifest generation')
    root = args.root.resolve()
    try:
        if args.check or args.output:
            files = verify(root)
        else:
            files = public_files(root)
            verify_packages(root)
            path = root / 'manifest.json'
            manifest = json.loads(path.read_text())
            if args.release:
                manifest['release'] = args.release
            manifest['sha256'] = {p.relative_to(root).as_posix(): digest(p) for p in files}
            path.write_text(json.dumps(manifest, indent=2) + '\n')
        if args.output:
            output = args.output.resolve()
            if output == root or root in output.parents or output.exists():
                raise ValueError('Export destination must be new and outside the portal')
            output.mkdir(parents=True)
            for file in files + [root / 'manifest.json']:
                target = output / file.relative_to(root)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(file, target)
            verify(output)
            print('Verified portal exported to', output)
        else:
            print(len(files), 'public portal files', 'verified' if args.check else 'indexed')
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
