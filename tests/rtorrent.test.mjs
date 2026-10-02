import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { installAndStart, ROOT, STATE } from '../vps-site/src/rtorrent.js';

const encode = value => new TextEncoder().encode(JSON.stringify(value));
const decode = bytes => JSON.parse(new TextDecoder().decode(bytes));
const legacy = '/data/botty/transmission/state';
const manifest = JSON.parse(await readFile(new URL('../vps-site/apps/rtorrent/manifest.json', import.meta.url)));

function fixture({running=false, transmission=false, staleProcess=false, corrupt, noStartup=false, diskFailure=false}={}) {
  const files=new Map(), writes=[], events=[], downloads=[];
  const io={
    listening:async port=>port===9091?transmission:port===5001?running:false,
    processes:async()=>staleProcess?[{pid:200,name:'transmission-da'}]:[],
    readFile:async path=>files.get(path)||null,
    mkdirs:async path=>events.push(['mkdir',path]),
    writeFile:async(path,bytes,exclusive)=>{
      if(exclusive)assert.equal(files.has(path),false);
      if(diskFailure&&path.endsWith('.elf'))throw Error('Disk full');
      writes.push(path);files.set(path,bytes.slice());
    },
    sendElf:async bytes=>{assert.deepEqual([...bytes.slice(0,4)],[127,69,76,70]);events.push(['launch']);if(!noStartup)running=true;},
  };
  const options={wait:async()=>{},fetchFile:async url=>{
    downloads.push(url);
    const bytes=new Uint8Array(await readFile(new URL('../vps-site/'+url.slice(2),import.meta.url)));
    if(corrupt&&url.endsWith(corrupt))bytes[0]^=1;
    return {ok:true,arrayBuffer:async()=>bytes.buffer};
  }};
  return {io,options,files,writes,events,downloads};
}

test('verified rTorrent package starts from isolated storage with persisted credentials',async()=>{
  const f=fixture();
  const result=await installAndStart(f.io,f.options);
  assert.deepEqual(result,{engine:'rtorrent',version:manifest.id});
  const auth=decode(f.files.get(STATE+'/botty-credentials.json'));
  assert.equal(auth.username,'botty');assert.match(auth.password,/^[A-Za-z0-9]{6}$/);
  for(const file of manifest.files)assert.equal(f.files.get(ROOT+'/'+manifest.id+'/'+file.path).length,file.size);
  assert.deepEqual(f.files.get(STATE+'/rtorrent.rc'),f.files.get(ROOT+'/'+manifest.id+'/rtorrent.rc'));
  assert.equal(f.events.filter(([event])=>event==='launch').length,1);
  assert.ok(f.writes.every(path=>path.startsWith(ROOT+'/')));
});

for(const options of [{transmission:true},{staleProcess:true}])test('live Transmission blocks migration before writes: '+JSON.stringify(options),async()=>{
  const f=fixture(options);
  await assert.rejects(installAndStart(f.io,f.options),/Stop Transmission and migrate/);
  assert.deepEqual(f.events,[]);assert.deepEqual(f.writes,[]);assert.deepEqual(f.downloads,[]);
});

for(const status of [null,'pending'])test('incomplete migration preserves the legacy queue: '+status,async()=>{
  const f=fixture();f.files.set(legacy+'/settings.json',encode({test:true}));
  f.files.set(legacy+'/resume/test.resume',encode({piece:42}));
  if(status)f.files.set(STATE+'/migration.json',encode({status}));
  const before=new Map(f.files);
  await assert.rejects(installAndStart(f.io,f.options),/downloads need migration/);
  assert.deepEqual(f.files,before);assert.deepEqual(f.events,[]);assert.deepEqual(f.downloads,[]);
});

test('completed migration keeps credentials, downloads, and resume data',async()=>{
  const f=fixture();const credentials={username:'botty',password:'B7mQ2x'};
  f.files.set(legacy+'/settings.json',encode({test:true}));
  f.files.set(legacy+'/botty-credentials.json',encode(credentials));
  f.files.set(STATE+'/migration.json',encode({status:'complete'}));
  f.files.set(STATE+'/session/test.torrent',new Uint8Array([1,2,3]));
  f.files.set('/data/botty/downloads/complete/test.bin',new Uint8Array([4,5,6]));
  const before=new Map(f.files);
  await installAndStart(f.io,f.options);
  for(const [path,bytes] of before)assert.deepEqual(f.files.get(path),bytes);
  assert.deepEqual(decode(f.files.get(STATE+'/botty-credentials.json')),credentials);
});

test('running rTorrent is preserved without relaunching or replacing active configuration',async()=>{
  const f=fixture({running:true});
  f.files.set(STATE+'/botty-credentials.json',encode({username:'botty',password:'B7mQ2x'}));
  f.files.set(STATE+'/rtorrent.rc',new TextEncoder().encode('existing configuration'));
  f.files.set(STATE+'/session/test.torrent',new Uint8Array([1,2,3]));
  const before=new Map(f.files);
  await installAndStart(f.io,f.options);
  for(const [path,bytes] of before)assert.deepEqual(f.files.get(path),bytes);
  assert.ok(f.writes.every(path=>path.startsWith(ROOT+'/'+manifest.id+'/')));
  assert.ok(!f.events.some(([event])=>event==='launch'));
});

for(const corrupt of ['manifest.json','rtorrent.elf','rtorrent.rc','cacert.pem'])test('corrupt rTorrent artifact prevents all writes: '+corrupt,async()=>{
  const f=fixture({corrupt});
  await assert.rejects(installAndStart(f.io,f.options),/verification failed/);
  assert.deepEqual(f.writes,[]);assert.deepEqual(f.events,[]);
});

test('failed rTorrent installation never launches a partial package',async()=>{
  const f=fixture({diskFailure:true});
  await assert.rejects(installAndStart(f.io,f.options),/Disk full/);
  assert.ok(!f.events.some(([event])=>event==='launch'));
});

test('failed rTorrent startup is reported after bounded polling',async()=>{
  const f=fixture({noStartup:true});let waits=0;f.options.wait=async()=>{waits++;};
  await assert.rejects(installAndStart(f.io,f.options),/rTorrent did not start/);
  assert.equal(waits,80);assert.equal(f.events.filter(([event])=>event==='launch').length,1);
});
