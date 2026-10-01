import { PS5IO, checkedPath } from './ps5-io.js';
import { sha256 } from './transmission.js';

export const NATIVE_ROOT = '/data/homebrew/PPSA99071';
const STAGE = '/data/botty/native/PPSA99071';
const HASH = 'baa4adcaf89a4541c091846b57f8f0bb4840a3ea4f73e82abfd7de1b4d9190b1';
const FILES = ['assets/Manrope-OFL.txt', 'assets/build.txt', 'assets/ui-font.bin', 'eboot.bin', 'sce_module/libc.prx', 'sce_sys/icon0.png', 'sce_sys/pic0.dds', 'sce_sys/param.json'];

// Only this installer can reach the single native title; service IO stays confined.
export class NativeIO extends PS5IO {
  checkedPath(path) {
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
  async publishNative() {
    // The native title must be readable/executable from the application sandbox.
    for (const path of [STAGE, STAGE + '/assets', STAGE + '/sce_module', STAGE + '/sce_sys']) {
      if (((await this.runtime.chain.syscall(15, this.string(path), 0o755)).low | 0) !== 0)
        throw Error('Could not set native directory permissions.');
    }
    for (const file of FILES) {
      if (((await this.runtime.chain.syscall(15, this.string(STAGE + '/' + file), file === 'eboot.bin' ? 0o755 : 0o644)).low | 0) !== 0)
        throw Error('Could not set native file permissions.');
    }
    await this.call('mkdir', this.string('/data/homebrew'), 0o755);
    const fd = await this.call('open', this.string('/data/homebrew'), 0x20000 | 0x100, 0);
    if (fd < 0) throw Error('Cannot access the homebrew directory.');
    await this.close(fd);
    if (await this.call('mkdir', this.string(NATIVE_ROOT), 0o755) !== 0)
      throw Error('Native title path already exists. Existing files were preserved.');
    if (await this.call('rename', this.string(STAGE), this.string(NATIVE_ROOT, this.otherPath)) !== 0)
      throw Error('Could not publish Botty+. Staged files were preserved.');
  }
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
  const manifest = JSON.parse(new TextDecoder().decode(bytes));
  if (manifest.schema !== 1 || manifest.titleId !== 'PPSA99071' ||
      manifest.files.length !== FILES.length || new Set(manifest.files.map(f => f.path)).size !== FILES.length ||
      manifest.files.some(f => !FILES.includes(f.path))) throw Error('Unexpected native package.');
  const installed = await io.nativeExists();
  for (const file of manifest.files) {
    const path = (installed ? NATIVE_ROOT : STAGE) + '/' + file.path;
    let data = await io.readFile(path, 16 * 1024 * 1024);
    if (data && data.length === file.size && await digest(data) === file.sha256) continue;
    if (installed) throw Error('Existing Botty+ differs from this package. Existing title preserved; update it before launching.');
    report('Installing Botty+…');
    const result = await fetchFile('./apps/botty-native/' + file.path, { cache: 'no-store' });
    if (!result.ok) throw Error('Native file download failed: ' + file.path);
    data = new Uint8Array(await result.arrayBuffer());
    if (data.length !== file.size || await digest(data) !== file.sha256) throw Error('Native file verification failed: ' + file.path);
    await io.mkdirs(path.slice(0, path.lastIndexOf('/')));
    await io.writeFile(path, data);
    const disk = await io.readFile(path, file.size);
    if (!disk || await digest(disk) !== file.sha256) throw Error('Native installation verification failed.');
  }
  if (!installed) await io.publishNative();
  report(installed ? 'Botty+ is already installed.' : 'Botty+ files installed. Preparing home screen discovery…');
}
