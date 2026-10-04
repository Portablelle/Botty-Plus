# PS5 rest-mode investigation

Experimental tools for determining whether FTP and rTorrent can run in the
firmware's **Main On Standby** phase. Console trials passed on the tested FW 13.00 setup. This remains an experimental
ten-minute tool, not a permanent service, and is not autoloaded or included in
public packages.

## Firmware evidence (2026-10-05)

The exact installed FW 13.00 `libSceSystemService.sprx`, `libkernel_sys.sprx`, and
`SceShellCore.elf` were retrieved over FTP in the server's default SELF-to-ELF
mode. Their received ELF magic, sizes, hashes and bounded disassembly are saved
privately under `backups/rest-mode-research-20261005/console/`. Addresses below
are ELF virtual addresses relative to the image, not live runtime addresses;
they apply only to these captured binaries.

- `sceSystemStateMgrRequestToKeepMainOnStandby` at `0x1c920` takes one pointer.
  Its internal implementation at `0x1cad0` uses `strlen`, sends the string plus
  its NUL terminator to IPMI method 3, and returns the service result. The inferred
  prototype for this firmware is `int function(const char *reason)`.
- The ShellCore wrapper at `0x992850` obtains the caller PID and passes the PID
  and reason to `0x98ce70`. That function replaces an existing record for the
  same PID/reason with a newly timestamped record.
- The keep-main decision at `0x989970` checks these records, treating their
  timestamps as valid for approximately 60000 ms, and emits
  `Keeping MAIN_ON_STANDNY (%s)`. It also checks other system conditions and
  exemptions. This supports testing a periodically renewed request; it does
  **not** establish that all homebrew threads or sockets remain operational.
- `sceKernelIsMainOnStandbyMode` at `0x22330` expects an output pointer, not a
  zero-argument Boolean getter. It reads a four-byte sysctl named
  `machdep.bootparams.is_main_on_standby`. The probe reads this sysctl directly
  without changing it. Because it is a boot parameter, it must not be treated
  as an independently validated live power-state indicator.
- The current ftpsrv 0.21.1 retries its listener after failure. etaHEN's rest
  handling detects socket error `0xa3` and restores servers on wake. The retrieved
  rTorrent runtime log contains fatal listener errors `E163` (`0xa3`), but has
  insufficient timestamps to attribute each occurrence to this particular rest
  cycle. Botty's LAN HTTP endpoint was refused after wake.

A successful API return only establishes acceptance of a request. Actual
standby behavior, FTP transfers, torrent progress, wake behavior and power use
need independent validation for any other setup and longer use. Main On Standby may consume more power than the
console's deepest rest state; no power savings are claimed.

## Tools

`rest-probe.elf` observes only. It logs wall/monotonic clocks and probes the
loopback FTP (2121) and rTorrent SCGI (5001) TCP listeners. It sends no FTP or
SCGI commands. A temporary HTTP status listener on 2122 returns only these
measurements; it cannot launch or stop services or change configuration.

`keep-main-probe.elf` additionally calls the inferred SystemStateMgr API with
`BottyRestProbe` every ten seconds. A nonzero result disables further requests.
It performs no kernel writes, credential changes, legacy Kstuff toggles,
application launches, rest requests or service restarts. Both variants stop
sampling after at most 600 records or ten minutes of observed wall time. If the
process is suspended, it cannot run its exit handler until resumed. An IPC call
that blocks can also delay exit. The last accepted keep-main request should age
out through ShellCore's observed roughly sixty-second expiry; no persistent
setting is modified.

Logs use a unique timestamp/PID filename, mode 0600, under
`/data/botty/manager/rest-probe-*.jsonl`. Existing captures are not overwritten.
Each record is flushed. A gap in the log alone is not proof of CPU suspension:
I/O or scheduling delays can also cause gaps.

## Build and validation

Host build: `make host`. Options: `-d SECONDS` (1–600), `-p PORT` (1024–65535),
`-o EXISTING_DIRECTORY`. The keep-main mode is selected at build time, so the
observation binary cannot be switched into it by an HTTP request.

PS5 builds: `make ps5 ps5-keep-main` inside the existing SDK image **on the VPS**.
The 2026-10-05 build used `botty-service-build:latest` and
`/opt/ps5-payload-sdk/bin/prospero-clang`; its SDK runtime is pinned by the
service Dockerfile. Do not build Docker images on the Mac.

Host compilation with `-Wall -Wextra -Werror`, HTTP JSON retrieval, private
logging, automatic three-second exit, and the observation build making no
keep-main requests passed. The host-only unsupported-API test also confirmed
that a failed request disables renewals while the probe continues and exits.
Both PS5 variants cross-built on the VPS. This is
build/host validation, not console acceptance.

Before any payload launch, establish that no compression is active, as required
by the repository's installation-incident restrictions. Keep existing services,
user archives, saves and games untouched. For a console trial, launch one variant,
verify its status and log first, record its remaining lifetime, then ask the user
to enter rest. Collect the log after wake and compare external FTP availability
with the actual sequence of samples. An unavailable service or stale PID file
must not trigger an automatic restart. Test rTorrent only when its existing
listener and download baseline are confirmed separately.

## Console trial results (2026-10-05)

The experimental keep-main payload ran on the PS5 after the user
confirmed that no compression was active. Its first request and subsequent
renewals returned zero. A separate Mac watcher performed FTP logins and actual
file retrievals every approximately three seconds; another watcher queried the
authenticated Botty API without saving credentials. The user confirmed an orange
steady rest-mode LED in both trials.

- First trial: all 46 sampled FTP file retrievals succeeded across 136.8 seconds
  of observed standby, while the probe continued sampling and renewing its request. rTorrent
  was already unavailable before that trial, so it was not an acceptance test of
  rTorrent survival.
- After wake, the exact installed rTorrent and Botty 1.4.1 executables were
  retrieved and validated as ELF64 with in-range load segments, then relaunched
  through the existing ELF loader. Their singleton locks were preserved; no
  jailbreak rerun, reboot, package update or configuration replacement was used.
  rTorrent SCGI and authenticated Botty API became available again.
- Second trial: all 59 sampled FTP/SCGI checks succeeded across 176.3 seconds
  of observed standby, and the authenticated Botty API remained available. A
  private 8 MiB torrent of generated bytes was added through the existing Botty
  API, using a tracker and seed restricted to the console and Mac on the LAN.
  Metadata exchange and all 8388608 download bytes completed while the probe
  reported standby. The entire file was then read back over FTP while in rest
  and its SHA-256 matched the generated source.
- Only the exact test torrent and its generated file were removed through Botty's
  normal deletion API. Both original torrent identities remained present. The user confirmed Botty
  was Online after wake. The probe subsequently logged its automatic stop after
  ten minutes; the local watchers and generated-data seed were stopped. No user
  archives, installed games, saves, or original torrents were removed.

The installed Botty 1.4.1 API accepts a magnet for this test, but rejected the newer
`metainfo` input with a missing `magnet` key; the local seed provided BEP 9 metadata
to use the installed API without upgrading it. The private capture directory
contains the network samples, API samples, payload log and download/hash/cleanup
receipt. These observations establish operation in the tested standby phase, not
long-term reliability, power savings or behavior after the request expires while
the console remains asleep. A permanent integration must renew the request while
its background services are intended to remain active and handle a full suspend
and network recovery separately.
