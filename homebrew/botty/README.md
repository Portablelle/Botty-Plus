# Botty service 1.2.2

Botty is the C++17 background service for **Botty+**. It listens on
port `8088`, controls the separate rTorrent process over loopback SCGI and manages extraction,
automatic preparation and library publication. Its API identity remains
`BTTY00001`; the daemon no longer registers a home-screen shortcut. The native
application is `PPSA99071`.

Use the repository's root README and `deployment/README.md` for installation.
The portal's **LAUNCH** action installs/starts the service after preparing the
native title and rTorrent. The service package lives under
`/data/botty/manager/1.2.2`; its installed marker is in the parent directory.
A running service is preserved. The portal stages a newer service in its own
versioned directory and reports it as pending until the next console restart.

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
  exFAT image. Existing destinations are refused. Same-filesystem rename/link
  avoids a second full copy; there is no cross-filesystem fallback.

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
python3 scripts/portal-manifest.py --release YOUR_RELEASE_LABEL
python3 scripts/portal-manifest.py --check
```

For documentation-only changes, `package-botty.py --source-only` refreshes the
source archive and notices while retaining the packaged binary and manifest.
Changing service versions also requires updating its default UI path and the
portal installer's version constant; keep the installed path and health version
consistent.

## Validation and operations

Recorded PS5 13.00 observations include service startup, isolated RAR/CRC and
cancellation fixtures, library permission checks and authenticated API requests.
They do not establish complete hardware acceptance for every firmware or workload.
See the native component's `VALIDATION.md` in the full repository.

Keep the console awake. Never restart the daemon for deployment during active
extraction. Stage the next version separately and keep rollback copies.
Transmission remains a separate process; closing the UI should not stop it.
Rest mode, sustained large transfers and simultaneous gameplay remain unvalidated.

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

The authenticated `POST /api/delete-library-game` endpoint requires a tracked job ID and `confirmed: true`. It removes only a previously moved PPSA game folder inside the configured Library and then removes its job record. Torrent data and original archives are preserved. Missing or partially deleted game folders can be retried. Links, special files, mounted game paths, inconsistent destinations, and Botty's own title are refused. Close and unmount the game and remove its home-screen entry before deletion. Other image-based games still require manual unmounting and removal; verified images created by Botty compression use the queued deletion workflow below.

## Library compression (1.2)

Library → Options → **Compress game** creates a separate compressed PS5 folder
game. Close Botty+ when prompted so ShadowMount can mount and verify every file.
Test the game before selecting **Delete uncompressed copy**. That action verifies
the image again and retains saves, compressed content and download archives.
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
