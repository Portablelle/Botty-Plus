"""Exercise the review verdict against realistic GitHub response fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / '.github/scripts/claude-verdict.sh'


@unittest.skipUnless(shutil.which('jq'), 'jq is required by the verdict script')
class ClaudeVerdictTests(unittest.TestCase):
    def verdict(self, summaries=(), labels=(), fork=False, override_sha=None, permission="write", override_state="COMMENTED",
                override_body="claude-review-override", host_result="", ci=False, review_result="success"):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            fixture = root / 'responses.json'
            fixture.write_text(json.dumps({
                'info': {'head': {'sha': 'a' * 40, 'repo': {'full_name':
                    'someone/fork' if fork else 'Portablelle/Botty-Plus'}},
                    'labels': [{'name': label} for label in labels]},
                'summaries': list(summaries), 'permission': permission,
                'reviews': [] if override_sha is None else [{'commit_id': override_sha,
                    'state': override_state, 'body': override_body, 'user': {'login': 'maintainer'}}],
            }))
            gh = root / 'gh'
            gh.write_text('''#!/usr/bin/env python3
import json, os, subprocess, sys
with open(os.environ['GH_FIXTURE']) as f:
    data = json.load(f)
endpoint = sys.argv[2]
if endpoint.endswith('/reviews'):
    response = data['reviews']
elif endpoint.endswith('/permission'):
    response = {'permission': data['permission']}
elif endpoint.endswith('/comments'):
    response = data['summaries']
else:
    response = data['info']
if '--jq' in sys.argv:
    query = sys.argv[sys.argv.index('--jq') + 1]
    result = subprocess.run(['jq', '-r', query], input=json.dumps(response), text=True)
    sys.exit(result.returncode)
print(json.dumps(response))
''')
            gh.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH'],
                       REPO='Portablelle/Botty-Plus', GH_FIXTURE=str(fixture), HOST_CHECK_RESULT=host_result, REVIEW_CHECK_RESULT=review_result, GITHUB_ACTIONS='true' if ci else '')
            return subprocess.run(['bash', str(SCRIPT), '19'], env=env,
                                  capture_output=True, text=True, timeout=5)

    def summary(self, verdict='pass', sha=None, open_ids='none', user='claude[bot]'):
        return {'user': {'login': user}, 'html_url': 'https://github.com/example/review',
                'body': f'<!-- claude-review sha={sha or "a" * 40} verdict={verdict} open={open_ids} -->\nReview'}

    def test_current_clean_review_passes(self):
        self.assertEqual(self.verdict([self.summary()]).returncode, 0)

    def test_clean_summary_cannot_override_failed_isolated_checks(self):
        for state in ('failure', 'cancelled', 'skipped'):
            with self.subTest(state=state):
                result = self.verdict([self.summary()], host_result=state)
                self.assertEqual(result.returncode, 2)
                self.assertIn('isolated host checks did not pass', result.stdout)
        self.assertEqual(self.verdict([self.summary()], host_result='success').returncode, 0)

    def test_authorized_override_still_clears_failed_isolated_checks(self):
        result = self.verdict([self.summary()], labels=['claude-review-override'],
                              override_sha='a' * 40, host_result='failure', review_result='failure')
        self.assertEqual(result.returncode, 0)
        self.assertIn('maintainer authorized', result.stdout)

    def test_ci_without_recorded_checks_cannot_clear_clean_summary(self):
        self.assertEqual(self.verdict([self.summary()], ci=True).returncode, 2)

    def test_label_readback_retains_failure_and_requires_real_success(self):
        for state in ('failure', 'pending', 'missing'):
            with self.subTest(state=state):
                self.assertEqual(self.verdict([self.summary()], ci=True, host_result=state).returncode, 2)
        self.assertEqual(self.verdict([self.summary()], ci=True, host_result='success').returncode, 0)

    def test_label_cannot_restore_success_before_review_completion(self):
        for state in ('pending', 'failure', 'skipped', 'missing', ''):
            with self.subTest(state=state):
                result = self.verdict([self.summary()], ci=True, host_result='success', review_result=state)
                self.assertEqual(result.returncode, 2)
        self.assertEqual(self.verdict([self.summary()], ci=True, host_result='success', review_result='success').returncode, 0)

    def test_current_findings_block_with_ids(self):
        result = self.verdict([self.summary(verdict='block', open_ids='R1,R3')])
        self.assertEqual(result.returncode, 1)
        self.assertIn('R1, R3', result.stdout)

    def test_failed_checks_block_without_finding_ids(self):
        result = self.verdict([self.summary(verdict='block')])
        self.assertEqual(result.returncode, 2)
        self.assertIn('failing host checks', result.stdout)

    def test_previous_head_cannot_clear_new_commit(self):
        self.assertEqual(self.verdict([self.summary(sha='b' * 40)]).returncode, 2)

    def test_forged_summary_cannot_clear_review(self):
        self.assertEqual(self.verdict([self.summary(user='attacker')]).returncode, 2)

    def test_fork_requires_local_review(self):
        self.assertEqual(self.verdict(fork=True).returncode, 2)

    def test_explicit_current_override_passes(self):
        self.assertEqual(self.verdict(labels=['claude-review-override'], override_sha='a' * 40).returncode, 0)

    def test_stale_override_does_not_clear_fork_push(self):
        result = self.verdict(labels=['claude-review-override'], fork=True, override_sha='b' * 40)
        self.assertEqual(result.returncode, 2)

    def test_label_without_current_review_does_not_clear(self):
        self.assertEqual(self.verdict(labels=['claude-review-override']).returncode, 2)

    def test_non_maintainer_cannot_authorize_override(self):
        result = self.verdict(labels=['claude-review-override'], override_sha='a' * 40, permission='read')
        self.assertEqual(result.returncode, 2)

    def test_dismissed_review_cannot_authorize_override(self):
        result = self.verdict(labels=['claude-review-override'], override_sha='a' * 40,
                              override_state='DISMISSED')
        self.assertEqual(result.returncode, 2)

    def test_wrong_first_line_cannot_authorize_override(self):
        result = self.verdict(labels=['claude-review-override'], override_sha='a' * 40,
                              override_body='Not an override\nclaude-review-override')
        self.assertEqual(result.returncode, 2)

    def test_stale_override_does_not_clear_same_repo_push(self):
        self.assertEqual(self.verdict(labels=['claude-review-override'],
                                      override_sha='b' * 40).returncode, 2)


if __name__ == '__main__':
    unittest.main()
