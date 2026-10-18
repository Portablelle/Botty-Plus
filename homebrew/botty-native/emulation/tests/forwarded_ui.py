#!/usr/bin/env python3
"""Run direct launches through the actual built-in save-sync UI lifecycle."""
import os
from pathlib import Path
import subprocess
import sys
binary, assets, output = map(Path,sys.argv[1:])
output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,EGL_PLATFORM='surfaceless',LIBGL_ALWAYS_SOFTWARE='1',GALLIUM_DRIVER='llvmpipe',MESA_GL_VERSION_OVERRIDE='4.6',MESA_GLSL_VERSION_OVERRIDE='460')
for mode in ('botty-play','botty-conflict','botty-return','botty-cancel','botty-failed','botty-return-failed'):
 with (output/(mode+'.log')).open('w') as log:
  p=subprocess.Popen([str(binary),str(assets),str(output),'960','540'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True,env=env)
  assert p.stdout.readline().startswith('READY ')
  def command(text):
   p.stdin.write(text+'\n');p.stdin.flush();lines=[]
   while True:
    line=p.stdout.readline();assert line,mode+' renderer stopped'
    lines.append(line)
    if line.startswith('OK '):return ''.join(lines)
  command(mode)
  if mode=='botty-conflict':
   assert 'done=0' in command('status')
   assert 'BOTTYPLAYS 0' in command('botty-plays')
   command('key cross');command('tick 180')
  if mode in ('botty-cancel','botty-failed'):
   assert 'BOTTYPLAYS 0' in command('botty-plays')
   command('key circle')
  if mode=='botty-return-failed':assert 'done=0' in command('status'), 'Backup failure notice must be visible before returning'
  status=command('status')
  for _ in range(8):
   if 'done=1' in status:break
   command('tick 120');status=command('status')
  assert 'done=1' in status,status
  assert ('BOTTYPLAYS 0' if mode in ('botty-return','botty-cancel','botty-failed','botty-return-failed') else 'BOTTYPLAYS 1') in command('botty-plays')
  if mode in ('botty-return','botty-cancel','botty-failed','botty-return-failed'):assert 'game= ' in status,status
  p.stdin.write('exit\n');p.stdin.flush();assert p.wait(timeout=15)==0
print('Forwarded save synchronization, conflict-before-launch, will_play and post-game return PASS')
