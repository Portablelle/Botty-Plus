import { sha256, shortPassword } from './transmission.js';
import { sleep, MAX_ELF_BYTES } from './ps5-io.js';
export const ROOT='/data/botty/rtorrent';
export const STATE=ROOT+'/state';
const VERSION='0.16.24-botty3';
const BASE='./apps/rtorrent/';
const HASH='c1aec1e97443217e8a104c201078d37e7371a97e9848d49c99b559a7d83ab3f4';
const enc=new TextEncoder(),dec=new TextDecoder();
export async function installAndStart(io,options={}) {
  const report=options.report||(()=>{}),wait=options.wait||sleep,fetchFile=options.fetchFile||fetch,digest=options.digest||sha256;
  if(await io.listening(9091)||(await io.processes()).some(p=>/^transmission/.test(p.name)))throw Error('Stop Transmission and migrate its downloads before starting rTorrent.');
  const old=await io.readFile('/data/botty/transmission/state/settings.json',1024*1024);
  if(old){
    const saved=await io.readFile(STATE+'/migration.json',1024*1024);
    if(!saved||JSON.parse(dec.decode(saved)).status!=='complete')throw Error('Existing Transmission downloads need migration before rTorrent can start. Files were preserved.');
  }
  const response=await fetchFile(BASE+'manifest.json',{cache:'no-store'});
  if(!response.ok)throw Error('rTorrent manifest unavailable.');
  const bytes=new Uint8Array(await response.arrayBuffer());
  if(await digest(bytes)!==HASH)throw Error('rTorrent manifest verification failed.');
  const manifest=JSON.parse(dec.decode(bytes));
  const allowed=['rtorrent.elf','rtorrent.rc','cacert.pem'];
  if(manifest.schema!==1||manifest.id!==VERSION||manifest.files.length!==3||new Set(manifest.files.map(f=>f.path)).size!==3)throw Error('Unexpected rTorrent package.');
  const staged=[];
  for(const file of manifest.files){
    if(!allowed.includes(file.path)||!Number.isInteger(file.size)||file.size<1||file.size>MAX_ELF_BYTES||!/^[a-f0-9]{64}$/.test(file.sha256))throw Error('Invalid rTorrent package entry.');
    const result=await fetchFile(BASE+file.path,{cache:'no-store'});if(!result.ok)throw Error('rTorrent download failed.');
    const data=new Uint8Array(await result.arrayBuffer());if(data.length!==file.size||await digest(data)!==file.sha256)throw Error('rTorrent file verification failed.');staged.push({file,data});
  }
  for(const directory of [ROOT+'/'+VERSION,STATE+'/session',STATE+'/incoming','/data/botty/downloads/complete'])await io.mkdirs(directory);
  for(const {file,data} of staged){const path=ROOT+'/'+VERSION+'/'+file.path;await io.writeFile(path,data);const disk=await io.readFile(path,file.size);if(!disk||await digest(disk)!==file.sha256)throw Error('rTorrent installation verification failed.');}
  let credentials=await io.readFile(STATE+'/botty-credentials.json',4096);
  if(!credentials){credentials=await io.readFile('/data/botty/transmission/state/botty-credentials.json',4096);if(!credentials)credentials=enc.encode(JSON.stringify({username:'botty',password:shortPassword()}));await io.writeFile(STATE+'/botty-credentials.json',credentials,true);}
  const auth=JSON.parse(dec.decode(credentials));if(auth.username!=='botty'||!/^([A-Za-z0-9]{6}|[a-f0-9]{32})$/.test(auth.password))throw Error('Invalid saved Botty credentials.');
  if(await io.listening(5001))return {engine:'rtorrent',version:VERSION};
  for(const {file,data} of staged)if(file.path!=='rtorrent.elf')await io.writeFile(STATE+'/'+file.path,data);
  report('Starting rTorrent…');await io.sendElf(staged.find(item=>item.file.path==='rtorrent.elf').data);
  for(let i=0;i<80;i++){if(await io.listening(5001))return {engine:'rtorrent',version:VERSION};await wait(250);}
  throw Error('rTorrent did not start. Its private runtime log contains details.');
}
