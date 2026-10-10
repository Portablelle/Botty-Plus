#!/usr/bin/env python3
"""Verify pinned injector inputs even in SDK images without Git."""
import hashlib
import json
from pathlib import Path
import sys
manifest=json.loads((Path(__file__).parent/'ps5debug-inputs.json').read_text())
root=Path(sys.argv[1])
for name,expected in manifest['files'].items():
    path=root/name
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=expected:
        raise SystemExit(f'Pinned ps5debug-NG input mismatch: {name}')
print('Pinned ps5debug-NG sources verified:',manifest['commit'])
