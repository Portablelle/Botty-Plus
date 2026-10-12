#!/usr/bin/env python3
"""Compare PS5 import modes against one dedicated-server fixture after approval.

Default is an offline plan. --execute creates, hashes and deletes ONLY fresh
fixture imports; the dedicated-server original is preserved. Credentials remain
in the supplied private JSON file, never in result output.
"""
import argparse
import base64
import ftplib
import hashlib
import json
from pathlib import Path
import time
import urllib.request


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--hash', required=True)
    p.add_argument('--member', required=True)
    p.add_argument('--size', required=True, type=int)
    p.add_argument('--expected-sha256', required=True)
    p.add_argument('--config', type=Path, help='Private JSON: url, username, password, ftpHost, optional ftpPort')
    p.add_argument('--output', type=Path, default=Path('ddl-benchmark.json'))
    p.add_argument('--execute', action='store_true', help='Use only after PS5 installation/test approval')
    a = p.parse_args()
    if len(a.hash) != 40 or any(c not in '0123456789abcdef' for c in a.hash): p.error('Invalid fixture hash')
    if a.size <= 0 or len(a.expected_sha256) != 64 or any(c not in '0123456789abcdef' for c in a.expected_sha256): p.error('Expected fixture size and SHA-256 required')
    if not a.execute:
        print(json.dumps({'fixture': a.hash, 'member': a.member, 'size': a.size, 'connections': [1, 2, 4], 'status': 'prepared; execution requires approved installation'}, indent=2))
        return
    if not a.config: p.error('--config is required with --execute')
    c = json.loads(a.config.read_text())
    headers = {'Content-Type': 'application/json', 'Authorization': 'Basic ' + base64.b64encode((c['username'] + ':' + c['password']).encode()).decode()}
    def api(path, body=None):
        request = urllib.request.Request(c['url'].rstrip('/') + path, None if body is None else json.dumps(body).encode(), headers)
        with urllib.request.urlopen(request, timeout=45) as response: return json.load(response)
    headers['X-Botty-Token'] = api('/api/bootstrap')['token']
    def rows(): return api('/api/state')['torrents']
    if any(r['hashString'] == a.hash and not r.get('remoteCandidate') for r in rows()):
        raise RuntimeError('Fixture already imported/tracked; choose a fresh benchmark fixture')
    results = []
    for connections in (1, 2, 4):
        added = api('/api/remote/import', {'hash': a.hash, 'automatic': False, 'parallelConnections': connections})
        id_ = added['torrent-added']['id']; begin = time.monotonic()
        while True:
            row = next(r for r in rows() if r['id'] == id_)
            if row.get('errorString'): raise RuntimeError('Fixture import stopped: ' + row['errorString'])
            if row['totalSize'] != a.size or len(row['files']) != 1 or row['files'][0]['name'] != a.hash + '/' + a.member: raise RuntimeError('Benchmark requires the same single-file fixture')
            if row['remotePhase'] == 'ready': break
            if time.monotonic() - begin > 1800: raise RuntimeError('Fixture import timed out')
            time.sleep(.25)
        elapsed = time.monotonic() - begin
        digest = hashlib.sha256()
        ftp = ftplib.FTP(); ftp.connect(c['ftpHost'], c.get('ftpPort', 2121), timeout=60); ftp.login(); ftp.set_pasv(True)
        try: ftp.retrbinary('RETR ' + row['downloadDir'].rstrip('/') + '/' + a.hash + '/' + a.member, digest.update, blocksize=1024*1024)
        finally: ftp.close()
        if digest.hexdigest() != a.expected_sha256: raise RuntimeError('Fixture SHA-256 mismatch; retained for inspection')
        results.append({'connections': connections, 'seconds': round(elapsed, 3), 'MiBPerSecond': round(a.size / elapsed / 1048576, 3), 'sha256': digest.hexdigest()})
        a.output.write_text(json.dumps({'fixture': a.hash, 'member': a.member, 'size': a.size, 'measure': 'end-to-end import including scheduling/fsync/publication; hash read excluded', 'results': results}, indent=2) + '\n')
        api('/api/torrent', {'id': id_, 'action': 'pause'})
        api('/api/torrent', {'id': id_, 'action': 'remove-data', 'confirmed': True})
    print(json.dumps(results, indent=2))


if __name__ == '__main__': main()
