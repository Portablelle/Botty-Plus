import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import source_parts as sources

class SourcePartsTests(unittest.TestCase):
    def test_round_trip_and_exclusive_output(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(sources, 'PART_SIZE', 32):
            base=Path(folder); original=bytes(range(256))*3
            archive=base/'botty-native-source.tar.gz';archive.write_bytes(original)
            sources.publish(archive,base)
            sources.publish(archive,base)  # Repeating publication preserves every complete source part.
            self.assertEqual(len(sources.metadata(base)['parts']),24)
            with tarfile.open(archive) as index:
                self.assertEqual(set(index.getnames()), {'README.txt','manifest.json','restore.py'})
            target=base/'restored.tar.gz';sources.restore(base,target)
            self.assertEqual(target.read_bytes(),original)
            with self.assertRaises(FileExistsError):sources.restore(base,target)
            self.assertEqual(target.read_bytes(),original)

    def test_reject_modified_missing_and_unlisted_parts(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(sources, 'PART_SIZE', 32):
            base=Path(folder);archive=base/'botty-native-source.tar.gz';archive.write_bytes(b'a'*90)
            sources.publish(archive,base)
            part=base/'source/native-source.part001';part.write_bytes(b'b'*32)
            with self.assertRaises(ValueError):sources.restore(base,base/'rejected')
            self.assertFalse((base/'rejected').exists())
            part.unlink()
            with self.assertRaises(ValueError):sources.source_names(base)
            part.write_bytes(b'a'*32);(base/'source/unlisted').write_bytes(b'junk')
            with self.assertRaises(ValueError):sources.source_names(base)

    def test_reject_unsafe_or_inconsistent_index(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(sources, 'PART_SIZE', 32):
            base=Path(folder);archive=base/'botty-native-source.tar.gz';archive.write_bytes(b'a'*90)
            sources.publish(archive,base)
            file=base/sources.MANIFEST;document=json.loads(file.read_text())
            document['parts'][0]['path']='../escape';file.write_text(json.dumps(document))
            with self.assertRaises(ValueError):sources.source_names(base)

    def test_index_cannot_survive_without_manifest(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(sources, 'PART_SIZE', 32):
            base=Path(folder);archive=base/'botty-native-source.tar.gz';archive.write_bytes(b'a'*90)
            sources.publish(archive,base);(base/sources.MANIFEST).unlink()
            with self.assertRaises(ValueError):sources.source_names(base)

    def test_entry_write_failure_preserves_original_and_complete_parts(self):
        with tempfile.TemporaryDirectory() as folder, patch.object(sources, 'PART_SIZE', 32):
            base=Path(folder);archive=base/'botty-native-source.tar.gz';original=b'a'*90;archive.write_bytes(original)
            with patch.object(sources.os, 'replace', side_effect=OSError('disk full')):
                with self.assertRaises(OSError): sources.publish(archive,base)
            self.assertEqual(archive.read_bytes(),original)
            self.assertFalse(list(base.glob('.source-entry-*')))
            archive.write_bytes(b'\x1f\x8b\x08')
            target=base/'restored.tar.gz';sources.restore(base,target)
            self.assertEqual(target.read_bytes(),original)

    def test_small_archive_remains_direct(self):
        with tempfile.TemporaryDirectory() as folder:
            base=Path(folder);archive=base/'botty-native-source.tar.gz';archive.write_bytes(b'small')
            sources.publish(archive,base)
            self.assertEqual(archive.read_bytes(),b'small');self.assertEqual(sources.source_names(base),[])

if __name__=='__main__':unittest.main()
