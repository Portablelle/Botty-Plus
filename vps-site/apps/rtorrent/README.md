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
