#!/usr/bin/env python3
"""Package the PS5 rTorrent port and its manifest."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
root=Path(__file__).resolve().parents[1]
source=root/'homebrew/rtorrent'
out=root/'packages/rtorrent'
out.mkdir(parents=True,exist_ok=True)
files=[]
for name,original in [('rtorrent.elf',source/'build/rtorrent.elf'),('rtorrent.rc',source/'rtorrent.rc'),('cacert.pem',root/'homebrew/botty/cacert.pem')]:
    shutil.copyfile(original,out/name)
    data=(out/name).read_bytes()
    files.append(dict(path=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()))
revision = re.search(r'^#define BOTTY_RT_RUNTIME_VERSION "([^"\r\n]+)"$', (source/'runtime-version.hpp').read_text(), re.MULTILINE)
if not revision or not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+-botty[0-9]+', revision.group(1)):
    raise ValueError('Invalid compiled rTorrent revision')
manifest=(json.dumps(dict(schema=1,id=revision.group(1),files=files),indent=2)+'\n').encode()
(out/'manifest.json').write_bytes(manifest)
for name in ['README.md','LICENSE']:
    shutil.copyfile(source/name,out/name)
with tarfile.open(out/'rtorrent-source.tar.gz','w:gz') as archive:
    for item in sorted(source.iterdir()):
        if item.is_file():archive.add(item,arcname='botty-rtorrent/'+item.name)
    for item in sorted((source/'tests').rglob('*')):
        if item.is_file() and '__pycache__' not in item.parts and item.suffix != '.pyc':
            archive.add(item,arcname='botty-rtorrent/'+item.relative_to(source).as_posix())
    # Include the exact upstream inputs to provide corresponding source offline.
    for name in ['rtorrent-0.16.24.tar.gz','libtorrent-0.16.24.tar.gz']:
        archive.add(source/'build/downloads'/name,arcname='upstream/'+name)
print('rTorrent package:',hashlib.sha256(manifest).hexdigest())

import subprocess, sys
subprocess.run([sys.executable, str(root/'scripts/botty-packages.py')], check=True)
