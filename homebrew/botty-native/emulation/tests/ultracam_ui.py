#!/usr/bin/env python3
"""Exercise UltraCam's per-game controls in the actual embedded renderer."""
import json,os,subprocess,sys
from pathlib import Path
modern="--190" in sys.argv
binary,assets,output=map(Path,[a for a in sys.argv[1:] if a!="--190"]);output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',GALLIUM_DRIVER='llvmpipe',MESA_GL_VERSION_OVERRIDE='4.6',MESA_GLSL_VERSION_OVERRIDE='460')
if modern:env["BOTTY_PREVIEW_BOTW190"]="1"
with (output/'renderer.log').open('w') as err:
 p=subprocess.Popen([str(binary),str(assets),str(output),'1280','720'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,env=env)
 assert p.stdout.readline().startswith('READY ')
 def command(text):
  p.stdin.write(text+'\n');p.stdin.flush();lines=[]
  while True:
   line=p.stdout.readline();assert line,'renderer stopped: '+text;lines.append(line)
   if line.startswith('OK '):assert 'GL=0' in line;return ''.join(lines)
 def prefs(game=False):
  text=command('gameprefs' if game else 'prefs');prefix='GAMEPREFERENCES ' if game else 'PREFERENCES '
  return json.loads(next(line[len(prefix):] for line in text.splitlines() if line.startswith(prefix)))
 general=prefs();command('config-first')
 for _ in range(7):command('key down')
 command('key cross');command('key cross');assert prefs(True)['ultracam'][0]==1
 command('key down');command('key right');assert prefs(True)['ultracam'][1]==3
 command('key left');assert prefs(True)['ultracam'][1]==2
 command('key down');command('key left');command('key left');assert prefs(True)['ultracam'][2]==(2 if modern else 0)
 if modern:
  assert prefs(True)['ultracam_190']
  command('key down');command('key right');assert prefs(True)['ultracam'][3]==1
  command('key down');command('key left');assert prefs(True)['ultracam'][4]==2
 if not modern:
  for _ in range(3):command('key right')
  assert prefs(True)['ultracam'][2]==3
 assert prefs()==general,'UltraCam must not change global video preferences'
 command('tick 120');command('capture '+str(output/'ultracam-config.png'))
 command('key circle');command('key circle');assert 'done=1 game= ' in command('status')
 p.stdin.write('exit\n');p.stdin.flush();assert p.wait(timeout=15)==0
print('UltraCam real UI: per-game enable, 60 FPS, 720p/1152p, unchanged global preferences and return PASS' if not modern else 'BOTW 1.9 real UI: supported controls, locked native resolution/quality and unchanged global preferences PASS')
