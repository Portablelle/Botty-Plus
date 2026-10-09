"""Exercise runtime fixes using API-shaped fixtures and real Git archive exports."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / '.github/workflows/claude-code-review.yml'
STATUS = ROOT / '.github/scripts/claude-status.sh'


def step(name, end):
    source = WORKFLOW.read_text()
    start = source.index('      - name: ' + name)
    start = source.index('        run: |\n', start) + len('        run: |\n')
    return textwrap.dedent(source[start:source.index(end, start)])


class ClaudeWorkflowRuntimeTests(unittest.TestCase):
    def test_status_list_preserves_creator_and_pagination(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            gh = root / 'gh'
            pages = [[{'context': 'Claude host checks', 'state': 'success',
                       'target_url': 'https://untrusted.test/run', 'creator': {'login': 'someone'}}],
                     [{'context': 'Claude host checks', 'state': 'failure',
                       'target_url': 'https://example.test/runs/100', 'creator': {'login': 'github-actions[bot]'}}]]
            gh.write_text('#!/bin/sh\nprintf "%s\\n" "$*" > "$CALL"\ncat "$RESPONSE"\n')
            gh.chmod(0o755)
            (root / 'response').write_text('\n'.join(json.dumps(page) for page in pages))
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                       HEAD='a' * 40, REPO='Portablelle/Botty-Plus',
                       RESPONSE=str(root / 'response'), CALL=str(root / 'call'))
            for field, expected in [('state', 'failure'), ('target_url', 'https://example.test/runs/100')]:
                result = subprocess.run(['bash', str(STATUS), 'Claude host checks', field], env=env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)
                self.assertIn('/statuses?per_page=100', (root / 'call').read_text())
                self.assertIn('--paginate', (root / 'call').read_text())
            (root / 'response').write_text('[]')
            result = subprocess.run(['bash', str(STATUS), 'Claude host checks', 'state'], env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.stdout.strip(), 'missing')

    def test_archive_exports_legacy_gitlinks_without_git_or_credentials(self):
        body = step('Export the PR head without submodule initialization', '\n      - uses: actions/setup-node@')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'source'
            checkout = root / 'checkout'
            source.mkdir()
            checkout.mkdir()
            env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')
            def git(*args, cwd=source):
                return subprocess.run(['git', *args], cwd=cwd, env=env, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git('init', '-q')
            git('config', 'user.email', 'fixture@example.test')
            git('config', 'user.name', 'Fixture')
            (source / 'payload.txt').write_text('exact head tree\n')
            git('add', 'payload.txt')
            git('commit', '-qm', 'Fixture')
            sha = git('rev-parse', 'HEAD')
            git('update-index', '--add', '--cacheinfo', f'160000,{sha},Relapse-Exploit')
            git('commit', '-qm', 'Legacy gitlink without .gitmodules')
            head = git('rev-parse', 'HEAD')
            git('init', '-q', cwd=checkout)
            git('remote', 'add', 'origin', str(source), cwd=checkout)
            env.update(HEAD=head, RUNNER_TEMP=temp, GITHUB_ENV=str(root / 'env'))
            result = subprocess.run(['bash', '-e', '-c', body], cwd=checkout, env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            exported = root / 'pr-head'
            self.assertEqual((exported / 'payload.txt').read_text(), 'exact head tree\n')
            self.assertFalse((exported / '.git').exists())
            self.assertFalse((exported / '.gitmodules').exists())
            self.assertIn(str(exported), (root / 'env').read_text())

    def test_dispatch_resolves_current_head_while_native_event_retains_snapshot(self):
        body = step('Resolve the pull request', '\n  announce:')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            gh = root / 'gh'
            gh.write_text('#!/bin/sh\ncat "$RESPONSE"\n')
            gh.chmod(0o755)
            info = {'number': 17, 'state': 'open', 'head': {'sha': 'a' * 40, 'ref': 'feature',
                    'repo': {'full_name': 'Portablelle/Botty-Plus'}}, 'base': {'ref': 'main'}}
            (root / 'response').write_text(json.dumps(info))
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                       RESPONSE=str(root / 'response'), PR_NUMBER='17', EVENT_PR='null',
                       REPO='Portablelle/Botty-Plus', DEFAULT_BRANCH='main', GITHUB_OUTPUT=str(root / 'output'))
            def run():
                return subprocess.run(['bash', '-e', '-c', body], env=env,
                                      capture_output=True, text=True, timeout=5)
            result = run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('a' * 40, (root / 'output').read_text())
            info['head']['sha'] = 'b' * 40
            env['EVENT_PR'] = json.dumps(info)
            (root / 'output').unlink()
            result = run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('b' * 40, (root / 'output').read_text())
            env.update(EVENT_PR='null', PR_NUMBER='17; unsafe')
            self.assertNotEqual(run().returncode, 0)
            env['PR_NUMBER'] = '17'
            info['state'] = 'closed'
            (root / 'response').write_text(json.dumps(info))
            self.assertNotEqual(run().returncode, 0)

    def test_autofix_records_only_its_own_pushed_head(self):
        body = step('Record the autofix push', '\n  follow-up:')
        for remote, local, expected in [('a', 'a', False), ('b', 'b', True), ('c', 'b', False)]:
            with self.subTest(remote=remote, local=local), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                gh = root / 'gh'
                gh.write_text('#!/bin/sh\necho "$REMOTE"\n')
                gh.chmod(0o755)
                git = root / 'git'
                git.write_text('#!/bin/sh\necho "$LOCAL"\n')
                git.chmod(0o755)
                env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                           HEAD='a' * 40, REMOTE=remote * 40, LOCAL=local * 40,
                           REPO='Portablelle/Botty-Plus', PR='17', GITHUB_OUTPUT=str(root / 'output'))
                result = subprocess.run(['bash', '-e', '-c', body], env=env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((root / 'output').exists(), expected)
                if expected:
                    self.assertEqual((root / 'output').read_text().strip(), 'head=' + 'b' * 40)

    def test_follow_up_ignores_changed_head_and_still_reviews_after_ci_dispatch_failure(self):
        source = WORKFLOW.read_text()
        start = source.index('      - name: Start checks and review after an autofix push')
        start = source.index('        run: |\n', start) + len('        run: |\n')
        body = textwrap.dedent(source[start:])
        for remote, ci_failure, count, code in [('b', False, 2, 0), ('c', False, 0, 0), ('b', True, 2, 1)]:
            with self.subTest(remote=remote, ci_failure=ci_failure), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                gh = root / 'gh'
                gh.write_text('#!/bin/sh\nif [ "$1" = api ]; then echo "$REMOTE"; else echo "$*" >> "$CALLS"; [ "$3" != botty-checks.yml ] || [ "$CI_FAILURE" != true ]; fi\n')
                gh.chmod(0o755)
                env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                           HEAD='b' * 40, REMOTE=remote * 40, CI_FAILURE='true' if ci_failure else 'false',
                           REPO='Portablelle/Botty-Plus', PR='17', BRANCH='feature', DEFAULT_BRANCH='main',
                           CALLS=str(root / 'calls'))
                result = subprocess.run(['bash', '-e', '-c', body], env=env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)
                calls = (root / 'calls').read_text().splitlines() if (root / 'calls').exists() else []
                self.assertEqual(len(calls), count)
                if calls:
                    self.assertIn('--ref feature', calls[0])
                    self.assertIn('--ref main -f pr=17', calls[1])
