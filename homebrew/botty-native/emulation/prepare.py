#!/usr/bin/env python3
"""Prepare the pinned Eden engine for Botty's title. Run in an isolated build tree."""
import argparse
import json
import hashlib
from pathlib import Path
import subprocess
import re
import shutil

from provenance import COMMIT, REPOSITORY


def prepare(root):
    if not root.exists():
        subprocess.run(['git', 'clone', '--branch', 'v1.005.000', '--depth', '1', REPOSITORY, str(root)], check=True)
    if subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() != COMMIT:
        raise RuntimeError('Unexpected Eden source revision')
    if subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain'], text=True).strip():
        raise RuntimeError('Use a clean isolated Eden checkout; patches are not reapplied to modified sources')
    patch = Path(__file__).resolve().parent / 'patches/standalone.diff'
    subprocess.run(['git', '-C', str(root), 'apply', '--check', str(patch)], check=True)
    subprocess.run(['git', '-C', str(root), 'apply', str(patch)], check=True)
    # The builder and assembler use the same canonical provenance policy.
    shutil.copyfile(Path(__file__).with_name('provenance.py'), root / 'tools/botty-provenance.py')
    names = re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.MULTILINE)
    names.append('tools/botty-provenance.py')
    sources = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names}
    (root / 'BOTTY-ENGINE.json').write_text(json.dumps(dict(schema=1, upstream=REPOSITORY, commit=COMMIT,
        titleId='PPSA99071', executable='emulation/eboot.bin', dataRoot='/data/botty/emulation/switch',
        selfUpdate=False, hardwareValidated=False, sourceFiles=sources,
        patchSha256=hashlib.sha256(patch.read_bytes()).hexdigest()), indent=2) + '\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout', type=Path)
    prepare(parser.parse_args().checkout.resolve())
