#!/usr/bin/env python3
"""Publish a verified snapshot of the latest main commit, without console changes."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


def command(args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True,
                          timeout=600, env={**os.environ, 'GIT_TERMINAL_PROMPT': '0'},
                          **kwargs).stdout.strip()


def remote_head(repository):
    row = command(['git', 'ls-remote', repository, 'refs/heads/main']).split()
    if len(row) != 2 or not re.fullmatch('[a-f0-9]{40}', row[0]):
        raise RuntimeError('Cannot resolve main')
    return row[0]


def extract_snapshot(cache, commit, destination):
    process = subprocess.Popen(
        ['git', '-C', str(cache), 'archive', commit,
         'vps-site', 'scripts/portal-manifest.py'], stdout=subprocess.PIPE,
        env={**os.environ, 'GIT_TERMINAL_PROMPT': '0'})
    try:
        with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
            archive.extractall(destination, filter='data')
        if process.wait(timeout=600):
            raise RuntimeError('Cannot extract main snapshot')
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.kill()
            process.wait()


def prune_releases(releases, current, previous):
    owned = [p for p in releases.iterdir()
             if re.fullmatch('main-[a-f0-9]{40}', p.name)
             and p.is_dir() and not p.is_symlink()]
    owned.sort(key=lambda p: p.stat().st_mtime_ns, reverse=True)
    keep = {current, previous, *owned[:3]}
    for path in owned:
        if path not in keep:
            shutil.rmtree(path)


def sync_main(repository, state, root):
    state.mkdir(parents=True, exist_ok=True)
    root.mkdir(parents=True, exist_ok=True)
    releases = root / 'releases'
    releases.mkdir(exist_ok=True)
    with (state / 'deploy.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 'busy'
        head = remote_head(repository)
        current = root / 'current'
        target = releases / ('main-' + head)
        if current.is_symlink() and current.resolve() == target.resolve():
            return 'unchanged'
        if current.exists() and not current.is_symlink():
            raise RuntimeError('current must be a release symlink')
        cache = state / 'repository.git'
        if not cache.exists():
            command(['git', 'init', '--bare', str(cache)])
            command(['git', '-C', str(cache), 'remote', 'add', 'origin', repository])
        elif command(['git', '-C', str(cache), 'remote', 'get-url', 'origin']) != repository:
            raise RuntimeError('Repository differs from configured cache')
        command(['git', '-C', str(cache), 'config', 'remote.origin.promisor', 'true'])
        command(['git', '-C', str(cache), 'config', 'remote.origin.partialclonefilter', 'blob:none'])
        command(['git', '-C', str(cache), 'fetch', '--quiet', '--depth=1',
                 '--filter=blob:none', '--no-tags', 'origin',
                 '+refs/heads/main:refs/remotes/origin/main'])
        fetched = command(['git', '-C', str(cache), 'rev-parse', 'refs/remotes/origin/main'])
        if fetched != head:
            return 'superseded'
        with tempfile.TemporaryDirectory(prefix='snapshot-', dir=state) as temporary:
            source = Path(temporary)
            extract_snapshot(cache, head, source)
            validator = source / 'scripts/portal-manifest.py'
            with tempfile.TemporaryDirectory(prefix='.staging-', dir=releases) as staging:
                export = Path(staging) / 'portal'
                command([sys.executable, str(validator), '--root',
                         str(source / 'vps-site'), '--output', str(export)])
                # A newer main commit must never be overwritten by a slow export.
                if remote_head(repository) != head:
                    return 'superseded'
                if target.exists() or target.is_symlink():
                    if not target.is_dir() or target.is_symlink():
                        raise RuntimeError('Invalid existing release path')
                    command([sys.executable, str(validator), '--root', str(target), '--check'])
                else:
                    export.rename(target)
                previous = current.resolve() if current.is_symlink() else None
                next_link = root / '.current.next'
                if next_link.is_symlink():
                    next_link.unlink()
                elif next_link.exists():
                    raise RuntimeError('Unexpected activation staging path')
                next_link.symlink_to('releases/' + target.name)
                os.replace(next_link, current)
        record = state / 'last-deploy.next.json'
        record.write_text(json.dumps({'commit': head, 'release': str(target),
                                     'previous': str(previous) if previous else None}) + '\n')
        os.replace(record, state / 'last-deploy.json')
        prune_releases(releases, target, previous)
        # Only this locked service uses this cache; discard unreachable old snapshots.
        command(['git', '-C', str(cache), 'gc', '--quiet', '--prune=now'])
        return 'deployed ' + head


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', default=os.environ.get(
        'BOTTY_PORTAL_REPOSITORY', 'https://github.com/Portablelle/botty-ps5.git'))
    parser.add_argument('--state', type=Path, default=Path('/var/lib/botty-portal'))
    parser.add_argument('--root', type=Path, default=Path('/var/www/botty-ps5'))
    args = parser.parse_args()
    try:
        print(sync_main(args.repository, args.state.resolve(), args.root.resolve()))
    except (OSError, RuntimeError, subprocess.SubprocessError, tarfile.TarError) as error:
        print('Portal deployment failed: ' + str(error), file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print((error.stderr or error.stdout or '').strip()[-2000:], file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
