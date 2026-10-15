#!/usr/bin/env python3
"""Private qBittorrent bridge. Imported torrents are read-only; file URLs are IDs."""
import base64
import contextlib
import hashlib
import hmac
import http.cookiejar
import json
import os
import re
import stat
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path, PurePosixPath
import importlib.util
_forge_spec = importlib.util.spec_from_file_location('botty_forge', Path(__file__).with_name('forge.py'))
_forge_module = importlib.util.module_from_spec(_forge_spec)
_forge_spec.loader.exec_module(_forge_module)
Forge = _forge_module.Forge

HASH = re.compile(r'[a-f0-9]{40}')

def metadata_hash(data):
    def decode(at, depth=0):
        if depth > 64 or at >= len(data):
            raise ValueError('Invalid torrent metadata')
        c = data[at:at+1]
        if c in (b'd', b'l'):
            at += 1; values = {} if c == b'd' else []
            while at < len(data) and data[at:at+1] != b'e':
                if c == b'd':
                    key, at = decode(at, depth + 1)
                    if not isinstance(key, bytes) or key in values:
                        raise ValueError('Duplicate/invalid torrent key')
                    value, at = decode(at, depth + 1); values[key] = value
                else:
                    value, at = decode(at, depth + 1); values.append(value)
            if at >= len(data):
                raise ValueError('Truncated torrent')
            return values, at + 1
        if c == b'i':
            end = data.index(b'e', at)
            value = data[at+1:end]
            if not re.fullmatch(rb'-?(0|[1-9][0-9]*)', value):
                raise ValueError('Invalid integer')
            return int(value), end + 1
        colon = data.index(b':', at)
        digits = data[at:colon]
        if len(digits) > 10 or not re.fullmatch(rb'0|[1-9][0-9]*', digits):
            raise ValueError('Invalid string')
        end = colon + 1 + int(digits)
        if end > len(data):
            raise ValueError('Truncated string')
        return data[colon+1:end], end
    if data[:1] != b'd':
        raise ValueError('Invalid torrent')
    at = 1; info_hash = None; seen = set()
    while at < len(data) and data[at:at+1] != b'e':
        key, at = decode(at)
        if not isinstance(key, bytes) or key in seen:
            raise ValueError('Invalid torrent key')
        seen.add(key); start = at; value, at = decode(at)
        if key == b'info':
            name = value[b'name'].decode('latin-1')
            if len(relative(name).parts) != 1:
                raise ValueError('Unsafe torrent name')
            if b'name.utf-8' in value and len(relative(value[b'name.utf-8'].decode('utf-8')).parts) != 1:
                raise ValueError('Unsafe UTF-8 torrent name')
            for file in value.get(b'files', []):
                for parts, encoding in ((file[b'path'], 'latin-1'), (file.get(b'path.utf-8', []), 'utf-8')):
                    for part in parts:
                        if len(relative(part.decode(encoding)).parts) != 1:
                            raise ValueError('Unsafe torrent component')
                if file[b'length'] < 0:
                    raise ValueError('Negative torrent size')
            info_hash = hashlib.sha1(data[start:at]).hexdigest()
    if at != len(data) - 1 or data[at:] != b'e' or not info_hash:
        raise ValueError('Invalid torrent metadata')
    return info_hash


def relative(name):
    parts = name.split('/')
    if not name or any(p in ('', '.', '..') for p in parts) or '\\' in name or '\0' in name:
        raise ValueError('Unsafe torrent path')
    return PurePosixPath(name)

def open_member(root, name):
    """Walk with O_NOFOLLOW, including the absolute root's ancestors."""
    parts = Path(root).parts
    if not Path(root).is_absolute():
        raise ValueError('Expected absolute torrent root')
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in list(parts[1:]) + list(relative(name).parts[:-1]):
            nxt = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = nxt
        result = os.open(relative(name).name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
        if not stat.S_ISREG(os.fstat(result).st_mode):
            os.close(result)
            raise ValueError('Not a regular torrent file')
        return result
    finally:
        os.close(fd)

def compatibility(name, files, root=None, complete=False):
    names = [str(relative(f['name'])) for f in files]
    archives = [n for n in names if n.lower().endswith('.rar') and
                (not re.search(r'\.part\d+\.rar$', n, re.I) or re.search(r'\.part0*1\.rar$', n, re.I))]
    if re.search(r'(?<![a-z0-9])ps5(?![a-z0-9])', name, re.I) and len(archives) == 1:
        return {'kind': 'rar', 'verified': False, 'reason': 'PS5 archive candidate; content checked after extraction'}
    packages = [n for n in names if n.lower().endswith(('.pkg', '.fpkg'))]
    if packages:
        return {'kind': 'pkg', 'verified': False, 'reason': 'Package; server conversion required'}
    # Completed loose folders/images require content evidence, not release names.
    candidates = []
    if complete and root:
        for n in names:
            if n.endswith('/sce_sys/param.json') or n == 'sce_sys/param.json':
                fd = open_member(root, n)
                with os.fdopen(fd, 'rb') as f:
                    data = json.loads(f.read(65537))
                if not isinstance(data, dict):
                    continue
                title = next((data[k] for k in ('titleId', 'title_id', 'TITLE_ID') if k in data), '')
                executable = str(PurePosixPath(n).parent.parent / 'eboot.bin')
                if isinstance(title, str) and re.fullmatch(r'PPSA[0-9]{5}', title) and executable in names:
                    fd = open_member(root, executable); os.close(fd)
                    candidates.append({'kind': 'folder', 'verified': True, 'reason': 'PS5 application metadata'})
            elif n.lower().endswith(('.exfat', '.ffpkg', '.ffpfs', '.ffpfsc')):
                fd = open_member(root, n)
                with os.fdopen(fd, 'rb') as f:
                    ext = Path(n).suffix.lower()
                    header = f.read(64)
                    valid = ext == '.exfat' and header[3:11] == b'EXFAT   '
                    if ext in ('.ffpfs', '.ffpfsc') and len(header) == 64:
                        block = int.from_bytes(header[32:36], 'little')
                        valid = (int.from_bytes(header[:8], 'little') in (1, 2) and
                                 int.from_bytes(header[8:16], 'little') == 20130315 and
                                 4096 <= block <= 65536 and block & (block-1) == 0)
                    if ext == '.ffpkg':
                        f.seek(65536 + 1372)
                        valid = int.from_bytes(f.read(4), 'little') == 0x19540119
                if valid and re.search(r'(?<![a-z0-9])ps5(?![a-z0-9])', name, re.I):
                    candidates.append({'kind': ext[1:], 'verified': False, 'reason': 'Image header recognized; ShadowMount validates the game'})
    return candidates[0] if len(candidates) == 1 else None

class Bridge:
    def __init__(self, config):
        self.c = config
        self.lock = threading.RLock()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.registry = Path(config['registry'])
        self.owned = json.loads(self.registry.read_text()) if self.registry.exists() else {}
        self.file_cache = {}
        self.forge = Forge(config['forgeBinary'], self.registry.parent / 'conversions') if config.get('forgeBinary') else None
        self.login()

    def login(self):
        result = self.qb('auth/login', {'username': self.c['username'], 'password': self.c['password']}, retry=False)
        if result != b'Ok.':
            raise RuntimeError('qBittorrent authentication failed')

    @contextlib.contextmanager
    def request_operation(self):
        # Reject busy requests before mutation instead of queuing past client deadlines.
        if not self.lock.acquire(timeout=2):
            raise BridgeBusy()
        try:
            with self.operation_deadline(20):
                yield
        finally:
            self.lock.release()

    @contextlib.contextmanager
    def operation_deadline(self, seconds):
        previous = getattr(self, 'qb_deadline', None)
        end = time.monotonic() + seconds
        self.qb_deadline = min(previous, end) if previous is not None else end
        try:
            yield
        finally:
            self.qb_deadline = previous

    def qb_timeout(self):
        end = getattr(self, 'qb_deadline', None)
        remaining = 15 if end is None else end - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('qBittorrent operation deadline reached')
        return min(15, remaining)

    def qb(self, route, fields=None, retry=True):
        data = None if fields is None else urllib.parse.urlencode(fields).encode()
        req = urllib.request.Request(self.c['qbUrl'] + '/api/v2/' + route, data=data)
        try:
            with self.opener.open(req, timeout=self.qb_timeout()) as r:
                result = r.read(16 * 1024 * 1024 + 1)
                if len(result) > 16 * 1024 * 1024:
                    raise ValueError('qBittorrent response exceeds limit')
                return result
        except urllib.error.HTTPError as e:
            if e.code == 403 and retry:
                self.login()
                return self.qb(route, fields, False)
            raise
        except urllib.error.URLError as e:
            if isinstance(e.reason, TimeoutError):
                raise TimeoutError('qBittorrent operation timed out') from e
            raise

    def info(self, hash):
        if not HASH.fullmatch(hash):
            raise ValueError('Invalid torrent hash')
        rows = json.loads(self.qb('torrents/info?hashes=' + hash))
        if len(rows) != 1:
            raise ValueError('Torrent unavailable')
        t = rows[0]
        files = json.loads(self.qb('torrents/files?hash=' + hash))
        if not files or len(files) > 200000:
            raise ValueError('Torrent metadata unavailable or too large')
        seen = set()
        for index, f in enumerate(files):
            f.setdefault('index', index)
            n = str(relative(f['name']))
            if n in seen or f['size'] < 0:
                raise ValueError('Invalid torrent inventory')
            seen.add(n)
        complete = t['amount_left'] == 0 and t['progress'] == 1 and all(f['progress'] == 1 for f in files)
        complete = complete and not t['state'].startswith(('checking', 'moving', 'error', 'missing'))
        return t, files, complete

    def snapshot(self, hash, require_owned=True):
        if require_owned and hash not in self.owned:
            raise ValueError('Torrent has not been imported into Botty')
        t, files, complete = self.info(hash)
        if require_owned and (Path(t['save_path']) != Path(self.owned[hash]['root'])):
            raise ValueError('Torrent location changed; import again explicitly')
        if require_owned and self.owned[hash].get('pending') and t.get('category') != 'botty-ps5':
            raise ValueError('Pending add does not match the Botty category')
        if require_owned and not self.owned[hash].get('package'):
            unpacked = [f for f in files if not f['name'].lower().endswith(('.pkg', '.fpkg'))]
            if unpacked and len(unpacked) != len(files):
                content = compatibility(t['name'], unpacked, t['save_path'], complete)
                if content and content['kind'] != 'pkg':
                    # Keep ancillary updates/DLC on the server. They must not
                    # replace the game archive/folder or be transferred to PS5.
                    files = unpacked
        result = {'capabilities': {'boundedRanges': True, 'fpkgToExfat': bool(getattr(self, 'forge', None))}, 'hash': hash, 'name': t['name'], 'complete': complete, 'paused': t['state'].startswith('paused'),
                'total': sum(f['size'] for f in files), 'done': sum(int(f['size'] * f['progress']) for f in files),
                'rate': t['dlspeed'], 'upload': t['upspeed'], 'peers': t['num_seeds'] + t['num_leechs'],
                'files': [{'index': f['index'], 'name': f['name'], 'size': f['size']} for f in files]}
        packages = [f for f in files if f['name'].lower().endswith(('.pkg', '.fpkg'))]
        if packages and require_owned:
            result['complete'] = False  # A raw package must never be sent to the PS5.
            result['preparation'] = {'status': 'waiting', 'phase': 'Waiting for package download'}
            if complete:
                forge = getattr(self, 'forge', None)
                if not self.owned[hash].get('automatic', False):
                    result['preparation'] = {'status': 'download-only', 'phase': 'Package kept on server; choose Prepare to convert', 'packages': [p['name'] for p in packages]}
                elif not forge:
                    result['preparation'] = {'status': 'failed', 'error': 'Configure forgeBinary on the dedicated server', 'phase': 'Server conversion unavailable'}
                else:
                    selected = self.owned[hash].get('package')
                    choices = [p for p in packages if p['name'] == selected] if selected else packages
                    if len(choices) != 1:
                        result['preparation'] = {'status': 'failed', 'phase': 'Choose a package',
                            'error': 'Multiple packages: choose the game or backport to convert',
                            'packages': [p['name'] for p in packages]}
                        return result
                    package = choices[0]
                    fd = open_member(t['save_path'], package['name'])
                    os.close(fd)
                    state = forge.poll(hash, Path(t['save_path']) / package['name'])
                    result['preparation'] = dict(state, packages=[p['name'] for p in packages])
                    if state['status'] == 'ready':
                        size = state['outputIdentity']['size']
                        result.update(complete=True, total=size, done=size,
                                      files=[{'index': 0, 'name': state['titleId'] + '.exfat', 'size': size}], converted=True)
                        published = dict(name=state['titleId'] + '.exfat', size=size, identity=state['outputIdentity'])
                        if self.owned[hash].get('preparedImage') != published:
                            self.owned[hash]['preparedImage'] = published
                            self.save()
        return result


    def file_member(self, hash, index, expected_name, expected_size):
        # Avoid fetching the entire torrent inventory for every small file.
        published = self.owned[hash].get('preparedImage')
        if published:
            forge = getattr(self, 'forge', None)
            if not forge or str(index) != '0' or expected_name != published['name'] or expected_size != published['size']:
                raise ValueError('Converted image manifest changed')
            output = forge.output(hash)
            if Forge.identity(output) != published['identity']:
                raise ValueError('Converted image changed')
            return str(output.parent), {'index': 0, 'name': output.name, 'size': published['size']}
        root = self.owned[hash]['root']
        now = time.monotonic()
        cached = self.file_cache.get(hash)
        if not cached or cached['root'] != root or now - cached['at'] >= 5:
            snapshot = self.snapshot(hash)
            if not snapshot['complete']:
                raise ValueError('All torrent files must be complete')
            if self.owned[hash].get('preparedImage'):
                return self.file_member(hash, index, expected_name, expected_size)
            cached = {'root': root, 'at': now,
                      'files': {str(f['index']): f for f in snapshot['files']}}
            self.file_cache.pop(hash, None)
            while len(self.file_cache) >= 4:
                del self.file_cache[next(iter(self.file_cache))]
            self.file_cache[hash] = cached
        file = cached['files'][index]
        if file['name'] != expected_name or file['size'] != expected_size:
            raise ValueError('Pinned file manifest disagrees with server')
        return root, file

    def save(self):
        self.registry.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.registry.with_suffix('.tmp')
        tmp.write_text(json.dumps(self.owned))
        with tmp.open('rb') as f:
            os.fsync(f.fileno())
        os.replace(tmp, self.registry)

    def candidates(self):
        with self.operation_deadline(8):
            return self.scan_candidates()

    def scan_candidates(self):
        rows = json.loads(self.qb('torrents/info?filter=all'))
        if len(rows) > 4096:
            raise ValueError('Too many torrents to discover')
        live = {t['hash'] for t in rows}
        cache = getattr(self, 'discovery_cache', {})
        cache = {h: v for h, v in cache.items() if h in live}
        retries = {h: v for h, v in getattr(self, 'discovery_retries', {}).items() if h in live}
        cursor = getattr(self, 'discovery_cursor', 0) % max(1, len(rows))
        batch = (rows[cursor:] + rows[:cursor])[:32]
        scanned = 0
        for t in batch:
            if retries.get(t['hash'], {}).get('after', 0) > time.monotonic():
                scanned += 1
                continue
            try:
                self.qb_timeout()
            except TimeoutError:
                break
            # Bound discovery: loose metadata/image checks only on completed entries.
            try:
                info, files, complete = self.info(t['hash'])
                kind = compatibility(t['name'], files, info['save_path'], complete)
                retries.pop(t['hash'], None)
                if kind:
                    cache[t['hash']] = {'hash': t['hash'], 'name': t['name'], 'size': sum(f['size'] for f in files),
                                   'complete': complete, 'compatibility': kind}
                else:
                    cache.pop(t['hash'], None)
            except TimeoutError:
                count = retries.get(t['hash'], {}).get('count', 0) + 1
                retries[t['hash']] = {'count': count, 'after': time.monotonic() + 30 if count >= 2 else 0}
                if count >= 2:
                    scanned += 1  # Back off this entry so the next poll can inspect others.
                break
            except (ValueError, OSError, KeyError):
                pass
            scanned += 1
        self.discovery_retries = retries
        self.discovery_cursor = cursor + scanned
        self.discovery_cache = cache
        return list(cache.values())

    def import_torrent(self, hash, automatic=True):
        t, files, complete = self.info(hash)
        kind = compatibility(t['name'], files, t['save_path'], complete)
        if not kind:
            raise ValueError('No supported PS5 content detected')
        if hash not in self.owned:
            self.owned[hash] = {'root': t['save_path'], 'created': False}
        else:
            self.owned[hash]['root'] = t['save_path']
        self.owned[hash]['automatic'] = automatic
        self.save()
        return self.snapshot(hash)

    def prepare(self, hash, package=None):
        if hash not in self.owned:
            raise ValueError('Import the torrent first')
        t, files, complete = self.info(hash)
        if Path(t['save_path']) != Path(self.owned[hash]['root']):
            raise ValueError('Source location changed')
        packages = [f for f in files if f['name'].lower().endswith(('.pkg', '.fpkg'))]
        choices = [p for p in packages if p['name'] == package] if package else packages
        if not complete or len(choices) != 1 or not self.forge:
            raise ValueError('One completed package and Dump Forge are required')
        package = choices[0]
        fd = open_member(t['save_path'], package['name']); os.close(fd)
        state = self.forge.poll(hash, Path(t['save_path']) / package['name'], retry=True)
        self.owned[hash]['package'] = package['name']
        self.owned[hash]['automatic'] = True
        self.save()
        return state

    def add(self, body):
        hash = body['hash']
        if not HASH.fullmatch(hash):
            raise ValueError('Invalid hash')
        existing = json.loads(self.qb('torrents/info?hashes=' + hash))
        if hash in self.owned:
            if existing:
                if self.owned[hash].get('pending'):
                    if Path(existing[0]['save_path']) != Path(self.owned[hash]['root']) or existing[0].get('category') != 'botty-ps5':
                        raise ValueError('Pending add does not match the Botty destination')
                    self.owned[hash].pop('pending', None)
                    self.save()
                    return {'hash': hash}
                return self.snapshot(hash)
            del self.owned[hash]
            self.save()
        # Never adopt an unrelated existing torrent as a side effect of adding.
        if existing:
            raise ValueError('Torrent already exists on server; use Import from candidates')
        root = str(Path(self.c['downloads']) / hash) + '/'
        fields = {'savepath': root, 'autoTMM': 'false', 'category': 'botty-ps5', 'paused': 'false'}
        def record_intent():
            # Persist before dispatch: an accepted add can lose its HTTP response.
            self.owned[hash] = {'root': root, 'created': True, 'pending': True, 'automatic': body.get('automatic', False)}
            self.save()
        if 'filename' in body:
            magnet = body['filename']
            if len(magnet) > 16384 or any(ord(c) < 32 or ord(c) == 127 for c in magnet) or not re.match(r'^magnet:\?', magnet) or [xt.lower() for xt in urllib.parse.parse_qs(urllib.parse.urlsplit(magnet).query).get('xt', [])] != ['urn:btih:' + hash]:
                raise ValueError('Invalid magnet')
            fields['urls'] = magnet
            record_intent()
            result = self.qb('torrents/add', fields)
        else:
            data = base64.b64decode(body['metainfo'], validate=True)
            if len(data) > 1024 * 1024 or metadata_hash(data) != hash:
                raise ValueError('Torrent too large')
            # Multipart body only to the trusted loopback qBittorrent endpoint.
            boundary = 'botty-' + os.urandom(16).hex()
            chunks = []
            for key, value in fields.items():
                chunks.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"\r\n\r\n{value}\r\n'.encode())
            chunks.append(f'--{boundary}\r\nContent-Disposition: form-data; name="torrents"; filename="upload.torrent"\r\nContent-Type: application/x-bittorrent\r\n\r\n'.encode() + data + f'\r\n--{boundary}--\r\n'.encode())
            req = urllib.request.Request(self.c['qbUrl'] + '/api/v2/torrents/add', b''.join(chunks),
                                         {'Content-Type': 'multipart/form-data; boundary=' + boundary})
            record_intent()
            try:
                with self.opener.open(req, timeout=self.qb_timeout()) as r:
                    result = r.read(1024)
            except urllib.error.HTTPError as e:
                if e.code != 403:
                    raise
                self.login()
                with self.opener.open(req, timeout=self.qb_timeout()) as r:
                    result = r.read(1024)
        if result != b'Ok.':
            del self.owned[hash]
            self.save()
            raise ValueError('qBittorrent rejected torrent')
        self.owned[hash] = {'root': root, 'created': True, 'automatic': body.get('automatic', False)}
        self.save()
        return {'hash': hash}

class BridgeBusy(Exception):
    pass

class BoundedHTTPServer(ThreadingHTTPServer):
    def __init__(self, *args, **kwargs):
        self.slots = threading.BoundedSemaphore(24)
        super().__init__(*args, **kwargs)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()

class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    timeout = 30
    file_slots = threading.BoundedSemaphore(4)

    @contextlib.contextmanager
    def file_transfer(self):
        if not self.file_slots.acquire(blocking=False):
            raise BridgeBusy()
        try:
            yield
        finally:
            self.file_slots.release()
    def log_message(self, *args):
        pass  # Do not log credentials or bearer URLs.

    def reply(self, status, data):
        encoded = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(encoded)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(encoded)

    def handle_request(self):
        b = self.server.bridge
        streaming = False
        if not hmac.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + b.c['token']):
            self.close_connection = True
            return self.reply(401, {'error': 'Unauthorized'})
        route = urllib.parse.urlsplit(self.path).path.split('/')[1:]
        try:
            if self.command == 'GET' and route == ['candidates']:
                with b.request_operation():
                    result = b.candidates()
                return self.reply(200, result)
            if self.command == 'GET' and len(route) == 2 and route[0] == 'torrent':
                with b.request_operation():
                    result = b.snapshot(route[1])
                return self.reply(200, result)
            if self.command == 'GET' and len(route) == 3 and route[0] == 'file':
                with self.file_transfer():
                    with b.request_operation():
                        encoded_name = self.headers.get('X-Botty-File-Name', '')
                        size = self.headers.get('X-Botty-File-Size', '')
                        if not encoded_name or len(encoded_name) > 12288 or not re.fullmatch(r'[0-9]{1,20}', size):
                            raise ValueError('Pinned file identity required')
                        name = urllib.parse.unquote(encoded_name, errors='strict')
                        root, file = b.file_member(route[1], route[2], name, int(size))
                        fd = open_member(root, file['name'])
                    with os.fdopen(fd, 'rb') as source:
                        if os.fstat(source.fileno()).st_size != file['size']:
                            raise ValueError('File size changed')
                        start = 0
                        end = file['size'] - 1
                        range_header = self.headers.get('Range')
                        if range_header:
                            m = re.fullmatch(r'bytes=(\d+)-(\d*)', range_header)
                            if not m or int(m[1]) >= file['size'] or (m[2] and (int(m[2]) < int(m[1]) or int(m[2]) >= file['size'])):
                                return self.reply(416, {'error': 'Invalid range'})
                            start = int(m[1])
                            if m[2]: end = int(m[2])
                        streaming = True
                        self.send_response(206 if range_header else 200)
                        self.send_header('Content-Type', 'application/octet-stream')
                        self.send_header('Accept-Ranges', 'bytes')
                        self.send_header('Content-Length', str(end - start + 1))
                        if range_header:
                            self.send_header('Content-Range', f"bytes {start}-{end}/{file['size']}")
                        self.end_headers()
                        source.seek(start)
                        left = end - start + 1
                        while left:
                            chunk = source.read(min(left, 1024 * 1024))
                            if not chunk:
                                self.close_connection = True
                                break
                            self.wfile.write(chunk)
                            left -= len(chunk)
                    return
            if self.command == 'POST':
                length = int(self.headers.get('Content-Length', 0))
                if length <= 0 or length > 2 * 1024 * 1024:
                    raise ValueError('Invalid request size')
                body = json.loads(self.rfile.read(length))
                with b.request_operation():
                    if route == ['add']:
                        result = b.add(body)
                    elif route == ['import']:
                        result = b.import_torrent(body['hash'], body.get('automatic', True))
                    elif route == ['prepare']:
                        result = b.prepare(body['hash'], body.get('package'))
                    elif route == ['action']:
                        hash = body['hash']; action = body['action']
                        if hash not in b.owned or action not in ('pause', 'resume'):
                            raise ValueError('Unexpected torrent control')
                        b.snapshot(hash)
                        b.qb('torrents/' + action, {'hashes': hash})
                        result = {'ok': True}
                    else:
                        raise ValueError('Unknown operation')
                return self.reply(200, result)
            self.reply(404, {'error': 'Not found'})
        except BridgeBusy:
            self.close_connection = True
            self.reply(429, {"error": "Request capacity reached; retry later"})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True
        except Exception:
            if streaming:
                self.close_connection = True
            else:
                self.reply(400, {'error': 'Remote request failed; torrent missing, incomplete, unsupported or inaccessible'})

    do_GET = handle_request
    do_POST = handle_request

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    args = parser.parse_args()
    os.umask(0o077)
    config = json.loads(Path(args.config).read_text())
    if config['qbUrl'] != 'http://127.0.0.1:8080' or not re.fullmatch(r'[a-f0-9]{64}', config['token']):
        raise ValueError('Expected loopback qBittorrent and a 256-bit token')
    server = BoundedHTTPServer(('127.0.0.1', config.get('port', 9698)), Handler)
    server.bridge = Bridge(config)
    server.serve_forever()
