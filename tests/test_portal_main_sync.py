import hashlib
import fcntl
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'portal_main_sync', Path(__file__).resolve().parents[1] / 'deployment/sync-portal-main.py')
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)

# A small independent public-export contract, avoiding the large binary packages
# while exercising Git fetching, verification, publication, and rollback.
VALIDATOR = '''import argparse, hashlib, json, pathlib, shutil
p=argparse.ArgumentParser();p.add_argument('--root',type=pathlib.Path);p.add_argument('--output',type=pathlib.Path);p.add_argument('--check',action='store_true');a=p.parse_args()
m=json.loads((a.root/'manifest.json').read_text())
for name,digest in m.items():
 if hashlib.sha256((a.root/name).read_bytes()).hexdigest()!=digest:raise SystemExit('Invalid public file')
if a.output:
 a.output.mkdir()
 for name in [*m,'manifest.json']:shutil.copyfile(a.root/name,a.output/name)
'''


class PortalMainSyncTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.repo = self.base / 'repo'
        self.state = self.base / 'state'
        self.root = self.base / 'public'
        self.repo.mkdir()
        self.git('init', '-b', 'main')
        self.git('config', 'user.name', 'Test')
        self.git('config', 'user.email', 'test@example.invalid')
        (self.repo / 'scripts').mkdir()
        (self.repo / 'scripts/portal-manifest.py').write_text(VALIDATOR)
        (self.repo / 'vps-site').mkdir()
        (self.repo / 'vps-site/private.txt').write_text('Not a public export')
        self.commit('first')
        self.manual = self.root / 'releases/manual-release'
        self.manual.mkdir(parents=True)
        (self.manual / 'index.html').write_text('previous working release')
        (self.root / 'current').symlink_to('releases/manual-release')

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.repo), *args], check=True,
                              text=True, capture_output=True).stdout.strip()

    def commit(self, content, valid=True):
        (self.repo / 'vps-site/index.html').write_text(content)
        digest = hashlib.sha256(content.encode()).hexdigest() if valid else '0' * 64
        (self.repo / 'vps-site/manifest.json').write_text(json.dumps({'index.html': digest}))
        self.git('add', '.')
        self.git('commit', '-m', content)
        return self.git('rev-parse', 'HEAD')

    def deploy(self):
        return sync.sync_main(str(self.repo), self.state, self.root)

    def test_publish_main_atomically_and_preserve_manual_rollback(self):
        head = self.git('rev-parse', 'HEAD')
        self.assertEqual(self.deploy(), 'deployed ' + head)
        current = (self.root / 'current').resolve()
        self.assertEqual(current.name, 'main-' + head)
        self.assertEqual((current / 'index.html').read_text(), 'first')
        self.assertFalse((current / 'private.txt').exists())
        self.assertTrue(self.manual.exists())
        record = json.loads((self.state / 'last-deploy.json').read_text())
        self.assertEqual(record['previous'], str(self.manual))
        self.assertEqual(self.deploy(), 'unchanged')

    def test_invalid_new_main_preserves_served_release(self):
        self.deploy()
        previous = (self.root / 'current').resolve()
        self.commit('invalid', valid=False)
        with self.assertRaises(subprocess.CalledProcessError):
            self.deploy()
        self.assertEqual((self.root / 'current').resolve(), previous)
        self.assertFalse(any(p.name.startswith('.staging-') for p in (self.root / 'releases').iterdir()))

    def test_branch_changes_are_never_published(self):
        self.deploy()
        previous = (self.root / 'current').resolve()
        self.git('checkout', '-b', 'feature')
        self.commit('unmerged feature')
        self.assertEqual(self.deploy(), 'unchanged')
        self.assertEqual((self.root / 'current').resolve(), previous)

    def test_newer_main_during_export_prevents_stale_activation(self):
        head = self.git('rev-parse', 'HEAD')
        with patch.object(sync, 'remote_head', side_effect=[head, 'f' * 40]):
            self.assertEqual(self.deploy(), 'superseded')
        self.assertEqual((self.root / 'current').resolve(), self.manual)
        self.assertFalse((self.state / 'last-deploy.json').exists())

    def test_invalid_existing_release_is_not_activated(self):
        target = self.root / 'releases' / ('main-' + self.git('rev-parse', 'HEAD'))
        target.mkdir()
        (target / 'manifest.json').write_text(json.dumps({'index.html': '0' * 64}))
        (target / 'index.html').write_text('incomplete release')
        with self.assertRaises(subprocess.CalledProcessError):
            self.deploy()
        self.assertEqual((self.root / 'current').resolve(), self.manual)

    def test_network_failure_preserves_served_release(self):
        self.deploy()
        previous = (self.root / 'current').resolve()
        with patch.object(sync, 'remote_head', side_effect=RuntimeError('Network unavailable')):
            with self.assertRaises(RuntimeError):
                self.deploy()
        self.assertEqual((self.root / 'current').resolve(), previous)

    def test_concurrent_run_does_not_fetch_or_activate(self):
        self.state.mkdir()
        with (self.state / 'deploy.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch.object(sync, 'remote_head') as fetch:
                self.assertEqual(self.deploy(), 'busy')
                fetch.assert_not_called()
        self.assertEqual((self.root / 'current').resolve(), self.manual)

    def test_retention_preserves_manual_and_previous_releases(self):
        self.deploy()
        previous = None
        for n in range(4):
            previous = (self.root / 'current').resolve()
            self.commit('release-' + str(n))
            self.deploy()
        automatic = list((self.root / 'releases').glob('main-*'))
        self.assertEqual(len(automatic), 3)
        self.assertTrue(previous.exists())
        self.assertTrue(self.manual.exists())


if __name__ == '__main__':
    unittest.main()
