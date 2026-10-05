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
test('idle startup verifies before one delivery; existing service is reused', async () => {
  const f=fixture();assert.equal((await start(f.io)).ready,true);
  assert.deepEqual(f.events,['verify','send']);
  assert.equal((await start(f.io)).reused,true);assert.deepEqual(f.events,['verify','send']);
});
test('uncertain delivery is never retried', async () => {
  const f=fixture();f.io.deliverPayload=async()=>{f.events.push('send');throw Error('interrupted');};
  await assert.rejects(start(f.io),/interrupted/);assert.deepEqual(f.events,['verify','send']);
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
