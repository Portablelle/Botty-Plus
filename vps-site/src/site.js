import { establishPrimitive } from "./webkit.js";
import { installWindowP } from "./utils/mem.js";
import { sendPayload } from "./payload-sender.js";
import { loadRequiredPayloads } from "./session.js";
import { PS5IO } from "./ps5-io.js";
import { installAndStart, stopTransmission } from "./transmission.js";
import { managerInstalled, installAndStartManager } from "./botty-manager.js";

const output = document.getElementById("console");

function writeLog(message, type = "log", replace = false) {
  let line = replace ? output.lastElementChild : null;
  if (!line) {
    line = document.createElement("div");
    output.appendChild(line);
  }
  let marker = "*";
  if (type === "error") marker = "-";
  if (type === "info" || type === "success") marker = "+";
  line.textContent = `[${marker}] ${message}`;
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

const jailbreakButton = document.getElementById("jailbreak");
const status = document.getElementById("status");
const payloadButtons = Array.from(document.querySelectorAll("[data-payload]"));
let runtime = null;
let busy = false;
let failed = false;
let started = false;
const sent = new Set();
const transmissionStart = document.getElementById("transmission-start");
const transmissionStop = document.getElementById("transmission-stop");
const transmissionStatus = document.getElementById("transmission-status");
let transmissionRunning = false;
let transmissionIO = null;
let transmissionUncertain = false;
const managerButton = document.getElementById("botty-install");
const managerStatus = document.getElementById("botty-status");
let managerRunning = false;
let managerUncertain = false;

function updateButtons() {
  managerButton.disabled = !runtime || busy || failed || managerRunning || managerUncertain || transmissionUncertain;
  transmissionStart.disabled = !runtime || busy || failed || transmissionRunning || transmissionUncertain;
  transmissionStop.disabled = !runtime || busy || failed || !transmissionRunning;
  for (const button of payloadButtons) {
    const name = button.dataset.payload;
    button.disabled = !runtime || busy || failed || sent.has(name) ||
      (name === "shadowmountplus.elf" && !sent.has("kstuff.elf"));
  }
}

function revealTransmission(credentials) {
  document.getElementById("transmission-user").textContent = credentials.username;
  document.getElementById("transmission-password").textContent = credentials.password;
  document.getElementById("transmission-access").hidden = false;
}

async function prepareManager() {
  if (!transmissionIO) transmissionIO = new PS5IO(runtime);
  const report = message => { managerStatus.textContent = message; writeLog(message, "info"); };
  await installAndStart(transmissionIO, { report, reveal: revealTransmission });
  transmissionRunning = true;
  transmissionStart.textContent = "Transmission running";
  await installAndStartManager(transmissionIO, { report });
  managerRunning = true;
  managerButton.textContent = "Botty ready";
  managerStatus.textContent = "Ready. Press PS, then open Botty Downloads from the home screen. Installed Botty starts automatically after future Start session runs.";
}

managerButton.addEventListener("click", async () => {
  if (managerButton.disabled) return;
  busy = true; updateButtons();
  try { await prepareManager(); }
  catch (error) {
    managerUncertain = true;
    managerStatus.textContent = error.message + " Restart the PS5 before retrying. Downloads are preserved.";
    writeLog(error.message, "error");
  } finally { busy = false; updateButtons(); }
});

function transmissionReport(message) {
  transmissionStatus.textContent = message;
  writeLog(message, "info");
}

transmissionStart.addEventListener("click", async () => {
  if (transmissionStart.disabled) return;
  busy = true;
  updateButtons();
  try {
    if (!transmissionIO) transmissionIO = new PS5IO(runtime);
    const result = await installAndStart(transmissionIO, {
      report: transmissionReport,
      reveal(credentials) {
        document.getElementById("transmission-user").textContent = credentials.username;
        document.getElementById("transmission-password").textContent = credentials.password;
        document.getElementById("transmission-access").hidden = false;
      },
    });
    transmissionRunning = true;
    transmissionStart.textContent = "Transmission running";
    const space = Number.isFinite(result.freeBytes) ? " · " + (result.freeBytes / 1073741824).toFixed(1) + " GiB free" : "";
    transmissionReport("Running · Authentication and web interface verified" + space + ". Use your phone on port 9091.");
  } catch (error) {
    // A failed readiness check may leave a live daemon: never offer a blind relaunch.
    transmissionUncertain = true;
    transmissionStatus.textContent = error.message + " Installation stopped. Restart the PS5 before retrying; existing downloads are preserved.";
    writeLog(error.message, "error");
  } finally {
    busy = false;
    updateButtons();
  }
});

transmissionStop.addEventListener("click", async () => {
  if (transmissionStop.disabled) return;
  busy = true;
  updateButtons();
  transmissionReport("Stopping Transmission and saving its queue…");
  try {
    await stopTransmission(transmissionIO);
    transmissionRunning = false;
    transmissionStart.textContent = "Start Transmission";
    transmissionReport("Transmission stopped. Downloads and resume state are preserved.");
  } catch (error) {
    transmissionStatus.textContent = error.message;
    writeLog(error.message, "error");
  } finally { busy = false; updateButtons(); }
});

const rejection = window.firmware.rejection();
document.getElementById("firmware").textContent = rejection ? "Open on your PS5" : "PS5 · Firmware " + window.fw_str;
if (rejection) {
  jailbreakButton.disabled = true;
  status.textContent = "Open this page in the PS5 browser to start. No computer is needed for payload loading.";
} else {
  jailbreakButton.focus();
}

jailbreakButton.addEventListener("click", async () => {
  if (started || rejection) return;
  started = true;
  jailbreakButton.disabled = true;
  status.textContent = "Running jailbreak. Keep this page open.";
  try {
    await window.offsetsReady;
    runtime = await run();
    busy = true;
    updateButtons();
    await loadRequiredPayloads(runtime, {
      send: sendPayload,
      report(message) { status.textContent = message; writeLog(message, "info"); },
      markSent(name) {
        sent.add(name);
        const button = payloadButtons.find((item) => item.dataset.payload === name);
        button.textContent = button.dataset.label + " — sent";
      },
    });
    // The user opts in by installing Botty. No install marker means no extra payloads.
    try {
      if (!transmissionIO) transmissionIO = new PS5IO(runtime);
      if (await managerInstalled(transmissionIO)) await prepareManager();
    } catch (error) {
      managerUncertain = true;
      managerStatus.textContent = "Botty startup stopped: " + error.message;
      writeLog("Botty: " + error.message + " Gaming payloads were already sent.", "error");
    }
    busy = false;
    jailbreakButton.textContent = "Session prepared";
    status.textContent = managerRunning
      ? "Session prepared. Botty is ready: press PS and open Botty Downloads, or wait for library synchronization and launch your game."
      : managerUncertain
        ? "Gaming payloads sent, but Botty could not start. See step 2 for the error. Wait for library synchronization before opening a game."
        : "Step 1 complete. First time using Botty? Continue to step 2 below. For games, wait for library synchronization, then press PS.";
    updateButtons();
  } catch (error) {
    failed = true;
    status.textContent = "Session setup stopped. Restart the PS5 before another attempt. See the log below.";
    writeLog(error.message || String(error), "error");
  }
});

for (const button of payloadButtons) {
  button.addEventListener("click", async () => {
    if (button.disabled || busy || failed) return;
    busy = true;
    updateButtons();
    const name = button.dataset.payload;
    status.textContent = "Downloading and sending " + button.dataset.label + "… Keep this page open.";
    writeLog("Sending " + name + " to the console's ELF loader", "info");
    try {
      const size = await sendPayload(runtime, name);
      sent.add(name);
      button.textContent = button.dataset.label + " — sent";
      writeLog(name + " sent (" + size + " bytes). Check the startup notification on the PS5.", "success");
      status.textContent = name === "kstuff.elf"
        ? "Kstuff sent. Wait for its welcome notification before loading ShadowMountPlus."
        : name === "shadowmountplus.elf"
          ? "ShadowMountPlus sent. Wait for library synchronization, then press PS and open your game."
          : name === "ftpsrv-ps5.elf"
            ? "FTP payload sent. Check its notification for the console address (port 2121)."
            : "Prospero sent. Check its notification, then use the console address on port 7070.";
    } catch (error) {
      failed = true;
      status.textContent = "Payload loading failed. Check the log. Restart the PS5 before retrying.";
      writeLog(error.message || String(error), "error");
    } finally {
      busy = false;
      updateButtons();
    }
  });
}
