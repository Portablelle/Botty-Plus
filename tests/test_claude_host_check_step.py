"""Execute the workflow's actual check step against API failure fixtures."""
import base64
import json
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

    def test_older_same_head_verdict_cannot_replace_newer_announcement(self):
        source = WORKFLOW.read_text()
        start = source.index("      - name: Check the review's verdict")
        start = source.index('        run: |\n', start) + len('        run: |\n')
        end = source.index('\n  autofix:', start)
        body = re.sub(r'\$\{\{(.*?)\}\}',
                      lambda m: 'success' if m.group(1).strip().startswith('needs.') else 'reopened',
                      textwrap.dedent(source[start:end]))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            calls = root / 'calls'
            gh = root / 'gh'
            gh.write_text("""#!/bin/sh
printf '%s\\n' "$*" >> "$GH_CALLS"
case "$2" in
  */pulls/*) echo "$HEAD" ;;
  */commits/*/status)
    case "$*" in
      *'Claude host checks'*) echo 'https://example.test/runs/200' ;;
      *) echo 'Wrong run-identity context' >&2; exit 42 ;;
    esac ;;
  *) exit 42 ;;
esac
""")
            gh.chmod(0o755)
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                       GH_CALLS=str(calls), HEAD='a' * 40, REPO='Portablelle/Botty-Plus', PR='20',
                       FORK='false', REVIEW_RUN='https://example.test/runs/100')
            result = subprocess.run(['bash', '-e', '-c', body], env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, '')
            self.assertEqual(len(calls.read_text().splitlines()), 2)
            self.assertNotIn('-X POST', calls.read_text())

    def test_new_run_invalidates_all_evidence_on_the_same_head(self):
        source = WORKFLOW.read_text()
        start = source.index('      - name: Announce the review')
        start = source.index('        run: |\n', start) + len('        run: |\n')
        end = source.index('\n  host-checks:', start)
        body = re.sub(r'\$\{\{(.*?)\}\}',
                      lambda m: 'b' * 40 if m.group(1).strip() == 'github.sha' else 'reopened',
                      textwrap.dedent(source[start:end]))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            calls = root / 'calls'
            gh = root / 'gh'
            gh.write_text("""#!/usr/bin/env python3
import json, os, sys
with open(os.environ['GH_CALLS'], 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\\n')
if '/pulls/' in sys.argv[2]:
    print(os.environ['HEAD'])
""")
            gh.chmod(0o755)
            git = root / 'git'
            git.write_text('#!/bin/sh\nprintf "#!/bin/sh\\nexit 0\\n"\n')
            git.chmod(0o755)
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                       GH_CALLS=str(calls), HEAD='a' * 40, REPO='Portablelle/Botty-Plus', PR='20',
                       RUNNER_TEMP=temp, REVIEW_RUN='https://example.test/run')
            result = subprocess.run(['bash', '-e', '-c', body], env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            posts = [json.loads(line) for line in calls.read_text().splitlines()
                     if '/statuses/' in line]
            self.assertEqual(len(posts), 3)
            contexts = set()
            for args in posts:
                self.assertIn('repos/Portablelle/Botty-Plus/statuses/' + 'a' * 40, args)
                self.assertIn('state=pending', args)
                contexts.update(arg for arg in args if arg.startswith('context='))
            self.assertEqual(contexts, {'context=Claude review verdict', 'context=Claude host checks',
                                        'context=Claude review completion'})

    def test_failed_job_rerun_reads_completed_logs_from_prior_attempt(self):
        source = WORKFLOW.read_text()
        start = source.index('      - name: Load completed host-check logs')
        start = source.index('        run: |\n', start) + len('        run: |\n')
        end = source.index('\n      - name: Run Claude Code Review', start)
        body = textwrap.dedent(source[start:end])
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            gh = root / 'gh'
            gh.write_text("""#!/usr/bin/env python3
import json, subprocess, sys
endpoint = sys.argv[2]
if '/actions/runs/' in endpoint:
    jobs = [{'id': 202, 'name': 'claude-review', 'status': 'completed', 'run_attempt': 2}]
    if 'filter=all' in endpoint:
        jobs += [{'id': 101, 'name': 'Claude host checks', 'status': 'completed', 'run_attempt': 1},
                 {'id': 303, 'name': 'Claude host checks', 'status': 'completed', 'run_attempt': 3}]
    query = sys.argv[sys.argv.index('--jq') + 1]
    result = subprocess.run(['jq', '-r', query], input=json.dumps({'jobs': jobs}), text=True)
    sys.exit(result.returncode)
if endpoint.endswith('/jobs/101/logs'):
    print('completed host checks from attempt 1')
else:
    sys.exit(42)
""")
            gh.chmod(0o755)
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'], RUNNER_TEMP=temp,
                       GITHUB_REPOSITORY='Portablelle/Botty-Plus', GITHUB_RUN_ID='1000',
                       GITHUB_RUN_ATTEMPT='2', GITHUB_ENV=str(root / 'env'))
            result = subprocess.run(['bash', '-e', '-c', body], env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('attempt 1', (root / 'claude-host-checks.log').read_text())


if __name__ == '__main__':
    unittest.main()
