"""Bind an embedded engine package to its pinned source and Botty patch."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess

COMMIT = 'a26ba788222465ec7fb94dc813958fc749734e07'
REPOSITORY = 'https://github.com/Swordpdf/ProsperoEden.git'
RECEIPT = 'botty-engine.json'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate(app, patch_sha):
    app = Path(app)
    receipt = json.loads((app / RECEIPT).read_text())
    if (receipt.get('schema') != 1 or receipt.get('commit') != COMMIT or
            receipt.get('upstream') != REPOSITORY or receipt.get('patchSha256') != patch_sha or
            receipt.get('titleId') != 'PPSA99071' or receipt.get('selfUpdate') is not False or
            receipt.get('sourceFiles', {}).get('tools/botty-provenance.py') != digest(__file__) or
            receipt.get('dataRoot') != '/data/botty/emulation/switch'):
        raise ValueError('Engine source/patch provenance mismatch')
    files = receipt.get('files')
    required = {'eboot.bin', 'sce_module/libc.prx', 'sce_sys/param.json', 'lapy.elf', 'lapy-manifest.json',
                'mihawkgen/network.fgw'}
    if not isinstance(files, dict) or not required.issubset(files):
        raise ValueError('Incomplete engine provenance')
    for name, identity in files.items():
        relative = PurePosixPath(name)
        if (relative.is_absolute() or '..' in relative.parts or str(relative) != name or
                '\\' in name or name == RECEIPT):
            raise ValueError('Unsafe engine provenance path')
        path = app / name
        if (any(p.is_symlink() for p in [path, *path.parents]) or not path.is_file() or
                path.stat().st_size != identity['size'] or digest(path) != identity['sha256']):
            raise ValueError('Engine provenance file mismatch: ' + name)
    param = json.loads((app / 'sce_sys/param.json').read_text())
    helper = json.loads((app / 'lapy-manifest.json').read_text())
    if (param.get('titleId') != 'PPSA99071' or param.get('contentVersion') != '01.005.000' or
            helper.get('target_title') != 'PPSA99071' or
            helper.get('elf_sha256') != digest(app / 'lapy.elf')):
        raise ValueError('Engine/helper identity mismatch')
    return receipt


def record(root, app):
    root, app = Path(root), Path(app)
    receipt = json.loads((root / 'BOTTY-ENGINE.json').read_text())
    head = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    if head != COMMIT or receipt.get('commit') != head:
        raise ValueError('Engine was built from an unexpected source revision')
    sources = receipt.get('sourceFiles', {})
    changed = subprocess.check_output(['git', '-C', str(root), 'diff', 'HEAD', '--name-only'], text=True).splitlines()
    if not sources or not set(changed).issubset(sources):
        raise ValueError('Unexpected engine source changes after preparation')
    for name, expected in sources.items():
        if digest(root / name) != expected:
            raise ValueError('Engine source changed after preparation: ' + name)
    receipt['files'] = {}
    for path in sorted(app.rglob('*')):
        if path.is_symlink():
            raise ValueError('Symbolic engine package file')
        if path.is_file() and path.name != RECEIPT:
            receipt['files'][path.relative_to(app).as_posix()] = dict(size=path.stat().st_size, sha256=digest(path))
    (app / RECEIPT).write_text(json.dumps(receipt, indent=2) + '\n')
    validate(app, receipt['patchSha256'])


if __name__ == '__main__':
    import sys
    record(*sys.argv[1:])
