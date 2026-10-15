"""Server-only, journaled PS5 Dump Forge adapter. Never writes torrent sources."""
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import selectors
import time
import threading
import tempfile


class Forge:
    def __init__(self, binary, root):
        self.binary = str(Path(binary).resolve())
        if not Path(binary).is_absolute() or not os.access(self.binary, os.X_OK):
            raise ValueError('forgeBinary must be an absolute executable path')
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        if self.root.is_symlink():
            raise ValueError('Conversion storage must not be a symlink')
        self.lock = threading.RLock()
        self.active = set()

    def save(self, directory, state, name='state'):
        tmp = directory / (name + '.tmp')
        with tmp.open('w') as f:
            json.dump(state, f)
            f.flush(); os.fsync(f.fileno())
        os.replace(tmp, directory / (name + '.json'))
        fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try: os.fsync(fd)
        finally: os.close(fd)

    def poll(self, hash_, source, retry=False):
        if not re.fullmatch('[a-f0-9]{40}', hash_):
            raise ValueError('Invalid conversion identity')
        with self.lock, self.open_source(source) as pinned_source:
            directory = self.root / hash_
            directory.mkdir(mode=0o700, exist_ok=True)
            if directory.is_symlink():
                raise ValueError('Unsafe conversion directory')
            state_path = directory / 'state.json'
            state = json.loads(state_path.read_text()) if state_path.exists() else {}
            identity = self.identity(source, pinned_source.fileno())
            request_path = directory / 'request.json'
            pending = json.loads(request_path.read_text()) if request_path.exists() else None
            if pending and pending['source'] != identity and not retry:
                raise ValueError('Queued conversion source changed')
            retry = retry or bool(pending)
            def waiting():
                # Keep retry intent separate from the worker journal: an older
                # process may still own the global lock and finish its output.
                if retry and pending != {'source': identity}:
                    self.save(directory, {'source': identity}, 'request')
                return {'status': 'waiting', 'phase': 'Waiting for server converter', 'source': identity}
            if state and state['source'] != identity:
                # A failed/interrupted inspection with no output may select another variant.
                # Once any output exists, preserve it for explicit recovery.
                if (not retry or state.get('status') not in ('failed', 'running') or hash_ in self.active or
                        (directory / 'game.exfat').exists() or any(directory.glob('*.part'))):
                    raise ValueError('Conversion source changed; retained output needs inspection')
                state = {}
            if hash_ in self.active:
                return state
            if state.get('status') == 'ready':
                output = directory / 'game.exfat'
                if self.identity(output) != state['outputIdentity']:
                    raise ValueError('Converted image changed')
                request_path.unlink(missing_ok=True)
                return state
            if state and not retry:
                return dict(state, status='failed', error=state.get('error') or 'Server restarted during conversion; retry explicitly')
            if self.active:
                return waiting()
            # Inherited by Forge: a bridge restart must not race an orphan converter.
            fd = os.open(self.root / 'worker.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            try:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    queued = waiting()
                    os.close(fd)
                    return queued
                state = {'status': 'running', 'source': identity, 'phase': 'Inspecting package on server'}
                self.save(directory, state)
                request_path.unlink(missing_ok=True)
                source_fd = os.dup(pinned_source.fileno())
                self.active.add(hash_)
                try:
                    threading.Thread(target=self.run, args=(hash_, Path(source), directory, state, fd, source_fd), daemon=True).start()
                except Exception:
                    os.close(source_fd); self.active.discard(hash_); raise
            except Exception:
                os.close(fd)
                raise
            return state

    @staticmethod
    def identity(path, fd=None):
        info = os.fstat(fd) if fd is not None else os.stat(path, follow_symlinks=False)
        import stat
        if not stat.S_ISREG(info.st_mode):
            raise ValueError('Expected a regular package or image')
        return {'path': str(path), 'device': info.st_dev, 'inode': info.st_ino,
                'size': info.st_size, 'mtime': info.st_mtime_ns}

    @staticmethod
    def open_source(path):
        # Walk every component without following symlinks, then retain the inode
        # across inspection and conversion even if its pathname is replaced.
        absolute = Path(os.path.abspath(path))
        directory = os.open(absolute.anchor, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            for part in absolute.parts[1:-1]:
                child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
                os.close(directory); directory = child
            fd = os.open(absolute.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
        finally: os.close(directory)
        return os.fdopen(fd, 'rb')

    def inspect(self, path, fd, directory, source_fd=None):
        # Read a bounded JSON inventory, with a wall-clock timeout even if stdout stalls.
        process = subprocess.Popen([self.binary, 'inspect', str(path), '--json'], stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, pass_fds=(fd,) if source_fd is None else (fd, source_fd))
        data = bytearray()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + 300
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not selector.select(remaining):
                        raise ValueError('Dump Forge inspection timed out')
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk: break
                    data.extend(chunk)
                    if len(data) > 32 * 1024 * 1024:
                        raise ValueError('Dump Forge inventory exceeds the metadata limit')
            if process.wait(timeout=max(1, deadline-time.monotonic())):
                raise ValueError('Dump Forge could not inspect this package/image')
            data = json.loads(data)
        finally:
            if process.poll() is None: process.kill()
            process.wait()
            process.stdout.close()
        if not re.fullmatch(r'PPSA[0-9]{5}', data.get('title_id') or ''):
            raise ValueError('No PS5 game metadata found')
        if not any(f.get('path') == 'eboot.bin' for f in data.get('files', [])):
            raise ValueError('Not a standalone PS5 game: eboot.bin is missing')
        return data

    def run(self, hash_, source, directory, state, fd, source_fd):
        alias_directory = None
        try:
            # Only our private output is disposable, never a torrent member.
            output = directory / 'game.exfat'
            if output.exists() or any(directory.glob('*.part')):
                raise ValueError('Uncommitted output or partial image exists; inspect it before retrying')
            # Forge dispatches by extension; the alias names an inherited,
            # pinned descriptor, never a pathname that can be swapped later.
            alias_directory = tempfile.TemporaryDirectory(prefix='input-', dir=directory)
            converter_source = Path(alias_directory.name) / 'source.pkg'
            converter_source.symlink_to('/proc/self/fd/' + str(source_fd))
            info = self.inspect(converter_source, fd, directory, source_fd)
            if info['kind'] != 'pkg':
                raise ValueError('Expected a supported PS5 debug package')
            required = int(info['total_bytes'])
            # exFAT allocation overhead and a reserve; the writer also handles ENOSPC.
            if required < 0 or shutil.disk_usage(directory).free < required + required // 20 + 1024**3:
                raise ValueError('Not enough server space for the exFAT image')
            state.update(phase='Converting FPKG to exFAT on server', titleId=info['title_id'])
            with self.lock: self.save(directory, state)
            # Forge streams source -> image; no expanded folder and no full reread.
            result = subprocess.run([self.binary, 'convert', str(converter_source), '--to', 'exfat', '-o', str(output)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    stdin=subprocess.DEVNULL, pass_fds=(fd, source_fd), timeout=86400, check=False)
            if result.returncode:
                raise ValueError('Dump Forge conversion failed; original package kept')
            with self.open_source(source) as current_source:
                if self.identity(source, current_source.fileno()) != state['source']:
                    raise ValueError('Package changed during conversion')
            converted = self.inspect(output, fd, directory)
            if converted['kind'] != 'exfat' or converted['title_id'] != info['title_id']:
                raise ValueError('Converted image metadata does not match the source')
            with output.open('rb') as image:
                os.fsync(image.fileno())
            state.update(status='ready', phase='Converted image ready', outputIdentity=self.identity(output))
        except Exception as error:
            state.update(status='failed', error=str(error), phase='Server conversion needs attention')
        finally:
            if alias_directory:
                try: alias_directory.cleanup()
                except OSError: pass  # A leftover private alias holds no package data.
            with self.lock:
                try: self.save(directory, state)
                finally:
                    self.active.discard(hash_)
                    os.close(source_fd); os.close(fd)

    def output(self, hash_):
        return self.root / hash_ / 'game.exfat'
