#!/usr/bin/env python3
"""Seed generated test bytes on one explicit LAN address; no user files are read.

Writes a v1 .torrent, serves its HTTP tracker and implements the minimal BitTorrent
seed protocol. Stop with Ctrl-C after the comparison. Bind only to a trusted LAN.
"""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import socket
import socketserver
import struct
import re
import select
import threading
import time
import urllib.parse


def bencode(value):
    if isinstance(value, int):
        return b'i' + str(value).encode() + b'e'
    if isinstance(value, str):
        value = value.encode()
    if isinstance(value, bytes):
        return str(len(value)).encode() + b':' + value
    if isinstance(value, list):
        return b'l' + b''.join(map(bencode, value)) + b'e'
    return b'd' + b''.join(bencode(k) + bencode(v) for k, v in sorted(value.items())) + b'e'


def receive(sock, length):
    data = bytearray()
    while len(data) < length:
        part = sock.recv(length - len(data))
        if not part:
            raise EOFError()
        data.extend(part)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', required=True)
    parser.add_argument('--advertise', help='Address published to clients, defaults to bind address')
    parser.add_argument('--allow-client', action='append', default=[], help='Restrict both listeners to this source IP')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--size-mib', type=int, default=4096)
    parser.add_argument('--name', default='botty-lan-benchmark.bin')
    parser.add_argument('--multi', action='store_true', help='Generate nested, Unicode and empty-file test members')
    parser.add_argument('--peer-port', type=int, default=18082)
    parser.add_argument('--tracker-port', type=int, default=18081)
    args = parser.parse_args()
    if not 1 <= args.size_mib <= 32768:
        parser.error('size must be between 1 MiB and 32 GiB')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,100}', args.name):
        parser.error('name must be a plain ASCII filename')
    socket.inet_aton(args.bind)
    advertise = args.advertise or args.bind
    socket.inet_aton(advertise)
    block = hashlib.sha256(b'Botty isolated LAN benchmark v1').digest() * 32768
    count = args.size_mib
    info = {'length': count * len(block), 'name': args.name,
            'piece length': len(block), 'pieces': hashlib.sha1(block).digest() * count,
            'private': 1}
    if args.multi:
        total = info.pop('length')
        boundary = len(block) // 2 + 13
        info['name'] = 'botty-lan-multifile'
        info['files'] = [{'length': boundary, 'path': ['one.bin']},
                         {'length': 0, 'path': ['empty.txt']},
                         {'length': total - boundary, 'path': ['nested', 'résumé.bin']}]
    torrent = {'announce': f'http://{advertise}:{args.tracker_port}/announce', 'info': info}
    info_hash = hashlib.sha1(bencode(info)).digest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bencode(torrent))
    stats = {'bytesSent': 0, 'connections': 0, 'accepted': 0, 'invalidHandshakes': 0, 'messages': {}, 'started': time.time()}
    lock = threading.Lock()

    class Peer(socketserver.BaseRequestHandler):
        def handle(self):
            if args.allow_client and self.client_address[0] not in args.allow_client:
                return
            sock = self.request
            with lock:
                stats['accepted'] += 1
            sock.settimeout(45)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                handshake = receive(sock, 68)
                if handshake[:20] != b'\x13BitTorrent protocol' or handshake[28:48] != info_hash:
                    with lock:
                        stats['invalidHandshakes'] += 1
                    return
                reserved = b'\0' * 5 + b'\x10' + b'\0' * 2  # BEP 10
                sock.sendall(b'\x13BitTorrent protocol' + reserved + info_hash + b'-BB0001-000000000000')
                bits = b'\xff' * (count // 8)
                if count % 8:
                    bits += bytes([256 - (1 << (8 - count % 8))])
                sock.sendall(struct.pack('!I', len(bits) + 1) + b'\x05' + bits)
                sock.sendall(b'\0\0\0\x01\x01')  # unchoke
                if handshake[25] & 0x10:
                    extension = b'\x14\0' + bencode({'m': {}, 'reqq': 4096, 'v': 'Botty benchmark 1'})
                    sock.sendall(struct.pack('!I', len(extension)) + extension)
                with lock:
                    stats['connections'] += 1
                while True:
                    if not select.select([sock], [], [], 5)[0]:
                        sock.sendall(bytes(4))  # keepalive, including during startup
                        continue
                    size = struct.unpack('!I', receive(sock, 4))[0]
                    if size > 128 * 1024:
                        return
                    message = receive(sock, size)
                    if message:
                        with lock:
                            key = str(message[0])
                            stats['messages'][key] = stats['messages'].get(key, 0) + 1
                    if not message or message[0] != 6:
                        continue
                    if len(message) != 13:
                        return
                    index, offset, length = struct.unpack('!III', message[1:])
                    if index >= count or not 0 < length <= 128 * 1024 or offset + length > len(block):
                        return
                    sock.sendall(struct.pack('!IBII', length + 9, 7, index, offset) + block[offset:offset + length])
                    with lock:
                        stats['bytesSent'] += length
            except (OSError, EOFError):
                return

    class Tracker(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if args.allow_client and self.client_address[0] not in args.allow_client:
                self.send_error(403)
                return
            url = urllib.parse.urlsplit(self.path)
            if url.path == '/stats':
                with lock:
                    data = json.dumps(stats).encode()
                mime = 'application/json'
            elif url.path == '/announce':
                query = dict(part.split('=', 1) for part in url.query.split('&') if '=' in part)
                if urllib.parse.unquote_to_bytes(query.get('info_hash', '')) != info_hash:
                    self.send_error(400)
                    return
                data = bencode({'interval': 30, 'complete': 1, 'incomplete': 0,
                                'peers': socket.inet_aton(advertise) + struct.pack('!H', args.peer_port)})
                mime = 'text/plain'
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    peers = Server((args.bind, args.peer_port), Peer)
    tracker = http.server.ThreadingHTTPServer((args.bind, args.tracker_port), Tracker)
    threading.Thread(target=peers.serve_forever, daemon=True).start()
    print(json.dumps({'torrent': str(args.output), 'infoHash': info_hash.hex(), 'sizeMiB': count}), flush=True)
    try:
        tracker.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        peers.shutdown()
        peers.server_close()
        tracker.server_close()


if __name__ == '__main__':
    main()
