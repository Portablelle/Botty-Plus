"""Run all trusted-policy loaders against a base without local AGENTS.md."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ('CLAUDE.md', '.claude/commands/review-pr.md', '.claude/review/history.md')


class ClaudeReviewPolicyTests(unittest.TestCase):
    def loaders(self):
        steps = []
        for workflow, count in [('claude-code-review.yml', 2), ('claude.yml', 1)]:
            source = (ROOT / '.github/workflows' / workflow).read_text()
            loaders = re.findall(r'      - name: Load trusted review policy\n(.*?)(?=\n      - )',
                                 source, re.DOTALL)
            self.assertEqual(len(loaders), count, workflow)
            steps.extend(loaders)
        return [textwrap.dedent(s.split('        run: |\n', 1)[1]) for s in steps]

    def run_loader(self, body, missing=None):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = dict(os.environ, GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1',
                       RUNNER_TEMP=temp, GITHUB_ENV=str(root / 'env'))
            def git(*args):
                return subprocess.run(['git', *args], cwd=root, env=env, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git('init', '-q')
            git('config', 'user.name', 'Fixture')
            git('config', 'user.email', 'fixture@example.test')
            for name in REQUIRED:
                if name == missing:
                    continue
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('trusted base: ' + name)
            git('add', '.')
            git('commit', '-qm', 'Trusted policy without local agent instructions')
            env['BASE'] = git('rev-parse', 'HEAD')
            # Neither local-only instructions nor changed checkout policy are authoritative.
            (root / 'AGENTS.md').write_text('untrusted local instructions')
            (root / 'CLAUDE.md').write_text('untrusted checkout instructions')
            result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                    capture_output=True, text=True, timeout=5)
            if missing is None:
                self.assertEqual(result.returncode, 0, result.stderr)
                policy = root / 'claude-policy'
                for name in REQUIRED:
                    self.assertEqual((policy / name).read_text(), 'trusted base: ' + name)
                self.assertFalse((policy / 'AGENTS.md').exists())
                self.assertIn('CLAUDE_REVIEW_POLICY=' + str(policy), (root / 'env').read_text())
            else:
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((root / 'env').exists())

    def test_review_autofix_and_manual_load_base_policy_without_agents(self):
        for body in self.loaders():
            self.run_loader(body)

    def test_missing_required_policy_still_fails_closed(self):
        for body in self.loaders():
            for missing in REQUIRED:
                with self.subTest(missing=missing):
                    self.run_loader(body, missing)


if __name__ == '__main__':
    unittest.main()
