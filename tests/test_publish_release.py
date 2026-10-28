import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('publish_release', ROOT / 'scripts/publish-release.py')
publish = importlib.util.module_from_spec(spec)
with patch.object(sys, 'path', sys.path.copy()):
    spec.loader.exec_module(publish)


class PublishReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.native = self.root / 'botty-native'
        for name, data in {'eboot.bin': b'native', 'emulation/eboot.bin': b'engine', 'sce_sys/param.json': b'{}'}.items():
            path = self.native / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def test_folder_zip_holds_the_native_package_under_its_title(self):
        output = self.root / 'folder.zip'
        self.assertEqual(publish.folder_zip(self.native, output), 3)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.namelist(), ['PPSA99071/eboot.bin', 'PPSA99071/emulation/eboot.bin',
                                                  'PPSA99071/sce_sys/param.json'])
            self.assertEqual(archive.read('PPSA99071/emulation/eboot.bin'), b'engine')

    def test_folder_zip_refuses_host_metadata_and_links(self):
        (self.native / 'emulation/._eboot.bin').write_bytes(b'apple double')
        with self.assertRaisesRegex(ValueError, 'Host metadata'):
            publish.folder_zip(self.native, self.root / 'folder.zip')
        (self.native / 'emulation/._eboot.bin').unlink()
        os.symlink(self.native / 'eboot.bin', self.native / 'link.bin')
        with self.assertRaisesRegex(ValueError, 'Symbolic'):
            publish.folder_zip(self.native, self.root / 'folder.zip')

    def test_title_comes_from_the_first_heading(self):
        self.assertEqual(publish.notes_title('# Botty+ 2.0.2 — Fixes\n\nBody\n'), 'Botty+ 2.0.2 — Fixes')
        for text in ['', 'Botty+ 2.0.2\n', '## Botty+\n', '# \n']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                publish.notes_title(text)

    def test_every_published_version_has_notes(self):
        version = publish.load('release_version', ROOT / 'homebrew/botty-native/tools/release_version.py').release_version()
        notes = ROOT / f'docs/releases/{version}.md'
        self.assertTrue(notes.is_file())
        publish.notes_title(notes.read_text())


if __name__ == '__main__':
    unittest.main()
