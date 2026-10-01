#!/usr/bin/env python3
"""Convert the original artwork to bounded RGB data (macOS build tool only)."""
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as directory:
    bitmap = Path(directory) / 'nebula.bmp'
    subprocess.run(['sips', '-z', '540', '960', '-s', 'format', 'bmp',
                    str(ROOT / 'artwork/nebula.png'), '--out', str(bitmap)], check=True)
    data = bitmap.read_bytes()
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bits = struct.unpack_from('<H', data, 28)[0]
    compression = struct.unpack_from('<I', data, 30)[0]
    if (width, abs(height), bits, compression) != (960, 540, 24, 0):
        raise ValueError('Expected uncompressed 960 x 540 RGB bitmap')
    stride = (width * 3 + 3) // 4 * 4
    if len(data) < offset + stride * abs(height):
        raise ValueError('Truncated bitmap')
    rows = range(abs(height)) if height < 0 else range(height - 1, -1, -1)
    pixels = bytearray()
    for y in rows:
        row = data[offset + y * stride:offset + y * stride + width * 3]
        for x in range(width):
            pixels.extend(row[x * 3:x * 3 + 3][::-1])
    (ROOT / 'assets/nebula.rgb').write_bytes(pixels)
