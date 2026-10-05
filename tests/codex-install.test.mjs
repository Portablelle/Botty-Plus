import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createHash, webcrypto} from 'node:crypto';
import {installCodex, NATIVE, SERVICE, UPDATE, nativeIdentity} from '../vps-site/src/codex-install.js';
import {startCodex, codexStatus} from '../vps-site/src/codex.js';
globalThis.crypto ||= webcrypto;
const encode = x => new TextEncoder().encode(typeof x === 'string' ? x : JSON.stringify(x));
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const PIN = /const HASH = '([a-f0-9]{64})'/.exec(readFileSync(new URL('../vps-site/src/codex-install.js',import.meta.url),'utf8'))[1];
const names = ['assets/ggml-base.bin','assets/ui-font.bin','eboot.bin','sce_module/libc.prx','sce_sys/icon0.png','sce_sys/param.json'];
const param = version => encode({titleId:'PPSA99105',contentId:'UP9000-PPSA99105_00-CODEXPS500000001',contentVersion:version,localizedParameters:{'en-US':{titleName:'Codex PS5 - Prototype'}}});
function fixture(installed = true) {
  const chunks = new Map(), disk = new Map(), dirs = new Set(), events = [];
  const native = names.map(path => {
    const data = path.endsWith('param.json') ? param('00.000.002') : encode('new-'+path);
    const sha = hash(data);chunks.set(sha,data);
    return {path,size:data.length,sha256:sha,chunks:[sha]};
  });
  const data=encode('new-service'),sha=hash(data);chunks.set(sha,data);
  const m={schema:1,titleId:'PPSA99105',version:'0.0.2',serviceBuild:'b'.repeat(64),chunkSize:1048576,native,service:[{path:'assistant-service.elf',size:data.length,sha256:sha,chunks:[sha]}]};
  const manifest=encode(m), digest=async bytes => hash(bytes) === hash(manifest) ? PIN : hash(bytes);
  const put=(path,data)=>{disk.set(path,data);let d=path.slice(0,path.lastIndexOf('/'));while(d){dirs.add(d);d=d.slice(0,d.lastIndexOf('/'));}};
  if(installed){put(NATIVE+'/sce_sys/param.json',param('00.001.000'));put(SERVICE+'/assistant-service.elf',encode('old-service'));}
  put('/data/codex-ps5/home/.codex/auth.json',encode('keep-auth'));put('/data/codex-ps5/workspace/file',encode('keep-workspace'));
  const io={
    readFile:async p=>disk.get(p)||null, writeFile:async(p,b)=>{events.push(['write',p]);put(p,b);},
    mkdirs:async p=>dirs.add(p),ensureHomebrew:async()=>dirs.add('/data/homebrew'),syncDirectory:async()=>{},
    assertNativeStopped:async()=>{}, directoryExists:async p=>dirs.has(p),
    removeEmptyDirectory:async p=>{if([...disk.keys()].some(k=>k.startsWith(p+'/')))return false;dirs.delete(p);return true;},
    matchesFile:async(p,f)=>!!disk.get(p)&&hash(disk.get(p))===f.sha256,
    fingerprint:async p=>disk.has(p)?hash(disk.get(p)):null,
    seedBlocks:async(p,f)=>{const b=disk.get(p);if(b&&hash(b)===f.chunks[0])put(UPDATE+'/blocks/'+f.chunks[0]+'.bin',b);},
    assembleFile:async(p,f,get)=>put(p,await get(f.chunks[0],f.size)),preparePermissions:async()=>{},syncCodexMetadata:async()=>{},
    moveDirectory:async(from,to)=>{events.push(['move',from,to]);assert.equal(dirs.has(to),false);for(const [p,b] of [...disk])if(p.startsWith(from+'/')){disk.delete(p);put(to+p.slice(from.length),b);}for(const d of [...dirs])if(d===from||d.startsWith(from+'/'))dirs.delete(d);},
  };
  const fetchFile=async url=>{events.push(['fetch',url]);const b=url.endsWith('manifest.json')?manifest:chunks.get(url.split('/').at(-1).slice(0,-4));return {ok:!!b,arrayBuffer:async()=>b};};
  return {io,disk,dirs,put,m,events,options:{digest,fetchFile}};
}
test('fresh installation publishes native/model/service and preserves auth and workspace',async()=>{
  const f=fixture(false);const r=await installCodex(f.io,f.options);assert.equal(r.updated,true);
  assert.ok(f.disk.has(NATIVE+'/assets/ggml-base.bin'));assert.ok(f.disk.has(SERVICE+'/assistant-service.elf'));
  assert.equal(new TextDecoder().decode(f.disk.get('/data/codex-ps5/home/.codex/auth.json')),'keep-auth');
  assert.equal(new TextDecoder().decode(f.disk.get('/data/codex-ps5/workspace/file')),'keep-workspace');
});
test('prototype updates with backup, unchanged model reused; second launch downloads only manifest',async()=>{
  const f=fixture();const model=f.m.native[0];f.put(NATIVE+'/'+model.path,encode('new-'+model.path));
  await installCodex(f.io,f.options);
  assert.ok(f.events.some(e=>e[0]==='move'&&e[1]===NATIVE));
  assert.equal(f.events.some(e=>e[0]==='fetch'&&e[1].endsWith(model.chunks[0]+'.bin')),false);
  f.events.length=0;assert.equal((await installCodex(f.io,f.options)).updated,false);
  assert.deepEqual(f.events,[['fetch','./apps/codex/manifest.json']]);
});
test('corrupt downloaded block never publishes or moves live title',async()=>{
  const f=fixture();const fetch=f.options.fetchFile;f.options.fetchFile=async url=>url.endsWith('manifest.json')?fetch(url):{ok:true,arrayBuffer:async()=>encode('corrupt')};
  await assert.rejects(installCodex(f.io,f.options),/verification/);assert.equal(f.events.some(e=>e[0]==='move'),false);
});
test('interrupted publication resumes using journal and keeps original backup',async()=>{
  const f=fixture();const move=f.io.moveDirectory;let interrupted=false;
  f.io.moveDirectory=async(from,to)=>{if(!interrupted&&to===NATIVE){interrupted=true;throw Error('power loss');}return move(from,to);};
  await assert.rejects(installCodex(f.io,f.options),/power loss/);assert.equal(f.disk.has(NATIVE+'/sce_sys/param.json'),false);
  f.io.moveDirectory=move;await installCodex(f.io,f.options);
  assert.ok(f.disk.has(NATIVE+'/eboot.bin'));assert.ok([...f.disk.keys()].some(p=>p.includes('/backups/')&&p.endsWith('/native/sce_sys/param.json')));
  assert.equal(JSON.parse(new TextDecoder().decode(f.disk.get(UPDATE+'/journal.json'))).status,'complete');
});
test('foreign title and open native app block publication',async()=>{
  const f=fixture();f.put(NATIVE+'/sce_sys/param.json',encode({titleId:'OTHER'}));await assert.rejects(installCodex(f.io,f.options),/Unrecognized/);
  const g=fixture();g.io.assertNativeStopped=async()=>{throw Error('Close native apps');};await assert.rejects(installCodex(g.io,g.options),/Close/);
  assert.equal(g.events.some(e=>e[0]==='move'),false);assert.throws(()=>nativeIdentity(encode({})),/Unrecognized/);
});
function serviceFixture(legacy=false) {
  let running=true;const events=[];
  const status={service:'codex-ps5',build:'a'.repeat(64),pid:123,idle:true};
  const io={listening:async port=>running&&(port===49322||!legacy),assertNativeStopped:async()=>{},visitPayload:async()=>events.push('verify'),deliverPayload:async()=>{events.push('send');running=true;},
    http:async(port,path)=>{if(port===8088)return {status:200,body:JSON.stringify(path.endsWith('bootstrap')?{token:'a'.repeat(32)}:{tasks:[]})};if(legacy)throw Error('no control');if(path==='/stop'){events.push('stop');running=false;}return {status:200,body:JSON.stringify(status)};}};
  return {io,events,options:{install:async()=>{events.push('install');return {version:'0.0.2',serviceBuild:'b'.repeat(64)};},wait:async()=>{}}};
}
test('already running old service still checks updates and stops cleanly before one delivery',async()=>{
  const f=serviceFixture();assert.equal((await startCodex(f.io,f.options)).ready,true);assert.deepEqual(f.events,['install','stop','verify','send']);
});
test('legacy engine installs updates but requests a full restart without killing or resending',async()=>{
  const f=serviceFixture(true);const r=await startCodex(f.io,f.options);assert.equal(r.updatePending,true);assert.match(codexStatus(r),/Fully restart/);assert.deepEqual(f.events,['install']);
});
