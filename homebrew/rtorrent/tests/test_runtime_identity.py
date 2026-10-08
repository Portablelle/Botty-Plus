import json
import os
import re
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'^#define BOTTY_RT_RUNTIME_VERSION "([^"]+)"$',
                    (ROOT / 'runtime-version.hpp').read_text(), re.MULTILINE).group(1)


@unittest.skipUnless(os.environ.get('RT_IDENTITY_HARNESS'), 'Build host harness on VPS and set RT_IDENTITY_HARNESS')
class RuntimeIdentityTests(unittest.TestCase):
    def run_harness(self, path, mode='publish', boot='valid', at_failure='', owner=''):
        process = subprocess.Popen([os.environ['RT_IDENTITY_HARNESS'], mode, str(path)],
                                   env=dict(os.environ, BOOT_TEST_MODE=boot, AT_TEST_FAILURE=at_failure,
                                            **({'OWNER_TEST_MODE': owner} if owner else {})),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.last_pid = process.pid
        try:
            process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            raise
        return process.returncode

    def test_publish_replace_private_exact_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            for _ in range(2):
                self.assertEqual(self.run_harness(path), 0)
                body = json.loads((path / 'runtime.json').read_text())
                self.assertEqual(set(body), {'schema', 'pid', 'version', 'boot'})
                self.assertEqual(body['schema'], 1)
                self.assertEqual(body['version'], VERSION)
                self.assertGreater(body['pid'], 0)
                self.assertEqual(body['pid'], self.last_pid)
                self.assertEqual(body['boot'], {'seconds': 1700000000, 'microseconds': 123456})
                self.assertLess((path / 'runtime.json').stat().st_size, 256)
                self.assertEqual(stat.S_IMODE((path / 'runtime.json').stat().st_mode), 0o600)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700)
                self.assertEqual(list(path.glob('runtime.json.tmp.*')), [])

    def test_invalid_boot_fails_without_replacing_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            original = b'previous identity'
            (path / 'runtime.json').write_bytes(original)
            for mode in ('unavailable', 'size', 'seconds', 'negative', 'microseconds'):
                self.assertEqual(self.run_harness(path, boot=mode), 1)
                self.assertEqual((path / 'runtime.json').read_bytes(), original)

    def test_root_installer_and_payload_owners_are_trusted_but_other_users_are_not(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(self.run_harness(Path(temp), 'owners'), 0)

    def test_entry_logs_runtime_publication_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            self.assertEqual(self.run_harness(path, 'default', boot='unavailable'), 1)
            log = (path / 'runtime.log').read_text()
            self.assertIn('runtime identity publication failed', log)
            self.assertNotIn('errno', log)

    def test_publish_with_uid_one_and_root_owned_directory_and_existing_record(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path / 'runtime.json').write_text('old identity')
            for _ in range(2):
                self.assertEqual(self.run_harness(path, owner='root'), 0)
                body = json.loads((path / 'runtime.json').read_text())
                self.assertEqual(body['pid'], self.last_pid)
                self.assertEqual(body['version'], VERSION)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700)
                self.assertEqual(stat.S_IMODE((path / 'runtime.json').stat().st_mode), 0o600)
                self.assertEqual(list(path.glob('runtime.json.tmp.*')), [])

    def test_foreign_owners_and_denied_chmod_fail_without_replacing_identity(self):
        for owner in ('foreign-directory', 'foreign-file', 'chmod-denied'):
            with self.subTest(owner=owner), tempfile.TemporaryDirectory() as temp:
                path = Path(temp)
                original = b'old identity'
                (path / 'runtime.json').write_bytes(original)
                self.assertEqual(self.run_harness(path, owner=owner), 1)
                self.assertEqual((path / 'runtime.json').read_bytes(), original)
                self.assertEqual(list(path.glob('runtime.json.tmp.*')), [])

    def test_symlinks_and_unsafe_paths_fail_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / 'state'
            state.mkdir()
            target = root / 'target'
            target.write_text('untouched')
            (state / 'runtime.json').symlink_to(target)
            self.assertEqual(self.run_harness(state), 1)
            self.assertEqual(target.read_text(), 'untouched')
            (state / 'runtime.json').unlink()
            alias = root / 'alias'
            alias.symlink_to(state, target_is_directory=True)
            for path in (alias, str(state) + '/../state', str(state) + '/', 'relative', '/' + 'a' * 1024):
                self.assertEqual(self.run_harness(path), 1)
            (state / 'runtime.json').mkdir()
            self.assertEqual(self.run_harness(state), 1)

    def test_entry_preserves_arguments_lock_cwd_and_logs(self):
        for mode in ('default', 'supervised'):
            with tempfile.TemporaryDirectory() as temp:
                path = Path(temp)
                self.assertEqual(self.run_harness(path, mode), 0)
                body = json.loads((path / 'runtime.json').read_text())
                self.assertEqual(int((path / 'rtorrent.pid').read_text()), body['pid'])
                self.assertIn(f"payload entered (pid {body['pid']})", (path / 'runtime.log').read_text())

    def test_positive_kernel_errno_is_normalized_for_all_at_calls(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            self.assertEqual(self.run_harness(path, 'at-errors'), 0)
            for number in ('499', '493', '501'):
                (path / 'runtime.json').write_text('previous identity')
                self.assertEqual(self.run_harness(path, at_failure=number), 1)
                self.assertEqual((path / 'runtime.json').read_text(), 'previous identity')
                self.assertEqual(list(path.glob('runtime.json.tmp.*')), [])

    def test_stale_pid_temp_and_random_collision_are_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            self.assertEqual(self.run_harness(path, 'stale'), 0)
            self.assertEqual(json.loads((path / 'runtime.json').read_text())['pid'], self.last_pid)
            legacy = path / f'runtime.json.tmp.{self.last_pid}'
            collision = path / f'runtime.json.tmp.{self.last_pid}.{"0" * 32}'
            self.assertEqual(legacy.read_text(), 'preserve stale file')
            self.assertTrue(collision.is_symlink())
            self.assertEqual(collision.read_text(), 'preserve stale file')
            self.assertEqual(set(path.glob('runtime.json.tmp.*')), {legacy, collision})


class RuntimeIdentitySourceTests(unittest.TestCase):
    def test_timeout_kills_and_reaps_before_returning(self):
        process = mock.Mock(pid=123)
        process.communicate.side_effect = [subprocess.TimeoutExpired('harness', 10), (b'', b'')]
        with mock.patch.dict(os.environ, RT_IDENTITY_HARNESS='/unused/harness'), \
             mock.patch('subprocess.Popen', return_value=process):
            with self.assertRaises(subprocess.TimeoutExpired):
                RuntimeIdentityTests().run_harness('/unused/state')
        self.assertEqual(process.method_calls, [mock.call.communicate(timeout=10), mock.call.kill(), mock.call.communicate()])

    def test_entry_and_build_integrate_identity_before_upstream_main(self):
        root = Path(__file__).resolve().parents[1]
        entry = (root / 'ps5-entry.hpp').read_text()
        publisher = (root / 'runtime-identity.hpp').read_text()
        build = (root / 'build.sh').read_text()
        self.assertLess(entry.index('flock(lock'), entry.index('botty_rtorrent_publish_runtime(state)'))
        self.assertLess(entry.index('botty_rtorrent_publish_runtime(state)'), entry.index('if (argc > 1)'))
        self.assertIn('cp /work/runtime-identity.hpp src/runtime-identity.hpp', build)
        self.assertIn('if (botty_rtorrent_init(argc, argv)) return 1;', build)
        self.assertIn('"kern.boottime", &boot, &size, nullptr, 0', publisher)
        self.assertIn('size != sizeof(boot)', publisher)
        self.assertIn('BOTTY_RT_RUNTIME_VERSION', publisher)
        self.assertIn('cp /work/runtime-at.hpp /work/runtime-version.hpp src/', build)
        self.assertIn('-D__PS5__', build)
        self.assertNotIn('metadata', publisher)
        self.assertLess(publisher.index('fsync(fd)'), publisher.index('renameat(dir'))
        self.assertLess(publisher.index('renameat(dir'), publisher.index('fsync(dir)'))

    def test_manifest_uses_compiled_revision(self):
        manifest = ROOT.parents[1] / 'packages' / 'rtorrent' / 'manifest.json'
        if not manifest.exists():
            self.skipTest(f'{manifest} is unavailable in this isolated source fixture')
        self.assertEqual(json.loads(manifest.read_text())['id'], VERSION)


if __name__ == '__main__':
    unittest.main()
