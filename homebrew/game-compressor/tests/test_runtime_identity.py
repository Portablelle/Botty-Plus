import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PATCHES = ('botty-copy-only.patch', 'botty-library.patch', 'botty-storage.patch')


class RuntimeIdentityPatchTests(unittest.TestCase):
    def test_pinned_chain_adds_only_bounded_binary_identity(self):
        meta = json.loads((ROOT / 'provenance.json').read_text())
        archive = ROOT / 'vendor' / meta['archive']
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), meta['sha256'])
        with tempfile.TemporaryDirectory() as temp:
            with tarfile.open(archive) as source:
                for member in source.getmembers():
                    path = Path(member.name)
                    self.assertFalse(path.is_absolute())
                    self.assertNotIn('..', path.parts)
                    self.assertTrue(member.isfile() or member.isdir())
                source.extractall(temp)
            for name in PATCHES:
                subprocess.run(['patch', '-p1', '-i', str(ROOT / 'patches' / name)],
                               cwd=temp, check=True, capture_output=True)
            source = Path(temp) / 'src' / 'gc_websrv.c'
            before = source.read_text()
            snapshot = {str(p.relative_to(temp)): p.read_bytes() for p in Path(temp).rglob('*') if p.is_file()}
            subprocess.run(['patch', '-p1', '-i', str(ROOT / 'patches' / 'botty-runtime-identity.patch')],
                           cwd=temp, check=True, capture_output=True)
            after = source.read_text()
            old = '    const char *body = "{\\"ok\\":true,\\"bottyWorker\\":\\"library-1.3\\"}";'
            new = '''    char body[160];
    pid_t pid = getpid();
    int n = snprintf(body, sizeof(body), "{\\"ok\\":true,\\"bottyWorker\\":\\"library-1.3\\",\\"version\\":\\"1.3.1\\",\\"pid\\":%ld}", (long)pid);
    if(pid <= 0 || n <= 0 || (size_t)n >= sizeof(body))
      return websrv_send_error_json(req->fd, 500, "Runtime identity unavailable");'''
            self.assertEqual(before.count(old), 1)
            self.assertEqual(after, before.replace(old, new))
            for name, content in snapshot.items():
                if name != 'src/gc_websrv.c':
                    self.assertEqual((Path(temp) / name).read_bytes(), content)
            prepare = (ROOT / 'prepare.py').read_text()
            self.assertLess(prepare.index('botty-storage.patch'), prepare.index('botty-runtime-identity.patch'))
            self.assertEqual(meta['mode'], 'library-1.3')


if __name__ == '__main__':
    unittest.main()
