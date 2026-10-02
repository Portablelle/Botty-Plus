import hashlib
import importlib.util
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import urllib.request

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('seed', ROOT / 'benchmark-seed.py')
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class WireTest(unittest.TestCase):
    def test_seed_transfers_verified_piece_and_rejects_out_of_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            peer, tracker = port(), port()
            process = subprocess.Popen([sys.executable, str(ROOT / 'benchmark-seed.py'),
                '--bind', '127.0.0.1', '--output', str(Path(directory) / 'test.torrent'),
                '--size-mib', '2', '--peer-port', str(peer), '--tracker-port', str(tracker)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                metadata = json.loads(process.stdout.readline())
                info_hash = bytes.fromhex(metadata['infoHash'])
                with socket.create_connection(('127.0.0.1', peer), timeout=5) as s:
                    s.sendall(b'\x13BitTorrent protocol' + bytes(5) + b'\x10' + bytes(2) + info_hash + b'-TEST00-000000000000')
                    self.assertEqual(seed.receive(s, 68)[28:48], info_hash)
                    def message():
                        return seed.receive(s, struct.unpack('!I', seed.receive(s, 4))[0])
                    self.assertEqual(message(), b'\x05\xc0')
                    self.assertEqual(message(), b'\x01')
                    extension = message()
                    self.assertTrue(extension.startswith(b'\x14\0d'))
                    self.assertIn(b'4:reqqi4096e', extension)
                    # Pipeline all requests to exercise the benchmark's real path.
                    s.sendall(b''.join(struct.pack('!IBIII', 13, 6, 1, off, 16384)
                                      for off in range(0, 1048576, 16384)))
                    piece = bytearray()
                    for off in range(0, 1048576, 16384):
                        packet = message()
                        self.assertEqual(packet[:9], struct.pack('!BII', 7, 1, off))
                        piece.extend(packet[9:])
                    expected = hashlib.sha256(b'Botty isolated LAN benchmark v1').digest() * 32768
                    self.assertEqual(hashlib.sha1(piece).digest(), hashlib.sha1(expected).digest())
                    s.sendall(struct.pack('!IBIII', 13, 6, 2, 0, 16384))
                    self.assertEqual(s.recv(1), b'')
                with urllib.request.urlopen(f'http://127.0.0.1:{tracker}/stats') as response:
                    self.assertEqual(json.load(response)['bytesSent'], 1048576)
            finally:
                process.terminate()
                process.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main()
