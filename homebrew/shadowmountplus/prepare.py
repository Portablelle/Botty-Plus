#!/usr/bin/env python3
"""Unpack verified upstream source and apply Botty's TitleDir recovery patch."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

root = Path(__file__).resolve().parent
meta = json.loads((root / 'provenance.json').read_text())
for name, digest in meta['sha256'].items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != digest:
        raise SystemExit('Source hash mismatch: ' + name)
build = root / 'build'
if build.exists():
    raise SystemExit('Build directory already exists; use a fresh checkout or preserve it before preparing again')
build.mkdir()
with tarfile.open(root / 'vendor/shadowmountplus-d7e35e6.tar.gz') as archive:
    archive.extractall(build, filter='data')
for patch in ('title-dir-recovery.patch', 'kstuff-lite-no-legacy-control.patch', 'pin-shellcore-hooks.patch'):
    subprocess.run(['patch', '-p1', '--batch', '--fuzz=0', '-i',
                    str(root / 'patches' / patch)], cwd=build, check=True)
print('Prepared ShadowMountPlus ' + meta['version'])
