"""Execute the workflow's actual check step against API failure fixtures."""
import base64
import os
import re
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest


WORKFLOW = Path(__file__).resolve().parents[1] / '.github/workflows/claude-code-review.yml'


class ClaudeHostCheckStepTests(unittest.TestCase):
    def run_step(self, api_script, head_script=None):
        source = WORKFLOW.read_text()
        start = source.index('      - name: Run trusted host checks against the PR head')
        start = source.index('        run: |\n', start) + len('        run: |\n')
        end = source.index('\n  claude-review:', start)
        body = textwrap.dedent(source[start:end])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            gh = root / 'gh'
            gh.write_text('#!/bin/sh\n' + api_script)
            gh.chmod(0o755)
            if head_script is not None:
                (root / 'scripts').mkdir()
                (root / 'scripts/botty-host-checks.sh').write_text(head_script)
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                       RUNNER_TEMP=temp, GITHUB_WORKSPACE=str(root.resolve()),
                       GITHUB_REPOSITORY='Portablelle/Botty-Plus', BASE='a' * 40,
                       GH_TOKEN='fixture-token')
            # Model GitHub's implicit bash -e without relying on external pipefail.
            return subprocess.run(['bash', '-e', '-c', body], cwd=temp, env=env,
                                  capture_output=True, text=True, timeout=5)

    def test_failed_download_cannot_pass_as_an_empty_test_run(self):
        self.assertNotEqual(self.run_step('exit 1\n').returncode, 0)

    def test_empty_successful_api_response_cannot_pass(self):
        self.assertNotEqual(self.run_step('exit 0\n').returncode, 0)

    def test_trusted_script_runs_in_head_workspace_without_token(self):
        script = 'test -z "${GH_TOKEN:-}" || exit 9\ntest "$BOTTY_HOST_CHECK_ROOT" = "$PWD" || exit 10\necho trusted-checks-ran\n'
        encoded = base64.b64encode(script.encode()).decode()
        result = self.run_step(f"echo '{encoded}'\n", 'echo untrusted-checks-ran\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('trusted-checks-ran', result.stdout)
        self.assertNotIn('untrusted-checks-ran', result.stdout)

    def test_head_noop_cannot_bypass_failed_trusted_download(self):
        result = self.run_step('exit 1\n', 'exit 0\n')
        self.assertNotEqual(result.returncode, 0)

    def test_cancelled_jobs_publish_nothing_or_spend_budget(self):
        source = WORKFLOW.read_text()
        start = source.index("      - name: Check the review's verdict")
        start = source.index('        run: |\n', start) + len('        run: |\n')
        end = source.index('\n  autofix:', start)
        template = textwrap.dedent(source[start:end])
        for host, review in [('cancelled', 'skipped'), ('success', 'cancelled')]:
            with self.subTest(host=host, review=review), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                calls = root / 'calls'
                gh = root / 'gh'
                gh.write_text('#!/bin/sh\necho called >> "$GH_CALLS"\n')
                gh.chmod(0o755)
                values = {'needs.host-checks.result': host, 'needs.claude-review.result': review}
                body = re.sub(r'\$\{\{(.*?)\}\}',
                              lambda m: values.get(m.group(1).strip(), 'synchronize'), template)
                env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'], GH_CALLS=str(calls))
                result = subprocess.run(['bash', '-e', '-c', body], env=env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse(calls.exists(), 'A superseded run must not touch GitHub or reserve autofix')


if __name__ == '__main__':
    unittest.main()
