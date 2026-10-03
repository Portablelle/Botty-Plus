"""A real service stays responsive; a lost destructive response is never replayed."""
import json, pathlib, socket, subprocess, tempfile, time, urllib.request
from rtorrent_fixture import RtorrentFixture
from shadow_fixture import ShadowFixture
ROOT=pathlib.Path(__file__).resolve().parents[1]
for scenario in ('success', 'failed', 'lost', 'rejected'):
 with tempfile.TemporaryDirectory(prefix='botty-background-') as tmp:
  root=pathlib.Path(tmp);game=root/'test-library/PPSA12345-app';game.mkdir(parents=True)
  (game/'fixture').write_text('preserve until accepted');(root/'jobs').mkdir()
  job_id='a'*32
  (root/'jobs'/f'{job_id}.json').write_text(json.dumps(dict(id=job_id,name='Isolated game',status='moved',destination=str(game),content=dict(kind='folder',titleId='PPSA12345',destination=game.name))))
  rpc=RtorrentFixture([]);shadow=ShadowFixture({'PPSA12345':str(game)})
  shadow.delay=.8;shadow.fail=scenario in ('failed','lost');shadow.lose_status=scenario=='lost'
  if scenario=='rejected':shadow.paths['PPSA12345']=str(root/'different-game')
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  command=[str(ROOT/'build/botty-native'),'--root',str(root),'--port',str(port),'--rpc-port',str(rpc.server_port),'--shadow-port',str(shadow.port)]
  token=''
  def request(route,body=None):
   req=urllib.request.Request(f'http://127.0.0.1:{port}'+route,data=None if body is None else json.dumps(body).encode(),headers={'X-Botty-Token':token,'Content-Type':'application/json'})
   with urllib.request.urlopen(req,timeout=2) as result:return result.status,json.load(result)
  def start():
   process=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
   for _ in range(100):
    try:return process,request('/api/bootstrap')[1]['token']
    except OSError:time.sleep(.05)
   raise AssertionError('Service startup timed out')
  process,token=start()
  try:
   assert request('/api/delete-library-game',dict(id=job_id,confirmed=True))[0]==202
   seen_running=False
   for _ in range(100):
    task=request('/api/processing')[1]['tasks'][0]
    assert request('/api/state')[0]==200
    if task['status']=='running':seen_running=True
    else:break
    time.sleep(.025)
   assert task['status']==('completed' if scenario=='success' else 'failed'),task
   if scenario=='success':assert seen_running and not game.exists()
   else:assert game.exists() and task['error']
   assert sum(route.endswith('/games/delete') for route,_ in shadow.calls)==1
   process.terminate();process.wait(timeout=5)
   process,token=start()
   assert sum(route.endswith('/games/delete') for route,_ in shadow.calls)==1
   state=request('/api/state')[1]
   assert bool(state['extracting'])==(scenario in ('lost','failed')),state['transfer']
  finally:
   process.terminate();process.wait(timeout=5);shadow.close();rpc.shutdown()
print('Background deletion: live UI, success, rejection, worker failure, lost status and no replay across restart passed')
