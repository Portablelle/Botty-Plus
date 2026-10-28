#!/usr/bin/env python3
"""Build the Botty+ release assets from a clean default-branch checkout and publish them on GitHub.

Assets, in dist/: botty-plus-<version>.tar.gz (every package, as package-release.py makes it),
botty-plus-<version>-Folder.zip (the native title as PPSA99071/), SHA256SUMS for the archive and
Botty-Catalog-SHA256SUMS.txt for the folder. The release notes are docs/releases/<version>.md
unless --notes names another file; the title is their first heading unless --title is given.
--dry-run builds and checks everything without creating the tag or the release. The release is
uploaded as a draft and published, as latest, only once every asset is checked; a failed run leaves
the draft to inspect or delete.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'Portablelle/Botty-Plus'
BRANCH = 'main'
# GitHub refuses release assets of 2 GiB or more.
ASSET_LIMIT = 2 * 1024 ** 3


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(path.parent))
    return module


def run(command, **options):
    result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, **options)
    if result.returncode:
        raise SystemExit(f'{" ".join(command[:3])} failed: {result.stderr.strip() or result.stdout.strip()}')
    return result.stdout.strip()


def sha256(path):
    # Streamed in blocks: hashlib.file_digest needs Python 3.11, and the documented minimum is 3.10.
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def folder_zip(native, output, title='PPSA99071'):
    """The native package as <title>/..., regular files only, in a stable order."""
    native, output = Path(native), Path(output)
    files = sorted(path for path in native.rglob('*') if path.is_file() or path.is_symlink())
    for path in files:
        if path.is_symlink():
            raise ValueError('Symbolic native package file: ' + path.relative_to(native).as_posix())
        if path.name.startswith('._') or path.name == '.DS_Store':
            raise ValueError('Host metadata in the native package: ' + path.relative_to(native).as_posix())
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, allowZip64=True) as archive:
        for path in files:
            archive.write(path, title + '/' + path.relative_to(native).as_posix())
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise ValueError('Corrupted folder archive')
        for path in files:
            if archive.getinfo(title + '/' + path.relative_to(native).as_posix()).file_size != path.stat().st_size:
                raise ValueError('Folder archive size mismatch: ' + path.name)
    return len(files)


def notes_title(text):
    first = text.lstrip().splitlines()[0] if text.strip() else ''
    if not first.startswith('# ') or not first[2:].strip():
        raise ValueError('Release notes must start with a "# " title, or pass --title')
    return first[2:].strip()


def check_checkout(tag):
    if run(['git', 'status', '--porcelain', '--untracked-files=no']):
        raise SystemExit('Commit or discard tracked changes first: the release must match a pushed commit')
    run(['git', 'fetch', '--quiet', 'origin', BRANCH, '--tags'])
    head = run(['git', 'rev-parse', 'HEAD'])
    if head != run(['git', 'rev-parse', 'origin/' + BRANCH]):
        raise SystemExit(f'HEAD is not origin/{BRANCH}: release only what has been merged')
    if subprocess.run(['git', 'rev-parse', '-q', '--verify', 'refs/tags/' + tag], cwd=ROOT,
                      capture_output=True).returncode == 0:
        raise SystemExit(f'Tag {tag} already exists')
    if subprocess.run(['gh', 'release', 'view', tag, '--repo', REPOSITORY], cwd=ROOT,
                      capture_output=True).returncode == 0:
        raise SystemExit(f'GitHub release {tag} already exists')
    return head


def build(version):
    packages = load('botty_packages', ROOT / 'scripts/botty-packages.py')
    record = packages.verify(ROOT / 'packages')
    packages.verify_public_version(record)
    if record['version'] != version:
        raise SystemExit('Packaged release does not match release.json')
    load('package_release', ROOT / 'scripts/package-release.py').main()
    dist = ROOT / 'dist'
    archive = dist / f'botty-plus-{version}.tar.gz'
    folder = dist / f'botty-plus-{version}-Folder.zip'
    count = folder_zip(ROOT / 'packages/botty-native', folder)
    (dist / 'Botty-Catalog-SHA256SUMS.txt').write_text(f'{sha256(folder)}  {folder.name}\n')
    assets = [archive, folder, dist / 'SHA256SUMS', dist / 'Botty-Catalog-SHA256SUMS.txt']
    for asset in assets:
        if asset.stat().st_size >= ASSET_LIMIT:
            raise SystemExit(f'{asset.name} exceeds the GitHub release asset limit')
    print(f'Built {archive.name}, {folder.name} ({count} files) and their checksums')
    return assets


def verify_uploaded(tag, assets, draft):
    published = json.loads(run(['gh', 'release', 'view', tag, '--repo', REPOSITORY, '--json', 'assets,isDraft']))
    # GitHub's own SHA-256 of each stored asset, so contents are checked without downloading them.
    digests = {asset['name']: (asset['size'], asset.get('digest')) for asset in published['assets']}
    if any(asset.get('digest') is None for asset in published['assets']):
        raise SystemExit(f'gh did not report asset digests: update gh; the {tag} draft stays unpublished')
    for asset in assets:
        if digests.get(asset.name) != (asset.stat().st_size, 'sha256:' + sha256(asset)):
            raise SystemExit(f'Uploaded {asset.name} does not match the local asset; the {tag} draft stays unpublished')
    if published['isDraft'] != draft:
        raise SystemExit(f'{tag} is {"a draft" if published["isDraft"] else "already published"}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--notes', type=Path, help='release notes (default: docs/releases/<version>.md)')
    parser.add_argument('--title', help='release title (default: the notes\' first heading)')
    parser.add_argument('--dry-run', action='store_true', help='build and check without publishing')
    args = parser.parse_args()
    version = load('release_version', ROOT / 'homebrew/botty-native/tools/release_version.py').release_version()
    tag = 'v' + version
    # Absolute: gh runs from the repository root, wherever this script was started.
    notes = (args.notes or ROOT / f'docs/releases/{version}.md').resolve()
    if not notes.is_file():
        raise SystemExit(f'Missing release notes: {notes}')
    title = args.title or notes_title(notes.read_text())
    head = check_checkout(tag)
    assets = build(version)
    if args.dry_run:
        print(f'Dry run: would publish {tag} "{title}" at {head[:12]} with {", ".join(a.name for a in assets)}')
        return
    # A draft first: nothing is public, and nothing is latest, until every asset is uploaded and checked.
    subprocess.run(['gh', 'release', 'create', tag, *map(str, assets), '--repo', REPOSITORY, '--target', head,
                    '--title', title, '--notes-file', str(notes), '--draft'], cwd=ROOT, check=True)
    verify_uploaded(tag, assets, draft=True)
    run(['gh', 'release', 'edit', tag, '--repo', REPOSITORY, '--draft=false', '--latest'])
    verify_uploaded(tag, assets, draft=False)
    print(f'Published https://github.com/{REPOSITORY}/releases/tag/{tag}')


if __name__ == '__main__':
    main()
