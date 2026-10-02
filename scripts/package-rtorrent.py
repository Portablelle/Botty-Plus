#!/usr/bin/env python3
"""Package the PS5 port and pin its installer to the exact manifest."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
root=Path(__file__).resolve().parents[1]
source=root/'homebrew/rtorrent'
out=root/'vps-site/apps/rtorrent'
out.mkdir(parents=True,exist_ok=True)
files=[]
for name,original in [('rtorrent.elf',source/'build/rtorrent.elf'),('rtorrent.rc',source/'rtorrent.rc'),('cacert.pem',root/'homebrew/botty/cacert.pem')]:
    shutil.copyfile(original,out/name)
    data=(out/name).read_bytes()
    files.append(dict(path=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()))
manifest=(json.dumps(dict(schema=1,id='0.16.24-botty2',files=files),indent=2)+'\n').encode()
(out/'manifest.json').write_bytes(manifest)
p=root/'vps-site/src/rtorrent.js'
s,n=re.subn(r"const HASH='[a-f0-9]{64}';","const HASH='"+hashlib.sha256(manifest).hexdigest()+"';",p.read_text())
assert n==1
p.write_text(s)
for name in ['README.md','LICENSE']:
    shutil.copyfile(source/name,out/name)
with tarfile.open(out/'rtorrent-source.tar.gz','w:gz') as archive:
    for item in sorted(source.iterdir()):
        if item.is_file():archive.add(item,arcname='botty-rtorrent/'+item.name)
    # Include the exact upstream inputs to provide corresponding source offline.
    for name in ['rtorrent-0.16.24.tar.gz','libtorrent-0.16.24.tar.gz']:
        archive.add(source/'build/downloads'/name,arcname='upstream/'+name)
print('rTorrent package:',hashlib.sha256(manifest).hexdigest())
