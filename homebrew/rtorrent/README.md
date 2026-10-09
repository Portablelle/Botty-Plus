# Botty rTorrent PS5 port

rTorrent and Rakshasa libtorrent 0.16.24, cross-compiled with PS5 Payload SDK 0.43.
Botty service 1.1.0 uses the console-local JSON-RPC/SCGI listener on port 5001.
The existing Botty torrent API and automatic extraction queue retain their hash
identities. The `transmissionReady` response key remains a compatibility alias
for existing native clients; `torrentEngine` identifies the current engine.

Build on the configured `test` host (Docker), mounting this directory at `/work`:

```
docker build -t botty-rtorrent-sdk .
docker run --rm -v "$PWD:/work" botty-rtorrent-sdk
```

`build.sh` verifies the pinned upstream archives before compiling. The PS5 entry
point sets the state directory, takes a singleton lock and redirects diagnostics
to private storage. Every invocation holds an OS `flock` until exit; the upstream
PID-file session lock is disabled because stale PIDs can be reused after reboot. `rtorrent.rc` is the production configuration; `benchmark.rc`
and the probe/seeder utilities are isolated development tools, not installed by
the portal. All downloaded data and settings live below `/data/botty`.

The memory setting is rTorrent's minimum 512 MiB mapping budget, not an allocation
of a 512 MiB heap. The SCGI API must remain on loopback: it provides powerful
commands without authentication. Botty provides its own authenticated LAN web UI
on port 8088 using the saved Botty credentials. Ports 51414 and 8088 serve peers
and the web UI respectively; port 9091 is no longer used.

The botty4 configuration requests up to 200 peer addresses. Downloading torrents
allow up to 200 connections each, with a target of 100; completed torrents use
a separate 50-peer seeding limit. rTorrent replenishes peers automatically
when connections and available addresses fall below its target, respecting each
tracker's announce minimum. A private tracker can impose a one-hour minimum;
the displayed seeder count is not a guarantee of reachable or fast peers.
The socket manager continues to share its bounded resources across torrents.

## Migration

Stop both clients before changing data paths. Preserve Transmission metadata and
resume files privately. Move only the selected torrent's directory from incomplete
to complete, remove `.part` suffixes only on exact members described by its saved
metadata, and import the original `.torrent` into rTorrent. Request a full piece
hash check before resuming. Transmission resume files cannot be reused directly.
Never allow both engines to write the same files. Keep a journal of renames for
rollback; stop rTorrent before reversing them and restarting Transmission.

The portal refuses to start rTorrent over an active Transmission process or an
unmigrated existing installation. A completed migration is recorded privately as
`rtorrent/state/migration.json`. Original user torrent metadata is retained for
rollback. Test torrent data may be removed by its exact known identity and paths.

## Validation status

The package revision is `0.16.24-botty9`; upstream `system.client_version`
remains `0.16.24`. Each launch publishes private `state/runtime.json` before
upstream main can start a listener, including supervised launches. Its exact
fields are `schema: 1`, positive `pid`, `version: "0.16.24-botty9"`, and
`boot: {seconds, microseconds}` containing the exact `kern.boottime` timeval.
This revision originates in the running binary, never installed metadata.
Missing sysctl support, a wrong response size, nonpositive seconds or microseconds
outside 0..999999 fail startup closed. No kernel writes are performed.

The publisher traverses the absolute state path with directory descriptors and
`O_NOFOLLOW`, accepts root-owned installer storage or storage owned by the
payload's real UID, sets the state directory to 0700, and writes a
bounded JSON record to an exclusive 0600 temporary file with a 128-bit random
suffix, trying at most eight distinct suffixed names. Stale PID-named temporary files
and colliding files/symlinks are preserved, not deleted. It checks writes,
fsyncs the file, atomically renames it and fsyncs the directory. Existing identity
symlinks/nonregular files and unsafe paths are rejected. Backend consumers must
compare `pid` with live `system.pid` and both boot fields with the current boot;
the file alone is not proof that the daemon is running.

On firmware 13.00 the portal creates storage with owner UID 0 while the ELF
loader runs the payload with real UID 1. Both are trusted owners; other owners
remain rejected. Identity-publication failures are recorded in `runtime.log`.

The target uses carry-normalized descriptor-relative syscall wrappers rather
than the SDK's positive-errno libc stubs. SDK v0.43's `sys/syscall.h` defines
`fstatat=493`, `openat=499`, `renameat=501`, and `unlinkat=503`; the target checks
these mappings at compile time and declares the syscall-clobbered registers.
`runtime-version.hpp` defines `BOTTY_RT_RUNTIME_VERSION` as the single compiled
engine revision; packaging and test expectations use the same value.

The SHA256-pinned upstream archive registers exported `system.pid` (getpid),
`session.save` (session persistence) and `system.shutdown.normal` (normal
shutdown). `system.shutdown` is a non-exported redirect, not the RPC name to use.

Run the entry/publisher harness on the VPS (not the local Mac), with the working
directory set to `homebrew/rtorrent/`:

```sh
c++ -std=c++20 -Wall -Wextra -Werror tests/runtime-identity-harness.cc -o /tmp/rtorrent-runtime-identity-test
RT_IDENTITY_HARNESS=/tmp/rtorrent-runtime-identity-test python3 -m unittest discover -s tests -v
```

The injected sysctl supplies boot values and errors, and the injected state path
isolates test writes. Tests cover exact fields, atomic replacement/private modes,
invalid boot responses, unsafe paths/symlinks, positive kernel errno normalization,
stale temporary files and collisions, and existing default/supervised
arguments, singleton lock, chdir, log and PID behavior. All nine focused host
tests passed on the VPS. An optimized production-entry probe containing the real
boot reader, random generator and carry-normalized wrappers compiled and linked
against the VPS's Payload SDK. This is not a full rebuilt-daemon or PS5 launch
acceptance result; runtime acceptance remains pending.

The earlier prototype ran on the PS5 and passed generated single-file and
multifile integrity checks. It did not establish a performance improvement.
The production switch was requested without further benchmarks or a test suite;
compilation, package hashes, transfer receipts and the live migration state are
operational checks only, not comprehensive regression coverage.

## Source and licenses

Upstream: https://github.com/rakshasa/rtorrent and
https://github.com/rakshasa/libtorrent, tag v0.16.24. The source package includes
both pinned upstream archives plus the port/build sources. Upstream license
notices are preserved in those archives. rTorrent is GPL-2.0-or-later; this port's
integration sources are distributed under the same terms. SDK and dependency
sources and versions are pinned in the Dockerfile; their own licenses apply.

The botty3 config disables the extra full rehash on download completion. Normal
piece checks and explicit manual verification remain enabled. Botty service 1.3.6
also applies this setting to an already-running daemon via local RPC.

## Descriptor exhaustion recovery (botty7)

Large multifile torrents could stop before downloading or report
`Hash check I/O error ... Too many open files`. The process can exhaust its
actual descriptor allowance before libtorrent reaches the advertised cache
budget. On `EMFILE`, the storage manager now waits for queued file closes and
retries; if still exhausted, it evicts one least-recently-used cached file,
waits for that descriptor to close, and retries once more. Other errors remain
errors. It never closes peer sockets, raises process/kernel limits, or deletes
data. Exhaustion with no cached file to reclaim remains a reported failure.

The Linux host regression compiles the patched pinned upstream FileManager,
SocketFile and asynchronous FdCloseQueue sources, plus the production
`fd_open_file` body with an injected logger that overwrites errno. The botty8
patch preserves the failed open error around that logging call. It writes and reads 187 files with
`RLIMIT_NOFILE=64` while the advertised file cache allows 128. The unpatched
source must fail with `EMFILE`; the patched source must preserve all contents.
Missing paths and exhaustion due solely to unrelated descriptors remain errors.
Run `python3 homebrew/rtorrent/tests/test_file_manager_emfile.py` on Linux with
Python 3.12+, Clang C++20 and OpenSSL development headers. The compiler
command in `CXX` supports wrappers and flags (for example `ccache clang++ -O2`). These host checks and the rebuilt
PS5 package alone do not establish console acceptance. See the botty9
empty-file regression and console validation below.


## Empty-file hash checks after recovery (botty9)

A successful descriptor-exhaustion retry previously left `errno=EMFILE` behind.
Initial hash checking of an empty or undersized file returns an invalid chunk
without setting an error, so ChunkList could mistake that stale errno for a new
I/O failure. The storage manager now restores the incoming errno after a
successful open; genuine failed opens retain their error. The focused regression
maps empty files after recovery under `RLIMIT_NOFILE=64`, then writes and reads
all 187 files. The previous recovery patch fails this empty-file regression;
botty9 passes it on the VPS Linux host.

The rebuilt botty9 daemon was staged and hash-verified on the PS5, then started
after a saved session and normal shutdown of botty8. Its runtime revision and
PID matched live RPC on the same boot; previously active torrents were restored.
CONTROL passed its initial hash check and began downloading without the I/O
error, confirmed by live service state and the user. Full download completion
and complete-data integrity remain unverified.
