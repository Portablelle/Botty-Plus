"""Exercise background host checks without running repository code or compilers."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/botty-host-checks.sh'

STUB = '''
import os
from pathlib import Path
import sys
import time

name = Path(sys.argv[0]).name
args = sys.argv[1:]
root = Path(os.environ['BOTTY_HOST_CHECK_ROOT'])
invocation = name + ' ' + ' '.join(args)
with (root / 'invocations').open('a') as log:
    log.write(invocation + '\\n')
print(invocation, flush=True)
if name == 'python3' and 'infra/ci-runner' in args:
    (root / 'worker-started').touch()
    if os.environ.get('WAIT_FOR_BUILD') == '1':
        deadline = time.monotonic() + 3
        while not (root / 'build-started').exists():
            if time.monotonic() > deadline:
                sys.exit(99)
            time.sleep(.01)
    time.sleep(float(os.environ.get('TREE_SLEEP', '0')))
    print('background tree output', flush=True)
    sys.exit(int(os.environ.get('TREE_EXIT', '0')))
if name == 'make' and 'homebrew/botty' in args:
    (root / 'build-started').touch()
    sys.exit(int(os.environ.get('BUILD_EXIT', '0')))
if name == 'python3' and 'test_assembly.py' in args:
    (root / 'worker-finished').touch()
    print('background worker finished', flush=True)
'''


class HostChecksSchedulingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        binaries = self.root / 'bin'
        binaries.mkdir()
        # Restrict PATH so a developer's ccache or compiler cannot be used.
        for name in ['bash', 'cat', 'mktemp', 'rm']:
            (binaries / name).symlink_to(shutil.which(name))
        for name in ['python3', 'make', 'node', 'clang++']:
            stub = binaries / name
            stub.write_text('#!' + sys.executable + '\n' + STUB)
            stub.chmod(0o755)
        changed = self.root / 'changed-files'
        # Exercise service/native builds but avoid rTorrent's fixed /tmp outputs.
        changed.write_text('homebrew/botty/src/server.cpp\n')
        self.env = dict(os.environ, PATH=str(binaries), TMPDIR=str(self.root),
                        BOTTY_HOST_CHECK_ROOT=str(self.root), BOTTY_CHANGED_FILES=str(changed),
                        BOTTY_HOST_JOBS='2', BOTTY_HOST_CHECK_BUILD_ONLY='0',
                        TREE_EXIT='0', BUILD_EXIT='0', TREE_SLEEP='0', WAIT_FOR_BUILD='0')

    def run_checks(self, **overrides):
        result = subprocess.run(['bash', str(SCRIPT)], env=dict(self.env, **overrides),
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stderr, '')
        return result

    def test_background_failure_is_reported_and_propagated(self):
        result = self.run_checks(TREE_EXIT='7')
        self.assertEqual(result.returncode, 7, result.stdout)
        self.assertIn('background tree output', result.stdout)
        self.assertRegex(result.stdout, r'Host check: Runner infrastructure \(\d+s, exit 7\)')

    def test_build_failure_waits_for_worker_and_keeps_build_exit_code(self):
        result = self.run_checks(BUILD_EXIT='9', WAIT_FOR_BUILD='1', TREE_SLEEP='.1')
        self.assertEqual(result.returncode, 9, result.stdout)
        self.assertTrue((self.root / 'worker-finished').exists())
        self.assertIn('background tree output', result.stdout)
        self.assertIn('background worker finished', result.stdout)
        self.assertIn('Host check: Engine assembly', result.stdout)

    def test_build_only_skips_tree_checks_and_keeps_builds(self):
        result = self.run_checks(BOTTY_HOST_CHECK_BUILD_ONLY='1')
        self.assertEqual(result.returncode, 0, result.stdout)
        invocations = (self.root / 'invocations').read_text()
        for command in ['unittest discover', 'botty-packages', 'ui-test']:
            self.assertNotIn(command, invocations)
        self.assertIn('make -C homebrew/botty -j2 NATIVE_CXX=clang++ test-build', invocations)
        self.assertIn('make -C homebrew/botty-native -j2 CXX=clang++ host-test-build', invocations)
        self.assertIn('Repository contracts (skipped: build only)', result.stdout)

    def test_success_prints_background_log_before_compression_worker(self):
        result = self.run_checks(TREE_SLEEP='.1')
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue((self.root / 'worker-finished').exists())
        self.assertLess(result.stdout.index('background worker finished'),
                        result.stdout.index('::group::Compression worker'))
        self.assertLess(result.stdout.index('Host check: Engine assembly'),
                        result.stdout.index('::group::Compression worker'))


if __name__ == '__main__':
    unittest.main()
