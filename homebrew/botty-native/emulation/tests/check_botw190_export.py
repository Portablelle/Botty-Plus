#!/usr/bin/env python3
"""Compare every C++ configuration to the pinned upstream Python exporter."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'vendor/botw190/src'))
from modules.GameManager.ExefsPatch import load_patch, encode_ipswitch, module_patches
from modules.GameManager.BotwNativePatch import build_patch


def records(text):
    return dict(line.split() for line in text.splitlines() if line and not line.startswith('@'))


folder = ROOT / 'vendor/botw190/src/PatchInfo/Breath Of The Wild'
base = load_patch(folder / 'FPS-1.9.json')
cases = json.loads(Path(sys.argv[1]).read_text())
assert len(cases) == 160
for case in cases:
    patch = build_patch(base, folder / 'NativeFPS-1.9.json', fps=case['fps'],
                        far_clip=case['distance'], fov=case['fov'])
    for module in module_patches(patch):
        actual = case['files'][f"exefs/{module['build_id']}.pchtxt"]
        expected = encode_ipswitch(module).decode()
        assert records(actual) == records(expected), case
        assert actual.splitlines()[0] == expected.splitlines()[0]
        assert '@flag offset_shift 0x100' in actual
print('BOTW 1.9: all 160 C++ main/SDK exports match the pinned upstream generator PASS')
