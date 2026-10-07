import importlib.util
import json
from pathlib import Path
import re
import unittest
import tempfile
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('contract',ROOT/'scripts/botty-packages.py')
contract=importlib.util.module_from_spec(spec);spec.loader.exec_module(contract)
class PackageTests(unittest.TestCase):
    def test_current_delivery_is_complete_and_verified(self):
        contract.verify(ROOT/'packages')
    def test_service_web_assets_belong_to_the_packaged_version(self):
        source=(ROOT/'homebrew/botty/src/server.cpp').read_text()
        version=json.loads((ROOT/'packages/botty/manifest.json').read_text())['id']
        expected='/data/botty/manager/'+version+'/ui'
        match=re.search(r'#define BOTTY_UI \"([^\"\n]+)\"',source)
        self.assertIsNotNone(match, 'BOTTY_UI define not found in server.cpp')
        self.assertEqual(match.group(1), expected)
        self.assertIn(expected.encode()+b'\0', (ROOT/'packages/botty/botty-manager.elf').read_bytes())
        source_ui=ROOT/'homebrew/botty/ui'
        shipped_ui=ROOT/'packages/botty/ui'
        names={p.relative_to(source_ui) for p in source_ui.rglob('*') if p.is_file()}
        self.assertEqual(names, {p.relative_to(shipped_ui) for p in shipped_ui.rglob('*') if p.is_file()})
        for name in names:
            with self.subTest(asset=str(name)):
                self.assertEqual((source_ui/name).read_bytes(), (shipped_ui/name).read_bytes())

    def fixture(self, root):
        for package, notices in contract.PACKAGES.items():
            base=root/package; base.mkdir()
            files=[]
            for name in contract.REQUIRED_FILES[package]:
                path=base/name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'fixture')
                files.append(dict(path=name,size=7,sha256=contract.digest(path)))
            for name in notices: (base/name).write_bytes(b'notice')
            (base/'manifest.json').write_text(json.dumps(dict(schema=1,id='1.0.0',version='1.0.0',files=files)))
        (root/'botty-release.json').write_text(json.dumps(dict(schema=1,sha256=contract.inventory(root))))

    def test_unlisted_file_is_rejected_by_index_and_check(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory).resolve(); self.fixture(root)
            (root/'botty/stray.elf').write_bytes(b'unlisted')
            for check in (contract.inventory, contract.verify):
                with self.assertRaisesRegex(ValueError, 'Unlisted package files'): check(root)

    def test_missing_runtime_cannot_be_hidden_by_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory).resolve(); self.fixture(root)
            path=root/'botty/manifest.json'; manifest=json.loads(path.read_text())
            manifest['files']=[e for e in manifest['files'] if e['path']!='game-compressor.elf']
            path.write_text(json.dumps(manifest)); (root/'botty/game-compressor.elf').unlink()
            with self.assertRaisesRegex(ValueError, 'Missing required package files'): contract.inventory(root)

    def test_links_and_special_files_are_rejected_before_reading(self):
        import os
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory).resolve(); self.fixture(root)
            path=root/'botty/botty-manager.elf'; path.unlink()
            for kind in ('symlink', 'fifo'):
                if kind=='symlink': path.symlink_to('/dev/zero')
                else: os.mkfifo(path)
                with patch.object(contract, 'digest', side_effect=AssertionError('must not read')):
                    with self.assertRaisesRegex(ValueError, 'Missing or symbolic'): contract.inventory(root)
                path.unlink()
            ui=root/'botty/ui'; backup=root/'original-ui'; ui.rename(backup); ui.symlink_to(backup, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'Missing or symbolic'): contract.inventory(root)
