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
                override_body="claude-review-override"):
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
                       REPO='Portablelle/Botty-Plus', GH_FIXTURE=str(fixture))
            return subprocess.run(['bash', str(SCRIPT), '19'], env=env,
                                  capture_output=True, text=True, timeout=5)

    def summary(self, verdict='pass', sha=None, open_ids='none', user='claude[bot]'):
        return {'user': {'login': user}, 'html_url': 'https://github.com/example/review',
                'body': f'<!-- claude-review sha={sha or "a" * 40} verdict={verdict} open={open_ids} -->\nReview'}

    def test_current_clean_review_passes(self):
        self.assertEqual(self.verdict([self.summary()]).returncode, 0)

    def test_current_findings_block_with_ids(self):
        result = self.verdict([self.summary(verdict='block', open_ids='R1,R3')])
        self.assertEqual(result.returncode, 1)
        self.assertIn('R1, R3', result.stdout)

    def test_failed_checks_block_without_finding_ids(self):
        result = self.verdict([self.summary(verdict='block')])
        self.assertEqual(result.returncode, 1)
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
