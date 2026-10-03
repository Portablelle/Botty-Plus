"""Loopback rTorrent JSON-RPC over SCGI fixture; no external daemon or files."""
import json
import socketserver
import threading


class RtorrentFixture(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, entries, hook=None):
        self.entries = entries
        self.calls = []
        self.errors = []
        self.hook = hook
        super().__init__(('127.0.0.1', 0), Handler)
        self.server_port = self.server_address[1]
        threading.Thread(target=self.serve_forever, daemon=True).start()

    def dispatch(self, request):
        self.calls.append(request)
        assert request['jsonrpc'] == '2.0'
        method, params = request['method'], request['params']
        if self.hook:
            result = self.hook(method, params)
            if result is not NotImplemented:
                return result
        if method == 'system.client_version':
            return '0.16.24'
        if method == 'session.save':
            return 0
        if method in ('load.start', 'load.normal'):
            return 0
        if method == 'd.multicall':
            assert params == ['', 'main', 'd.hash=', 'd.name=', 'd.is_active=', 'd.complete=', 'd.left_bytes=', 'd.size_bytes=', 'd.down.rate=', 'd.up.rate=', 'd.directory=', 'd.is_multi_file=', 'd.hashing=', 'd.is_hash_checked=', 'd.message=', 'd.peers_connected=']
            return [[t['hashString'], t['name'], int(t['status'] in (4, 6)),
                     int(t.get('leftUntilDone', 0) == 0), t.get('leftUntilDone', 0),
                     t['totalSize'], t.get('rateDownload', 0), t.get('rateUpload', 0),
                     t['downloadDir'], 0, int(t['status'] == 2),
                     int(all(f['bytesCompleted'] == f['length'] for f in t['files'])),
                     t.get('errorString', ''), t.get('peersConnected', 0)] for t in self.entries]
        t = next(t for t in self.entries if t['hashString'] == params[0])
        if method == 'f.multicall':
            assert params[1:] == ['', 'f.path=', 'f.size_bytes=', 'f.completed_chunks=', 'f.size_chunks=']
            return [[f['name'], f['length'], f['bytesCompleted'], f['length']] for f in t['files']]
        if method == 'p.multicall':
            assert params[1:] == ['', 'p.down_rate=', 'p.up_rate=']
            return [[int(i < t.get('peersSendingToUs', 0)), int(i < t.get('peersGettingFromUs', 0))]
                    for i in range(t.get('peersConnected', 0))]
        if method in ('d.stop', 'd.start', 'd.check_hash'):
            t['status'] = {'d.stop': 0, 'd.start': 6 if not t.get('leftUntilDone', 0) else 4, 'd.check_hash': 2}[method]
            return 0
        if method == 'd.close':
            return 0
        if method == 'd.directory.set':
            t['downloadDir'] = params[1]
            return 0
        if method == 'd.erase':
            self.entries.remove(t)
            return 0
        raise AssertionError('Unexpected rTorrent method: ' + method)

    def assert_clean(self):
        assert not self.errors, self.errors


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.connection.settimeout(5)
        request = {}
        try:
            digits = bytearray()
            while True:
                char = self.rfile.read(1)
                if char == b':':
                    break
                assert char and char.isdigit() and len(digits) < 8
                digits.extend(char)
            length = int(digits)
            assert 0 < length < 4096
            header = self.rfile.read(length)
            assert len(header) == length and header.endswith(b'\0')
            assert self.rfile.read(1) == b','
            fields = header[:-1].split(b'\0')
            assert len(fields) % 2 == 0
            headers = dict(zip(fields[::2], fields[1::2]))
            assert fields[0] == b'CONTENT_LENGTH'
            assert headers[b'SCGI'] == b'1' and headers[b'CONTENT_TYPE'] == b'application/json'
            size = int(headers[b'CONTENT_LENGTH'])
            assert 0 < size <= 4 * 1024 * 1024
            body = self.rfile.read(size)
            assert len(body) == size
            request = json.loads(body)
            response = {'jsonrpc': '2.0', 'id': request['id'], 'result': self.server.dispatch(request)}
        except Exception as exc:
            self.server.errors.append(repr(exc))
            response = {'jsonrpc': '2.0', 'id': request.get('id', 1), 'error': {'code': -32603, 'message': str(exc)}}
        data = json.dumps(response).encode()
        self.wfile.write(b'Status: 200 OK\r\nContent-Type: application/json\r\nContent-Length: ' + str(len(data)).encode() + b'\r\n\r\n' + data)
