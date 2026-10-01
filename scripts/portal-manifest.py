#!/usr/bin/env python3
"""Refresh the static portal manifest before publishing a versioned release."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / 'vps-site'
manifest = root / 'manifest.json'
previous = json.loads(manifest.read_text())
previous['release'] = '2026-10-01-botty-plus-artwork'
previous['sha256'] = {
    str(file.relative_to(root)): hashlib.sha256(file.read_bytes()).hexdigest()
    for file in sorted(root.rglob('*'))
    if file.is_file() and file != manifest and not any(part.startswith('.') for part in file.relative_to(root).parts)
}
manifest.write_text(json.dumps(previous, indent=2) + '\n')
print(len(previous['sha256']), 'portal files indexed')
