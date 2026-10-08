"""Check the persistent PR attempt budget using actual gh jq filters."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / '.github/scripts/claude-autofix-attempt.sh'


@unittest.skipUnless(shutil.which('jq'), 'jq is required by the ledger script')
class ClaudeAutofixAttemptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixture = self.root / 'comments.json'
        self.fixture.write_text('[]')
        gh = self.root / 'gh'
        gh.write_text('''#!/usr/bin/env python3
import json, os, subprocess, sys
from pathlib import Path
fixture = Path(os.environ['GH_FIXTURE'])
comments = json.loads(fixture.read_text())
if sys.argv[1] == 'api':
    assert sys.argv[2].endswith('/comments'), 'Do not use mutable commit history'
    query = sys.argv[sys.argv.index('--jq') + 1]
    result = subprocess.run(['jq', '-r', query], input=json.dumps(comments), text=True)
    sys.exit(result.returncode)
assert sys.argv[1:3] == ['pr', 'comment']
comments.append({'user': {'login': 'github-actions[bot]'}, 'body': sys.stdin.read()})
fixture.write_text(json.dumps(comments))
''')
        gh.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ['PATH'],
                        REPO='Portablelle/Botty-Plus', GH_FIXTURE=str(self.fixture))

    def invoke(self, *args):
        return subprocess.run(['bash', str(SCRIPT), *args], env=self.env,
                              capture_output=True, text=True, timeout=5)

    def reserve(self, run, attempt='1'):
        return self.invoke('reserve', '21', str(run), attempt)

    def test_third_attempt_is_blocked_after_two_reservations(self):
        self.assertEqual(self.reserve(100).stdout.strip(), '1')
        self.assertEqual(self.reserve(200).stdout.strip(), '2')
        self.assertEqual(self.reserve(300).returncode, 3)
        self.assertEqual(len(json.loads(self.fixture.read_text())), 2)

    def test_retry_of_same_reservation_is_idempotent(self):
        self.reserve(100)
        self.assertEqual(self.reserve(100).stdout.strip(), '1')
        self.assertEqual(len(json.loads(self.fixture.read_text())), 1)

    def test_workflow_rerun_spends_a_new_attempt(self):
        self.reserve(100, '1')
        self.assertEqual(self.reserve(100, '2').stdout.strip(), '2')
        self.assertEqual(self.reserve(100, '3').returncode, 3)

    def test_budget_remains_without_any_fix_commits(self):
        # The fake API only permits PR comment reads. Removing or rewriting all
        # branch commits cannot affect this persisted state.
        self.reserve(100)
        self.reserve(200)
        self.assertEqual(self.invoke('count', '21').stdout.strip(), '2')
        self.assertEqual(self.reserve(300).returncode, 3)

    def test_forged_or_malformed_records_do_not_spend_budget(self):
        self.fixture.write_text(json.dumps([
            {'user': {'login': 'attacker'}, 'body': '<!-- claude-autofix-attempt run=100 attempt=1 -->'},
            {'user': {'login': 'github-actions[bot]'}, 'body': '<!-- claude-autofix-attempt invalid -->'},
        ]))
        self.assertEqual(self.invoke('count', '21').stdout.strip(), '0')
        self.assertEqual(self.reserve(200).stdout.strip(), '1')

    def test_duplicate_marker_counts_only_once(self):
        self.reserve(100)
        comments = json.loads(self.fixture.read_text())
        self.fixture.write_text(json.dumps(comments + comments))
        self.assertEqual(self.invoke('count', '21').stdout.strip(), '1')


if __name__ == '__main__':
    unittest.main()
