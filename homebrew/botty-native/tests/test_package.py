import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('verify_package', ROOT/'tools/verify_package.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.dist=Path(self.temp.name);self.param=json.loads((ROOT/'sce_sys/param.json').read_text())
        self.title=self.param['titleId'];self.app=self.dist/self.title
        files={'eboot.bin':bytes.fromhex('4f153d1d')+b'fixture',
               'sce_module/libc.prx':bytes.fromhex('5414f5ee')+b'runtime',
               'sce_sys/param.json':json.dumps(self.param).encode(),
               'sce_sys/icon0.png':b'icon','sce_sys/pic0.dds':b'background','assets/ui-font.bin':b'font',
               'assets/Manrope-OFL.txt':b'license','assets/build.txt':b'preview',
               'assets/nebula.rgb':bytes(960*540*3),
               **{f'assets/{name}.rgba':bytes(512*512*4) for name in ('courier','extractor','vault')}}
        for name,data in files.items():
            p=self.app/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        self.manifest=dict(schema=1,titleId=self.title,version=self.param['contentVersion'],
            hardwareValidated=False,registrationVerified=False,readOnly=False,
            files=[dict(path=n,size=len(d),sha256=hashlib.sha256(d).hexdigest()) for n,d in files.items()])
        self.write_manifest()
        with zipfile.ZipFile(self.dist/(self.title+'.zip'),'w') as z:
            for n,d in files.items():z.writestr(self.title+'/'+n,d)
    def write_manifest(self):
        (self.dist/'manifest.json').write_text(json.dumps(self.manifest))
    def test_complete_package(self):
        self.assertEqual(module.verify(self.dist,self.param),12)
    def test_rejected_console_download_reservation(self):
        self.param['downloadDataSize']=16
        with self.assertRaisesRegex(ValueError,'downloadDataSize'):
            module.verify(self.dist,self.param)
    def test_corrupt_executable(self):
        (self.app/'eboot.bin').write_bytes(b'damaged')
        with self.assertRaisesRegex(ValueError,'digest'):module.verify(self.dist,self.param)
    def test_interrupted_copy(self):
        (self.app/'sce_module/libc.prx').unlink()
        with self.assertRaisesRegex(ValueError,'Missing'):module.verify(self.dist,self.param)
    def test_stale_zip(self):
        with zipfile.ZipFile(self.dist/(self.title+'.zip'),'w') as z:z.writestr(self.title+'/eboot.bin',b'old')
        with self.assertRaises(ValueError):module.verify(self.dist,self.param)
    def test_unlisted_file(self):
        (self.app/'unexpected').write_text('extra')
        with self.assertRaisesRegex(ValueError,'Unlisted'):module.verify(self.dist,self.param)
    def test_traversal(self):
        self.manifest['files'][0]['path']='../outside';self.write_manifest()
        with self.assertRaisesRegex(ValueError,'Unsafe'):module.verify(self.dist,self.param)
    def test_duplicate(self):
        self.manifest['files'].append(self.manifest['files'][0]);self.write_manifest()
        with self.assertRaisesRegex(ValueError,'duplicate'):module.verify(self.dist,self.param)
    def test_hardware_claim(self):
        self.manifest['hardwareValidated']=True;self.write_manifest()
        with self.assertRaisesRegex(ValueError,'manifest'):module.verify(self.dist,self.param)
