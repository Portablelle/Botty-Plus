#!/usr/bin/env python3
"""Bake the original transparent PNGs to bounded native RGBA (Pillow 12)."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
for name in ('courier', 'extractor', 'vault'):
    with Image.open(ROOT / 'artwork' / f'{name}.png') as source:
        image = source.convert('RGBA')
        image.thumbnail((512, 512), Image.Resampling.LANCZOS)
        canvas = Image.new('RGBA', (512, 512))
        canvas.paste(image, ((512-image.width)//2, (512-image.height)//2))
        (ROOT / 'assets' / f'{name}.rgba').write_bytes(canvas.tobytes())
