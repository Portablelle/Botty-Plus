import { establishPrimitive } from "./webkit.js";
import { installWindowP } from "./utils/mem.js";



import { cheatRunnerStatus } from './cheatrunner.js';
import { launchSession } from "./launch.js";


const output = document.getElementById("console");
let launchStartedAt = null;

function writeLog(message, type = "log", replace = false) {
  let line = replace ? output.lastElementChild : null;
  if (!line) {
    line = document.createElement("div");
    output.appendChild(line);
  }
  let marker = "*";
  if (type === "error") marker = "-";
  if (type === "info" || type === "success") marker = "+";
  const elapsed = launchStartedAt === null ? "" : `${Math.floor((performance.now() - launchStartedAt) / 1000)}s `;
  line.textContent = `${elapsed}[${marker}] ${message}`;
  output.scrollTop = output.scrollHeight;
}

function writeEvent(name, detail, type) {
  writeLog(detail == null || detail === "" ? name : `${name}: ${detail}`,
    type || (name === "Failed" ? "error" : "log"));
}

window.writeLog = writeLog;
window.jb = { mark: writeEvent };

async function getPrimitive() {
  writeLog("Starting WebKit exploit");
  const primitive = installWindowP(await establishPrimitive(writeEvent));
  if (!primitive || typeof primitive.read8 !== "function")
    throw new Error("Memory primitive unavailable");

  writeLog("ARW ready", "success");
  return primitive;
}

function getWebKitBase() {
  const ctor = globalThis.__ps5NativeCtor;
  if (typeof ctor !== "number" || typeof OFFSET_wk_host_constructor_candidates === "undefined")
    throw new Error("WebKit base inputs are unavailable");

  for (const offset of OFFSET_wk_host_constructor_candidates) {
    const base = ctor - offset;
    if (base >= 0x800000000 && base < 0x900000000 && base % 0x4000 === 0)
      return base;
  }

  throw new Error("WebKit base not found");
}

async function run() {
  const rejection = window.firmware.rejection();
  if (rejection)
    throw new Error(rejection);
  writeLog("Credits: Sonic_Iso, Jordy, ntfargo, ufm42, Dr. Yenyen, TheFlow, SlidyBat, Flatz, cow, nhk, bollarz, Sleirsgoevy, EchoStretch, EarthOnion", "info");
  writeLog(`Agent: ${navigator.userAgent}`, "info");
  writeLog(`Firmware: ${window.fw_str}`, "info");
  const primitive = await getPrimitive();
  writeLog(`WebKit base: 0x${getWebKitBase().toString(16)}`, "info");

  await import("./relapse_exploit.js");
  return await main(primitive);
}

const button = document.getElementById("launch");
const status = document.getElementById("status");
const rejection = window.firmware.rejection();
let started = false;
document.getElementById("firmware").textContent = rejection ? "PS5 browser required" : "PS5 / " + window.fw_str;
button.disabled = Boolean(rejection);
if (rejection) status.textContent = "Open this page on your PS5 to launch.";
else button.focus();
button.addEventListener("click", async () => {
  if (started || rejection) return;
  started = true;
  launchStartedAt = performance.now();
  document.body.dataset.state = "launching";
  button.disabled = true;
  button.textContent = "LAUNCHING";
  button.setAttribute("aria-busy", "true");
  const report = message => { status.textContent = message; writeLog(message, "info"); };
  try {
    report("Starting session…");
    // Give the browser a paint opportunity before the synchronous WebKit work.
    await new Promise(resolve => requestAnimationFrame(() => setTimeout(resolve, 0)));
    const result = await launchSession({
      jailbreak: async () => { await window.offsetsReady; return await run(); },
      report,
    });
    button.textContent = "READY";
    status.textContent = result.manager?.updatePending
      ? "Press PS and open Botty+. Service update applies next console session; current work continues."
      : "Press PS and open Botty+. Allow time for the home screen to refresh.";
    status.textContent += ' ' + cheatRunnerStatus(result.cheatrunner);
    document.getElementById('cheatrunner').hidden = !result.cheatrunner?.ready;
    document.body.dataset.state = "ready";
  } catch (error) {
    button.textContent = "STOPPED";
    status.textContent = "Setup stopped. Restart your PS5 before trying again.";
    writeLog(error.message || String(error), "error");
    document.body.dataset.state = "error";
  } finally {
    button.setAttribute("aria-busy", "false");
  }
});
