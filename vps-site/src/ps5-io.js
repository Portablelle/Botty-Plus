// FreeBSD/PS5 syscall adapter. All writes stay inside Botty's private data tree.
// The ROP runtime is serialized by the portal; it must never be used concurrently.
const ROOT = '/data/botty';
// Shared with package installers so a verified payload can reach the ELF loader.
export const MAX_ELF_BYTES = 32 * 1024 * 1024;
const SYS = { read: 3, write: 4, open: 5, close: 6, kill: 37, fsync: 95,
  socket: 97, connect: 98, setsockopt: 105, rename: 128, mkdir: 136, sysctl: 202 };
const encoder = new TextEncoder();
const decoder = new TextDecoder();
export const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

export function checkedPath(path) {
  if (typeof path !== 'string' || !path.startsWith(ROOT + '/') ||
      !/^[/a-zA-Z0-9._-]+$/.test(path) || path.split('/').some(x => x === '..' || x === '.'))
    throw Error('Refusing a path outside Botty storage.');
  return path;
}

export class PS5IO {
  constructor(runtime) {
    if (!runtime || !runtime.p || !runtime.chain) throw Error('Start a session first.');
    this.runtime = runtime;
    this.buffer = runtime.p.malloc(65536, 1);
    this.pathBuffer = runtime.p.malloc(1024, 1);
    this.otherPath = runtime.p.malloc(1024, 1);
    this.address = runtime.p.malloc(16, 1);
    this.option = runtime.p.malloc(16, 1);
    this.processBuffer = null;
  }
  async call(name, ...args) {
    return (await this.runtime.chain.syscall(SYS[name], ...args)).low | 0;
  }
  string(text, ptr = this.pathBuffer) {
    const bytes = encoder.encode(text);
    if (bytes.length >= ptr.backing.length) throw Error('Path too long.');
    ptr.backing.fill(0); ptr.backing.set(bytes); return ptr;
  }
  checkedPath(path) { return checkedPath(path); }
  async close(fd) { if (fd >= 0) await this.call('close', fd); }
  async mkdirs(path) {
    this.checkedPath(path + '/check');
    const components = path.split('/').filter(Boolean);
    for (let i = 2; i <= components.length; i++) {
      const part = '/' + components.slice(0, i).join('/');
      await this.call('mkdir', this.string(part), 0o700);
      // O_DIRECTORY | O_NOFOLLOW: reject files/symlinks as existing directories.
      const fd = await this.call('open', this.string(part), 0x20000 | 0x100, 0);
      if (fd < 0) throw Error('Cannot access directory: ' + part);
      await this.close(fd);
    }
  }
  async readFile(path, limit = 2 * 1024 * 1024) {
    this.checkedPath(path);
    const fd = await this.call('open', this.string(path), 0x100, 0);
    if (fd < 0) return null; // Creation uses O_EXCL, so unreadable files are never replaced.
    const chunks = []; let length = 0;
    try {
      while (true) {
        const n = await this.call('read', fd, this.buffer, 65536);
        if (n < 0 || n > 65536) throw Error('Read failed: ' + path);
        if (!n) break;
        length += n;
        if (length > limit) throw Error('File exceeds expected size: ' + path);
        chunks.push(this.buffer.backing.slice(0, n));
      }
    } finally { await this.close(fd); }
    const result = new Uint8Array(length); let offset = 0;
    for (const chunk of chunks) { result.set(chunk, offset); offset += chunk.length; }
    return result;
  }
  async writeAll(fd, bytes) {
    for (let offset = 0; offset < bytes.length;) {
      const size = Math.min(65536, bytes.length - offset);
      this.buffer.backing.set(bytes.subarray(offset, offset + size));
      let sent = 0;
      while (sent < size) {
        const n = await this.call('write', fd, this.buffer.add32(sent), size - sent);
        if (n <= 0 || n > size - sent) throw Error('Write interrupted (connection or disk space).');
        sent += n;
      }
      offset += size;
    }
  }
  async writeFile(path, bytes, exclusive = false) {
    this.checkedPath(path);
    const target = exclusive ? path : path + '.part';
    // O_WRONLY | O_CREAT | O_NOFOLLOW | (O_EXCL or O_TRUNC)
    const fd = await this.call('open', this.string(target), 1 | 0x200 | 0x100 | (exclusive ? 0x800 : 0x400), 0o600);
    if (fd < 0) throw Error('Cannot create file (existing, permissions or disk space): ' + target);
    try {
      await this.writeAll(fd, bytes);
      if (await this.call('fsync', fd) !== 0) throw Error('Could not flush file: ' + target);
    } finally { await this.close(fd); }
    if (!exclusive && await this.call('rename', this.string(target), this.string(path, this.otherPath)) !== 0)
      throw Error('Could not commit file: ' + path);
  }
  async processes() {
    const mib = this.pathBuffer;
    new Int32Array(mib.backing.buffer, mib.backing.byteOffset, 4).set([1, 14, 8, 0]);
    this.option.backing.fill(0);
    if (await this.call('sysctl', mib, 4, 0, this.option, 0, 0) !== 0) throw Error('Cannot inspect PS5 processes.');
    const view = new DataView(this.option.backing.buffer, this.option.backing.byteOffset);
    const size = view.getUint32(0, true) + 65536;
    if (view.getUint32(4, true) || size > 4 * 1024 * 1024) throw Error('Unexpected process table size.');
    if (!this.processBuffer || this.processBuffer.backing.length < size)
      this.processBuffer = this.runtime.p.malloc(size, 1);
    view.setUint32(0, this.processBuffer.backing.length, true);
    if (await this.call('sysctl', mib, 4, this.processBuffer, this.option, 0, 0) !== 0) throw Error('Process table changed; try again.');
    const bytes = this.processBuffer.backing;
    const data = new DataView(bytes.buffer, bytes.byteOffset);
    const result = []; const total = view.getUint32(0, true);
    if (total > bytes.length) throw Error('Invalid process table length.');
    for (let offset = 0; offset < total;) {
      const size = data.getInt32(offset, true);
      if (size < 480 || offset + size > total) throw Error('Unknown PS5 process table layout.');
      const name = decoder.decode(bytes.subarray(offset + 447, offset + 479)).split('\0')[0];
      result.push({ pid: data.getInt32(offset + 72, true), name });
      offset += size;
    }
    return result;
  }
  async stopHelper(pid) {
    const same = (await this.processes()).some(p => p.pid === pid && p.name === 'websrv.elf');
    if (!same) return;
    if (await this.call('kill', pid, 15) !== 0) throw Error('Could not stop temporary websrv. Restart the PS5.');
    for (let i = 0; i < 20; i++) {
      if (!(await this.processes()).some(p => p.pid === pid && p.name === 'websrv.elf')) return;
      await sleep(100);
    }
    throw Error('Temporary websrv has not stopped. Restart the PS5.');
  }
  async connect(port) {
    if (![2121, 8080, 8088, 9091, 9021, 5001, 5910, 9999, 49322, 49323].includes(port)) throw Error('Unsupported local port.');
    const fd = await this.call('socket', 2, 1, 0);
    if (fd < 0) throw Error('Cannot create local socket.');
    try {
      // Prevent a broken socket from terminating the browser process.
      this.option.backing.fill(0); this.option.backing[0] = 1;
      if (await this.call('setsockopt', fd, 0xffff, 0x0800, this.option, 4) !== 0) throw Error('Cannot protect socket.');
      this.option.backing.fill(0); this.option.backing[0] = 2;
      for (const opt of [0x1005, 0x1006])
        if (await this.call('setsockopt', fd, 0xffff, opt, this.option, 16) !== 0) throw Error('Cannot set socket timeout.');
      this.address.backing.set([16, 2, port >> 8, port & 255, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]);
      if (await this.call('connect', fd, this.address, 16) !== 0) { await this.close(fd); return -1; }
      return fd;
    } catch (error) { await this.close(fd); throw error; }
  }
  async listening(port) {
    const fd = await this.connect(port);
    if (fd < 0) return false;
    await this.close(fd); return true;
  }
  async sendElf(bytes) {
    if (bytes.length > MAX_ELF_BYTES) throw Error('Launcher ELF exceeds the 32 MiB limit.');
    if (bytes.length < 4096 ||
        bytes[0] !== 127 || bytes[1] !== 69 || bytes[2] !== 76 || bytes[3] !== 70)
      throw Error('Invalid launcher ELF.');
    const fd = await this.connect(9021);
    if (fd < 0) throw Error('ELF loader unavailable.');
    try { await this.writeAll(fd, bytes); } finally { await this.close(fd); }
  }
  async http(port, path, body = null, headers = {}) {
    if (!path.startsWith('/') || /[\r\n]/.test(path)) throw Error('Invalid request path.');
    const content = body === null ? new Uint8Array() : encoder.encode(body);
    let request = `${body === null ? 'GET' : 'POST'} ${path} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nConnection: close\r\n`;
    for (const [key, value] of Object.entries(headers)) {
      if (/[\r\n]/.test(key + value)) throw Error('Invalid request header.');
      request += key + ': ' + value + '\r\n';
    }
    request += 'Content-Length: ' + content.length + '\r\n\r\n';
    const fd = await this.connect(port);
    if (fd < 0) throw Error('Service unavailable on port ' + port);
    const chunks = []; let length = 0; const deadline = Date.now() + 15000;
    try {
      await this.writeAll(fd, encoder.encode(request));
      if (content.length) await this.writeAll(fd, content);
      while (true) {
        if (Date.now() > deadline) throw Error('Local HTTP request timed out.');
        const n = await this.call('read', fd, this.buffer, 65536);
        if (n < 0 || n > 65536) throw Error('Local HTTP response interrupted.');
        if (!n) break;
        chunks.push(this.buffer.backing.slice(0, n)); length += n;
        if (length > 1024 * 1024) throw Error('Local HTTP response too large.');
      }
    } finally { await this.close(fd); }
    const bytes = new Uint8Array(length); let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
    return parseHttp(bytes);
  }
}

export function parseHttp(input) {
  const bytes = typeof input === 'string' ? encoder.encode(input) : input;
  function lineEnd(start) {
    for (let i = start; i < bytes.length - 1; i++) if (bytes[i] === 13 && bytes[i + 1] === 10) return i;
    throw Error('Truncated HTTP line.');
  }
  const firstEnd = lineEnd(0);
  const status = /^HTTP\/1\.[01] (\d{3})/.exec(decoder.decode(bytes.subarray(0, firstEnd)));
  if (!status) throw Error('Invalid local HTTP response.');
  const headers = {}; let cursor = firstEnd + 2;
  while (true) {
    const end = lineEnd(cursor);
    if (end === cursor) { cursor += 2; break; }
    const line = decoder.decode(bytes.subarray(cursor, end));
    const colon = line.indexOf(':');
    if (colon <= 0) throw Error('Invalid HTTP header.');
    headers[line.slice(0, colon).toLowerCase()] = line.slice(colon + 1).trim();
    cursor = end + 2;
  }
  let body;
  if (headers['transfer-encoding'] === 'chunked') {
    const chunks = []; let total = 0;
    while (true) {
      const end = lineEnd(cursor);
      const sizeText = decoder.decode(bytes.subarray(cursor, end));
      if (!/^[0-9a-f]+(?:;.*)?$/i.test(sizeText)) throw Error('Invalid HTTP chunk.');
      const size = parseInt(sizeText, 16); cursor = end + 2;
      if (!size) { lineEnd(cursor); break; }
      if (!Number.isSafeInteger(size) || cursor + size + 2 > bytes.length || bytes[cursor + size] !== 13 || bytes[cursor + size + 1] !== 10)
        throw Error('Truncated HTTP chunk.');
      chunks.push(bytes.subarray(cursor, cursor + size)); total += size; cursor += size + 2;
    }
    const data = new Uint8Array(total); let offset = 0;
    for (const chunk of chunks) { data.set(chunk, offset); offset += chunk.length; }
    body = decoder.decode(data);
  } else {
    if (headers['transfer-encoding']) throw Error('Unsupported HTTP encoding.');
    const data = bytes.subarray(cursor);
    if (headers['content-length'] !== undefined && (!/^\d+$/.test(headers['content-length']) || data.length !== Number(headers['content-length'])))
      throw Error('Truncated local HTTP response.');
    body = decoder.decode(data);
  }
  return { status: Number(status[1]), headers, body };
}
