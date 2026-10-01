// Socket completion confirms delivery, not payload startup.
export async function loadRequiredPayloads(runtime, options) {
  const { send, report, markSent } = options;
  const wait = options.wait || ((ms) => new Promise((resolve) => setTimeout(resolve, ms)));
  report("Loading Kstuff…");
  await send(runtime, "kstuff.elf");
  markSent("kstuff.elf");
  report("Kstuff sent. Allowing 10 seconds for startup; watch for its welcome notification.");
  await wait(10000);
  report("Loading ShadowMountPlus…");
  await send(runtime, "shadowmountplus.elf");
  markSent("shadowmountplus.elf");
}
