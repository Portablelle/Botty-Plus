"""Select compiled host-check suites from the PR's changed paths, failing open to the full suite."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/botty-host-checks.sh'
ALL = 'service=true native=true rtorrent=true'
NONE = 'service=false native=false rtorrent=false'


class HostCheckScopeTests(unittest.TestCase):
    def scope(self, changed=None, raw=None):
        with tempfile.TemporaryDirectory() as temp:
            env = dict(os.environ)
            env.pop('BOTTY_CHANGED_FILES', None)
            if changed is not None or raw is not None:
                path = Path(temp) / 'changed-files'
                path.write_text(raw if raw is not None else ''.join(f'{p}\n' for p in changed))
                env['BOTTY_CHANGED_FILES'] = str(path)
            result = subprocess.run(['bash', str(SCRIPT), '--print-scope'], env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout.strip()

    def test_without_a_list_every_suite_runs(self):
        self.assertEqual(self.scope(), ALL)

    def test_unreadable_list_runs_every_suite(self):
        env = dict(os.environ, BOTTY_CHANGED_FILES='/nonexistent/changed-files')
        result = subprocess.run(['bash', str(SCRIPT), '--print-scope'], env=env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.stdout.strip(), ALL)

    def test_docs_ci_and_fast_check_paths_skip_compiled_suites(self):
        self.assertEqual(self.scope([
            'CLAUDE.md', 'docs/guide.md', '.github/workflows/claude-code-review.yml', '.claude/commands/review-pr.md',
            'infra/ci-runner/slot.sh', 'tests/test_packages.py', 'deployment/botty-artwork/server.py',
            'homebrew/game-compressor/tools/verify_copy.py', 'homebrew/rest-mode-companion/main.c',
            'packages/botty-release.json', 'scripts/package-botty.py']), NONE)
        self.assertEqual(self.scope([]), NONE)

    def test_each_compiled_suite_follows_its_sources(self):
        self.assertEqual(self.scope(['homebrew/botty/src/server.cpp']), 'service=true native=true rtorrent=false')
        self.assertEqual(self.scope(['homebrew/botty/README.md']), 'service=true native=true rtorrent=false')
        self.assertEqual(self.scope(['deployment/botty-remote/server.py']), 'service=true native=true rtorrent=false')
        self.assertEqual(self.scope(['homebrew/botty-native/src/main.cpp']), 'service=false native=true rtorrent=false')
        self.assertEqual(self.scope(['packages/rtorrent/manifest.json']), 'service=false native=false rtorrent=true')

    def test_entry_point_and_unknown_paths_keep_the_full_suite(self):
        for path in ('scripts/botty-host-checks.sh', 'output/build.log', 'Makefile', 'homebrew/new-module/a.c'):
            with self.subTest(path=path):
                self.assertEqual(self.scope(['CLAUDE.md', path]), ALL)

    def test_last_line_without_newline_counts(self):
        self.assertEqual(self.scope(raw='CLAUDE.md\nhomebrew/rtorrent/tests/a.py'),
                         'service=false native=false rtorrent=true')


if __name__ == '__main__':
    unittest.main()
