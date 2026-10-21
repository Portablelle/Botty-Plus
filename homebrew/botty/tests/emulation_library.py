#!/usr/bin/env python3
"""Real manager API: legacy ROM metadata, associations, artwork and cache invalidation."""
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from switch_fixtures import package,encrypted_headers
from rtorrent_fixture import RtorrentFixture

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x0100152000022000
TITLE = f'{BASE:016X}'

with tempfile.TemporaryDirectory(prefix='botty-library-') as folder:
    root=Path(folder)
    for sub in ('roms','updates','keys','config','mods'):
        (root/'eden'/sub).mkdir(parents=True)
    game=root/'eden/roms/opaque-scene-file.nsp'
    game.write_bytes(package(base=BASE,display=('Mario Kart 8 Deluxe','1.0.0')))
    update=root/'eden/updates/unknown-update.nsp'
    update.write_bytes(package('update',1376256,base=BASE,display=('Mario Kart 8 Deluxe','3.0.5')))
    (root/'eden/updates/dlc.nsp').write_bytes(package('dlc',base=BASE))
    (root/'eden/updates/unrelated.nsp').write_bytes(package('dlc',base=BASE+0x10000))
    legacy=root/'legacy-eden/roms';legacy.mkdir(parents=True)
    (legacy/'old-file.nsp').write_bytes(package(base=BASE+0x20000,display=('Legacy fixture','1.0.0')))
    for name in ('Example mod','Disabled mod'):
        mod=root/'eden/mods'/TITLE.lower()/name/'romfs';mod.mkdir(parents=True)
        (mod/'fixture.bin').write_bytes(b'test')
    settings=root/'eden/config/prosperoeden.json'
    settings.write_text(json.dumps({'games':{TITLE:{'mods_off':['Disabled mod']}}}))
    # No real keys. The synthetic NCAs deliberately use clear metadata/Control.
    (root/'eden/keys/prod.keys').write_text('# synthetic unencrypted test fixtures\n')
    credentials=root/'rtorrent/state';credentials.mkdir(parents=True)
    (credentials/'botty-credentials.json').write_text(json.dumps({'username':'botty','password':'fixture'}))
    rpc=RtorrentFixture([])
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    process=subprocess.Popen([str(ROOT/'build/botty-native'),'--root',str(root),'--ui',str(ROOT/'ui'),'--port',str(port),'--rpc-port',str(rpc.server_port)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    token=''
    def get(path,binary=False,status=200):
        req=urllib.request.Request(f'http://127.0.0.1:{port}{path}',headers={'X-Botty-Token':token})
        try:response=urllib.request.urlopen(req,timeout=10)
        except urllib.error.HTTPError as error:response=error
        assert response.status==status,(response.status,response.read())
        value=response.read();return value if binary else json.loads(value)
    def no_embedded_cover(ident):
        # No remote artwork configuration in this fixture: never substitute square icons.
        req=urllib.request.Request(f'http://127.0.0.1:{port}/api/artwork?id=m:'+ident,headers={'X-Botty-Token':token})
        for _ in range(100):
            try:
                with urllib.request.urlopen(req,timeout=5) as response:
                    assert response.status==202;response.read()
            except urllib.error.HTTPError as error:
                assert error.code==404;return
            time.sleep(.02)
        raise AssertionError('Artwork did not settle')
    def entries():return get('/api/state')['emulation']['entries']
    try:
        for _ in range(200):
            try:token=get('/api/bootstrap')['token'];break
            except urllib.error.URLError:
                assert process.poll() is None;time.sleep(.05)
        rows=entries();selected=next(r for r in rows if r['kind']=='roms')
        legacy_entry=next(r for r in rows if r.get('source')=='existing-ps5')
        assert legacy_entry['name']=='Legacy fixture'
        no_embedded_cover(legacy_entry['artworkId'])
        assert selected['name']=='Mario Kart 8 Deluxe',selected
        assert selected['titleId']==TITLE
        assert selected['baseVersion']=='v1.0.0' and selected['updateVersion']=='v3.0.5'
        assert 'v1.0.0' in selected['phase'] and 'v3.0.5' in selected['phase'],selected
        assert len(selected['updates'])==1 and len(selected['dlc'])==1,selected
        assert selected['modsKnown'] and sum(m['enabled'] for m in selected['mods'])==1
        assert selected['dlcCount']==1 and selected['modCount']==2 and selected['modsOn']==1
        assert selected['library']['verified'] is False # display is not import verification
        orphan=next(r for r in rows if r['path'].endswith('unrelated.nsp'))
        assert orphan['dependencyStatus']=='base-game-missing'
        no_embedded_cover(selected['id'])
        old_artwork=selected['artworkId']
        hash_value=2166136261
        for ch in game.name.encode():hash_value=((hash_value^ch)*16777619)&0xffffffff
        cover=root/'eden/covers'/f'{hash_value:08x}.tga';cover.parent.mkdir()
        # Embedded launcher artwork changes no longer replace uniform remote portraits.
        cover.write_bytes(bytes([0,0,2])+bytes(9)+bytes([2,0,2,0,24,32])+bytes([255,0,0])*4)
        selected=next(r for r in entries() if r['kind']=='roms')
        assert selected['artworkId']!=old_artwork
        no_embedded_cover(selected['artworkId'])
        # A valid TGA header with no pixels must fall back to the embedded icon,
        # never expose an uninitialized decoder buffer.
        cover.write_bytes(cover.read_bytes()[:18])
        selected=next(r for r in entries() if r['kind']=='roms')
        no_embedded_cover(selected['artworkId'])
        get('/api/artwork?id=m:not-a-file',binary=True,status=404)
        # Settings changes do not require re-reading a multi-GB game.
        settings.write_text(json.dumps({'games':{TITLE:{'mods':False}}}))
        selected=next(r for r in entries() if r['kind']=='roms')
        assert not any(m['enabled'] for m in selected['mods'])
        # Replacing a same-path update invalidates its cached display version.
        update.write_bytes(package('update',1441792,base=BASE,display=('Mario Kart 8 Deluxe','4.0.0')))
        assert 'v4.0.0' in next(r for r in entries() if r['kind']=='roms')['phase']
        update.unlink()
        selected=next(r for r in entries() if r['kind']=='roms')
        assert not selected['updates'] and 'Update: none detected' in selected['details']
        game.write_bytes(package(base=BASE,display=('Replacement title','2.0.0')))
        assert next(r for r in entries() if r['kind']=='roms')['name']=='Replacement title'
        # Encrypted NCA2 resets filesystem-header tweaks; NCA3 does not.
        for nca2 in (True,False):
            encoded,key=encrypted_headers(package(base=BASE,display=('Encrypted title','2.1.0')),nca2)
            (root/'eden/keys/prod.keys').write_text('header_key = '+key+'\n')
            game.write_bytes(encoded)
            selected=next(r for r in entries() if r['kind']=='roms')
            assert selected['name']=='Encrypted title' and selected['baseVersion']=='v2.1.0',selected
        # Unreadable metadata never hides a launchable file or invents its version.
        game.write_bytes(b'PFS0bad metadata')
        selected=next(r for r in entries() if r['kind']=='roms')
        assert selected['name']=='opaque-scene-file' and 'Details unavailable' in selected['phase']
    finally:
        process.terminate();process.wait(timeout=10);rpc.shutdown();rpc.server_close()
print('Emulation library metadata, artwork and cache regressions passed')
