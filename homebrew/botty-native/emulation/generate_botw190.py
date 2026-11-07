#!/usr/bin/env python3
"""Reproduce the separately licensed BOTW 1.9 runtime template (no game files)."""
import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'vendor/botw190/src'))
from modules.GameManager.ExefsPatch import load_patch
from modules.GameManager.BotwNativePatch import build_patch


def generated():
    folder = ROOT / 'vendor/botw190/src/PatchInfo/Breath Of The Wild'
    patch = build_patch(load_patch(folder / 'FPS-1.9.json'), folder / 'NativeFPS-1.9.json',
                        fps=60, far_clip=5000, fov=60)
    return json.dumps(dict(schema=1, main=patch['build_id'], sdk=patch['modules'][0]['build_id'],
                           main_sha256=patch['main_sha256'], sdk_sha256=patch['modules'][0]['main_sha256'],
                           patches=patch['patches'], sdk_patches=patch['modules'][0]['patches']),
                      indent=2) + '\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    target = ROOT / 'ultracam/botw190/patches.json'
    content = generated()
    if args.check:
        if target.read_text() != content:
            raise SystemExit('BOTW 1.9 template differs; run generate_botw190.py')
    else:
        target.write_text(content)
