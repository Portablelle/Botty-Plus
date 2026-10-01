"""HTTPS Prowlarr fixture -> Botty -> Transmission -> verified RAR -> Library."""
import base64, importlib.util, json, pathlib, socket, ssl, subprocess, tempfile, threading, time, urllib.request, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fixtures', HERE/'make_fixtures.py')
fixtures = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixtures)
with tempfile.TemporaryDirectory(prefix='botty-search-') as temp:
 root=pathlib.Path(temp); fixtures.build(root/'fixtures')
 complete=root/'downloads/complete'; complete.mkdir(parents=True)
 (complete/'sample.rar').write_bytes((root/'fixtures/app.rar').read_bytes())
 size=(complete/'sample.rar').stat().st_size
 torrent=dict(id=1,hashString='a'*40,name='Original homebrew fixture',status=4,error=0,leftUntilDone=size,totalSize=size,downloadDir=str(complete),files=[dict(name='sample.rar',length=size,bytesCompleted=0)])
 calls=[]; queries=[]; cover_calls=[]
 class RPC(BaseHTTPRequestHandler):
  def log_message(self,*args): pass
  def do_POST(self):
   request=json.loads(self.rfile.read(int(self.headers['Content-Length']))); calls.append(request)
   method=request['method']; args={}
   if method=='torrent-get': args={'torrents':[torrent]}
   if method=='torrent-add':
    assert base64.b64decode(request['arguments']['metainfo'])==b'd4:infod4:name7:fixtureee'
    assert request['arguments']['download-dir']==str(complete)
    args={'torrent-added':{'id':1,'hashString':'a'*40}}
   self.reply({'result':'success','arguments':args})
  def reply(self,obj):
   data=json.dumps(obj).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 class Indexer(RPC):
  def do_GET(self):
   assert self.headers.get('X-Api-Key')=='b'*32
   url=urllib.parse.urlsplit(self.path)
   if url.path=='/api/v1/search':
    q=urllib.parse.parse_qs(url.query);queries.append(q)
    row=dict(title='Original homebrew PS5 fixture',indexerId=1,protocol='torrent',size=size,seeders=4,leechers=1,categories=[{'id':1080}],downloadUrl='http://127.0.0.1:9696/1/download?apikey='+('b'*32)+'&link=fixture')
    if q['query']==['PS5']:
     ident=int(q['indexerIds'][0]);row.update(indexerId=ident,downloadUrl=f'http://localhost/{ident}/download?link=fixture')
     self.reply([dict(row,title='Demo PS5',seeders=10,grabs=3,publishDate='2026-09-01T00:00:00Z'),dict(row,title='Second PS5',seeders=2,grabs=50,publishDate='2026-10-01T00:00:00Z'),dict(row,title='Original homebrew fixture PS5'),dict(row,title='Installed Game PS5'),dict(row,title='Wrong PS4'),dict(row,title='False XPS5suffix'),dict(row,title='Foreign PS5',indexerId=99)])
    else:self.reply([dict(row,indexerId=2),dict(row,categories=[{'id':2000}]),row])
   elif url.path=='/artwork':
    cover_calls.append(self.path)
    data=bytes([12,80,160])*(160*240);self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
   else:
    assert url.path in ['/1/download','/2/download','/3/download'] and 'apikey' not in urllib.parse.parse_qs(url.query)
    data=b'd4:infod4:name7:fixtureee';self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 rpc=ThreadingHTTPServer(('127.0.0.1',0),RPC);indexer=ThreadingHTTPServer(('127.0.0.1',0),Indexer)
 cert=root/'cert.pem';key=root/'key.pem'
 subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(cert,key);indexer.socket=ctx.wrap_socket(indexer.socket,server_side=True)
 for server in [rpc,indexer]:threading.Thread(target=server.serve_forever,daemon=True).start()
 (root/'prowlarr.json').write_text(json.dumps(dict(url=f'https://localhost:{indexer.server_port}',apiKey='b'*32,caFile=str(cert),exploreIndexers={'seeders':2,'completed':3,'newest':1})))
 credentials=root/'transmission/state';credentials.mkdir(parents=True);(credentials/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password='TESTpw')))
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 command=[str(HERE.parent/'build/botty-native'),'--root',str(root),'--port',str(port),'--rpc-port',str(rpc.server_port),'--ui',str(HERE.parent/'ui')]
 proc=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);token=''
 def request(path,data=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,headers={'X-Botty-Token':token,'Content-Type':'application/json'},data=json.dumps(data).encode() if data is not None else None)
  return json.load(urllib.request.urlopen(req,timeout=5))
 def until(fn):
  for _ in range(150):
   try:
    value=fn()
    if value:return value
   except OSError:pass
   time.sleep(.1)
  raise AssertionError('Timed out')
 try:
  token=until(lambda:request('/api/bootstrap').get('token'))
  request('/api/search',{'query':'Homebrew & demo','categories':[2000],'indexerIds':[2]})
  state=until(lambda:(s if not s['search']['busy'] else None) if (s:=request('/api/state')) else None)
  results=state['search']['results'];assert len(results)==1 and 'download' not in results[0]
  assert queries[0]['categories']==['1080'] and queries[0]['indexerIds']==['1'] and queries[0]['query']==['Homebrew & demo']
  library=root/'test-library/installed/sce_sys';library.mkdir(parents=True)
  (library/'param.json').write_text(json.dumps({'localizedParameters':{'defaultLanguage':'en-US','en-US':{'titleName':'Installed Game'}}}))
  for sort,ident,first in [('seeders','2','Demo PS5'),('completed','3','Second PS5'),('newest','1','Second PS5')]:
   request('/api/explore',{'sort':sort})
   browse=until(lambda:(e if not e['busy'] else None) if (e:=request('/api/state')['explore']) else None)
   assert [r['name'] for r in browse['results']]==([first,'Second PS5'] if sort=='seeders' else [first,'Demo PS5']),browse
   assert queries[-1]['indexerIds']==[ident] and queries[-1]['categories']==['1080']
   assert request('/api/state')['search']['results']==results
  query_count=len(queries)
  request('/api/explore',{'sort':'newest'})
  cached=request('/api/state')['explore'];assert not cached['busy'] and cached['results']==browse['results']
  assert len(queries)==query_count
  request('/api/explore',{'sort':'newest','refresh':True})
  browse=until(lambda:(e if not e['busy'] else None) if (e:=request('/api/state')['explore']) else None)
  assert len(queries)==query_count+1 and browse['results']==cached['results']
  def artwork():
   req=urllib.request.Request(f'http://127.0.0.1:{port}/api/explore/artwork?id='+browse['results'][0]['id'],headers={'X-Botty-Token':token})
   with urllib.request.urlopen(req,timeout=2) as response:
    data=response.read();return data if response.status==200 else None
  assert until(artwork)==bytes([12,80,160])*(160*240)
  def catalog_artwork(ident):
   req=urllib.request.Request(f'http://127.0.0.1:{port}/api/artwork?id='+urllib.parse.quote(ident),headers={'X-Botty-Token':token})
   with urllib.request.urlopen(req,timeout=2) as response:
    data=response.read();return data if response.status==200 else None
  assert state['catalogArtworkSupported']
  assert until(lambda:catalog_artwork('t:1'))==bytes([12,80,160])*(160*240)
  assert until(lambda:catalog_artwork('s:'+results[0]['id']))==bytes([12,80,160])*(160*240)
  for invalid in ['t:999','j:unknown','https://example.com/image.jpg','../../outside']:
   try:catalog_artwork(invalid);raise AssertionError('Unknown artwork ID accepted')
   except urllib.error.HTTPError as e:assert e.code==404
  try:
   urllib.request.urlopen(f'http://127.0.0.1:{port}/api/artwork?id=t:1');raise AssertionError('Unauthenticated artwork accepted')
  except urllib.error.HTTPError as e:assert e.code==403
  try:request('/api/explore',{'sort':'invalid'});raise AssertionError('Invalid sort accepted')
  except urllib.error.HTTPError as e:assert e.code==400
  try: request('/api/search/add',{'id':'invalid'});raise AssertionError('Invalid ID accepted')
  except urllib.error.HTTPError as e:assert e.code==400
  selected=browse['results'][0]['id']
  request('/api/explore/add',{'id':selected})
  until(lambda:not request('/api/state')['explore']['adding'])
  assert selected not in [r['id'] for r in request('/api/state')['explore']['results']]
  request('/api/explore/add',{'id':selected})
  assert sum(c['method']=='torrent-add' for c in calls)==1
  assert not request('/api/state')['jobs']
  # Queue survives a service restart while the download is incomplete.
  proc.terminate();proc.wait(timeout=5);proc=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  token=until(lambda:request('/api/bootstrap').get('token'))
  query_count=len(queries);cover_count=len(cover_calls)
  request('/api/explore',{'sort':'newest'})
  assert not request('/api/state')['explore']['busy'] and len(queries)==query_count
  assert until(artwork)==bytes([12,80,160])*(160*240) and len(cover_calls)==cover_count
  assert (root/'cache/explore-newest.json').stat().st_mode & 0o777 == 0o600
  torrent.update(leftUntilDone=0,status=6);torrent['files'][0]['bytesCompleted']=size
  def installed():
   s=request('/api/state');return s if any(j['status']=='moved' for j in s['jobs']) else None
  state=until(installed)
  job_id=next(j['id'] for j in state['jobs'] if j['status']=='moved')
  assert until(lambda:catalog_artwork('j:'+job_id))==bytes([12,80,160])*(160*240)
  assert (root/'test-library/PPSA12345-app/eboot.bin').is_file()
  assert ((root/'test-library/PPSA12345-app/eboot.bin').stat().st_mode & 0o777)==0o755
  assert ((root/'test-library/PPSA12345-app/sce_sys/param.json').stat().st_mode & 0o777)==0o644
  assert (complete/'sample.rar').is_file()
  assert not any(j['status']=='failed' for j in state['jobs'])
  time.sleep(.2);assert len(request('/api/state')['jobs'])==1
  # A second automatic job must preserve an existing library destination.
  torrent['hashString']='c'*40
  (root/'automatic'/('c'*40+'.json')).write_text(json.dumps({'hash':'c'*40,'status':'waiting'}))
  def collision():
   return next((j for j in request('/api/state')['jobs'] if j.get('hash')=='c'*40 and j.get('phase')=='Needs attention'),None)
  conflict=until(collision);assert conflict['status']=='ready' and 'already exists' in conflict['error']
  assert (root/'test-library/PPSA12345-app/eboot.bin').is_file()
  # CRC failure must never publish an app or delete the source.
  (complete/'broken.rar').write_bytes((root/'fixtures/bad-crc.rar').read_bytes());badsize=(complete/'broken.rar').stat().st_size
  torrent.update(hashString='d'*40,files=[dict(name='broken.rar',length=badsize,bytesCompleted=badsize)])
  (root/'automatic'/('d'*40+'.json')).write_text(json.dumps({'hash':'d'*40,'status':'waiting'}))
  def failed():return next((j for j in request('/api/state')['jobs'] if j.get('hash')=='d'*40 and j['status']=='failed'),None)
  until(failed);assert (complete/'broken.rar').is_file()
  print('Search pipeline passed: HTTPS, fixed category/indexer, opaque IDs, duplicate prevention, durable queue, automatic extraction/publication and archive preservation.')
 finally:
  proc.terminate();proc.wait(timeout=5);rpc.shutdown();indexer.shutdown()
