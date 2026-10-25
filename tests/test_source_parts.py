import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import source_parts


class PublicSourcePermissions(unittest.TestCase):
    def test_parts_are_readable_after_a_root_build_with_private_umask(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / 'botty-native-source.tar.gz'
            archive.write_bytes(b'public corresponding sources' * 10)
            previous = os.umask(0o077)
            try:
                with patch.object(source_parts, 'PART_SIZE', 64):
                    source_parts.publish(archive, root)
                    self.assertEqual(stat.S_IMODE((root/'source').stat().st_mode), 0o755)
                    for name in source_parts.source_names(root):
                        self.assertEqual(stat.S_IMODE((root/name).stat().st_mode), 0o644)
                    self.assertEqual(stat.S_IMODE(archive.stat().st_mode), 0o644)
                    # An already-published package from an older builder is repairable too.
                    (root/'source').chmod(0o700)
                    archive.chmod(0o600)
                    source_parts.publish(archive, root)
                    self.assertEqual(stat.S_IMODE((root/'source').stat().st_mode), 0o755)
                    self.assertEqual(stat.S_IMODE(archive.stat().st_mode), 0o644)
            finally:
                os.umask(previous)
