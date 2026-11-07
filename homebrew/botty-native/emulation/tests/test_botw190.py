"""Keep the licensed runtime payload reproducible and bound to its adapter."""
import hashlib
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Botw190Template(unittest.TestCase):
    def test_template_matches_pinned_source_and_adapter_identity(self):
        spec = importlib.util.spec_from_file_location('botw190_generator', ROOT / 'generate_botw190.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        template = (ROOT / 'ultracam/botw190/patches.json').read_bytes()
        self.assertEqual(template, module.generated().encode())
        adapter = (ROOT / 'patches/standalone.diff').read_text()
        self.assertIn(f'"patches.json",{len(template)},"{hashlib.sha256(template).hexdigest()}"', adapter)

    def test_original_ultracam_payload_is_unchanged(self):
        expected = {
            'exefs/main.npdm': '7cedbb57ba93e3ab6e660490a0a7b932716854004786a2ac42d39dbf8b85e678',
            'exefs/subsdk3': '1216d58cb3eb8028a905a3c88b104a1338b48cff2b1dfeac571bff7b9826dac6',
            'romfs/UltraCam/maxlastbreath.ini': '8ff291f955068298d1210212ace9787f9701f3cf7a8f704f7531b59b0f37f80a',
        }
        for name, digest in expected.items():
            with self.subTest(name=name):
                self.assertEqual(hashlib.sha256((ROOT / 'ultracam/botw' / name).read_bytes()).hexdigest(), digest)
