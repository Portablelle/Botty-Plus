#!/usr/bin/env python3
"""Create a complete Botty+ release asset named after the public version."""
import hashlib
import importlib.util
from pathlib import Path
from release_sources import source_archive

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('packages', ROOT / 'scripts/botty-packages.py')
packages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packages)


def main():
    record = packages.verify(ROOT / 'packages')
    packages.sync(check=True)
    if record['version'] != packages.release_version():
        raise ValueError('Rebuild the native package for the public release version')
    output = ROOT / 'dist'
    output.mkdir(exist_ok=True)
    asset = output / f"botty-plus-{record['version']}.tar.gz"
    source_archive(ROOT / 'packages', asset, ['botty-release.json', *packages.PACKAGES])
    (output / 'SHA256SUMS').write_text(hashlib.sha256(asset.read_bytes()).hexdigest() + '  ' + asset.name + '\n')
    print(asset)


if __name__ == '__main__':
    main()
