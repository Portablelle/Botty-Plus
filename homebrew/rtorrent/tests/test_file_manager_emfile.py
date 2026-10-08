"""Compile the patched upstream storage sources and exercise real EMFILE."""
from io import BytesIO
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import sys
import unittest

SOURCE = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux host storage harness")
class FileManagerEmfileTests(unittest.TestCase):
    def test_real_descriptor_exhaustion(self):
        with tempfile.TemporaryDirectory(prefix='rtorrent-emfile-') as directory:
            root = Path(directory)
            upstream = SOURCE / 'build/downloads/libtorrent-0.16.24.tar.gz'
            if upstream.exists():
                data = upstream.read_bytes()
            else:
                package = SOURCE.parents[1] / 'packages/rtorrent/rtorrent-source.tar.gz'
                with tarfile.open(package) as archive:
                    data = archive.extractfile('upstream/libtorrent-0.16.24.tar.gz').read()
            with tarfile.open(fileobj=BytesIO(data)) as archive:
                archive.extractall(root, filter='data')
            source = root / 'libtorrent-0.16.24'
            (source / 'config.h').write_text('#define LT_SMP_CACHE_BYTES 64\n')
            units = ['torrent/data/file_manager.cc', 'utils/fd_close_queue.cc',
                     'data/socket_file.cc', 'torrent/data/file.cc',
                     'torrent/types/string_utf8.cc', 'torrent/utils/string_manip.cc',
                     'torrent/exceptions.cc']
            binary = root / 'test'
            def compile_storage():
                subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-pthread',
                                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                                '-I' + str(source), '-I' + str(source / 'src'), '-I' + str(source / 'src/torrent'),
                                str(SOURCE / 'tests/file-manager-emfile.cc'),
                                *[str(source / 'src' / unit) for unit in units],
                                '-o', str(binary)], check=True)
            target = root / 'files'
            target.mkdir()
            compile_storage()
            baseline = subprocess.run([str(binary), str(target)], capture_output=True, text=True, timeout=20)
            self.assertEqual(baseline.returncode, 42, baseline.stderr)
            self.assertIn('file open failed: errno=24', baseline.stderr)
            for _ in range(2): # Patching is safe on a reused build tree.
                subprocess.run(['python3', str(SOURCE / 'patch-libtorrent.py'), str(source)], check=True)
            compile_storage()
            subprocess.run([str(binary), str(target)], check=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
