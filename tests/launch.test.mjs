import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { launchSession } from '../vps-site/src/launch.js';
import { installNative, NativeIO, NATIVE_ROOT } from '../vps-site/src/botty-native.js';
import { checkedPath } from '../vps-site/src/ps5-io.js';

for (const running of [false, true]) test('one launch prepares all components; FTP already running: ' + running, async () => {
  const events = []; let ftp = running;
  await launchSession({ jailbreak: async () => { events.push('jailbreak'); return {}; }, io: { listening: async () => ftp }, nativeIO: {},
    native: async () => events.push('native'), transmission: async () => events.push('transmission'), manager: async () => events.push('manager'),
    send: async (_, name) => { events.push(name); if (name === 'ftpsrv-ps5.elf') ftp = true; }, wait: async ms => events.push(ms) });
  assert.deepEqual(events, ['jailbreak', 'native', 'kstuff.elf', 10000, 'shadowmountplus.elf', ...(running ? [] : ['ftpsrv-ps5.elf']), 'transmission', 'manager']);
});
test('failed FTP startup stops without blind duplicate sends', async () => {
  const sent = [];
  await assert.rejects(launchSession({ jailbreak: async () => ({}), io: { listening: async () => false }, nativeIO: {}, native: async () => {},
    send: async (_, name) => sent.push(name), wait: async () => {}, transmission: async () => assert.fail('must not run') }), /FTP did not start/);
  assert.equal(sent.filter(n => n === 'ftpsrv-ps5.elf').length, 1);
});
function nativeFixture() {
  const files = new Map(), downloads = [], writes = []; let installed = false;
  const io = { nativeExists: async () => installed, readFile: async p => files.get(p), mkdirs: async () => {},
    writeFile: async (p, b) => { writes.push(p); files.set(p, b); }, publishNative: async () => {
      installed = true; for (const [p, b] of [...files]) files.set(p.replace('/data/botty/native/PPSA99071', NATIVE_ROOT), b);
    } };
  const options = { fetchFile: async url => { downloads.push(url); const b = new Uint8Array(await readFile(new URL('../vps-site/' + url.slice(2), import.meta.url))); return { ok: true, arrayBuffer: async () => b.buffer }; } };
  return { files, downloads, writes, io, options };
}
test('first launch installs native; second launch does not download or rewrite installed files', async () => {
  const f = nativeFixture(); await installNative(f.io, f.options); assert.equal(f.writes.length, 8);
  f.writes.length = 0; f.downloads.length = 0; await installNative(f.io, f.options);
  assert.deepEqual(f.writes, []); assert.deepEqual(f.downloads, ['./apps/botty-native/manifest.json']);
});
test('interrupted native staging resumes; corrupt installed title is preserved', async () => {
  const f = nativeFixture(); const publish = f.io.publishNative; f.io.publishNative = async () => { throw Error('interrupted'); };
  await assert.rejects(installNative(f.io, f.options), /interrupted/); f.writes.length = 0;
  f.io.publishNative = publish; await installNative(f.io, f.options); assert.equal(f.writes.length, 0);
  f.files.set(NATIVE_ROOT + '/eboot.bin', new Uint8Array([0]));
  await assert.rejects(installNative(f.io, f.options), /Existing title preserved/); assert.equal(f.writes.length, 0);
});
test('native adapter allows only its title; default service confinement remains intact', () => {
  const check = p => NativeIO.prototype.checkedPath(p);
  check(NATIVE_ROOT + '/eboot.bin');
  for (const p of ['/data/homebrew/OTHER/eboot.bin', NATIVE_ROOT + '/../OTHER/eboot.bin', '/user/app/PPSA99071/eboot.bin']) assert.throws(() => check(p));
  assert.throws(() => checkedPath(NATIVE_ROOT + '/eboot.bin'));
});
