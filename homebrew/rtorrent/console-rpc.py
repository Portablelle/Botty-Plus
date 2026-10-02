#!/usr/bin/env python3
"""Use the bounded FTP-backed probe to call PS5-local rTorrent during testing."""
import argparse
from ftplib import FTP, error_perm
from io import BytesIO
import json
from pathlib import Path
import socket
import time
import uuid

ROOT = '/data/botty/rtorrent-probe'


def read(ftp, path):
    output = BytesIO()
    ftp.retrbinary('RETR ' + path, output.write)
    return output.getvalue()


def request(host, method, params, timeout=15):
    ident = uuid.uuid4().hex
    body = json.dumps({'jsonrpc': '2.0', 'id': ident, 'method': method, 'params': params}).encode()
    with FTP() as ftp:
        ftp.connect(host, 2121, timeout=timeout)
        ftp.login()
        try:
            read(ftp, ROOT + '/request.json')
        except error_perm as exc:
            if not str(exc).startswith('550'):
                raise
        else:
            raise RuntimeError('Another diagnostic request is pending')
        ftp.storbinary('STOR ' + ROOT + '/request.tmp', BytesIO(body))
        ftp.rename(ROOT + '/request.tmp', ROOT + '/request.json')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                raw = read(ftp, ROOT + '/response.txt')
            except error_perm:
                time.sleep(.2)
                continue
            _, separator, payload = raw.partition(b'\r\n\r\n')
            if not separator:
                _, separator, payload = raw.partition(b'\n\n')
            try:
                response = json.loads(payload)
            except (ValueError, UnicodeError):
                time.sleep(.2)
                continue
            if response.get('id') == ident:
                if 'error' in response:
                    raise RuntimeError(json.dumps(response['error']))
                return response['result']
            time.sleep(.2)
        raise TimeoutError('No matching response from the console-local SCGI probe')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--launch-bridge', type=Path)
    parser.add_argument('--method', default='system.client_version')
    parser.add_argument('--params', default='[""]')
    args = parser.parse_args()
    if args.launch_bridge:
        data = args.launch_bridge.read_bytes()
        if data[:4] != b'\x7fELF':
            raise ValueError('Expected an ELF payload')
        with socket.create_connection((args.host, 9021), timeout=10) as sock:
            sock.sendall(data)
        time.sleep(.5)
    print(json.dumps(request(args.host, args.method, json.loads(args.params)), indent=2))


if __name__ == '__main__':
    main()
