#!/usr/bin/env python3
"""Exercise the actual embedded settings UI through its Linux renderer."""
import json, os
from pathlib import Path
import subprocess, sys
binary, assets, output = map(Path,sys.argv[1:])
output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',GALLIUM_DRIVER='llvmpipe',MESA_GL_VERSION_OVERRIDE='4.6',MESA_GLSL_VERSION_OVERRIDE='460')
def run(game=False):
 with (output/('game.log' if game else 'global.log')).open('w') as err:
  p=subprocess.Popen([str(binary),str(assets),str(output),'960','540'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,env=env)
  assert p.stdout.readline().startswith('READY ')
  def command(text):
   p.stdin.write(text+'\n');p.stdin.flush();lines=[]
   while True:
    s=p.stdout.readline();assert s,'renderer stopped: '+text
    lines.append(s)
    if s.startswith('OK '):
     assert 'GL=0' in s,s
     return ''.join(lines)
  def prefs(game=False):
   s=command('gameprefs' if game else 'prefs');prefix='GAMEPREFERENCES ' if game else 'PREFERENCES '
   return json.loads(next(l[len(prefix):] for l in s.splitlines() if l.startswith(prefix)))
  command('config-first' if game else 'config')
  assert 'done=0' in command('status')
  if game:
   command('capture '+str(output/'game.png'))
   command('key down');command('key cross');command('key down');command('key right')
   assert prefs(True)['resolution']==0, prefs(True)
   command('key circle')
  else:
   command('capture '+str(output/'categories.png'))
   command('key right');command('key cross')
   command('key down');command('key right');command('key right')
   assert prefs()['output']==2
   command('key down');command('key right')
   assert prefs()['resolution']==3 and prefs()['output']==2
   command('capture '+str(output/'video.png'))
   command('key circle');command('key right');command('key cross')
   command('key right');assert prefs()['block_list']
   command('key circle')
  command('key circle')
  assert 'done=1 game= ' in command('status')
  p.stdin.write('exit\n');p.stdin.flush();assert p.wait(timeout=15)==0
run();run(True)
print('Real settings renderer: global 2160p, independent render scale, performance switch, per-game override, Circle return PASS')
