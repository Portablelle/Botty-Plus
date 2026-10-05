import test from 'node:test';
import assert from 'node:assert/strict';
import {startCodex, CodexIO} from '../vps-site/src/codex.js';
import {normalizeLaunchServices} from '../vps-site/src/launch-options.js';
function fixture(tasks = []) {
  let ready = false; const events = [];
  return {events, io: {
    listening: async port => port === 49322 && ready,
    http: async (port, path) => port === 49323 ? ({status:200, body:JSON.stringify({service:'codex-ps5',build:'b'.repeat(64),idle:true,pid:123})}) : ({status:200,body:JSON.stringify(path.endsWith('bootstrap') ? {token:'a'.repeat(32)} : {tasks})}),
    visitPayload: async () => events.push('verify'),
    deliverPayload: async () => {events.push('send');ready=true;},
  }};
}
test('Codex is opt-in and old preferences keep it disabled', () => {
  assert.equal(normalizeLaunchServices({}).codex,false);
  assert.equal(normalizeLaunchServices({codex:'true'}).codex,false);
  assert.equal(normalizeLaunchServices({codex:true}).codex,true);
});
test('active work defers Codex without verification or delivery', async () => {
  const f=fixture([{kind:'compression',busy:true,status:'running'}]);
  assert.equal((await start(f.io)).ready,false);assert.deepEqual(f.events,[]);
});
const install = async () => ({version:'0.0.2',serviceBuild:'b'.repeat(64)});
const start = (io, options = {}) => startCodex(io, {install, ...options});
test('idle startup makes one verified transfer; existing service is reused', async () => {
  const f=fixture();assert.equal((await start(f.io)).ready,true);
  assert.deepEqual(f.events,['send']);
  assert.equal((await start(f.io)).reused,true);assert.deepEqual(f.events,['send']);
});
test('uncertain delivery is never retried', async () => {
  const f=fixture();f.io.deliverPayload=async()=>{f.events.push('send');throw Error('interrupted');};
  await assert.rejects(start(f.io),/interrupted/);assert.deepEqual(f.events,['send']);
});
test('payload access rejects foreign paths and closes a corrupt file', async () => {
  assert.throws(()=>CodexIO.prototype.checkedPath('/data/botty/jobs/file'),/Unexpected/);
  const events=[];const buffer={backing:new Uint8Array(65536)};
  const io={buffer,checkedPath:CodexIO.prototype.checkedPath,string:p=>p,
    call:async(name,...args)=>{events.push(name);if(name==='open')return 42;return args[2];},
    close:async fd=>events.push(['close',fd])};
  await assert.rejects(CodexIO.prototype.visitPayload.call(io,null,async()=> 'wrong'),/differs/);
  assert.deepEqual(events.at(-1),['close',42]);
});
const {launchSession}=await import('../vps-site/src/launch.js');
for (const fail of [false,true]) test('optional Codex starts after Botty and isolates startup failure: '+fail, async()=>{
  const events=[];
  const result=await launchSession({services:{ftp:false,rtorrent:false,cheatrunner:false,codex:true},
    jailbreak:async()=>({}),io:{},nativeIO:{},codexIO:{},native:async()=>{},
    send:async()=>{},wait:async()=>{},manager:async()=>events.push('botty'),
    codex:async()=>{events.push('codex');if(fail)throw Error('unavailable');return {ready:true};}});
  assert.deepEqual(events,['botty','codex']);assert.equal(result.codex.ready,!fail);
});
test('large Codex transfers use MiB writes and preserve partial-write offsets', async () => {
  const buffer={backing:new Uint8Array(1048576),add32:offset=>offset}, writes=[];
  const io={buffer,call:async(name,fd,offset,length)=>{assert.equal(name,'write');assert.equal(fd,7);const n=Math.min(length,300000);writes.push(buffer.backing.slice(offset,offset+n));return n;}};
  const input=Uint8Array.from({length:1048583},(_,i)=>i%251);
  await CodexIO.prototype.writeAll.call(io,7,input);
  assert.deepEqual(Buffer.concat(writes.map(x=>Buffer.from(x))),Buffer.from(input));
  assert.equal(writes.length,5);
  io.call=async()=>0;await assert.rejects(CodexIO.prototype.writeAll.call(io,7,input),/interrupted/);
});
test('file stamps use the SDK ABI, ignore access time and always close descriptors', async () => {
  const source=new Uint8Array(120),view=new DataView(source.buffer);
  view.setUint16(8,0x81ed,true);view.setUint32(72,162782168,true);view.setUint32(4,123,true);
  const events=[],statBuffer={backing:new Uint8Array(256)};
  const io={checkedPath:()=>{},string:p=>p,statBuffer,call:async()=>9,close:async fd=>events.push(fd),
    runtime:{chain:{syscall:async(number,fd,target)=>{assert.equal(number,189);assert.equal(fd,9);target.backing.set(source);return {low:0};}}}};
  const stamp=await CodexIO.prototype.fileStamp.call(io,'/data/codex-ps5/payloads/assistant-service/assistant-service.elf');
  assert.equal(stamp.size,162782168);assert.equal(stamp.stamp.length,176);
  view.setUint32(24,555,true);assert.deepEqual(await CodexIO.prototype.fileStamp.call(io,'test'),stamp);
  view.setUint32(56,777,true);assert.notDeepEqual(await CodexIO.prototype.fileStamp.call(io,'test'),stamp);
  view.setUint16(8,0x41ed,true);assert.equal(await CodexIO.prototype.fileStamp.call(io,'test'),null);
  assert.deepEqual(events,[9,9,9,9]);
});
