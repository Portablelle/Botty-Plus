#!/usr/bin/env python3
"""Package the pinned Botty fix and its patched upstream source snapshot."""
import argparse
import gzip
import io
import tarfile
import tempfile
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.17.2-botty.1'
REVISION = '59dcb9efaba71af00dc29d6cfe35da7d1ea43651'
ELF_HASH = 'f105d11e873747a029689576f8684dc4caaf98770e18f66130905733e5f1b405'
UPSTREAM_HASH = '36ad03e236e6603aaaf05d95ed3399c7b54475a9bceba085cbe02899bc88ae0f'
TILE_HASH = '22091bb243335bfca5d4e0e4fd1a6684138acca67bd778c1481bd7f23b3a58d2'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--upstream-elf', required=True, type=Path)
    args = parser.parse_args()
    official = args.upstream_elf.read_bytes()
    if hashlib.sha256(official).hexdigest() != UPSTREAM_HASH:
        parser.error('Expected the pinned upstream ELF for its original tile')
    tile = official[2903568:2903568 + 7091707]
    if hashlib.sha256(tile).hexdigest() != TILE_HASH:
        parser.error('Original tile hash mismatch')
    data = args.elf.read_bytes()
    if hashlib.sha256(data).hexdigest() != ELF_HASH or data[:4] != b'\x7fELF':
        parser.error('Not the pinned 0.17.2-botty.1 ELF')
    revision = subprocess.check_output(['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(args.source), 'status', '--porcelain'], text=True).strip()
    if revision != REVISION or dirty:
        parser.error('Expected a clean checkout of the pinned source revision')
    if b'\x7fFIH' not in data or b'http://127.0.0.1:9999' not in data:
        parser.error('Embedded home-screen package missing')
    target = ROOT / 'vps-site/apps/cheatrunner'
    target.mkdir(parents=True, exist_ok=True)
    (target / 'CheatRunner.elf').write_bytes(data)
    shutil.copyfile(args.source / 'LICENSE', target / 'LICENSE')
    source = subprocess.check_output(['git', '-C', str(args.source), 'archive', '--format=tar', '--prefix=CheatRunner/', REVISION])
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        with tarfile.open(fileobj=io.BytesIO(source)) as archive:
            archive.extractall(work, filter='data')
        tree = work / 'CheatRunner'
        patch = ROOT / 'homebrew/cheatrunner/patches/source-worker-stack.patch'
        for name in ['Makefile', 'src/cr_source_jobs.c', 'src/cr_remote_sources.c']:
            path = tree / name
            path.write_text(path.read_text())
        subprocess.run(['git', 'apply', str(patch)], cwd=tree, check=True)
        (tree / 'dist').mkdir(exist_ok=True)
        (tree / 'dist/CheatRunner.pkg').write_bytes(tile)
        result = io.BytesIO()
        with tarfile.open(fileobj=result, mode='w') as archive:
            for path in sorted(tree.rglob('*')):
                if not path.is_file():
                    continue
                content = path.read_bytes()
                info = tarfile.TarInfo('CheatRunner/' + path.relative_to(tree).as_posix())
                info.size = len(content)
                info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                archive.addfile(info, io.BytesIO(content))
        (target / 'cheatrunner-source.tar.gz').write_bytes(gzip.compress(result.getvalue(), mtime=0))
    manifest = dict(schema=1, app='CheatRunner', version=VERSION, revision=REVISION,
                    upstream='https://github.com/notmaj0r/CheatRunner', files=[
                        dict(path='CheatRunner.elf', size=len(data), sha256=ELF_HASH)])
    raw = (json.dumps(manifest, indent=2) + '\n').encode()
    (target / 'manifest.json').write_bytes(raw)
    installer = ROOT / 'vps-site/src/cheatrunner.js'
    code = installer.read_text()
    code, count = re.subn(r"const HASH = '[a-f0-9]{64}'", "const HASH = '" + hashlib.sha256(raw).hexdigest() + "'", code)
    if count != 1:
        raise ValueError('Installer manifest pin not found')
    code = re.sub(r"export const VERSION = '[^']+'", "export const VERSION = '" + VERSION + "'", code)
    installer.write_text(code)
    shutil.copyfile(ROOT / 'homebrew/cheatrunner/README.md', target / 'NOTICE.md')
    print('Packaged CheatRunner ' + VERSION)

if __name__ == '__main__':
    main()
