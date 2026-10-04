import test from 'node:test';
import assert from 'node:assert/strict';
import { sendPayload } from '../vps-site/src/payload-sender.js';
import { loadRequiredPayloads } from '../vps-site/src/session.js';

function fixture({ failConnect = 0, writeError = false, chunkLimit = 10000 } = {}) {
  const bytes = new Uint8Array(150007);
  bytes.set([127,69,76,70]);
  for (let i=4;i<bytes.length;i++) bytes[i]=i%251;
  const allocations=[], closed=[], delivered=[];
  let connects=0;
  const runtime={p:{malloc(size){
    const backing=new Uint8Array(size);
    const ptr={backing,offset:0,add32(offset){return {backing,offset};}};
    allocations.push(ptr);return ptr;
  }},chain:{async syscall(number,...args){
    if(number===97)return {low:42};
    if(number===98){
      assert.deepEqual([...args[1].backing], [16,2,35,61,127,0,0,1,0,0,0,0,0,0,0,0]);
      return {low:connects++<failConnect ? 0xffffffff : 0};
    }
    if(number===6){closed.push(args[0]);return {low:0};}
    if(number===4){
      if(writeError)return {low:0xffffffff};
      const count=Math.min(args[2],chunkLimit);
      delivered.push(args[1].backing.slice(args[1].offset,args[1].offset+count));
      return {low:count};
    }
    throw Error('Unexpected syscall');
  }}};
  const fetchFile=async url=>{assert.equal(url,'./payloads/kstuff.elf');return {ok:true,arrayBuffer:async()=>bytes.buffer};};
  return {runtime,fetchFile,bytes,allocations,closed,delivered};
}
test('delivers exact ELF bytes to loopback, including short writes, and closes socket',async()=>{
  const f=fixture();assert.equal(await sendPayload(f.runtime,'kstuff.elf',f.fetchFile),f.bytes.length);
  assert.deepEqual(Buffer.concat(f.delivered),Buffer.from(f.bytes));assert.deepEqual(f.closed,[42]);
});
test('failed connects close sockets before retry; success sends once',async()=>{
  const f=fixture({failConnect:2});await sendPayload(f.runtime,'kstuff.elf',f.fetchFile,async()=>{});
  assert.equal(f.closed.length,3);assert.deepEqual(Buffer.concat(f.delivered),Buffer.from(f.bytes));
});
test('unavailable loader terminates after bounded retries',async()=>{
  const f=fixture({failConnect:99});await assert.rejects(sendPayload(f.runtime,'kstuff.elf',f.fetchFile,async()=>{}),/not accepting/);
  assert.equal(f.closed.length,20);assert.equal(f.delivered.length,0);
});
test('interrupted transfer closes socket and fails',async()=>{
  const f=fixture({writeError:true});await assert.rejects(sendPayload(f.runtime,'kstuff.elf',f.fetchFile),/interrupted/);assert.deepEqual(f.closed,[42]);
});
test('rejects unexpected paths and non-ELF downloads before allocating memory',async()=>{
  const f=fixture();await assert.rejects(sendPayload(f.runtime,'../bad',f.fetchFile),/Unknown/);
  await assert.rejects(sendPayload(f.runtime,'kstuff.elf',async()=>({ok:true,arrayBuffer:async()=>new ArrayBuffer(4096)})),/Invalid ELF/);
  assert.equal(f.allocations.length,0);
});
test('HTTP errors and absent runtime stop without allocation',async()=>{
  const f=fixture();await assert.rejects(sendPayload(null,'kstuff.elf'),/Jailbreak first/);
  await assert.rejects(sendPayload(f.runtime,'kstuff.elf',async()=>({ok:false,status:404})),/404/);
  assert.equal(f.allocations.length,0);
});
test('gaming sequence waits after Kstuff and excludes optional payloads',async()=>{
  const events=[];await loadRequiredPayloads({}, {send:async(_,name)=>events.push(name),wait:async ms=>events.push(ms),report(){},markSent:name=>events.push('sent:'+name)});
  assert.deepEqual(events,['kstuff.elf','sent:kstuff.elf',10000,'shadowmountplus.elf','sent:shadowmountplus.elf']);
});
test('Kstuff failure prevents ShadowMountPlus delivery',async()=>{
  const events=[];await assert.rejects(loadRequiredPayloads({}, {send:async(_,name)=>{events.push(name);throw Error('failed');},report(){},markSent(){},wait:async()=>assert.fail('must not wait')}),/failed/);
  assert.deepEqual(events,['kstuff.elf']);
});

test('PPR waits for user confirmation before ShadowMountPlus', async () => {
  const events = [];
  await loadRequiredPayloads({}, {ppr:true, send:async(_,name)=>events.push(name), wait:async()=>{}, report(){}, markSent(){}, confirmPpr:async()=>events.push('confirmed')});
  assert.deepEqual(events, ['kstuff.elf', 'a53_ppr_install.elf', 'confirmed', 'shadowmountplus.elf']);
  const sent=[];
  await assert.rejects(loadRequiredPayloads({}, {ppr:true, send:async(_,name)=>sent.push(name), wait:async()=>{}, report(){}, markSent(){}}), /requires confirmation/);
  assert.deepEqual(sent, ['kstuff.elf']);
});
