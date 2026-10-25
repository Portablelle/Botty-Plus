"""Embed the unchanged pinned PS5 forwarder template and its metadata."""
import hashlib
from pathlib import Path
import sys
import tarfile
import zlib

source, destination = map(Path, sys.argv[1:])
if hashlib.sha256(source.read_bytes()).hexdigest() != "f4d1616aad3f13fbf0a8d57e2f5ebf42b248070ce871929c13772cdc5b9ac0bc":
    raise SystemExit("Pinned PS5 forwarder source digest mismatch")
with tarfile.open(source) as archive:
    files = {name: archive.extractfile(name).read() for name in (
        'template/eboot.bin', 'template/sce_module/libc.prx', 'forwarder/param.json')}
output = '#pragma once\nnamespace botty {\n'
for name, key in [('template/eboot.bin', 'forwarderProgram'),
                  ('template/sce_module/libc.prx', 'forwarderRuntime')]:
    data = files[name]
    compressed = zlib.compress(data, 9)
    output += 'inline constexpr unsigned char '+key+'[]={\n'
    output += ',\n'.join(','.join(map(str, compressed[i:i+24])) for i in range(0, len(compressed), 24))+'\n};\n'
    output += f'inline constexpr unsigned long {key}Size={len(data)};\n'
    output += f'inline constexpr const char* {key}Hash="{hashlib.sha256(data).hexdigest()}";\n'
output += 'inline constexpr const char* forwarderParam=R"json('+files['forwarder/param.json'].decode()+')json";\n}\n'
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text(output)
