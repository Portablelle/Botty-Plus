"""Native command -> authenticated real service -> bounded mock compressor."""
import json,subprocess,tempfile,time,threading,socket,urllib.request,urllib.error
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from urllib.parse import urlsplit,parse_qs
ROOT=Path(__file__).resolve().parents[1]
def port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
with tempfile.TemporaryDirectory() as temp:
 root=Path(temp);source=root/'test-library/PPSA23732-app';(source/'sce_sys').mkdir(parents=True)
 (source/'sce_sys/param.json').write_text('{"titleId":"PPSA23732"}')
 (source/'eboot.bin').write_bytes(b'original')
 (root/'jobs').mkdir();jobid='a'*32
 (root/'jobs'/f'{jobid}.json').write_text(json.dumps(dict(id=jobid,name='LEGO',status='moved',destination=str(source),content=dict(kind='folder',titleId='PPSA23732',destination='PPSA23732-app'))))
 (root/'compressor').mkdir();(root/'compressor/enabled.json').write_text('{"mode":"library-1.2"}');(root/'compressor/token').write_text('a'*64)
 started=False;done=False;cancelled=False;epoch=int(time.time());calls=[]
 class Worker(BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def do_GET(self):self.handle_api(False)
  def do_POST(self):self.handle_api(True)
  def handle_api(self,post):
   global started,cancelled
   u=urlsplit(self.path);q=parse_qs(u.query);assert q['token']==['a'*64];calls.append((u.path,post))
   b=dict(ok=True)
   if u.path=='/api/status':b['bottyWorker']='library-1.2'
   elif u.path=='/api/gc/compress':
    assert post and q['sourcePath']==[str(source)] and q['deletePolicy']==['keep'] and q['format']==['exfat'];started=True;b.update(id='op-1',status='running')
   elif u.path=='/api/gc/history':b['history']=[dict(id='op-1',sourcePath=str(source),createdAt=epoch,status='success' if done else 'running',result='copy-created-unverified' if done else '',outputPath=str(root/'copy.ffpfsc'),compressedSize=3)]
   elif u.path=='/api/gc/job/cancel':cancelled=True
   elif u.path=='/api/gc/job':b.update(busy=started and not done,activeId='op-1' if started else '',phase='compressing',copiedBytes=2,totalBytes=8)
   data=json.dumps(b).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 worker=ThreadingHTTPServer(('127.0.0.1',0),Worker);threading.Thread(target=worker.serve_forever,daemon=True).start()
 service_port=port();proc=subprocess.Popen([str(ROOT/'build/botty-native'),'--root',str(root),'--port',str(service_port),'--rpc-port',str(port()),'--compressor-port',str(worker.server_port)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 token=''
 def req(path,body=None,status=200,auth=True):
  headers={'X-Botty-Token':token} if auth else {}
  r=urllib.request.Request(f'http://127.0.0.1:{service_port}'+path,data=None if body is None else json.dumps(body).encode(),headers=headers)
  try:response=urllib.request.urlopen(r,timeout=8)
  except urllib.error.HTTPError as e:response=e
  data=json.load(response);assert response.status==status,(path,response.status,data);return data
 try:
  for _ in range(100):
   try:token=req('/api/bootstrap',auth=False)['token'];break
   except OSError:time.sleep(.05)
  assert token
  req('/api/compress-game',dict(id=jobid,confirmed=True),403,False)
  req('/api/compress-game',dict(id=jobid),400)
  client=ROOT.parent/'botty-native/build/action-client'
  def native(op):
   r=subprocess.run([str(client),str(service_port),str(op),jobid],capture_output=True,text=True,timeout=15);assert r.returncode==0,r.stdout+r.stderr
  native(16);assert started
  req('/api/compress-game',dict(id=jobid,confirmed=True),400)
  req('/api/extract',dict(id=1,archive='a.rar'),400)
  req('/api/delete-library-game',dict(id=jobid,confirmed=True),400)
  req('/api/beta/shutdown',dict(confirmed=True),400)
  state=req('/api/state');assert state['compression']['busy'] and state['jobs'][0]['status']=='moved'
  native(17);assert cancelled
  done=True
  for _ in range(30):
   s=req('/api/state')['compression']
   if s['status']=='waiting-close':break
   time.sleep(.2)
  assert s['status']=='waiting-close' and s['busy'],s
  req('/api/delete-library-game',dict(id=jobid,confirmed=True),400)
  assert (source/'eboot.bin').read_bytes()==b'original'
  assert sum(p=='/api/gc/compress' for p,_ in calls)==1
  req('/api/beta/shutdown',dict(confirmed=True),400)
  assert ('/api/control/shutdown',True) not in calls
  print('Shutdown remains refused until activation finishes. Native/API compression, authentication, confirmation, concurrent file-operation guards, cancellation, progress and source preservation passed.')
 finally:proc.terminate();proc.wait(timeout=5);worker.shutdown()
