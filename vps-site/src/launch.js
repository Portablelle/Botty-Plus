import { PS5IO, sleep } from './ps5-io.js';
import { NativeIO, installNative } from './botty-native.js';
import { sendPayload } from './payload-sender.js';
import { loadRequiredPayloads } from './session.js';
import { installAndStart } from './transmission.js';
import { installAndStartManager } from './botty-manager.js';

export async function launchSession(options) {
  const report = options.report || (() => {});
  const send = options.send || sendPayload;
  const wait = options.wait || sleep;
  report('Running jailbreak. Keep this page open.');
  const runtime = await options.jailbreak();
  const io = options.io || new PS5IO(runtime);
  // Publish the complete title before ShadowMountPlus scans the homebrew directory.
  const native = await (options.native || installNative)(options.nativeIO || new NativeIO(runtime), { report });
  await loadRequiredPayloads(runtime, { send, wait, report, markSent() {} });
  report('Starting FTP…');
  if (!await io.listening(2121)) {
    await send(runtime, 'ftpsrv-ps5.elf');
    let ready = false;
    for (let attempt = 0; attempt < 40; attempt++) {
      if (await io.listening(2121)) { ready = true; break; }
      await wait(250);
    }
    if (!ready) throw Error('FTP did not start on port 2121.');
  }
  report('Preparing Transmission…');
  await (options.transmission || installAndStart)(io, { report });
  report('Preparing Botty…');
  const manager = await (options.manager || installAndStartManager)(io, { report });
  report(manager?.updatePending ? 'Services ready. Service update applies next console session; active work is preserved.' : 'Services ready. Waiting for home screen discovery.');
  return {native, manager};
}
