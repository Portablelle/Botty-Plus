import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get('RT_IDENTITY_HARNESS'), 'Build host harness on VPS and set RT_IDENTITY_HARNESS')
class RuntimeIdentityTests(unittest.TestCase):
    def run_harness(self, path, mode='publish', boot='valid'):
        process = subprocess.Popen([os.environ['RT_IDENTITY_HARNESS'], mode, str(path)],
                                   env=dict(os.environ, BOOT_TEST_MODE=boot),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.last_pid = process.pid
        process.communicate(timeout=10)
        return process.returncode

    def test_publish_replace_private_exact_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            for _ in range(2):
                self.assertEqual(self.run_harness(path), 0)
                body = json.loads((path / 'runtime.json').read_text())
                self.assertEqual(set(body), {'schema', 'pid', 'version', 'boot'})
                self.assertEqual(body['schema'], 1)
                self.assertEqual(body['version'], '0.16.24-botty5')
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


class RuntimeIdentitySourceTests(unittest.TestCase):
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
        self.assertIn('"0.16.24-botty5', publisher.replace('\\"', '"'))
        self.assertNotIn('metadata', publisher)
        self.assertLess(publisher.index('fsync(fd)'), publisher.index('renameat(dir'))
        self.assertLess(publisher.index('renameat(dir'), publisher.index('fsync(dir)'))


if __name__ == '__main__':
    unittest.main()
