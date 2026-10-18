import fcntl
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
from switch_prepare import SwitchPreparer


class Forge:
    @staticmethod
    def open_source(path): return path.open('rb')
    @staticmethod
    def identity(path, fd):
        stat = os.fstat(fd)
        return dict(path=str(path), size=stat.st_size, inode=stat.st_ino)


class PreparationStateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'Game.nsp').write_bytes(b'fixture')
        self.preparer = SwitchPreparer('/bin/true', '', self.root/'jobs', Forge)
        self.members = [dict(name='Game.nsp', size=7)]

    def poll(self, retry=False):
        return self.preparer.poll('a'*40, self.root, self.members, 'Fixture', retry)

    def test_setup_failures_release_worker_lock(self):
        for operation in ('request', 'state'):
            with self.subTest(operation=operation):
                target = patch.object(Path, 'write_text', side_effect=OSError('disk full')) if operation == 'request' else patch.object(self.preparer, 'save', side_effect=[None, OSError('disk full')])
                with (self.root/'Game.nsp').open('rb') as f: identity=[Forge.identity(self.root/'Game.nsp',f.fileno())]
                self.preparer.settling['a'*40]=(identity,time.monotonic()-3)
                with target, self.assertRaises(OSError): self.poll(retry=True)
                fd = os.open(self.root/'jobs/worker.lock', os.O_RDWR)
                try: fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally: os.close(fd)
                self.assertFalse(self.preparer.active)

    def test_busy_retry_is_durable_until_a_worker_can_start(self):
        self.preparer.active.add('b'*40)
        self.assertEqual(self.poll(retry=True)['status'], 'waiting')
        # A fresh adapter sees the saved retry even after the previous process ends.
        self.preparer = SwitchPreparer('/bin/true', '', self.root/'jobs', Forge)
        with patch('switch_prepare.time.monotonic', side_effect=[0,3]), patch('switch_prepare.threading.Thread') as worker:
            self.assertEqual(self.poll()['status'], 'waiting')
            self.assertEqual(self.poll()['status'], 'running')
            fd = worker.call_args.kwargs['args'][-1]
            os.close(fd)
            self.preparer.active.clear()

    def test_explicit_changed_source_retry_settles_and_preserves_previous_output(self):
        directory=self.root/'jobs'/('a'*40);directory.mkdir()
        with (self.root/'Game.nsp').open('rb') as f: original=Forge.identity(self.root/'Game.nsp',f.fileno())
        previous=directory/'previous';previous.mkdir();(previous/'retained').write_bytes(b'old output')
        self.preparer.save(directory,dict(status='failed',source=[original],output=str(previous)))
        replacement=self.root/'replacement';replacement.write_bytes(b'changed');replacement.replace(self.root/'Game.nsp')
        with self.assertRaises(ValueError): self.poll()
        with patch('switch_prepare.time.monotonic',side_effect=[0,3]), patch('switch_prepare.threading.Thread') as worker:
            self.assertEqual(self.poll(retry=True)['status'],'waiting')
            self.assertEqual(self.poll()['status'],'running')
            self.assertNotEqual(worker.call_args.kwargs['args'][3]['output'],str(previous))
            os.close(worker.call_args.kwargs['args'][-1]);self.preparer.active.clear()
        self.assertEqual((previous/'retained').read_bytes(),b'old output')

    def test_changed_active_or_published_source_is_not_replaced(self):
        directory=self.root/'jobs'/('a'*40);directory.mkdir()
        for published in (False,True):
            with (self.root/'Game.nsp').open('rb') as f: original=Forge.identity(self.root/'Game.nsp',f.fileno())
            self.preparer.save(directory,dict(status='ready' if published else 'running',source=[original]))
            replacement=self.root/'replacement';replacement.write_bytes(b'changed');replacement.replace(self.root/'Game.nsp')
            if not published:self.preparer.active.add('a'*40)
            with self.assertRaises(ValueError):self.poll(retry=True)
            self.preparer.active.clear()

    def test_first_manual_preparation_cannot_skip_or_reset_settlement(self):
        with patch('switch_prepare.time.monotonic', side_effect=[0,1,3]), patch('switch_prepare.threading.Thread') as worker:
            self.assertEqual(self.poll(retry=True)['status'], 'waiting')
            self.assertEqual(self.poll(retry=True)['status'], 'waiting')
            worker.assert_not_called()
            self.assertEqual(self.poll()['status'], 'running')
            worker.assert_called_once()
            os.close(worker.call_args.kwargs['args'][-1]);self.preparer.active.clear()

    def test_explicit_retry_can_rebuild_invalid_ready_output(self):
        directory=self.root/'jobs'/('a'*40);directory.mkdir()
        with (self.root/'Game.nsp').open('rb') as f: source=Forge.identity(self.root/'Game.nsp',f.fileno())
        previous=directory/'previous';(previous/'content/roms').mkdir(parents=True)
        published=previous/'content/roms/Game.nsp';published.write_bytes(b'old output')
        with published.open('rb') as f: identity=Forge.identity(published,f.fileno())
        self.preparer.save(directory,dict(status='ready',source=[source],output=str(previous),files=[dict(name='roms/Game.nsp',identity=identity)]))
        published.write_bytes(b'broken')
        with self.assertRaises(ValueError):self.poll()
        with patch('switch_prepare.time.monotonic',side_effect=[0,3]),patch('switch_prepare.threading.Thread') as worker:
            self.assertEqual(self.poll(retry=True)['status'],'waiting')
            self.assertEqual(self.poll()['status'],'running')
            self.assertNotEqual(worker.call_args.kwargs['args'][3]['output'],str(previous))
            os.close(worker.call_args.kwargs['args'][-1]);self.preparer.active.clear()
        self.assertEqual(published.read_bytes(),b'broken')

    def test_more_than_64_sources_do_not_evict_each_others_observations(self):
        def poll(index):return self.preparer.poll(f'{index+1:040x}',self.root,self.members,'Fixture')
        with patch('switch_prepare.time.monotonic',return_value=0),patch('switch_prepare.threading.Thread') as worker:
            for index in range(128):self.assertEqual(poll(index)['status'],'waiting')
            worker.assert_not_called()
        with patch('switch_prepare.time.monotonic',return_value=3),patch('switch_prepare.threading.Thread') as worker:
            for index in range(64):
                self.assertEqual(poll(index)['status'],'running')
                os.close(worker.call_args.kwargs['args'][-1]);self.preparer.active.clear()
            for index in range(64,128):self.assertEqual(poll(index)['status'],'waiting')
        with patch('switch_prepare.time.monotonic',return_value=6),patch('switch_prepare.threading.Thread') as worker:
            self.assertEqual(poll(64)['status'],'running')
            os.close(worker.call_args.kwargs['args'][-1]);self.preparer.active.clear()

    def test_changed_existing_observation_updates_without_needing_a_free_slot(self):
        with (self.root/'Game.nsp').open('rb') as f: identity=Forge.identity(self.root/'Game.nsp',f.fileno())
        self.preparer.settling={f'{index+1:040x}':([identity],0) for index in range(63)}
        self.preparer.settling['a'*40]=([identity],0)
        replacement=self.root/'replacement';replacement.write_bytes(b'changed');replacement.replace(self.root/'Game.nsp')
        with patch('switch_prepare.time.monotonic',side_effect=[0,3]),patch('switch_prepare.threading.Thread') as worker:
            self.assertEqual(self.poll()['status'],'waiting')
            self.assertEqual(self.poll()['status'],'running')
            os.close(worker.call_args.kwargs['args'][-1]);self.preparer.active.clear()
