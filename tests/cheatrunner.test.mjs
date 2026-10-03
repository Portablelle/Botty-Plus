import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { installAndStartCheatRunner, CheatRunnerIO, prepareConfig, cheatRunnerStatus, VERSION } from '../vps-site/src/cheatrunner.js';
import { launchSession } from '../vps-site/src/launch.js';
const encode = text => new TextEncoder().encode(text);
const decode = bytes => new TextDecoder().decode(bytes);
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const configPath = '/data/cheatrunner/config.ini';
const base = '/data/botty/cheatrunner';
const diskPath = base + '/releases/' + VERSION + '/CheatRunner.elf';
const manifest = new Uint8Array(await readFile(new URL('../vps-site/apps/cheatrunner/manifest.json', import.meta.url)));
const binary = new Uint8Array(await readFile(new URL('../vps-site/apps/cheatrunner/CheatRunner.elf', import.meta.url)));

function fixture() {
  const f = { files: new Map(), writes: [], downloads: [], sends: [], busy: false, ready: false, registered: true, processes: [], version: VERSION, workChecks: 0, waits: 0 };
  f.io = {
    listening: async port => { assert.equal(port, 9999); return f.ready; },
    processes: async () => f.processes,
    tileRegistered: async () => f.registered,
    http: async (port, path, body, headers) => {
      let result;
      if (port === 8088 && path === '/api/bootstrap') result = { token: 'a'.repeat(32) };
      else if (port === 8088 && path === '/api/state') {
        assert.equal(headers['X-Botty-Token'], 'a'.repeat(32));
        f.workChecks++;
        result = {extracting: f.busy, compression: {busy: f.compressing || false}};
        if (f.uncertain) delete result.compression;
      } else if (port === 9999 && path === '/api/health') result = f.foreign ? {ok:true} : {ok:true, version:f.version, busy:{}, http:{}};
      else if (port === 9999 && path === '/api/config') result = {ok:true,http_port:9999,tile_autoinstall_enabled:1,hotkey_enabled:0};
      else assert.fail('Unexpected request: ' + path);
      return {status:200,body:JSON.stringify(result)};
    },
    readFile: async (p, limit) => { const bytes = f.files.get(p); if (bytes && bytes.length > limit) throw Error('oversized'); return bytes || null; },
    mkdirs: async () => {},
    writeFile: async (p, bytes, exclusive) => { if (exclusive && f.files.has(p)) throw Error('exists'); f.files.set(p, bytes); f.writes.push(p); },
    sendElf: async bytes => { f.sends.push(bytes); f.ready = !f.noStartup; },
  };
  f.options = { digest: hash, wait: async () => { f.waits++; }, fetchFile: async url => {
    f.downloads.push(url);
    let bytes = url.endsWith('manifest.json') ? manifest : binary;
    if (url.endsWith('.elf') && f.corruptDownload) bytes = binary.slice(1);
    if (url.endsWith('manifest.json') && f.corruptManifest) bytes = encode('{}');
    if (f.busyAfterDownload) f.busy = true;
    return {ok:true,arrayBuffer:async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)};
  }};
  return f;
}

test('pinned release installs, verifies readback, starts once and confirms tile registration', async () => {
  const f = fixture(); const result = await installAndStartCheatRunner(f.io, f.options);
  assert.equal(result.ready, true); assert.equal(result.tileRegistered, true);
  assert.equal(hash(f.sends[0]), hash(binary)); assert.equal(f.sends.length, 1);
  assert.match(decode(f.files.get(configPath)), /hotkey_enabled=0\n/);
  assert.match(decode(f.files.get(configPath)), /tile_autoinstall_enabled=1\n/);
  assert.equal(f.workChecks, 2);
});
test('running service is reused without config changes, downloads or duplicate delivery', async () => {
  const f = fixture(); f.ready = true; f.version = '0.17.1'; f.busy = true;
  const result = await installAndStartCheatRunner(f.io, f.options);
  assert.equal(result.reused, true); assert.equal(result.updatePending, true);
  assert.deepEqual(f.writes, []); assert.deepEqual(f.downloads, []); assert.deepEqual(f.sends, []);
});
test('stopped service restarts from verified staged bytes without redownloading ELF', async () => {
  const f = fixture(); f.files.set(diskPath, binary);
  await installAndStartCheatRunner(f.io, f.options);
  assert.deepEqual(f.downloads, ['./apps/cheatrunner/manifest.json']);
});
test('config change preserves unrelated preferences and backs up exact original bytes', async () => {
  const f = fixture(); const old = encode('# custom\r\nhttp_port=8888\r\nhotkey_enabled=1\r\ntheme=oled\r\nhotkey_enabled=1\r\n');
  f.files.set(configPath, old); f.files.set('/data/cheatrunner/cheats/game.json', encode('original'));
  await installAndStartCheatRunner(f.io, f.options);
  assert.deepEqual(f.files.get(base + '/config-backups/' + hash(old) + '.ini'), old);
  const next = decode(f.files.get(configPath)); assert.match(next, /theme=oled/);
  assert.equal((next.match(/hotkey_enabled=/g) || []).length, 1);
  assert.equal(decode(f.files.get('/data/cheatrunner/cheats/game.json')), 'original');
  assert.equal(prepareConfig(next), next);
});
for (const mode of ['busy', 'compressing', 'busyAfterDownload']) test('defers startup during work: ' + mode, async () => {
  const f = fixture(); f[mode] = true;
  const result = await installAndStartCheatRunner(f.io, f.options);
  assert.equal(result.deferred, true); assert.equal(f.sends.length, 0); assert.equal(f.files.has(configPath), false);
});
test('uncertain work state prevents injection', async () => {
  const f = fixture(); f.uncertain = true;
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /work state/);
  assert.equal(f.sends.length, 0);
});
for (const mode of ['corruptManifest', 'corruptDownload']) test('rejects altered release: ' + mode, async () => {
  const f = fixture(); f[mode] = true;
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /verification failed/);
  assert.equal(f.sends.length, 0); assert.equal(f.writes.length, 0);
});
test('disk corruption prevents launch', async () => {
  const f = fixture(); const write = f.io.writeFile;
  f.io.writeFile = async (path, bytes, exclusive) => write(path, path.endsWith('.elf') ? bytes.slice(1) : bytes, exclusive);
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /disk verification/);
  assert.equal(f.sends.length, 0);
});
test('unresponsive existing process is never killed or reinjected', async () => {
  const f = fixture(); f.processes = [{name:'CheatRunner.elf',pid:10}];
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /already running/);
  assert.equal(f.sends.length, 0); assert.equal(f.writes.length, 0);
});
test('foreign listener does not count as CheatRunner health', async () => {
  const f = fixture(); f.ready = true; f.foreign = true;
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /unexpected service/);
  assert.equal(f.sends.length, 0);
});
test('missing tile does not falsely report a working homescreen install', async () => {
  const f = fixture(); f.registered = false;
  const result = await installAndStartCheatRunner(f.io, f.options);
  assert.equal(result.ready, true); assert.equal(result.tileRegistered, false);
  assert.match(cheatRunnerStatus(result), /not confirmed/); assert.equal(f.waits, 60);
});
test('startup timeout sends only once', async () => {
  const f = fixture(); f.noStartup = true;
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /did not become ready/);
  assert.equal(f.sends.length, 1); assert.equal(f.waits, 60);
});
test('storage exception is restricted to the config; title check is read-only', async () => {
  const check = p => CheatRunnerIO.prototype.checkedPath(p);
  check(configPath); check(base + '/releases/0.17.2/CheatRunner.elf');
  for (const path of ['/data/cheatrunner/cheats/game.json', '/data/cheatrunner/../other', '/user/appmeta/CHTR09999/param.json']) assert.throws(() => check(path));
  const calls = [];
  assert.equal(await CheatRunnerIO.prototype.tileRegistered.call({string:p=>p,call:async(...args)=>{calls.push(args);return 12;},close:async fd=>calls.push(['close',fd])}),true);
  assert.deepEqual(calls, [['open','/user/appmeta/CHTR09999',0x20100,0],['close',12]]);
});
test('CheatRunner error leaves successful Botty session ready and reports cause', async () => {
  const messages = [];
  const result = await launchSession({jailbreak:async()=>({}),io:{listening:async()=>true},nativeIO:{},cheatRunnerIO:{},
    native:async()=>({}),send:async()=>{},wait:async()=>{},rtorrent:async()=>{},manager:async()=>({version:'1.3.5'}),
    cheatrunner:async()=>{throw Error('tile failure');},report:message=>messages.push(message)});
  assert.equal(result.manager.version,'1.3.5'); assert.equal(result.cheatrunner.ready,false);
  assert.match(messages.at(-1),/tile failure/);
});

test('corrupt config backup cannot replace the user config or launch a payload', async () => {
  const f = fixture(); const old = encode('hotkey_enabled=1\ntheme=oled\n');
  f.files.set(configPath, old); f.files.set(base + '/config-backups/' + hash(old) + '.ini', encode('damaged'));
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /backup verification/);
  assert.deepEqual(f.files.get(configPath), old); assert.equal(f.sends.length, 0);
});
test('a process appearing during staging prevents duplicate payload delivery', async () => {
  const f = fixture(); let calls = 0;
  f.io.processes = async () => ++calls === 1 ? [] : [{name:'CheatRunner.elf',pid:20}];
  await assert.rejects(installAndStartCheatRunner(f.io, f.options), /duplicate delivery/);
  assert.equal(f.sends.length, 0); assert.equal(f.files.has(configPath), false);
});
