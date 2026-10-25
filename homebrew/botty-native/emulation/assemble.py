#!/usr/bin/env python3
"""Attach a separately built native engine to the Botty title; no private data."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
import tarfile
from provenance import COMMIT, RECEIPT, digest, validate

ROOT = Path(__file__).resolve().parents[1]


def validate_sources(path, provenance):
    # The archiver puts sanitized Git metadata and this source receipt first.
    scanned = 0
    with tarfile.open(path, 'r|gz') as archive:
        for member in archive:
            scanned += member.size
            if scanned > 64 * 1024 * 1024:
                break
            if member.name != 'prosperoeden/BOTTY-ENGINE.json':
                continue
            if not member.isfile() or member.size > 128 * 1024:
                break
            source = json.load(archive.extractfile(member))
            fields = ('commit', 'upstream', 'patchSha256', 'sourceFiles', 'titleId', 'dataRoot')
            if all(source.get(field) == provenance.get(field) for field in fields):
                return
            break
    raise ValueError('Corresponding-source archive does not match the built engine')


def assemble(engine):
    provenance = validate(engine, digest(ROOT / 'emulation/patches/standalone.diff'))
    validate_sources(ROOT / 'build/emulation-source.tar.gz', provenance)
    with (ROOT / 'build/emulation-source.tar.gz').open('rb') as stream:
        sources_sha = hashlib.file_digest(stream, 'sha256').hexdigest()
    app = ROOT / 'dist/PPSA99071'
    if not (app / 'eboot.bin').is_file():
        raise RuntimeError('Build Botty native UI first, on the dedicated host')
    param = json.loads((engine / 'sce_sys/param.json').read_text())
    if param['titleId'] != 'PPSA99071':
        raise RuntimeError('Engine was not built for Botty')
    if (engine / 'eboot.bin').read_bytes()[:4] != bytes.fromhex('4f153d1d'):
        raise RuntimeError('Engine is not a native FSELF')
    for path in engine.rglob('*'):
        name = path.relative_to(engine).as_posix()
        if path.is_symlink() or 'keys' in name.lower() or 'firmware' in name.lower() or path.suffix.lower() in ('.nca', '.nsp', '.xci'):
            raise RuntimeError('Private or symbolic content is forbidden in the engine package')
    target = app / 'emulation'
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(engine, target)
    shutil.copyfile(ROOT/"emulation/licenses/Montserrat-OFL.txt", target/"licenses/Montserrat-OFL.txt")
    # UltraCam is a separately licensed optional mod; no game data is included.
    shutil.copytree(ROOT/"emulation/ultracam", target/"ultracam")
    # Both processes use the engine's pinned clean-room libc from /app0/sce_module.
    shutil.copy2(engine / 'sce_module/libc.prx', app / 'sce_module/libc.prx')
    runtime = dict(schema=1, engine='ProsperoEden', commit=COMMIT,
        patchSha256=provenance['patchSha256'], provenanceSha256=digest(target / RECEIPT),
        sourcesSha256=sources_sha,
        executable='emulation/eboot.bin', dataRoot='/data/botty/emulation/switch', standalone=True,
        hardwareValidated=False, privateProvisioning=True, configuration=True, configurationProtocol=1,
        engineSha256=hashlib.sha256((target/'eboot.bin').read_bytes()).hexdigest())
    (app / 'emulation-runtime.json').write_text(json.dumps(runtime, indent=2)+'\n')
    with zipfile.ZipFile(ROOT/'dist/PPSA99071.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(app.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(ROOT/'dist'))
    subprocess.run([sys.executable, str(ROOT/'tools/package.py')], check=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('engine', type=Path)
    assemble(parser.parse_args().engine.resolve())
