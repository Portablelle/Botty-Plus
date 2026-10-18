"""Verify source archives retain usable Git provenance without local secrets."""
import importlib.util
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('engine_sources',Path(__file__).resolve().parents[1]/'sources.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class SourceGitTests(unittest.TestCase):
    def test_offline_revision_and_index_without_private_git_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=root/'original';source.mkdir()
            def git(*args): return subprocess.check_output(['git','-C',str(source),*args],text=True).strip()
            git('init','-q');(source/'code.c').write_text('original')
            git('add','code.c');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','Pinned snapshot')
            pinned=git('rev-parse','HEAD')
            (source/'private.txt').write_text('not part of published snapshot')
            git('add','private.txt');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','Unrelated private commit')
            private=git('rev-parse','HEAD');git('reset','--hard',pinned)
            git('remote','add','origin','https://fixture:secret@example.invalid/private')
            (source/'.git/hooks/private-hook').write_text('secret')
            archive=root/'sources.tar'
            with tarfile.open(archive,'w') as out:module.add_git_metadata(source,out,'restored')
            with tarfile.open(archive) as inp:inp.extractall(root,filter='data')
            restored=root/'restored';(restored/'code.c').write_text('patched')
            command=['git','-C',str(restored)]
            self.assertEqual(subprocess.check_output(command+['rev-parse','HEAD'],text=True).strip(),pinned)
            self.assertEqual(subprocess.run(command+['cat-file','-e',private],capture_output=True).returncode,1)
            self.assertFalse((restored/'.git/hooks').exists())
            self.assertEqual(subprocess.run(command+['config','--get','remote.origin.url'],capture_output=True).returncode,1)
            self.assertEqual(subprocess.check_output(command+['show','HEAD:code.c'],text=True),'original')
            self.assertIn('patched',subprocess.check_output(command+['diff','--','code.c'],text=True))
