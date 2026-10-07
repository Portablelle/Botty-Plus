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
# while exercising Git fetching, verification, publication, and rollback.  It
# deliberately checks both a fixed public-file set and the manifest envelope:
# validating only whatever a manifest happens to list would miss an incomplete
# portal export.
VALIDATOR = '''import argparse, hashlib, json, pathlib, shutil
REQUIRED = ('index.html', 'portal.css', 'README.md', 'LICENSE', 'src/app.js')
p=argparse.ArgumentParser();p.add_argument('--root',type=pathlib.Path);p.add_argument('--output',type=pathlib.Path);p.add_argument('--check',action='store_true');a=p.parse_args()
m=json.loads((a.root/'manifest.json').read_text())
hashes=m.get('sha256')
if not isinstance(hashes,dict) or set(hashes) != set(REQUIRED): raise SystemExit('Stale public manifest')
for name in REQUIRED:
 path=a.root/name
 if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=hashes[name]: raise SystemExit('Invalid public file')
if a.output:
 a.output.mkdir()
 for name in [*REQUIRED,'manifest.json']:
  target=a.output/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(a.root/name,target)
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
        for name, content in {
                'portal.css': 'body {}',
                'README.md': '# Portal',
                'LICENSE': 'Test license',
                'src/app.js': 'export default null;',
        }.items():
            path = self.repo / 'vps-site' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (self.repo / 'vps-site/private.txt').write_text('Not a public export')
        self.commit('first')
        self.manual = self.root / 'releases/manual-release'
        self.manual.mkdir(parents=True)
        (self.manual / 'index.html').write_text('previous working release')
        (self.root / 'current').symlink_to('releases/manual-release')

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.repo), *args], check=True,
                              text=True, capture_output=True).stdout.strip()

    def commit(self, content, valid=True, missing=(), manifest_omits=()):
        (self.repo / 'vps-site/index.html').write_text(content)
        root = self.repo / 'vps-site'
        for name in missing:
            (root / name).unlink()
        public = ('index.html', 'portal.css', 'README.md', 'LICENSE', 'src/app.js')
        hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                  for name in public if (root / name).exists()}
        for name in manifest_omits:
            hashes.pop(name)
        if not valid:
            hashes['index.html'] = '0' * 64
        (root / 'manifest.json').write_text(json.dumps({'release': content, 'sha256': hashes}))
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

    def test_missing_required_public_file_preserves_served_release(self):
        self.deploy()
        previous = (self.root / 'current').resolve()
        self.commit('missing stylesheet', missing=('portal.css',))
        with self.assertRaises(subprocess.CalledProcessError):
            self.deploy()
        self.assertEqual((self.root / 'current').resolve(), previous)

    def test_manifest_that_omits_a_public_file_preserves_served_release(self):
        self.deploy()
        previous = (self.root / 'current').resolve()
        self.commit('stale manifest', manifest_omits=('src/app.js',))
        with self.assertRaises(subprocess.CalledProcessError):
            self.deploy()
        self.assertEqual((self.root / 'current').resolve(), previous)

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

    def test_dangling_current_symlink_is_repaired_by_a_verified_release(self):
        (self.root / 'current').unlink()
        (self.root / 'current').symlink_to('releases/no-longer-present')
        head = self.git('rev-parse', 'HEAD')
        self.assertEqual(self.deploy(), 'deployed ' + head)
        target = (self.root / 'current').resolve()
        self.assertEqual(target.name, 'main-' + head)
        self.assertIsNone(json.loads((self.state / 'last-deploy.json').read_text())['previous'])

    def test_unchanged_release_regenerates_missing_deployment_metadata(self):
        head = self.git('rev-parse', 'HEAD')
        self.assertEqual(self.deploy(), 'deployed ' + head)
        target = (self.root / 'current').resolve()
        (self.state / 'last-deploy.json').unlink()

        self.assertEqual(self.deploy(), 'unchanged')
        self.assertEqual(json.loads((self.state / 'last-deploy.json').read_text()), {
            'commit': head,
            'release': str(target),
            'previous': None,
        })

    def test_unchanged_release_repairs_stale_metadata_and_keeps_rollback(self):
        first = self.git('rev-parse', 'HEAD')
        self.assertEqual(self.deploy(), 'deployed ' + first)
        previous = (self.root / 'current').resolve()
        second = self.commit('second release')
        self.assertEqual(self.deploy(), 'deployed ' + second)
        target = (self.root / 'current').resolve()
        (self.state / 'last-deploy.json').write_text(json.dumps({
            'commit': first,
            'release': str(previous),
            'previous': str(self.manual),
        }))

        self.assertEqual(self.deploy(), 'unchanged')
        self.assertEqual(json.loads((self.state / 'last-deploy.json').read_text()), {
            'commit': second,
            'release': str(target),
            'previous': str(previous),
        })

    def test_interrupted_activation_recovers_deployment_metadata(self):
        first = self.git('rev-parse', 'HEAD')
        self.assertEqual(self.deploy(), 'deployed ' + first)
        previous = (self.root / 'current').resolve()
        second = self.commit('second release')
        real_replace = sync.os.replace

        def interrupt_after_activation(source, destination):
            real_replace(source, destination)
            if Path(source) == self.root / '.current.next':
                raise OSError('simulated crash after activation')

        with patch.object(sync.os, 'replace', side_effect=interrupt_after_activation):
            with self.assertRaises(OSError):
                self.deploy()
        target = self.root / 'releases' / ('main-' + second)
        self.assertEqual((self.root / 'current').resolve(), target)
        self.assertTrue((self.state / 'activation-pending.json').exists())

        self.assertEqual(self.deploy(), 'unchanged')
        record = json.loads((self.state / 'last-deploy.json').read_text())
        self.assertEqual(record, {
            'commit': second,
            'release': str(target),
            'previous': str(previous),
        })
        self.assertFalse((self.state / 'activation-pending.json').exists())

    def test_network_failure_preserves_served_release(self):
        self.deploy()
        previous = (self.root / 'current').resolve()
        with patch.object(sync, 'remote_head', side_effect=RuntimeError('Network unavailable')):
            with self.assertRaises(RuntimeError):
                self.deploy()
        self.assertEqual((self.root / 'current').resolve(), previous)

    def test_recovery_discards_missing_or_invalid_rollback_targets(self):
        head = self.git('rev-parse', 'HEAD')
        self.deploy()
        target = (self.root / 'current').resolve()
        missing = self.root / 'releases/pruned-release'
        regular_file = self.root / 'releases/not-a-directory'
        regular_file.write_text('invalid rollback')
        link = self.root / 'releases/release-link'
        link.symlink_to(self.manual)
        record_path = self.state / 'last-deploy.json'
        pending_path = self.state / 'activation-pending.json'
        expected = {'commit': head, 'release': str(target), 'previous': None}
        for previous in (missing, regular_file, link):
            for recovery in ('journal', 'legacy-release', 'legacy-previous', 'unchanged'):
                with self.subTest(previous=previous.name, recovery=recovery):
                    record = dict(expected, previous=str(previous))
                    if recovery == 'journal':
                        pending_path.write_text(json.dumps(record))
                        sync.reconcile_activation(self.state, self.root)
                        self.assertFalse(pending_path.exists())
                    else:
                        if recovery == 'legacy-release':
                            record.update(commit='0' * 40, release=str(previous), previous=str(self.manual))
                        elif recovery == 'legacy-previous':
                            record['commit'] = '0' * 40
                        record_path.write_text(json.dumps(record))
                        sync.reconcile_deploy_record(self.state, head, target)
                    self.assertEqual(json.loads(record_path.read_text()), expected)

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
