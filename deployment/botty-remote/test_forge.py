import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from forge import Forge


class ForgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / 'fake-forge'
        self.binary.write_text('''#!/usr/bin/env python3
import json, pathlib, sys
source=pathlib.Path(sys.argv[2])
if source.suffix not in ('.pkg', '.exfat'): raise SystemExit(2)
if sys.argv[1]=='inspect':
 print(json.dumps({'kind':'exfat' if source.suffix=='.exfat' else 'pkg', 'title_id':'PPSA12345', 'total_bytes':4096, 'files':[{'path':'eboot.bin','size':4096}]}))
else:
 pathlib.Path(sys.argv[sys.argv.index('-o')+1]).write_bytes(b'converted image')
''')
        self.binary.chmod(0o700)
        self.source = self.root / 'game.pkg'
        self.source.write_bytes(b'original package')
        self.forge = Forge(str(self.binary), self.root / 'outputs')
        self.hash = 'a' * 40

    def completed(self):
        self.forge.poll(self.hash, self.source)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = self.forge.poll(self.hash, self.source)
            if state['status'] != 'running': return state
            time.sleep(.01)
        self.fail('Converter did not finish')

    def test_direct_conversion_is_reused_without_rewriting_source_or_output(self):
        original = Forge.identity(self.source)
        state = self.completed()
        self.assertEqual(state['status'], 'ready', state)
        output = Forge.identity(self.forge.output(self.hash))
        reopened = Forge(str(self.binary), self.root / 'outputs')
        self.assertEqual(reopened.poll(self.hash, self.source)['outputIdentity'], output)
        self.assertEqual(Forge.identity(self.source), original)
        self.assertEqual(self.source.read_bytes(), b'original package')

    def test_fpkg_uses_private_pkg_alias_without_copying_source(self):
        renamed = self.source.with_suffix('.FPKG')
        self.source.rename(renamed); self.source = renamed
        original = Forge.identity(self.source)
        script = self.binary.read_text()
        self.binary.write_text(script.replace("if sys.argv[1]=='inspect':", "if source.suffix=='.pkg': assert source.is_symlink() and source.resolve()==pathlib.Path(" + repr(str(self.source.resolve())) + ")\nif sys.argv[1]=='inspect':"))
        state = self.completed()
        self.assertEqual(state['status'], 'ready', state)
        self.assertEqual(Forge.identity(self.source), original)
        self.assertFalse(list((self.forge.root / self.hash).glob('input-*')))

    def test_changed_source_and_output_refuse_reuse(self):
        self.assertEqual(self.completed()['status'], 'ready')
        self.forge.output(self.hash).write_bytes(b'damaged')
        with self.assertRaisesRegex(ValueError, 'image changed'): self.forge.poll(self.hash, self.source)
        self.source.write_bytes(b'new package')
        with self.assertRaisesRegex(ValueError, 'source changed'): self.forge.poll(self.hash, self.source)

    def test_interruption_requires_explicit_retry(self):
        directory = self.forge.root / self.hash
        directory.mkdir()
        self.forge.save(directory, {'status':'running', 'source':Forge.identity(self.source)})
        self.assertEqual(self.forge.poll(self.hash, self.source)['status'], 'failed')
        self.assertFalse(self.forge.output(self.hash).exists())

    def test_failed_inspection_can_choose_another_package(self):
        binary = self.binary.read_text()
        self.binary.write_text('#!/usr/bin/env python3\nraise SystemExit(2)\n')
        self.assertEqual(self.completed()['status'], 'failed')
        self.binary.write_text(binary)
        self.source = self.root / 'alternative.pkg'
        self.source.write_bytes(b'other package')
        self.forge.poll(self.hash, self.source, retry=True)
        self.assertEqual(self.completed()['status'], 'ready')

    def test_interrupted_inspection_can_choose_another_package(self):
        directory = self.forge.root / self.hash; directory.mkdir()
        self.forge.save(directory, {'status': 'running', 'source': Forge.identity(self.source)})
        self.assertEqual(self.forge.poll(self.hash, self.source)['status'], 'failed')
        self.source = self.root / 'alternative.pkg'; self.source.write_bytes(b'alternative')
        self.forge.poll(self.hash, self.source, retry=True)
        self.assertEqual(self.completed()['status'], 'ready')

    def test_replaced_source_path_cannot_redirect_converter_reads(self):
        outside = self.root / 'outside.pkg'; outside.write_bytes(b'outside data')
        script = self.binary.read_text().replace('import json, pathlib, sys', 'import json, pathlib, sys, os')
        script = script.replace("if sys.argv[1]=='inspect':", "if sys.argv[1]=='inspect' and source.suffix=='.pkg':\n original=pathlib.Path(" + repr(str(self.source)) + ")\n original.rename(original.with_suffix('.kept'))\n original.symlink_to(" + repr(str(outside)) + ")\nif sys.argv[1]=='inspect':")
        script = script.replace("write_bytes(b'converted image')", 'write_bytes(source.read_bytes())')
        self.binary.write_text(script)
        self.forge.poll(self.hash, self.source)
        deadline = time.monotonic() + 5
        while self.hash in self.forge.active and time.monotonic() < deadline: time.sleep(.01)
        self.assertNotIn(self.hash, self.forge.active)
        state = json.loads((self.forge.root / self.hash / 'state.json').read_text())
        self.assertEqual(state['status'], 'failed')
        self.assertEqual(self.forge.output(self.hash).read_bytes(), b'original package')

    def test_global_lock_survives_new_adapter_for_another_torrent(self):
        import fcntl
        fd = os.open(self.forge.root / 'worker.lock', os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            restarted = Forge(str(self.binary), self.forge.root)
            state = restarted.poll('b' * 40, self.source)
            self.assertEqual(state['status'], 'waiting')
            self.assertFalse(restarted.output('b' * 40).exists())
        finally: os.close(fd)
        self.assertEqual(self.completed()['status'], 'ready')

    def test_same_source_retry_preserves_partial_without_allocating_another(self):
        directory = self.forge.root / self.hash; directory.mkdir()
        partial = directory / '.game.interrupted.part'; partial.write_bytes(b'partial image')
        original = Forge.identity(partial)
        self.forge.save(directory, {'status': 'failed', 'source': Forge.identity(self.source)})
        for _ in range(3):
            self.forge.poll(self.hash, self.source, retry=True)
            state = self.completed()
            self.assertEqual(state['status'], 'failed')
            self.assertIn('partial image', state['error'])
            self.assertEqual(Forge.identity(partial), original)
            self.assertEqual(list(directory.glob('*.part')), [partial])
            self.assertFalse(self.forge.output(self.hash).exists())

    def test_busy_retry_survives_polling_and_adapter_restart(self):
        import fcntl
        initial = self.source
        for hash_, source in [('c' * 40, initial), ('d' * 40, self.root / 'alternative.pkg')]:
            if source != initial: source.write_bytes(b'alternative package')
            self.hash = hash_; self.source = source
            directory = self.forge.root / hash_; directory.mkdir()
            self.forge.save(directory, {'status': 'failed', 'source': Forge.identity(initial)})
            fd = os.open(self.forge.root / 'worker.lock', os.O_CREAT | os.O_RDWR, 0o600)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.assertEqual(self.forge.poll(hash_, source, retry=True)['status'], 'waiting')
                self.forge = Forge(str(self.binary), self.forge.root)
                self.assertEqual(self.forge.poll(hash_, source)['status'], 'waiting')
            finally: os.close(fd)
            self.assertEqual(self.completed()['status'], 'ready')
            self.assertFalse((directory / 'request.json').exists())

    def test_symlink_and_invalid_hash_refused(self):
        link = self.root / 'link.pkg'; link.symlink_to(self.source)
        with self.assertRaises((ValueError, OSError)): self.forge.poll(self.hash, link)
        directory_link = self.root / 'parent'; directory_link.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises((ValueError, OSError)): self.forge.poll(self.hash, directory_link / self.source.name)
        with self.assertRaises(ValueError): self.forge.poll('../escape', self.source)

    def test_failed_converter_does_not_delete_original(self):
        self.binary.write_text('#!/usr/bin/env python3\nraise SystemExit(2)\n')
        self.assertEqual(self.completed()['status'], 'failed')
        self.assertEqual(self.source.read_bytes(), b'original package')

if __name__ == '__main__': unittest.main()
