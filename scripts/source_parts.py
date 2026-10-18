"""Publish and reconstruct complete sources using GitHub-sized, hashed parts."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import tarfile
import tempfile

PART_SIZE = 90 * 1024 * 1024
MANIFEST = 'source/manifest.json'


def entry_index(archive):
    if not archive.is_file() or archive.stat().st_size < 10:
        return None
    try:
        with tarfile.open(archive, 'r|gz') as tar:
            first = tar.next()
            if first is None or first.name != 'README.txt':
                return None
            second = tar.next()
            if second is None or second.name != 'manifest.json' or second.size > 65536:
                raise ValueError('Invalid source entry archive')
            return json.loads(tar.extractfile(second).read())
    except (tarfile.ReadError, EOFError, gzip.BadGzipFile):
        return None


def metadata(base):
    path = Path(base) / MANIFEST
    if not path.exists():
        if entry_index(Path(base) / 'botty-native-source.tar.gz') is not None:
            raise ValueError('Source entry archive is missing its parts index')
        return None
    if path.is_symlink() or path.parent.is_symlink() or path.stat().st_size > 65536:
        raise ValueError('Symbolic source index')
    doc = json.loads(path.read_text())
    if (not isinstance(doc, dict) or doc.get('schema') != 1 or doc.get('archive') != 'botty-native-source.tar.gz' or
            type(doc.get('size')) is not int or doc['size'] < 1 or
            not re.fullmatch('[a-f0-9]{64}', doc.get('sha256', '')) or
            not isinstance(doc.get('parts'), list) or not 1 <= len(doc['parts']) <= 64):
        raise ValueError('Invalid source index')
    total = 0
    for index, part in enumerate(doc['parts'], 1):
        if (not isinstance(part, dict) or part.get('path') != f'source/native-source.part{index:03d}' or
                type(part.get('size')) is not int or not 1 <= part['size'] <= PART_SIZE or
                not re.fullmatch('[a-f0-9]{64}', part.get('sha256', ''))):
            raise ValueError('Invalid source part')
        total += part['size']
    if total != doc['size']:
        raise ValueError('Source part sizes disagree')
    index = entry_index(Path(base) / 'botty-native-source.tar.gz')
    if index is not None and index != doc:
        raise ValueError('Source entry archive and parts index disagree')
    return doc


def source_names(base, verify=True):
    doc = metadata(base)
    if doc is None:
        return []
    names = [MANIFEST]
    combined = hashlib.sha256()
    for part in doc['parts']:
        path = Path(base) / part['path']
        if path.is_symlink() or not path.is_file() or path.stat().st_size != part['size']:
            raise ValueError('Missing or altered source part: ' + part['path'])
        if verify:
            digest = hashlib.sha256()
            with path.open('rb') as source:
                for block in iter(lambda: source.read(1024 * 1024), b''):
                    digest.update(block)
                    combined.update(block)
            if digest.hexdigest() != part['sha256']:
                raise ValueError('Source part hash mismatch: ' + part['path'])
        names.append(part['path'])
    if verify and combined.hexdigest() != doc['sha256']:
        raise ValueError('Complete source hash mismatch')
    actual = {p.relative_to(base).as_posix() for p in (Path(base) / 'source').rglob('*') if not p.is_dir() or p.is_symlink()}
    if actual != set(names):
        raise ValueError('Unlisted source part')
    return names


def publish(archive, base):
    """Replace a large archive with an index bundle; retain every byte in parts."""
    archive, base = Path(archive), Path(base)
    if entry_index(archive) is not None:
        source_names(base)
        return
    if archive.stat().st_size <= PART_SIZE:
        if (base / 'source').exists():
            source_names(base)
            shutil.rmtree(base / 'source')
        return
    with tempfile.TemporaryDirectory(prefix='.source-parts-', dir=base) as temporary:
        stage = Path(temporary)
        combined = hashlib.sha256()
        parts = []
        with archive.open('rb') as source:
            while block := source.read(PART_SIZE):
                name = f'native-source.part{len(parts)+1:03d}'
                (stage / name).write_bytes(block)
                combined.update(block)
                parts.append(dict(path='source/' + name, size=len(block), sha256=hashlib.sha256(block).hexdigest()))
        doc = dict(schema=1, archive='botty-native-source.tar.gz', size=sum(p['size'] for p in parts),
                   sha256=combined.hexdigest(), parts=parts)
        (stage / 'manifest.json').write_text(json.dumps(doc, indent=2) + '\n')
        target = base / 'source'
        if target.is_symlink():
            raise ValueError('Symbolic source output')
        if target.exists():
            shutil.rmtree(target)
        stage.rename(target)
        # The small entry archive contains the index and standalone reconstruction tool.
        readme = ('Complete corresponding sources are stored losslessly in the hashed parts listed in manifest.json.\n'
                  'From a complete repository checkout, run:\n'
                  '  python3 restore.py /path/to/packages/botty-native complete-source.tar.gz\n'
                  'For Portal, use /path/to/apps/botty-native instead. Extract complete-source.tar.gz to build.\n'
                  'The entry archive is not the complete source tree on its own.\n')
        files = {'README.txt': readme.encode(), 'manifest.json': (target / 'manifest.json').read_bytes(),
                 'restore.py': Path(__file__).read_bytes()}
        temporary_entry = None
        try:
            with tempfile.NamedTemporaryFile(prefix='.source-entry-', dir=base, delete=False) as raw:
                temporary_entry = Path(raw.name)
                with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as zipped:
                    with tarfile.open(fileobj=zipped, mode='w') as tar:
                        for name, data in files.items():
                            info = tarfile.TarInfo(name); info.size = len(data); info.mode = 0o644
                            tar.addfile(info, io.BytesIO(data))
                raw.flush(); os.fsync(raw.fileno())
            os.replace(temporary_entry, archive)
            fd = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
            try: os.fsync(fd)
            finally: os.close(fd)
        finally:
            if temporary_entry is not None:
                temporary_entry.unlink(missing_ok=True)


def restore(base, output):
    """Verify the complete source before publishing; never replace an existing file."""
    base, output = Path(base), Path(output)
    doc = metadata(base)
    if doc is None:
        raise ValueError('Source index missing')
    source_names(base)
    with tempfile.NamedTemporaryFile(prefix='.source-restore-', dir=output.parent, delete=False) as target:
        temporary = Path(target.name)
        try:
            combined = hashlib.sha256()
            for part in doc['parts']:
                digest = hashlib.sha256(); size = 0
                with (base / part['path']).open('rb') as source:
                    for block in iter(lambda: source.read(1024 * 1024), b''):
                        target.write(block); digest.update(block); combined.update(block); size += len(block)
                if size != part['size'] or digest.hexdigest() != part['sha256']:
                    raise ValueError('Source part changed during reconstruction')
            if combined.hexdigest() != doc['sha256']:
                raise ValueError('Complete source hash mismatch')
            target.flush(); os.fsync(target.fileno())
            os.link(temporary, output)  # Exclusive publication; no overwritten source/output.
        finally:
            temporary.unlink(missing_ok=True)
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Reconstruct complete corresponding sources from checked parts.')
    parser.add_argument('package', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(restore(args.package, args.output))
