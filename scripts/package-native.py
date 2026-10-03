#!/usr/bin/env python3
"""Publish a verified native build to the local portal, or refresh source/notices only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
from release_sources import source_archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-only', action='store_true')
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    source = project / 'homebrew/botty-native'
    out = project / 'vps-site/apps/botty-native'
    out.mkdir(parents=True, exist_ok=True)
    if not args.source_only:
        sys.path.insert(0, str(source / 'tools'))
        from verify_package import verify
        expected = json.loads((source / 'sce_sys/param.json').read_text())
        dist = source / 'dist'
        verify(dist, expected)
        manifest = dist / 'manifest.json'
        for item in json.loads(manifest.read_text())['files']:
            target = out / item['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(dist / expected['titleId'] / item['path'], target)
        shutil.copyfile(manifest, out / 'manifest.json')
        digest = hashlib.sha256(manifest.read_bytes()).hexdigest()
        installer = project / 'vps-site/src/botty-native.js'
        text, count = re.subn(r"const HASH\s*=\s*'[a-f0-9]{64}';", "const HASH = '" + digest + "';", installer.read_text())
        if count != 1:
            raise ValueError('Missing native installer hash')
        installer.write_text(text)
        print('Native manifest:', digest)
    source_archive(source, out / 'botty-native-source.tar.gz',
                   ['src', 'assets', 'sce_sys', 'vendor', 'tests', 'tools', 'artwork',
                    'Dockerfile', 'Makefile', 'BUILD-ENVIRONMENT.json', 'README.md',
                    'VALIDATION.md', 'LICENSE'])
    shutil.copyfile(source / 'LICENSE', out / 'LICENSE')
    shutil.copyfile(source / 'vendor/NOTICE.md', out / 'NOTICE.md')
    print('Native source and notices refreshed')


if __name__ == '__main__':
    main()
