"""Create deterministic source archives from explicit component directories."""
import gzip
from pathlib import Path
import tarfile

PRIVATE_NAMES = {'prowlarr.json', 'settings.json', 'api-key', '__pycache__', 'fixtures'}
PRIVATE_SUFFIXES = {'.key', '.p12', '.pfx', '.log', '.pyc', '.bak'}


def source_archive(source, output, names):
    source, output = Path(source), Path(output)
    files = []
    for name in names:
        path = source / name
        if path.is_symlink() or not path.exists():
            raise ValueError('Missing or symbolic source path: ' + name)
        for file in sorted(path.rglob('*')) if path.is_dir() else [path]:
            rel = file.relative_to(source)
            if file.is_symlink():
                raise ValueError('Symbolic source file: ' + str(rel))
            if any(part.startswith('.') or part in PRIVATE_NAMES or
                   part.startswith('botty-credentials') for part in rel.parts):
                continue
            if file.is_file() and file.suffix not in PRIVATE_SUFFIXES:
                files.append(file)
    with output.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as archive:
            for file in sorted(files):
                info = archive.gettarinfo(str(file), arcname=source.name + '/' + file.relative_to(source).as_posix())
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ''
                info.mode = 0o755 if file.stat().st_mode & 0o111 else 0o644
                with file.open('rb') as stream:
                    archive.addfile(info, stream)
