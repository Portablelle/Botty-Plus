import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { launchSession } from '../vps-site/src/launch.js';
import { installNative, NativeIO, NATIVE_ROOT } from '../vps-site/src/botty-native.js';
import { checkedPath } from '../vps-site/src/ps5-io.js';

for (const running of [false, true]) test('one launch prepares all components; FTP already running: ' + running, async () => {
  const events = []; let ftp = running;
  await launchSession({ jailbreak: async () => { events.push('jailbreak'); return {}; }, io: { listening: async () => ftp }, nativeIO: {},
    native: async () => events.push('native'), rtorrent: async () => events.push('rtorrent'), manager: async () => events.push('manager'),
    send: async (_, name) => { events.push(name); if (name === 'ftpsrv-ps5.elf') ftp = true; }, wait: async ms => events.push(ms) });
  assert.deepEqual(events, ['jailbreak', 'native', 'kstuff.elf', 10000, 'shadowmountplus.elf', ...(running ? [] : ['ftpsrv-ps5.elf']), 'rtorrent', 'manager']);
});
test('failed FTP startup stops without blind duplicate sends', async () => {
  const sent = [];
  await assert.rejects(launchSession({ jailbreak: async () => ({}), io: { listening: async () => false }, nativeIO: {}, native: async () => {},
    send: async (_, name) => sent.push(name), wait: async () => {}, rtorrent: async () => assert.fail('must not run') }), /FTP did not start/);
  assert.equal(sent.filter(n => n === 'ftpsrv-ps5.elf').length, 1);
});
const manifestBytes = new Uint8Array(await readFile(new URL('../vps-site/apps/botty-native/manifest.json', import.meta.url)));
const {createHash} = await import('node:crypto');
const nativeHash = createHash('sha256').update(manifestBytes).digest('hex');
const stage = '/data/botty/native/' + nativeHash + '/PPSA99071';
const journalPath = '/data/botty/native/update.json';
const encode = value => new TextEncoder().encode(JSON.stringify(value));
const decode = bytes => JSON.parse(new TextDecoder().decode(bytes));
function nativeFixture() {
  const files = new Map(), downloads = [], writes = [], events = [];
  const move = (source, target) => {
    assert.ok([...files.keys()].some(p => p.startsWith(source + '/')), 'source must exist');
    assert.ok(![...files.keys()].some(p => p.startsWith(target + '/')), 'never overwrite destination');
    for (const [p, b] of [...files]) if (p.startsWith(source + '/')) {
      files.set(target + p.slice(source.length), b); files.delete(p);
    }
  };
  const io = {
    nativeExists: async () => [...files.keys()].some(p => p.startsWith(NATIVE_ROOT + '/')),
    readFile: async p => files.get(p), mkdirs: async () => {},
    writeFile: async (p, b) => { writes.push(p); files.set(p, b); },
    assertNativeStopped: async () => events.push('check-stopped'),
    writeJournal: async (value, exclusive) => {
      if (exclusive) assert.equal(files.has(journalPath), false);
      await io.writeFile(journalPath, encode(value));
    },
    backupNative: async backup => { events.push('backup'); move(NATIVE_ROOT, backup); },
    restoreNative: async backup => { events.push('restore'); move(backup, NATIVE_ROOT); },
    publishNative: async () => { events.push('publish'); move(stage, NATIVE_ROOT); },
    syncRegisteredMetadata: async () => {},
    prepareNativePermissions: async () => {},
    removeEmptyNative: async () => false,
  };
  const options = { fetchFile: async url => { downloads.push(url); const b = new Uint8Array(await readFile(new URL('../vps-site/' + url.slice(2), import.meta.url))); return { ok: true, arrayBuffer: async () => b.buffer }; } };
  const previous = async (version = '00.005.001') => {
    await installNative(io, options);
    const param = decode(files.get(NATIVE_ROOT + '/sce_sys/param.json'));
    param.contentVersion = version;
    files.set(NATIVE_ROOT + '/sce_sys/param.json', encode(param));
    files.set(NATIVE_ROOT + '/eboot.bin', new Uint8Array([1, 2, 3]));
    writes.length = downloads.length = events.length = 0;
  };
  return { files, downloads, writes, events, io, options, previous };
}
test('first launch installs native; second launch does not download or rewrite installed files', async () => {
  const f = nativeFixture(); await installNative(f.io, f.options); assert.equal(f.writes.length, 13);
  f.writes.length = 0; f.downloads.length = 0; await installNative(f.io, f.options);
  assert.deepEqual(f.writes, []); assert.deepEqual(f.downloads, ['./apps/botty-native/manifest.json']);
});
test('matching native installation repairs permissions without rewriting content', async () => {
  const f = nativeFixture(); await installNative(f.io, f.options);
  f.writes.length = 0; f.downloads.length = 0;
  const roots = [];
  f.io.prepareNativePermissions = async root => roots.push(root);
  await installNative(f.io, f.options);
  assert.deepEqual(roots, [NATIVE_ROOT]);
  assert.deepEqual(f.writes, []);
  assert.deepEqual(f.downloads, ['./apps/botty-native/manifest.json']);
  f.io.prepareNativePermissions = async () => { throw Error('Permission repair failed'); };
  await assert.rejects(installNative(f.io, f.options), /Permission repair failed/);
});
test('native permissions make the executable and runtime loadable, with readable assets', async () => {
  const calls = [], synced = [];
  const io = { string: path => path, syncDirectory: async path => synced.push(path),
    runtime: { chain: { syscall: async (...args) => { calls.push(args); return { low: 0 }; } } } };
  for (const root of [stage, NATIVE_ROOT]) {
    calls.length = 0; synced.length = 0;
    await NativeIO.prototype.prepareNativePermissions.call(io, root);
    assert.equal(calls.length, 17);
    for (const [syscall, path, mode] of calls) {
      assert.equal(syscall, 15);
      const relative = path.slice(root.length);
      const executable = ['', '/assets', '/sce_module', '/sce_sys', '/eboot.bin', '/sce_module/libc.prx'].includes(relative);
      assert.equal(mode, executable ? 0o755 : 0o644, path);
    }
    assert.equal(synced.length, 4);
  }
  calls.length = 0;
  await assert.rejects(NativeIO.prototype.prepareNativePermissions.call(io, '/data/homebrew/OTHER'), /Unexpected native permission root/);
  assert.deepEqual(calls, []);
  io.runtime.chain.syscall = async () => ({ low: 0xffffffff });
  await assert.rejects(NativeIO.prototype.prepareNativePermissions.call(io, stage), /permissions/);
});
test('native publication prepares runtime permissions before exposing the title', async () => {
  const events = [];
  const io = { prepareNativePermissions: async root => events.push(['permissions', root]),
    string: path => path, call: async () => 0, close: async () => {},
    moveDirectory: async (...paths) => events.push(['publish', ...paths]) };
  await NativeIO.prototype.publishNative.call(io);
  assert.deepEqual(events, [['permissions', stage], ['publish', stage, NATIVE_ROOT]]);
  events.length = 0;
  io.prepareNativePermissions = async () => { throw Error('Permission setup failed'); };
  await assert.rejects(NativeIO.prototype.publishNative.call(io), /Permission setup failed/);
  assert.deepEqual(events, []);
});
test('interrupted native staging resumes without redownloading', async () => {
  const f = nativeFixture(); const publish = f.io.publishNative; f.io.publishNative = async () => { throw Error('interrupted'); };
  await assert.rejects(installNative(f.io, f.options), /interrupted/); f.writes.length = 0;
  f.io.publishNative = publish; await installNative(f.io, f.options); assert.equal(f.writes.length, 0);
});
test('older native title updates only after staging, retains exact previous tree and user state', async () => {
  const f = nativeFixture(); await f.previous();
  f.files.set('/data/botty/jobs/example.json', encode({state:'extracting'}));
  f.files.set(NATIVE_ROOT + '/extra-user-file', new Uint8Array([7]));
  const old = new Map([...f.files].filter(([p]) => p.startsWith(NATIVE_ROOT + '/')));
  const result = await installNative(f.io, f.options);
  assert.equal(result.updated, true);
  const journal = decode(f.files.get(journalPath)); assert.equal(journal.status, 'complete');
  for (const [p, data] of old) assert.deepEqual(f.files.get(journal.backup + p.slice(NATIVE_ROOT.length)), data);
  assert.equal(decode(f.files.get('/data/botty/jobs/example.json')).state, 'extracting');
  assert.equal(decode(f.files.get(NATIVE_ROOT + '/sce_sys/param.json')).contentVersion, '01.000.006');
  assert.deepEqual(f.events, ['check-stopped', 'check-stopped', 'backup', 'publish']);
});
test('recognized current title with damaged executable is backed up and repaired', async () => {
  const f = nativeFixture(); await f.previous('01.000.006');
  await installNative(f.io, f.options);
  assert.ok(f.files.get(NATIVE_ROOT + '/eboot.bin').length > 3);
  assert.deepEqual(f.files.get(decode(f.files.get(journalPath)).backup + '/eboot.bin'), new Uint8Array([1,2,3]));
});
for (const kind of ['foreign', 'newer', 'missing-metadata']) test('preserves unsupported installation: ' + kind, async () => {
  const f = nativeFixture(); await f.previous(kind === 'newer' ? '99.000.000' : '00.005.001');
  if (kind === 'foreign') {const p = decode(f.files.get(NATIVE_ROOT+'/sce_sys/param.json'));p.contentId='OTHER';f.files.set(NATIVE_ROOT+'/sce_sys/param.json',encode(p));}
  if (kind === 'missing-metadata') f.files.delete(NATIVE_ROOT+'/sce_sys/param.json');
  await assert.rejects(installNative(f.io, f.options), /recognized|Downgrade/);
  assert.equal(f.writes.length, 0); assert.equal(f.events.length, 0);
});
test('launch keeps a recognized newer title untouched and starts every service', async () => {
  const f = nativeFixture(); await f.previous('99.000.000');
  const before = new Map(f.files), reports = [], services = [];
  f.io.prepareNativePermissions = async () => assert.fail('must not change newer title permissions');
  f.io.syncRegisteredMetadata = async () => assert.fail('must not change newer title metadata');
  const result = await launchSession({ jailbreak: async () => ({}), io: {listening: async () => true}, nativeIO: f.io,
    native: (io, options) => installNative(io, {...f.options, ...options}),
    rtorrent: async () => services.push('rtorrent'), manager: async () => services.push('manager'),
    send: async (_, name) => services.push(name), wait: async () => {}, report: message => reports.push(message) });
  assert.deepEqual(result.native, {version: '99.000.000', updated: false});
  assert.deepEqual(services, ['kstuff.elf', 'shadowmountplus.elf', 'rtorrent', 'manager']);
  assert.deepEqual(f.files, before); assert.deepEqual(f.writes, []); assert.deepEqual(f.events, []);
  assert.deepEqual(f.downloads, ['./apps/botty-native/manifest.json']);
  assert.ok(reports.some(message => /Keeping installed Botty\+ 99\.000\.000/.test(message)));
});
for (const kind of ['foreign', 'missing-metadata', 'invalid-version']) test('launch reuse still rejects invalid title: ' + kind, async () => {
  const f = nativeFixture(); await f.previous('01.000.006');
  const path = NATIVE_ROOT + '/sce_sys/param.json', param = decode(f.files.get(path));
  if (kind === 'foreign') param.contentId = 'OTHER';
  if (kind === 'invalid-version') param.contentVersion = '1.0.3';
  f.files.set(path, encode(param));
  if (kind === 'missing-metadata') f.files.delete(path);
  await assert.rejects(installNative(f.io, {...f.options, reuseNewer: true}), /recognized/);
  assert.deepEqual(f.writes, []); assert.deepEqual(f.events, []);
});
test('running app and process inspection failures block update before staging', async () => {
  const f = nativeFixture(); await f.previous();
  f.io.assertNativeStopped = async () => { throw Error('app running'); };
  await assert.rejects(installNative(f.io, f.options), /app running/);
  assert.equal(f.writes.length, 0); assert.deepEqual(f.events, []);
});
test('app launched during preparation blocks the backup and preserves live title', async () => {
  const f = nativeFixture(); await f.previous(); let checks = 0;
  f.io.assertNativeStopped = async () => { if (++checks === 2) throw Error('app running'); };
  await assert.rejects(installNative(f.io, f.options), /app running/);
  assert.deepEqual(f.files.get(NATIVE_ROOT+'/eboot.bin'), new Uint8Array([1,2,3]));
  assert.equal(f.files.has(journalPath), false); assert.deepEqual(f.events, []);
});
test('corrupt staged download cannot move old title', async () => {
  const f = nativeFixture(); await f.previous(); const fetch = f.options.fetchFile;
  f.options.fetchFile = async url => {const r=await fetch(url);if(url.endsWith('/eboot.bin'))return {ok:true,arrayBuffer:async()=>new Uint8Array([0]).buffer};return r;};
  await assert.rejects(installNative(f.io, f.options), /verification failed/);
  assert.equal(f.events.includes('backup'), false); assert.equal(f.files.has(journalPath), false);
});
test('promotion failure restores previous title without deleting staging or user data', async () => {
  const f = nativeFixture(); await f.previous(); const publish=f.io.publishNative;
  f.io.publishNative=async()=>{throw Error('promotion failed');};
  await assert.rejects(installNative(f.io,f.options),/promotion failed/);
  assert.deepEqual(f.files.get(NATIVE_ROOT+'/eboot.bin'),new Uint8Array([1,2,3]));
  assert.equal(decode(f.files.get(journalPath)).status,'rolled-back');
  f.io.publishNative=publish;await installNative(f.io,f.options);
  assert.equal(decode(f.files.get(journalPath)).status,'complete');
});
test('next session recovers a power loss between backup and promotion', async () => {
  const f=nativeFixture();await f.previous();const publish=f.io.publishNative,restore=f.io.restoreNative;
  f.io.publishNative=async()=>{throw Error('power loss');};f.io.restoreNative=async()=>{throw Error('power loss');};
  await assert.rejects(installNative(f.io,f.options),/restart to recover/);
  assert.equal(await f.io.nativeExists(),false);assert.equal(decode(f.files.get(journalPath)).status,'pending');
  f.io.publishNative=publish;f.io.restoreNative=restore;await installNative(f.io,f.options);
  assert.ok(f.events.includes('restore'));assert.equal(decode(f.files.get(journalPath)).status,'complete');
});
test('lost completion checkpoint converges without a second replacement', async () => {
  const f=nativeFixture();await f.previous();const write=f.io.writeJournal;
  f.io.writeJournal=async(value,exclusive)=>{if(value.status==='complete')throw Error('checkpoint failed');await write(value,exclusive);};
  await assert.rejects(installNative(f.io,f.options),/restart to recover/);
  f.io.writeJournal=write;f.events.length=0;await installNative(f.io,f.options);
  assert.equal(decode(f.files.get(journalPath)).status,'complete');assert.deepEqual(f.events,[]);
});
test('malformed or escaping recovery journal cannot move any title', async () => {
  const f=nativeFixture();await f.previous();f.files.set(journalPath,encode({schema:1,status:'pending',target:nativeHash,previous:nativeHash,backup:'/data/homebrew/OTHER'}));
  await assert.rejects(installNative(f.io,f.options),/journal is damaged/);assert.deepEqual(f.events,[]);assert.equal(f.writes.length,0);
});
test('real native process guard fails closed without sending a signal', async () => {
  for(const name of ['eboot.bin','eboot','Botty+'])await assert.rejects(NativeIO.prototype.assertNativeStopped.call({processes:async()=>[{pid:123,name}]}),/Close Botty/);
  await NativeIO.prototype.assertNativeStopped.call({processes:async()=>[{pid:123,name:'transmission-da'},{pid:321,name:'payload.elf'}]});
});
test('native adapter allows only its title; default service confinement remains intact', () => {
  const check = p => NativeIO.prototype.checkedPath(p);
  check(NATIVE_ROOT + '/eboot.bin');
  for (const p of ['/data/homebrew/OTHER/eboot.bin', NATIVE_ROOT + '/../OTHER/eboot.bin', '/user/app/PPSA99071/eboot.bin', '/user/app/OTHER/sce_sys/param.json', '/user/app/PPSA99071/sce_sys/../../OTHER/param.json']) assert.throws(() => check(p));
  assert.throws(() => checkedPath(NATIVE_ROOT + '/eboot.bin'));
});

test('registered metadata is backed up, updated, readable and confined to Botty+', async () => {
  const f=nativeFixture();await installNative(f.io,f.options);
  const manifest=decode(manifestBytes),old=decode(f.files.get(NATIVE_ROOT+'/sce_sys/param.json'));
  old.contentVersion='00.005.001';
  const path='/user/app/PPSA99071/sce_sys/param.json';
  const oldBytes=encode(old);f.files.set(path,oldBytes);
  f.files.set('/user/app/OTHER/sce_sys/param.json',new Uint8Array([8]));
  const calls=[];f.io.call=async()=>1;f.io.close=async()=>{};f.io.string=p=>p;f.io.syncDirectory=async()=>{};
  f.io.runtime={chain:{syscall:async(...args)=>{calls.push(args);return {low:0};}}};
  const {sha256}=await import('../vps-site/src/transmission.js');
  const backup='/data/botty/native/backups/'+'a'.repeat(32)+'/PPSA99071';
  await NativeIO.prototype.syncRegisteredMetadata.call(f.io,manifest,backup,sha256);
  assert.deepEqual(f.files.get(backup.replace('/PPSA99071','')+'/metadata/0.bin'),oldBytes);
  assert.equal(decode(f.files.get(path)).contentVersion,'01.000.006');
  assert.deepEqual(f.files.get('/user/app/PPSA99071/sce_sys/snd0.at9'), f.files.get(NATIVE_ROOT+'/sce_sys/snd0.at9'));
  assert.deepEqual(f.files.get('/user/app/OTHER/sce_sys/param.json'),new Uint8Array([8]));
  assert.ok(calls.every(([nr,p,mode])=>nr===15&&p.startsWith('/user/app/PPSA99071/')&&mode===0o644));
  await NativeIO.prototype.syncRegisteredMetadata.call(f.io,manifest,backup,sha256);
  assert.deepEqual(f.files.get(backup.replace('/PPSA99071','')+'/metadata/0.bin'),oldBytes);
});
test('registered metadata with foreign identity is never overwritten',async()=>{
  const f=nativeFixture();await installNative(f.io,f.options);f.writes.length=0;
  f.files.set('/user/app/PPSA99071/sce_sys/param.json',encode({titleId:'OTHER'}));
  const {sha256}=await import('../vps-site/src/transmission.js');
  await assert.rejects(NativeIO.prototype.syncRegisteredMetadata.call(f.io,decode(manifestBytes),'/data/botty/native/backups/test',sha256),/recognized/);
  assert.equal(f.writes.length,0);
});

test('empty first-install reservation can recover from intact verified staging',async()=>{
 const f=nativeFixture();const publish=f.io.publishNative;f.io.publishNative=async()=>{throw Error('power loss');};
 await assert.rejects(installNative(f.io,f.options),/power loss/);
 f.io.nativeExists=async()=>true;f.io.removeEmptyNative=async()=>true;f.io.publishNative=publish;
 await installNative(f.io,f.options);
 assert.equal(decode(f.files.get(NATIVE_ROOT+'/sce_sys/param.json')).contentVersion,'01.000.006');
});
test('native rename failure removes only its empty reservation and never deletes source',async()=>{
 const events=[];const io={checkedPath:()=>{},string:p=>p,
 call:async(name,...args)=>{events.push([name,...args]);return name==='rename'?-1:0;},
 runtime:{chain:{syscall:async(...args)=>{events.push(args);return {low:0};}}}};
 await assert.rejects(NativeIO.prototype.moveDirectory.call(io,'/data/botty/native/staged','/data/homebrew/PPSA99071'),/Could not move/);
 assert.deepEqual(events.map(e=>e[0]),['mkdir','rename',137]);
 assert.equal(events[2][1],NATIVE_ROOT);
});
