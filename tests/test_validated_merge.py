"""Skip re-running tests on the default branch only for an exactly validated PR tree."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / '.github/workflows/botty-checks.yml'
SCRIPT = ROOT / 'scripts/botty-host-checks.sh'
REPO = 'Portablelle/Botty-Plus'
HEAD = 'c' * 40

FAKE_GH = """#!/usr/bin/env python3
import json, os, subprocess, sys
fixture = json.loads(open(os.environ['FIXTURE']).read())
endpoint = sys.argv[2]
if fixture.get('fail') and fixture['fail'] in endpoint:
    sys.exit(1)
if endpoint.endswith('/pulls'):
    data = fixture['pulls']
elif '/git/commits/' in endpoint:
    data = {'tree': {'sha': fixture['head_tree']}}
elif '/statuses?' in endpoint:
    print(json.dumps(fixture['statuses']))
    sys.exit(0)
else:
    sys.exit(42)
if '--jq' in sys.argv:
    query = sys.argv[sys.argv.index('--jq') + 1]
    sys.stdout.write(subprocess.run(['jq', '-r', query], input=json.dumps(data), text=True,
                                    capture_output=True, check=True).stdout)
else:
    print(json.dumps(data))
"""


def step_body():
    source = WORKFLOW.read_text()
    start = source.index('      - name: Detect an already validated merge')
    start = source.index('        run: |\n', start) + len('        run: |\n')
    return textwrap.dedent(source[start:source.index('\n      - name: Run host checks', start)])


class ValidatedMergeTests(unittest.TestCase):
    def detect(self, mutate=None):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')
            def git(*args):
                return subprocess.run(['git', *args], cwd=root, env=env, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git('init', '-q')
            git('config', 'user.email', 'fixture@example.test')
            git('config', 'user.name', 'Fixture')
            (root / '.github/scripts').mkdir(parents=True)
            shutil.copy(ROOT / '.github/scripts/claude-status.sh', root / '.github/scripts/claude-status.sh')
            git('add', '.')
            git('commit', '-qm', 'Merged')
            sha = git('rev-parse', 'HEAD')
            fixture = {
                'pulls': [{'merge_commit_sha': sha, 'merged_at': '2026-10-12T00:00:00Z',
                           'head': {'sha': HEAD, 'repo': {'full_name': REPO}}}],
                'head_tree': git('rev-parse', 'HEAD^{tree}'),
                'statuses': [{'context': 'Claude host checks', 'state': 'success',
                              'creator': {'login': 'github-actions[bot]'}}],
            }
            if mutate:
                mutate(fixture)
            (root / 'fixture.json').write_text(json.dumps(fixture))
            bin_dir = root / 'bin'
            bin_dir.mkdir()
            (bin_dir / 'gh').write_text(FAKE_GH)
            (bin_dir / 'gh').chmod(0o755)
            env.update(PATH=str(bin_dir) + os.pathsep + env['PATH'], FIXTURE=str(root / 'fixture.json'),
                       REPO=REPO, SHA=sha, GITHUB_ENV=str(root / 'github-env'))
            result = subprocess.run(['bash', '-e', '-c', step_body()], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            github_env = root / 'github-env'
            return github_env.read_text() if github_env.exists() else ''

    def test_exactly_validated_tree_compiles_without_rerunning_tests(self):
        self.assertEqual(self.detect(), 'BOTTY_HOST_CHECK_BUILD_ONLY=1\n')

    def test_any_doubt_keeps_the_full_suite(self):
        def status(state, login='github-actions[bot]'):
            return lambda f: f.update(statuses=[{'context': 'Claude host checks', 'state': state,
                                                 'creator': {'login': login}}])
        cases = {
            'different tree': lambda f: f.update(head_tree='d' * 40),
            'failed host checks': status('failure'),
            'pending host checks': status('pending'),
            'status from another user': status('success', 'someone'),
            'no status': lambda f: f.update(statuses=[]),
            'no merged PR': lambda f: f.update(pulls=[]),
            'unmerged PR': lambda f: f['pulls'][0].update(merged_at=None),
            'other merge commit': lambda f: f['pulls'][0].update(merge_commit_sha='e' * 40),
            'two merged PRs': lambda f: f['pulls'].append(dict(f['pulls'][0])),
            'fork PR': lambda f: f['pulls'][0]['head']['repo'].update(full_name='someone/Botty-Plus'),
            'malformed head': lambda f: f['pulls'][0]['head'].update(sha='not-a-sha'),
            'pulls API failure': lambda f: f.update(fail='/pulls'),
            'commit API failure': lambda f: f.update(fail='/git/commits/'),
            'status API failure': lambda f: f.update(fail='/statuses'),
        }
        for name, mutate in cases.items():
            with self.subTest(name):
                self.assertEqual(self.detect(mutate), '')

    def test_build_only_compiles_every_selected_suite_without_running_tests(self):
        source = SCRIPT.read_text()
        self.assertIn('make -C homebrew/botty -j"$jobs" NATIVE_CXX="$cxx" test-build', source)
        self.assertIn('skip_check "Native tests" "build only"', source)
        self.assertIn('skip_check "rTorrent" "build only"', source)
        makefile = (ROOT / 'homebrew/botty/Makefile').read_text()
        self.assertIn('test-build: $(TEST_BINARIES)', makefile)
        self.assertIn('test: $(TEST_BINARIES)', makefile)


if __name__ == '__main__':
    unittest.main()
