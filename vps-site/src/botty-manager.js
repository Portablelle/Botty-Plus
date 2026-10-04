import { sha256 } from './transmission.js';
import { sleep } from './ps5-io.js';
export const MANAGER_ROOT='/data/botty/manager';
const VERSION='1.5.1';
const APP=MANAGER_ROOT+'/'+VERSION;
const BASE='./apps/botty/';
const HASH='0cc2140fd8a36750b2ead9a0a5125593638dcdf9c4e6c413b41c9156db54f2de';
const encoder=new TextEncoder();
export async function managerInstalled(io) {
  const bytes=await io.readFile(MANAGER_ROOT+'/installed.json',4096);
  if(!bytes)return false;
  try {const data=JSON.parse(new TextDecoder().decode(bytes));return data.app==='Botty'&&['0.1.0','0.1.1','0.1.2','0.1.3','0.1.4','0.1.5','0.2.0','0.3.0','0.3.1','0.3.2','0.3.3','0.3.4','1.0.0','1.0.1','1.0.2','1.0.3','1.0.4','1.1.0','1.2.0','1.2.1','1.2.2','1.3.0','1.3.1','1.3.2','1.3.3','1.3.4','1.3.5','1.3.6','1.3.7','1.4.0','1.4.1','1.4.2','1.4.3','1.5.0',VERSION].includes(data.version);}
  catch(_){throw Error('Botty installation record is damaged. Reinstall Botty from its button.');}
}
async function health(io) {
  const response=await io.http(8088,'/health');
  if(response.status!==200)throw Error('Botty is not responding.');
  const data=JSON.parse(response.body);
  if(data.app!=='Botty'||!['0.1.0','0.1.1','0.1.2','0.1.3','0.1.4','0.1.5','0.2.0','0.3.0','0.3.1','0.3.2','0.3.3','0.3.4','1.0.0','1.0.1','1.0.2','1.0.3','1.0.4','1.1.0','1.2.0','1.2.1','1.2.2','1.3.0','1.3.1','1.3.2','1.3.3','1.3.4','1.3.5','1.3.6','1.3.7','1.4.0','1.4.1','1.4.2','1.4.3','1.5.0',VERSION].includes(data.version)||data.titleId!=='BTTY00001')throw Error('Port 8088 is used by an unexpected service.');
  return data;
}
async function verifyWorker(io) {
  const token=await io.readFile('/data/botty/compressor/token',128);
  const key=token?new TextDecoder().decode(token).trim():'';
  if(!/^[a-f0-9]{64}$/.test(key))throw Error('Compression worker credentials are unavailable.');
  const response=await io.http(5910,'/api/status?token='+key);
  const data=JSON.parse(response.body);
  if(response.status!==200||data.ok!==true||!['library-1.2','library-1.3'].includes(data.bottyWorker))throw Error('An incompatible compression worker is running. Keep current work intact and start a new console session.');
}
export async function installAndStartManager(io,options={}) {
  const fetchFile=options.fetchFile||fetch,digest=options.digest||sha256,wait=options.wait||sleep,report=options.report||(()=>{});
  const running = await io.listening(8088) ? await health(io) : null;
  if(running?.version===VERSION&&await io.listening(5910)){await verifyWorker(io);return running;}
  report('Verifying the Botty homebrew package…');
  const response=await fetchFile(BASE+'manifest.json',{cache:'no-store'});
  if(!response.ok)throw Error('Botty package manifest unavailable.');
  const bytes=new Uint8Array(await response.arrayBuffer());
  if(await digest(bytes)!==HASH)throw Error('Botty manifest verification failed.');
  const manifest=JSON.parse(new TextDecoder().decode(bytes));
  const allowed=['botty-manager.elf','icon0.png','ui/index.html','ui/app.js','ui/style.css','cacert.pem','game-compressor.elf'];
  if(manifest.schema!==1||manifest.id!==VERSION||manifest.files.length!==allowed.length)throw Error('Unexpected Botty package.');
  const staged=[];let executable,compressor;
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
    if(file.path==='game-compressor.elf')compressor=data;
  }
  if(!executable)throw Error('Botty executable missing.');
  report(staged.length ? 'Installing Botty service…' : 'Botty service files already installed.');
  await io.mkdirs(APP+'/ui');
  for(const {file,data} of staged) {
    await io.writeFile(APP+'/'+file.path,data);
    const disk=await io.readFile(APP+'/'+file.path,file.size);
    if(!disk||await digest(disk)!==file.sha256)throw Error('Botty installation verification failed.');
  }
  if(!running||running.version===VERSION){
  await io.mkdirs('/data/botty/compressor');
  let workerToken=await io.readFile('/data/botty/compressor/token',128);
  if(!workerToken){
    const value=Array.from(crypto.getRandomValues(new Uint8Array(32)),x=>x.toString(16).padStart(2,'0')).join('');
    workerToken=encoder.encode(value);await io.writeFile('/data/botty/compressor/token',workerToken);
  }
  if(!/^[a-f0-9]{64}\s*$/.test(new TextDecoder().decode(workerToken)))throw Error('Invalid compression worker credentials; existing data was preserved.');
  // Do not replace a running worker or restart active file operations.
  if(!running||running.version===VERSION){
    await io.writeFile('/data/botty/compressor/enabled.json',encoder.encode('{"mode":"library-1.2"}\n'));
    if(!await io.listening(5910)){
      if(!compressor)throw Error('Compression worker missing.');
      report('Starting Library compression service…');await io.sendElf(compressor);
      let ready=false;for(let i=0;i<40;i++){if(await io.listening(5910)){ready=true;break;}await wait(250);}
      if(!ready)throw Error('Compression worker did not start.');
    }
    await verifyWorker(io);
  }
  }
  if(await io.listening(8088)) {
    const active = await health(io);
    const updatePending = active.version !== VERSION;
    if(updatePending)report('Botty service ' + VERSION + ' is installed. Running work is preserved; it starts next console session.');
    return {...active, updatePending, availableVersion: VERSION};
  }
  report('Starting Botty service…');
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
