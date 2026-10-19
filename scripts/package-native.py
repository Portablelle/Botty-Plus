#!/usr/bin/env python3
"""Publish a verified native build to standalone packages, or refresh source/notices only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from release_sources import source_archive
from source_parts import publish


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-only', action='store_true')
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    source = project / 'homebrew/botty-native'
    out = project / 'packages/botty-native'
    out.mkdir(parents=True, exist_ok=True)
    if not args.source_only:
        sys.path.insert(0, str(source / 'tools'))
        from verify_package import verify
        expected = json.loads((source / 'sce_sys/param.json').read_text())
        dist = source / 'dist'
        verify(dist, expected)
        manifest = dist / 'manifest.json'
        target_manifest = json.loads(manifest.read_text())
        target_names = {item['path'] for item in target_manifest['files']}
        if 'emulation-runtime.json' in target_names and not (source / 'build/emulation-source.tar.gz').is_file():
            raise RuntimeError('Build emulation-source.tar.gz before distributing the embedded engine')
        removed = []
        if (out / 'manifest.json').is_file():
            for item in json.loads((out / 'manifest.json').read_text())['files']:
                name = item['path']
                if name in target_names:
                    continue
                relative = Path(name)
                if relative.is_absolute() or '..' in relative.parts or relative.as_posix() != name:
                    raise ValueError('Unsafe previous package path')
                stale = out / relative
                if stale.exists():
                    if any(parent.is_symlink() for parent in [stale, *stale.parents]) or not stale.is_file() or hashlib.sha256(stale.read_bytes()).hexdigest() != item['sha256']:
                        raise ValueError('Changed previous package asset retained: ' + name)
                    removed.append(stale)
        for item in target_manifest['files']:
            target = out / item['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(dist / expected['titleId'] / item['path'], target)
        for stale in removed:
            stale.unlink()
        shutil.copyfile(manifest, out / 'manifest.json')
        digest = hashlib.sha256(manifest.read_bytes()).hexdigest()
        print('Native manifest:', digest)
    source_names = []
    current_manifest = json.loads((out / 'manifest.json').read_text())
    if any(f['path'] == 'emulation-runtime.json' for f in current_manifest['files']):
        engine_sources = source / 'build/emulation-source.tar.gz'
        if not engine_sources.is_file():
            raise RuntimeError('Build emulation-source.tar.gz before distributing the embedded engine')
        source_names.append('build/emulation-source.tar.gz')
    source_archive(source, out / 'botty-native-source.tar.gz',
                   ['src', 'assets', 'sce_sys', 'vendor', 'tests', 'tools', 'artwork', 'emulation',
                    'Dockerfile', 'Makefile', 'BUILD-ENVIRONMENT.json', 'README.md',
                    'VALIDATION.md', 'LICENSE', 'update-compatibility.json', 'release.json'] + source_names)
    publish(out / 'botty-native-source.tar.gz', out)
    shutil.copyfile(source / 'LICENSE', out / 'LICENSE')
    shutil.copyfile(source / 'vendor/NOTICE.md', out / 'NOTICE.md')
    if source_names:
        with (out / 'NOTICE.md').open('a') as notice:
            notice.write('\nEmbedded Switch engine: ProsperoEden (GPL-3.0-or-later), commit '
                         '8f392248f7d49fc2be47f60ad7764ac5a392b1fe.\n'
                         'Complete corresponding sources are distributed as checked parts in source/.\n'
                         'botty-native-source.tar.gz contains their index and restore.py; run it against this package folder '
                         'to reconstruct the complete archive, including build/emulation-source.tar.gz.\n'
                         'https://github.com/blackbearreloaded/ProsperoEden\n')
    with (out / 'NOTICE.md').open('a') as notice:
        notice.write('\nOptional UltraCam BOTW 1.0.1 by MaxLastBreath, CC-BY-NC-2.0.\nSee emulation/ultracam/NOTICE.md and LICENSE.md in the runtime package for attribution and terms.\n')
    print('Native source and notices refreshed')
    import subprocess
    subprocess.run([sys.executable, str(project / 'scripts/botty-packages.py')], check=True)


if __name__ == '__main__':
    main()
