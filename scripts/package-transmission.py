#!/usr/bin/env python3
"""Rebuild the pinned portal package from downloaded upstream artifacts. No network or PS5 writes."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ARCHIVE_SHA = 'c2d579d739c4aca053cf0722681cb921a0d1fad47ec83f7a47ae631743d9d1bd'
HELPER_SHA = '4676a40ac4c7ca1adeb04980efa48dd33a8961446b42e6a12abfcbe96090c8ac'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def package(archive, helper, output):
    archive_data, helper_data = archive.read_bytes(), helper.read_bytes()
    if digest(archive_data) != ARCHIVE_SHA or digest(helper_data) != HELPER_SHA:
        raise ValueError('Upstream artifact checksum mismatch; no output was written')
    entries = []
    with zipfile.ZipFile(archive) as source:
        for name in source.namelist():
            relative = name.removeprefix('Transmission/')
            if name.endswith('/') or not (relative == 'transmission-daemon.elf' or relative.startswith('public_html/')):
                continue
            if not name.startswith('Transmission/') or '..' in Path(relative).parts or Path(relative).is_absolute():
                raise ValueError('Unsafe archive entry')
            entries.append((relative, source.read(name)))
    if len(entries) != 8:
        raise ValueError('Unexpected package contents')
    output.mkdir(parents=True, exist_ok=True)
    files = []
    for relative, data in entries:
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        files.append(dict(path=relative, size=len(data), sha256=digest(data)))
    (output / 'websrv-ps5.elf').write_bytes(helper_data)
    manifest = dict(schema=1, id='4.0.6-v0.33-botty1', version='4.0.6',
                    source='https://github.com/ps5-payload-dev/websrv/releases/tag/v0.33',
                    archiveSha256=ARCHIVE_SHA, files=files,
                    helper=dict(path='websrv-ps5.elf', size=len(helper_data), sha256=HELPER_SHA))
    data = (json.dumps(manifest, indent=2) + '\n').encode()
    (output / 'manifest.json').write_bytes(data)
    print('Manifest SHA-256:', digest(data))
    print('This must match MANIFEST_HASH in vps-site/src/transmission.js.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('helper', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1] / 'vps-site/apps/transmission')
    args = parser.parse_args()
    package(args.archive, args.helper, args.output)
