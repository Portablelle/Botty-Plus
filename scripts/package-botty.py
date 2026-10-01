#!/usr/bin/env python3
"""Package the compiled Botty homebrew and pin its manifest in the portal installer."""
import hashlib,json,pathlib,re,shutil,tarfile
project=pathlib.Path(__file__).resolve().parents[1]
source=project/'homebrew/botty';out=project/'vps-site/apps/botty';out.mkdir(parents=True,exist_ok=True)
files=[]
for name in ['botty-manager.elf','icon0.png','ui/index.html','ui/app.js','ui/style.css','cacert.pem']:
    original=source/('build/'+name if name.endswith('.elf') else name)
    target=out/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,target)
    data=target.read_bytes();files.append(dict(path=name,size=len(data),sha256=hashlib.sha256(data).hexdigest()))
manifest=(json.dumps(dict(schema=1,id='0.3.3',files=files),indent=2)+'\n').encode();(out/'manifest.json').write_bytes(manifest)
digest=hashlib.sha256(manifest).hexdigest();installer=project/'vps-site/src/botty-manager.js'
text,count=re.subn(r"const HASH='[a-f0-9]{64}';","const HASH='"+digest+"';",installer.read_text())
assert count==1;installer.write_text(text)
with tarfile.open(out/'botty-source.tar.gz','w:gz') as archive:
    for file in sorted(source.rglob('*')):
        if file.is_file() and not any(p in ('build','fixtures','__pycache__') for p in file.relative_to(source).parts):
            archive.add(file,arcname='botty/'+str(file.relative_to(source)),recursive=False)
shutil.copyfile(source/'README.md',out/'NOTICE.md')
print('Botty manifest:',digest)
