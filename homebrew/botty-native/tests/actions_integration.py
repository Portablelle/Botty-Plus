"""Native POST client -> real Botty HTTP service -> deterministic rTorrent SCGI fixture."""
import sys, importlib.util, pathlib, tempfile, shutil, subprocess, json, threading, time, urllib.request, socket, base64
ROOT=pathlib.Path(__file__).resolve().parents[1]
SERVICE=ROOT.parent/'botty'
sys.path.insert(0,str(SERVICE/'tests'))
from rtorrent_fixture import RtorrentFixture
from shadow_fixture import ShadowFixture
spec=importlib.util.spec_from_file_location('fixtures',SERVICE/'tests/make_fixtures.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)
with tempfile.TemporaryDirectory(prefix='botty-native-actions-') as directory:
 root=pathlib.Path(directory);fixtures.build(root/'fixtures')
 complete=root/'downloads/complete';complete.mkdir(parents=True)
 for name,source in [('sample.rar','app.rar'),('broken.rar','bad-crc.rar')]:shutil.copy(root/'fixtures'/source,complete/name)
 config=root/'rtorrent/state';config.mkdir(parents=True)
 (config/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password='TESTpw')))
 entries=[]
 for i,name in enumerate(['sample.rar','broken.rar'],1):
  size=(complete/name).stat().st_size
  entries.append(dict(id=i,name=name,hashString=str(i)*40,status=0,error=0,percentDone=1,leftUntilDone=0,totalSize=size,downloadDir=str(complete),files=[dict(name=name,length=size,bytesCompleted=size)]))
 rpc=RtorrentFixture(entries);calls=rpc.calls
 shadow=ShadowFixture({"PPSA12345":str(root/"test-library/PPSA12345-app")})
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 process=subprocess.Popen([str(SERVICE/'build/botty-native'),'--root',str(root),'--ui',str(SERVICE/'ui'),'--port',str(port),'--rpc-port',str(rpc.server_port),'--shadow-port',str(shadow.port)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 def get(path):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,headers={'X-Botty-Token':token})
  return json.load(urllib.request.urlopen(req,timeout=5))
 def action(op,id='',archive='',text='',success=True):
  result=subprocess.run([str(ROOT/'build/action-client'),str(port),str(op),str(id),archive,text],capture_output=True,text=True,timeout=20)
  assert (result.returncode==0)==success,(op,result.stdout,result.stderr)
  return result.stdout
 def job():
  for _ in range(100):
   jobs=get('/api/state')['jobs']
   if jobs and jobs[-1]['status']!='extracting':return jobs[-1]
   time.sleep(.05)
  raise AssertionError('Extraction did not finish')
 try:
  token=''
  for _ in range(100):
   try:token=get('/api/bootstrap')['token'];break
   except OSError:time.sleep(.05)
  assert token
  probe=subprocess.run([str(ROOT/'build/action-client'),str(port),'--probe'],capture_output=True,text=True,timeout=20)
  assert probe.returncode==0,(probe.stdout,probe.stderr) # Real /health -> bootstrap -> connection -> catalog.
  action(2,1);assert entries[0]['status']==6
  action(1,1);assert entries[0]['status']==0
  action(3,1);assert entries[0]['status']==2
  action(5,1,'sample.rar',success=False) # Verification blocks extraction.
  entries[0]['status']=6
  magnet='magnet:?xt=urn:btih:'+'a'*40+'&dn=Native%20fixture'
  action(4,text=magnet);assert next(c for c in calls if c['method']=='load.start')['params'][1]==magnet
  action(5,1,'sample.rar');ready=job();assert ready['status']=='ready'
  action(6,ready['id']);assert get('/api/state')['jobs'][0]['status']=='moved'
  action(7,ready['id'],success=False) # Moved library files are protected.
  assert (root/'test-library/PPSA12345-app/eboot.bin').is_file()
  action(5,2,'broken.rar');failed=job();assert failed['status']=='failed'
  action(7,failed['id']);assert all(j['id']!=failed['id'] for j in get('/api/state')['jobs'])
  assert (complete/'sample.rar').is_file() and (complete/'broken.rar').is_file()
  action(15,ready['id'])
  assert get('/api/processing')['tasks'][0]['status']=='running'
  for _ in range(100):
   if get('/api/processing')['tasks'][0]['status']=='completed':break
   time.sleep(.05)
  assert not (root/'test-library/PPSA12345-app').exists()
  assert (complete/'sample.rar').is_file() and (complete/'broken.rar').is_file()
  assert not any(j['id']==ready['id'] for j in get('/api/state')['jobs'])
  assert sum(c['method']=='load.start' for c in calls)==1
  rpc.assert_clean()
  print('Native actions passed: pause/resume/verify/add, extraction, library move, protected deletion, failed-output deletion and source preservation.')
 finally:
  process.terminate();process.wait(timeout=5);rpc.shutdown();shadow.close()
