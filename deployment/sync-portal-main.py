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


def write_json_atomically(path, value):
    next_path = path.with_name(path.stem + '.next' + path.suffix)
    next_path.write_text(json.dumps(value) + '\n')
    os.replace(next_path, path)


def read_deploy_record(path):
    if not path.exists():
        return None
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError('Invalid deployment record') from error
    if not isinstance(value, dict):
        raise RuntimeError('Invalid deployment record')
    return value


def is_release_directory(path):
    return path.is_dir() and not path.is_symlink()


def current_is_release(current, target):
    return (current.is_symlink() and is_release_directory(target)
            and current.resolve() == target.resolve())


def deployment_record(commit, release, previous):
    return {'commit': commit, 'release': str(release),
            'previous': str(previous) if previous else None}


def reconcile_activation(state, root):
    """Finish or discard an activation that was interrupted after its journal."""
    pending_path = state / 'activation-pending.json'
    pending = read_deploy_record(pending_path)
    if pending is None:
        return
    commit = pending.get('commit')
    release = pending.get('release')
    previous = pending.get('previous')
    if (not isinstance(commit, str) or not re.fullmatch('[a-f0-9]{40}', commit)
            or not isinstance(release, str)
            or previous is not None and not isinstance(previous, str)):
        raise RuntimeError('Invalid activation journal')
    releases = root / 'releases'
    target = releases / ('main-' + commit)
    if Path(release) != target:
        raise RuntimeError('Invalid activation journal')
    current = root / 'current'
    if current_is_release(current, target):
        write_json_atomically(state / 'last-deploy.json', pending)
    pending_path.unlink()


def reconcile_deploy_record(state, commit, target):
    """Repair metadata left stale by an older interrupted activation."""
    record_path = state / 'last-deploy.json'
    existing = read_deploy_record(record_path)
    expected = deployment_record(commit, target, None)
    if existing and existing.get('commit') == commit and existing.get('release') == str(target):
        return
    # Before activation, the recorded release was the active one. This preserves
    # the rollback target when recovering a deployment made by an older service.
    if existing and isinstance(existing.get('release'), str) and existing['release'] != str(target):
        expected['previous'] = existing['release']
    elif existing and isinstance(existing.get('previous'), str):
        expected['previous'] = existing['previous']
    write_json_atomically(record_path, expected)


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
        reconcile_activation(state, root)
        head = remote_head(repository)
        current = root / 'current'
        target = releases / ('main-' + head)
        if current_is_release(current, target):
            reconcile_deploy_record(state, head, target)
            return 'unchanged'
        if current.exists() and not current.is_symlink():
            raise RuntimeError('current must be a release symlink')
        # Capture this before creating target: a dangling current link can name
        # the release we are about to publish, but it is not a rollback point.
        previous = current.resolve() if current.is_symlink() and current.exists() else None
        if previous is not None and not is_release_directory(previous):
            previous = None
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
                pending = deployment_record(head, target, previous)
                write_json_atomically(state / 'activation-pending.json', pending)
                next_link = root / '.current.next'
                if next_link.is_symlink():
                    next_link.unlink()
                elif next_link.exists():
                    raise RuntimeError('Unexpected activation staging path')
                next_link.symlink_to('releases/' + target.name)
                os.replace(next_link, current)
        write_json_atomically(state / 'last-deploy.json', pending)
        (state / 'activation-pending.json').unlink()
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
