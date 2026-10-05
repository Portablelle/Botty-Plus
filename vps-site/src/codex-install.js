import { sha256 } from './transmission.js';
export const NATIVE = '/data/homebrew/PPSA99105';
export const SERVICE = '/data/codex-ps5/payloads/assistant-service';
export const UPDATE = '/data/codex-ps5/installer';
const HASH = '9265bcdcdbf76dc9498767173a4e513ad62705c0d8418792920fa4a8cb835681';
const FILES = ['assets/ggml-base.bin', 'assets/ui-font.bin', 'eboot.bin', 'sce_module/libc.prx', 'sce_sys/icon0.png', 'sce_sys/param.json'];
const encoder = new TextEncoder(), decoder = new TextDecoder();
const hex = x => /^[a-f0-9]{64}$/.test(x);
export function validatePackage(m) {
  if (m.schema !== 1 || m.titleId !== 'PPSA99105' || !/^\d+\.\d+\.\d+$/.test(m.version) || !hex(m.serviceBuild) || m.chunkSize !== 1048576 ||
      !Array.isArray(m.native) || !Array.isArray(m.service) || m.native.length !== FILES.length || m.service.length !== 1 ||
      new Set(m.native.map(f => f.path)).size !== FILES.length || m.native.some(f => !FILES.includes(f.path)) || m.service[0].path !== 'assistant-service.elf')
    throw Error('Unexpected Codex package.');
  for (const f of [...m.native, ...m.service]) {
    if (!Number.isSafeInteger(f.size) || f.size < 1 || f.size > 256 * 1024 * 1024 || !hex(f.sha256) ||
        !Array.isArray(f.chunks) || f.chunks.length !== Math.ceil(f.size / m.chunkSize) || f.chunks.some(h => !hex(h))) throw Error('Invalid Codex file.');
  }
  return m;
}
export function nativeIdentity(bytes) {
  let p; try { p = JSON.parse(decoder.decode(bytes)); } catch (_) {}
  if (p?.titleId !== 'PPSA99105' || p.contentId !== 'UP9000-PPSA99105_00-CODEXPS500000001' ||
      !/^\d{2}\.\d{3}\.\d{3}$/.test(p.contentVersion) || !['Codex PS5', 'Codex PS5 - Prototype'].includes(p.localizedParameters?.['en-US']?.titleName))
    throw Error('Unrecognized Codex title. Existing files preserved.');
  return p;
}
const RECEIPT = UPDATE + '/verified.json';
function packageFiles(m) {
  return [...m.native.map(f => ({...f, absolute: NATIVE + '/' + f.path})),
    ...m.service.map(f => ({...f, absolute: SERVICE + '/' + f.path}))];
}
function sameStamp(a, b) {
  return !!a && !!b && a.size === b.size && typeof a.stamp === 'string' && /^[a-f0-9]{176}$/.test(a.stamp) && a.stamp === b.stamp;
}
async function installedMatches(io, m, digest, report) {
  // An unavailable/unknown stat ABI falls back to full verification, never to trust.
  let receipt;
  try { const bytes = await io.readFile(RECEIPT, 16384); if (bytes) receipt = JSON.parse(decoder.decode(bytes)); } catch (_) {}
  const cached = receipt?.schema === 1 && receipt.target === HASH && receipt.files && typeof receipt.files === 'object';
  const entries = {};
  let reusable = typeof io.fileStamp === 'function', changed = false;
  for (const file of packageFiles(m)) {
    const before = io.fileStamp ? await io.fileStamp(file.absolute) : null;
    if (before?.size === file.size && cached && sameStamp(before, receipt.files[file.absolute])) {
      entries[file.absolute] = before; continue;
    }
    report('Verifying Codex ' + file.path + '…');
    if (!await io.matchesFile(file.absolute, file, digest)) return false;
    const after = io.fileStamp ? await io.fileStamp(file.absolute) : null;
    if (before && after && !sameStamp(before, after)) throw Error('Codex file changed during verification. Relaunch the portal.');
    if (!sameStamp(before, after) || after?.size !== file.size) reusable = false;
    else entries[file.absolute] = after;
    changed = true;
  }
  if (reusable && (changed || !cached)) {
    await io.writeFile(RECEIPT, encoder.encode(JSON.stringify({schema: 1, target: HASH, files: entries})));
    await io.syncDirectory(UPDATE);
  }
  return true;
}
async function recordVerified(io, m) {
  // Publication already hashed each file. Capture metadata only after this succeeds.
  if (!io.fileStamp) return;
  const files = {};
  for (const file of packageFiles(m)) {
    const stamp = await io.fileStamp(file.absolute);
    if (!stamp || stamp.size !== file.size) return;
    files[file.absolute] = stamp;
  }
  await io.writeFile(RECEIPT, encoder.encode(JSON.stringify({schema: 1, target: HASH, files})));
  await io.syncDirectory(UPDATE);
}
async function matches(io, root, files, digest) {
  for (const f of files) if (!await io.matchesFile(root + '/' + f.path, f, digest)) return false;
  return true;
}
async function save(io, journal) {
  await io.writeFile(UPDATE + '/journal.json', encoder.encode(JSON.stringify(journal)));
  await io.syncDirectory(UPDATE);
}
async function recover(io, j, digest, report) {
  if (j.status !== 'pending') return;
  await io.assertNativeStopped();
  validatePackage(j.manifest);
  for (const c of j.components) {
    if (await matches(io, c.live, j.manifest[c.name], digest)) continue;
    if (!await matches(io, c.stage, j.manifest[c.name], digest)) throw Error('Codex staging unavailable. Backup retained.');
    if (await io.directoryExists(c.live) && await io.fingerprint(c.live + '/' + c.marker, digest) === null) await io.removeEmptyDirectory(c.live);
    if (await io.directoryExists(c.live)) {
      const fingerprint = await io.fingerprint(c.live + '/' + c.marker, digest);
      if (!c.previous || fingerprint !== c.previous || await io.directoryExists(c.backup))
        throw Error('Codex recovery found unexpected files. Existing files preserved.');
      await io.moveDirectory(c.live, c.backup);
    } else if (c.previous && await io.fingerprint(c.backup + '/' + c.marker, digest) !== c.previous) {
      throw Error('Codex recovery backup unavailable. Existing files preserved.');
    }
    await io.moveDirectory(c.stage, c.live);
    if (!await matches(io, c.live, j.manifest[c.name], digest)) throw Error('Codex publication verification failed.');
  }
  await io.syncCodexMetadata(j.backup, digest);
  await save(io, {...j, status: 'complete'});
  if (j.target === HASH) await recordVerified(io, j.manifest);
  report('Codex update installed; previous files retained in backup.');
}
export async function installCodex(io, options = {}) {
  const digest = options.digest || sha256, fetchFile = options.fetchFile || fetch, report = options.report || (() => {});
  report('Checking Codex PS5 updates…');
  const response = await fetchFile('./apps/codex/manifest.json', {cache: 'no-store'});
  if (!response.ok) throw Error('Codex release manifest unavailable.');
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.length > 256 * 1024 || await digest(bytes) !== HASH) throw Error('Codex release manifest verification failed.');
  const m = validatePackage(JSON.parse(decoder.decode(bytes)));
  await io.mkdirs(UPDATE);
  const record = await io.readFile(UPDATE + '/journal.json', 256 * 1024);
  if (record) {
    const j = JSON.parse(decoder.decode(record));
    if (j.schema !== 1 || !['pending', 'complete'].includes(j.status) || !/^\/data\/codex-ps5\/backups\/[a-f0-9]{32}$/.test(j.backup) ||
        !hex(j.target) || !Array.isArray(j.components) || j.components.length !== 2 || j.components.some((c, i) =>
          c.name !== ['native', 'service'][i] || c.live !== [NATIVE, SERVICE][i] || c.stage !== UPDATE + '/' + j.target + '/' + c.name ||
          c.backup !== j.backup + '/' + c.name || c.marker !== ['sce_sys/param.json', 'assistant-service.elf'][i] || (c.previous !== null && !hex(c.previous))))
      throw Error('Codex update journal damaged. Existing files preserved.');
    await recover(io, j, digest, report);
  }
  const current = await installedMatches(io, m, digest, report);
  await io.mkdirs('/data/ps5-ai-cli'); // SDK spawn shim; credentials/workspace are never installer targets.
  if (current) return {...m, updated: false};
  await io.assertNativeStopped();
  const param = await io.readFile(NATIVE + '/sce_sys/param.json', 16384);
  if (param) {
    const installed = nativeIdentity(param);
    const target = m.version.split('.').map((x,i) => x.padStart(i ? 3 : 2, '0')).join('.');
    // 00.001.000 was the prototype before formal 0.0.x numbering.
    if (installed.contentVersion !== '00.001.000' && installed.contentVersion > target) throw Error('Newer Codex installed; downgrade refused.');
  } else if (await io.directoryExists(NATIVE)) throw Error('Codex title identity missing. Existing files preserved.');
  report('Preparing Codex PS5 ' + m.version + '…');
  const id = Array.from(crypto.getRandomValues(new Uint8Array(16)), x => x.toString(16).padStart(2, '0')).join('');
  const backup = '/data/codex-ps5/backups/' + id;
  const components = [];
  for (const [name, live, marker] of [['native', NATIVE, 'sce_sys/param.json'], ['service', SERVICE, 'assistant-service.elf']]) {
    const stage = UPDATE + '/' + HASH + '/' + name;
    for (const f of m[name]) {
      report('Preparing Codex ' + f.path + '…');
      if (await io.matchesFile(stage + '/' + f.path, f, digest)) continue;
      await io.seedBlocks(live + '/' + f.path, f, digest);
      await io.assembleFile(stage + '/' + f.path, f, async (hash, size) => {
        const blob = UPDATE + '/blocks/' + hash + '.bin';
        let data = await io.readFile(blob, 1048576);
        if (!data || data.length !== size || await digest(data) !== hash) {
          const r = await fetchFile('./apps/codex/chunks/' + hash + '.bin', {cache: 'no-store'});
          if (!r.ok) throw Error('Codex block download failed.');
          data = new Uint8Array(await r.arrayBuffer());
          if (data.length !== size || await digest(data) !== hash) throw Error('Codex block verification failed.');
          await io.mkdirs(UPDATE + '/blocks'); await io.writeFile(blob, data);
        }
        return data;
      });
      if (!await io.matchesFile(stage + '/' + f.path, f, digest)) throw Error('Codex staging verification failed.');
    }
    await io.preparePermissions(stage, m[name]);
    components.push({name, live, stage, marker, backup: backup + '/' + name, previous: await io.fingerprint(live + '/' + marker, digest)});
  }
  await io.assertNativeStopped();
  if (options.beforePublish) await options.beforePublish();
  if ((await io.readFile(NATIVE + '/sce_sys/param.json', 16384))?.toString() !== param?.toString()) throw Error('Codex title changed during download.');
  await io.mkdirs(backup); await io.ensureHomebrew(); await io.mkdirs('/data/codex-ps5/payloads');
  const journal = {schema: 1, status: 'pending', target: HASH, backup, manifest: m, components};
  await save(io, journal);
  await recover(io, journal, digest, report);
  return {...m, updated: true};
}
