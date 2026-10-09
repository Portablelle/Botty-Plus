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
                       RUNNER_TEMP=temp, GITHUB_WORKSPACE=str(root.resolve()), CLAUDE_HOST_CHECK_ROOT=str(root.resolve()),
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

    def test_cancelled_current_run_fails_closed_but_replacements_are_untouched(self):
        source = WORKFLOW.read_text()
        start = source.index("      - name: Check the review's verdict")
        start = source.index('        run: |\n', start) + len('        run: |\n')
        end = source.index('\n  autofix:', start)
        template = textwrap.dedent(source[start:end])
        for host, review in [('cancelled', 'skipped'), ('success', 'cancelled')]:
            for replacement in [None, 'head', 'run']:
                with self.subTest(host=host, review=review, replacement=replacement), tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    calls = root / 'calls'
                    gh = root / 'gh'
                    gh.write_text("""#!/usr/bin/env python3
import json, os, sys
with open(os.environ['GH_CALLS'], 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\\n')
endpoint = sys.argv[2]
if '/pulls/' in endpoint:
    print(os.environ['REMOTE_HEAD'])
elif '/commits/' in endpoint:
    print(json.dumps([{'context': 'Claude host checks', 'creator': {'login': 'github-actions[bot]'}, 'target_url': os.environ['LATEST_RUN']}]))
elif '-X' not in sys.argv or 'POST' not in sys.argv:
    sys.exit(42)
""")
                    gh.chmod(0o755)
                    scripts = root / '.github/scripts'
                    scripts.mkdir(parents=True)
                    (scripts / 'claude-status.sh').write_text((WORKFLOW.parent.parent / 'scripts/claude-status.sh').read_text())
                    post = scripts / 'post-verdict.sh'
                    post.write_text('#!/bin/sh\nprintf "%s\\n" "$2" > "$COMMENT"\n')
                    post.chmod(0o755)
                    values = {'needs.host-checks.result': host, 'needs.claude-review.result': review}
                    body = re.sub(r'\$\{\{(.*?)\}\}',
                                  lambda m: values.get(m.group(1).strip(), 'reopened'), template)
                    env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                               GH_CALLS=str(calls), COMMENT=str(root / 'comment'), HEAD='a' * 40,
                               REMOTE_HEAD=('b' if replacement == 'head' else 'a') * 40,
                               LATEST_RUN='https://example.test/runs/' + ('200' if replacement == 'run' else '100'),
                               REVIEW_RUN='https://example.test/runs/100', FORK='false',
                               REPO='Portablelle/Botty-Plus', PR='22',
                               GITHUB_STEP_SUMMARY=str(root / 'summary'), GITHUB_OUTPUT=str(root / 'output'))
                    result = subprocess.run(['bash', '-e', '-c', body], cwd=temp, env=env,
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0 if replacement else 2, result.stderr)
                    self.assertEqual(result.stderr, '')
                    posts = [json.loads(line) for line in calls.read_text().splitlines()
                             if '/statuses/' in line]
                    if replacement:
                        self.assertEqual(posts, [])
                        self.assertFalse((root / 'comment').exists())
                    else:
                        self.assertEqual(len(posts), 4)
                        for args in posts:
                            self.assertIn('state=failure', args)
                            self.assertIn('repos/Portablelle/Botty-Plus/statuses/' + 'a' * 40, args)
                        self.assertIn('re-run the review', (root / 'comment').read_text())
                        self.assertNotIn('Wait:', (root / 'comment').read_text())
                    self.assertFalse((root / 'output').exists(), 'Cancellation must never start autofix')

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
            gh.write_text("""#!/usr/bin/env python3
import json, os, sys
with open(os.environ['GH_CALLS'], 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\\n')
endpoint = sys.argv[2]
if '/pulls/' in endpoint:
    print(os.environ['HEAD'])
elif '/statuses?' in endpoint:
    print(json.dumps([{'context': 'Claude host checks', 'creator': {'login': 'github-actions[bot]'},
                       'target_url': 'https://example.test/runs/200'}]))
else:
    sys.exit(42)
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
                       RUNNER_TEMP=temp, GITHUB_OUTPUT=str(root / 'output'), REVIEW_RUN='https://example.test/run')
            result = subprocess.run(['bash', '-e', '-c', body], env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            posts = [json.loads(line) for line in calls.read_text().splitlines()
                     if '/statuses/' in line]
            self.assertEqual(len(posts), 4)
            self.assertEqual((root / 'output').read_text().strip(), 'active=true')
            contexts = set()
            for args in posts:
                self.assertIn('repos/Portablelle/Botty-Plus/statuses/' + 'a' * 40, args)
                self.assertIn('state=pending', args)
                contexts.update(arg for arg in args if arg.startswith('context='))
            self.assertEqual(contexts, {'context=verify', 'context=Claude review verdict', 'context=Claude host checks',
                                        'context=Claude review completion'})

    def test_required_verify_uses_real_checks_and_does_not_replace_fork_checks(self):
        source = WORKFLOW.read_text()
        start = source.index("      - name: Check the review's verdict")
        start = source.index('        run: |\n', start) + len('        run: |\n')
        template = textwrap.dedent(source[start:source.index('\n  autofix:', start)])
        for host, fork, code in [('success', False, 0), ('failure', False, 2), ('skipped', True, 2)]:
            with self.subTest(host=host, fork=fork), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                calls = root / 'calls'
                gh = root / 'gh'
                gh.write_text("""#!/usr/bin/env python3
import json, os, sys
with open(os.environ['GH_CALLS'], 'a') as f:
    f.write(json.dumps(sys.argv[1:]) + '\\n')
if '/pulls/' in sys.argv[2]:
    print(os.environ['HEAD'])
elif '/commits/' in sys.argv[2]:
    print(json.dumps([{'context': 'Claude host checks', 'creator': {'login': 'github-actions[bot]'},
                      'target_url': os.environ['REVIEW_RUN']}]))
""")
                gh.chmod(0o755)
                git = root / 'git'
                git.write_text('#!/bin/sh\nexit 0\n')
                git.chmod(0o755)
                scripts = root / '.github/scripts'
                scripts.mkdir(parents=True)
                (scripts / 'claude-status.sh').write_text((WORKFLOW.parent.parent / 'scripts/claude-status.sh').read_text())
                verdict = scripts / 'claude-verdict.sh'
                verdict.write_text('#!/bin/sh\necho code-review-clean\n[ "$HOST_CHECK_RESULT" = success ] || exit 2\n')
                verdict.chmod(0o755)
                post = scripts / 'post-verdict.sh'
                post.write_text('#!/bin/sh\nexit 0\n')
                post.chmod(0o755)
                values = {'needs.host-checks.result': host, 'needs.claude-review.result': 'success'}
                body = re.sub(r'\$\{\{(.*?)\}\}', lambda m: values.get(m.group(1).strip(), 'reopened'), template)
                env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'], GH_CALLS=str(calls),
                           HEAD='a' * 40, BASE='b' * 40, BASE_REF='main', DEFAULT_BRANCH='main',
                           REPO='Portablelle/Botty-Plus', PR='20', FORK='true' if fork else 'false', FRESH='true',
                           REVIEW_RUN='https://example.test/runs/100',
                           GITHUB_STEP_SUMMARY=str(root / 'summary'), GITHUB_OUTPUT=str(root / 'output'))
                result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)
                posts = [json.loads(line) for line in calls.read_text().splitlines() if '/statuses/' in line]
                verify = [args for args in posts if 'context=verify' in args]
                self.assertEqual(len(verify), 0 if fork else 1)
                if verify:
                    self.assertIn('state=' + ('success' if host == 'success' else 'failure'), verify[0])
                self.assertFalse((root / 'output').exists(), 'A check failure cannot start autofix')

    def test_standalone_checks_skip_only_when_the_trusted_base_has_a_publisher(self):
        source = (WORKFLOW.parent / 'botty-checks.yml').read_text()
        start = source.index('        run: |\n') + len('        run: |\n')
        body = textwrap.dedent(source[start:source.index('\n  verify:', start)])
        for policy, failure, trusted in [('', False, False), ('jobs: old-policy', False, False),
                                        ('# BOTTY_REQUIRED_VERIFY_PUBLISHER=1', False, True),
                                        ('# BOTTY_REQUIRED_VERIFY_PUBLISHER=1', True, False)]:
            with self.subTest(policy=policy, failure=failure), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                gh = root / 'gh'
                encoded = base64.b64encode(policy.encode()).decode()
                gh.write_text('#!/bin/sh\nprintf "%s\\n" "$2" >> "$CALL"\n' +
                              ('exit 1\n' if failure else
                               f"case \"$2\" in *claude-code-review.yml*) echo '{encoded}' ;; *) echo '{'a' * 40}' ;; esac\n"))
                gh.chmod(0o755)
                # A publisher introduced by the PR itself is not trusted yet.
                (root / 'claude-code-review.yml').write_text('# BOTTY_REQUIRED_VERIFY_PUBLISHER=1')
                env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'], BASE='b' * 40, HEAD='c' * 40,
                           REPO='Portablelle/Botty-Plus', GH_TOKEN='fixture-token',
                           CALL=str(root / 'call'), GITHUB_OUTPUT=str(root / 'output'))
                result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((root / 'output').read_text().strip(),
                                 'trusted-verify=' + ('true' if trusted else 'false'))
                self.assertIn('?ref=' + 'b' * 40, (root / 'call').read_text())
        self.assertIn("'Checks handled by review workflow' || 'verify'", source)

    def test_changed_or_unreadable_host_check_policy_keeps_standalone_verify(self):
        source = (WORKFLOW.parent / 'botty-checks.yml').read_text()
        start = source.index('        run: |\n') + len('        run: |\n')
        body = textwrap.dedent(source[start:source.index('\n  verify:', start)])
        for path in ('scripts/botty-host-checks.sh', '.github/workflows/botty-checks.yml'):
            for failure in ('changed', 'missing', 'malformed'):
                with self.subTest(path=path, failure=failure), tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    gh = root / 'gh'
                    gh.write_text("""#!/usr/bin/env python3
import base64, os, sys
endpoint = sys.argv[2]
if 'claude-code-review.yml?' in endpoint:
    print(base64.b64encode(b'# BOTTY_REQUIRED_VERIFY_PUBLISHER=1').decode())
elif '/contents/' + os.environ['CHANGED_PATH'] + '?ref=' + os.environ['HEAD'] in endpoint:
    failure = os.environ['FAILURE']
    if failure == 'missing': sys.exit(1)
    print('invalid' if failure == 'malformed' else 'd' * 40)
else:
    print('a' * 40)
""")
                    gh.chmod(0o755)
                    env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                               BASE='b' * 40, HEAD='c' * 40, REPO='Portablelle/Botty-Plus',
                               CHANGED_PATH=path, FAILURE=failure, GITHUB_OUTPUT=str(root / 'output'))
                    result = subprocess.run(['bash', '-e', '-c', body], cwd=root, env=env,
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((root / 'output').read_text().strip(), 'trusted-verify=false')

    def test_review_starts_without_waiting_for_checks_but_verdict_waits_for_both(self):
        source = WORKFLOW.read_text()
        review = source.split('\n  claude-review:', 1)[1].split('\n  verdict:', 1)[0]
        self.assertIn('needs: [context, announce]', review)
        self.assertNotIn('needs.host-checks', review)
        self.assertIn('CLAUDE_HOST_CHECK_RESULT: pending', review)
        self.assertNotIn('Load completed host-check result', review)
        verdict = source.split('\n  verdict:', 1)[1].split('\n  autofix:', 1)[0]
        self.assertIn('needs: [context, host-checks, claude-review]', verdict)
        self.assertIn("for context in 'verify' 'Claude host checks'", verdict)



if __name__ == '__main__':
    unittest.main()
