#!/usr/bin/env python3
"""Package the standalone Botty service; --source-only refreshes documentation/source."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
from release_sources import source_archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-only', action='store_true')
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    source = project / 'homebrew/botty'
    out = project / 'packages/botty'
    out.mkdir(parents=True, exist_ok=True)
    if not args.source_only:
        files = []
        for name in ['botty-manager.elf', 'icon0.png', 'ui/index.html', 'ui/app.js', 'ui/style.css', 'cacert.pem', 'game-compressor.elf']:
            original = source / ('build/' + name if name.endswith('.elf') else name)
            target = out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(original, target)
            data = target.read_bytes()
            if name == 'botty-manager.elf' and len(data) >= 16 * 1024 * 1024:
                raise ValueError('Manager exceeds the Portal cached-file read bound')
            files.append(dict(path=name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
        version = re.search(r'\{"version","([0-9.]+)"\}', (source / 'src/server.cpp').read_text()).group(1)
        worker = json.loads((project / 'homebrew/game-compressor/provenance.json').read_text())
        manifest = (json.dumps(dict(schema=1, id=version, apiVersion=1,
                                   workerVersion=worker['runtimeVersion'], workerApi=worker['mode'],
                                   updaterVersion='1.0.0', files=files), indent=2) + '\n').encode()
        (out / 'manifest.json').write_bytes(manifest)
        digest = hashlib.sha256(manifest).hexdigest()
        print('Botty manifest:', digest)
    source_archive(source, out / 'botty-source.tar.gz',
                   ['src', 'ui', 'vendor', 'tests', 'tools', 'Dockerfile', 'Makefile',
                    'README.md', 'LICENSE', 'cacert.pem', 'icon0.png'])
    shutil.copyfile(source / 'README.md', out / 'NOTICE.md')
    shutil.copyfile(source / 'LICENSE', out / 'LICENSE')
    worker=project/'homebrew/game-compressor'
    source_archive(worker,out/'game-compressor-source.tar.gz',['prepare.py','provenance.json','patches','vendor','tools','tests','README.md','NOTICE.md'])
    shutil.copyfile(worker/'NOTICE.md',out/'game-compressor-NOTICE.md')
    print('Botty source and notices refreshed')
    import subprocess, sys
    subprocess.run([sys.executable, str(project / 'scripts/botty-packages.py')], check=True)


if __name__ == '__main__':
    main()
