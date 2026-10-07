#!/usr/bin/env python3
"""Exercise the production adapter against started-but-inactive rTorrent state."""
import json
import pathlib
import subprocess
import tempfile
from rtorrent_fixture import RtorrentFixture

ROOT = pathlib.Path(__file__).resolve().parents[1]


def check(started, status, expected_resume, method='torrent-start', incomplete=False, fail_command=None):
    with tempfile.TemporaryDirectory(prefix='botty-resume-') as directory:
        root = pathlib.Path(directory)
        complete = root/'downloads/complete'
        complete.mkdir(parents=True)
        torrent = dict(hashString='a'*40, name='fixture', status=status,
                       leftUntilDone=4 if incomplete else 0, totalSize=8,
                       downloadDir=str(complete),
                       files=[dict(name='fixture', length=8,
                                   bytesCompleted=4 if incomplete else 8)])

        def hook(command, params):
            nonlocal started
            if command == fail_command:
                raise RuntimeError('Injected resume RPC failure')
            if command == 'd.start':
                if not started:
                    started = True
                    torrent['status'] = 4 if incomplete else 6
                return 0
            if command == 'd.resume':
                torrent['status'] = 4 if incomplete else 6
                return 0
            return NotImplemented

        rpc = RtorrentFixture([torrent], hook)
        try:
            result = subprocess.run([str(ROOT/'build/rtorrent-resume-client'),
                                     str(root), str(rpc.server_port), method],
                                    capture_output=True, text=True, timeout=15)
            if fail_command:
                assert result.returncode == 1 and not result.stdout, result
                assert 'rTorrent: Injected resume RPC failure' in result.stderr, result.stderr
                assert len(rpc.errors) == 1 and 'Injected resume RPC failure' in rpc.errors[0], rpc.errors
                return
            assert result.returncode == 0, result.stderr
            state = json.loads(result.stdout)['torrents'][0]
            assert state['status'] == (2 if status == 2 else 4 if incomplete else 6), state
            commands = [call['method'] for call in rpc.calls]
            assert commands.count('d.start') == 1, commands
            assert commands.count('d.resume') == expected_resume, commands
            assert 'd.stop' not in commands and 'd.check_hash' not in commands, commands
            assert 'session.save' in commands, commands
            rpc.assert_clean()
        finally:
            rpc.shutdown()
            rpc.server_close()


for method in ('torrent-start', 'torrent-start-now'):
    check(True, 0, 1, method)
    check(False, 0, 0, method)
    check(True, 6, 0, method)
    check(True, 2, 0, method)
    check(True, 0, 1, method, incomplete=True)
    for command in ('d.is_active', 'd.hashing', 'd.resume'):
        check(True, 0, 0, method, fail_command=command)
print('rTorrent resume regression passed')
