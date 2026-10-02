#!/usr/bin/env python3
"""Package the patched ShadowMount payload and its complete source recipe."""
import hashlib
import json
from pathlib import Path
import shutil

from release_sources import source_archive

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / 'homebrew/shadowmountplus'
meta = json.loads((source / 'provenance.json').read_text())
binary = source / 'build/shadowmountplus.elf'
if binary.read_bytes()[:4] != b'\x7fELF':
    raise SystemExit('Expected a built PS5 ELF')
portal = ROOT / 'vps-site/payloads'
shutil.copyfile(binary, ROOT / 'payloads/shadowmountplus.elf')
shutil.copyfile(binary, portal / 'shadowmountplus.elf')
source_archive(source, portal / 'shadowmountplus-source.tar.gz',
               ['README.md', 'LICENSE', 'Dockerfile', 'prepare.py', 'build.sh',
                'provenance.json', 'vendor', 'patches', 'tests'])
shutil.copyfile(source / 'LICENSE', portal / 'shadowmountplus-LICENSE.txt')
(portal / 'shadowmountplus-NOTICE.md').write_text(
    '# Modified ShadowMountPlus\n\nVersion: ' + meta['version'] + '\n\n'
    'Upstream: ' + meta['upstream'] + ', revision `' + meta['revision'] + '`.\n\n'
    'Botty modification: guarded automatic TitleDir hook recovery and transient '
    'read handling; resident ShellCore hook pages; legacy Kstuff runtime toggles disabled for the bundled Lite implementation. This is not the unmodified upstream release.\n\n'
    'GPL-3.0 license: [license](shadowmountplus-LICENSE.txt). Complete pinned '
    'source, patch, build recipe, tests and SDK stub license: '
    '[corresponding source](shadowmountplus-source.tar.gz).\n')
provenance = ROOT / 'payloads/versions.json'
data = json.loads(provenance.read_text())
entry = next(item for item in data['payloads'] if item['file'] == binary.name)
entry.setdefault('upstream_version', entry['version'])
entry.setdefault('upstream_digest', entry['digest'])
entry.setdefault('upstream_url', entry.pop('url', None))
entry.setdefault('upstream_published_at', entry.pop('published_at', None))
entry.update(version=meta['version'], digest='sha256:' + hashlib.sha256(binary.read_bytes()).hexdigest(),
             source='homebrew/shadowmountplus', source_revision=meta['revision'],
             corresponding_source='vps-site/payloads/shadowmountplus-source.tar.gz')
provenance.write_text(json.dumps(data, indent=2) + '\n')
print('Packaged ' + meta['version'] + ' with corresponding source and notices')
