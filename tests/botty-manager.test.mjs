import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { installAndStartManager, managerInstalled, MANAGER_ROOT } from '../vps-site/src/botty-manager.js';
function fixture({corrupt, occupied=false, noStartup=false, existingBotty=false, version='1.3.5'}={}) {
 const files=new Map(),writes=[],events=[];let running=occupied||existingBotty,worker=existingBotty;
 if(existingBotty)files.set('/data/botty/compressor/token',new TextEncoder().encode('a'.repeat(64)));
 const io={readFile:async path=>files.get(path)||null,mkdirs:async()=>{},writeFile:async(path,data)=>{files.set(path,data);writes.push(path);},listening:async port=>port===5910?worker:running,
  sendElf:async bytes=>{assert.equal(bytes[0],127);events.push('sent');if(!worker)worker=true;else running=!noStartup;},
  http:async port=>port===5910?({status:200,body:JSON.stringify({ok:true,bottyWorker:'library-1.2'})}):({status:200,body:JSON.stringify({app:occupied?'Other':'Botty',version,titleId:'BTTY00001'})})};
 const options={wait:async()=>{},fetchFile:async url=>{const bytes=new Uint8Array(await readFile(new URL('../vps-site/'+url.slice(2),import.meta.url)));if(corrupt&&url.endsWith(corrupt))bytes[0]^=1;return {ok:true,arrayBuffer:async()=>bytes.buffer};}};
 return {io,options,files,writes,events};
}
test('verified manager installation records opt-in only after native service responds',async()=>{
 const f=fixture();assert.equal(await managerInstalled(f.io),false);await installAndStartManager(f.io,f.options);
 assert.equal(await managerInstalled(f.io),true);assert.equal(f.writes.length,10);assert.equal(f.writes.at(-1),MANAGER_ROOT+'/installed.json');assert.deepEqual(f.events,['sent','sent']);
});
for(const corrupt of ['manifest.json','botty-manager.elf','ui/app.js'])test('corrupt Botty artifact prevents all writes: '+corrupt,async()=>{
 const f=fixture({corrupt});await assert.rejects(installAndStartManager(f.io,f.options),/verification failed/);assert.equal(f.writes.length,0);assert.equal(f.events.length,0);
});
test('foreign port 8088 service is not overwritten',async()=>{
 const f=fixture({occupied:true});await assert.rejects(installAndStartManager(f.io,f.options),/unexpected service/);assert.equal(f.writes.length,0);
});
test('failed native startup leaves no automatic startup marker',async()=>{
 const f=fixture({noStartup:true});await assert.rejects(installAndStartManager(f.io,f.options),/did not start/);assert.equal(await managerInstalled(f.io),false);
});
test('corrupt installation record fails explicitly',async()=>{
 const f=fixture();f.files.set(MANAGER_ROOT+'/installed.json',new TextEncoder().encode('{'));await assert.rejects(managerInstalled(f.io),/damaged/);
});

test('startup failure reports native diagnostic without recording installation',async()=>{
 const f=fixture({noStartup:true});f.files.set(MANAGER_ROOT+'/startup.log',new TextEncoder().encode('Botty failed while creating working directories: Permission denied'));
 await assert.rejects(installAndStartManager(f.io,f.options),/creating working directories: Permission denied/);
 assert.equal(await managerInstalled(f.io),false);
});

test('running legacy web app is reused without replacement',async()=>{
 const f=fixture({existingBotty:true,version:'0.1.0'});
 const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,'0.1.0');assert.equal(f.writes.length,7);assert.equal(result.updatePending,true);assert.equal(f.files.has(MANAGER_ROOT+'/installed.json'),false);assert.equal(f.events.length,0);
});
test('legacy installation record continues to opt in to normal session startup',async()=>{
 const f=fixture();f.files.set(MANAGER_ROOT+'/installed.json',new TextEncoder().encode(JSON.stringify({app:'Botty',version:'0.1.0'})));
 assert.equal(await managerInstalled(f.io),true);
});
test('wrong version after payload launch cannot record the new service as installed',async()=>{
 const f=fixture({version:'0.1.0'});
 await assert.rejects(installAndStartManager(f.io,f.options),/previous Botty service/);
 assert.equal(f.files.has(MANAGER_ROOT+'/installed.json'),false);
});
test('running 0.1.1 service remains compatible during the catalog update',async()=>{
 const f=fixture({existingBotty:true,version:'0.1.1'});
 const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,'0.1.1');assert.equal(f.writes.length,7);assert.equal(result.updatePending,true);assert.equal(f.files.has(MANAGER_ROOT+'/installed.json'),false);
});
test('0.1.1 installation continues to opt in to startup',async()=>{
 const f=fixture();f.files.set(MANAGER_ROOT+'/installed.json',new TextEncoder().encode(JSON.stringify({app:'Botty',version:'0.1.1'})));
 assert.equal(await managerInstalled(f.io),true);
});

test('running 0.1.2 service remains compatible during the Unicode update',async()=>{
 const f=fixture({existingBotty:true,version:'0.1.2'});
 const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,'0.1.2');assert.equal(f.writes.length,7);assert.equal(result.updatePending,true);assert.equal(f.files.has(MANAGER_ROOT+'/installed.json'),false);
});

test('active 0.1.3 extraction service is kept running during upgrade',async()=>{
 const f=fixture({existingBotty:true,version:'0.1.3'});
 const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,'0.1.3');assert.equal(f.writes.length,7);assert.equal(result.updatePending,true);assert.equal(f.files.has(MANAGER_ROOT+'/installed.json'),false);assert.deepEqual(f.events,[]);
});

test('active 0.1.4 service is preserved during parallel extraction upgrade',async()=>{
 const f=fixture({existingBotty:true,version:'0.1.4'});const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,'0.1.4');assert.equal(f.writes.length,7);assert.equal(result.updatePending,true);assert.equal(f.files.has(MANAGER_ROOT+'/installed.json'),false);assert.deepEqual(f.events,[]);
});

test('current running service is reused without writes',async()=>{
 const f=fixture({existingBotty:true});const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,'1.3.5');assert.equal(f.writes.length,0);assert.deepEqual(f.events,[]);
});
test('running 0.3.3 stages current service without stopping extraction or touching credentials',async()=>{
 const f=fixture({existingBotty:true,version:'0.3.3'});
 f.files.set('/data/botty/transmission/state/botty-credentials.json',new Uint8Array([9]));
 const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.updatePending,true);assert.equal(result.availableVersion,'1.3.5');assert.deepEqual(f.events,[]);
 assert.deepEqual(f.files.get('/data/botty/transmission/state/botty-credentials.json'),new Uint8Array([9]));
});

test('1.0 service stages deletion and resume update without interrupting its work',async()=>{
 const f=fixture({existingBotty:true,version:'1.0.0'});
 const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,'1.0.0');assert.equal(result.availableVersion,'1.3.5');
 assert.equal(result.updatePending,true);assert.deepEqual(f.events,[]);
});

for(const version of ['1.0.1','1.0.2','1.0.3','1.0.4','1.3.0','1.3.1','1.3.2','1.3.3','1.3.4'])test(`previous ${version} service is recognized and updated without stopping its work`, async () => {
 const f=fixture({existingBotty:true,version});
 const result=await installAndStartManager(f.io,f.options);
 assert.equal(result.version,version);assert.equal(result.availableVersion,'1.3.5');assert.equal(result.updatePending,true);
});

test('a listening incompatible worker is rejected without replacing active processes',async()=>{
 const f=fixture({existingBotty:true});const original=f.io.http;
 f.io.http=async(port,...args)=>port===5910?{status:200,body:JSON.stringify({ok:true,bottyWorker:'copy-only-beta1'})}:original(port,...args);
 await assert.rejects(installAndStartManager(f.io,f.options),/incompatible compression worker/);assert.equal(f.writes.length,0);assert.deepEqual(f.events,[]);
});
