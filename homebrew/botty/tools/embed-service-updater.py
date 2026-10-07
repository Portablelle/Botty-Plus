import gzip
import hashlib
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
payload = source.read_bytes()
if not payload.startswith(b"\x7fELF") or not 4 <= len(payload) <= 32 * 1024 * 1024:
    raise SystemExit("Invalid service updater ELF")
compressed = gzip.compress(payload, mtime=0)
rows = [",".join(str(value) for value in compressed[i:i + 24]) for i in range(0, len(compressed), 24)]
destination.write_text(
    "#pragma once\nnamespace botty {\n"
    "inline constexpr unsigned char serviceUpdaterGzip[]={\n" + ",\n".join(rows) + "\n};\n"
    f"inline constexpr size_t serviceUpdaterSize={len(payload)};\n"
    f'inline constexpr const char* serviceUpdaterHash="{hashlib.sha256(payload).hexdigest()}";\n'
    "}\n"
)
