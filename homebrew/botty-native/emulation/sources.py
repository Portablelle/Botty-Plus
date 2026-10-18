#!/usr/bin/env python3
"""Archive the isolated engine build's corresponding sources, excluding private data."""
import argparse
import gzip
import subprocess
import tempfile
from pathlib import Path
import tarfile

# Build intermediates are reproducible from the included source and build scripts.
EXCLUDED_DIRS = {'.git', '__pycache__', 'build', 'dist', 'CMakeFiles', '_build', '.local'}
EXCLUDED_SUFFIXES = {'.o', '.a', '.so', '.prx', '.elf', '.bin', '.nca', '.nsp', '.xci', '.keys', '.pyc', '.log'}


def add_git_metadata(tree, archive, prefix):
    """Keep only the pinned commit/tree, an index and a detached HEAD.

    No remotes, credentials, hooks, reflogs, alternate object stores or unrelated
    commits are copied. Existing build scripts can inspect the source revision
    offline, including repositories nested inside the dependency trees.
    """
    command = ['git', '-c', 'safe.directory='+str(tree), '-C', str(tree)]
    head = subprocess.check_output(command+['rev-parse', 'HEAD'], text=True).strip()
    objects = subprocess.check_output(command+['rev-list', '--objects', '--no-object-names', head+'^{tree}'])
    with tempfile.TemporaryDirectory(prefix='botty-source-git-') as temporary:
        metadata = Path(temporary)/'.git'
        packs = metadata/'objects/pack'; packs.mkdir(parents=True)
        pack = packs/'snapshot.pack'
        with pack.open('wb') as output:
            subprocess.run(command+['pack-objects', '--stdout'], input=head.encode()+b'\n'+objects, stdout=output, check=True)
        digest = subprocess.check_output(['git', 'index-pack', str(pack)], text=True).strip()
        pack.rename(packs/('pack-'+digest+'.pack'))
        (packs/'snapshot.idx').rename(packs/('pack-'+digest+'.idx'))
        (metadata/'refs/heads').mkdir(parents=True)
        (metadata/'refs/heads/source-snapshot').write_text(head+'\n')
        (metadata/'HEAD').write_text(head+'\n')
        (metadata/'shallow').write_text(head+'\n')
        (metadata/'config').write_text('[core]\n repositoryformatversion = 0\n bare = false\n filemode = true\n')
        subprocess.run(['git', '--git-dir='+str(metadata), '--work-tree='+str(tree), 'read-tree', 'HEAD'], check=True)
        for file in sorted(metadata.rglob('*')):
            if not file.is_file(): continue
            info = archive.gettarinfo(str(file), arcname=prefix+'/.git/'+file.relative_to(metadata).as_posix())
            info.uid = info.gid = info.mtime = 0; info.uname = info.gname = ''
            with file.open('rb') as stream: archive.addfile(info, stream)


def collect(root, output):
    scratch = Path((root/'.local/headless-cache').read_text().strip())
    if (scratch/'owner').read_text().strip() != str(root):
        raise RuntimeError('Unexpected engine build cache')
    sources = [('prosperoeden', root), ('eden-core', scratch/'source')]
    for name in ('ps5-native-app-boilerplate', 'mihawk-vulkan-review', 'mihawk-mesa-review', 'mihawk-sdk-review'):
        sources.append((name, root.parent/name))
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0, compresslevel=1) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as archive:
            for prefix, tree in sources:
                if not tree.is_dir():
                    raise RuntimeError('Missing corresponding source: '+prefix)
                # walk prunes build outputs before descending into large trees.
                import os
                for directory, dirs, files in os.walk(tree, followlinks=False):
                    if (Path(directory)/'.git').exists():
                        add_git_metadata(Path(directory), archive, prefix if Path(directory)==tree else prefix+'/'+Path(directory).relative_to(tree).as_posix())
                    dirs[:] = sorted(d for d in dirs if d not in EXCLUDED_DIRS and not (Path(directory)/d).is_symlink())
                    for name in sorted(files):
                        file = Path(directory)/name
                        relative = file.relative_to(tree)
                        if name == '.git' or file.is_symlink() or not file.is_file() or file.suffix.lower() in EXCLUDED_SUFFIXES:
                            continue
                        if any(part.lower() in ('keys', 'firmware', 'roms') for part in relative.parts):
                            continue
                        info = archive.gettarinfo(str(file), arcname=prefix+'/'+relative.as_posix())
                        info.uid = info.gid = info.mtime = 0
                        info.uname = info.gname = ''
                        with file.open('rb') as stream:
                            archive.addfile(info, stream)
    print('Corresponding engine sources archived:', output)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    collect(args.checkout.resolve(), args.output.resolve())
