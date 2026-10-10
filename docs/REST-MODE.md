# Rest-mode retention investigation (2026-10-10)

Botty can renew a standby request while its manager executes. This does not
establish that the manager, torrent engine, network or storage will survive a
deeper suspend. The user reports interrupted torrents and an unavailable manager
after several hours. No live console capture was taken during this investigation,
so the exact trigger and installed service version remain unconfirmed.

## Reference examined

PS5Tailscale v1.0.3 Beta, public repository commit
`d99261c21b678d9637a92c88168e1f4a72563040`:

- [Rest-mode design and test results](https://github.com/atreus04-GG/ps5tailscale/blob/d99261c21b678d9637a92c88168e1f4a72563040/docs/REST_MODE.md)
- [SceJSCd resident loop](https://github.com/atreus04-GG/ps5tailscale/blob/d99261c21b678d9637a92c88168e1f4a72563040/src/keepmain_resident_inner.c)
- [ShellCore companion loop](https://github.com/atreus04-GG/ps5tailscale/blob/d99261c21b678d9637a92c88168e1f4a72563040/src/keepmain_shellcore_inner.c)
- [Resident injector](https://github.com/atreus04-GG/ps5tailscale/blob/d99261c21b678d9637a92c88168e1f4a72563040/src/keepmain_resident_injector.c)
- [Service lifecycle control](https://github.com/atreus04-GG/ps5tailscale/blob/d99261c21b678d9637a92c88168e1f4a72563040/src/libtailscale_auth_probe.c)

Both projects call `sceSystemStateMgrRequestToKeepMainOnStandby(const char*)`.
The important difference is where that call executes.

| Behavior | Botty released manager | PS5Tailscale v1.0.3 |
| --- | --- | --- |
| Renewal host | Botty manager thread | Injected SceJSCd and SceShellCore threads |
| Successful renewal cadence | 10 seconds | 30 seconds |
| Nonzero API return | Permanently stops renewal | Retries after 5 seconds |
| Lifetime | Manager lifetime | SceJSCd until reboot; ShellCore controlled by service file |
| Diagnostic evidence | API status and startup/error messages | Persistent heartbeat/error logs and resident counters |
| Long standby validation | Short FW 13.00 trials | Mixed: reported >6 hours; another run failed after 11–19 minutes |

PS5Tailscale reports retaining already-running FTP, ELF loader, PKG Manager and
Remote Play while Sony discovery reports standby. These tests do not establish
Botty compression or torrent compatibility. Initialization status files are not
live resident-thread health checks. The SceJSCd loop renews unconditionally until
reboot, even when the ShellCore control file is inactive.

## Failure paths in Botty

1. One nonzero IPC response permanently ends the renewal worker. This is a
   concrete recoverability defect, but has not been confirmed as the user's trigger.
2. If the manager is suspended, its renewal thread is suspended too. After the
   approximately sixty-second lease expires, it cannot renew itself until it runs
   again. Moving the request to a retained system process is the reference's
   attempt to address this circular dependency; it is not a proven universal fix.
3. rTorrent or the HTTP listener can fail independently after network suspension.
   Earlier FW 13.00 research observed fatal rTorrent listener error E163 and a
   refused Botty endpoint after wake, without sufficient timestamps to attribute
   them to a particular transition. Renewing the lease does not reopen listeners
   or resurrect an exited process.
4. Storage and worker lifetime need separate validation. HTTP reachability alone
   does not establish that external-disk compression or extraction progresses.

## Changes prepared in source

Retry failed requests every five seconds while preserving the ten-second success
cadence. Stop retrying when runtime API lookup returns `-ENOSYS`. Keep failure
visible until a later successful request; never report a rejected request as
active. Timestamp requests before IPC so a long call cannot extend the apparent
lease. Keep bounded counters and a single private JSON snapshot, written every
minute and on failure/recovery/gap transitions. Snapshot failures do not stop
renewals. The distributed UI is unchanged until a rebuilt release is staged; API diagnostics identify the experimental strategy.

A separate observer coalesces diagnostic work so storage delays cannot stall
renewal. HTTP now rebinds its listener after recognized suspend/network errors
without restarting the manager or replaying requests. rTorrent's own fatal
listener handling remains a separate unresolved recovery path.

An optional [SceJSCd companion prototype](../homebrew/rest-mode-companion/README.md)
implements a firmware/boot/PID-bound, three-minute expiring manager control and
bounded resident heartbeat. It is deliberately excluded from automatic launch
and public packages until its kernel offsets and hardware behavior are validated.
The SceJSCd thread can renew independently while Botty's control remains valid.
A stale heartbeat is never reported as a live resident. The last system request
can outlive control expiration by its roughly sixty-second lease.

These changes do not prove long-duration standby or storage/compression survival.
Experimental binaries were cross-built on the VPS; no console service was
replaced, no public release packages were regenerated, and no process injection
was performed.

## Next hardware experiment

First collect the installed version, the manager runtime log, rTorrent runtime
log and (once this source is deployed) `rest-mode.json` before restarting anything.
Confirm the console's rest network settings. Correlate an external, bounded
service watcher with real torrent progress and the user's rest/wake times.
Do not use historical PID files or API acceptance as process/power-state proof.
Test the retry change through a long overnight torrent run before attributing
remaining failure to deeper suspend.

If renewal stops because the manager no longer executes, test the prepared
resident companion separately from production services. The reference uses
ps5debug-NG injection and kernel writes to unlock target syscall filters, including
fixed process/thread offsets (`0x3e8`, `0xf0`, `0xf8`, `0x294`). Validate those
against the actual firmware, SDK and jailbreak/Kstuff Lite environment before
any console launch. Reproduce singleton detection and inner runtime initialization
handling; duplicate injected threads and unintended process credential changes
must be avoided.

Prefer a companion with an expiring manager heartbeat/control lease and a defined
stop path, rather than unconditional renewal until reboot. A stopped or dead
manager should not leave permanent standby retention. Preserve active jobs during
installation and testing; first test a private generated torrent, then separately
validate extraction and internal/external-storage compression. Measure recovery
on wake and actual work progress over several hours. Keep automatic service
relaunch separate until ownership, singleton locks and operation recovery are
validated. The implementation must also fit Portal+'s launcher and package contract.

Diagnostic shutdown waits at most 250 ms, then leaves any stalled callback with shared ownership of its data. Companion authorization runs on its own ten-second timer, including when manager IPC blocks or returns ENOSYS. A stalled control-file write can still exhaust the three-minute authorization; it does not stall the manager lease or diagnostic thread.

Companion status is polled every ten seconds independently of new manager lease snapshots, including after ENOSYS. HTTP readiness requires an owned accepting listener, not merely workers draining existing requests. Control and resident heartbeat publications use exclusive randomly named temporary files.

Listener recovery runs inside the accept loop before request workers are drained, so one stalled handler cannot postpone rebinding. Initial diagnostics are published even on unsupported firmware. Injection publishes its pending record atomically and rechecks manager authorization immediately before the syscall filter is touched.
