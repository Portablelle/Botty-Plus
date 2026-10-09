from pathlib import Path
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest


class RtorrentPackageRevisionTests(unittest.TestCase):
    def test_matching_binary_can_publish_while_next_native_release_is_prepared(self):
        project = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            (root / 'scripts').mkdir()
            for name in ['package-rtorrent.py', 'botty-packages.py']:
                shutil.copyfile(project / 'scripts' / name, root / 'scripts' / name)
            native_source = root / 'homebrew/botty-native'
            for name in ['tools', 'src', 'sce_sys', 'assets']:
                (native_source / name).mkdir(parents=True)
            for name in ['release.json', 'tools/release_version.py', 'src/version.hpp', 'sce_sys/param.json', 'assets/build.txt']:
                shutil.copyfile(project / 'homebrew/botty-native' / name, native_source / name)
            public_version = json.loads((native_source / 'release.json').read_text())['version']
            native_version = json.loads((native_source / 'sce_sys/param.json').read_text())['contentVersion']
            # Independent component indexing uses the shipped native identity;
            # release/CI checks require the next native build before publication.
            (native_source / 'release.json').write_text(json.dumps(dict(schema=1, version='1.7.0')))
            spec = importlib.util.spec_from_file_location('fixture_packages', project / 'scripts/botty-packages.py')
            packages = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(packages)
            for package in ['botty', 'botty-native']:
                base = root / 'packages' / package
                base.mkdir(parents=True)
                files = []
                for name in packages.REQUIRED_FILES[package]:
                    path = base / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    data = b'synthetic package input'
                    if name == 'sce_sys/param.json': data = json.dumps(dict(contentVersion=native_version)).encode()
                    path.write_bytes(data)
                    files.append({'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
                for name in packages.PACKAGES[package]:
                    (base / name).write_bytes(b'synthetic notice/source')
                manifest = {'schema': 1, 'files': files}
                manifest['version' if package == 'botty-native' else 'id'] = native_version if package == 'botty-native' else '1.5.4'
                if package == 'botty-native':
                    manifest['releaseVersion'] = public_version
                    manifest['requires'] = {'manager': '1.5.4', 'worker': '1.3.1', 'rtorrent': '0.16.24-botty5', 'apiVersion': 1}
                else:
                    manifest.update(apiVersion=1, workerApi='library-1.3', workerVersion='1.3.1', updaterVersion='1.0.0')
                (base / 'manifest.json').write_text(json.dumps(manifest))
            source = root / 'homebrew/rtorrent'
            (source / 'build/downloads').mkdir(parents=True)
            (source / 'tests').mkdir()
            (source / 'runtime-version.hpp').write_text('#define BOTTY_RT_RUNTIME_VERSION "0.16.24-botty6"\n')
            (source / 'build/rtorrent.elf').write_bytes(b'\x7fELF synthetic "version":"0.16.24-botty6","boot":')
            for name in ['README.md', 'LICENSE', 'rtorrent.rc']:
                (source / name).write_text('synthetic source')
            for name in ['rtorrent-0.16.24.tar.gz', 'libtorrent-0.16.24.tar.gz']:
                (source / 'build/downloads' / name).write_bytes(b'synthetic upstream fixture')
            (root / 'homebrew/botty').mkdir()
            (root / 'homebrew/botty/cacert.pem').write_text('synthetic CA fixture')
            result = subprocess.run([sys.executable, str(root / 'scripts/package-rtorrent.py')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = root / 'packages/rtorrent/manifest.json'
            self.assertTrue(manifest.exists())
            self.assertEqual(json.loads(manifest.read_text())['id'], '0.16.24-botty6')
            self.assertEqual(json.loads((root / 'packages/botty-release.json').read_text())['version'], public_version)

    def test_stale_binary_is_rejected_before_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'scripts').mkdir()
            source = root / 'homebrew/rtorrent'
            (source / 'build').mkdir(parents=True)
            (source / 'runtime-version.hpp').write_text('#define BOTTY_RT_RUNTIME_VERSION "0.16.24-botty6"\n')
            (source / 'build/rtorrent.elf').write_bytes(b'\x7fELF stale "version":"0.16.24-botty5","boot":')
            script = root / 'scripts/package-rtorrent.py'
            shutil.copyfile(Path(__file__).resolve().parents[1] / 'scripts/package-rtorrent.py', script)
            result = subprocess.run([sys.executable, str(script)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('does not contain the compiled runtime revision', result.stderr)
            self.assertFalse((root / 'packages/rtorrent/manifest.json').exists())


if __name__ == '__main__':
    unittest.main()
