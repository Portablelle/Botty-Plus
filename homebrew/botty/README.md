# Botty service 1.5.4

Botty is the C++17 background service for **Botty+**. It listens on
port `8088`, controls the separate rTorrent process over loopback SCGI and manages extraction,
automatic preparation and library publication. Its API identity remains
`BTTY00001`; the daemon no longer registers a home-screen shortcut. The native
application is `PPSA99071`.

Use the repository's root README and `deployment/README.md` for installation.
The portal's **LAUNCH** action installs/starts the service after preparing the
native title and rTorrent. The service package lives under
`/data/botty/manager/1.5.4`; its installed marker is in the parent directory.

## Installation updates

`GET /api/state` retains the `nativeUpdate` key with `scope: "installation"`,
`updateAvailable`, installed/available native, manager, worker and engine versions,
a bounded English message, `requested`, `closeRequired` and a status of
`checking`, `current`, `available`, `waiting`, `installing`, `complete`, `error`
or `blocked`. Release checks run asynchronously and never download in state
handlers. `POST /api/native-update/check` accepts `{}` to request another check.

Authenticated `POST /api/native-update` accepts `{}` only after a compatible
installation update has been verified, including service-only changes. Its HTTP
202 response is flat JSON containing `apiVersion: 1`, `scope: "installation"`,
`status: "queued"`, native `version`, `serviceVersion` and a 32-hex `transaction`.
The request is flushed before acknowledgment; repeated requests acknowledge the
same transaction.
The native app may then close normally. The manager waits for extraction,
transfer, deletion, the complete compression/activation pipeline, a confirmed
idle worker and all native app processes to exit. Existing work finishes; new
extraction, compression, transfer and deletion operations are blocked from queue
acceptance until completion or verified safe failure. Torrent additions are also
blocked while the update is queued; new preparation is deferred. An independent embedded
updater durably takes over before retiring the manager. Engine changes pause only
active torrents, retain their hashes and resume only updater-paused torrents.
Service replacements are identity-verified before native publication and success.

Updates use only the fixed HTTPS GitHub release source, the manager package CA
bundle and release-index-anchored SHA-256 hashes. The exact thirteen native files
are staged, flushed and verified before transactional publication. The Portal-compatible
native journal and retained backups allow guarded recovery after interruption;
unexpected files or symlinks are never removed to make recovery succeed.
Registered metadata refresh is confined to the known Botty+ metadata paths.
ShadowMount unmount is requested only after native process exit is confirmed,
and its normal scan follows verified publication and metadata refresh.

Service manifests are staged in immutable version directories, and the gzip
relay is embedded in the manager so the seven-file Portal package stays compatible.
The manager must remain below Portal's 16 MiB cached-file read bound. A private
installation journal records takeover and every uncertain pause/retire/load/resume
intent; ambiguous outcomes are not repeated. Helper ownership requires current
boot identity, a live distinct PID and a held updater lock. Safe pre-handoff
failures release admission only after proving unchanged originals and peers.

The updater checks directory access and ownership before writing. PS5 runtime
permission acceptance, process-table compatibility, unmount/scan behavior and
power-loss recovery still require console acceptance; host tests do not prove
these properties on a PS5. A blocked updater preserves files and requires the
reported permission, worker or recovery issue to be resolved. App reopening is
manual; the updater does not launch the native app.
Legacy worker/botty4 peers lack required process evidence and need Portal bootstrap
in a deliberately restarted idle console session. The portal itself still stages
service updates without replacing a running older daemon. Do not run Portal and
the in-app updater concurrently.

## Capabilities

- Torrent listing, progress, ETA, connected peers, magnet input, pause/resume and
  verification through console-local rTorrent JSON-RPC.
- Optional Prowlarr Search and Explore with privately provisioned configuration,
  verified HTTPS, bounded responses, opaque selection IDs and artwork caches.
- A persistent automatic queue for downloads explicitly selected through
  Search/Explore. Completion leads to extraction, CRC verification and publication
  of recognized content. Unrelated torrents are not enrolled.
- RAR extraction for old-style `.rar`, `.r00`–`.r99`, `.s00` and later volumes,
  and `.part1.rar` sets. Missing volumes, unsafe paths, links, duplicate members,
  CRC failures and incomplete torrent data are rejected.
- Password support without storing archive passwords in job records.
- Cooperative cancellation at an UnRAR callback/header boundary. Partial output
  remains until the job is removed through the UI. Interrupted jobs are recorded
  after restart and are not silently replayed.
- Publication of one recognized PS5 app directory with a `PPSA` title ID, or one
  exFAT image. Existing destinations are refused. Same-filesystem publication avoids a second full copy. Cross-disk publication
  copies, flushes and verifies content before source cleanup.

Original torrent archives remain intact during extraction and publication.
**Remove torrent and files** is a separate, confirmed action confined to Botty's
download directories. It refuses removal during extraction and preserves library
and extraction records. Removing a failed/cancelled/interrupted extraction cleans
that job's partial files; dismissing ready/moved jobs preserves their content.

Library publication applies 0755 to directories and executable files, and 0644
to data. Staging remains private. ZIP, 7z and PKG installation are not supported.

## Runtime boundaries

The API enforces Host and Origin, requires a random per-process token on its
`/api/` endpoints except bootstrap, and does not enable CORS. API version is 1.
LAN requests additionally require HTTP Basic authentication using credentials in
`rtorrent/state/botty-credentials.json`. The local native client uses the per-process
token. `/api/connections` supplies the authenticated web URL and saved credentials.
The powerful rTorrent SCGI listener on port 5001 remains strictly on loopback.

| Path | Purpose |
| --- | --- |
| `/data/botty/downloads/complete` | Torrent data, including partial downloads; completion is verified through rTorrent |
| `/data/botty/extracted/<job-id>.working` | Private extraction staging |
| `/data/botty/extracted/<job-id>` | Verified extraction output |
| `/data/botty/jobs` | Durable progress and job records |
| `/data/botty/automatic` | Queue entries keyed by torrent info hash |
| `/data/botty/cache` | Explore results and cover cache |
| `/data/botty/rtorrent/state` | rTorrent session, incoming metadata, credentials and stable IDs |
| `/data/homebrew` | Published library |

Preflight reserves 512 MiB beyond estimated expanded size. Other software can
still consume storage concurrently; write failure leaves a failed job rather than
publishing incomplete output. Progress updates are throttled to four per second,
with durable checkpoints every ten seconds and immediate phase/terminal writes.
Saved progress can lag after power loss; it is not a resumable decoder checkpoint.

Service 1.5.2 estimates remaining time from measured progress over a rolling
60-second window, using the full observed interval during the first minute.
Torrent, extraction, compression and move ETAs wait for five seconds of
measurements. On each measurement, the estimator makes the ETA unavailable after
15 seconds without progress. Move and compression responses also clear the rate
and ETA when their last update is more than five seconds old. Extraction records
refresh on progress callbacks, so their last estimate can remain visible until
the next callback. History resets for a new phase, changed totals, counter rollback
or a torrent pause/resume; observations more than 30 seconds apart start a new
measurement interval for all users of the estimator.
Displayed transfer rates remain independent of the ETA. Estimates cover the
current phase, not later verification or cleanup work.

Non-solid, unencrypted RAR method versions up to 29 use three independent member
workers by default. Solid, encrypted and newer formats use the sequential path;
RAR5 may use UnRAR's internal decoder threads. Each member has a single owner and
all workers join before terminal publication. Sample benchmarks are not a promise
of full-archive throughput.

Explore rankings cache for ten minutes and covers for thirty days. Stale rankings
can display while refreshing; Square requests a refresh. Cover requests accept
known result/torrent/job IDs, not arbitrary URLs. Unknown artwork remains a
placeholder. See `deployment/README.md` for the private configuration contract.

## Build and test

From this directory, with a C++17 compiler, make, Python 3, curl development
headers/libraries and OpenSSL available for integration fixtures:

```sh
python3 tests/make_fixtures.py
make -j4 native
make test
```

`build/botty-native` is the **host service test binary**, not the native PS5 UI.
Host tests use isolated temporary storage and mock Transmission/Prowlarr services.
They cover multivolume extraction (including 163 original synthetic volumes), CRC
errors, cancellation, path confinement, source preservation, publication permissions
and the automatic pipeline.

Cross-build with the component's own container:

```sh
docker build --platform linux/amd64 -t botty-service-build .
docker run --rm --platform linux/amd64 -v "$PWD:/work" botty-service-build
```

The Dockerfile uses PacBrew v0.40.2 ports and separately overlays payload SDK
v0.43, both pinned by SHA-256. The native UI uses a different toolchain. The output
is `build/botty-manager.elf`. From the repository root, package it with:

```sh
python3 scripts/package-botty.py
python3 scripts/botty-packages.py --check
```

For documentation-only changes, `package-botty.py --source-only` refreshes the
source archive and notices while retaining the packaged binary and manifest.
Changing service versions also requires updating its default UI path and the
packaged binary; keep the installed path and health version consistent. Portal+
reads the verified package contract from Botty+ `main` and owns its installer
configuration.

## Validation and operations

Recorded PS5 13.00 observations include service startup, isolated RAR/CRC and
cancellation fixtures, library permission checks and authenticated API requests.
They do not establish complete hardware acceptance for every firmware or workload.
See the native component's `VALIDATION.md` in the full repository.

Check the rest-mode maintenance status before entering rest. Never restart the daemon for deployment during active
extraction. Stage the next version separately and keep rollback copies.
Transmission remains a separate process; closing the UI should not stop it.
FTP and a generated torrent download in standby were validated on firmware 13.00.
Upload, extraction, compression in standby, other firmwares, sustained large
transfers and simultaneous gameplay remain unvalidated on hardware.

## Third-party provenance

- UnRAR: `bizkut/unrar-ps5`, commit
  `c7357571a30b9eb9bb191b063126ec191f8e2ed5`; `vendor/unrar/license.txt`.
  Only the RAR DLL engine is linked. Botty supplies its own confined writer and
  progress reporting. Local changes isolate concurrent error state and RAR3 tables.
- cpp-httplib v0.18.3, commit `a7bc00e3307fecdb4d67545e93be7b88cfb1e186`;
  `vendor/HTTPLIB-LICENSE`. PS5 compatibility uses `accept` instead of `accept4`.
- nlohmann/json v3.11.3, commit `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`;
  `vendor/JSON-LICENSE`.
- Historical launcher integration derives from `ps5-payload-dev/websrv` v0.33,
  commit `baabe27e5449baeb059b850d0393c31fdee219b7`.

Retain the component `LICENSE`, vendored notices and corresponding source archive.

Torrent deletion stops the torrent before removing its exact files from the download and incomplete directories. Botty verifies removal before discarding torrent metadata; on failure the paused torrent remains available for retry. Library games and unrelated files are preserved. Transmission RPC success alone is not treated as proof that disk space was reclaimed.

To resume an interrupted extraction, select the same torrent and first RAR volume and choose Extract again. The service reuses its existing job and private staging directory. Unencrypted, non-solid RAR4 output is checked against each full-file CRC (the final split header for multivolume members); only valid completed files are retained. Incomplete/corrupt files restart from their beginning. Other formats fail safely with partial files preserved. Free-space checks account for retained output and actual allocated blocks of incomplete files. Do not delete/dismiss the interrupted extraction if you intend to resume it.

### Delete a Library game

The authenticated `POST /api/delete-library-game` endpoint requires a tracked job ID and `confirmed: true`. It removes only a previously moved PPSA game folder inside the configured Library and then removes its job record. Torrent data and original archives are preserved. Missing or partially deleted game folders can be retried. Links, special files, mounted game paths, inconsistent destinations, and Botty's own title are refused. Close and unmount the game and remove its home-screen entry before deletion. Other image-based games still require manual unmounting and removal; ready images created by Botty compression use the queued deletion workflow below.

## Library compression (1.2)

Library → Options → **Compress game** creates a separate compressed PS5 folder
game. Close Botty+ when prompted so ShadowMount can mount and check file names and sizes. Full content comparison is optional.
Test the game before selecting **Delete uncompressed copy**. That action deletes the original without another file comparison, regardless of
the compression verification flag. It retains saves, compressed content and
download archives. A retained backup, the selected compressed image and a closed
game are still required; active compression and deletion cannot be interrupted.
**Restore uncompressed game** can return to a retained original. Existing images
and APR games without an existing index are not supported by this workflow. See the game-compressor component
for build instructions, dependency notices and validation limits.

In 1.2.1, compressed games show **Delete game** even when the original and
torrent archives are absent. After confirmation, close Botty+ and games so the
service can unmount and delete the image, verification sidecar and any retained
original. Saves and downloaded archives are preserved. Already-compressed games
do not offer **Compress game**. A service capability flag prevents newer clients
from offering deletion against an older service. Interrupted deletion stays locked
for inspection rather than repeating automatically.

In service 1.2.2, retrying failed or cancelled compression removes only that
tracked title's unfinished temporary image and hash sidecar before checking free
space and restarting from zero. The worker must be idle and the original source
must still match. Completed images, untracked files and interrupted activation
or deletion remain protected. Compression has no checkpoint resume.

## External storage (1.3)

Botty+ 01.003.000 and the web interface ask for **Internal SSD** or an available
**External SSD**, showing free bytes, then **Full auto** or **Download only**
when adding a game. Full auto downloads, extracts and publishes on the selected
disk. Manual extraction, Library publication and compression ask for their own
destination, defaulting to the source disk.

The PS5 service discovers writable, mounted exFAT volumes at `/mnt/usb0` through
`/mnt/usb7`. Use ordinary exFAT media, not console-encrypted extended storage.
Botty writes `.botty-volume.json` at the disk root to recognize the same disk.
External downloads/extractions/compressed copies live under `botty/`; published
folders and extracted images live in `homebrew/`. Durable job records, torrent
metadata and credentials remain internal. Missing/replaced volumes are rejected;
no absent mount directory is created as a fallback. Reconnect the original disk
at the same USB mount path for existing rTorrent/ShadowMount registrations.

**Transfer to another disk** supports torrent data (including partial downloads),
ready extractions, published folders/images and ready compressed images with
their verification sidecars. Copying is asynchronous, refuses existing targets,
reserves 512 MiB, flushes output and checks the file inventory and sizes without rereading content. Source cleanup follows
successful publication and metadata updates. Transferred torrents remain paused;
use Verify/Resume explicitly. A compressed game's retained original backup stays
on its original disk and remains available through the existing restore/delete
workflow. Compression requires the `library-1.3` worker for external paths and
reserves its worst-case output plus 1 GiB on the selected disk.

Library transfers wait for games/Botty+ to close when ShadowMount refuses an
unmount. Mount registration is confirmed before deleting a relocated Library
source. Interrupted/ambiguous transfers retain copies and lock new file operations
for inspection; they are never automatically replayed. Their journal is
`/data/botty/transfer.json`; a Library source backup is under
`<source botty root>/transfers/<job id>`. Do not manually clear this journal until
source, destination and rTorrent/ShadowMount selection have been reconciled.

Host regression commands: `make test test-storage test-compressor
 test-compression-library` and the native component's `make test preview
 integration compression-integration`. These use temporary fixtures and mock
SCGI/ShadowMount/compression APIs. The external USB workflow has not yet received
PS5 hardware acceptance; prior compression acceptance does not validate 1.3.

## Live Processing status (1.3.1)

Authenticated `GET /api/processing` exposes deletion and compression tasks without
waiting for the catalog mutex held by synchronous deletions. File deletion
callbacks retain descriptor-relative path checks. Counters measure items removed
or checked absent; ETA is phase-local and omitted until enough rate samples exist.
Compression verification publishes live counters independently of durable records.
Botty+ 01.003.001 combines these snapshots with extraction jobs in Processing.

## Verification completion notification (1.3.5)

After a compressed copy passes the complete file comparison, the service releases
its runtime mount and saves the ready state before sending a PS5 notification:
“Botty+: Verification complete (title ID). You can reopen Botty+. Original kept.”
Verification failures never announce success. Notification delivery is best-effort;
a delivery failure does not invalidate a verified copy.

## Provider-neutral search (1.3.5)

Search and Explore use all enabled Prowlarr torrent indexers in the Console category tree. Botty merges duplicate releases and sorts the returned subset locally by seeders, grabs or date. Missing grabs count as zero. No tracker-specific indexer IDs or ranking profiles are required. See `deployment/README.md` for proxy setup and cache behavior.

## Optional content checks (service 1.3.6)

Automatic rTorrent completion rehash is disabled in config and applied over local
RPC to a running daemon when this service first uses it. Normal piece validation
and manual Verify remain. Cross-disk copying keeps bounded paths, complete
writes, fsync and inventory/size checks, but omits the second full content read.
Compression activation checks the mounted inventory and marks the copy ready
with `verified: false` and `verificationSkipped: true`; original files remain.

POST `/api/verify-compressed` with a tracked `id` requests optional full comparison
using the retained original. POST `/api/skip-verification` with that same `id`
requests a cooperative stop at the next read boundary. Both use the normal API
authentication. A skip never sets `verified: true`. Old running service versions
cannot acquire this stop mechanism without an update; do not kill an old active
verification or edit its JSON to simulate completion.

## Background Library operations (1.4.0)

Moves and deletions now expose live tasks through `/api/processing`. Folder
Library deletion returns HTTP 202 immediately and releases the catalog lock
while its worker runs. Completed, failed and interrupted operations remain
visible; a lost response never causes an automatic destructive retry.

With ShadowMount `botty_background_storage_v1`, tracked folder/image moves and
compressed-image deletions use its guarded worker while Botty+ remains open.
The service checks returned job identity, expected source, destination and final
file presence before publishing completion. Deleting a retained original checks
that the compressed source is selected and that the original folder is unmounted;
it does not require unmounting the distinct compressed image.

Older ShadowMount builds keep the legacy workflow; transfer waits are bounded
and completion notifications indicate when to reopen Botty. Compression
activation/restoration/verification and new-title registration still use the
existing scanner/mount restrictions. This change does not claim those can run
inside Botty. UI progress distinguishes bytes from item counts and hides speed
estimates during preparation/finalization. Transfer integrity remains structural,
not a full content hash verification.

Host tests cover responsiveness during deletion, source preservation, remote
rejection/failure, lost status, and no replay across service restart. Console
acceptance of these changes remains pending for release 1.4.0.

## Optional search configuration (1.4.1)

Missing, unreadable or invalid `prowlarr.json` configuration leaves Botty usable.
Search and Explore display an inline configuration message; Downloads, Processing
and Library remain available. Correct the private configuration and retry Search
or refresh Explore without restarting the service. Existing search choices are
cleared when configuration is unavailable.

## Torrent file uploads (1.4.2)

The web interface accepts .torrent files up to 1 MiB alongside magnet links.
Uploads use the same rTorrent queue and appear in Botty+ with normal download
progress. When an external disk is connected, choose the destination and either
download only or full automatic processing. Otherwise downloads start on the
internal SSD. Invalid metadata is rejected before loading the torrent.

## Web asset update correction (1.4.3)

The PS5 service loads web assets from its own versioned installation directory.
Service 1.4.2 incorrectly retained the 1.4.1 path, displaying the old web UI.
The portal now also recognizes both previous service versions during upgrades.

## Background services in rest mode (1.5.0)

Service 1.5.1 restores the flat `/health` response required by the installed
native app. The nested health field added in 1.5.0 caused Botty+ to show Offline
even with a healthy backend. Rest-mode status remains on the API routes below.

The PS5 manager requests `sceSystemStateMgrRequestToKeepMainOnStandby` with the
reason `BottyBackgroundServices` at startup, then renews it every ten seconds for
the manager's whole lifetime. Unlike the diagnostic prototype, there is no
ten-minute limit. Closing the native Botty+ app leaves this manager running.
The feature is enabled for the portal's firmware range, 7.00–13.60. The module
and API are resolved at runtime; missing support or a nonzero response stops
renewals and exposes `failed` without disabling Botty's other features.

`GET /api/rest-mode` and `/api/state.restMode` report
`supported`, `active`, `status`, `lastResult`, `renewalSeconds`, and `leaseSeconds`.
`/health` retains its flat API v1 contract for installed native clients.
API routes retain normal authentication. `supported` means the configured
firmware range, and `active` means a successful request less than sixty seconds
old, not an independent power-meter reading or a guarantee of every workload.
Delayed renewals report `expired`; failed requests disable future renewals for
that process. Stopping the manager stops and joins the renewal thread; its last
request expires through the system's observed roughly sixty-second lease.
No global rest settings, kernel patches, syscall-table toggles or extra network
listeners are introduced.

The user-confirmed rest trials on firmware 13.00 kept FTP, rTorrent SCGI and
Botty HTTP responsive. A private generated 8 MiB torrent downloaded during rest,
was read back in full over FTP, matched its SHA-256 and was removed without
removing either original torrent. Those trials validate the underlying request
and tested services; uploads, extraction, compression, other firmwares, long
rest sessions and power consumption still need hardware validation. Holding the
main processor in standby may use more power than the deepest rest mode.

Host regressions cover the firmware boundaries, initial rejection, expired
requests, renewal over a simulated day, shutdown, and API status. PS5
cross-compilation uses the pinned SDK on the VPS.

The final 1.5.0 manager was installed on firmware 13.00 after live API checks
showed no active extraction, transfer or compression. All seven versioned
package files were read back and hash-verified in confirmed raw SELF mode,
with the prior service retained for rollback. Runtime API lookup succeeded,
Botty and both original torrents remained available, and rest maintenance was
still active 130 seconds after the activation receipt, beyond one sixty-second
lease. This establishes production startup and ongoing renewals while awake;
the standby download/readback evidence above comes from the prototype trials.
