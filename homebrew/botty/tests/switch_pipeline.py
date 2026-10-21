#!/usr/bin/env python3
"""Run the real archive/format/CNMT/hash pipeline, including refusal cases."""
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile
import struct
from switch_fixtures import package,xci
from make_fixtures import single
ROOT=Path(__file__).resolve().parents[1]
BINARY=ROOT/'build/botty-switch-prepare'

def run(folder,names,ok=True,label='Fixture release'):
    out=folder/('prepared-'+str(len(list(folder.glob('request-*.json')))))
    req=folder/('request-'+out.name+'.json')
    req.write_text(json.dumps(dict(root=str(folder),files=[dict(name=n,size=(folder/n).stat().st_size) for n in names],output=str(out),keys='',name=label,transaction='fixture')))
    p=subprocess.run([str(BINARY),str(req)],capture_output=True,text=True)
    assert (p.returncode==0)==ok,(p.returncode,p.stderr,p.stdout[-1000:])
    if not ok:
        assert not (out/'prepared.json').exists()
        return p.stderr
    assert not list(out.glob('archive-*')), 'Successful preparation must remove intermediate archive output'
    return json.loads((out/'prepared.json').read_text())

with tempfile.TemporaryDirectory(prefix='botty-switch-pipeline-') as tmp:
    root=Path(tmp).resolve()
    contents=[]
    for kind,version in [('game',0),('update',65536),('dlc',0),('unlocker',1)]:
        # Unlocker belongs to another DLC ID so this is a valid multi-package bundle.
        base=0x0100000000002000 if kind=='unlocker' else 0x0100000000000000
        data=package(kind,version,base);contents.append((kind+'.nsp',data))
        id_=base if kind=='game' else base+0x800 if kind=='update' else base+0x1001
        title='Fixture' + (' DLC Unlocker' if kind=='unlocker' else ' DLC' if kind=='dlc' else ' Update v2.0.0' if kind=='update' else '')
        notes=f'Title: {title}\nTitle ID: {id_:016X}\n'
        if kind in ('dlc','unlocker'):notes+='Update v2.0.0 is required to use this DLC.\n'
        contents.append((kind+'.nfo',notes.encode()))
    single(root/'bundle.rar',contents)
    receipt=run(root,['bundle.rar'])
    types={f['metadata']['type'] for f in receipt['files'] if 'metadata' in f}
    assert types=={'game','update','dlc','unlocker'},types
    assert (root/'bundle.rar').exists()
    with zipfile.ZipFile(root/'bundle.zip','w',zipfile.ZIP_DEFLATED) as z:
        for n,b in contents:z.writestr(n,b)
    assert len(run(root,['bundle.zip'])['files'])==8
    with zipfile.ZipFile(root/'directories.zip','w',zipfile.ZIP_DEFLATED) as z:
        z.writestr('nested/', b'');z.writestr('nested/Game.nsp',package())
    assert run(root,['directories.zip'])['files'][0]['metadata']['type']=='game'
    single(root/'partial-notes.rar',[(n,b) for n,b in contents if not n.endswith('.nfo') or n=='dlc.nfo']);assert len(run(root,['partial-notes.rar'])['files'])==8
    (root/'Game.xci').write_bytes(xci());assert run(root,['Game.xci'])['files'][0]['metadata']['type']=='game'
    (root/'MultiProgram.nsp').write_bytes(package(id_offset=2));assert run(root,['MultiProgram.nsp'])['files'][0]['metadata']['type']=='game'
    (root/'MultiProgramUpdate.nsp').write_bytes(package('update',id_offset=2));assert run(root,['MultiProgramUpdate.nsp'])['files'][0]['metadata']['type']=='update'
    for name,options in [('WrongUpdateProgram',dict(wrong_program_id=True)),('PatchIdProgram',dict(update_program_uses_patch_id=True))]:
        path=root/(name+'.nsp');path.write_bytes(package('update',**options));assert 'program identity' in run(root,[path.name],False)
    (root/'DataDlc.nsp').write_bytes(package('dlc',dlc_content_type=4));assert run(root,['DataDlc.nsp'])['files'][0]['metadata']['type']=='dlc'
    (root/'WrongDlcType.nsp').write_bytes(package('dlc',dlc_content_type=0));assert 'NCA content type' in run(root,['WrongDlcType.nsp'],False)
    # Dedicated preparation followed by console import must preserve a bundle
    # whose XCI contains an older update alongside a newer separate patch.
    (root/'BundledGame.xci').write_bytes(xci(include_update=True))
    (root/'NewPatch.nsp').write_bytes(package('update',131072))
    (root/'PublicDlc.nsp').write_bytes(package('dlc'))
    first=run(root,['NewPatch.nsp','BundledGame.xci','PublicDlc.nsp'])
    prepared=root/('prepared-'+str(len(list(root.glob('request-*.json')))-1))/'content'
    second=run(prepared,[f['name'] for f in first['files']])
    assert {f['metadata']['type'] for f in second['files'] if 'metadata' in f}=={'game','update','dlc'}
    (root/'Corrupt.nsp').write_bytes(package(corrupt=True));assert 'SHA-256' in run(root,['Corrupt.nsp'],False)
    (root/'Delta.nsp').write_bytes(package('update',missing_delta=True));assert run(root,['Delta.nsp'])['files'][0]['metadata']['omittedDeltaFragments']==1
    (root/'NoProgram.nsp').write_bytes(package('game',missing_delta=True));run(root,['NoProgram.nsp'],False)
    for label,options,message in [('ProgramIdentity',dict(wrong_program_id=True),'program identity'),('MetaIdentity',dict(wrong_meta_id=True),'title identity'),('MetadataHash',dict(corrupt_metadata=True),'Metadata SHA-256'),('Empty',dict(empty=True),'required program'),('Duplicate',dict(duplicate=True),'Duplicate CNMT'),('Extra',dict(extra_nca=True),'Unreferenced NCA')]:
        path=root/(label+'.nsp');path.write_bytes(package(**options));assert message in run(root,[path.name],False)
    (root/'BadLabel.nsp').write_bytes(package())
    (root/'BadLabel.nsp.botty.json').write_text(json.dumps(dict(titleId='0100000000000000',name='Fixture',type='game',versionLabel=1)))
    assert 'labels must be text' in run(root,['BadLabel.nsp','BadLabel.nsp.botty.json'],False)
    (root/'HugeLabel.nsp').write_bytes(package())
    (root/'HugeLabel.nsp.botty.json').write_text(json.dumps(dict(titleId='0100000000000000',name='Fixture',type='game',versionLabel='x'*65400)))
    assert 'sidecar metadata exceeds' in run(root,['HugeLabel.nsp','HugeLabel.nsp.botty.json'],False)
    batch=[]
    for index in range(256):
        base=0x0100000000100000+index*0x2000;name=f'Batch{index}.nsp';sidecar=name+'.botty.json'
        (root/name).write_bytes(package(base=base))
        (root/sidecar).write_text(json.dumps(dict(titleId=f'{base:016X}',name='Fixture',type='game',versionLabel='x'*5000)))
        batch.extend([name,sidecar])
    assert 'batch receipt exceeds' in run(root,batch,False)
    assert all((root/name).exists() for name in batch)
    (root/'Wrong.nsp').write_bytes(b'PFS0broken');run(root,['Wrong.nsp'],False)
    with zipfile.ZipFile(root/'traversal.zip','w') as z:z.writestr('../escape.nsp',package())
    run(root,['traversal.zip'],False);assert not (root.parent/'escape.nsp').exists()
    with zipfile.ZipFile(root/'link.zip','w') as z:
        e=zipfile.ZipInfo('link');e.create_system=3;e.external_attr=0o120777<<16;z.writestr(e,b'/tmp/escape')
    run(root,['link.zip'],False)
    with zipfile.ZipFile(root/'zip64.zip','w',zipfile.ZIP_STORED) as z:
        with z.open('Game.nsp','w',force_zip64=True) as f:f.write(package())
    b=bytearray((root/'zip64.zip').read_bytes());at=len(b)-22;count=struct.unpack_from('<H',b,at+10)[0];central_size,central_at=struct.unpack_from('<II',b,at+12)
    end=bytes(b[at:]);b=b[:at]+struct.pack('<IQHHIIQQQQ',0x06064b50,44,45,45,0,0,count,count,central_size,central_at)+struct.pack('<IIQI',0x07064b50,0,at,1)+end
    struct.pack_into('<HHII',b,len(b)-14,65535,65535,0xffffffff,0xffffffff)
    (root/'zip64.zip').write_bytes(b);run(root,['zip64.zip'])
    bad=bytearray((root/'zip64.zip').read_bytes());bad[bad.index(b'NCA3')+3]^=1;(root/'bad-crc.zip').write_bytes(bad);assert 'CRC' in run(root,['bad-crc.zip'],False)
    single(root/'mismatch.rar',[('Game.nsp',package()),('release.nfo',b'Title: Fixture DLC\nTitle ID: 0100000000001001\n')]);assert 'NFO title ID' in run(root,['mismatch.rar'],False)
    print('RAR, ZIP, ZIP64, XCI, game/update/DLC/unlocker CNMT detection, NFO dependencies, CRC/hash failures, traversal and links PASS')
