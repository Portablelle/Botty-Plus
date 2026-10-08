import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('packages', Path(__file__).resolve().parents[1] / 'scripts/botty-packages.py')
packages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packages)


class UpdateCompatibilityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.native = {'version': '01.004.002', 'requires': {'manager': '1.5.4', 'worker': '1.3.1', 'rtorrent': '0.16.24-botty5', 'apiVersion': 1}}
        self.manager = {'id': '1.5.4', 'workerVersion': '1.3.1', 'apiVersion': 1, 'workerApi': 'library-1.3', 'updaterVersion': '1.0.0'}
        self.engine = {'id': '0.16.24-botty5'}

    def write(self):
        for name, manifest in [('botty-native', self.native), ('botty', self.manager), ('rtorrent', self.engine)]:
            path = self.root / name / 'manifest.json'
            path.parent.mkdir(exist_ok=True)
            path.write_text(json.dumps(manifest))

    def check(self):
        self.write()
        packages.compatible_versions(self.root)

    def test_current_and_service_only_newer_bundle(self):
        self.check()
        self.manager['id'] = '1.10.0'
        self.manager['workerVersion'] = '1.3.2'
        self.engine['id'] = '0.16.24-botty6'
        self.check()

    def test_rejects_old_manager_worker_and_engine(self):
        for component, key, value in [(self.manager, 'id', '1.5.3'), (self.manager, 'workerVersion', '1.3.0'), (self.engine, 'id', '0.16.24-botty4')]:
            original = component[key]
            component[key] = value
            with self.assertRaises(ValueError):
                self.check()
            component[key] = original

    def test_rejects_mismatched_or_malformed_api(self):
        for value in [2, True, '1', None]:
            self.manager['apiVersion'] = value
            with self.assertRaises(ValueError):
                self.check()
        self.manager['apiVersion'] = 1
        for value in [0, 2, True, '1', None]:
            self.native['requires']['apiVersion'] = value
            with self.assertRaises(ValueError):
                self.check()

    def test_rejects_incompatible_worker_and_relay_api(self):
        for key, value in [('workerApi', 'library-2'), ('updaterVersion', '2.0.0')]:
            original = self.manager[key]
            self.manager[key] = value
            with self.assertRaises(ValueError):
                self.check()
            self.manager[key] = original
        for key in ['workerApi', 'updaterVersion']:
            original = self.manager.pop(key)
            with self.assertRaises(ValueError):
                self.check()
            self.manager[key] = original

    def test_version_bounds_match_updater(self):
        self.manager['id'] = '1.10000.0'
        self.check()
        for value in ['1.1000000.0', '1.' + '0' * 64 + '.1']:
            self.manager['id'] = value
            with self.assertRaises(ValueError):
                self.check()

    def test_rejects_invalid_or_partial_requirements(self):
        self.native['requires']['worker'] = 'not-a-version'
        with self.assertRaises(ValueError):
            self.check()
        del self.native['requires']['worker']
        with self.assertRaises(ValueError):
            self.check()
        self.native['requires'] = []
        with self.assertRaises(ValueError):
            self.check()

    def test_legacy_manifest_is_still_indexable(self):
        del self.native['requires']
        self.check()


if __name__ == '__main__':
    unittest.main()
