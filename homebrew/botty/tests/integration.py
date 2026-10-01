#!/usr/bin/env python3
"""Exercise the real native HTTP service + UnRAR with a local Transmission RPC fixture."""
import base64,json,pathlib,shutil,socket,subprocess,tempfile,threading,time,urllib.request,urllib.error
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from make_fixtures import build, single
ROOT=pathlib.Path(__file__).resolve().parents[1]
PASS='B7mQ2x'

def free_port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]

with tempfile.TemporaryDirectory(prefix='botty-integration-') as directory:
    root=pathlib.Path(directory);fixtures=build(root/'fixtures')
    complete=root/'downloads/complete';complete.mkdir(parents=True)
    incomplete=root/'downloads/incomplete';incomplete.mkdir()
    shutil.copy(fixtures/'app.rar',complete/'app.rar');shutil.copy(fixtures/'bad-crc.rar',complete/'bad.rar')
    single(complete/'cancel.rar',[('large.bin',b'cancel test data '*4194304)])
    config=root/'transmission/state';config.mkdir(parents=True)
    (config/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password=PASS)))
    entries=[]
    for i,name in enumerate(['app.rar','bad.rar','cancel.rar'],1):
        size=(complete/name).stat().st_size
        entries.append(dict(id=i,hashString=str(i)*40,name=name,status=6,percentDone=1,leftUntilDone=0,totalSize=size,downloadDir=str(complete),rateDownload=0,rateUpload=0,error=0,errorString='',files=[dict(name=name,length=size,bytesCompleted=size)]))
    entries[0].update(peersConnected=12,peersSendingToUs=7,peersGettingFromUs=3)
    entries[1].update(peersConnected=0,peersSendingToUs=0,peersGettingFromUs=0)
    entries[0]['eta']=120
    entries[0]['sizeWhenDone']=entries[0]['totalSize']
    refuse_stop=False
    sabotage_stop=False
    class RPC(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_POST(self):
            body=json.loads(self.rfile.read(int(self.headers.get('Content-Length',0))))
            if self.headers.get('Authorization')!='Basic '+base64.b64encode(('botty:'+PASS).encode()).decode():self.send_response(401);self.end_headers();return
            if self.headers.get('X-Transmission-Session-Id')!='test123':self.send_response(409);self.send_header('X-Transmission-Session-Id','test123');self.send_header('Content-Length','0');self.end_headers();return
            if body['method']=='torrent-get':
                assert {'eta','sizeWhenDone','peersConnected','peersSendingToUs','peersGettingFromUs'} <= set(body['arguments']['fields'])
            if body['method']=='torrent-remove':
                assert body['arguments']['delete-local-data'] is False
                removed=[t for t in entries if t['hashString'] in body['arguments']['ids']]
                assert len(removed)==1
                for t in removed:
                    # RPC acknowledges removal but does no filesystem work.
                    # Botty must have removed both completed and partial files.
                    assert t['status']==0
                    for file in t['files']:
                        for directory in [complete,incomplete]:
                            for suffix in ['', '.part']:assert not (directory/(file['name']+suffix)).exists()
                    entries.remove(t)
            if body['method']=='torrent-stop':
                for t in entries:
                    if t['id'] in body['arguments']['ids'] or t['hashString'] in body['arguments']['ids']:
                        if not refuse_stop:t['status']=0
                        if sabotage_stop:(incomplete/(t['files'][0]['name']+'.part')).mkdir()
            arguments={}
            if body['method']=='torrent-get':arguments=dict(torrents=entries)
            if body['method']=='session-get':arguments={'incomplete-dir-enabled':True,'incomplete-dir':str(incomplete)}
            output=json.dumps(dict(result='success',arguments=arguments)).encode()
            self.send_response(200);self.send_header('Content-Length',str(len(output)));self.end_headers();self.wfile.write(output)
    rpc=ThreadingHTTPServer(('127.0.0.1',0),RPC);threading.Thread(target=rpc.serve_forever,daemon=True).start()
    port=free_port();origin=f'http://127.0.0.1:{port}';process=None;token=''
    def request(path,body=None,headers=None,expected=200,auth=True):
        h={'Content-Type':'application/json'}
        if auth:h['X-Botty-Token']=token
        h.update(headers or {})
        req=urllib.request.Request(origin+path,data=None if body is None else json.dumps(body).encode(),headers=h)
        try:r=urllib.request.urlopen(req,timeout=10)
        except urllib.error.HTTPError as e:r=e
        data=r.read();assert r.status==expected,(path,r.status,data)
        return json.loads(data) if 'application/json' in r.headers.get('Content-Type','') else data.decode()
    def start():
        global process,token
        process=subprocess.Popen([str(ROOT/'build/botty-native'),'--root',str(root),'--ui',str(ROOT/'ui'),'--port',str(port),'--rpc-port',str(rpc.server_port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        for _ in range(100):
            if process.poll() is not None:raise RuntimeError(process.stderr.read().decode())
            try:
                token=request('/api/bootstrap',auth=False)['token'];return
            except (ConnectionError,urllib.error.URLError):time.sleep(.05)
        raise AssertionError('Service did not start')
    def stop():
        process.terminate();process.wait(timeout=5)
    def wait_job(job_id):
        for _ in range(100):
            job=next(j for j in request('/api/state')['jobs'] if j['id']==job_id)
            if job['status']!='extracting':return job
            time.sleep(.05)
        raise AssertionError('Extraction did not finish')
    try:
        start()
        assert request('/health')['app']=='Botty'
        assert request('/health')['apiVersion']==1
        assert 'password' not in request('/health')
        assert 'password' not in request('/api/bootstrap')
        request('/api/connections',auth=False,expected=403)
        request('/api/connections',headers={'Host':'untrusted.example'},expected=403)
        request('/api/connections',headers={'Origin':'https://untrusted.example'},expected=403)
        details=request('/api/connections')
        assert details['username']=='botty' and details['password']==PASS
        assert details['apiVersion']==1
        assert not details['url'] or details['url'].startswith('http://192.168.')
        PASS='1234567890abcdef1234567890abcdef'
        (config/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password=PASS)))
        assert request('/api/connections')['password']==PASS
        PASS='B7mQ2x'
        (config/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password=PASS)))
        assert '<title>Botty Downloads</title>' in request('/')
        request('/api/state',auth=False,expected=403)
        request('/api/state',headers={'Origin':'https://untrusted.example'},expected=403)
        request('/api/state',headers={'Host':'untrusted.example'},expected=403)
        request('/fs/etc/passwd',expected=404)
        state=request('/api/state');assert len(state['torrents'])==3 and state['transmissionReady']
        assert state['torrents'][0]['peersConnected']==12
        assert state['torrents'][0]['peersSendingToUs']==7 and state['torrents'][0]['peersGettingFromUs']==3
        assert state['torrents'][1]['peersConnected']==0
        assert state['torrents'][0]['eta']==120
        request('/api/torrent',{'action':'remove','id':1},expected=400)
        request('/api/torrent',{'action':'add','magnet':'http://unexpected.example'},expected=400)
        request('/api/torrent',{'action':'pause','id':1})
        entries[0]['status']=2
        request('/api/extract',{'id':1,'archive':'app.rar'},expected=400)
        entries[0]['status']=6
        entries[0]['files'][0]['bytesCompleted']-=1
        request('/api/extract',{'id':1,'archive':'app.rar'},expected=400)
        entries[0]['files'][0]['bytesCompleted']+=1
        request('/api/extract',{'id':1,'archive':'../app.rar'},expected=400)
        job=request('/api/extract',{'id':1,'archive':'app.rar'},expected=202)
        ready=wait_job(job['id']);assert ready['status']=='ready',ready
        request('/api/cancel-extraction',{'id':job['id']},expected=400)
        request('/api/dismiss-extraction',{'id':job['id']},auth=False,expected=403)
        request('/api/dismiss-extraction',{'id':job['id']})
        assert next(j for j in request('/api/state')['jobs'] if j['id']==job['id'])['dismissed']
        assert (root/'extracted'/job['id']/'Demo/eboot.bin').exists()
        moved=request('/api/move',{'id':job['id']});assert moved['status']=='moved'
        assert (root/'test-library/PPSA12345-app/eboot.bin').read_bytes()==b'original test bytes'*400
        assert (complete/'app.rar').exists()
        request('/api/delete-extraction',{'id':job['id']},expected=400)
        request('/api/extract',{'id':1,'archive':'app.rar'},expected=400)
        bad=request('/api/extract',{'id':2,'archive':'bad.rar'},expected=202)
        assert wait_job(bad['id'])['status']=='failed'
        request('/api/move',{'id':bad['id']},expected=400)
        request('/api/dismiss-extraction',{'id':bad['id']})
        assert not (root/'extracted'/(bad['id']+'.working')).exists()
        request('/api/delete-extraction',{'id':bad['id']})
        assert (complete/'bad.rar').exists()
        cancel=request('/api/extract',{'id':3,'archive':'cancel.rar'},expected=202)
        request('/api/dismiss-extraction',{'id':cancel['id']},expected=400)
        request('/api/cancel-extraction',{'id':cancel['id']},auth=False,expected=403)
        request('/api/torrent',{'action':'remove-data','id':3,'confirmed':True},expected=400)
        request('/api/cancel-extraction',{'id':cancel['id']},expected=202)
        stopped=wait_job(cancel['id']);assert stopped['status']=='cancelled',stopped
        request('/api/dismiss-extraction',{'id':cancel['id']})
        assert not (root/'extracted'/(cancel['id']+'.working')).exists()
        assert (complete/'cancel.rar').exists()
        stop()
        interrupted='a'*32
        (root/'jobs'/f'{interrupted}.json').write_text(json.dumps(dict(id=interrupted,status='extracting',name='Interrupted test')))
        partial=root/'extracted'/(interrupted+'.working');partial.mkdir();(partial/'partial.bin').write_bytes(b'partial')
        resume_id='b'*32
        shutil.copy(fixtures/'app.rar',complete/'resume.rar')
        resume_size=(complete/'resume.rar').stat().st_size
        entries.append(dict(entries[0],id=4,hashString='4'*40,name='resume.rar',files=[dict(name='resume.rar',length=resume_size,bytesCompleted=resume_size)]))
        stage=root/'extracted'/(resume_id+'.working');(stage/'Demo/sce_sys').mkdir(parents=True)
        (stage/'Demo/sce_sys/param.json').write_text(json.dumps({'titleId':'PPSA12345'}))
        (stage/'Demo/eboot.bin').write_bytes(b'incomplete')
        kept_time=(stage/'Demo/sce_sys/param.json').stat().st_mtime_ns
        (root/'jobs'/f'{resume_id}.json').write_text(json.dumps(dict(id=resume_id,status='extracting',name='Resume test',hash='4'*40,archive='resume.rar')))
        start()
        restored=next(j for j in request('/api/state')['jobs'] if j['id']==interrupted)
        assert restored['status']=='interrupted'
        request('/api/dismiss-extraction',{'id':interrupted});assert not partial.exists()
        assert (root/'test-library/PPSA12345-app/eboot.bin').exists()
        resumed=request('/api/extract',{'id':4,'archive':'resume.rar'},expected=202)
        assert resumed['id']==resume_id
        assert wait_job(resume_id)['status']=='ready'
        assert (root/'extracted'/resume_id/'Demo/sce_sys/param.json').stat().st_mtime_ns==kept_time
        assert (root/'extracted'/resume_id/'Demo/eboot.bin').read_bytes()==b'original test bytes'*400
        assert len([j for j in request('/api/state')['jobs'] if j['id']==resume_id])==1
        assert request('/api/state')['torrentRemovalSupported']
        request('/api/torrent',{'action':'remove-data','id':1},expected=400)
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True},auth=False,expected=403)
        original=entries[0]['downloadDir'];entries[0]['downloadDir']=str(root/'test-library')
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True},expected=400)
        entries[0]['downloadDir']=original
        original_name=entries[0]['files'][0]['name'];entries[0]['files'][0]['name']='../outside.rar'
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True},expected=400)
        entries[0]['files'][0]['name']='linked/eboot.bin';(complete/'linked').symlink_to(root/'test-library/PPSA12345-app')
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True},expected=400)
        (complete/'linked').unlink();entries[0]['files'][0]['name']=original_name
        other_name=entries[1]['files'][0]['name'];entries[1]['files'][0]['name']=original_name
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True},expected=400)
        assert (complete/'app.rar').exists()
        entries[1]['files'][0]['name']=other_name
        refuse_stop=True;entries[0]['status']=6
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True},expected=400)
        assert (complete/'app.rar').exists() and any(t['id']==1 for t in entries)
        refuse_stop=False;sabotage_stop=True
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True},expected=400)
        assert (complete/'app.rar').exists() and any(t['id']==1 and t['status']==0 for t in entries)
        sabotage_stop=False;(incomplete/'app.rar.part').rmdir()
        (incomplete/'app.rar.part').write_bytes(b'partial download')
        (incomplete/'unrelated.txt').write_text('keep')
        task=root/'automatic'/('1'*40+'.json');task.write_text(json.dumps({'hash':'1'*40,'status':'tracked'}))
        request('/api/torrent',{'action':'remove-data','id':1,'confirmed':True})
        assert not (complete/'app.rar').exists() and (complete/'bad.rar').exists()
        assert not (incomplete/'app.rar.part').exists() and (incomplete/'unrelated.txt').read_text()=='keep'
        assert (root/'test-library/PPSA12345-app/eboot.bin').read_bytes()==b'original test bytes'*400
        assert json.loads(task.read_text())['status']=='deletion-requested'
        assert all(t['id']!=1 for t in request('/api/state')['torrents'])
        assert any(j['status']=='moved' for j in request('/api/state')['jobs'])
        assert request('/api/state')['libraryDeletionSupported']
        request('/api/delete-library-game',{'id':job['id'],'confirmed':True},auth=False,expected=403)
        request('/api/delete-library-game',{'id':job['id']},expected=400)
        assert (root/'test-library/PPSA12345-app/eboot.bin').exists()
        request('/api/delete-library-game',{'id':job['id'],'confirmed':True})
        assert not (root/'test-library/PPSA12345-app').exists()
        assert (complete/'bad.rar').exists() and (complete/'resume.rar').exists()
        assert not any(j['id']==job['id'] for j in request('/api/state')['jobs'])
        print('HTTP integration passed: local access controls, real RPC handshake, download completeness, real extraction/CRC, library moves, source preservation, cleanup and crash recovery.')
    finally:
        if process and process.poll() is None:stop()
        rpc.shutdown();rpc.server_close()
