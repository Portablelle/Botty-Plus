import importlib.util
import json
from pathlib import Path
import re
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('contract',ROOT/'scripts/botty-packages.py')
contract=importlib.util.module_from_spec(spec);spec.loader.exec_module(contract)
class PackageTests(unittest.TestCase):
    def test_current_delivery_is_complete_and_verified(self):
        contract.verify(ROOT/'packages')
    def test_service_web_assets_belong_to_the_packaged_version(self):
        source=(ROOT/'homebrew/botty/src/server.cpp').read_text()
        version=json.loads((ROOT/'packages/botty/manifest.json').read_text())['id']
        self.assertEqual(re.search(r'#define BOTTY_UI "([^"\n]+)"',source)[1],'/data/botty/manager/'+version+'/ui')
