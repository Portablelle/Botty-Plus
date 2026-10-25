#!/usr/bin/env python3
"""Fail closed on incomplete, altered or ambiguous native preview packages."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import re
import zipfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'emulation'))
from provenance import COMMIT, RECEIPT, digest, validate

def verify(dist, expected):
    dist=Path(dist); title=expected['titleId']; app=dist/title
    # Keep the upstream reservation: firmware 13.00 rejects 16 before main (0x80a40087).
    if expected.get('downloadDataSize') != 256:
        raise ValueError('Unvalidated downloadDataSize; native preview requires 256')
    manifest=json.loads((dist/'manifest.json').read_text())
    if (manifest.get('schema')!=1 or manifest.get('titleId')!=title or
        manifest.get('version')!=expected['contentVersion'] or
        manifest.get('hardwareValidated') is not False or
        manifest.get('registrationVerified') is not False or
        manifest.get('readOnly') is not False):
        raise ValueError('Unexpected preview manifest')
    if not isinstance(manifest.get('files'),list) or len(manifest['files'])>2048:raise ValueError('Native inventory exceeds installation limits')
    files={};total_size=0
    for item in manifest['files']:
        name=item['path']; path=PurePosixPath(name)
        if (path.is_absolute() or '..' in path.parts or str(path)!=name or
            '\\' in name or name in files or len(name)>240):
            raise ValueError('Unsafe or duplicate manifest path')
        if type(item.get('size')) is not int or not 1<=item['size']<=128*1024*1024:raise ValueError('Native file exceeds installation limits')
        total_size+=item['size']
        if total_size>512*1024*1024:raise ValueError('Native package exceeds installation limits')
        p=app/name
        if any(parent.is_symlink() for parent in [p, *p.parents]) or not p.is_file():raise ValueError('Missing or symbolic file')
        if p.stat().st_size!=item['size']:raise ValueError('File digest/size mismatch: '+name)
        data=p.read_bytes()
        if len(data)!=item['size'] or hashlib.sha256(data).hexdigest()!=item['sha256']:
            raise ValueError('File digest/size mismatch: '+name)
        files[name]=data
    if len(files)>2048 or any(len(data)<1 or len(data)>128*1024*1024 for data in files.values()) or sum(map(len,files.values()))>512*1024*1024:
        raise ValueError('Native package exceeds installation limits')
    required={'eboot.bin','sce_sys/param.json','sce_sys/icon0.png','sce_sys/pic0.dds','sce_sys/snd0.at9','sce_module/libc.prx',
              'assets/ui-font.bin','assets/Manrope-OFL.txt','assets/build.txt','assets/nebula.rgb',
              'assets/courier.rgba','assets/extractor.rgba','assets/vault.rgba'}
    extra=set(files)-required
    if extra:
        engine_files=json.loads((Path(__file__).resolve().parents[1]/'emulation/runtime-files.json').read_text())['files']
        if not ({'emulation-runtime.json'} | {'emulation/'+name for name in engine_files}).issubset(extra):
            raise ValueError('Incomplete embedded Switch engine')
        for name in extra:
            if name != 'emulation-runtime.json' and (not re.fullmatch(r'emulation/[A-Za-z0-9_./-]+',name) or
                '..' in name or 'keys' in name.lower() or 'firmware' in name.lower() or Path(name.lower()).suffix in ('.nca','.nsp','.xci')):
                raise ValueError('Unexpected or private engine file')
        if files['emulation/eboot.bin'][:4]!=bytes.fromhex('4f153d1d'):
            raise ValueError('Embedded engine is not a native FSELF')
        runtime=json.loads(files['emulation-runtime.json'])
        patch_sha=digest(Path(__file__).resolve().parents[1]/'emulation/patches/standalone.diff')
        validate(app/'emulation', patch_sha)
        if (runtime.get('standalone') is not True or runtime.get('commit') != COMMIT or
            runtime.get('patchSha256') != patch_sha or
            runtime.get('provenanceSha256') != digest(app/'emulation'/RECEIPT) or
            runtime.get('engineSha256')!=hashlib.sha256(files['emulation/eboot.bin']).hexdigest()):
            raise ValueError('Engine receipt mismatch')
        required |= extra
    if set(files)!=required:raise ValueError('Unexpected title file set')
    if len(files['assets/nebula.rgb']) != 960*540*3:
        raise ValueError('Invalid backdrop dimensions')
    for name in ('courier','extractor','vault'):
        if len(files[f'assets/{name}.rgba']) != 512*512*4:
            raise ValueError('Invalid illustration dimensions')
    if {str(p.relative_to(app)) for p in app.rglob('*') if p.is_file()}!=required:
        raise ValueError('Unlisted files in title folder')
    if json.loads(files['sce_sys/param.json'])!=expected:raise ValueError('Wrong title identity')
    # Verify actual native FSELF container signature, not an ELF payload or web shortcut.
    for name,magic in [('eboot.bin','4f153d1d'),('sce_module/libc.prx','5414f5ee')]:
        if files[name][:4]!=bytes.fromhex(magic):raise ValueError('Not a native FSELF')
    with zipfile.ZipFile(dist/(title+'.zip')) as archive:
        seen=set()
        for entry in archive.infolist():
            if entry.is_dir():continue
            if stat.S_ISLNK(entry.external_attr>>16):raise ValueError('Symlink in ZIP')
            name=entry.filename
            if not name.startswith(title+'/'):raise ValueError('Unexpected ZIP root')
            rel=name[len(title)+1:]
            if rel not in files or rel in seen or entry.file_size!=len(files[rel]):
                raise ValueError('Unexpected ZIP file')
            if archive.read(entry)!=files[rel]:raise ValueError('ZIP differs from title folder')
            seen.add(rel)
        if seen!=required:raise ValueError('Incomplete ZIP')
    return len(files)

if __name__=='__main__':
    root=Path(__file__).resolve().parents[1]
    count=verify(root/'dist',json.loads((root/'sce_sys/param.json').read_text()))
    print(f'Verified {count} native title files and identical ZIP contents; hardware remains unverified.')
