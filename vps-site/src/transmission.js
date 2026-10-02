import { sleep } from './ps5-io.js';

export const ROOT = '/data/botty/transmission';
export const STATE = ROOT + '/state';
export const DOWNLOADS = '/data/botty/downloads';
const ID = '4.0.6-v0.33-botty1';
const APP = ROOT + '/' + ID;
const BASE = './apps/transmission/';
const MANIFEST_HASH = '5c54304c722f8bc4757dde7493fed83204fbfc4b07573592c4effc50f82255d0';
const enc = new TextEncoder();
const dec = new TextDecoder();

export async function sha256(bytes) {
  if (!globalThis.crypto || !crypto.subtle) throw Error('Open this portal over HTTPS for verified installation.');
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, b => b.toString(16).padStart(2, '0')).join('');
}
function jsonBytes(value) { return enc.encode(JSON.stringify(value, null, 2) + '\n'); }
function parse(bytes, label) {
  try { return JSON.parse(dec.decode(bytes)); } catch (_) { throw Error(label + ' is damaged. It has not been overwritten.'); }
}
export function settingsFor(credentials) {
  return {
    'download-dir': DOWNLOADS + '/complete',
    'incomplete-dir': DOWNLOADS + '/incomplete',
    'incomplete-dir-enabled': true,
    'rename-partial-files': true,
    'rpc-authentication-required': true,
    'rpc-username': credentials.username,
    'rpc-password': credentials.password,
    'rpc-bind-address': '0.0.0.0',
    'rpc-enabled': true,
    'rpc-port': 9091,
    'rpc-whitelist-enabled': true,
    'rpc-whitelist': '127.0.0.1,192.168.*.*',
    'rpc-host-whitelist-enabled': true,
    'rpc-host-whitelist': 'localhost',
    'port-forwarding-enabled': false,
    'download-queue-enabled': true,
    'download-queue-size': 1,
    'peer-limit-global': 60,
    'peer-limit-per-torrent': 40,
    'cache-size-mb': 32,
    'umask': '077',
    'script-torrent-done-enabled': false,
    'script-torrent-added-enabled': false,
  };
}
function validateSettings(settings) {
  if (settings['rpc-authentication-required'] !== true || settings['rpc-enabled'] !== true ||
      settings['rpc-username'] !== 'botty' || settings['rpc-port'] !== 9091 ||
      settings['rpc-whitelist-enabled'] !== true || settings['rpc-whitelist'] !== '127.0.0.1,192.168.*.*' ||
      settings['port-forwarding-enabled'] !== false ||
      settings['download-dir'] !== DOWNLOADS + '/complete' ||
      settings['incomplete-dir'] !== DOWNLOADS + '/incomplete' || settings['incomplete-dir-enabled'] !== true)
    throw Error('Transmission settings differ from this installer. Existing settings were preserved; review them before starting.');
}
async function readCredentials(io) {
  const bytes = await io.readFile(STATE + '/botty-credentials.json', 4096);
  if (bytes === null) return null;
  const value = parse(bytes, 'Saved credentials');
  if (value.username !== 'botty' || typeof value.password !== 'string' || !/^(?:[A-Za-z0-9]{6}|[a-f0-9]{32})$/.test(value.password)) throw Error('Invalid saved credentials; refusing to replace them.');
  return value;
}
export function shortPassword(random = bytes => crypto.getRandomValues(bytes)) {
  // Rejection sampling avoids bias; omit visually ambiguous characters for TV entry.
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789';
  let result = '';
  const limit = Math.floor(256 / alphabet.length) * alphabet.length;
  while (result.length < 6) {
    const bytes = new Uint8Array(16); random(bytes);
    for (const byte of bytes) {
      if (byte < limit) result += alphabet[byte % alphabet.length];
      if (result.length === 6) break;
    }
  }
  return result;
}
export async function prepareShortPassword(io, credentials, settings) {
  // Caller has verified that the daemon is absent. No running downloads are stopped.
  if (await io.listening(9091) || (await io.processes()).some(p => /^transmission/.test(p.name)))
    throw Error('Wait for Transmission to stop before changing its password.');
  const pendingPath = STATE + '/password-migration.json';
  const pendingBytes = await io.readFile(pendingPath, 4096);
  let pending = pendingBytes ? parse(pendingBytes, 'Password migration') : null;
  if (pending && (pending.schema !== 1 || typeof pending.password !== 'string' || !/^[A-Za-z0-9]{6}$/.test(pending.password) ||
      pending.username !== 'botty' || !['pending', 'complete'].includes(pending.status)))
    throw Error('Invalid password migration record. Existing files were preserved.');
  if (pending?.status === 'complete' && pending.password !== credentials.password)
    throw Error('Credentials disagree with the completed password migration.');
  if (credentials.password.length === 6 && (!pending || pending.status === 'complete')) return credentials;
  if (pending?.status === 'complete') throw Error('Credentials disagree with the completed password migration.');
  if (!pending && await io.listening(8088)) {
    const manager = await io.http(8088, '/health');
    const info = manager.status === 200 ? JSON.parse(manager.body) : {};
    if (info.app !== 'Botty' || info.apiVersion !== 1) return credentials;
  }
  if (!pending) {
    for (const [name, value] of [['botty-credentials.before-native.json', credentials], ['settings.before-native.json', settings]]) {
      const existing = await io.readFile(STATE + '/' + name, 65536);
      if (existing && JSON.stringify(parse(existing, 'Password backup')) !== JSON.stringify(value))
        throw Error('Password backup differs from the current configuration. Startup stopped.');
      if (!existing) await io.writeFile(STATE + '/' + name, jsonBytes(value), true);
    }
    pending = {schema: 1, status: 'pending', username: 'botty', password: shortPassword(), previousPassword: credentials.password};
    await io.writeFile(pendingPath, jsonBytes(pending));
  }
  if (pending.previousPassword !== credentials.password && pending.password !== credentials.password)
    throw Error('Credentials changed during password migration. Startup stopped.');
  const next = {username: 'botty', password: pending.password};
  await io.writeFile(STATE + '/settings.json', jsonBytes({...settings, 'rpc-password': next.password}));
  const verifiedSettings = parse(await io.readFile(STATE + '/settings.json', 65536), 'Transmission settings');
  validateSettings(verifiedSettings);
  if (verifiedSettings['rpc-password'] !== next.password) throw Error('Password settings verification failed.');
  await io.writeFile(STATE + '/botty-credentials.json', jsonBytes(next));
  const saved = await readCredentials(io);
  if (saved.password !== next.password) throw Error('Password verification failed. Startup stopped.');
  await io.writeFile(pendingPath, jsonBytes({...next, schema: 1, status: 'complete'}));
  return next;
}
export async function rpc(io, credentials, method) {
  if (!['session-get', 'session-close'].includes(method)) throw Error('Unsupported RPC operation.');
  const headers = { 'Authorization': 'Basic ' + btoa(credentials.username + ':' + credentials.password), 'Content-Type': 'application/json' };
  const body = JSON.stringify({ method });
  let response = await io.http(9091, '/transmission/rpc', body, headers);
  if (response.status === 409) {
    const token = response.headers['x-transmission-session-id'];
    if (!token || !/^[a-zA-Z0-9]+$/.test(token)) throw Error('Invalid Transmission session token.');
    headers['X-Transmission-Session-Id'] = token;
    response = await io.http(9091, '/transmission/rpc', body, headers);
  }
  if (response.status !== 200) throw Error('Transmission RPC failed (HTTP ' + response.status + '). Credentials and downloads were preserved.');
  const result = JSON.parse(response.body);
  if (result.result !== 'success') throw Error('Transmission RPC did not succeed.');
  return result.arguments || {};
}
async function health(io, credentials) {
  const unauthenticated = await io.http(9091, '/transmission/rpc', JSON.stringify({ method: 'session-get' }), { 'Content-Type': 'application/json' });
  if (unauthenticated.status === 403) throw Error('Transmission refused access (HTTP 403): its IP allowlist or login protection blocked the request.');
  if (unauthenticated.status !== 401) throw Error('Port 9091 is not the expected password-protected Transmission service (HTTP ' + unauthenticated.status + ').');
  const info = await rpc(io, credentials, 'session-get');
  if (!/^4\.0\.6(?:\s|$)/.test(info.version || '') || info['download-dir'] !== DOWNLOADS + '/complete' ||
      info['incomplete-dir'] !== DOWNLOADS + '/incomplete' || info['incomplete-dir-enabled'] !== true)
    throw Error('The running Transmission instance does not match Botty configuration.');
  const web = await io.http(9091, '/transmission/web/', null, { Authorization: 'Basic ' + btoa(credentials.username + ':' + credentials.password) });
  if (web.status !== 200 || !/<html[\s>]/i.test(web.body)) throw Error('Transmission RPC works, but its web interface is unavailable.');
  return { credentials, version: info.version, freeBytes: info['download-dir-free-space'] };
}
async function download(file, fetchFile, digest) {
  if (!/^(transmission-daemon\.elf|websrv-ps5\.elf|public_html\/[a-zA-Z0-9/_.-]+)$/.test(file.path) ||
      file.path.split('/').some(s => s === '.' || s === '..') ||
      !Number.isInteger(file.size) || file.size < 1 || file.size > 16 * 1024 * 1024 || !/^[0-9a-f]{64}$/.test(file.sha256))
    throw Error('Invalid package entry.');
  const response = await fetchFile(BASE + file.path, { cache: 'no-store' });
  if (!response.ok) throw Error('Package download failed: HTTP ' + response.status);
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.length !== file.size || await digest(bytes) !== file.sha256) throw Error('Package verification failed: ' + file.path);
  return bytes;
}
// Retained export for older callers: never restart the retired engine.
export async function installAndStart() {
  throw Error('Transmission startup is disabled. Reload the portal to start rTorrent.');
}

export async function stopTransmission(io, wait = sleep) {
  const credentials = await readCredentials(io);
  if (!credentials) throw Error('No Botty Transmission credentials found.');
  const pidBytes = await io.readFile(STATE + '/transmission.pid', 64);
  const pid = pidBytes && Number(dec.decode(pidBytes).trim());
  const validPid = Number.isInteger(pid) && pid > 1;
  if (await io.listening(9091)) {
    await health(io, credentials);
    if (!validPid) throw Error('Cannot verify the Transmission process ID for a safe stop. Keep the PS5 on.');
    await rpc(io, credentials, 'session-close');
  } else if (!validPid) {
    if (pidBytes) throw Error('Saved process ID is invalid. Cannot confirm a safe stop.');
    return;
  }
  for (let i = 0; i < 120; i++) {
    if (!await io.listening(9091) && !(await io.processes()).some(p => p.pid === pid)) return;
    await wait(250);
  }
  throw Error('Transmission is still stopping. Keep the PS5 on until it has exited.');
}
