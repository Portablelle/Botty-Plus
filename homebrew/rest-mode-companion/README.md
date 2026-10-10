# Experimental Botty rest-mode system companion

Source prototype for FW **13.00 only**. It is not part of the public packages,
Portal bootstrap or automatic manager startup. Cross-compilation and host tests
are not console acceptance; the fixed kernel offsets and Kstuff Lite compatibility
must be validated before a console trial. Never launch it during compression,
extraction, transfer, deletion or an installation update.

The injector installs one named `botty_rest_resident` thread in `SceJSCd`, using
pinned ps5debug-NG injection machinery. The resident calls the same SystemStateMgr
API as Botty, every ten seconds or five seconds after rejection. Requests are
conditional on a manager control record with a matching firmware, current boot,
valid PID and a monotonic deadline no more than three minutes away. A missing,
malformed, expired or disabled control record stops renewal; a confirmed dead
owner also stops it. The resident remains idle in SceJSCd until reboot and may
accept a new valid manager record. The last Sony request can remain valid for
approximately sixty additional seconds after renewal stops.

The manager writes the control record on an independent authorization thread, about
every ten seconds, and sets its deadline to zero on orderly shutdown or when the
activation marker is absent. If storage stalls or the manager exits without
cleanup, the last control expires automatically. The resident publishes a
bounded JSON heartbeat every ten seconds. Botty reports it as stale after twenty
seconds or on a boot mismatch; initialization files alone never establish live
retention. No worker, torrent or other payload is started/restarted by this
companion. It does not repair suspended storage or guarantee an overnight run.

## Build

Use the service SDK image **on the VPS**, not a new Docker build on the Mac.
Fetch the dependency into an isolated build directory:

```sh
git clone https://github.com/OpenSourcereR-dev/ps5debug-NG.git /tmp/botty-ps5debug
git -C /tmp/botty-ps5debug checkout --detach d32d2d001dbbfd4cd2c0b7d6335b9a49d8a1cb86
make -C homebrew/rest-mode-companion PS5DEBUG=/tmp/botty-ps5debug experimental
make -C homebrew/rest-mode-companion test
```

`check-inputs.py` verifies the pinned upstream source/header hashes before building,
including in SDK images without Git. Output: `build/resident.elf` and
`build/botty-rest-injector.elf`. Preserve corresponding ps5debug-NG sources and
licenses if redistributing either binary. The inner CRT wrapper prevents the
standalone SDK's credential initialization from running again inside SceJSCd.
The injector temporarily uses upstream capability/ptrace control and unlocks the
target syscall filter. These are kernel/process mutations; a firmware gate alone
does not validate them. The duplicate-thread census is bounded and rejects
incomplete enumeration rather than injecting another resident. A kernel-backed
injector lock serializes concurrent launches. A fresh manager authorization and
the explicit activation marker are checked before target mutation; original
filter values are retained for attempted rollback on confirmed initialization failure. Remote-call errors can be ambiguous after registers were installed; those preserve the filter and pending guard until reboot. An
ambiguous initialization outcome prevents retry until reboot.

## Controlled hardware trial

1. Validate the actual FW 13.00 process/filter/thread layout and jailbreak payload.
   Keep ongoing work intact; establish a fully idle console before injection.
2. Run a manager built from the corresponding source. Deliberately create the
   private activation marker `/data/botty/rest-mode-companion.enabled` containing
   exactly `experimental-fw13` followed by a newline. Wait for its fresh control
   record. This marker only permits heartbeat publication; it never injects.
3. Launch the experimental injector once through the existing ELF loader. Inspect
   its status and fresh resident heartbeat before entering rest. A successful
   injector status is initialization evidence, not a standby test.
4. Validate a private generated torrent through rest and wake with an external
   watcher; observe real bytes, FTP/API reachability, LED/discovery state and logs.
   Extend to several hours. Test compression and external disks separately only
   after the initial resident trial has passed.
5. Remove the marker to stop manager authorization; do not inject again to stop.
   Confirm the control becomes inactive and allow the final system lease to expire.
   Reboot when idle to remove the injected thread. Never relaunch other services
   merely because their port stopped responding.

Keep state/log captures private and collect them before restarting the manager.
No trial or deployment was performed for this source change.

## Provenance

`injector.c` adapts the SceJSCd resident injector from
[atreus04-GG/ps5tailscale](https://github.com/atreus04-GG/ps5tailscale), commit
`d99261c21b678d9637a92c88168e1f4a72563040` (GPL-3.0-only). The resident design
also follows that project's request cadence/retry model, with an expiring Botty
control instead of unconditional lifetime renewal. Injection dependencies are
[OpenSourcereR-dev/ps5debug-NG](https://github.com/OpenSourcereR-dev/ps5debug-NG),
commit `d32d2d001dbbfd4cd2c0b7d6335b9a49d8a1cb86` (GPL-3.0-only).
See the repository [license](../../LICENSE). No upstream Tailscale or Go runtime
is needed by this component.

Injection records a boot-bound pending marker before changing the target. Confirmed resident initialization failure restores the saved filter. An unresolved timeout preserves that marker and refuses another injection in the same boot, even after the injector lock is released. Reboot is required before retrying; do not delete the pending record to bypass this guard. Status publication uses an exclusive random temporary file.
