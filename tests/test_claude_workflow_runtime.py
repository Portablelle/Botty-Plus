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
        body = step('Resolve the pull request', '\n      - uses: actions/checkout@')
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

    def test_changed_file_list_is_published_only_when_the_diff_succeeds(self):
        body = step("List the PR's changed files", '\n      # Restore only:')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')
            def git(*args):
                return subprocess.run(['git', *args], cwd=root, env=env, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git('init', '-q')
            git('config', 'user.email', 'fixture@example.test')
            git('config', 'user.name', 'Fixture')
            (root / 'kept.md').write_text('base\n')
            (root / 'deleted.cpp').write_text('base\n')
            git('add', '.')
            git('commit', '-qm', 'Base')
            base = git('rev-parse', 'HEAD')
            (root / 'kept.md').write_text('head\n')
            git('rm', '-q', 'deleted.cpp')
            (root / 'added.py').write_text('head\n')
            git('add', '.')
            git('commit', '-qm', 'Head')
            env.update(BASE=base, HEAD=git('rev-parse', 'HEAD'), RUNNER_TEMP=temp, GITHUB_ENV=str(root / 'env'))
            result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(sorted((root / 'changed-files').read_text().split()), ['added.py', 'deleted.cpp', 'kept.md'])
            self.assertIn('BOTTY_CHANGED_FILES=' + str(root / 'changed-files'), (root / 'env').read_text())
            (root / 'env').unlink()
            (root / 'changed-files').unlink()
            env['HEAD'] = 'f' * 40
            result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((root / 'env').exists(), 'A failed diff must leave every suite enabled')
            self.assertFalse((root / 'changed-files').exists())

    def test_reviewer_is_lighter_only_for_docs_or_small_ordinary_code(self):
        body = step('Choose the reviewer', '\n      - uses: actions/setup-node@')
        cases = [
            ({'README.md': 'more\n', 'docs/guide.md': 'new\n'}, 'claude-sonnet-5-5', 'high'),
            ({'homebrew/botty/src/a.cpp': 'x\n' * 30}, 'claude-opus-5-5', 'medium'),
            ({'homebrew/botty/src/a.cpp': 'x\n' * 51}, 'claude-opus-5-5', 'high'),
            ({'README.md': 'more\n', 'homebrew/botty/src/a.cpp': 'x\n'}, 'claude-opus-5-5', 'medium'),
            ({'CLAUDE.md': 'policy\n'}, 'claude-opus-5-5', 'high'),
            ({'.claude/commands/review-pr.md': 'policy\n'}, 'claude-opus-5-5', 'high'),
            ({'.github/workflows/x.yml': 'on: push\n'}, 'claude-opus-5-5', 'high'),
            ({'scripts/botty-host-checks.sh': 'exit 0\n'}, 'claude-opus-5-5', 'high'),
            ({'packages/botty-release.json': '{}\n'}, 'claude-opus-5-5', 'high'),
            ({'.github/\u00e9.yml': 'on: push\n'}, 'claude-opus-5-5', 'high'),
            ({'homebrew/botty/icon.png': b'\x00\x01binary'}, 'claude-opus-5-5', 'high'),
            ({}, 'claude-opus-5-5', 'high'),
        ]
        for files, model, effort in cases:
            with self.subTest(files=list(files)), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1')
                def git(*args):
                    return subprocess.run(['git', *args], cwd=root, env=env, check=True,
                                          capture_output=True, text=True).stdout.strip()
                git('init', '-q')
                git('config', 'user.email', 'fixture@example.test')
                git('config', 'user.name', 'Fixture')
                (root / 'README.md').write_text('base\n')
                git('add', '.')
                git('commit', '-qm', 'Base')
                base = git('rev-parse', 'HEAD')
                git('remote', 'add', 'origin', str(root))
                for name, content in files.items():
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    if isinstance(content, bytes):
                        path.write_bytes(content)
                    else:
                        path.write_text(content)
                git('add', '.')
                git('commit', '-qm', 'Head', '--allow-empty')
                env.update(BASE=base, HEAD=git('rev-parse', 'HEAD'),
                           GITHUB_OUTPUT=str(root.parent / (root.name + '.output')),
                           GITHUB_STEP_SUMMARY=os.devnull)
                result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                output = Path(env['GITHUB_OUTPUT'])
                self.assertEqual(output.read_text().split(), ['model=' + model, 'effort=' + effort])
                output.unlink()

    def test_reviewer_keeps_opus_high_when_the_head_is_unavailable(self):
        body = step('Choose the reviewer', '\n      - uses: actions/setup-node@')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1',
                       BASE='a' * 40, HEAD='b' * 40, GITHUB_OUTPUT=str(root / 'output'),
                       GITHUB_STEP_SUMMARY=os.devnull)
            subprocess.run(['git', 'init', '-q'], cwd=root, env=env, check=True)
            result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / 'output').read_text().split(), ['model=claude-opus-5-5', 'effort=high'])
