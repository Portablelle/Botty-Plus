"""Private server adapter for Botty's shared Switch archive/CNMT verifier."""
import fcntl
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import threading
import time


class SwitchPreparer:
    def __init__(self, binary, keys, root, forge):
        self.binary = str(Path(binary).resolve())
        if not Path(binary).is_absolute() or not os.access(self.binary, os.X_OK):
            raise ValueError('switchBinary must be an absolute executable path')
        self.keys = str(Path(keys).absolute()) if keys else ''
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        if self.root.is_symlink():
            raise ValueError('Switch preparation storage must not be a symlink')
        self.forge = forge
        self.lock = threading.RLock()
        self.active = set()
        self.settling = {}

    def save(self, directory, state):
        data = directory / 'state.tmp'
        with data.open('w') as f:
            json.dump(state, f); f.flush(); os.fsync(f.fileno())
        os.replace(data, directory / 'state.json')
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)

    def poll(self, hash_, root, files, name, retry=False):
        if not re.fullmatch('[a-f0-9]{40}', hash_) or not 0 < len(files) <= 4096:
            raise ValueError('Invalid Switch download identity or inventory')
        identity = []
        for member in files:
            path = Path(root) / member['name']
            with self.forge.open_source(path) as f:
                actual = self.forge.identity(path, f.fileno())
            if actual['size'] != member['size']:
                raise ValueError('Switch download member is incomplete')
            identity.append(actual)
        with self.lock:
            directory = self.root / hash_
            directory.mkdir(mode=0o700, exist_ok=True)
            if directory.is_symlink():
                raise ValueError('Unsafe Switch preparation directory')
            state_file = directory / 'state.json'
            state = json.loads(state_file.read_text()) if state_file.exists() else {}
            if state and state['source'] != identity:
                if (not retry and not state.get('settleRequested')) or hash_ in self.active or state.get('status') == 'ready':
                    raise ValueError('Switch source changed; wait for preparation to stop, then choose Prepare to retry')
                # A fresh attempt may use the new settled source; never overwrite
                # the earlier output or replace a currently served ready result.
                state = dict(state, source=identity, status='waiting', retryRequested=True, settleRequested=True)
                self.save(directory, state)
                self.settling.pop(hash_, None)
            if hash_ in self.active:
                return state
            if state.get('status') == 'ready':
                try:
                    self.validate(state)
                except (OSError, ValueError, KeyError, TypeError):
                    if not retry: raise
                    state = dict(state, status='failed', retryRequested=True, settleRequested=True)
                    self.save(directory, state)
                    self.settling.pop(hash_, None)
                else:
                    return state
            if retry:
                state = dict(state, source=identity, retryRequested=True,
                             settleRequested=not state or state.get('settleRequested', False) or hash_ in self.settling)
                self.save(directory, state)
            if state and not retry and not state.get('retryRequested'):
                return dict(state, status='failed', error=state.get('error') or 'Server restarted during Switch preparation; choose Prepare to retry')
            if self.active:
                return {'status': 'waiting', 'phase': 'Waiting for Switch preparation on server'}
            if not state or state.get('settleRequested'):
                now = time.monotonic()
                previous = self.settling.get(hash_)
                if not previous or previous[0] != identity:
                    if hash_ not in self.settling and len(self.settling) >= 64:
                        oldest = min(self.settling, key=lambda key: self.settling[key][1])
                        if now-self.settling[oldest][1] < 300:
                            return {'status': 'waiting', 'phase': 'Waiting for Switch source observation slot'}
                        self.settling.pop(oldest)
                    self.settling[hash_] = (identity, now)
                    return {'status': 'waiting', 'phase': 'Finalizing Switch download before preparation'}
                if now - previous[1] < 2:
                    return {'status': 'waiting', 'phase': 'Waiting for Switch source files to settle'}
            fd = os.open(self.root / 'worker.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                os.close(fd)
                return {'status': 'waiting', 'phase': 'Waiting for previous Switch preparation to stop'}
            except BaseException:
                os.close(fd)
                raise
            try:
                attempt = os.urandom(16).hex()
                request = directory / (attempt + '.json')
                output = directory / attempt
                request.write_text(json.dumps(dict(root=str(root), files=[dict(name=f['name'], size=f['size']) for f in files], output=str(output), keys=self.keys, name=name, transaction=attempt)))
                state = dict(status='running', source=identity, phase='Extracting and identifying Switch content on server', output=str(output))
                self.save(directory, state)
                self.active.add(hash_)
                threading.Thread(target=self.run, args=(hash_, directory, request, state, fd), daemon=True).start()
                self.settling.pop(hash_, None)
            except BaseException:
                self.active.discard(hash_); os.close(fd); raise
            return state

    def validate(self, state):
        for member in state['files']:
            path = Path(state['output']) / 'content' / member['name']
            with self.forge.open_source(path) as f:
                if self.forge.identity(path, f.fileno()) != member['identity']:
                    raise ValueError('Verified Switch output changed; transfer refused')

    def run(self, hash_, directory, request, state, fd):
        process = None
        try:
            process = subprocess.Popen([self.binary, str(request)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, pass_fds=(fd,))
            deadline = time.monotonic() + 86400
            pending = b''
            error = ''
            last_save = 0
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while True:
                    if time.monotonic() >= deadline:
                        raise ValueError('Switch server preparation timed out; sources kept')
                    if not selector.select(min(10, deadline-time.monotonic())):
                        continue
                    data = os.read(process.stdout.fileno(), 8192)
                    if not data: break
                    pending += data
                    if len(pending) > 65536:
                        raise ValueError('Switch preparation output exceeds metadata limits')
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        message = json.loads(line)
                        if 'error' in message: error = str(message['error'])[:2048]
                        for key in ('phase', 'file', 'bytes', 'total'):
                            if key in message: state[key] = message[key]
                        if time.monotonic()-last_save >= 1:
                            with self.lock: self.save(directory, state)
                            last_save = time.monotonic()
            if process.wait(timeout=30):
                raise ValueError(error or 'Switch archive/format verification failed; sources kept')
            output = Path(state['output'])
            receipt_path = output / 'prepared.json'
            if receipt_path.stat().st_size > 1024*1024:
                raise ValueError('Switch preparation manifest exceeds limits')
            receipt = json.loads(receipt_path.read_text())
            if receipt.get('platform') != 'switch' or receipt.get('transaction') != request.stem:
                raise ValueError('Switch preparation receipt mismatch')
            files = []
            for index, member in enumerate(receipt['files']):
                name = member['name']
                if not re.fullmatch(r'(roms|updates)/[0-9A-F]{16}-v[0-9]+\.(nsp|xci)(\.botty\.json)?', name):
                    raise ValueError('Invalid prepared Switch member')
                path = output / 'content' / name
                with self.forge.open_source(path) as f:
                    identity = self.forge.identity(path, f.fileno())
                if identity['size'] != member['size']:
                    raise ValueError('Prepared Switch member size mismatch')
                files.append(dict(index=index, name=name, size=member['size'], identity=identity))
            for source in state['source']:
                with self.forge.open_source(source['path']) as f:
                    if self.forge.identity(source['path'], f.fileno()) != source:
                        raise ValueError('Switch source changed during preparation')
            state.update(status='ready', phase='Switch content verified; ready for emulator import', files=files)
        except Exception as error:
            state.update(status='failed', error=str(error), phase='Switch preparation needs attention; sources kept')
        finally:
            if process is not None:
                if process.poll() is None: process.kill()
                process.wait(); process.stdout.close()
            with self.lock:
                try: self.save(directory, state)
                finally:
                    self.active.discard(hash_); os.close(fd)
