"""Execute the manual review's real status filter against paginated API fixtures."""
import json
from pathlib import Path
import subprocess
import unittest

POLICY = Path(__file__).resolve().parents[1] / '.claude/commands/review-pr.md'


def status(context, state='success', run='https://example.test/runs/100',
           creator='github-actions[bot]'):
    return {'context': context, 'state': state, 'target_url': run,
            'creator': {'login': creator}}


class ManualReviewValidationTests(unittest.TestCase):
    def evaluate(self, pages):
        source = POLICY.read_text()
        query = source.split('```jq\n# MANUAL_HOST_VALIDATION\n', 1)[1].split('```', 1)[0]
        result = subprocess.run(['jq', '-s', query],
                                input='\n'.join(json.dumps(page) for page in pages),
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_success_requires_both_authentic_statuses_across_pages(self):
        result = self.evaluate([[status('verify')], [status('Claude host checks')]])
        self.assertEqual(result, {'state': 'success', 'run': 'https://example.test/runs/100'})

    def test_missing_failed_pending_and_mismatched_evidence_cannot_pass(self):
        host, verify = status('Claude host checks'), status('verify')
        cases = [[], [host], [verify],
                 [host, status('verify', run='https://example.test/runs/99')],
                 [status('Claude host checks', run=None), status('verify', run=None)],
                 [status('Claude host checks', run=''), status('verify', run='')]]
        for context in ('Claude host checks', 'verify'):
            for state in ('pending', 'failure', 'error'):
                cases.append([status(context, state),
                              verify if context == 'Claude host checks' else host])
        for case in cases:
            with self.subTest(case=case):
                self.assertEqual(self.evaluate([case])['state'], 'missing_or_not_successful')

    def test_spoofed_success_cannot_hide_authentic_failure_or_missing_status(self):
        spoof = status('verify', creator='contributor')
        for pages in ([[spoof, status('Claude host checks')]],
                      [[spoof, status('verify', 'failure'), status('Claude host checks')]]):
            with self.subTest(pages=pages):
                self.assertEqual(self.evaluate(pages)['state'], 'missing_or_not_successful')

    def test_latest_status_cannot_be_replaced_by_an_older_success(self):
        for state in ('pending', 'failure'):
            with self.subTest(state=state):
                self.assertEqual(self.evaluate([
                    [status('Claude host checks', state)],
                    [status('verify'), status('Claude host checks')]
                ])['state'], 'missing_or_not_successful')


if __name__ == '__main__':
    unittest.main()
