import { PS5IO, sleep } from './ps5-io.js';
import { sha256 } from './transmission.js';
import { installCodex, NATIVE, SERVICE, UPDATE, nativeIdentity } from './codex-install.js';
import { PAYLOAD } from './codex-payload.js';
const PATH = '/data/codex-ps5/payloads/assistant-service/assistant-service.elf';

export class CodexIO extends PS5IO {
  checkedPath(path) {
    const roots = [NATIVE, SERVICE, UPDATE, '/data/codex-ps5/backups'];
    const metadata = ['/user/app/PPSA99105/sce_sys', '/user/appmeta/PPSA99105', '/system_data/priv/appmeta/PPSA99105'];
    if (typeof path === 'string' && /^[/a-zA-Z0-9._-]+$/.test(path) && !path.split('/').some(x => x === '..' || x === '.') &&
        (roots.some(root => path === root || path.startsWith(root + '/')) || ['/data/homebrew/check', '/data/codex-ps5/payloads/check', '/data/ps5-ai-cli/check'].includes(path) ||
         metadata.some(root => ['param.json', 'icon0.png'].some(file => path === root + '/' + file)))) return path;
    throw Error('Unexpected Codex installer path.');
  }
  async ensureHomebrew() {
    await this.call('mkdir', this.string('/data/homebrew'), 0o755);
    if (!await this.directoryExists('/data/homebrew')) throw Error('Cannot access homebrew directory.');
    if (((await this.runtime.chain.syscall(15, this.string('/data/homebrew'), 0o755)).low | 0)) throw Error('Homebrew permissions failed.');
  }
  async directoryExists(path) {
    this.checkedPath(path + '/check');
    const fd = await this.call('open', this.string(path), 0x20000 | 0x100, 0);
    if (fd < 0) return false;
    await this.close(fd); return true;
  }
  async assertNativeStopped() {
    if ((await this.processes()).some(p => /^(eboot(?:\.bin)?|codex.*)$/i.test(p.name)))
      throw Error('Close Codex PS5 and other native apps before updating, then relaunch the portal.');
  }
  async syncDirectory(path) {
    const fd = await this.call('open', this.string(path), 0x20000 | 0x100, 0);
    if (fd < 0) throw Error('Cannot access Codex update directory.');
    try { if (await this.call('fsync', fd)) throw Error('Cannot flush Codex update directory.'); }
    finally { await this.close(fd); }
  }
  async removeEmptyDirectory(path) {
    this.checkedPath(path + '/check');
    return ((await this.runtime.chain.syscall(137, this.string(path))).low | 0) === 0;
  }
  async moveDirectory(source, destination) {
    this.checkedPath(source + '/check'); this.checkedPath(destination + '/check');
    if (await this.call('mkdir', this.string(destination), 0o755)) throw Error('Codex update destination exists; files preserved.');
    if (await this.call('rename', this.string(source), this.string(destination, this.otherPath))) {
      await this.removeEmptyDirectory(destination); throw Error('Codex publication interrupted; relaunch to recover.');
    }
    await this.syncDirectory(source.slice(0, source.lastIndexOf('/')));
    await this.syncDirectory(destination.slice(0, destination.lastIndexOf('/')));
  }
  async readBlocks(path, consume) {
    this.checkedPath(path);
    const fd = await this.call('open', this.string(path), 0x100, 0);
    if (fd < 0) return false;
    let total = 0, index = 0;
    try {
      for (;;) {
        const block = new Uint8Array(1048576); let size = 0;
        while (size < block.length) {
          const n = await this.call('read', fd, this.buffer, Math.min(65536, block.length - size));
          if (n < 0 || n > Math.min(65536, block.length - size)) throw Error('Codex file read failed.');
          if (!n) break;
          block.set(this.buffer.backing.subarray(0, n), size); size += n;
        }
        if (!size) break;
        total += size;
        if (total > 256 * 1024 * 1024) throw Error('Codex file exceeds allowed size.');
        await consume(block.subarray(0, size), index++);
        if (size < block.length) break;
      }
      return total;
    } finally { await this.close(fd); }
  }
  async matchesFile(path, file, digest = sha256) {
    let count = 0, valid = true;
    const size = await this.readBlocks(path, async (block, index) => {
      count++; if (index >= file.chunks.length || await digest(block) !== file.chunks[index]) valid = false;
    });
    return valid && size === file.size && count === file.chunks.length;
  }
  async fingerprint(path, digest = sha256) {
    const hashes = [];
    const size = await this.readBlocks(path, async b => hashes.push(await digest(b)));
    return size === false ? null : digest(new TextEncoder().encode(JSON.stringify({size, hashes})));
  }
  async seedBlocks(path, file, digest = sha256) {
    await this.mkdirs(UPDATE + '/blocks');
    await this.readBlocks(path, async (block, index) => {
      const hash = file.chunks[index];
      if (hash && await digest(block) === hash) {
        const target = UPDATE + '/blocks/' + hash + '.bin';
        const existing = await this.readFile(target, 1048576);
        if (!existing || await digest(existing) !== hash) await this.writeFile(target, block);
      }
    });
  }
  async assembleFile(path, file, getBlock) {
    await this.mkdirs(path.slice(0, path.lastIndexOf('/')));
    const temporary = path + '.part';
    const fd = await this.call('open', this.string(temporary), 1 | 0x200 | 0x100 | 0x400, 0o600);
    if (fd < 0) throw Error('Cannot stage Codex file.');
    try {
      for (let i = 0; i < file.chunks.length; i++) await this.writeAll(fd, await getBlock(file.chunks[i], Math.min(1048576, file.size - i * 1048576)));
      if (await this.call('fsync', fd)) throw Error('Cannot flush Codex file.');
    } finally { await this.close(fd); }
    if (await this.call('rename', this.string(temporary), this.string(path, this.otherPath))) throw Error('Cannot commit Codex staged file.');
  }
  async preparePermissions(root, files) {
    const directories = new Set([root]);
    for (const f of files) directories.add((root + '/' + f.path).slice(0, (root + '/' + f.path).lastIndexOf('/')));
    for (const path of directories) if (((await this.runtime.chain.syscall(15, this.string(path), 0o755)).low | 0)) throw Error('Codex directory permissions failed.');
    for (const f of files) {
      const executable = ['eboot.bin', 'sce_module/libc.prx', 'assistant-service.elf'].includes(f.path);
      if (((await this.runtime.chain.syscall(15, this.string(root + '/' + f.path), executable ? 0o755 : 0o644)).low | 0)) throw Error('Codex file permissions failed.');
    }
    for (const path of directories) await this.syncDirectory(path);
  }
  async syncCodexMetadata(backup, digest = sha256) {
    let index = 0;
    for (const root of ['/user/app/PPSA99105/sce_sys', '/user/appmeta/PPSA99105', '/system_data/priv/appmeta/PPSA99105']) {
      const param = await this.readFile(root + '/param.json', 16384);
      if (!param) continue;
      nativeIdentity(param);
      for (const file of ['param.json', 'icon0.png']) {
        const path = root + '/' + file, saved = backup + '/metadata/' + index++ + '.bin';
        const previous = await this.readFile(path, 4 * 1024 * 1024), data = await this.readFile(NATIVE + '/sce_sys/' + file, 4 * 1024 * 1024);
        if (!data) throw Error('Codex metadata source missing.');
        if (previous && await digest(previous) === await digest(data)) continue;
        if (previous && !await this.readFile(saved, 4 * 1024 * 1024)) { await this.mkdirs(backup + '/metadata'); await this.writeFile(saved, previous, true); }
        await this.writeFile(path, data);
        if (((await this.runtime.chain.syscall(15, this.string(path), 0o644)).low | 0)) throw Error('Codex metadata permissions failed.');
        const disk = await this.readFile(path, 4 * 1024 * 1024);
        if (!disk || await digest(disk) !== await digest(data)) throw Error('Codex metadata verification failed.');
      }
      await this.syncDirectory(root);
    }
  }
  async visitPayload(consume, digest = sha256) {
    this.checkedPath(PATH);
    const fd = await this.call('open', this.string(PATH), 0x100, 0);
    if (fd < 0) throw Error('Codex service installation is missing.');
    try {
      let total = 0;
      for (const expected of PAYLOAD.chunks) {
        const size = Math.min(PAYLOAD.chunkSize, PAYLOAD.size - total);
        const block = new Uint8Array(size); let offset = 0;
        while (offset < size) {
          const n = await this.call('read', fd, this.buffer, Math.min(65536, size - offset));
          if (n <= 0 || n > Math.min(65536, size - offset)) throw Error('Codex payload read interrupted.');
          block.set(this.buffer.backing.subarray(0, n), offset); offset += n;
        }
        if (await digest(block) !== expected) throw Error('Installed Codex payload differs from this portal build; relaunch the portal to update it.');
        if (total === 0 && (block[0] !== 127 || block[1] !== 69 || block[2] !== 76 || block[3] !== 70)) throw Error('Invalid Codex ELF.');
        if (consume) await consume(block);
        total += size;
      }
      if (total !== PAYLOAD.size || await this.call('read', fd, this.buffer, 1) !== 0) throw Error('Unexpected Codex payload size.');
    } finally { await this.close(fd); }
  }
  async deliverPayload() {
    const fd = await this.connect(9021);
    if (fd < 0) throw Error('ELF loader unavailable.');
    // Check each block again during delivery; never resend after an uncertain transfer.
    try { await this.visitPayload(block => this.writeAll(fd, block)); }
    finally { await this.close(fd); }
  }
}
async function busy(io) {
  const boot = await io.http(8088, '/api/bootstrap');
  if (boot.status !== 200) throw Error('Cannot check Botty work state.');
  const token = JSON.parse(boot.body).token;
  if (!/^[a-f0-9]{32}$/.test(token)) throw Error('Cannot check Botty work state.');
  const response = await io.http(8088, '/api/processing', null, { 'X-Botty-Token': token });
  const tasks = response.status === 200 ? JSON.parse(response.body).tasks : null;
  if (!Array.isArray(tasks)) throw Error('Cannot check Botty work state.');
  return tasks.some(task => task.busy === true || ['running', 'queued', 'preparing', 'compressing', 'extracting', 'transferring', 'deleting'].includes(task.status));
}
export async function startCodex(io, options = {}) {
  const report = options.report || (() => {}), wait = options.wait || sleep;
  if (await busy(io)) return { ready: false, reason: 'Codex update deferred while Botty is processing files.' };
  const install = options.install || installCodex;
  const installed = await install(io, {...options, beforePublish: async () => {
    if (await busy(io)) throw Error('Codex update deferred: Botty started processing files.');
  }});
  if (await busy(io)) return {ready: false, reason: 'Codex startup deferred while Botty is processing files.'};
  if (await io.listening(49322)) {
    let status;
    try { const r = await io.http(49323, '/status'); if (r.status === 200) status = JSON.parse(r.body); } catch (_) {}
    if (status?.service === 'codex-ps5' && status.build === installed.serviceBuild) return {ready: true, reused: true, version: installed.version};
    if (!status && await io.listening(49323)) return {ready: true, updatePending: true, reason: 'Codex engine is busy. Close Codex PS5, wait for its conversation to end, then run LAUNCH again.'};
    if (!status || status.service !== 'codex-ps5' || status.idle !== true || !Number.isInteger(status.pid))
      return {ready: true, updatePending: true, reason: 'Codex files updated. Fully restart the PS5 and run LAUNCH with Codex checked to activate the new engine.'};
    await io.assertNativeStopped();
    report('Stopping the previous Codex engine…');
    const stopped = await io.http(49323, '/stop', '');
    const confirmation = JSON.parse(stopped.body);
    if (stopped.status !== 200 || confirmation.build !== status.build || confirmation.pid !== status.pid || confirmation.service !== 'codex-ps5') throw Error('Codex stop was not confirmed. No payload sent.');
    let closed = false;
    for (let i = 0; i < 40; i++) {
      if (!await io.listening(49322) && !await io.listening(49323)) {closed = true; break;}
      await wait(250);
    }
    if (!closed) throw Error('Previous Codex engine is still stopping. No payload sent.');
  }
  report('Verifying Codex PS5 ' + installed.version + '…');
  await io.visitPayload();
  if (await busy(io)) return {ready: false, reason: 'Codex startup deferred while Botty is processing files.'};
  if (await io.listening(49322)) throw Error('Another Codex engine started during update. No payload sent.');
  report('Starting Codex PS5…');
  await io.deliverPayload();
  for (let attempt = 0; attempt < 40; attempt++) {
    if (await io.listening(49322)) return { ready: true, version: installed.version };
    await wait(500);
  }
  throw Error('The service did not become ready. No second payload was sent.');
}
export function codexStatus(result) {
  if (result?.updatePending) return result.reason;
  return result?.ready ? 'Codex ' + (result.version || 'PS5') + ' is ready. Open it in the game library; L1 connects ChatGPT, Triangle dictates.' : result?.reason || 'Codex PS5 is unavailable.';
}
