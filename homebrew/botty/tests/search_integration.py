"""HTTPS Prowlarr fixture -> Botty -> rTorrent -> verified RAR -> Library."""
import hashlib, importlib.util, json, pathlib, socket, ssl, subprocess, tempfile, threading, time, urllib.request, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from rtorrent_fixture import RtorrentFixture
HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fixtures', HERE/'make_fixtures.py')
fixtures = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixtures)
with tempfile.TemporaryDirectory(prefix='botty-search-') as temp:
 root=pathlib.Path(temp); fixtures.build(root/'fixtures')
 complete=root/'downloads/complete'; complete.mkdir(parents=True)
 (complete/'sample.rar').write_bytes((root/'fixtures/app.rar').read_bytes())
 size=(complete/'sample.rar').stat().st_size
 torrent=dict(id=1,hashString=hashlib.sha1(b'd4:name7:fixturee').hexdigest(),name='Original homebrew fixture',status=4,error=0,leftUntilDone=size,totalSize=size,downloadDir=str(complete),files=[dict(name='sample.rar',length=size,bytesCompleted=0)])
 calls=[]; queries=[]; cover_calls=[]
 refresh_started=threading.Event();refresh_release=threading.Event();delayed_refresh=False
 def rpc_hook(method, params):
  if method=='load.start':
   assert pathlib.Path(params[1]).read_bytes()==b'd4:infod4:name7:fixtureee'
   entries.append(torrent)
   return 0
  return NotImplemented
 entries=[dict(torrent,hashString='a'*40)]
 class Indexer(BaseHTTPRequestHandler):
  def log_message(self,*args): pass
  def reply(self,obj):
   if delayed_refresh and isinstance(obj,list):
    refresh_started.set();assert refresh_release.wait(10)
    obj=[r for r in obj if r.get('title')!='Demo PS5']
   data=json.dumps(obj).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
  def do_GET(self):
   assert self.headers.get('X-Api-Key')=='b'*32
   url=urllib.parse.urlsplit(self.path)
   if url.path=='/api/v1/search':
    q=urllib.parse.parse_qs(url.query);queries.append(q)
    row=dict(indexer='Tracker One',title='Original homebrew PS5 fixture',indexerId=1,protocol='torrent',size=size,seeders=4,leechers=1,categories=[{'id':1080}],downloadUrl='http://127.0.0.1:9696/1/download?apikey='+('b'*32)+'&link=fixture')
    assert q['indexerIds']==['-2'] and q['categories']==['1000']
    other=dict(row,indexer='Tracker Eight',indexerId=8,downloadUrl='https://untrusted.invalid/8/download?apikey='+('c'*32)+'&link=fixture',categories=[{'id':1180}])
    if q['query']==['PS5']:
     self.reply([dict(row,title='Demo PS5',seeders=10,grabs=3,publishDate='2026-09-01T00:00:00Z'),dict(row,title='Second PS5',seeders=2,grabs=50,publishDate='2026-10-01T00:00:00Z'),dict(other,title='Third PS5',seeders=8,grabs=90,publishDate='2026-10-02T00:00:00Z'),dict(other,title='Demo PS5',seeders=9,grabs=5),dict(other,title='Demo PS5 Deluxe',size=size*2,seeders=7,grabs=12,downloadUrl='https://untrusted.invalid/8/download?link=deluxe'),dict(other,title='Null counts PS5',seeders=None,grabs=None,infoHash=None,publishDate=None),dict(other,title='Malformed PS5',indexerId='oops'),dict(row,title='Original homebrew fixture PS5'),dict(row,title='Installed Game PS5'),dict(row,title='Wrong PS4'),dict(row,title='False XPS5suffix'),dict(row,title='Foreign PS5',indexerId=99),dict(other,title='Wrong category PS5',categories=[{'id':2000}]),dict(other,title='Usenet PS5',protocol='usenet')])
    else:self.reply([dict(row,indexerId=2),dict(row,categories=[{'id':2000}]),row,dict(other,title='Other homebrew PS5 fixture',seeders=6),dict(row,title='Original homebrew PS5 fixture',grabs=4)])
   elif url.path=='/artwork':
    cover_calls.append(self.path)
    data=bytes([12,80,160])*(160*240);self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
   else:
    assert url.path in ['/1/download','/8/download'] and 'apikey' not in urllib.parse.parse_qs(url.query)
    data=b'd4:infod4:name7:fixtureee';self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 rpc=RtorrentFixture(entries,rpc_hook);calls=rpc.calls;indexer=ThreadingHTTPServer(('127.0.0.1',0),Indexer)
 cert=root/'cert.pem';key=root/'key.pem'
 subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(cert),'-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(cert,key);indexer.socket=ctx.wrap_socket(indexer.socket,server_side=True)
 for server in [indexer]:threading.Thread(target=server.serve_forever,daemon=True).start()
 config=json.dumps(dict(url=f'https://localhost:{indexer.server_port}',apiKey='b'*32,caFile=str(cert),exploreIndexers={'seeders':2,'completed':3,'newest':1}))
 credentials=root/'rtorrent/state';credentials.mkdir(parents=True);(credentials/'botty-credentials.json').write_text(json.dumps(dict(username='botty',password='TESTpw')))
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
  # A fresh console starts without private search setup. Requests must succeed
  # with an inline error, leaving the catalog and other actions available.
  for broken in [None,'{broken json',json.dumps(dict(url='http://invalid',apiKey='b'*32)),json.dumps(dict(url=f'https://localhost:{indexer.server_port}',apiKey='invalid'))]:
   if broken is not None:(root/'prowlarr.json').write_text(broken)
   for path,body,section in [('/api/explore',{'sort':'seeders'},'explore'),('/api/search',{'query':'demo'},'search')]:
    assert request(path,body)['ok']
    state=request('/api/state');unavailable=state[section]
    assert not unavailable['busy'] and not unavailable['adding'] and unavailable['results']==[]
    assert 'Prowlarr configuration' in unavailable['error'] and 'Other tabs remain available' in unavailable['error']
    assert str(root) not in unavailable['error'] and 'b'*32 not in unavailable['error']
    assert state['torrents'] and 'jobs' in state and 'storage' in state
   assert not queries
  # Provisioning configuration recovers immediately, without a service restart.
  (root/'prowlarr.json').write_text(config)
  request('/api/search',{'query':'Homebrew & demo','categories':[2000],'indexerIds':[2]})
  state=until(lambda:(s if not s['search']['busy'] else None) if (s:=request('/api/state')) else None)
  results=state['search']['results'];assert len(results)==2 and all('download' not in r for r in results)
  assert queries[0]['categories']==['1000'] and queries[0]['indexerIds']==['-2'] and queries[0]['query']==['Homebrew & demo']
  library=root/'test-library/installed/sce_sys';library.mkdir(parents=True)
  (library/'param.json').write_text(json.dumps({'localizedParameters':{'defaultLanguage':'en-US','en-US':{'titleName':'Installed Game'}}}))
  for sort,names in [('seeders',['Demo PS5','Third PS5','Second PS5','Null counts PS5']),('completed',['Third PS5','Second PS5','Demo PS5','Null counts PS5']),('newest',['Third PS5','Second PS5','Demo PS5','Null counts PS5'])]:
   request('/api/explore',{'sort':sort})
   browse=until(lambda:(e if not e['busy'] else None) if (e:=request('/api/state')['explore']) else None)
   assert [r['name'] for r in browse['results']]==names,browse
   assert next(r for r in browse['results'] if r['name']=='Demo PS5')['completed']==12
   assert next(r for r in browse['results'] if r['name']=='Null counts PS5')['completed']==0
   demo=next(r for r in browse['results'] if r['name']=='Demo PS5')
   assert len(demo['sources'])==3 and {r['size'] for r in demo['sources']}=={size,size*2}
   assert all('download' not in source for game in browse['results'] for source in game['sources'])
   assert {r['tracker'] for r in demo['sources']}=={'Tracker One','Tracker Eight'}
   assert queries[-1]['indexerIds']==['-2'] and queries[-1]['categories']==['1000']
   assert request('/api/state')['search']['results']==results
  stale_cache=root/'cache/explore-newest.json'
  saved=json.loads(stale_cache.read_text());saved['saved']=int(time.time())-3600;stale_cache.write_text(json.dumps(saved))
  query_count=len(queries)
  request('/api/explore',{'sort':'newest'})
  cached=request('/api/state')['explore'];assert not cached['busy'] and cached['results']==browse['results']
  assert 'outdated' in cached['notice']
  assert len(queries)==query_count
  (root/'prowlarr.json').write_text('{broken json')
  request('/api/explore',{'sort':'newest'})
  assert request('/api/state')['explore']['results']==[]
  try:request('/api/explore/add',{'id':cached['results'][0]['sources'][0]['id']});raise AssertionError('Invalidated source accepted after configuration failure')
  except urllib.error.HTTPError as e:assert e.code==400
  (root/'prowlarr.json').write_text(config)
  request('/api/explore',{'sort':'newest'})
  recovered=request('/api/state')['explore'];assert not recovered['busy'] and recovered['results']==cached['results'] and len(queries)==query_count
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
  demo=next(r for r in browse['results'] if r['name']=='Demo PS5')
  assert all(source['id']!=demo['id'] for source in demo['sources'])
  selected=next(source['id'] for source in demo['sources'] if source['tracker']=='Tracker Eight' and source['size']==size)
  assert selected!=demo['id']
  delayed_refresh=True
  request('/api/explore',{'sort':'newest','refresh':True});assert refresh_started.wait(5)
  refreshing=request('/api/state')['explore'];assert refreshing['busy'] and refreshing['results']==browse['results']
  refresh_release.set()
  refreshed=until(lambda:(e if not e['busy'] else None) if (e:=request('/api/state')['explore']) else None)
  delayed_refresh=False;refresh_started.clear();refresh_release.clear()
  refreshed_demo=next(r for r in refreshed['results'] if r['id']==demo['id'])
  assert refreshed_demo['name']=='Demo PS5 Deluxe'
  assert refreshed_demo['id']==demo['id'] and all(s['id']!=selected for s in refreshed_demo['sources'])
  request('/api/explore/add',{'id':selected})
  until(lambda:not request('/api/state')['explore']['adding'])
  assert all(r['id']!=demo['id'] for r in request('/api/state')['explore']['results'])
  assert not any(r['name']=='Demo PS5' for r in request('/api/state')['explore']['results'])
  request('/api/explore/add',{'id':selected})
  assert sum(c['method']=='load.start' for c in calls)==1
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
  # Losing setup after a successful search also drops stale download choices.
  (root/'prowlarr.json').unlink()
  for path,body,section in [('/api/explore',{'sort':'newest','refresh':True},'explore'),('/api/search',{'query':'demo'},'search')]:
   assert request(path,body)['ok']
   state=request('/api/state')
   assert state[section]['results']==[] and not state[section]['busy'] and state[section]['error']
   assert state['torrents'] and any(j['id']==job_id for j in state['jobs'])
  rpc.assert_clean()
  print('Search pipeline passed: HTTPS, all-provider merge, grabs ranking, null metrics, download origin confinement, opaque IDs, duplicate prevention, durable queue, automatic extraction/publication and archive preservation.')
 finally:
  proc.terminate();proc.wait(timeout=5);rpc.shutdown();indexer.shutdown()
