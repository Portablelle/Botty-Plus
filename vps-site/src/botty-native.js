import { PS5IO, checkedPath } from './ps5-io.js';
import { sha256 } from './transmission.js';

export const NATIVE_ROOT = '/data/homebrew/PPSA99071';
const JOURNAL = '/data/botty/native/update.json';
const BACKUPS = '/data/botty/native/backups';
const HASH='6307da38c0d4c73a75d7cc7fea2e519c6548363523b4210343c1a8562b8985e9';
const FILES = ['assets/Manrope-OFL.txt', 'assets/build.txt', 'assets/nebula.rgb', 'assets/courier.rgba', 'assets/extractor.rgba', 'assets/vault.rgba', 'assets/ui-font.bin', 'eboot.bin', 'sce_module/libc.prx', 'sce_sys/icon0.png', 'sce_sys/pic0.dds', 'sce_sys/param.json', 'sce_sys/snd0.at9'];

const STAGE = '/data/botty/native/' + HASH + '/PPSA99071';
const encoder = new TextEncoder();
const decoder = new TextDecoder();
const METADATA_ROOTS = ['/user/app/PPSA99071/sce_sys', '/user/appmeta/PPSA99071', '/system_data/priv/appmeta/PPSA99071'];
const METADATA_FILES = METADATA_ROOTS.flatMap(root => ['param.json', 'icon0.png', 'pic0.dds'].map(file => root + '/' + file));
METADATA_FILES.push('/user/app/PPSA99071/icon0.png');
// Append to preserve existing journal backup indices.
METADATA_FILES.push(...METADATA_ROOTS.map(root => root + '/snd0.at9'));

// Only this installer can reach the single native title; service IO stays confined.
export class NativeIO extends PS5IO {
  checkedPath(path) {
    if (METADATA_FILES.includes(path)) return path;
    if (typeof path === 'string' && path.startsWith(NATIVE_ROOT + '/'))
      return checkedPath('/data/botty/' + path.slice(NATIVE_ROOT.length + 1));
    return checkedPath(path);
  }
  async nativeExists() {
    // O_NOFOLLOW rejects an existing symlink as well as an unreadable collision.
    const fd = await this.call('open', this.string(NATIVE_ROOT), 0x20000 | 0x100, 0);
    if (fd >= 0) { await this.close(fd); return true; }
    // Do not infer absence from an arbitrary open failure. mkdir succeeds only if absent.
    // Publication uses rename to an empty directory, never over an installed title.
    return false;
  }
  async assertNativeStopped() {
    // Process names do not identify the title reliably: fail closed for any eboot.
    if ((await this.processes()).some(p => /^(eboot(?:\.bin)?|botty.*)$/i.test(p.name)))
      throw Error('Close Botty+ and other native apps before updating, then start a new session.');
  }
  async syncDirectory(path) {
    const fd = await this.call('open', this.string(path), 0x20000 | 0x100, 0);
    if (fd < 0) throw Error('Cannot access update directory.');
    try {
      if (await this.call('fsync', fd) !== 0) throw Error('Could not flush update directory.');
    } finally { await this.close(fd); }
  }
  async moveDirectory(source, destination) {
    this.checkedPath(source + '/check'); this.checkedPath(destination + '/check');
    // Reserve a new empty target; never rename over an existing installation/backup.
    if (await this.call('mkdir', this.string(destination), 0o755) !== 0)
      throw Error('Update destination already exists. Existing files were preserved.');
    if (await this.call('rename', this.string(source), this.string(destination, this.otherPath)) !== 0) {
      await this.runtime.chain.syscall(137, this.string(destination)); // empty reservation only
      throw Error('Could not move native title. Recovery will run next session.');
    }
    await this.syncDirectory(source.slice(0, source.lastIndexOf('/')));
    await this.syncDirectory(destination.slice(0, destination.lastIndexOf('/')));
  }
  async backupNative(backup) {
    await this.mkdirs(backup.slice(0, backup.lastIndexOf('/')));
    await this.moveDirectory(NATIVE_ROOT, backup);
  }
  async removeEmptyNative() {
    return ((await this.runtime.chain.syscall(137, this.string(NATIVE_ROOT))).low | 0) === 0;
  }
  async restoreNative(backup) {
    // rmdir removes only a possible empty reservation, never title contents.
    await this.runtime.chain.syscall(137, this.string(NATIVE_ROOT));
    await this.moveDirectory(backup, NATIVE_ROOT);
  }
  async writeJournal(value, exclusive = false) {
    await this.mkdirs('/data/botty/native');
    await this.writeFile(JOURNAL, encoder.encode(JSON.stringify(value) + '\n'), exclusive);
    await this.syncDirectory('/data/botty/native');
  }
  async syncRegisteredMetadata(manifest, backup, digest) {
    await this.assertNativeStopped();
    // Refresh only this title's known metadata; never edit the application database.
    const eligible = new Set();
    for (const root of METADATA_ROOTS) {
      const param = await this.readFile(root + '/param.json', 16384);
      if (!param) continue; // A fresh installation is registered by ShadowMountPlus.
      identity(param, manifest.version);
      const fd = await this.call('open', this.string(root), 0x20000 | 0x100, 0);
      if (fd < 0) throw Error('Cannot access registered Botty+ metadata.');
      await this.close(fd);
      eligible.add(root);
    }
    for (const [index, path] of METADATA_FILES.entries()) {
      const root = path.slice(0, path.lastIndexOf('/'));
      if (!eligible.has(root === '/user/app/PPSA99071' ? root + '/sce_sys' : root)) continue;
      const name = path.slice(path.lastIndexOf('/') + 1);
      const file = manifest.files.find(f => f.path === 'sce_sys/' + name);
      const data = await this.readFile(NATIVE_ROOT + '/' + file.path, file.size);
      if (!data || await digest(data) !== file.sha256) throw Error('Native metadata source verification failed.');
      const previous = await this.readFile(path, 16 * 1024 * 1024);
      if (previous && await digest(previous) === file.sha256) continue;
      const metadataBackup = backup.slice(0, backup.lastIndexOf('/')) + '/metadata';
      const saved = metadataBackup + '/' + index + '.bin';
      if (previous && !await this.readFile(saved, 16 * 1024 * 1024)) {
        await this.mkdirs(metadataBackup);
        await this.writeFile(saved, previous, true);
        const disk = await this.readFile(saved, 16 * 1024 * 1024);
        if (!disk || await digest(disk) !== await digest(previous)) throw Error('Metadata backup verification failed.');
      }
      await this.writeFile(path, data);
      if (((await this.runtime.chain.syscall(15, this.string(path), 0o644)).low | 0) !== 0)
        throw Error('Could not set native metadata permissions.');
      const disk = await this.readFile(path, file.size);
      if (!disk || await digest(disk) !== file.sha256) throw Error('Registered metadata verification failed.');
      await this.syncDirectory(root);
    }
  }
  async prepareNativePermissions(root) {
    if (root !== STAGE && root !== NATIVE_ROOT) throw Error('Unexpected native permission root.');
    // The native title must be readable/executable from the application sandbox.
    for (const path of [root, root + '/assets', root + '/sce_module', root + '/sce_sys']) {
      if (((await this.runtime.chain.syscall(15, this.string(path), 0o755)).low | 0) !== 0)
        throw Error('Could not set native directory permissions.');
    }
    for (const file of FILES) {
      // The loader also requires executable permissions on the native runtime.
      const executable = file === 'eboot.bin' || file === 'sce_module/libc.prx';
      if (((await this.runtime.chain.syscall(15, this.string(root + '/' + file), executable ? 0o755 : 0o644)).low | 0) !== 0)
        throw Error('Could not set native file permissions.');
    }
    for (const path of [root + '/assets', root + '/sce_module', root + '/sce_sys', root])
      await this.syncDirectory(path);
  }
  async publishNative() {
    await this.prepareNativePermissions(STAGE);
    await this.call('mkdir', this.string('/data/homebrew'), 0o755);
    const fd = await this.call('open', this.string('/data/homebrew'), 0x20000 | 0x100, 0);
    if (fd < 0) throw Error('Cannot access the homebrew directory.');
    await this.close(fd);
    await this.moveDirectory(STAGE, NATIVE_ROOT);
  }
}

function identity(bytes, targetVersion, reuseNewer = false) {
  let value;
  try { value = JSON.parse(decoder.decode(bytes)); } catch (_) {}
  if (!value || value.titleId !== 'PPSA99071' ||
      value.contentId !== 'UP9000-PPSA99071_00-BOTTYNATIVE00001' ||
      !/^\d{2}\.\d{3}\.\d{3}$/.test(value.contentVersion) ||
      !['Botty+', 'Botty Native Preview', 'Botty Native'].includes(value.localizedParameters?.['en-US']?.titleName))
    throw Error('Existing title is not a recognized Botty+ installation. Existing files were preserved.');
  if (value.contentVersion > targetVersion && !reuseNewer)
    throw Error('A newer Botty+ is already installed. Downgrade refused.');
  return value;
}

async function matches(io, root, manifest, digest) {
  for (const file of manifest.files) {
    const bytes = await io.readFile(root + '/' + file.path, 16 * 1024 * 1024);
    if (!bytes || bytes.length !== file.size || await digest(bytes) !== file.sha256) return false;
  }
  return true;
}

async function recover(io, journal, manifest, digest, report) {
  if (journal.status !== 'pending') return;
  if (journal.target === HASH && await matches(io, NATIVE_ROOT, manifest, digest)) {
    await io.syncRegisteredMetadata(manifest, journal.backup, digest);
    await io.writeJournal({...journal, status: 'complete'});
    report('Previous Botty+ update verified. Backup retained.');
    return;
  }
  const current = await io.readFile(NATIVE_ROOT + '/sce_sys/param.json', 16384);
  if (current && await digest(current) === journal.previous) {
    await io.writeJournal({...journal, status: 'rolled-back'});
    return; // The original tree was never moved, or was already restored.
  }
  const saved = await io.readFile(journal.backup + '/sce_sys/param.json', 16384);
  if (!saved || await digest(saved) !== journal.previous)
    throw Error('Native recovery backup is unavailable. Existing files were preserved.');
  if (current) throw Error('Native recovery found unexpected title files. Existing files were preserved.');
  await io.assertNativeStopped();
  await io.restoreNative(journal.backup);
  await io.writeJournal({...journal, status: 'rolled-back'});
  report('Previous Botty+ restored after an interrupted update.');
}

export async function installNative(io, options = {}) {
  const fetchFile = options.fetchFile || fetch;
  const digest = options.digest || sha256;
  const report = options.report || (() => {});
  report('Checking Botty+…');
  const response = await fetchFile('./apps/botty-native/manifest.json', { cache: 'no-store' });
  if (!response.ok) throw Error('Botty+ manifest unavailable.');
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (await digest(bytes) !== HASH) throw Error('Botty+ manifest verification failed.');
  const manifest = JSON.parse(decoder.decode(bytes));
  if (manifest.schema !== 1 || manifest.titleId !== 'PPSA99071' ||
      !/^\d{2}\.\d{3}\.\d{3}$/.test(manifest.version) ||
      manifest.files.length !== FILES.length || new Set(manifest.files.map(f => f.path)).size !== FILES.length ||
      manifest.files.some(f => !FILES.includes(f.path))) throw Error('Unexpected native package.');

  const record = await io.readFile(JOURNAL, 8192);
  let journal;
  if (record) {
    try { journal = JSON.parse(decoder.decode(record)); } catch (_) {}
    if (!journal || journal.schema !== 1 || !['pending', 'complete', 'rolled-back'].includes(journal.status) ||
        !/^[a-f0-9]{64}$/.test(journal.target) || !/^[a-f0-9]{64}$/.test(journal.previous) ||
        !/^\/data\/botty\/native\/backups\/[a-f0-9]{32}\/PPSA99071$/.test(journal.backup))
      throw Error('Native update journal is damaged. Existing files were preserved.');
    await recover(io, journal, manifest, digest, report);
  }
  let installed = await io.nativeExists();
  // A power loss during first publication may leave only an empty reservation.
  if (installed && !await io.readFile(NATIVE_ROOT + '/sce_sys/param.json', 16384) &&
      await matches(io, STAGE, manifest, digest)) {
    await io.assertNativeStopped();
    if (await io.removeEmptyNative()) installed = false;
  }
  let previous;
  if (installed) {
    if (await matches(io, NATIVE_ROOT, manifest, digest)) {
      // Hashes cannot detect permissions left by an older installer.
      await io.prepareNativePermissions(NATIVE_ROOT);
      report('Botty+ ' + manifest.version + ' is already installed.');
      return {version: manifest.version, updated: false};
    }
    previous = await io.readFile(NATIVE_ROOT + '/sce_sys/param.json', 16384);
    const current = identity(previous, manifest.version, options.reuseNewer === true);
    if (current.contentVersion > manifest.version) {
      // Launch can reuse a recognized newer title, but must never modify it using
      // an older package's file list, permissions, or registered metadata.
      report('Keeping installed Botty+ ' + current.contentVersion + ' (portal: ' + manifest.version + ').');
      return {version: current.contentVersion, updated: false};
    }
    await io.assertNativeStopped();
    report('Preparing Botty+ update to ' + manifest.version + '…');
  } else report('Installing Botty+ ' + manifest.version + '…');

  for (const file of manifest.files) {
    const path = STAGE + '/' + file.path;
    let data = await io.readFile(path, 16 * 1024 * 1024);
    if (data && data.length === file.size && await digest(data) === file.sha256) continue;
    const result = await fetchFile('./apps/botty-native/' + file.path, { cache: 'no-store' });
    if (!result.ok) throw Error('Native file download failed: ' + file.path);
    data = new Uint8Array(await result.arrayBuffer());
    if (data.length !== file.size || await digest(data) !== file.sha256) throw Error('Native file verification failed: ' + file.path);
    await io.mkdirs(path.slice(0, path.lastIndexOf('/')));
    await io.writeFile(path, data);
    const disk = await io.readFile(path, file.size);
    if (!disk || await digest(disk) !== file.sha256) throw Error('Native installation verification failed.');
  }
  if (!await matches(io, STAGE, manifest, digest)) throw Error('Native staging verification failed.');
  if (installed) {
    await io.assertNativeStopped();
    const current = await io.readFile(NATIVE_ROOT + '/sce_sys/param.json', 16384);
    if (!current || await digest(current) !== await digest(previous))
      throw Error('Installed title changed during preparation. Existing files were preserved.');
    const random = crypto.getRandomValues(new Uint8Array(16));
    const id = Array.from(random, x => x.toString(16).padStart(2, '0')).join('');
    journal = {schema: 1, status: 'pending', target: HASH, previous: await digest(previous),
      backup: BACKUPS + '/' + id + '/PPSA99071'};
    await io.writeJournal(journal, !record);
    try {
      await io.backupNative(journal.backup);
      await io.publishNative();
      if (!await matches(io, NATIVE_ROOT, manifest, digest)) throw Error('Published native title verification failed.');
      await io.syncRegisteredMetadata(manifest, journal.backup, digest);
      await io.writeJournal({...journal, status: 'complete'});
    } catch (error) {
      // Restore only if the live path is absent/empty; never delete uncertain content.
      try { await recover(io, journal, manifest, digest, report); }
      catch (_) { throw Error('Botty+ update interrupted. Backup retained; restart to recover before opening the app.'); }
      throw error;
    }
    report('Botty+ updated to ' + manifest.version + '. Previous version backed up.');
  } else {
    await io.publishNative();
    report('Botty+ files installed. Preparing home screen discovery…');
  }
  return {version: manifest.version, updated: installed};
}
