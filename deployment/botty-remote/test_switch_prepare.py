"""Exercise the actual server worker with the compiled shared verifier."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import zipfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('bridge_switch_tests', HERE/'server.py')
bridge = importlib.util.module_from_spec(spec); spec.loader.exec_module(bridge)
BOTTY = HERE.parents[1]/'homebrew/botty'
if not (BOTTY/'Makefile').exists(): BOTTY = HERE.parents[1]
sys.path.insert(0, str(BOTTY/'tests'))
from switch_fixtures import package

with tempfile.TemporaryDirectory(prefix='botty-switch-server-') as tmp:
    root = Path(tmp).resolve(); source=root/'source'; source.mkdir()
    archive=source/'Fixture.NSW.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('Game.nsp', package())
        z.writestr('Update.nsp', package('update', 65536))
        z.writestr('Content.nsp', package('dlc'))
    b=bridge.Bridge.__new__(bridge.Bridge)
    b.owned={'a'*40: dict(root=str(source), platform='switch', automatic=True)}
    b.save=lambda: None
    b.switch=bridge.SwitchPreparer(str(BOTTY/'build/botty-switch-prepare'), '', root/'prepared', bridge.Forge)
    b.forge=None
    info=dict(name='Fixture.NSW', save_path=str(source), state='uploading', dlspeed=0, upspeed=0, num_seeds=0, num_leechs=0)
    files=[dict(index=0, name=archive.name, size=archive.stat().st_size, progress=1)]
    b.info=lambda h:(info,files,True)
    result=b.snapshot('a'*40)
    assert not result['complete']
    for _ in range(100):
        result=b.snapshot('a'*40)
        if result['preparation']['status'] in ('ready','failed'): break
        time.sleep(.05)
    assert result['complete'],result
    assert len(result['files'])==6
    assert archive.exists()
    for member in result['files']:
        folder, actual=b.file_member('a'*40, str(member['index']), member['name'], member['size'])
        fd=bridge.open_member(folder, actual['name']); os.close(fd)
    assert all(not f['name'].endswith('.zip') for f in result['files'])
    changed=result['files'][0]
    path=Path(b.owned['a'*40]['preparedSwitch']['output'])/'content'/changed['name']
    with path.open('ab') as f:f.write(b'changed')
    try:b.file_member('a'*40,str(changed['index']),changed['name'],changed['size'])
    except ValueError:pass
    else:raise AssertionError('Changed prepared file served')
    print('Dedicated ZIP extraction, CNMT roles, verified-only transfer and mutation refusal PASS')
