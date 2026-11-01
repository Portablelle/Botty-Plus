import hashlib
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import apr


def image_fixture(path, executable=b'unrelated executable', fragmented=False):
    """Original synthetic exFAT fixture; no game assets or executables."""
    count = (len(executable) + 511) // 512
    clusters = list(range(3, 3 + count))
    if fragmented:
        clusters.reverse()
    heap = 25
    data = bytearray((heap + count + 1) * 512)
    data[3:11] = b'EXFAT   '
    data[510:512] = b'\x55\xaa'
    struct.pack_into('<Q', data, 72, len(data) // 512)
    struct.pack_into('<5I', data, 80, 24, 1, heap, count + 1, 2)
    data[108:111] = bytes([9, 0, 1])
    struct.pack_into('<I', data, 24 * 512 + 2 * 4, 0xffffffff)
    for a, b in zip(clusters, clusters[1:] + [0xffffffff]):
        struct.pack_into('<I', data, 24 * 512 + a * 4, b)
    entry = bytearray(96)
    entry[0:2] = b'\x85\x02'
    entry[32] = 0xc0
    entry[33] = 1 if fragmented else 3
    entry[35] = 9
    struct.pack_into('<Q', entry, 40, len(executable))
    struct.pack_into('<IQ', entry, 52, clusters[0], len(executable))
    entry[64] = 0xc1
    entry[66:84] = 'eboot.bin'.encode('utf-16-le')
    checksum = 0
    for i, b in enumerate(entry):
        if i not in (2, 3):
            checksum = (((checksum & 1) << 15) + (checksum >> 1) + b) & 0xffff
    struct.pack_into('<H', entry, 2, checksum)
    data[heap * 512:heap * 512 + len(entry)] = entry
    for i, c in enumerate(clusters):
        chunk = executable[i * 512:(i + 1) * 512]
        start = (heap + c - 2) * 512
        data[start:start + len(chunk)] = chunk
    path.write_bytes(data)


class AprTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / 'game.exfat'
        self.before = bytes.fromhex('0f8510030000')
        self.after = bytes.fromhex('e91103000090')
        self.original = b'x' * 510 + self.before + b'y' * 900
        self.expected = self.original[:510] + self.after + self.original[516:]
        self.rule = dict(title='PPSA12345', version='01.000.001', offset=510,
                         before=self.before.hex(), after=self.after.hex(),
                         original=hashlib.sha256(self.original).hexdigest(),
                         patched=hashlib.sha256(self.expected).hexdigest())

    def save(self, directory, state, name):
        (directory / (name + '.json')).write_text(json.dumps(state))

    def repair(self):
        return apr.repair_image(self.image, 'PPSA12345', '01.000.001', self.root, self.save)

    def test_reviewed_patch_crosses_clusters_and_retains_original(self):
        for fragmented in (False, True):
            with self.subTest(fragmented=fragmented):
                image_fixture(self.image, self.original, fragmented)
                self.root.joinpath('apr-original-eboot.self').unlink(missing_ok=True)
                with patch.object(apr, 'PATCHES', [self.rule]):
                    report = self.repair()
                    with self.image.open('rb') as stream:
                        self.assertEqual(apr.ExfatExecutable(stream).executable(), self.expected)
                    self.assertEqual(self.repair()['status'], 'already_applied')
                self.assertEqual(report['status'], 'applied')
                self.assertEqual((self.root / 'apr-original-eboot.self').read_bytes(), self.original)
                self.assertEqual(json.loads((self.root / 'apr-fix.json').read_text())['status'],
                                 'already_applied')

    def test_unknown_candidate_is_reported_without_any_image_write(self):
        image_fixture(self.image, b'Unity: ' + apr.APR_MESSAGE)
        before = self.image.read_bytes()
        report = self.repair()
        self.assertEqual(report['status'], 'unrecognized')
        self.assertTrue(report['candidate'])
        self.assertEqual(self.image.read_bytes(), before)
        self.assertFalse((self.root / 'apr-original-eboot.self').exists())

    def test_large_valid_executable_is_skipped_without_breaking_conversion(self):
        image_fixture(self.image, b'x' * 1024)
        before = self.image.read_bytes()
        with patch.object(apr, 'MAX_EXECUTABLE', 512): report = self.repair()
        self.assertEqual(report['status'], 'unrecognized')
        self.assertFalse(report['scanned'])
        self.assertIsNone(report['candidate'])
        self.assertEqual(self.image.read_bytes(), before)

    def test_bad_rule_or_title_does_not_write_image(self):
        for override in [dict(title='PPSA99999'), dict(version='other'),
                         dict(before='00' * 6), dict(patched='0' * 64)]:
            with self.subTest(override=override):
                image_fixture(self.image, self.original)
                before = self.image.read_bytes()
                with patch.object(apr, 'PATCHES', [dict(self.rule, **override)]):
                    with self.assertRaises(ValueError): self.repair()
                self.assertEqual(self.image.read_bytes(), before)

    def test_verification_error_restores_original_patch_bytes(self):
        image_fixture(self.image, self.original)
        reader = apr.ExfatExecutable.executable
        reads = 0
        def corrupt_check(volume):
            nonlocal reads
            reads += 1
            return reader(volume) if reads == 1 else b'wrong readback'
        before = self.image.read_bytes()
        with patch.object(apr, 'PATCHES', [self.rule]), \
                patch.object(apr.ExfatExecutable, 'executable', corrupt_check):
            with self.assertRaisesRegex(ValueError, 'verification failed'): self.repair()
        self.assertEqual(self.image.read_bytes(), before)
        self.assertEqual(json.loads((self.root / 'apr-fix.json').read_text())['status'], 'prepared')

    def test_truncated_geometry_checksum_and_cycle_are_rejected(self):
        for failure in ('geometry', 'checksum', 'cycle'):
            image_fixture(self.image, self.original, fragmented=True)
            data = bytearray(self.image.read_bytes())
            if failure == 'geometry': struct.pack_into('<I', data, 92, 0x7fffffff)
            if failure == 'checksum': data[25 * 512 + 2] ^= 1
            if failure == 'cycle': struct.pack_into('<I', data, 24 * 512 + 5 * 4, 5)
            self.image.write_bytes(data)
            with self.subTest(failure=failure), self.assertRaises(ValueError): self.repair()
            self.assertEqual(self.image.read_bytes(), data)

    def test_symlink_image_or_existing_backup_is_not_overwritten(self):
        image_fixture(self.image, self.original)
        link = self.root / 'link.exfat'; link.symlink_to(self.image)
        with self.assertRaises(OSError):
            apr.repair_image(link, 'PPSA12345', '01.000.001', self.root, self.save)
        backup = self.root / 'apr-original-eboot.self'; backup.write_bytes(b'keep')
        with patch.object(apr, 'PATCHES', [self.rule]), self.assertRaises(FileExistsError):
            self.repair()
        self.assertEqual(backup.read_bytes(), b'keep')


if __name__ == '__main__': unittest.main()
