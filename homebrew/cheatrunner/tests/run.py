#!/usr/bin/env python3
"""Compile the actual patched source for focused host regressions."""
import pathlib, subprocess, sys, tempfile
src = pathlib.Path(sys.argv[1]).resolve() / 'src'
tests = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    for name in ['worker', 'version']:
        cmd = ['cc', '-ffunction-sections', '-fdata-sections', '-I', str(src), str(tests / (name + '.c'))]
        if name == 'version':
            cmd += [str(src / 'cr_remote_sources.c'), '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections']
        binary = str(pathlib.Path(tmp) / name)
        subprocess.run(cmd + ['-o', binary], check=True)
        subprocess.run([binary], check=True)
print('Worker stack/error paths and version parsing passed')
