import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from assemble import validate_sources


class CorrespondingSources(unittest.TestCase):
    def test_archive_must_describe_the_same_built_sources(self):
        receipt = dict(commit='revision', upstream='repository', patchSha256='patch',
                       sourceFiles={'main.cpp': 'source hash'}, titleId='PPSA99071', dataRoot='/data/botty/emulation/switch')
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / 'sources.tar.gz'
            for variant in ('matching', 'old revision', 'old patch', 'different sources', 'missing receipt'):
                with self.subTest(variant=variant):
                    source = dict(receipt)
                    if variant == 'old revision':
                        source['commit'] = 'old'
                    if variant == 'old patch':
                        source['patchSha256'] = 'old'
                    if variant == 'different sources':
                        source['sourceFiles'] = {'main.cpp': 'other hash'}
                    with tarfile.open(archive, 'w:gz') as output:
                        if variant != 'missing receipt':
                            data = json.dumps(source).encode()
                            info = tarfile.TarInfo('prosperoeden/BOTTY-ENGINE.json')
                            info.size = len(data)
                            output.addfile(info, io.BytesIO(data))
                    if variant == 'matching':
                        validate_sources(archive, receipt)
                    else:
                        with self.assertRaisesRegex(ValueError, 'does not match'):
                            validate_sources(archive, receipt)
