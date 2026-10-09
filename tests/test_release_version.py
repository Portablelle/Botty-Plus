import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_version', ROOT / 'homebrew/botty-native/tools/release_version.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseVersionTests(unittest.TestCase):
    def test_ps5_conversion_and_bounds(self):
        self.assertEqual(release.ps5_version('1.6.0'), '01.006.000')
        self.assertEqual(release.ps5_version('99.999.999'), '99.999.999')
        for version in ['01.6.0', '1.06.0', '1.6.00', 'v1.6.0', '1.6', '1.6.0-beta', '100.0.0', '1.1000.0', '1.0.1000', None]:
            with self.subTest(version=version), self.assertRaises(ValueError):
                release.ps5_version(version)

    def test_checked_in_native_identities_are_generated_from_release(self):
        release.sync(check=True)

    def test_github_tag_must_match_the_public_bundle(self):
        command = [sys.executable, str(ROOT / 'scripts/botty-packages.py'), '--check', '--tag']
        result = subprocess.run(command + ['v' + release.release_version()], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(command + ['v1.5.4'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('GitHub release tag does not match', result.stderr)

    def test_stale_outputs_fail_until_regenerated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ['src', 'sce_sys', 'assets']: (root / name).mkdir()
            definition = root / 'release.json'
            definition.write_text(json.dumps(dict(schema=1, version='1.6.0')))
            param=json.loads((release.ROOT / 'sce_sys/param.json').read_text())
            param['contentVersion']='01.004.002'
            (root / 'sce_sys/param.json').write_text(json.dumps(param))
            release.sync(root)
            definition.write_text(json.dumps(dict(schema=1, version='1.7.0')))
            with self.assertRaisesRegex(ValueError, 'Stale generated release'): release.sync(root, check=True)
            release.sync(root)
            release.sync(root, check=True)
            param['contentVersion']='01.007.000'
            self.assertEqual(json.loads((root / 'sce_sys/param.json').read_text()), param)

    def test_generation_and_check_reject_changed_or_missing_native_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for name in ['src', 'sce_sys', 'assets']: (root/name).mkdir()
            (root/'release.json').write_text(json.dumps(dict(schema=1, version='1.6.0')))
            path=root/'sce_sys/param.json'
            original=json.loads((release.ROOT/'sce_sys/param.json').read_text())
            for key, expected in release.NATIVE_IDENTITY.items():
                for value in (None, True if type(expected) is int else 'wrong-identity'):
                    param=original.copy()
                    if value is None: del param[key]
                    else: param[key]=value
                    path.write_text(json.dumps(param))
                    for check in (False, True):
                        with self.subTest(key=key, value=value, check=check):
                            with self.assertRaisesRegex(ValueError, f'Invalid native identity field {key}'):
                                release.sync(root, check=check)
            for localized in [None, {}, {'en-US': {'titleName': 'Another app'}}]:
                param=original.copy(); param['localizedParameters']=localized
                path.write_text(json.dumps(param))
                for check in (False, True):
                    with self.subTest(localized=localized, check=check):
                        with self.assertRaisesRegex(ValueError, 'localizedParameters.en-US.titleName'):
                            release.sync(root, check=check)


if __name__ == '__main__':
    unittest.main()
