import importlib.util
import os
import json
from pathlib import Path
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location('bridge', Path(__file__).with_name('server.py'))
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

class BridgeTests(unittest.TestCase):
    def test_lost_add_response_recovered_without_adopting_foreign_destination(self):
        b = bridge.Bridge.__new__(bridge.Bridge); b.c = {'downloads': '/safe/downloads'}; b.owned = {}; b.save = lambda: None
        rows = []
        h = 'a' * 40
        def qb(route, fields=None):
            if fields is None: return json.dumps(rows).encode()
            rows.append({'save_path': fields['savepath'], 'category': fields['category']})
            raise TimeoutError('Accepted but response lost')
        b.qb = qb
        body = {'hash': h, 'filename': 'magnet:?xt=urn:btih:' + h}
        with self.assertRaises(TimeoutError): b.add(body)
        self.assertTrue(b.owned[h]['pending'])
        rows[0]['save_path'] = '/foreign'
        with self.assertRaises(ValueError): b.add(body)
        rows[0]['save_path'] = b.owned[h]['root']
        self.assertEqual(b.add(body), {'hash': h})
        self.assertNotIn('pending', b.owned[h])

    def test_stream_read_failure_closes_without_appending_error_response(self):
        import contextlib
        import http.client
        import threading
        from unittest.mock import patch
        with tempfile.TemporaryDirectory(dir=str(Path(tempfile.gettempdir()).resolve())) as tmp:
            Path(tmp, 'file').write_bytes(b'payload')
            b = bridge.Bridge.__new__(bridge.Bridge); b.c = {'token': 'a' * 64}
            b.request_operation = contextlib.nullcontext
            b.file_member = lambda *args: (tmp, {'name': 'file', 'size': 7})
            original = os.fdopen
            class FailingSource:
                def __init__(self, fd, mode): self.file = original(fd, mode); self.once = False
                def __enter__(self): return self
                def __exit__(self, *args): self.file.close()
                def fileno(self): return self.file.fileno()
                def seek(self, offset): return self.file.seek(offset)
                def read(self, size):
                    if self.once: raise OSError('disk read failed')
                    self.once = True
                    return self.file.read(3)
            server = bridge.BoundedHTTPServer(('127.0.0.1', 0), bridge.Handler); server.bridge = b
            worker = threading.Thread(target=server.serve_forever); worker.start()
            try:
                with patch.object(bridge.os, 'fdopen', FailingSource):
                    client = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=5)
                    client.request('GET', '/file/' + 'a' * 40 + '/0', headers={'Authorization': 'Bearer ' + b.c['token'], 'X-Botty-File-Name': 'file', 'X-Botty-File-Size': '7'})
                    response = client.getresponse()
                    self.assertEqual(response.status, 200)
                    with self.assertRaises(http.client.IncompleteRead) as error: response.read()
                    self.assertEqual(error.exception.partial, b'pay')
                    client.close()
            finally:
                server.shutdown(); server.server_close(); worker.join()

    def test_bounded_and_legacy_ranges(self):
        import contextlib
        import http.client
        import threading
        with tempfile.TemporaryDirectory() as tmp:
            tmp = str(Path(tmp).resolve())
            Path(tmp, 'file').write_bytes(b'0123456789')
            b = bridge.Bridge.__new__(bridge.Bridge); b.c = {'token': 'a' * 64}
            b.request_operation = contextlib.nullcontext
            b.file_member = lambda *args: (tmp, {'name': 'file', 'size': 10})
            server = bridge.BoundedHTTPServer(('127.0.0.1', 0), bridge.Handler); server.bridge = b
            worker = threading.Thread(target=server.serve_forever); worker.start()
            try:
                for range_, status, body, content_range in [
                    (None, 200, b'0123456789', None), ('bytes=3-', 206, b'3456789', 'bytes 3-9/10'),
                    ('bytes=0-3', 206, b'0123', 'bytes 0-3/10'), ('bytes=9-9', 206, b'9', 'bytes 9-9/10'),
                    ('bytes=3-2', 416, None, None), ('bytes=0-10', 416, None, None),
                    ('bytes=10-', 416, None, None), ('bytes=-3', 416, None, None), ('bytes=0-1,3-4', 416, None, None)]:
                    client = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=5)
                    headers = {'Authorization': 'Bearer ' + b.c['token'], 'X-Botty-File-Name': 'file', 'X-Botty-File-Size': '10'}
                    if range_: headers['Range'] = range_
                    client.request('GET', '/file/' + 'a' * 40 + '/0', headers=headers)
                    response = client.getresponse(); data = response.read()
                    self.assertEqual(response.status, status)
                    if body is not None:
                        self.assertEqual(data, body); self.assertEqual(int(response.getheader('Content-Length')), len(body))
                        self.assertEqual(response.getheader('Content-Range'), content_range)
                    client.close()
            finally:
                server.shutdown(); server.server_close(); worker.join()

    def test_metadata_hash(self):
        import hashlib
        info = b'd6:lengthi0e4:name4:test12:piece lengthi16384e6:pieces0:e'
        self.assertEqual(bridge.metadata_hash(b'd4:info' + info + b'e'), hashlib.sha1(info).hexdigest())
        legacy = b'd6:lengthi0e4:name1:\xff10:name.utf-85:caf\xc3\xa912:piece lengthi16384e6:pieces0:e'
        self.assertEqual(bridge.metadata_hash(b'd4:info' + legacy + b'e'), hashlib.sha1(legacy).hexdigest())
        for value in (b'', b'de', b'd4:info' + info + b'eTRAILING', b'd4:info' + info + b'4:info' + info + b'e'):
            with self.assertRaises((ValueError, KeyError)):
                bridge.metadata_hash(value)

    def test_one_magnet_only(self):
        b = bridge.Bridge.__new__(bridge.Bridge); b.c = {'downloads': '/safe/downloads'}; b.owned = {}; b.save = lambda: None
        added = []
        def qb(route, fields=None):
            if fields is None: return b'[]'
            added.append(fields); return b'Ok.'
        b.qb = qb
        h = 'a' * 40
        for suffix in ('\nhttps://example.com/other.torrent', '\r\nmagnet:?xt=urn:btih:' + 'b' * 40, '&xt=urn:btih:' + 'b' * 40):
            with self.assertRaises(ValueError): b.add({'hash': h, 'filename': 'magnet:?xt=urn:btih:' + h + suffix})
        self.assertFalse(added)
        b.add({'hash': h, 'filename': 'magnet:?xt=urn:btih:' + h.upper() + '&dn=Game%20PS5'})
        self.assertEqual(len(added), 1)
        b.owned[h] = {'root': '/stale', 'created': True}
        b.add({'hash': h, 'filename': 'magnet:?xt=urn:btih:' + h})
        self.assertEqual(len(added), 2)
        self.assertNotEqual(b.owned[h]['root'], '/stale')

    def test_discovery_resumes_uninspected_entry(self):
        b = bridge.Bridge.__new__(bridge.Bridge)
        rows = [{'hash': format(i, '040x'), 'name': 'Game PS5'} for i in range(32)]
        b.qb = lambda route: json.dumps(rows).encode()
        slow = [True]
        def info(h):
            if slow[0] and h != rows[0]['hash']: raise TimeoutError('scan budget exhausted')
            return {'save_path': '/safe'}, [{'name': 'game.rar', 'size': 1}], False
        b.info = info
        self.assertEqual(len(b.candidates()), 1)
        self.assertEqual(b.discovery_cursor, 1)
        slow[0] = False
        self.assertEqual(len(b.candidates()), 32)

    def test_slow_entry_does_not_starve_others(self):
        b = bridge.Bridge.__new__(bridge.Bridge)
        rows = [{'hash': format(i, '040x'), 'name': 'Game PS5'} for i in range(32)]
        b.qb = lambda route: json.dumps(rows).encode()
        calls = []
        def info(h):
            calls.append(h)
            if h == rows[0]['hash']: raise TimeoutError('consistently slow entry')
            return {'save_path': '/safe'}, [{'name': 'game.rar', 'size': 1}], False
        b.info = info
        self.assertFalse(b.candidates())
        self.assertEqual(b.discovery_cursor, 0)
        self.assertFalse(b.candidates())
        self.assertEqual(b.discovery_cursor, 1)
        self.assertEqual(len(b.candidates()), 31)
        self.assertEqual(calls.count(rows[0]['hash']), 2)

    def test_operation_deadline_restored(self):
        b = bridge.Bridge.__new__(bridge.Bridge)
        with b.operation_deadline(8):
            end = b.qb_deadline
            self.assertLessEqual(b.qb_timeout(), 8)
            with b.operation_deadline(20): self.assertEqual(b.qb_deadline, end)
            b.qb_deadline = time.monotonic() - 1
            with self.assertRaises(TimeoutError): b.qb_timeout()
        self.assertEqual(b.qb_timeout(), 15)

    def test_older_file_indices(self):
        b = bridge.Bridge.__new__(bridge.Bridge)
        t = {'amount_left': 0, 'progress': 1, 'state': 'uploading'}
        b.qb = lambda route: json.dumps([t] if 'info?' in route else [{'name': 'game.rar', 'size': 1, 'progress': 1}]).encode()
        _, files, complete = b.info('a' * 40)
        self.assertTrue(complete)
        self.assertEqual(files[0]['index'], 0)

    def test_file_inventory_cache(self):
        b = bridge.Bridge.__new__(bridge.Bridge)
        b.owned = {'a' * 40: {'root': '/safe'}}; b.file_cache = {}
        calls = []
        def snapshot(h):
            calls.append(h)
            return {'complete': True, 'files': [{'index': 0, 'name': 'a', 'size': 1}, {'index': 1, 'name': 'b', 'size': 2}]}
        b.snapshot = snapshot
        self.assertEqual(b.file_member('a' * 40, '0', 'a', 1)[1]['name'], 'a')
        self.assertEqual(b.file_member('a' * 40, '1', 'b', 2)[1]['name'], 'b')
        self.assertEqual(len(calls), 1)
        with self.assertRaises(ValueError): b.file_member('a' * 40, '0', 'b', 1)
        with self.assertRaises(ValueError): b.file_member('a' * 40, '0', 'a', 2)
        b.file_cache['a' * 40]['at'] -= 6
        b.file_member('a' * 40, '0', 'a', 1)
        self.assertEqual(len(calls), 2)
        b.owned['a' * 40]['root'] = '/other'
        self.assertEqual(b.file_member('a' * 40, '1', 'b', 2)[0], '/other')
        self.assertEqual(len(calls), 3)

    def test_candidates(self):
        files = [{'name': 'release/game.part01.rar'}, {'name': 'release/game.part02.rar'}]
        self.assertIsNone(bridge.compatibility('Movie 1080p', files))
        self.assertEqual(bridge.compatibility('Game.PS5.Release', files)['kind'], 'rar')
        self.assertIsNone(bridge.compatibility('Game PS5', files + [{'name': 'other.rar'}]))
        self.assertIsNone(bridge.compatibility('Game PS5', [{'name': 'game.pkg'}]))
        self.assertIsNone(bridge.compatibility('Game PS5', [{'name': 'game.part2.rar'}]))
        self.assertIsNone(bridge.compatibility('Game PS50', files))

    def test_folder_content(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d).resolve(); d = str(root)
            (root / 'folder/sce_sys').mkdir(parents=True)
            p = root / 'folder/sce_sys/param.json'
            p.write_text('{"titleId":"PPSA12345"}')
            files = [{'name': 'folder/sce_sys/param.json'}]
            self.assertEqual(bridge.compatibility('Some folder', files, d, True)['kind'], 'folder')
            for invalid in ('null', '123', '[]', '"string"'):
                p.write_text(invalid)
                self.assertIsNone(bridge.compatibility('PS5', files, d, True))
            p.write_text('{"titleId":"CUSA12345"}')
            self.assertIsNone(bridge.compatibility('PS5', files, d, True))

    def test_confined_open(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d).resolve(); d = str(root)
            (root / 'data').mkdir()
            (root / 'data/a file').write_bytes(b'hello')
            fd = bridge.open_member(d, 'data/a file')
            self.assertEqual(os.read(fd, 5), b'hello')
            os.close(fd)
            (root / 'link').symlink_to('/etc')
            with self.assertRaises(OSError):
                bridge.open_member(d, 'link/passwd')
            (root / 'leaf').symlink_to(root / 'data/a file')
            with self.assertRaises(OSError):
                bridge.open_member(d, 'leaf')
            for unsafe in ('../etc/passwd', '/etc/passwd', 'a//b', 'a/../b', 'a\\b'):
                with self.assertRaises(ValueError):
                    bridge.open_member(d, unsafe)

    def test_import_is_read_only(self):
        b = bridge.Bridge.__new__(bridge.Bridge)
        b.owned = {}
        b.info = lambda h: ({'name': 'Game PS5', 'save_path': '/existing/'}, [{'name': 'game.rar'}], True)
        b.save = lambda: None
        b.snapshot = lambda h: {'hash': h}
        self.assertEqual(b.import_torrent('a' * 40)['hash'], 'a' * 40)
        self.assertFalse(b.owned['a' * 40]['created'])
        b.info = lambda h: ({'name': 'Movie', 'save_path': '/existing/'}, [{'name': 'movie.mkv'}], True)
        with self.assertRaises(ValueError):
            b.import_torrent('b' * 40)
        self.assertNotIn('b' * 40, b.owned)

if __name__ == '__main__':
    unittest.main()
