"""Recover stale Library moves using real service state, without replay or deletion."""
import json, os, pathlib, socket, subprocess, tempfile, time, urllib.request
from rtorrent_fixture import RtorrentFixture
from shadow_fixture import ShadowFixture

ROOT = pathlib.Path(__file__).resolve().parents[1]
JOB = 'a' * 32
VOLUME = 'external-' + 'b' * 32
TITLE = 'PPSA12345'
CASES = ('legacy-complete', 'checkpointed-job', 'running-journal', 'scanner-refused',
         'both-copies', 'active-worker', 'queued-worker', 'offline', 'disconnected',
         'wrong-catalog', 'symlink', 'partial-refusal', 'compressed-record',
         'live-complete', 'live-scanner-refused')

for case in CASES:
 with tempfile.TemporaryDirectory(prefix='botty-transfer-recovery-') as tmp:
  base = pathlib.Path(tmp); root = base/'internal'; usb = base/'usb'
  source = root/'test-library'/f'{TITLE}-app'; dest = usb/'homebrew'/source.name
  source.parent.mkdir(parents=True); dest.mkdir(parents=True)
  (dest/'fixture').write_text('already moved')
  (usb/'.botty-volume.json').write_text(json.dumps(dict(id=VOLUME)))
  (root/'jobs').mkdir()
  job = dict(id=JOB, name='Recovered game', status='moved', storage='internal',
             archiveStorage='internal', destination=str(source),
             content=dict(kind='folder', titleId=TITLE, destination=source.name))
  if case == 'checkpointed-job': job.update(storage=VOLUME, destination=str(dest))
  if case == 'live-complete': del job['content']['titleId']
  journal = dict(status='running' if case == 'running-journal' else 'uncertain',
                 kind='job', id=JOB, sourceStorage='internal', storage=VOLUME,
                 remoteJobId=7, error='Interrupted file operation')
  (root/'jobs'/f'{JOB}.json').write_text(json.dumps(job))
  if not case.startswith('live-'): (root/'transfer.json').write_text(json.dumps(journal))
  if case in ('both-copies', 'scanner-refused', 'partial-refusal', 'live-complete', 'live-scanner-refused'):
   source.mkdir(); (source/'fixture').write_text('source retained')
  if case in ('scanner-refused', 'partial-refusal', 'live-complete', 'live-scanner-refused'):
   (dest/'fixture').unlink(); dest.rmdir()
  if case == 'symlink':
   (dest/'fixture').unlink(); dest.rmdir(); (usb/'other-game').mkdir()
   dest.symlink_to(usb/'other-game', target_is_directory=True)
  if case == 'compressed-record':
   (root/'compressor').mkdir()
   (root/'compressor/enabled.json').write_text('{"mode":"library-1.2"}')
   (root/'compressor/games.json').write_text(json.dumps({JOB:dict(status='ready', jobId=JOB, output=str(root/'compressor/output'/f'{TITLE}.ffpfsc'), titleId=TITLE)}))
  rpc = RtorrentFixture([]); shadow = ShadowFixture({TITLE:str(dest)})
  if case == 'wrong-catalog': shadow.paths[TITLE] = str(usb/'different-game')
  if case in ('offline', 'live-complete'): shadow.lose_status = True
  if case == 'live-scanner-refused': shadow.fail = True; shadow.fail_reason = 'scanner_busy'
  if case in ('active-worker', 'queued-worker', 'scanner-refused', 'partial-refusal'):
   shadow.task = dict(job_id=7, title_id=TITLE, operation='move', source=str(source),
                      destination=str(dest), state='failed', active=False,
                      queue_running=False, queue_count=0, result_error_reason='scanner_busy',
                      result_error='Scanner busy', processed_bytes=0, processed_files=0)
   if case == 'active-worker': shadow.task.update(state='transferring', active=True)
   if case == 'queued-worker': shadow.task.update(queue_running=True, queue_count=1)
   if case == 'partial-refusal': shadow.task.update(processed_bytes=1)
  if case == 'disconnected': usb.rename(base/'unplugged')
  with socket.socket() as sock:
   sock.bind(('127.0.0.1',0)); port = sock.getsockname()[1]
  command = [str(ROOT/'build/botty-native'), '--root', str(root), '--external', str(usb),
             '--port', str(port), '--rpc-port', str(rpc.server_port), '--shadow-port', str(shadow.port)]
  token = ''
  def request(route, body=None):
   req = urllib.request.Request(f'http://127.0.0.1:{port}'+route, data=None if body is None else json.dumps(body).encode(), headers={'X-Botty-Token':token, 'Content-Type':'application/json'})
   with urllib.request.urlopen(req, timeout=3) as reply: return json.load(reply)
  process = subprocess.Popen(command, env={**os.environ, 'BOTTY_TEST_POLL_MS':'50'},
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
  try:
   for _ in range(100):
    try: token = request('/api/bootstrap')['token']; break
    except OSError: time.sleep(.05)
   if case.startswith('live-'): request('/api/transfer', dict(kind='job', id=JOB, storage=VOLUME))
   recovered = case in ('legacy-complete', 'checkpointed-job', 'running-journal', 'scanner-refused', 'live-complete', 'live-scanner-refused')
   deadline = time.monotonic() + (14 if case == 'live-complete' else 4 if recovered else .7)
   started = time.monotonic(); seen_uncertain = False
   while time.monotonic() < deadline:
    if case == 'live-complete' and time.monotonic()-started > 1: shadow.lose_status = False
    state = request('/api/state')
    seen_uncertain |= state['transfer'].get('status') == 'uncertain'
    if not state['extracting']: break
    time.sleep(.05)
   assert bool(state['extracting']) != recovered, (case, state['transfer'])
   if recovered:
    assert state['transfer']['status'] == ('failed' if case in ('scanner-refused', 'live-scanner-refused') else 'complete'), (case, state)
    settled = next(j for j in state['jobs'] if j['id'] == JOB)
    if case not in ('scanner-refused', 'live-scanner-refused'):
     assert settled['destination'] == str(dest) and settled['storage'] == VOLUME, settled
     assert settled['archiveStorage'] == 'internal' and settled['content']['titleId'] == TITLE
     assert not source.exists() and (dest/'fixture').read_text() == ('source retained' if case == 'live-complete' else 'already moved')
     assert json.loads((root/'jobs'/f'{JOB}.json').read_text())['destination'] == str(dest)
    else: assert source.exists() and not dest.exists()
    assert json.loads((root/'transfer.json').read_text())['status'] == state['transfer']['status']
   else:
    assert json.loads((root/'transfer.json').read_text())['status'] in ('uncertain', 'running')
   if case.startswith('live-'):
    assert sum(route.endswith('/games/move') for route,_ in shadow.calls) == 1, shadow.calls
    assert not any(route.endswith('/games/delete') for route,_ in shadow.calls)
    if case == 'live-complete': assert seen_uncertain
   else:
    assert not any(route.endswith(('/games/move','/games/delete','/manual/add','/manual/remove')) for route,_ in shadow.calls), (case, shadow.calls)
  finally:
   process.terminate(); process.wait(timeout=8); shadow.close(); rpc.shutdown(); rpc.server_close()
print('Library move recovery: legacy/completed/running journals, checkpoint recovery, no-work refusal, and conservative blocking passed')
