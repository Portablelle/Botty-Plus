import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('ddl_benchmark', Path(__file__).resolve().parents[1] / 'scripts/benchmark-remote-ddl.py')
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class BenchmarkTests(unittest.TestCase):
    def test_slot_contention_waits_for_manager_retry(self):
        row = {'remotePhase': 'transfer', 'status': 4, 'remoteRetrying': True,
               'errorString': 'Temporary server contention'}
        self.assertEqual(benchmark.import_error(row), '')
        row['errorString'] = 'Another diagnostic wording'
        self.assertEqual(benchmark.import_error(row), '')
        row['errorString'] = ''
        row['remotePhase'] = 'ready'
        self.assertEqual(benchmark.import_error(row), '')

    def test_paused_and_fatal_errors_are_not_hidden(self):
        for error in ['Server rejected pinned byte range (416); pause and inspect the remote file', 'Storage unavailable']:
            self.assertEqual(benchmark.import_error({'remotePhase': 'transfer', 'status': 4, 'errorString': error}), error)
        error = 'Server transfer slots busy (429); retrying after bounded delay'
        self.assertEqual(benchmark.import_error({'remotePhase': 'transfer', 'status': 0, 'remoteRetrying': True, 'errorString': error}), error)
