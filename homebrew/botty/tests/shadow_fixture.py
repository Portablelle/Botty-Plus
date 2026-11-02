"""Scoped ShadowMount worker double: only temporary paths supplied by the test."""
import json, pathlib, shutil, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class ShadowFixture:
    def __init__(self, paths=None, online=True):
        self.paths = paths if paths is not None else {}
        self.online = online
        self.task = None
        self.calls = []
        self.delay = .4
        self.fail = False
        self.fail_reason = ""
        self.lose_status = False
        owner = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.calls.append((self.path, data))
                code, out = owner.handle(self.path, data)
                self.send_response(code); self.send_header('Content-Type', 'application/json'); self.end_headers()
                self.wfile.write(json.dumps(out).encode())
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.port = self.server.server_port
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
    def close(self): self.server.shutdown(); self.server.server_close()
    def handle(self, route, data):
        out = dict(status=0)
        if route.endswith('/version'):
            out['capabilities'] = ['botty_background_storage_v1'] if self.online else []
        elif route.endswith('/games'):
            out['games'] = [dict(title_id=k,path=v) for k,v in self.paths.items()]
        elif route.endswith('/games/info'):
            path = str(self.paths[data['title_id']]);out.update(title_id=data['title_id'], source_available=pathlib.Path(path).exists(), path=path, image_backed=path.endswith('.ffpfsc'), source_type='image' if path.endswith('.ffpfsc') else 'folder')
        elif route.endswith('/manual/add'):
            path = pathlib.Path(data['path']);self.paths['PPSA12348' if path.name=='demo.exfat' else path.name[:9]] = str(path)
        elif route.endswith('/games/storage/status'):
            if self.lose_status: return 503, dict(status=5, error='fixture connection lost')
            if self.task is None: out.update(job_id=0, state='idle', active=False, queue_running=False, queue_count=0)
            else:
                if 'job_id' in data: assert data['job_id'] == self.task['job_id']
                out.update(self.task)
        elif route.endswith('/games/move') or route.endswith('/games/delete'):
            title = data['title_id'];source = pathlib.Path(self.paths[title])
            assert source.is_absolute() and str(source).startswith('/'), source
            if data['expected_source'] != str(source): return 409, dict(status=116, error='Source changed')
            operation = route.rsplit('/',1)[1]
            dest = pathlib.Path(data['destination_dir'])/source.name if operation == 'move' else None
            self.task = dict(active=True, queue_running=False, queue_count=0, job_id=(self.task or {}).get('job_id',0)+1, title_id=title, source=str(source), destination=str(dest) if dest else '', operation=operation, state='transferring' if dest else 'deleting', processed_bytes=1, total_bytes=2, processed_files=1, total_files=2, speed_bytes_per_second=1, elapsed_ms=1, result_error='')
            task = self.task
            def finish():
                time.sleep(self.delay)
                if self.fail:
                    task.update(state='failed', active=False, result_error='Busy fixture', result_error_reason=self.fail_reason)
                    if self.fail_reason: task.update(processed_bytes=0, processed_files=0)
                    return
                if dest: shutil.move(str(source),str(dest));self.paths[title]=str(dest)
                elif source.is_dir(): shutil.rmtree(source)
                else: source.unlink()
                task.update(state='completed',active=False,processed_bytes=2,processed_files=2)
            threading.Thread(target=finish, daemon=True).start()
            return 202, dict(status=0, **task)
        return 200, out
