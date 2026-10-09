#!/usr/bin/env python3
"""Build a normal title via the pinned clean-room runtime, never a payload."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import zipfile
from release_version import sync

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'vendor/boilerplate-dd44bbd.tar.gz'
SHA = '133b4ec9d21d1f49148c823131d7fb73f93fe653d35407f71702546a88a12831'

def main():
    sync()
    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != SHA:
        raise SystemExit('Native boilerplate source digest mismatch')
    cache = ROOT / '.deps/boilerplate-dd44bbd'
    cache.mkdir(parents=True, exist_ok=True)
    # Reapply pinned source; downloaded SDK/build caches remain local to this tree.
    with tarfile.open(ARCHIVE) as source:
        source.extractall(cache, filter='data')
    for name in ('src', 'vendor', 'sce_sys', 'assets'):
        target = cache / 'botty' / name
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(ROOT / name, target, ignore=shutil.ignore_patterns('boilerplate-*.tar.gz', '._*', '.DS_Store'))
    # The pinned boilerplate requires a pair of backdrops. Botty uses only the
    # selection image; add it to the assembled title after the runtime build.
    selection = cache / 'botty/sce_sys/pic0.dds'
    if selection.exists() and not (selection.parent / 'pic1.dds').exists():
        selection.unlink()
    env = dict(os.environ, APP_SOURCE_DIR='botty/src', APP_PARAM='botty/sce_sys/param.json',
               APP_SCE_SYS='botty/sce_sys', APP_ASSETS='botty/assets', USE_CCACHE='0')
    subprocess.run(['make', 'app'], cwd=cache, env=env, check=True)
    title = json.loads((ROOT / 'sce_sys/param.json').read_text())['titleId']
    output = ROOT / 'dist'
    output.mkdir(exist_ok=True)
    if (output / title).exists():
        shutil.rmtree(output / title)
    shutil.copytree(cache / 'dist' / title, output / title)
    if (ROOT / 'sce_sys/pic0.dds').exists():
        shutil.copy2(ROOT / 'sce_sys/pic0.dds', output / title / 'sce_sys/pic0.dds')
    shutil.copy2(ROOT / 'sce_sys/snd0.at9', output / title / 'sce_sys/snd0.at9')
    with zipfile.ZipFile(output / (title + '.zip'), 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted((output / title).rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(output))
    # Preserve exact tool versions beside the package, for rebuilding/auditing.
    with (output / 'build-tools.txt').open('w') as log:
        subprocess.run(['dpkg-query', '-W'], stdout=log, check=True)
    subprocess.run(['python3', str(ROOT / 'tools/package.py')], check=True)

if __name__ == '__main__':
    main()
