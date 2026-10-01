import { sha256 } from './transmission.js';
import { sleep } from './ps5-io.js';
export const MANAGER_ROOT='/data/botty/manager';
const VERSION='0.1.5';
const APP=MANAGER_ROOT+'/'+VERSION;
const BASE='./apps/botty/';
const HASH='8f82b8a8b45f209c0633e56e44720b0e22629edda546476d8691e34c7295026e';
const encoder=new TextEncoder();
export async function managerInstalled(io) {
  const bytes=await io.readFile(MANAGER_ROOT+'/installed.json',4096);
  if(!bytes)return false;
  try {const data=JSON.parse(new TextDecoder().decode(bytes));return data.app==='Botty'&&['0.1.0','0.1.1','0.1.2','0.1.3','0.1.4',VERSION].includes(data.version);}
  catch(_){throw Error('Botty installation record is damaged. Reinstall Botty from its button.');}
}
async function health(io) {
  const response=await io.http(8088,'/health');
  if(response.status!==200)throw Error('Botty is not responding.');
  const data=JSON.parse(response.body);
  if(data.app!=='Botty'||!['0.1.0','0.1.1','0.1.2','0.1.3','0.1.4',VERSION].includes(data.version)||data.titleId!=='BTTY00001')throw Error('Port 8088 is used by an unexpected service.');
  return data;
}
export async function installAndStartManager(io,options={}) {
  const fetchFile=options.fetchFile||fetch,digest=options.digest||sha256,wait=options.wait||sleep,report=options.report||(()=>{});
  if(await io.listening(8088))return await health(io);
  report('Verifying the Botty homebrew package…');
  const response=await fetchFile(BASE+'manifest.json',{cache:'no-store'});
  if(!response.ok)throw Error('Botty package manifest unavailable.');
  const bytes=new Uint8Array(await response.arrayBuffer());
  if(await digest(bytes)!==HASH)throw Error('Botty manifest verification failed.');
  const manifest=JSON.parse(new TextDecoder().decode(bytes));
  const allowed=['botty-manager.elf','icon0.png','ui/index.html','ui/app.js','ui/style.css'];
  if(manifest.schema!==1||manifest.id!==VERSION||manifest.files.length!==allowed.length)throw Error('Unexpected Botty package.');
  const staged=[];let executable;
  for(const file of manifest.files) {
    if(!allowed.includes(file.path))throw Error('Invalid Botty package path.');
    let data=await io.readFile(APP+'/'+file.path,16*1024*1024);
    if(!data||data.length!==file.size||await digest(data)!==file.sha256) {
      const result=await fetchFile(BASE+file.path,{cache:'no-store'});
      if(!result.ok)throw Error('Botty file download failed: '+file.path);
      data=new Uint8Array(await result.arrayBuffer());
      if(data.length!==file.size||await digest(data)!==file.sha256)throw Error('Botty file verification failed: '+file.path);
      staged.push({file,data});
    }
    if(file.path==='botty-manager.elf')executable=data;
  }
  if(!executable)throw Error('Botty executable missing.');
  report('Installing Botty and its controller interface…');
  await io.mkdirs(APP+'/ui');
  for(const {file,data} of staged) {
    await io.writeFile(APP+'/'+file.path,data);
    const disk=await io.readFile(APP+'/'+file.path,file.size);
    if(!disk||await digest(disk)!==file.sha256)throw Error('Botty installation verification failed.');
  }
  report('Starting Botty and registering its home screen icon…');
  if(await io.listening(8088))return await health(io);
  await io.sendElf(executable);
  let result;
  for(let attempt=0;attempt<80;attempt++) {
    if(await io.listening(8088)) {result=await health(io);break;}
    await wait(250);
  }
  if(!result) {
    const log=await io.readFile(MANAGER_ROOT+'/startup.log',65536);
    const detail=log?new TextDecoder().decode(log).trim().slice(-1500):'No startup log: the executable may have failed before initialization.';
    throw Error('Botty did not start. '+detail);
  }
  if(result.version!==VERSION)throw Error('The previous Botty service is still running. Start a new session to finish the update.');
  await io.writeFile(MANAGER_ROOT+'/installed.json',encoder.encode(JSON.stringify({app:'Botty',version:VERSION})+'\n'));
  return result;
}
