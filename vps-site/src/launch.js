import { CodexIO, startCodex, codexStatus } from './codex.js';
import { normalizeLaunchServices, supportsPpr } from './launch-options.js';
import { CheatRunnerIO, installAndStartCheatRunner, cheatRunnerStatus } from './cheatrunner.js';
import { PS5IO, sleep } from './ps5-io.js';
import { NativeIO, installNative } from './botty-native.js';
import { sendPayload } from './payload-sender.js';
import { loadRequiredPayloads } from './session.js';
import { installAndStart } from './rtorrent.js';
import { installAndStartManager } from './botty-manager.js';

export async function launchSession(options) {
  const services = normalizeLaunchServices(options.services);
  if (services.ppr && !supportsPpr(options.firmware)) throw Error('A53 PPR supports PS5 firmware up to 11.40 only.');
  const report = options.report || (() => {});
  const send = options.send || sendPayload;
  const wait = options.wait || sleep;
  report('Running jailbreak. Keep this page open.');
  const runtime = await options.jailbreak();
  const io = options.io || new PS5IO(runtime);
  // Publish the complete title before ShadowMountPlus scans the homebrew directory.
  const native = await (options.native || installNative)(options.nativeIO || new NativeIO(runtime), { report, reuseNewer: true });
  await loadRequiredPayloads(runtime, { send, wait, report, ppr: services.ppr, confirmPpr: options.confirmPpr, markSent() {} });
  if (services.ftp) {
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
  } else report('FTP startup skipped by launch options.');
  if (services.rtorrent) {
    report('Preparing rTorrent…');
    await (options.rtorrent || installAndStart)(io, { report });
  } else report('rTorrent startup skipped by launch options.');
  report('Preparing Botty…');
  const manager = await (options.manager || installAndStartManager)(io, { report });
  report(manager?.updatePending ? 'Services ready. Service update applies next console session; active work is preserved.' : 'Services ready. Waiting for home screen discovery.');
  let cheatrunner = { ready: false, skipped: true, reason: 'CheatRunner startup skipped by launch options.' };
  if (services.cheatrunner) try {
    cheatrunner = await (options.cheatrunner || installAndStartCheatRunner)(options.cheatRunnerIO || new CheatRunnerIO(runtime), { report, wait });
  } catch (error) {
    cheatrunner = { ready: false, reason: 'CheatRunner setup: ' + (error.message || String(error)) };
  }
  report(cheatRunnerStatus(cheatrunner));
  let codex = { ready: false, skipped: true };
  if (services.codex) try {
    codex = await (options.codex || startCodex)(options.codexIO || new CodexIO(runtime), { report, wait });
  } catch (error) { codex = { ready: false, reason: 'Codex PS5: ' + (error.message || String(error)) }; }
  if (services.codex) report(codexStatus(codex));
  return {native, manager, cheatrunner, codex};
}
