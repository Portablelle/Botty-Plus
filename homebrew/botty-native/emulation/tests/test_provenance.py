import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('provenance', Path(__file__).resolve().parents[1] / 'provenance.py')
provenance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provenance)


class EngineProvenance(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.app = Path(self.temp.name).resolve()
        contents = {
            'eboot.bin': b'engine', 'sce_module/libc.prx': b'libc', 'lapy.elf': b'helper',
            'mihawkgen/network.fgw': b'weights',
            'sce_sys/param.json': json.dumps({'titleId': 'PPSA99071', 'contentVersion': '01.005.000'}).encode(),
        }
        for name, data in contents.items():
            path = self.app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.app / 'lapy-manifest.json').write_text(json.dumps({
            'target_title': 'PPSA99071', 'elf_sha256': provenance.digest(self.app / 'lapy.elf')}))
        self.receipt = dict(schema=1, upstream=provenance.REPOSITORY, commit=provenance.COMMIT,
                            patchSha256='a' * 64, titleId='PPSA99071', selfUpdate=False,
                            dataRoot='/data/botty/emulation/switch', files={})
        self.receipt['sourceFiles'] = {'tools/botty-provenance.py': provenance.digest(provenance.__file__)}
        for path in self.app.rglob('*'):
            if path.is_file():
                self.receipt['files'][path.relative_to(self.app).as_posix()] = dict(
                    size=path.stat().st_size, sha256=provenance.digest(path))
        self.save()

    def save(self):
        (self.app / provenance.RECEIPT).write_text(json.dumps(self.receipt))

    def test_matching_package(self):
        self.assertEqual(provenance.validate(self.app, 'a' * 64)['commit'], provenance.COMMIT)

    def test_wrong_source_or_patch(self):
        for field in ('commit', 'patchSha256', 'titleId', 'dataRoot', 'upstream'):
            with self.subTest(field=field):
                original = self.receipt[field]
                self.receipt[field] = 'wrong'
                self.save()
                with self.assertRaisesRegex(ValueError, 'provenance mismatch'):
                    provenance.validate(self.app, 'a' * 64)
                self.receipt[field] = original

    def test_stale_builder_policy(self):
        self.receipt['sourceFiles']['tools/botty-provenance.py'] = 'old builder policy'
        self.save()
        with self.assertRaisesRegex(ValueError, 'provenance mismatch'):
            provenance.validate(self.app, 'a' * 64)

    def test_tampered_binary_or_missing_weights(self):
        for name in ('eboot.bin', 'mihawkgen/network.fgw'):
            path = self.app / name
            data = path.read_bytes()
            path.write_bytes(b'altered')
            with self.assertRaisesRegex(ValueError, 'file mismatch'):
                provenance.validate(self.app, 'a' * 64)
            path.write_bytes(data)
        del self.receipt['files']['mihawkgen/network.fgw']
        self.save()
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            provenance.validate(self.app, 'a' * 64)

    def test_helper_for_another_title(self):
        name = 'lapy-manifest.json'
        path = self.app / name
        path.write_text(json.dumps({'target_title': 'PPSA99008', 'elf_sha256': provenance.digest(self.app/'lapy.elf')}))
        self.receipt['files'][name] = dict(size=path.stat().st_size, sha256=provenance.digest(path))
        self.save()
        with self.assertRaisesRegex(ValueError, 'identity mismatch'):
            provenance.validate(self.app, 'a' * 64)

    def test_path_escape(self):
        self.receipt['files']['../outside'] = dict(size=1, sha256='b'*64)
        self.save()
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            provenance.validate(self.app, 'a' * 64)

    def test_source_mutated_after_preparation(self):
        source = self.app / 'source.cpp'
        source.write_text('prepared source')
        self.receipt['sourceFiles'] = {'source.cpp': provenance.digest(source)}
        (self.app / 'BOTTY-ENGINE.json').write_text(json.dumps(self.receipt))
        source.write_text('changed after preparation')
        with patch.object(provenance.subprocess, 'check_output', side_effect=[provenance.COMMIT, 'source.cpp\n']):
            with self.assertRaisesRegex(ValueError, 'changed after preparation'):
                provenance.record(self.app, self.app / 'package')

    def test_unexpected_source_change(self):
        self.receipt['sourceFiles'] = {'source.cpp': 'a' * 64}
        (self.app / 'BOTTY-ENGINE.json').write_text(json.dumps(self.receipt))
        with patch.object(provenance.subprocess, 'check_output', side_effect=[provenance.COMMIT, 'another.cpp\n']):
            with self.assertRaisesRegex(ValueError, 'Unexpected engine source'):
                provenance.record(self.app, self.app / 'package')
