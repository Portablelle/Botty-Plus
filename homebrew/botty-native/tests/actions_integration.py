"""Native POST client -> real Botty HTTP service -> deterministic Transmission fixture."""
import importlib.util, pathlib, tempfile, shutil, subprocess, json, threading, time, urllib.request, socket, base64
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
ROOT=pathlib.Path(__file__).resolve().parents[1]
SERVICE=ROOT.parent/'botty'
spec=importlib.util.spec_from_file_location('fixtures',SERVICE/'tests/make_fixtures.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)
with tempfile.TemporaryDirectory(prefix='botty-native-actions-') as directory:
 root=pathlib.Path(directory);fixtures.build(root/'fixtures')
 complete=root/'downloads/complete';complete.mkdir(parents=True)
 for name,source in [('sample.rar','app.rar'),('broken.rar','bad-crc.rar')]:shutil.copy(root/'fixtures'/source,complete/name)
 config=root/'transmission/state';config.mkdir(parents=True)
 (config/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password='TESTpw')))
 entries=[]
 for i,name in enumerate(['sample.rar','broken.rar'],1):
  size=(complete/name).stat().st_size
  entries.append(dict(id=i,name=name,hashString=str(i)*40,status=0,error=0,percentDone=1,leftUntilDone=0,totalSize=size,downloadDir=str(complete),files=[dict(name=name,length=size,bytesCompleted=size)]))
 calls=[]
 class RPC(BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def do_POST(self):
   data=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
   if self.headers.get('Authorization')!='Basic '+base64.b64encode(b'botty:TESTpw').decode():self.send_response(401);self.end_headers();return
   if self.headers.get('X-Transmission-Session-Id')!='fixture':
    self.send_response(409);self.send_header('X-Transmission-Session-Id','fixture');self.send_header('Content-Length','0');self.end_headers();return
   calls.append(data);method=data['method']
   if method in ['torrent-stop','torrent-start','torrent-verify']:
    for entry in entries:
     if entry['id'] in data['arguments']['ids']:entry['status']={'torrent-stop':0,'torrent-start':6,'torrent-verify':2}[method]
   result=dict(result='success',arguments=dict(torrents=entries) if method=='torrent-get' else {})
   body=json.dumps(result).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
 rpc=ThreadingHTTPServer(('127.0.0.1',0),RPC);threading.Thread(target=rpc.serve_forever,daemon=True).start()
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 process=subprocess.Popen([str(SERVICE/'build/botty-native'),'--root',str(root),'--ui',str(SERVICE/'ui'),'--port',str(port),'--rpc-port',str(rpc.server_port)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
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
  action(2,1);assert entries[0]['status']==6
  action(1,1);assert entries[0]['status']==0
  action(3,1);assert entries[0]['status']==2
  action(5,1,'sample.rar',success=False) # Verification blocks extraction.
  entries[0]['status']=6
  magnet='magnet:?xt=urn:btih:'+'a'*40+'&dn=Native%20fixture'
  action(4,text=magnet);assert next(c for c in calls if c['method']=='torrent-add')['arguments']['filename']==magnet
  action(5,1,'sample.rar');ready=job();assert ready['status']=='ready'
  action(6,ready['id']);assert get('/api/state')['jobs'][0]['status']=='moved'
  action(7,ready['id'],success=False) # Moved library files are protected.
  assert (root/'test-library/PPSA12345-app/eboot.bin').is_file()
  action(5,2,'broken.rar');failed=job();assert failed['status']=='failed'
  action(7,failed['id']);assert all(j['id']!=failed['id'] for j in get('/api/state')['jobs'])
  assert (complete/'sample.rar').is_file() and (complete/'broken.rar').is_file()
  assert sum(c['method']=='torrent-add' for c in calls)==1
  print('Native actions passed: pause/resume/verify/add, extraction, library move, protected deletion, failed-output deletion and source preservation.')
 finally:
  process.terminate();process.wait(timeout=5);rpc.shutdown()
