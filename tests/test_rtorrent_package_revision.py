from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class RtorrentPackageRevisionTests(unittest.TestCase):
    def test_stale_binary_is_rejected_before_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'scripts').mkdir()
            source = root / 'homebrew/rtorrent'
            (source / 'build').mkdir(parents=True)
            (source / 'runtime-version.hpp').write_text('#define BOTTY_RT_RUNTIME_VERSION "0.16.24-botty6"\n')
            (source / 'build/rtorrent.elf').write_bytes(b'\x7fELF stale "version":"0.16.24-botty5","boot":')
            script = root / 'scripts/package-rtorrent.py'
            shutil.copyfile(Path(__file__).resolve().parents[1] / 'scripts/package-rtorrent.py', script)
            result = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('does not contain the compiled runtime revision', result.stderr)
            self.assertFalse((root / 'packages/rtorrent/manifest.json').exists())


if __name__ == '__main__':
    unittest.main()
