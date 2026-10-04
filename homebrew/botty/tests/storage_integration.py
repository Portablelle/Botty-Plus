"""Real service + synthetic RAR and SCGI, entirely isolated from mounted user disks."""
import base64,hashlib,json,pathlib,shutil,socket,subprocess,tempfile,time,urllib.request,urllib.error,threading
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from rtorrent_fixture import RtorrentFixture
from shadow_fixture import ShadowFixture
from make_fixtures import build, single
ROOT=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='botty-storage-http-') as tmp:
 base=pathlib.Path(tmp);root=base/'internal';usb=base/'usb';usb.mkdir();complete=root/'downloads/complete';complete.mkdir(parents=True)
 (root/'jobs').mkdir();(root/'compressor/output').mkdir(parents=True)
 compressed_id='c'*32;image=root/'compressor/output/PPSA12347.ffpfsc';image.write_bytes(b'compressed fixture');pathlib.Path(str(image)+'.vhash').write_bytes(b'hash fixture')
 rec=dict(jobId=compressed_id,status='ready',verified=False,originalKept=False,titleId='PPSA12347',output=str(image),source=str(root/'test-library/PPSA12347-app'))
 (root/'compressor/state.json').write_text(json.dumps(rec));(root/'compressor/games.json').write_text(json.dumps({compressed_id:rec}));(root/'compressor/enabled.json').write_text('{"mode":"library-1.2"}')
 (root/'jobs'/ (compressed_id+'.json')).write_text(json.dumps(dict(id=compressed_id,name='Compressed fixture',status='moved',destination=rec['source'],content=dict(kind='folder',titleId='PPSA12347',destination='PPSA12347-app'))))
 image_id='d'*32;published=root/'test-library/demo.exfat';published.parent.mkdir();published.write_bytes(b'image fixture')
 (root/'jobs'/(image_id+'.json')).write_text(json.dumps(dict(id=image_id,name='Published image',status='moved',destination=str(published),content=dict(kind='exfat',destination='demo.exfat'))))
 selected={'PPSA12347':str(image),'PPSA12348':str(published)}
 shadow=ShadowFixture(selected)
 fixtures=build(base/'fixtures');shutil.copy(fixtures/'app.rar',complete/'app.rar');size=(complete/'app.rar').stat().st_size
 entries=[dict(id=1,hashString='1'*40,name='app.rar',status=6,leftUntilDone=0,totalSize=size,downloadDir=str(complete),files=[dict(name='app.rar',length=size,bytesCompleted=size)])]
 def hook(method,params):
  if method in ('load.start','load.normal'):
   assert len(params)==3 and params[2].startswith('d.directory.set="')
   directory=params[2].split('"')[1];assert pathlib.Path(directory).is_dir()
   hash=params[1].split('btih:')[1] if params[1].startswith('magnet:') else pathlib.Path(params[1]).stem;n=len(entries)+1
   entries.append(dict(id=n,hashString=hash,name='new.rar',status=4,leftUntilDone=123,totalSize=123,downloadDir=directory,files=[]));return 0
  return NotImplemented
 rpc=RtorrentFixture(entries,hook)
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 origin=f'http://127.0.0.1:{port}';token=''
 def request(path,body=None,expected=200):
  req=urllib.request.Request(origin+path,data=None if body is None else json.dumps(body).encode(),headers={'X-Botty-Token':token,'Content-Type':'application/json'})
  try:r=urllib.request.urlopen(req,timeout=10)
  except urllib.error.HTTPError as e:r=e
  data=json.loads(r.read());assert r.status==expected,(path,r.status,data);return data
 def until(predicate):
  for _ in range(250):
   state=request('/api/state')
   if predicate(state):return state
   time.sleep(.05)
  raise AssertionError(state)
 def transfer(body):
  request('/api/transfer',body,202);state=until(lambda s:s['transfer']['status']!='running');assert state['transfer']['status']=='complete',state['transfer'];tasks=request('/api/processing')['tasks'];assert any(t['kind']=='transfer' and t['status']=='completed' for t in tasks);return state
 proc=subprocess.Popen([str(ROOT/'build/botty-native'),'--root',str(root),'--external',str(usb),'--ui',str(ROOT/'ui'),'--port',str(port),'--rpc-port',str(rpc.server_port),'--shadow-port',str(shadow.port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 try:
  for _ in range(100):
   try:token=request('/api/bootstrap')['token'];break
   except urllib.error.URLError:time.sleep(.05)
  state=request('/api/state');external=state['storage'][1]['id'];assert state['storageSupported'] and state['storage'][1]['freeBytes']>0
  # Explicit download-only and full-auto enrollment, per-torrent directory.
  for digit,automatic,storage in [('2',False,external),('3',True,external),('4',False,'internal')]:
   request('/api/torrent',dict(action='add',magnet='magnet:?xt=urn:btih:'+digit*40,storage=storage,automatic=automatic))
   task=json.loads((root/'automatic'/ (digit*40+'.json')).read_text());assert task['storage']==storage and (task['status']=='waiting')==automatic
  # Uploaded metadata follows the same storage, enrollment and catalog pipeline.
  info=b'd6:lengthi123e4:name10:upload.rar12:piece lengthi16384e6:pieces20:12345678901234567890e'
  metadata=b'd4:info'+info+b'e';encoded=base64.b64encode(metadata).decode();uploaded_hash=hashlib.sha1(info).hexdigest()
  request('/api/torrent',dict(action='add',metainfo=encoded,storage=external,automatic=False))
  uploaded=next(t for t in request('/api/state')['torrents'] if t['hashString']==uploaded_hash)
  assert uploaded['downloadDir']==str(usb/'botty/downloads/complete') and uploaded['status']!=0
  assert (root/'rtorrent/state/incoming'/ (uploaded_hash+'.torrent')).read_bytes()==metadata
  task=json.loads((root/'automatic'/ (uploaded_hash+'.json')).read_text());assert task['storage']==external and task['status']!='waiting'
  request('/api/torrent',dict(action='add',metainfo=base64.b64encode(b'invalid').decode()),400)
  request('/api/torrent',dict(action='add',metainfo=encoded,magnet='magnet:?xt=urn:btih:'+'5'*40),400)
  assert entries[1]['downloadDir']==str(usb/'botty/downloads/complete')
  auto=usb/'botty/downloads/complete/auto.rar';single(auto,[('Auto/sce_sys/param.json',b'{"titleId":"PPSA12346"}'),('Auto/eboot.bin',b'auto game')]);n=auto.stat().st_size
  entries[2].update(name='auto.rar',status=6,leftUntilDone=0,totalSize=n,files=[dict(name='auto.rar',length=n,bytesCompleted=n)])
  autoState=until(lambda s:any(j.get('automatic') and j['status']=='moved' for j in s['jobs']))
  autoJob=next(j for j in autoState['jobs'] if j.get('automatic'));assert autoJob['storage']==external and autoJob['destination'].startswith(str(usb/'homebrew'))
  assert auto.exists() and not (root/'test-library/PPSA12346-app').exists()
  transfer(dict(kind='torrent',id=1,storage=external));assert not (complete/'app.rar').exists();assert (usb/'botty/downloads/complete/app.rar').read_bytes()==(fixtures/'app.rar').read_bytes();assert entries[0]['status']==0
  # Extract directly to external; then relocate verified extraction back and forth.
  job=request('/api/extract',dict(id=1,archive='app.rar',storage=external),202)
  state=until(lambda s:next(j for j in s['jobs'] if j['id']==job['id'])['status']!='extracting');job=next(j for j in state['jobs'] if j['id']==job['id']);assert job['status']=='ready',job
  assert (usb/'botty/extracted'/job['id']).is_dir() and not (root/'extracted'/job['id']).exists()
  transfer(dict(kind='job',id=job['id'],storage='internal'));assert (root/'extracted'/job['id']).exists();assert not (usb/'botty/extracted'/job['id']).exists()
  transfer(dict(kind='job',id=job['id'],storage=external))
  request('/api/move',dict(id=job['id'],storage=external),202);state=until(lambda s:s['transfer']['status']!='running');assert state['transfer']['status']=='complete',state['transfer'];assert next(j for j in state['jobs'] if j['id']==job['id'])['destination'].startswith(str(usb/'homebrew'))
  assert (usb/'botty/downloads/complete/app.rar').exists()
  # Published folders and verified compressed images use ShadowMount selection.
  transfer(dict(kind='job',id=job['id'],storage='internal'));assert (root/'test-library'/job['content']['destination']).exists()
  transfer(dict(kind='job',id=job['id'],storage=external))
  transfer(dict(kind='job',id=compressed_id,storage=external));external_image=usb/'botty/compressor/output/PPSA12347.ffpfsc';assert external_image.read_bytes()==b'compressed fixture' and not image.exists()
  transfer(dict(kind='job',id=compressed_id,storage='internal'));assert image.read_bytes()==b'compressed fixture' and pathlib.Path(str(image)+'.vhash').read_bytes()==b'hash fixture' and not external_image.exists()
  transfer(dict(kind='job',id=image_id,storage=external));assert (usb/'homebrew/demo.exfat').read_bytes()==b'image fixture' and not published.exists()
  transfer(dict(kind='job',id=image_id,storage='internal'));assert published.read_bytes()==b'image fixture'
  # Compressed deletion uses the same worker and exposes live progress.
  shadow.delay=1
  req_body=dict(id=compressed_id,confirmed=True)
  request('/api/delete-library-game',req_body,202)
  measured=False
  for _ in range(150):
   tasks=request('/api/processing')['tasks']
   measured=measured or any(t.get('remoteJobId') and t.get('total',0)>0 and t['status']=='deleting-game' for t in tasks)
   state=request('/api/state')
   if not any(j['id']==compressed_id for j in state['jobs']):break
   time.sleep(.05)
  assert measured and not image.exists() and not pathlib.Path(str(image)+'.vhash').exists()
  # The native UI can stay connected throughout remote deletion.
  shadow.delay=.8
  request('/api/delete-library-game',dict(id=job['id'],confirmed=True),202)
  live=request('/api/processing')['tasks'];assert any(t['kind']=='deletion' and t['status']=='running' for t in live)
  until(lambda s:not any(j['id']==job['id'] for j in s['jobs']))
  assert (usb/'botty/downloads/complete/app.rar').exists()
  assert sum(route.endswith('/games/delete') for route,_ in shadow.calls)==2
  # No missing-disk fallback, even if its old mount directory is replaced.
  usb.rename(base/'removed');request('/api/torrent',dict(action='add',magnet='magnet:?xt=urn:btih:'+'5'*40,storage=external),400);assert not usb.exists()
  usb.mkdir();request('/api/extract',dict(id=1,archive='app.rar',storage=external),400);assert not (usb/'botty').exists()
  rpc.assert_clean()
  print('External HTTP pipeline passed: selection, modes, verified archive/extraction transfers, publication and disk loss')
 finally:
  proc.terminate();proc.wait(timeout=5);rpc.shutdown();shadow.close()
