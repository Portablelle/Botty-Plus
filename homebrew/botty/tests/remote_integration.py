"""Real bridge HTTPS -> Botty HTTP -> multi-file resumable local publication."""
import importlib.util
import json
import os
import shutil
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.request
from http.server import ThreadingHTTPServer
from rtorrent_fixture import RtorrentFixture

HERE = Path(__file__).resolve().parent
bridge_path = HERE.parent / 'deployment/botty-remote/server.py'
if not bridge_path.is_file(): bridge_path = HERE.parents[2] / 'deployment/botty-remote/server.py'
spec = importlib.util.spec_from_file_location('remote_bridge', bridge_path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def wait(fn, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = fn()
        if result:
            return result
        time.sleep(.15)
    raise AssertionError('Remote workflow timed out')

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp).resolve()
    sources = root / 'sources'; sources.mkdir()
    game = 'a' * 40; movie = 'b' * 40; folder = 'c' * 40
    payload = os.urandom(2 * 512 * 1024 + 73)
    (sources / 'Game PS5').mkdir()
    (sources / 'Game PS5/a space.RAR').write_bytes(payload)
    (sources / 'Game PS5/ü.r00').write_bytes(b'volume')
    (sources / 'app/sce_sys').mkdir(parents=True)
    (sources / 'app/sce_sys/param.json').write_text('{"titleId":"PPSA12345"}')
    (sources / 'app/eboot.bin').write_bytes(b'fixture app')
    rows = {
        game: ({'name': 'Game PS5', 'save_path': str(sources) + '/'}, [{'name': 'Game PS5/a space.RAR', 'size': len(payload), 'index': 0}, {'name': 'Game PS5/ü.r00', 'size': 6, 'index': 1}]),
        movie: ({'name': 'Movie', 'save_path': str(sources) + '/'}, [{'name': 'movie.mkv', 'size': 20, 'index': 0}]),
        folder: ({'name': 'Loose application', 'save_path': str(sources) + '/'}, [{'name': 'app/sce_sys/param.json', 'size': (sources / 'app/sce_sys/param.json').stat().st_size, 'index': 0}, {'name': 'app/eboot.bin', 'size': 11, 'index': 1}]),
    }
    (sources / 'bad/sce_sys').mkdir(parents=True)
    (sources / 'bad/sce_sys/param.json').write_text('null')
    rows[movie][1].append({'name': 'bad/sce_sys/param.json', 'size': 4, 'index': 1})
    bridge = module.Bridge.__new__(module.Bridge)
    bridge.c = {'token': 'd' * 64, 'downloads': str(sources)}
    bridge.file_cache = {}; bridge.lock = threading.RLock(); bridge.owned = {}; bridge.registry = root / 'registry.json'
    paused = {game: True}
    def info(h):
        t, files = rows[h]
        return dict(t, amount_left=1 if paused.get(h) else 0, progress=0 if paused.get(h) else 1, state='pausedDL' if paused.get(h) else 'uploading', dlspeed=0, upspeed=20, num_seeds=3, num_leechs=0), [dict(f, progress=0 if paused.get(h) else 1) for f in files], not paused.get(h, False)
    bridge.info = info
    def qb(route, fields=None):
        if fields is not None:
            if route == 'torrents/add':
                h = fields['urls'].split('urn:btih:', 1)[1].split('&', 1)[0]
                rows[h] = ({'name': 'Engine choice PS5', 'save_path': fields['savepath'], 'category': fields['category']}, [{'name': 'game.rar', 'size': 32, 'index': 0}])
                paused[h] = True
                return b'Ok.'
            paused[fields['hashes']] = route.endswith('pause')
            return b'Ok.'
        if 'info?hashes=' in route:
            h = route.split('hashes=', 1)[1]
            return json.dumps([dict(rows[h][0], hash=h)] if h in rows else []).encode()
        return json.dumps([dict(t, hash=h) for h, (t, f) in rows.items()]).encode()
    bridge.qb = qb
    ranges = []; rejected = []; interrupted = []; blocked_copy = threading.Event(); release_copy = threading.Event(); blocked_folder = threading.Event(); release_folder = threading.Event()
    class Handler(module.Handler):
        def do_GET(self):
            if self.path == '/file/' + folder + '/0' and not release_folder.is_set():
                blocked_folder.set(); release_folder.wait(20)
            if self.path == '/file/' + game + '/0':
                ranges.append(self.headers.get('Range'))
                if not rejected:
                    rejected.append(True)
                    body = b'<html>redirect response</html>'
                    self.send_response(302); self.send_header('Location', '/untrusted'); self.send_header('Content-Length', str(len(body))); self.end_headers()
                    self.wfile.write(body); self.close_connection = True
                    return
                if not interrupted:
                    interrupted.append(True)
                    self.send_response(200); self.send_header('Content-Length', str(len(payload))); self.end_headers()
                    self.wfile.write(payload[:16384]); self.wfile.flush(); self.close_connection = True
                    return
                if not release_copy.is_set():
                    blocked_copy.set(); release_copy.wait(60)
            self.handle_request()
    remote = ThreadingHTTPServer(('127.0.0.1', 0), Handler); remote.bridge = bridge
    cert = root / 'cert.pem'; key = root / 'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key), '-out', str(cert), '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key); remote.socket = ctx.wrap_socket(remote.socket, server_side=True)
    threading.Thread(target=remote.serve_forever, daemon=True).start()
    data = root / 'botty'; data.mkdir()
    usb = root / 'usb'; usb.mkdir()
    (data / 'remote.json').write_text(json.dumps({'url': f'https://localhost:{remote.server_port}/bridge', 'token': 'd' * 64, 'caFile': str(cert), 'connectAddress': '127.0.0.1'}))
    # Strip the configured URL prefix at the bridge boundary, like nginx.
    original = Handler.handle_request
    def prefixed(self):
        if self.path.startswith('/bridge'):
            self.path = self.path[len('/bridge'):]
        return original(self)
    Handler.handle_request = prefixed
    Handler.do_POST = lambda self: self.handle_request()
    original_get = Handler.do_GET
    def get(self):
        if self.path.startswith('/bridge'):
            self.path = self.path[len('/bridge'):]
        return original_get(self)
    Handler.do_GET = get
    credentials = data / 'rtorrent/state'; credentials.mkdir(parents=True)
    (credentials / 'botty-credentials.json').write_text('{"username":"botty","password":"fixture"}')
    local_entries = []
    rpc = RtorrentFixture(local_entries)
    ignore_stops = [False]; backend_offline = [False]; fail_inspection = [None]
    def rpc_hook(method, params):
        if backend_offline[0]: raise RuntimeError('Fixture rTorrent unavailable')
        if method == 'd.multicall' and fail_inspection[0] is not None:
            if fail_inspection[0] == 0:
                fail_inspection[0] = None
                raise RuntimeError('Fixture transient inspection failure')
            fail_inspection[0] -= 1
        if ignore_stops[0] and method == 'd.stop': return 0
        return NotImplemented
    rpc.hook = rpc_hook
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    log = (root / 'service.log').open('w')
    proc = subprocess.Popen([str(HERE.parent / 'build/botty-native'), '--root', str(data), '--external', str(usb), '--port', str(port), '--rpc-port', str(rpc.server_port), '--ui', str(HERE.parent / 'ui')], stdout=log, stderr=log)
    base = f'http://127.0.0.1:{port}'; token = ''
    def api(path, body=None):
        req = urllib.request.Request(base + path, None if body is None else json.dumps(body).encode(), {'X-Botty-Token': token, 'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                return json.load(response)
        except urllib.error.HTTPError as e:
            raise AssertionError(e.read().decode()) from e
    try:
        def bootstrap():
            try: return api('/api/bootstrap')['token']
            except OSError: return None
        token = wait(bootstrap)
        state = wait(lambda: (s if len(s['remoteDownloads']['candidates']) == 2 else None) if (s := api('/api/state')) else None)
        assert {c['hash'] for c in state['remoteDownloads']['candidates']} == {game, folder}
        assert movie not in bridge.owned
        assert not (data / 'downloads/complete' / game).exists()
        choice_hash = '1' * 40; magnet = 'magnet:?xt=urn:btih:' + choice_hash
        owned_before = dict(bridge.owned)
        local_choice = api('/api/torrent', {'action': 'add', 'magnet': magnet, 'automatic': False})['torrent-added']['id']
        assert local_choice < 1000000000 and bridge.owned == owned_before
        assert any(c['method'] == 'load.start' and c['params'][1] == magnet for c in rpc.calls)
        local_entries.append({'id': 1, 'hashString': choice_hash, 'name': 'Local engine choice', 'status': 0, 'leftUntilDone': 1, 'totalSize': 32, 'downloadDir': str(data / 'downloads/complete'), 'files': [{'name': 'game.rar', 'length': 32, 'bytesCompleted': 0}]})
        assert api('/api/torrent', {'action': 'add', 'magnet': magnet, 'engine': 'rtorrent', 'automatic': False})['torrent-duplicate']['id'] == local_choice
        remote_choice = api('/api/torrent', {'action': 'add', 'magnet': magnet, 'engine': 'qbittorrent', 'automatic': False})['torrent-added']['id']
        assert remote_choice >= 1000000000 and choice_hash in bridge.owned
        assert json.loads((data / 'automatic' / (choice_hash + '.json')).read_text())['remote'] is False
        assert json.loads((data / 'automatic' / ('remote-' + str(remote_choice) + '.json')).read_text())['remote'] is True
        api('/api/torrent', {'id': remote_choice, 'action': 'remove-data', 'confirmed': True})
        local_entries.clear(); rows.pop(choice_hash); paused.pop(choice_hash); bridge.owned.pop(choice_hash)
        for engine in ['unexpected', 'rtorrent']:
            backend_offline[0] = engine == 'rtorrent'
            before = dict(bridge.owned)
            try:
                api('/api/torrent', {'action': 'add', 'magnet': 'magnet:?xt=urn:btih:' + '2' * 40, 'engine': engine})
                raise RuntimeError('Invalid/unavailable selected engine was accepted')
            except AssertionError: pass
            assert bridge.owned == before
        backend_offline[0] = False
        # A discovered candidate must not intercept operations on a local hash match.
        local_file = data / 'downloads/complete/local-copy.rar'; local_file.write_bytes(b'local copy')
        local_entries.append({'id': 1, 'hashString': game, 'name': 'Local copy', 'status': 0, 'error': 0, 'leftUntilDone': 0, 'totalSize': 10, 'downloadDir': str(data / 'downloads/complete'), 'files': [{'name': 'local-copy.rar', 'length': 10, 'bytesCompleted': 10}]})
        local = wait(lambda: next((t for t in api('/api/state')['torrents'] if t['hashString'] == game and not t.get('remote')), None))
        local_entries.append(dict(local_entries[0], hashString=movie, name='Shared local file'))
        fail_inspection[0] = 1
        try:
            api('/api/torrent', {'id': local['id'], 'action': 'remove-data', 'confirmed': True})
            raise RuntimeError('Transient local inspection failure allowed deletion')
        except AssertionError as e:
            assert 'cannot be inspected' in str(e)
        assert local_file.exists()
        local_entries.pop(); fail_inspection[0] = None
        local_entries[0]['status'] = 4; ignore_stops[0] = True
        try:
            api('/api/torrent', {'id': local['id'], 'action': 'remove-data', 'confirmed': True})
            raise RuntimeError('Paused candidate falsely confirmed local stop')
        except AssertionError as e:
            assert 'did not stop' in str(e)
        assert local_file.exists()
        ignore_stops[0] = False
        api('/api/torrent', {'id': local['id'], 'action': 'remove-data', 'confirmed': True})
        assert not local_entries and not local_file.exists()
        assert any(t['hashString'] == game and t.get('remoteCandidate') for t in api('/api/state')['torrents'])
        local_task = data / 'automatic' / (game + '.json')
        local_task_data = {'hash': game, 'status': 'waiting', 'storage': 'internal'}
        local_task.write_text(json.dumps(local_task_data))
        result = api('/api/remote/import', {'hash': game, 'automatic': False})
        assert json.loads(local_task.read_text()) == local_task_data
        local_task_data['status'] = 'download-only'; local_task.write_text(json.dumps(local_task_data))
        id = result['torrent-added']['id']
        assert id >= 1000000000
        assert next(t for t in api("/api/state")["torrents"] if t["id"] == id)["status"] == 0
        assert not (data / "downloads/complete" / game).exists()
        other = 'e' * 40; rows[other] = rows[game]; paused[other] = True
        other_id = api('/api/remote/import', {'hash': other, 'automatic': False})['torrent-added']['id']
        paused[game] = False
        assert blocked_copy.wait(15), 'Console copy never started'
        stage_name = '.remote-' + game + '.partial/Game PS5/a space.RAR'
        local_entries.append({'id': 1, 'hashString': game, 'name': 'Shared staging file', 'status': 0, 'error': 0, 'leftUntilDone': 1, 'totalSize': len(payload), 'downloadDir': str(data / 'downloads/complete'), 'files': [{'name': stage_name, 'length': len(payload), 'bytesCompleted': 16384}]})
        staged_local = wait(lambda: next((t for t in api('/api/state')['torrents'] if not t.get('remote')), None))
        try:
            api('/api/torrent', {'id': staged_local['id'], 'action': 'remove-data', 'confirmed': True})
            raise RuntimeError('Active remote staging deletion was accepted')
        except AssertionError as e:
            assert 'shared with another torrent' in str(e)
        stage_storage = next(s['id'] for s in api('/api/state')['storage'] if s['id'] != 'internal')
        api('/api/transfer', {'id': staged_local['id'], 'kind': 'torrent', 'storage': stage_storage})
        wait(lambda: api('/api/state')['transfer'].get('status') != 'running')
        assert 'shared with another torrent' in api('/api/state')['transfer']['error']
        paused[other] = False
        wait(lambda: any(t['id'] == other_id and t['remotePhase'] == 'remote' and t['percentDone'] > .99 for t in api('/api/state')['torrents']), 16)
        newcomer = 'f' * 40; rows[newcomer] = rows[game]; paused[newcomer] = True
        wait(lambda: any(t['hashString'] == newcomer and t.get('remoteCandidate') for t in api('/api/state')['torrents']), 40)
        assert newcomer not in bridge.owned and not release_copy.is_set()
        release_copy.set()
        wait(lambda: any(t['id'] == id and 'shared with a local torrent' in t.get('errorString', '') for t in api('/api/state')['torrents']))
        assert not (data / 'downloads/complete' / game).exists()
        assert (data / 'downloads/complete' / ('.remote-' + game + '.partial') / 'Game PS5/a space.RAR').read_bytes() == payload
        proc.terminate(); proc.wait(timeout=10)
        partial = data / 'downloads/complete' / ('.remote-' + game + '.partial')
        old_inode = partial.stat().st_ino
        replacement = partial.with_name(partial.name + '.reconnected')
        shutil.copytree(partial, replacement); shutil.rmtree(partial); replacement.rename(partial)
        assert partial.stat().st_ino != old_inode
        proc = subprocess.Popen(proc.args, stdout=log, stderr=log); token = ''; token = wait(bootstrap)
        paused[game] = True  # The server lost/rechecked content after console transfer began.
        wait(lambda: any(t['id'] == id and t['remotePhase'] == 'remote' for t in api('/api/state')['torrents']))
        api('/api/torrent', {'id': id, 'action': 'pause'})
        api('/api/torrent', {'id': id, 'action': 'resume'})
        assert not paused[game], 'Resume did not restart the incomplete server torrent'
        local_entries.clear()
        wait(lambda: any(t['id'] == id and t['remotePhase'] == 'ready' for t in api('/api/state')['torrents']))
        assert (data / 'downloads/complete' / game / 'Game PS5/a space.RAR').read_bytes() == payload
        assert (data / 'downloads/complete' / game / 'Game PS5/ü.r00').read_bytes() == b'volume'
        assert 'bytes=16384-' in ranges, ranges
        assert not next(t for t in api('/api/state')['torrents'] if t['id'] == id)['remoteLoose']
        proc.terminate(); proc.wait(timeout=10)
        published = data / 'downloads/complete' / game
        old_inode = published.stat().st_ino
        replacement = published.with_name(published.name + '.reconnected')
        shutil.copytree(published, replacement); shutil.rmtree(published); replacement.rename(published)
        assert published.stat().st_ino != old_inode
        proc = subprocess.Popen(proc.args, stdout=log, stderr=log); token = ''; token = wait(bootstrap)
        assert (sources / 'Game PS5/a space.RAR').read_bytes() == payload
        saved = json.loads((data / 'remote-downloads.json').read_text())
        assert set(saved[game]['publishedIdentity']) == {'marker'}
        marker = published / '.botty-remote-owner'; ownership = marker.read_bytes(); marker.unlink()
        try:
            api('/api/torrent', {'id': id, 'action': 'remove-data', 'confirmed': True})
            raise RuntimeError('Import without its ownership marker was deleted')
        except AssertionError as e:
            assert 'was replaced' in str(e)
        assert (published / 'Game PS5/a space.RAR').read_bytes() == payload
        marker.write_bytes(ownership)
        assert not list((data / 'jobs').glob('*.json'))
        proc.terminate(); proc.wait(timeout=10)
        previous_job = {'id': '0123456789abcdef' * 2, 'hash': game, 'name': 'Game PS5', 'archive': game + '/Game PS5/a space.RAR', 'automatic': True, 'torrentId': id - 1, 'status': 'ready', 'storage': 'internal'}
        (data / 'jobs' / (previous_job['id'] + '.json')).write_text(json.dumps(previous_job))
        automatic_game = data / 'automatic' / ('remote-' + str(id) + '.json')
        automatic_game.write_text(json.dumps({'hash': game, 'torrentId': id, 'remote': True, 'status': 'waiting', 'storage': 'internal'}))
        proc = subprocess.Popen(proc.args, stdout=log, stderr=log); token = ''; token = wait(bootstrap)
        wait(lambda: json.loads(automatic_game.read_text())['status'] == 'attention')
        assert any(j.get('torrentId') == id and j.get('error') == 'This archive was already extracted' for j in api('/api/state')['jobs'])
        assert (published / 'Game PS5/a space.RAR').read_bytes() == payload
        local_movie = data / 'downloads/complete/local-movie.mkv'; local_movie.write_bytes(b'movie')
        local_entries.append({'id': 1, 'hashString': folder, 'name': 'Local hash match', 'status': 0, 'error': 0, 'leftUntilDone': 0, 'totalSize': 5, 'downloadDir': str(data / 'downloads/complete'), 'files': [{'name': 'local-movie.mkv', 'length': 5, 'bytesCompleted': 5}]})
        folder_id = api('/api/remote/import', {'hash': folder, 'automatic': True})['torrent-added']['id']
        assert blocked_folder.wait(15)
        time.sleep(6)  # Give automatic preparation a complete polling cycle.
        task = json.loads((data / 'automatic' / ('remote-' + str(folder_id) + '.json')).read_text())
        assert task['status'] == 'waiting', task
        release_folder.set(); local_entries.clear()
        wait(lambda: (data / 'test-library/PPSA12345-app/eboot.bin').exists())
        wait(lambda: not api('/api/state')['extracting'])
        assert next(t for t in api('/api/state')['torrents'] if t['id'] == folder_id)['remoteLoose']
        assert not list((data / 'test-library').rglob('.botty-remote-owner'))
        assert (sources / 'app/eboot.bin').read_bytes() == b'fixture app'
        member = {'name': game + '/Game PS5/a space.RAR', 'length': len(payload), 'bytesCompleted': len(payload)}
        local_entries.append({'id': 1, 'hashString': game, 'name': 'Shared local torrent', 'status': 0, 'error': 0, 'leftUntilDone': 0, 'totalSize': len(payload), 'downloadDir': str(data / 'downloads/complete'), 'files': [member]})
        try:
            api('/api/torrent', {'id': id, 'action': 'remove-data', 'confirmed': True})
            raise RuntimeError('Shared download deletion was accepted')
        except AssertionError as e:
            assert 'shared with another torrent' in str(e)
        shared_local = next(t for t in api('/api/state')['torrents'] if not t.get('remote'))
        try:
            api('/api/torrent', {'id': shared_local['id'], 'action': 'remove-data', 'confirmed': True})
            raise RuntimeError('Local deletion of remote published files was accepted')
        except AssertionError as e:
            assert 'shared with another torrent' in str(e)
        local_entries.clear()
        assert (data / 'downloads/complete' / game / 'Game PS5/a space.RAR').read_bytes() == payload
        external = next(s['id'] for s in api('/api/state')['storage'] if s['id'] != 'internal')
        sessions = data / 'rtorrent/state/session'; sessions.mkdir(exist_ok=True)
        saved_local = sessions / 'saved-local.torrent'; saved_local.write_bytes(b'fixture saved session')
        backend_offline[0] = True
        api('/api/transfer', {'id': id, 'kind': 'torrent', 'storage': external})
        wait(lambda: api('/api/state')['transfer'].get('status') != 'running')
        failed_transfer = api('/api/state')['transfer']
        assert failed_transfer['status'] == 'failed' and 'saved torrents' in failed_transfer['error'], failed_transfer
        assert (data / 'downloads/complete' / game / 'Game PS5/a space.RAR').read_bytes() == payload
        backend_offline[0] = False; saved_local.unlink()
        api('/api/transfer', {'id': id, 'kind': 'torrent', 'storage': external})
        wait(lambda: api('/api/state')['transfer'].get('status') != 'running')
        assert api('/api/state')['transfer']['status'] == 'complete'
        assert not (data / 'downloads/complete' / game).exists()
        moved_row = next(t for t in api('/api/state')['torrents'] if t['id'] == id)
        external_copy = Path(moved_row['downloadDir']) / game
        assert (external_copy / 'Game PS5/a space.RAR').read_bytes() == payload
        api('/api/torrent', {'id': id, 'action': 'remove-data', 'confirmed': True})
        assert not external_copy.exists()
        assert json.loads(local_task.read_text()) == local_task_data
        assert (sources / 'Game PS5/a space.RAR').exists()
        assert movie not in bridge.owned
        api('/api/remote/import', {'hash': game, 'automatic': False})
        wait(lambda: any(t.get('hashString') == game and t.get('remotePhase') == 'ready' for t in api('/api/state')['torrents']))
        assert (data / 'downloads/complete' / game / 'Game PS5/a space.RAR').read_bytes() == payload
        # An unrelated empty destination is never adopted as our reservation.
        proc.terminate(); proc.wait(timeout=10)
        target = data / 'downloads/complete' / game
        stage = data / 'downloads/complete' / ('.remote-' + game + '.partial')
        target.rename(stage); target.mkdir(); foreign_inode = target.stat().st_ino
        saved = json.loads((data / 'remote-downloads.json').read_text())
        saved[game].update(phase='transfer', paused=False, publishing=True)
        saved[game].pop('publishedIdentity', None); saved[game].pop('reservationIdentity', None)
        (data / 'remote-downloads.json').write_text(json.dumps(saved))
        proc = subprocess.Popen(proc.args, stdout=log, stderr=log); token = ''; token = wait(bootstrap)
        wait(lambda: any(t.get('hashString') == game and 'already exists' in t.get('errorString', '') for t in api('/api/state')['torrents']))
        assert target.stat().st_ino == foreign_inode and not list(target.iterdir())
        assert (stage / 'Game PS5/a space.RAR').read_bytes() == payload
        target.rmdir()  # The test actor removes only its own unrelated empty directory.
        wait(lambda: any(t.get('hashString') == game and t.get('remotePhase') == 'ready' for t in api('/api/state')['torrents']))
        # Simulate a crash after reserving the destination but before publication.
        proc.terminate(); proc.wait(timeout=10)
        target = data / 'downloads/complete' / game
        stage = data / 'downloads/complete' / ('.remote-' + game + '.partial')
        target.rename(stage); target.mkdir()
        saved = json.loads((data / 'remote-downloads.json').read_text())
        saved[game].update(phase='transfer', paused=False, publishing=True)
        saved[game]['reservationIdentity'] = {'device': target.stat().st_dev, 'inode': target.stat().st_ino}
        saved[game].pop('publishedIdentity', None)
        (data / 'remote-downloads.json').write_text(json.dumps(saved))
        proc = subprocess.Popen(proc.args, stdout=log, stderr=log)
        token = ''; token = wait(bootstrap)
        wait(lambda: any(t.get('hashString') == game and t.get('remotePhase') == 'ready' for t in api('/api/state')['torrents']))
        assert (target / 'Game PS5/a space.RAR').read_bytes() == payload
        # A fully published import recovers even after the server torrent disappears.
        proc.terminate(); proc.wait(timeout=10)
        saved = json.loads((data / 'remote-downloads.json').read_text())
        saved[game].update(phase='transfer', paused=False, publishing=True)
        saved[game].pop('publishedIdentity', None)
        (data / 'remote-downloads.json').write_text(json.dumps(saved))
        server_game = rows.pop(game)
        proc = subprocess.Popen(proc.args, stdout=log, stderr=log); token = ''; token = wait(bootstrap)
        wait(lambda: any(t.get('hashString') == game and t.get('remotePhase') == 'ready' for t in api('/api/state')['torrents']))
        assert (target / 'Game PS5/a space.RAR').read_bytes() == payload
        rows[game] = server_game
        # Deletion also recovers ownership after rename, without a ready marker.
        proc.terminate(); proc.wait(timeout=10)
        saved = json.loads((data / 'remote-downloads.json').read_text())
        interrupted_id = saved[game]['id']; saved[game].update(phase='transfer', paused=True, publishing=True)
        saved[game].pop('publishedIdentity', None)
        (data / 'remote-downloads.json').write_text(json.dumps(saved))
        proc = subprocess.Popen(proc.args, stdout=log, stderr=log); token = ''; token = wait(bootstrap)
        api('/api/torrent', {'id': interrupted_id, 'action': 'remove-data', 'confirmed': True})
        assert not target.exists()
        api('/api/remote/import', {'hash': game, 'automatic': False})
        wait(lambda: any(t.get('hashString') == game and t.get('remotePhase') == 'ready' for t in api('/api/state')['torrents']))
        conflict = 'd' * 40; rows[conflict] = rows[game]
        preexisting = data / 'downloads/complete' / conflict / 'Game PS5/a space.RAR'
        preexisting.parent.mkdir(parents=True); preexisting.write_bytes(b'pre-existing user file')
        conflict_id = api('/api/remote/import', {'hash': conflict, 'automatic': False})['torrent-added']['id']
        wait(lambda: any(t['id'] == conflict_id and 'already exists' in t.get('errorString', '') for t in api('/api/state')['torrents']))
        api('/api/torrent', {'id': conflict_id, 'action': 'remove-data', 'confirmed': True})
        assert preexisting.read_bytes() == b'pre-existing user file'
        print('Remote candidates, explicit import, multi-file HTTPS resume, loose PS5 preparation and local-only deletion passed')
    except Exception:
        log.flush(); print((root / 'service.log').read_text()); raise
    finally:
        proc.terminate(); proc.wait(timeout=10); remote.shutdown(); rpc.shutdown(); log.close()
