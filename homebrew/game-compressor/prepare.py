#!/usr/bin/env python3
"""Prepare the pinned Library compression worker. No download or deployment."""
from pathlib import Path
import hashlib,json,subprocess,tarfile
root=Path(__file__).resolve().parent
meta=json.loads((root/'provenance.json').read_text())
a=root/'vendor'/meta['archive']
assert hashlib.sha256(a.read_bytes()).hexdigest()==meta['sha256']
out=root/'build'
if out.exists():raise SystemExit('Preserve/remove the existing build directory before preparing again')
out.mkdir()
with tarfile.open(a) as t:
 for m in t.getmembers():
  p=Path(m.name)
  assert not p.is_absolute() and '..' not in p.parts and (m.isfile() or m.isdir())
 t.extractall(out,filter='data')
subprocess.run(['patch','-p1','-i',str(root/'patches/botty-copy-only.patch')],cwd=out,check=True)

subprocess.run(["patch","-p1","-i",str(root/"patches/botty-library.patch")],cwd=out,check=True)
