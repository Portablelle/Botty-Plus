"""Overlapping core patches must survive refreshes; no compiler/dependencies needed."""
import io
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

engine = Path(sys.argv[1]).resolve()
prepare = (engine / 'tools/prepare-build.sh').read_text().split('# Optional: seed Eden')[0]
with tempfile.TemporaryDirectory(prefix='botty-patch-series-') as temporary:
    root = Path(temporary)
    (root / 'tools/eden-patches').mkdir(parents=True)
    (root / '.deps').mkdir()
    (root / 'tools/prepare-build.sh').write_text(prepare)
    with tarfile.open(root / '.deps/eden-67bada77.tar.gz', 'w:gz') as archive:
        for name, data in [('CMakeLists.txt', b'# fixture\n'), ('shared.txt', b'first\nmiddle\nlast\n')]:
            info = tarfile.TarInfo('pinned/' + name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    first = root / 'tools/eden-patches/01-first.diff'
    later = root / 'tools/eden-patches/02-later.diff'
    first.write_text('--- a/shared.txt\n+++ b/shared.txt\n@@ -1,3 +1,3 @@\n-first\n+alpha\n middle\n last\n')
    later.write_text('--- a/shared.txt\n+++ b/shared.txt\n@@ -1,3 +1,3 @@\n alpha\n middle\n-last\n+omega\n')

    def run(success=True):
        result = subprocess.run(['bash', str(root / 'tools/prepare-build.sh')],
                                env={'PATH': '/usr/bin:/bin', 'HOME': str(root), 'XDG_CACHE_HOME': str(root / 'cache')},
                                capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        return Path((root / '.local/headless-cache').read_text().strip()) / 'source'

    source = run()
    assert (source / 'shared.txt').read_text() == 'alpha\nmiddle\nomega\n'
    inode = (source / 'shared.txt').stat().st_ino
    run()
    assert (source / 'shared.txt').stat().st_ino == inode  # unchanged series is not reapplied
    (source / '.cache').mkdir()
    (source / '.cache/retained').write_text('download cache')
    later.write_text(later.read_text().replace('+omega', '+omega2'))
    run()
    assert (source / 'shared.txt').read_text() == 'alpha\nmiddle\nomega2\n'
    assert (source / '.cache/retained').read_text() == 'download cache'
    valid = later.read_text()
    later.write_text(valid.replace(' alpha\n', ' missing-anchor\n'))
    run(False)
    assert not (source / '.botty-patch-series').exists()
    later.write_text(valid)
    run()
    assert (source / 'shared.txt').read_text() == 'alpha\nmiddle\nomega2\n'
print('PASS: ordered overlapping patches, idempotence, cache retention and failed-series recovery')
