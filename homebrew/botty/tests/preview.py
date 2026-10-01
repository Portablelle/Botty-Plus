#!/usr/bin/env python3
"""Local visual preview with sample torrents; never talks to a console."""
import json,pathlib,subprocess,tempfile,threading,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root_project=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='botty-preview-') as tmp:
 root=pathlib.Path(tmp);state=root/'transmission/state';state.mkdir(parents=True)
 (state/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password='1'*32)))
 class RPC(BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def do_POST(self):
   self.rfile.read(int(self.headers.get('Content-Length',0)))
   items=[dict(id=1,hashString='1'*40,name='Sample multipart archive',status=6,percentDone=1,leftUntilDone=0,totalSize=38752000000,downloadDir=str(root/'downloads/complete'),rateDownload=0,rateUpload=512000,error=0,errorString='',files=[dict(name='sample.rar',length=100,bytesCompleted=100)]),dict(id=2,hashString='2'*40,name='Homebrew collection',status=4,percentDone=.64,leftUntilDone=1000000,totalSize=2800000000,downloadDir=str(root/'downloads/complete'),rateDownload=7400000,rateUpload=140000,error=0,errorString='',files=[])]
   data=json.dumps(dict(result='success',arguments=dict(torrents=items))).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 rpc=ThreadingHTTPServer(('127.0.0.1',0),RPC);threading.Thread(target=rpc.serve_forever,daemon=True).start()
 process=subprocess.Popen([str(root_project/'build/botty-native'),'--root',str(root),'--ui',str(root_project/'ui'),'--port','8088','--rpc-port',str(rpc.server_port)])
 try:
  print('Preview: http://127.0.0.1:8088 — sample data only',flush=True)
  process.wait()
 except KeyboardInterrupt:pass
 finally:
  process.terminate();process.wait(timeout=5);rpc.shutdown()
