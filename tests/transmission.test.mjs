import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { webcrypto } from 'node:crypto';
import { installAndStart, stopTransmission, settingsFor, sha256, prepareShortPassword, shortPassword, ROOT, STATE, DOWNLOADS } from '../vps-site/src/transmission.js';
import { checkedPath, parseHttp, PS5IO } from '../vps-site/src/ps5-io.js';
if (!globalThis.crypto) globalThis.crypto = webcrypto;
const encode = value => new TextEncoder().encode(JSON.stringify(value));
const credentials = { username: 'botty', password: 'B7mQ2x' };
const legacyCredentials = {username: 'botty', password: '1234567890abcdef1234567890abcdef'};
const manifest = JSON.parse(await readFile(new URL('../vps-site/apps/transmission/manifest.json', import.meta.url)));
const app = ROOT + '/' + manifest.id;

function fixture(options = {}) {
  const files = new Map(); const writes = []; const events = []; const downloads = [];
  let running = !!options.running, helper = false, configured = false;
  let revealed;
  if (options.running) files.set(STATE + '/transmission.pid', new TextEncoder().encode('200'));
  if (options.saved !== false) {
    files.set(STATE + '/botty-credentials.json', encode(credentials));
    files.set(STATE + '/settings.json', encode(settingsFor(credentials)));
  }
  const info = { version: '4.0.6 (38c164933e)', 'download-dir': DOWNLOADS + '/complete',
    'incomplete-dir': DOWNLOADS + '/incomplete', 'incomplete-dir-enabled': true, 'download-dir-free-space': 100 * 1073741824 };
  const io = {
    async readFile(path) { return files.get(path) || null; },
    async mkdirs(path) { events.push('mkdir:' + path); },
    async writeFile(path, bytes, exclusive) {
      if (exclusive && files.has(path)) throw Error('Existing file');
      if (options.diskFailure && path.endsWith('.elf')) throw Error('Disk full');
      files.set(path, bytes.slice()); writes.push(path);
    },
    async processes() { return [...(helper || options.existingHelper ? [{pid: 100, name: 'websrv.elf'}] : []),
      ...(running || options.staleProcess ? [{pid: 200, name: 'transmission-da'}] : [])]; },
    async listening(port) { return port === 9091 ? running : port === 8080 ? helper || !!options.busy8080 : port === 8088 ? !!options.manager : true; },
    async sendElf(bytes) { assert.equal(bytes[0], 127); events.push('helper-start'); helper = true; },
    async stopHelper(pid) {
      assert.equal(pid, 100); events.push('helper-stop');
      if (options.cleanupFailure) throw Error('Cannot stop helper');
      helper = false;
      if (options.diesWithHelper) running = false;
    },
    async http(port, path, body, headers = {}) {
      if (port === 8088) return {status:200,body:JSON.stringify(options.manager)};
      if (port === 8080) {
        events.push('launch');
        const query = new URL('http://localhost' + path).searchParams;
        assert.equal(query.get('path'), app + '/transmission-daemon.elf');
        assert.equal(query.get('cwd'), STATE);
        assert.equal(query.get('pipe'), '0');
        assert.equal(query.get('daemon'), '1');
        assert.match(query.get('env'), new RegExp('TRANSMISSION_WEB_HOME=' + app + '/public_html'));
        assert.match(query.get('args'), /--config-dir .* --pid-file /);
        const launchCredentials = JSON.parse(new TextDecoder().decode(files.get(STATE + '/botty-credentials.json')));
        assert.ok(query.get('args').includes('--auth --username botty --password ' + launchCredentials.password));
        assert.ok(query.get('args').includes('--allowed 127.0.0.1,192.168.*.* --port 9091 --no-portmap'));
        assert.ok(query.get('args').includes('--download-dir ' + DOWNLOADS + '/complete --incomplete-dir ' + DOWNLOADS + '/incomplete'));
        assert.ok(query.get('args').includes('--logfile ' + STATE + '/transmission.log'));
        assert.ok(files.has(app + '/transmission-daemon.elf'));
        assert.ok(files.has(STATE + '/settings.json'));
        configured = true;
        if (options.launchFailure) return {status: 503};
        running = true;
        return {status: 200};
      }
      if (!running) throw Error('Service unavailable');
      if (path.endsWith('/web/')) return { status: options.missingUI ? 404 : 200, body: '<html lang="en">Transmission</html>' };
      if (!headers.Authorization) return { status: options.forbidden ? 403 : options.noAuth ? 200 : 401 };
      const creds = JSON.parse(new TextDecoder().decode(files.get(STATE + '/botty-credentials.json')));
      assert.equal(headers.Authorization, 'Basic ' + btoa(creds.username + ':' + creds.password));
      if (!headers['X-Transmission-Session-Id']) return {status: 409, headers: {'x-transmission-session-id': 'abc123'}};
      assert.equal(headers['X-Transmission-Session-Id'], 'abc123');
      const method = JSON.parse(body).method;
      if (method === 'session-close') { running = false; events.push('daemon-stop'); }
      return {status: 200, body: JSON.stringify({result: 'success', arguments: info})};
    },
  };
  const fetchFile = async url => {
    downloads.push(url);
    const data = new Uint8Array(await readFile(new URL('../vps-site/' + url.slice(2), import.meta.url)));
    if (options.corrupt && url.endsWith(options.corrupt)) data[0] ^= 1;
    return {ok: true, arrayBuffer: async () => data.buffer};
  };
  const opts = {fetchFile, wait: async () => {}, reveal: c => { revealed = c; }, report: m => events.push(m)};
  return {io, opts, files, writes, events, downloads, info, get revealed() { return revealed; }, get configured() {return configured;} };
}

test('fresh installation verifies files, configures protected isolated storage, launches and closes helper', async () => {
  const f = fixture({saved: false});
  const result = await installAndStart(f.io, f.opts);
  assert.equal(result.freeBytes, 100 * 1073741824);
  assert.equal(f.revealed.username, 'botty'); assert.match(f.revealed.password, /^[A-Za-z0-9]{6}$/);
  const settings = JSON.parse(new TextDecoder().decode(f.files.get(STATE + '/settings.json')));
  assert.equal(settings['rpc-password'], f.revealed.password);
  assert.equal(settings['rpc-authentication-required'], true);
  assert.equal(settings['port-forwarding-enabled'], false);
  assert.equal(settings['incomplete-dir-enabled'], true);
  assert.equal(settings['cache-size-mb'], 32);
  assert.ok(f.events.indexOf('helper-stop') > f.events.indexOf('launch'));
  assert.ok(f.writes.every(path => path.startsWith('/data/botty/')));
  assert.equal(f.writes.length, 10);
});
test('restart preserves credentials, settings and queue, skips valid installed assets', async () => {
  const f = fixture();
  for (const file of manifest.files) f.files.set(app + '/' + file.path, new Uint8Array(await readFile(new URL('../vps-site/apps/transmission/' + file.path, import.meta.url))));
  const resume = encode({sample: 'resume state'}); f.files.set(STATE + '/resume/sample.resume', resume);
  await installAndStart(f.io, f.opts);
  assert.equal(f.writes.length, 0);
  assert.equal(f.downloads.length, 2); // manifest + temporary helper only
  assert.equal(f.files.get(STATE + '/resume/sample.resume'), resume);
  assert.deepEqual(f.revealed, credentials);
});
test('already running instance is authenticated and reused without downloading or writing', async () => {
  const f = fixture({running: true}); await installAndStart(f.io, f.opts);
  assert.equal(f.writes.length, 0); assert.equal(f.downloads.length, 0); assert.equal(f.configured, false);
});

for (const cache of [undefined, 4, 8, 64]) test('restart persists 32 MiB cache from ' + cache + ' and preserves other settings', async () => {
  const f = fixture();
  const previous = {...settingsFor(credentials), 'speed-limit-down': 1234};
  if (cache === undefined) delete previous['cache-size-mb'];
  else previous['cache-size-mb'] = cache;
  const original = encode(previous);
  f.files.set(STATE + '/settings.json', original);
  const resume = encode({sample: 'resume state'});
  f.files.set(STATE + '/resume/sample.resume', resume);
  await installAndStart(f.io, f.opts);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(f.files.get(STATE + '/settings.json'))), {...previous, 'cache-size-mb': 32});
  assert.deepEqual(f.files.get(STATE + '/settings.before-cache-32.json'), original);
  assert.equal(f.files.get(STATE + '/resume/sample.resume'), resume);
});
test('unrelated port 9091 instance is not modified', async () => {
  const f = fixture({running: true, saved: false});
  await assert.rejects(installAndStart(f.io, f.opts), /already in use/); assert.equal(f.writes.length, 0);
});
for (const options of [{busy8080: true}, {existingHelper: true}]) test('existing launcher or port 8080 is left untouched: ' + JSON.stringify(options), async () => {
  const f = fixture(options); await assert.rejects(installAndStart(f.io, f.opts), /already uses/);
  assert.equal(f.writes.length, 0); assert.ok(!f.events.includes('helper-start'));
});
for (const corrupt of ['manifest.json', 'transmission-daemon.elf', 'websrv-ps5.elf']) test('corrupt download fails before all writes: ' + corrupt, async () => {
  const f = fixture({corrupt}); await assert.rejects(installAndStart(f.io, f.opts), /verification failed/);
  assert.equal(f.writes.length, 0); assert.ok(!f.events.includes('helper-start'));
});
test('disk full prevents daemon and helper launch', async () => {
  const f = fixture({diskFailure: true}); await assert.rejects(installAndStart(f.io, f.opts), /Disk full/);
  assert.ok(!f.events.includes('helper-start'));
});
test('damaged existing credentials are preserved, not reset', async () => {
  const f = fixture(); f.files.set(STATE + '/botty-credentials.json', new TextEncoder().encode('{'));
  await assert.rejects(installAndStart(f.io, f.opts), /damaged/); assert.equal(f.writes.length, 0);
});
test('saved live PID prevents a second daemon even if RPC is unresponsive', async () => {
  const f = fixture({staleProcess: true}); f.files.set(STATE + '/transmission.pid', new TextEncoder().encode('200'));
  await assert.rejects(installAndStart(f.io, f.opts), /still present/); assert.equal(f.writes.length, 0);
});
test('unsafe existing settings are preserved and block startup', async () => {
  const f = fixture(); const bad = encode({...settingsFor(credentials), 'rpc-authentication-required': false});
  f.files.set(STATE + '/settings.json', bad);
  await assert.rejects(installAndStart(f.io, f.opts), /settings differ/);
  assert.equal(f.files.get(STATE + '/settings.json'), bad); assert.ok(!f.events.includes('helper-start'));
});
for (const [options, error] of [[{launchFailure: true}, /rejected/], [{missingUI: true}, /web interface/], [{noAuth: true}, /password-protected/]])
  test('startup failure still cleans up helper: ' + JSON.stringify(options), async () => {
    const f = fixture(options); await assert.rejects(installAndStart(f.io, f.opts), error);
    assert.ok(f.events.includes('helper-stop'));
  });
test('cleanup failure cannot be reported as running successfully', async () => {
  const f = fixture({cleanupFailure: true}); await assert.rejects(installAndStart(f.io, f.opts), /Cannot stop helper/);
});
test('daemon must survive helper shutdown', async () => {
  const f = fixture({diesWithHelper: true}); await assert.rejects(installAndStart(f.io, f.opts), /unavailable/);
});
test('graceful stop authenticates and keeps stored files', async () => {
  const f = fixture({running: true}); await stopTransmission(f.io, async () => {});
  assert.ok(f.events.includes('daemon-stop')); assert.equal(f.writes.length, 0); assert.equal(f.files.size, 3);
});
test('SHA-256 known vector', async () => assert.equal(await sha256(new TextEncoder().encode('abc')), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'));
test('filesystem rejects traversal and writes outside dedicated storage', () => {
  for (const path of ['/user/app/file', '/data/botty/../bad', '/data/botty/a/./b', '/data/botty/file\0']) assert.throws(() => checkedPath(path));
  assert.equal(checkedPath(STATE + '/settings.json'), STATE + '/settings.json');
});
test('HTTP parser handles split UTF-8 across chunk boundaries and rejects truncation', () => {
  const bytes = Buffer.concat([Buffer.from('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\n'), Buffer.from([0xc3]), Buffer.from('\r\n1\r\n'), Buffer.from([0xa9]), Buffer.from('\r\n0\r\n\r\n')]);
  assert.equal(parseHttp(bytes).body, 'é');
  assert.throws(() => parseHttp('HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc'), /Truncated/);
  assert.throws(() => parseHttp('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabc'), /Truncated/);
});

function syscallFixture({writeError = false, fsyncError = false} = {}) {
  const events = [], written = [];
  const runtime = {p: {malloc(size) {const backing = new Uint8Array(size + 1000); return {backing, offset: 0, add32(offset) {return {backing, offset};}};}}, chain: {async syscall(number, ...args) {
    events.push({number, args});
    if (number === 5) return {low: 7};
    if (number === 4) {
      if (writeError) return {low: -1};
      const length = Math.min(7000, args[2]); written.push(args[1].backing.slice(args[1].offset, args[1].offset + length)); return {low: length};
    }
    if (number === 95 && fsyncError) return {low: -1};
    return {low: 0};
  }}};
  return {io: new PS5IO(runtime), events, written};
}
test('PS5 file adapter completes short writes, fsyncs, closes then renames', async () => {
  const f = syscallFixture(); const bytes = new Uint8Array(160001).map((_, i) => i % 251);
  await f.io.writeFile(STATE + '/file', bytes);
  assert.deepEqual(Buffer.concat(f.written), Buffer.from(bytes));
  assert.deepEqual(f.events.slice(-3).map(e => e.number), [95, 6, 128]);
  const open = f.events[0]; assert.equal(open.args[2], 0o600); assert.ok(open.args[1] & 0x100);
});
for (const failure of [{writeError: true}, {fsyncError: true}]) test('PS5 failed writes close fd without committing: ' + JSON.stringify(failure), async () => {
  const f = syscallFixture(failure); await assert.rejects(f.io.writeFile(STATE + '/file', new Uint8Array(20)));
  assert.equal(f.events.at(-1).number, 6); assert.ok(!f.events.some(e => e.number === 128));
});
test('PS5 credentials creation uses exclusive open, never truncates an existing credential file', async () => {
  const f = syscallFixture(); await f.io.writeFile(STATE + '/secret', new Uint8Array(10), true);
  assert.ok(f.events[0].args[1] & 0x800); assert.equal(f.events[0].args[1] & 0x400, 0);
  assert.ok(!f.events.some(e => e.number === 128));
});

test('safe stop waits for process exit after RPC port closes', async () => {
  const f = fixture({running: true}); let checks = 0; let waits = 0;
  f.io.processes = async () => ++checks < 3 ? [{pid: 200, name: 'transmission-da'}] : [];
  await stopTransmission(f.io, async () => { waits++; });
  assert.equal(waits, 2);
});
test('safe stop does not claim success when process stays alive', async () => {
  const f = fixture({running: true}); f.io.processes = async () => [{pid: 200, name: 'transmission-da'}];
  await assert.rejects(stopTransmission(f.io, async () => {}), /still stopping/);
});
test('safe stop refuses an unverifiable PID', async () => {
  const f = fixture({running: true}); f.files.delete(STATE + '/transmission.pid');
  await assert.rejects(stopTransmission(f.io, async () => {}), /process ID/);
  assert.ok(!f.events.includes('daemon-stop'));
});
test('helper cleanup never signals a reused PID with another process name', async () => {
  const f = syscallFixture(); f.io.processes = async () => [{pid: 100, name: 'unrelated-app'}];
  await f.io.stopHelper(100);
  assert.equal(f.events.length, 0);
});
test('PS5 process table rejects invalid record sizes before walking arbitrary memory', async () => {
  const f = syscallFixture();
  f.io.call = async (name, mib, count, target, size) => {
    assert.equal(name, 'sysctl');
    new DataView(size.backing.buffer).setUint32(0, 1000, true);
    if (target) new DataView(target.backing.buffer).setInt32(0, 0x7fffffff, true);
    return 0;
  };
  await assert.rejects(f.io.processes(), /Unknown PS5 process table layout/);
});
test('retrying safe stop waits for a daemon already shutting down with its port closed', async () => {
  const f = fixture(); let checks = 0, waits = 0;
  f.files.set(STATE + '/transmission.pid', new TextEncoder().encode('200'));
  f.io.processes = async () => ++checks < 3 ? [{pid: 200, name: 'transmission-da'}] : [];
  await stopTransmission(f.io, async () => { waits++; });
  assert.equal(waits, 2); assert.ok(!f.events.includes('daemon-stop'));
});

test('blocked RPC reports HTTP 403 distinctly and preserves a running installation', async () => {
  const f = fixture({running: true, forbidden: true});
  await assert.rejects(installAndStart(f.io, f.opts), /HTTP 403.*IP allowlist or login protection/);
  assert.equal(f.writes.length, 0);
  assert.equal(f.downloads.length, 0);
});


test('short password is six unambiguous alphanumeric characters', () => {
  for(let i=0;i<100;++i) assert.match(shortPassword(), /^[A-HJ-NP-Za-km-z2-9]{6}$/);
});
test('legacy password is shortened only while stopped; unrelated settings and resume data survive', async () => {
  const f=fixture();
  const settings={...settingsFor(legacyCredentials), 'speed-limit-down': 1234};
  f.files.set(STATE+'/botty-credentials.json',encode(legacyCredentials));
  f.files.set(STATE+'/settings.json',encode(settings));
  f.files.set(STATE+'/resume/test.resume',encode({piece: 123}));
  const result=await installAndStart(f.io,f.opts);
  assert.match(result.credentials.password,/^[A-Za-z0-9]{6}$/);
  assert.equal(JSON.parse(new TextDecoder().decode(f.files.get(STATE+'/settings.json')))['speed-limit-down'],1234);
  assert.equal(JSON.parse(new TextDecoder().decode(f.files.get(STATE+'/botty-credentials.before-native.json'))).password,legacyCredentials.password);
  assert.deepEqual(f.files.get(STATE+'/resume/test.resume'),encode({piece:123}));
});
test('running legacy instance keeps its real password without writes or restart', async () => {
  const f=fixture({running:true});
  f.files.set(STATE+'/botty-credentials.json',encode(legacyCredentials));
  f.files.set(STATE+'/settings.json',encode(settingsFor(legacyCredentials)));
  const result=await installAndStart(f.io,f.opts);
  assert.equal(result.credentials.password,legacyCredentials.password);assert.equal(f.writes.length,0);
  assert.ok(!f.events.includes('daemon-stop'));
});
for(const interruptedPath of ['password-migration.json','settings.json','botty-credentials.json'])
  test('password migration resumes after interruption writing '+interruptedPath, async () => {
    const f=fixture();const settings=settingsFor(legacyCredentials);
    f.files.set(STATE+'/botty-credentials.json',encode(legacyCredentials));
    f.files.set(STATE+'/settings.json',encode(settings));
    const write=f.io.writeFile.bind(f.io);let fail=true;
    f.io.writeFile=async(path,bytes,exclusive)=>{
      await write(path,bytes,exclusive);
      if(fail && path===STATE+'/'+interruptedPath){fail=false;throw Error('Interrupted');}
    };
    await assert.rejects(prepareShortPassword(f.io,legacyCredentials,settings),/Interrupted/);
    const pending=JSON.parse(new TextDecoder().decode(f.files.get(STATE+'/password-migration.json')));
    const current=JSON.parse(new TextDecoder().decode(f.files.get(STATE+'/botty-credentials.json')));
    const savedSettings=JSON.parse(new TextDecoder().decode(f.files.get(STATE+'/settings.json')));
    const recovered=await prepareShortPassword(f.io,current,savedSettings);
    assert.equal(recovered.password,pending.password);
    assert.equal(JSON.parse(new TextDecoder().decode(f.files.get(STATE+'/settings.json')))['rpc-password'],pending.password);
    assert.equal(JSON.parse(new TextDecoder().decode(f.files.get(STATE+'/password-migration.json'))).status,'complete');
  });
test('migration refuses a live Transmission process even when RPC is unavailable', async () => {
  const f=fixture({staleProcess:true});
  await assert.rejects(prepareShortPassword(f.io,legacyCredentials,settingsFor(legacyCredentials)),/stop/);
  assert.equal(f.writes.length,0);
});

test('legacy Botty manager keeps long credentials until a new session', async () => {
  const f=fixture({manager:{app:'Botty',version:'0.1.0'}});
  const value=await prepareShortPassword(f.io,legacyCredentials,settingsFor(legacyCredentials));
  assert.equal(value.password,legacyCredentials.password);assert.equal(f.writes.length,0);
});

test('partial password backup blocks migration without overwriting current credentials', async () => {
 const f=fixture();f.files.set(STATE+'/botty-credentials.before-native.json',new TextEncoder().encode('{'));
 await assert.rejects(prepareShortPassword(f.io,legacyCredentials,settingsFor(legacyCredentials)),/damaged/);
 assert.equal(f.writes.length,0);
});

test('numeric credentials are invalid and never silently converted or overwritten', async () => {
 const f=fixture();f.files.set(STATE+'/botty-credentials.json',encode({username:'botty',password:123456}));
 await assert.rejects(installAndStart(f.io,f.opts),/Invalid saved credentials/);assert.equal(f.writes.length,0);
});
