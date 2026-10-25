#!/usr/bin/env python3
"""Exercise authenticated Eden inventory/import using a real host manager."""
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import zipfile
import urllib.error
import urllib.request
from rtorrent_fixture import RtorrentFixture
from switch_fixtures import package,xci

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='botty-emulation-') as folder:
    root = Path(folder)
    complete = root / 'downloads/complete'
    complete.mkdir(parents=True)
    game = xci()
    update = package('update',65536)
    (complete / 'Game.xci').write_bytes(game)
    (complete / 'Update.nsp').write_bytes(update)
    (root / 'eden/roms').mkdir(parents=True)
    credentials = root / 'rtorrent/state'
    credentials.mkdir(parents=True)
    (credentials / 'botty-credentials.json').write_text(json.dumps(dict(username='botty', password='fixture')))
    entries = []
    for index, name in enumerate(('Game.xci', 'Update.nsp'), 1):
        size = (complete / name).stat().st_size
        entries.append(dict(id=index, hashString=str(index)*40, name=name, status=6, percentDone=1,
                            leftUntilDone=0, totalSize=size, downloadDir=str(complete), rateDownload=0,
                            rateUpload=0, error=0, errorString='',
                            files=[dict(name=name, length=size, bytesCompleted=size)]))
    rpc = RtorrentFixture(entries)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    process = subprocess.Popen([str(ROOT/'build/botty-native'), '--root', str(root), '--ui', str(ROOT/'ui'),
                                '--port', str(port), '--rpc-port', str(rpc.server_port)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    token = ''
    def request(path, body=None, status=200, authenticated=True, deadline=None):
        headers = {'Content-Type': 'application/json'}
        if authenticated:
            headers['X-Botty-Token'] = token
        req = urllib.request.Request(f'http://127.0.0.1:{port}'+path,
                                     data=None if body is None else json.dumps(body).encode(), headers=headers)
        try:
            response = urllib.request.urlopen(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        value = json.loads(response.read())
        if status==202 and response.status==400 and 'Finalizing Switch download' in str(value):
            deadline=deadline or time.monotonic()+10
            assert time.monotonic()<deadline,value
            time.sleep(.25)
            return request(path,body,status,authenticated,deadline)
        assert response.status == status, (response.status, value)
        return value
    try:
        for _ in range(600):
            try:
                token = request('/api/bootstrap', authenticated=False)['token']
                break
            except (ConnectionError, urllib.error.URLError):
                assert process.poll() is None
                time.sleep(.05)
        assert token
        state = request('/api/state')['emulation']
        assert len(state['entries']) == 2
        by_name = {e['name']: e for e in state['entries']}
        request('/api/emulation/import', {'id': by_name['Game']['id'], 'kind': 'auto'}, 403, False)
        request('/api/emulation/import', {'id': by_name['Game']['id'], 'kind': '../keys'}, 400)
        for name, kind, expected in [('Game', 'roms', game), ('Update', 'updates', update)]:
            entry = by_name[name]
            request('/api/emulation/import', {'id': entry['id'], 'kind': 'auto'}, 202) if kind=='roms' else request('/api/extract', {'id': 2}, 202)
            for _ in range(600):
                snapshot = request('/api/state')
                if not snapshot['extracting']:
                    break
                time.sleep(.05)
            assert snapshot['jobs'][-1]['status'] == 'moved', snapshot['jobs'][-1]
            refusal=request('/api/transfer',{'kind':'job','id':snapshot['jobs'][-1]['id'],'storage':'internal'},400)
            assert 'Switch content' in refusal['error'],refusal
            filename = Path(entry['path']).name
            assert next((root/'eden'/kind).rglob('*.xci' if kind=='roms' else '*.nsp')).read_bytes() == expected
            assert (complete/filename).read_bytes() == expected
            request('/api/emulation/import', {'id': entry['id'], 'kind': 'auto'}, 400)
        # A cancelled publication remains recoverable across a missing staging disk,
        # and a later restart retries the same durable receipt rather than a new job.
        process.terminate();process.wait(timeout=5)
        jobfile=next(p for p in (root/'jobs').glob('*.json') if json.loads(p.read_text()).get('torrentId')==1)
        job=json.loads(jobfile.read_text());job.update(status='cancelled',publicationPending=True);jobfile.write_text(json.dumps(job))
        record=root/'eden/imports'/(job['id']+'.json');r=json.loads(record.read_text());r['status']='pending';record.write_text(json.dumps(r))
        stage=root/'eden/.botty-preparation'/(job['id']+'.working');hold=stage.with_suffix('.unavailable');stage.rename(hold)
        def restart():
            global process,token
            process=subprocess.Popen([str(ROOT/'build/botty-native'),'--root',str(root),'--ui',str(ROOT/'ui'),'--port',str(port),'--rpc-port',str(rpc.server_port)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            for _ in range(600):
                try:token=request('/api/bootstrap',authenticated=False)['token'];return
                except (ConnectionError,urllib.error.URLError):
                    assert process.poll() is None;time.sleep(.05)
            raise AssertionError('Manager did not restart')
        restart();snapshot=request('/api/state')
        assert next(j for j in snapshot['jobs'] if j['id']==job['id'])['status']=='interrupted'
        request('/api/extract', {'id':1}, 400)
        request('/api/delete-extraction', {'id':job['id']}, 400)
        request('/api/dismiss-extraction', {'id':job['id']}, 400)
        assert jobfile.exists() and record.exists()
        process.terminate();process.wait(timeout=5);hold.rename(stage);restart();snapshot=request('/api/state')
        recovered=next(j for j in snapshot['jobs'] if j['id']==job['id'])
        assert recovered['status']=='moved' and not recovered['error'],recovered
        request('/api/emulation/import', {'id': by_name['Game']['id'], 'kind': 'roms'}, 400)
        request('/api/emulation/launch', {'id': by_name['Game']['id']}, 400)
        installed = request('/api/state')['emulation']['entries']
        update_entry = next(e for e in installed if e['kind'] == 'updates')
        request('/api/emulation/launch', {'id': update_entry['id']}, 400)
        game_entry = next(e for e in installed if e['kind'] == 'roms')
        request('/api/emulation/launch', {'id': game_entry['id']}, 400)
        (root/'eden-app/emulation').mkdir(parents=True)
        (root/'eden-app/emulation/eboot.bin').write_bytes(b'fixture')
        (root/'eden-app/emulation-runtime.json').write_text('{}')
        (root/'eden/keys').mkdir()
        (root/'eden/firmware').mkdir()
        (root/'eden/keys/prod.keys').write_text('synthetic fixture')
        (root/'eden/firmware/test.nca').write_text('synthetic fixture')
        request('/api/emulation/launch', {'id': game_entry['id']}, 400)
        (root/'eden/private-runtime.json').write_text('{"schema":1}')
        launch = request('/api/emulation/launch', {'id': game_entry['id']})
        assert launch['engine'] == 'embedded-eden'
        assert launch['launchPath'] == game_entry['path']
        # Real authenticated shortcut publication, collision handling and idempotency.
        import hashlib, struct, zlib
        (root/'homebrew').mkdir()
        (root/'homebrew/PPSA99270').mkdir() # unrelated homebrew must stay intact
        (root/'homebrew/PPSA99270/keep').write_text('unrelated')
        (root/'appmeta/PPSA99271').mkdir(parents=True) # stale registration still reserves its ID
        (root/'eden-app/sce_sys').mkdir()
        # Complete minimal PNG used when the synthetic game has no Control icon.
        def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        picture=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',512,512,8,2,0,0,0))+chunk(b'IDAT',zlib.compress((b'\0'+bytes([20,40,60])*512)*512))+chunk(b'IEND',b'')
        (root/'eden-app/sce_sys/icon0.png').write_bytes(picture)
        request('/api/emulation/home',{'id':game_entry['id']},403,False)
        request('/api/emulation/home',{'id':update_entry['id']},400)
        request('/api/emulation/home',{'id':'../escape'},400)
        request('/api/emulation/home',{'id':game_entry['id'],'path':'/tmp/game.xci'},400)
        tile=request('/api/emulation/home',{'id':game_entry['id']})
        assert tile['titleId']=='PPSA99272' and not tile['existing'],tile
        folder=root/'homebrew'/tile['titleId']
        forward=json.loads((folder/'forwarder.json').read_text())
        assert forward['target']=='PPSA99071' and forward['args']==['--botty-home-game',game_entry['id']]
        assert (folder/'eboot.bin').read_bytes()[:4]==bytes.fromhex('4f153d1d')
        assert (folder/'sce_module/libc.prx').read_bytes()[:4]==bytes.fromhex('5414f5ee')
        assert (folder/'eboot.bin').stat().st_mode&0o777==0o755
        assert (folder/'sce_sys/icon0.png').read_bytes()==picture
        param=json.loads((folder/'sce_sys/param.json').read_text())
        assert param['titleId']==tile['titleId'] and param['downloadDataSize']==256
        before={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*') if p.is_file()}
        assert request('/api/emulation/home',{'id':game_entry['id']})['existing']
        assert before=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*') if p.is_file()}
        assert not list((root/'homebrew').glob('.botty-home-*'))
        assert (root/'homebrew/PPSA99270/keep').read_text()=='unrelated'
        state=request('/api/state')['emulation']
        assert state['homeScreenSupported'] and next(e for e in state['entries'] if e['id']==game_entry['id'])['homeTitleId']==tile['titleId']
        # Missing game/disconnected disk refuses launch instead of opening another game.
        saved=Path(game_entry['path']);saved.rename(saved.with_suffix('.held'))
        request('/api/emulation/launch',{'id':game_entry['id']},400)
        saved.with_suffix('.held').rename(saved)
        legacy = root/'legacy-eden/roms/Existing.xci'
        legacy.parent.mkdir(parents=True)
        legacy.write_bytes(game)
        available = request('/api/state')['emulation']['entries']
        old_game = next(e for e in available if e.get('source') == 'existing-ps5')
        request('/api/emulation/import', {'id': old_game['id'], 'kind': 'auto'}, 202)
        for _ in range(600):
            snapshot = request('/api/state')
            if not snapshot['extracting']:
                break
            time.sleep(.05)
        assert snapshot['jobs'][-1]['status'] == 'moved', snapshot['jobs'][-1]
        assert len(list((root/'eden/roms').rglob('*.xci')))==2
        assert legacy.read_bytes() == game
        # A completed Switch archive follows the same durable automatic queue
        # as a real download, without a UI import request.
        archive=complete/'Auto.NSW.zip'
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
            z.writestr('Game.nsp',package(base=0x0100000000010000))
            z.writestr('Update.nsp',package('update',65536,base=0x0100000000010000))
            z.writestr('DLC.nsp',package('dlc',base=0x0100000000010000))
        size=archive.stat().st_size;h='3'*40
        entries.append(dict(id=3,hashString=h,name=archive.name,status=6,leftUntilDone=0,totalSize=size,downloadDir=str(complete),error=0,files=[dict(name=archive.name,length=size,bytesCompleted=size)]))
        (root/'automatic'/f'{h}.json').write_text(json.dumps(dict(torrentId=3,hash=h,status='waiting',storage='internal',platform='switch')))
        for _ in range(600):
            snapshot=request('/api/state')
            task=next((j for j in snapshot['jobs'] if j.get('hash')==h),None)
            if task and task['status'] not in ('extracting','moving'):break
            time.sleep(.05)
        assert task and task['status']=='moved',task
        identified=[e for e in snapshot['emulation']['entries'] if e.get('metadata',{}).get('baseTitleId')=='0100000000010000']
        assert {e['metadata']['type'] for e in identified}=={'game','update','dlc'},identified
        assert archive.exists()
        # Web Full's default PS5 task must infer raw Switch content.
        raw=complete/'AutoRaw.nsp';raw.write_bytes(package(base=0x0100000000020000));size=raw.stat().st_size;h='4'*40
        entries.append(dict(id=4,hashString=h,name=raw.name,status=6,leftUntilDone=0,totalSize=size,downloadDir=str(complete),error=0,files=[dict(name=raw.name,length=size,bytesCompleted=size)]))
        automatic=root/'automatic'/f'{h}.json'
        automatic.write_text(json.dumps(dict(torrentId=4,hash=h,status='waiting',storage='internal',platform='ps5')))
        for _ in range(600):
            snapshot=request('/api/state')
            task=next((j for j in snapshot['jobs'] if j.get('hash')==h),None)
            if task and task['status'] not in ('extracting','moving'):break
            time.sleep(.05)
        assert task and task['status']=='moved',task
        assert json.loads(automatic.read_text())['platform']=='switch'
        # A missing published game must be restorable from its original download.
        missing=Path(game_entry['path']);missing.unlink()
        request('/api/extract',{'id':1},202)
        for _ in range(600):
            snapshot=request('/api/state')
            if not snapshot['extracting']:break
            time.sleep(.05)
        assert snapshot['jobs'][-1]['status']=='moved',snapshot['jobs'][-1]
        assert any(Path(e['path']).read_bytes()==game for e in snapshot['emulation']['entries'] if e['kind']=='roms')
        print('Eden inventory, HTTP imports, durable automatic ZIP queue, game/update/DLC, source preservation and authorization PASS')
    finally:
        process.terminate()
        process.wait(timeout=5)
        rpc.shutdown()
        rpc.server_close()
