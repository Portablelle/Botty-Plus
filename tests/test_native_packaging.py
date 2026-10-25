"""Reject stale corresponding sources before changing the published native package."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
spec = importlib.util.spec_from_file_location('native_packaging', SCRIPTS / 'package-native.py')
packaging = importlib.util.module_from_spec(spec)
with patch.object(sys, 'path', [str(SCRIPTS), *sys.path]):
    spec.loader.exec_module(packaging)


class NativePackagingTests(unittest.TestCase):
    def test_rejected_sources_leave_public_package_unchanged(self):
        for source_only in (False, True):
            for missing in (False, True):
                with self.subTest(source_only=source_only, missing=missing), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    source = root / 'homebrew/botty-native'
                    out = root / 'packages/botty-native'
                    out.mkdir(parents=True)
                    (source / 'sce_sys').mkdir(parents=True)
                    (source / 'sce_sys/param.json').write_text(json.dumps({'titleId': 'PPSA99071'}))
                    (source / 'build').mkdir()
                    if not missing:
                        (source / 'build/emulation-source.tar.gz').write_bytes(b'stale sources')
                    candidate = source / 'dist/PPSA99071'
                    candidate.mkdir(parents=True)
                    runtime = json.dumps({'sourcesSha256': hashlib.sha256(b'new sources').hexdigest()})
                    (candidate / 'emulation-runtime.json').write_text(runtime)
                    (source / 'dist/manifest.json').write_text(json.dumps({'files': [{'path': 'emulation-runtime.json'}]}))
                    (out / 'emulation-runtime.json').write_text(runtime)
                    (out / 'old.bin').write_bytes(b'keep this asset')
                    (out / 'manifest.json').write_text(json.dumps({'files': [
                        {'path': 'emulation-runtime.json'},
                        {'path': 'old.bin', 'sha256': hashlib.sha256(b'keep this asset').hexdigest()},
                    ]}))
                    before = {p.name: p.read_bytes() for p in out.iterdir()}
                    verifier = types.SimpleNamespace(verify=lambda *args: None)
                    with patch.object(packaging, '__file__', str(root / 'scripts/package-native.py')), \
                            patch.object(sys, 'argv', ['package-native.py'] + (['--source-only'] if source_only else [])), \
                            patch.dict(sys.modules, verify_package=verifier):
                        with self.assertRaisesRegex(RuntimeError, 'Build emulation-source|Corresponding sources'):
                            packaging.main()
                    self.assertEqual({p.name: p.read_bytes() for p in out.iterdir()}, before)
