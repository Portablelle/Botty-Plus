import { PS5IO, checkedPath, sleep } from './ps5-io.js';
import { sha256 } from './transmission.js';

export const VERSION = '0.17.2-botty.1';
const HASH = '161b9d3111a77f3c772d57940bd03fb3f90a9f931bc77386eb85f4c2088d9807';
const BASE = './apps/cheatrunner/';
const ROOT = '/data/botty/cheatrunner';
const CONFIG = '/data/cheatrunner/config.ini';
const TILE = '/user/appmeta/CHTR09999';
const encoder = new TextEncoder();
const decoder = new TextDecoder();

export class CheatRunnerIO extends PS5IO {
  checkedPath(path) {
    if (path === CONFIG || path === CONFIG + '.part' || path === '/data/cheatrunner/check') return path;
    return checkedPath(path);
  }
  async tileRegistered() {
    // Read-only, exact title directory. Never modify the application database.
    const fd = await this.call('open', this.string(TILE), 0x20000 | 0x100, 0);
    if (fd < 0) return false;
    await this.close(fd);
    return true;
  }
}

async function json(io, port, path, headers) {
  const response = await io.http(port, path, null, headers);
  if (response.status !== 200) throw Error('Service check failed: ' + path);
  return JSON.parse(response.body);
}

async function health(io) {
  const data = await json(io, 9999, '/api/health');
  const config = await json(io, 9999, '/api/config');
  if (data.ok !== true || !/^\d+\.\d+(?:\.\d+)?(?:-botty\.\d+)?$/.test(data.version) ||
      !data.busy || !data.http || config.ok !== true || config.http_port !== 9999 ||
      typeof config.tile_autoinstall_enabled !== 'number' || typeof config.hotkey_enabled !== 'number')
    throw Error('Port 9999 is used by an unexpected service.');
  return { version: data.version, hotkeyEnabled: config.hotkey_enabled !== 0 };
}

async function workBusy(io) {
  const bootstrap = await json(io, 8088, '/api/bootstrap');
  if (typeof bootstrap.token !== 'string' || !/^[a-f0-9]{32}$/.test(bootstrap.token))
    throw Error('Cannot verify Botty work state.');
  const state = await json(io, 8088, '/api/state', { 'X-Botty-Token': bootstrap.token });
  if (typeof state.extracting !== 'boolean' || typeof state.compression?.busy !== 'boolean')
    throw Error('Cannot verify Botty work state.');
  return state.extracting || state.compression.busy;
}

export function prepareConfig(text) {
  // Replace all occurrences because the upstream INI parser accepts repeated keys.
  const values = { http_port: 9999, tile_autoinstall_enabled: 1, hotkey_enabled: 0 };
  const lines = text.split(/\r?\n/).filter(line => !/^\s*(http_port|tile_autoinstall_enabled|hotkey_enabled)\s*=/.test(line));
  while (lines.length && !lines[lines.length - 1]) lines.pop();
  return [...lines, ...Object.entries(values).map(([key, value]) => key + '=' + value), ''].join('\n');
}

export async function installAndStartCheatRunner(io, options = {}) {
  const fetchFile = options.fetchFile || fetch, digest = options.digest || sha256;
  const wait = options.wait || sleep, report = options.report || (() => {});
  if (await io.listening(9999)) {
    const active = await health(io);
    return { ...active, ready: true, reused: true, tileRegistered: await io.tileRegistered(), updatePending: active.version !== VERSION };
  }
  if ((await io.processes()).some(p => /^CheatRunner(?:\.elf)?$/i.test(p.name)))
    throw Error('CheatRunner is already running but not responding on port 9999; it was preserved.');
  if (await workBusy(io)) return { ready: false, deferred: true, reason: 'Active extraction, transfer or compression; CheatRunner starts on a later idle session.' };

  report('Verifying CheatRunner and its home-screen package…');
  const response = await fetchFile(BASE + 'manifest.json', { cache: 'no-store' });
  if (!response.ok) throw Error('CheatRunner manifest unavailable.');
  const raw = new Uint8Array(await response.arrayBuffer());
  if (await digest(raw) !== HASH) throw Error('CheatRunner manifest verification failed.');
  const manifest = JSON.parse(decoder.decode(raw));
  const file = manifest.files?.[0];
  if (manifest.schema !== 1 || manifest.app !== 'CheatRunner' || manifest.version !== VERSION ||
      manifest.files.length !== 1 || file.path !== 'CheatRunner.elf' ||
      !Number.isSafeInteger(file.size) || file.size < 4096 || file.size > 16 * 1024 * 1024 || !/^[a-f0-9]{64}$/.test(file.sha256))
    throw Error('Unexpected CheatRunner package.');
  const path = ROOT + '/releases/' + VERSION + '/' + file.path;
  let elf = await io.readFile(path, 16 * 1024 * 1024);
  if (!elf || elf.length !== file.size || await digest(elf) !== file.sha256) {
    const result = await fetchFile(BASE + file.path, { cache: 'no-store' });
    if (!result.ok) throw Error('CheatRunner download failed.');
    elf = new Uint8Array(await result.arrayBuffer());
    if (elf.length !== file.size || await digest(elf) !== file.sha256) throw Error('CheatRunner ELF verification failed.');
    await io.mkdirs(path.slice(0, path.lastIndexOf('/')));
    await io.writeFile(path, elf);
    const disk = await io.readFile(path, file.size);
    if (!disk || await digest(disk) !== file.sha256) throw Error('CheatRunner disk verification failed.');
  }
  // Downloads may take time. Recheck immediately before config writes and delivery.
  if (await workBusy(io)) return { ready: false, deferred: true, reason: 'Active work started; CheatRunner is staged for a later idle session.' };
  if (await io.listening(9999) || (await io.processes()).some(p => /^CheatRunner(?:\.elf)?$/i.test(p.name)))
    throw Error('CheatRunner started during setup; duplicate delivery was refused.');
  const previous = await io.readFile(CONFIG, 65536);
  const config = encoder.encode(prepareConfig(previous ? decoder.decode(previous) : ''));
  if (!previous || await digest(previous) !== await digest(config)) {
    if (previous) {
      const backup = ROOT + '/config-backups/' + await digest(previous) + '.ini';
      await io.mkdirs(ROOT + '/config-backups');
      const existing = await io.readFile(backup, 65536);
      if (!existing) await io.writeFile(backup, previous, true);
      const saved = await io.readFile(backup, 65536);
      if (!saved || await digest(saved) !== await digest(previous)) throw Error('CheatRunner config backup verification failed.');
    }
    await io.mkdirs('/data/cheatrunner');
    await io.writeFile(CONFIG, config, !previous);
    const saved = await io.readFile(CONFIG, 65536);
    if (!saved || await digest(saved) !== await digest(config)) throw Error('CheatRunner config verification failed.');
  }
  report('Starting CheatRunner…');
  await io.sendElf(elf);
  let active;
  for (let attempt = 0; attempt < 60; attempt++) {
    if (await io.listening(9999)) {
      try { active = await health(io); break; } catch (error) { if (attempt === 59) throw error; }
    }
    await wait(250);
  }
  if (!active) throw Error('CheatRunner did not become ready; no duplicate payload was sent.');
  if (active.version !== VERSION) throw Error('Unexpected CheatRunner version after startup.');
  report('CheatRunner is ready. Waiting for its Media home-screen tile…');
  let registered = false;
  for (let attempt = 0; attempt < 60; attempt++) {
    if (await io.tileRegistered()) { registered = true; break; }
    await wait(500);
  }
  return { ...active, ready: true, tileRegistered: registered, updatePending: false };
}

export function cheatRunnerStatus(result) {
  if (!result?.ready) return result?.reason || 'CheatRunner is unavailable; Botty is ready.';
  let text = result.tileRegistered ? 'CheatRunner is ready in Media on the home screen.' : 'CheatRunner is online; its Media tile registration is not confirmed.';
  if (result.updatePending) text += ' The running version was preserved; update on the next console session.';
  if (result.hotkeyEnabled) text += ' The existing ShellUI hotkey setting is enabled; it was not changed.';
  return text;
}
