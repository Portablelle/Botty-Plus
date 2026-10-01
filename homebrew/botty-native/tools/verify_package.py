#!/usr/bin/env python3
"""Fail closed on incomplete, altered or ambiguous native preview packages."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import zipfile

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
    files={}
    for item in manifest['files']:
        name=item['path']; path=PurePosixPath(name)
        if (path.is_absolute() or '..' in path.parts or str(path)!=name or
            '\\' in name or name in files):
            raise ValueError('Unsafe or duplicate manifest path')
        p=app/name
        if p.is_symlink() or not p.is_file():raise ValueError('Missing or symbolic file')
        data=p.read_bytes()
        if len(data)!=item['size'] or hashlib.sha256(data).hexdigest()!=item['sha256']:
            raise ValueError('File digest/size mismatch: '+name)
        files[name]=data
    required={'eboot.bin','sce_sys/param.json','sce_sys/icon0.png','sce_module/libc.prx',
              'assets/ui-font.bin','assets/Manrope-OFL.txt','assets/build.txt'}
    if set(files)!=required:raise ValueError('Unexpected title file set')
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
