# Dedicated qBittorrent downloads

Botty can send magnets, uploaded torrents and Search/Explore selections to an
existing qBittorrent 4.1–4.6 Web API. qBittorrent downloads on the dedicated server;
Botty then imports every file over authenticated HTTPS, preserving relative paths.
The PS5 initiates the transfer; no inbound PS5 connection or shared filesystem is
required. Server torrent completion and PS5 import progress are separate phases.
Extraction/preparation starts only after the complete local inventory is published.

Configuring the bridge does not replace local rTorrent. Each new magnet, `.torrent`,
Search or Discover download offers a **Download on** choice: **PS5 (rTorrent)** or
**Dedicated server (qBittorrent)**. Both preserve the destination storage and
automatic-preparation choices. Requests to `/api/torrent` (add), `/api/search/add`
and `/api/explore/add` accept `engine: "rtorrent"` or `engine: "qbittorrent"`;
omitting it defaults to local rTorrent for older clients. Invalid or unavailable
engines fail without falling back to the other backend. Duplicate detection is
scoped to the selected engine. Existing-server **Import to PS5** remains an explicit
remote-only operation, with no engine selector.

## Existing server torrents

Discovery reads the existing torrent catalogue without enrolling it. A release
with a distinct `PS5` token and a single first RAR volume is an **archive
candidate**, not proof of compatible contents. Actual extraction checks the
supported PS5 folder/image and reports passwords, missing volumes, CRC errors,
unsupported contents and destination collisions through the existing workflow.
Completed loose folders need a `sce_sys/param.json` containing a `PPSA` title ID.
Completed `.exfat` images need the exFAT signature and a PS5 release name. ZIP,
7z, PKG, PS4 metadata and unrelated movies/software are not offered for import.
Discovery scans up to 32 entries per poll, caching candidates across batches.
Repeated timeouts back off for 30 seconds so one slow torrent cannot block others.

Candidates appear in Activity/Downloads with **Import to PS5**. Selecting one
explicitly enrolls that hash. Both interfaces offer destination storage and automatic preparation or download
only before importing.
Nothing is automatically imported merely because it exists on the server.
An archive candidate can still turn out to be unsupported after extraction.

Torrents added from Botty use a separate `botty-ps5` category and hash-specific
server download directory. Adding an already-existing, unenrolled server hash
refuses to silently adopt it; import its candidate instead. Existing torrents keep
their current server locations, categories and files. Only explicit Pause/Resume
while the server download is pending controls their qBittorrent state. Once HTTPS
import starts, these controls pause/resume the local copy. **Delete torrent &
files** removes Botty's PS5 copy and tracking only, keeping the server torrent,
seeding and originals. Candidate discovery allows a later explicit reimport.
Piece verification remains available in the server's qBittorrent UI.

Each PS5 import directory contains a private `.botty-remote-owner` marker. Its
random identity survives disk remounts and directory renames; saved device/inode
numbers are not used for new staged or published imports. File inventory and
storage confinement checks still apply. The marker is excluded from prepared
games, regenerated on disk moves, and removed only with the owned import.
Empty publication reservations retain an inode guard: an unrecognized reservation
is left intact rather than adopted after a reconnect.

## Upgrading to parallel imports

Update `/opt/botty-remote/server.py` and `/opt/botty-remote/forge.py`, then restart the bridge while its transfers are
idle **before** updating the PS5 manager to 1.7.0. Keep the private configuration,
registry and server downloads. The bridge advertises `capabilities.boundedRanges`
in torrent snapshots. Older managers can still use its open-ended ranges.
Manager 1.7.0 checks that capability before touching partial files; an older bridge
produces an explicit "Update the dedicated-server bridge" error rather than a
misleading file-range fault. Imports retry after the bridge is updated.

## Private provisioning

1. Install both `server.py` and `forge.py` in `/opt/botty-remote` (the module is required even when conversion is disabled), and the systemd unit in
   `/etc/systemd/system`. The supplied unit runs as the existing `ubuntu` account;
   adapt that account and paths for another installation.
2. Create private `/etc/botty-remote/config.json` with fields `qbUrl`
   (`http://127.0.0.1:8080`), `username`, `password`, a random 64-character lowercase
   hex `token`, `registry` (`/var/lib/botty-remote/registry.json`), `downloads`
   (`/home/ubuntu/Downloads/botty-ps5`) and `port` (`9698`). Give the service account
   read access to this file and write access only to its registry directory. Create
   both registry and download directories before starting the supplied unit.
3. Include `nginx.conf` inside a valid HTTPS virtual host. Preserve existing UI
   restrictions in `location /`; a server-level IP restriction also applies to the
   bridge, and must be scoped appropriately if the PS5 needs public HTTPS access.
   The dedicated deployment uses `nginx-public.conf` at HTTP scope and its two
   guard variables in the TLS host. Only exact bridge routes bypass the Tailnet
   restrictions; the console does not need Tailscale. This exposes only the
   bearer-authenticated bridge. qBittorrent remains on
   loopback; nginx buffers neither full files nor credentials in access logs.
4. Validate nginx, reload it, and enable/start `botty-remote.service`. Existing
   qBittorrent and UI processes need no restart. Preserve `config.json` and the
   registry when updating the bridge.
5. Provision `remote.example.json`, with the same token and the bridge HTTPS URL,
   as private **0600** `/data/botty/remote.json`. No trailing slash. Optional `connectAddress` pins the public IPv4 address
   when DNS resolves to a Tailnet address, preserving HTTPS hostname/certificate
   verification. Update this field if the public IP changes. The CA bundle
   path must match the installed manager. Restart Botty only when its current
   operations are idle, using a manager built with this integration. Configuration
   is read at manager startup. Remove this file to return additions to local
   rTorrent; retain it while remote jobs are still tracked.

Credentials never appear in Botty's public API. Do not commit private bridge or
console configuration. Invalid configuration leaves the service usable and
reports an error in `remoteDownloads`; new additions are rejected until fixed.
Local rTorrent jobs stay visible alongside enrolled remote jobs.

## Transfer/recovery behavior

The joined worker checks completion of **all** torrent members, including skipped
files, before importing. Downloads use `.remote2-<hash>.partial` staging on the
selected Botty disk, a pinned file manifest, no-follow file opens, bounded response
sizes, a storage identity check, a 512 MiB reserve and flushed files. An interrupted
HTTPS stream resumes with HTTP Range; a changed manifest is refused. Files are
published under a hash directory only after an exact inventory/size check, so
multiple torrents with identical filenames remain separate. A restart can recover recorded staging/publication reservations and publication
after rename using its durable intent, directory identities and complete inventory.
Registry metadata has a 16 MiB bound; oversized snapshots are rejected while
retaining the last valid registry, so they cannot disable all imports on restart.
This is structural transfer validation, not a second full content hash pass.

Unplugged/replaced disks, unsafe paths/links, file-size changes and conflicting
local destinations stop the import and retain partial files. Pause before deleting
an import. No remote delete/move endpoint exists. Loose folders/images are copied
into the normal preparation staging and classified again before library
publication; server originals and the imported download copy are retained.
Manager retirement waits for the current remote transfer; queued remote work does
not begin during an installation update. The 1.6.0 manager and 1.8.0 native app are cross-built for this workflow.
Full PS5 hardware acceptance remains pending.

## Tests

```sh
python3 deployment/botty-remote/test_server.py
make -C homebrew/botty test-remote
```

The integration fixture uses the real HTTPS bridge and host manager. It covers
mixed server content, explicit import of paused server torrents, public-address
pinning, multivolume filenames with spaces/Unicode,
an interrupted stream and HTTP Range recovery, exact bytes, loose-folder
publication, disk moves and reimport after local deletion, preserving the server
source. It also checks that failed imports preserve pre-existing destinations and
that deletion refuses members shared with a local torrent.

## Bounded parallel ranges

Manager 1.7.0 requests `Range: bytes=start-end` in 8 MiB segments, with
1, 2 or 4 connections per import (default 2). The bridge returns 206 and exact
Content-Range/Content-Length for every range, including ranges starting at zero.
Legacy unbounded `bytes=start-` clients remain supported; requests without
Range return 200. Malformed, reversed and out-of-file ranges return 416.
The four file slots remain global across consoles; exhausted slots return 429.
No slot or metadata admission limit is raised by parallel imports.

## Optional server-only package preparation

Copy `forge.py` alongside `server.py` when upgrading. To prepare PKG/FPKG,
install [PS5 Dump Forge](https://github.com/quer3q/ps5-dump-forge) on the server
and set `forgeBinary` in the private bridge configuration to its absolute CLI
path, for example `/opt/ps5-dump-forge/ps5-dump-forge`. The CLI contract inspected
for this integration is commit `9f59894ffd55869f6ff99e48c6747956a43a3df4`:

```
ps5-dump-forge inspect /path/to/game.pkg --json
ps5-dump-forge convert /path/to/game.pkg --to exfat -o /private/output/game.exfat
```

The bridge passes arguments directly, never through a shell. It requires a PS5
title and `eboot.bin`, and checks the output title. No backport/fakelib removal
flags are added. Existing originals remain read-only. Conversion journals and
outputs live in `conversions/<hash>/` next to the registry; allow space there for
the image plus overhead. Only one conversion starts at a time per bridge.
A process lock prevents a restarted bridge from racing a surviving converter.

Full auto converts a single package; multiple variants enter Needs you for an
explicit choice. Download only keeps packages on the server. `/prepare` accepts
an imported hash and an optional exact `package` member name to choose/retry.
Update the supplied nginx public-route map to allow this authenticated route.
The PS5 never receives the package itself. Pausing the torrent or import does
not suspend an already running conversion. Converted output is kept for retry;
an incomplete private output is not overwritten automatically.

Run `python3 test_server.py` and `python3 test_forge.py` before deploying. These
changes do not install Forge, change your running service or modify any torrent.
See [the preparation design](../../docs/game-preparation.md) for storage,
ShadowMount/home-screen admission and the supported-format boundaries.
