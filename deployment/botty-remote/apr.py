"""Apply reviewed Unity APR fallbacks to private converter outputs, never inputs."""
import hashlib
import os
import stat
import struct


# Original SELF hashes, not decoded ELF hashes. Each branch was independently
# mapped to the ordinary reader and its build boot-tested on a PS5.
PATCHES = (
    dict(title='PPSA08242', version='01.000.006', offset=0xe9b599,
         original='1083fffaa01b17f26ad8011f06f199263b5b7be26a71db3a4d11a2addeab8534',
         patched='f1cfd96288e6a872ecbeb8d6346320dac3a6b244beb8032c55c48cb240cc25e9',
         before='0f8510030000', after='e91103000090'),
    dict(title='PPSA24589', version='01.000.002', offset=0x147e6a0,
         original='65b92f88013a95ff60b1329516aa15c819b75eb973bd5a5c2c788b51e4d5559c',
         patched='f335cc64943bffea939e9a0641549167c99a5bd5eb39e62bf83633ab6945618d',
         before='0f8590020000', after='e99102000090'),
    dict(title='PPSA23066', version='01.012.000', offset=0x1699d7e,
         original='3b855050b3ad4ebaa26b4438bd3ce70ff2ae9c34de979c85cc2d044496f4c32f',
         patched='13bc80fad0c5477b576965a7de8d792918e2f209512a53a8fc9f78820a4b1162',
         before='0f85d0020000', after='e9d102000090'),
)
MAX_EXECUTABLE = 128 * 1024 * 1024
APR_MESSAGE = b'not considered suitable for apr reads'


class ScanLimitError(ValueError):
    """Valid executable outside the optional compatibility scanner's budget."""


def retain_original(backup, original, digest):
    """Create a durable backup, or validate one retained by an interrupted run."""
    try:
        fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    except FileExistsError:
        fd = os.open(backup, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_size != len(original):
                raise ValueError('APR: retained backup must be a matching regular file: ' + backup.name)
            actual = hashlib.sha256()
            left = len(original)
            while left:
                chunk = os.read(fd, min(left, 256 * 1024))
                if not chunk: break
                actual.update(chunk)
                left -= len(chunk)
            if left or actual.hexdigest() != digest or os.read(fd, 1):
                raise ValueError('APR: retained backup hash mismatch: ' + backup.name)
            os.fsync(fd)
        finally:
            os.close(fd)
    else:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(original)
            stream.flush(); os.fsync(stream.fileno())


class ExfatExecutable:
    """Bounded exFAT reader; writes only existing eboot data, without allocation."""
    def __init__(self, image):
        self.image = image
        self.length = os.fstat(image.fileno()).st_size
        boot = self.read(0, 512)
        if boot[3:11] != b'EXFAT   ' or boot[510:512] != b'\x55\xaa':
            raise ValueError('APR: expected an exFAT converter output')
        sector_shift, cluster_shift, fats = boot[108:111]
        if not 9 <= sector_shift <= 12 or cluster_shift + sector_shift > 25 or fats not in (1, 2):
            raise ValueError('APR: unsupported exFAT geometry')
        sector = 1 << sector_shift
        self.cluster_bytes = sector << cluster_shift
        volume = struct.unpack_from('<Q', boot, 72)[0] * sector
        fat_offset, fat_sectors, heap, self.count, root = struct.unpack_from('<5I', boot, 80)
        self.heap = heap * sector
        active = struct.unpack_from('<H', boot, 106)[0] & 1
        if (volume > self.length or self.heap + self.count * self.cluster_bytes > volume or
                fat_sectors * sector < (self.count + 2) * 4 or
                (fat_offset + fat_sectors * fats) * sector > self.heap or active >= fats):
            raise ValueError('APR: invalid exFAT bounds')
        self.fat = (fat_offset + active * fat_sectors) * sector
        self.spans = self.find_eboot(root)

    def read(self, offset, length):
        if offset < 0 or length < 0 or offset + length > self.length:
            raise ValueError('APR: read exceeds image bounds')
        self.image.seek(offset)
        data = self.image.read(length)
        if len(data) != length:
            raise ValueError('APR: truncated image')
        return data

    def cluster_offset(self, cluster):
        if not 2 <= cluster < self.count + 2:
            raise ValueError('APR: invalid cluster')
        return self.heap + (cluster - 2) * self.cluster_bytes

    def chain(self, first, limit, contiguous=False):
        cluster, seen = first, set()
        for _ in range(limit):
            if cluster in seen:
                raise ValueError('APR: cyclic cluster chain')
            seen.add(cluster)
            yield self.cluster_offset(cluster)
            if contiguous:
                cluster += 1
            else:
                cluster = struct.unpack('<I', self.read(self.fat + cluster * 4, 4))[0]
                if cluster >= 0xfffffff8:
                    return
        if not contiguous:
            raise ValueError('APR: cluster chain exceeds limit')

    def find_eboot(self, root):
        # Directory traversal is capped independently of executable/image size.
        directory = bytearray()
        for offset in self.chain(root, max(1, 8 * 1024 * 1024 // self.cluster_bytes)):
            chunk = self.read(offset, self.cluster_bytes)
            directory.extend(chunk)
            if any(chunk[i] == 0 for i in range(0, len(chunk), 32)):
                break
        found = []
        i = 0
        while i < len(directory):
            kind = directory[i]
            if kind == 0:
                break
            if kind != 0x85:
                i += 32
                continue
            secondaries = directory[i + 1]
            end = i + (secondaries + 1) * 32
            if not 2 <= secondaries <= 18 or end > len(directory):
                raise ValueError('APR: invalid directory entry set')
            entry = directory[i:end]
            checksum = 0
            for j, byte in enumerate(entry):
                if j not in (2, 3):
                    checksum = (((checksum & 1) << 15) + (checksum >> 1) + byte) & 0xffff
            if checksum != struct.unpack_from('<H', entry, 2)[0] or entry[32] != 0xc0:
                raise ValueError('APR: invalid directory entry checksum/stream')
            name = bytearray()
            for at in range(64, len(entry), 32):
                if entry[at] != 0xc1:
                    raise ValueError('APR: unsupported filename entry')
                name.extend(entry[at + 2:at + 32])
            name = name[:entry[35] * 2].decode('utf-16-le')
            if name.casefold() == 'eboot.bin':
                if name != 'eboot.bin' or struct.unpack_from('<H', entry, 4)[0] & 0x10:
                    raise ValueError('APR: invalid executable name/type')
                valid = struct.unpack_from('<Q', entry, 40)[0]
                first, size = struct.unpack_from('<IQ', entry, 52)
                if valid != size or not 0 < size:
                    raise ValueError('APR: invalid executable length')
                if size > MAX_EXECUTABLE:
                    raise ScanLimitError('Executable exceeds APR scan budget; no bytes changed')
                count = (size + self.cluster_bytes - 1) // self.cluster_bytes
                # Allow one more FAT node to distinguish a complete chain from
                # a too-short one; the root/file may also be contiguous.
                spans = []
                for offset in self.chain(first, count + 1, bool(entry[33] & 2)):
                    length = min(size, self.cluster_bytes)
                    spans.append((offset, length))
                    size -= length
                    if not size:
                        break
                if size:
                    raise ValueError('APR: truncated executable cluster chain')
                found.append(spans)
            i = end
        if len(found) != 1:
            raise ValueError('APR: expected one root eboot.bin')
        return found[0]

    def executable(self):
        return b''.join(self.read(offset, size) for offset, size in self.spans)

    def replace(self, offset, data):
        if offset < 0 or offset + len(data) > sum(n for _, n in self.spans):
            raise ValueError('APR: patch exceeds executable bounds')
        for start, length in self.spans:
            if offset >= length:
                offset -= length
                continue
            count = min(length - offset, len(data))
            self.image.seek(start + offset)
            if self.image.write(data[:count]) != count:
                raise OSError('APR: short patch write')
            data = data[count:]
            offset = 0
            if not data:
                return


def repair_image(path, title, version, directory, save):
    """Return a JSON report. Unknown executables remain unchanged, never guessed."""
    fd = os.open(path, os.O_RDWR | os.O_NOFOLLOW)
    with os.fdopen(fd, 'r+b') as image:
        if not stat.S_ISREG(os.fstat(image.fileno()).st_mode):
            raise ValueError('APR: output must be a regular file')
        try:
            volume = ExfatExecutable(image)
        except ScanLimitError as error:
            report = dict(schema=1, policy='reviewed-sha256-v1', executable='eboot.bin',
                          titleId=title, version=version, status='unrecognized',
                          candidate=None, reason=str(error), scanned=False)
            save(directory, report, 'apr-fix')
            return report
        original = volume.executable()
        digest = hashlib.sha256(original).hexdigest()
        patch = next((p for p in PATCHES if p['original'] == digest or p['patched'] == digest), None)
        report = dict(schema=1, policy='reviewed-sha256-v1', executable='eboot.bin',
                      originalSha256=digest, titleId=title, version=version)
        if patch is None:
            report.update(status='unrecognized', candidate=APR_MESSAGE in original,
                          reason='No reviewed executable hash; no bytes changed')
        else:
            if patch['title'] != title or patch['version'] != version:
                raise ValueError('APR: reviewed executable does not match title/version')
            if digest == patch['patched']:
                report.update(status='already_applied', patchedSha256=digest)
            else:
                before, after = bytes.fromhex(patch['before']), bytes.fromhex(patch['after'])
                at = patch['offset']
                if original[at:at + len(before)] != before:
                    raise ValueError('APR: expected instruction bytes do not match')
                expected = original[:at] + after + original[at + len(before):]
                if len(before) != len(after) or hashlib.sha256(expected).hexdigest() != patch['patched']:
                    raise ValueError('APR: reviewed replacement hash does not match')
                backup = directory / 'apr-original-eboot.self'
                retain_original(backup, original, digest)
                report.update(status='prepared', backup=backup.name, selfOffset=at,
                              before=before.hex(), after=after.hex(), patchedSha256=patch['patched'])
                save(directory, report, 'apr-fix')
                try:
                    volume.replace(at, after)
                    image.flush(); os.fsync(image.fileno())
                    if hashlib.sha256(volume.executable()).hexdigest() != patch['patched']:
                        raise ValueError('APR: patched executable verification failed')
                except Exception:
                    volume.replace(at, before)
                    image.flush(); os.fsync(image.fileno())
                    raise
                report['status'] = 'applied'
        save(directory, report, 'apr-fix')
        return report
