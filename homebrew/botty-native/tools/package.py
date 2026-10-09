#!/usr/bin/env python3
"""Manifest and corresponding-source archive. No production portal mutation."""
import hashlib
import json
from pathlib import Path
import tarfile
from verify_package import verify
from release_version import release_version, sync
ROOT = Path(__file__).resolve().parents[1]

def main():
    sync(check=True)
    param = json.loads((ROOT / 'sce_sys/param.json').read_text())
    title = param['titleId']
    dist = ROOT / 'dist'
    app = dist / title
    if not (app / 'eboot.bin').is_file() or not (app / 'sce_module/libc.prx').is_file():
        raise SystemExit('Missing native executable/runtime')
    if json.loads((app / 'sce_sys/param.json').read_text()) != param:
        raise SystemExit('Packaged metadata does not match source')
    files = [dict(path=str(p.relative_to(app)), size=p.stat().st_size,
                  sha256=hashlib.sha256(p.read_bytes()).hexdigest())
             for p in sorted(app.rglob('*')) if p.is_file()]
    manifest = dict(schema=1, app='Botty+', titleId=title,
                    version=param['contentVersion'], releaseVersion=release_version(), milestone=4,
                    hardwareValidated=False, registrationVerified=False,
                    identityStatus='provisional-until-console-inventory',
                    readOnly=False, requires=json.loads((ROOT / 'update-compatibility.json').read_text()), files=files)
    (dist / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    with tarfile.open(dist / 'botty-native-source.tar.gz', 'w:gz') as archive:
        for p in sorted(ROOT.rglob('*')):
            rel = p.relative_to(ROOT)
            if any(part in {'.deps','dist','build','__pycache__'} for part in rel.parts):
                continue
            if p.is_file():
                archive.add(p, arcname='botty-native/'+str(rel), recursive=False)
    (dist / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n'
        for p in sorted(dist.iterdir()) if p.is_file() and p.name!='SHA256SUMS'))
    verify(dist,param)
    print('Native preview package verified:', app)

if __name__ == '__main__':
    main()
