# Validation record

This is a consolidated record of earlier development observations as of
2026-10-01, not a claim that every workflow or supported firmware has passed.
Previously observed packages: native **00.006.000**, service **0.3.4**, Transmission **4.0.6**.
Hardware observations below were made on **PS5 firmware 13.00**.

## Host coverage

The repository includes automated checks for:

- Native model, JSON/HTTP parsing, bounded network responses, reconnect behavior,
  controller press/release guards, action confirmation and uncertain responses.
- Package integrity, traversal/duplicate paths, extra files, corrupted FSELF
  containers, mismatched ZIPs, title metadata and invalid validation claims.
- Package staging, identity/downgrade checks, state preservation and service
  coexistence. Portal launch order, installer hashes and launcher cleanup are
  validated in the separate Portal+ repository.
- Original synthetic RAR fixtures, including 163 volumes across `.r99` → `.s00`,
  Unicode conversion, CRC failures, missing volumes, cancellation, parallel worker
  error isolation, path confinement and original-source preservation.
- Mock HTTP/HTTPS service integration, Search/Explore filters, the persistent
  automatic queue, destination collisions, library permissions and artwork access.
- Artwork identity matching, cache behavior, provider failures and origin limits.

Renderer previews use the real drawing code with simulated services/controllers.
They cover full lists, long names, details, confirmation and offline screens.
They do not measure PS5 frame rate, TV readability or real controller behavior.
Run current checks using [the development guide](../../docs/DEVELOPMENT.md).

## Recorded hardware observations

| Area | Observed result | Limit |
| --- | --- | --- |
| Native registration and launch | Earlier native title registered and displayed on 13.00 | Does not validate all later UI interactions |
| Native reservation | `downloadDataSize: 256` resolved the pre-main launch rejection | Keep the tested reservation |
| Local API and Transmission | Authenticated RPC/web UI and native API connection observed | Other firmware/network layouts untested |
| Credential migration | Six-character migration completed with journal and backups | Recovery scenarios are mainly host-tested |
| RAR engine | Isolated original fixtures passed extraction, CRC, Unicode, parallel-worker and cancellation checks | No general large-archive performance guarantee |
| Library permissions | Isolated publication checks and launch after permission correction observed | Content compatibility remains separate |
| Explore/artwork | All three rankings and correctly sized RGB responses observed | New selection through the complete real download pipeline remains unvalidated |
| Current 00.006.000 / 0.3.4 delivery | Cross-builds, staged/active read-back hashes, registration refresh and API smoke checks recorded | Navigation and smoothness confirmed by the user on 13.00 |

Raw binary verification through ftpsrv required disabling SELF-to-ELF conversion
with the `SELF` command and checking its response before read-back. Previous title
and service copies were retained. Service replacements were gated on idle
extraction and native replacement on a closed app.

## Portal update implementation

The separate Portal+ repository covers launch order, installer hashes and its
update implementation. Botty+ host regressions cover package staging,
identity/downgrade checks, state preservation and service coexistence. These
update paths have not yet been exercised on PS5 hardware. Existing hardware
observations above do not validate the updater.

## Remaining acceptance checks

- [ ] Automatic native upgrade, registered metadata refresh and rollback on hardware.
- [ ] Cold-boot **LAUNCH** through every stage on a clean console installation.
- [x] Native navigation and smoothness: confirmed by the user on PS5 13.00 on 2026-10-01.
- [ ] TV margins/readability across displays and measured sustained frame rate.
- [x] DualSense navigation: confirmed by the user on PS5 13.00.
- [ ] Held-input edge cases and controller disconnect/reconnect.
- [ ] On-screen keyboard, password entry and confirmed destructive actions.
- [ ] Complete authorized small download → extraction → library workflow on hardware.
- [ ] Large transfers, low-space behavior, network interruption and sustained load.
- [ ] Quit through the app and PS menu; confirm background services survive.
- [ ] Suspension/resumption and switching between Botty+ and another title.
- [ ] Optional User's Guide DNS/TLS redirect on the actual PS5 browser.
- [ ] Any additional firmware version, tested separately.

Rest mode and simultaneous gameplay are not promised. Manifest fields
`hardwareValidated` and `registrationVerified` remain false until the relevant
release acceptance criteria are completed. Host tests alone must not change them.

## Version 1.0 release

Native 01.000.000 and service 1.0.0 promote the existing feature set to the first
public release. Version labels, package paths, installers and release artifacts
are updated together. This does not complete the hardware checks above; their
status remains unchanged and manifest hardware-validation flags remain false.

### User-confirmed navigation

On 2026-10-01, the user confirmed that navigation is tested and smooth on the
PS5. This validates normal controller navigation and perceived fluidity of the
current UI. It does not assert a measured FPS value or complete the separate
upgrade/recovery, rest-mode and long-duration workload checks.

### Native 1.0 launch investigation

On 2026-10-01, after the user reported a completed portal launch followed by a
black screen and an unresponsive PS button, ShadowMountPlus recorded a pre-main
loader error for `PPSA99071`: `mount flag / attribute error` on
`/app0/sce_module/libc.prx`. The installed and mounted executable/runtime hashes
matched the release. The runtime had mode 0644, while the previous working
installation's copy was executable. The portal installer now assigns 0755 to
both `eboot.bin` and `sce_module/libc.prx`, including permission repair when the
installed content already matches. The console runtime's mode was repaired and
read back as 0755. A successful relaunch on hardware is still required to confirm
that this resolves the black screen.

On the next user attempt, the screen remained black, but the native 01.000.000
log was newly written: `main` ran, VideoOut initialized, the controller connected,
the API connected, and eight frame submissions completed. The runtime remained
0755 and ShadowMountPlus no longer reported the loader error. Kernel logs also
recorded a PS-button event followed by a ShellUI focus transition to its menu.
The active HDMI mode was 3840x2160 at 59.94 Hz with HDR/PQ and VRR Boost. These
observations confirm progress past the original loader failure, not successful
TV output. The remaining black screen needs a separate display-path diagnosis.

The user then confirmed that switching the TV away from and back to the PS5's
HDMI input revealed the running application. Closing Botty+ from the home screen
and reopening it in the same console session displayed normally. Logs confirmed
a new native process (PID 92 after PID 89 exited), with the same 4K/59.94 Hz
HDR/PQ and VRR Boost HDMI mode as the black-screen attempt. Thus the observed
failure was not reproduced on the second launch, and disabling VRR is not an
established fix. The initial display transition remains unverified on a fresh
console session; its cause has not been isolated. Comparisons with saved portal
releases found identical Relapse, Kstuff and ShadowMountPlus artifacts and
payload ordering; the saved 00.006.000 renderer sources match 01.000.000 exactly.

A separate continuous-presentation diagnostic variant was host-tested (including idle-frame submission, buffer freshness and notification ordering) and cross-built successfully in the isolated VPS build directory `/home/ubuntu/botty-video-cadence-test`. It has not been installed on the console or published as a confirmed black-screen fix. Cold-launch hardware testing remains pending while the user-requested extraction resume is running; do not restart the console or replace the active service for video testing.

### 01.000.001 reconnection handling

Host worker regressions simulate transport timeouts and a failed Transmission RPC while extraction state still updates. The last catalog remains visible, stale actions are rejected, and automatic recovery clears the stale flag without controller input. A renderer preview of Reconnecting retains the download cards and displays the stale-data notice without overlap. Native actions integration and package tests pass. The PS5 package was cross-built in isolation. This release leaves the VideoOut renderer unchanged; the continuous-presentation experiment is saved separately in the ignored `build/video-cadence/` directory. The native app was closed, all staged/published file hashes were verified over FTP, the previous application tree and registered metadata were backed up, and 01.000.001 was activated without restarting the service. A new hardware log confirms 01.000.001 reached main, initialized VideoOut/controller and connected to API v1 while the extraction continued. Recovery from a naturally occurring timeout still needs observation on this build; simulated recovery is covered by the host worker tests.

### 01.000.002 pending torrent deletion feedback

The native UI displays the torrent name, elapsed time and an indeterminate activity
indicator during deletion. A lost response changes the screen to "Checking
deletion..." while the worker waits for a fresh catalog, without replaying the
delete request. Duplicate actions are blocked. After two minutes without a fresh
state, the UI reports an unconfirmed outcome instead of waiting indefinitely or
claiming success. Other connection failures retain the existing reconnect behavior.

Host worker regressions cover success, response loss, stale state, unavailable
Transmission, recovery, the bounded wait and exactly one deletion POST. Native
action integration and package tests pass. Actual renderer previews for
`BOTTY_PREVIEW_MODE=deleting` and `checking-deletion` were inspected. The title
was cross-built in isolation on the SSH host `test` with the pinned native runtime.
Version 01.000.002 was installed with the app closed: all twelve staged and live
files were read back and hash-verified, the previous complete title and three
registered metadata copies were backed up, and the service was not restarted.
The deletion screen still requires observation during a real PS5 deletion; host
simulations do not establish hardware acceptance.


### Release 1.1: native 01.000.006 and service 1.1.0

On 2026-10-02, the user confirmed that LAUNCH reached READY and Botty+ opened
normally with ShadowMountPlus 1.7beta3-botty.3. Its startup log reported four
ShellCore hook/bridge pages pinned; a later read confirmed the hooks and bridge
were still present. This addresses observed missing hook bytes without using the
legacy Kstuff toggle protocol, which is incompatible with bundled Lite v1.11.
Page reclamation is the leading explanation, not measured rTorrent causation.

The native connection parser now accepts the service's port 8088 as well as the
legacy 9091 URL. Native 01.000.006 was installed with the app closed, a complete
rollback tree and all thirteen files verified in raw SELF transfer mode. The user
confirmed that torrents and Library appear. A full probe-to-catalog host regression
covers the port 8088 response.

The user also confirmed Battlefield 6 artwork after replacing a near-uniform grey
Steam cache entry with its verified PlayStation cover. The updated artwork resolver
rejects near-uniform provider images and re-resolves cached placeholders. During
cache refresh, no extraction was active; only the idle Botty service was reloaded.
The rTorrent PID stayed unchanged and download progress increased.

These observations do not complete clean-install migration, long-duration load,
all native actions or additional firmware acceptance. Package-wide hardware
validation flags remain unchanged. The older mock-Transmission service integration
suite does not establish rTorrent backend coverage.

## Botty+ 1.2 — 2026-10-02

The user confirmed Library display and LEGO launch after compression, mounted
verification and deletion of the tracked uncompressed copy. See the detailed
[compression acceptance record](../game-compressor/VALIDATION.md). The final
release header/footer label update is cosmetic and was checked in the host renderer.

The service HTTP and search integration suites and native action client now use
a rTorrent SCGI fixture and pass. This supersedes the old mock-Transmission
coverage limitation above; it remains host validation rather than daemon/console
acceptance for every action.

## Botty+ 1.2.1 — 2026-10-02

The compressed-only Library menu contains one enabled **Delete game** action.
Compression, original deletion and restore actions are omitted when inapplicable.
The real native command client successfully queues deletion against the actual
service with no original, torrent daemon or archive. Authentication, explicit
confirmation, duplicate requests and interrupted deletion are covered.

Production compression-library tests also ran as an isolated PS5 payload:
activation, comparison, restore, original deletion, compressed-only deletion and
deletion with a retained original passed. They used tiny synthetic files and a
loopback ShadowMount mock, not a mounted real game. Source mismatch, busy mount
and invalid output guards were exercised. Host tests additionally exercise
symlink guards; SDK symlink creation did not create a usable test link on PS5.
The console test uses a fixed loopback port because the ephemeral-port lookup
failed in this SDK. Checked descriptor-relative deletion is shared with Botty's
existing PS5 game-folder deletion code, with post-deletion absence checks.

The user's only compressed LEGO copy was explicitly excluded from destructive
tests and retained in production. No claim of deleting a real compressed game
on this console is made. The menu screenshot is from the actual host renderer.


### 1.3 external storage — host validation, PS5 acceptance pending

Service 1.3.0 / native 01.003.000 add storage and process selection at game
addition, destination selection for extraction/publication/compression, and
transfers of torrent data, ready output, published folders and compressed images.
Host tests use synthetic archives and isolated temporary directories; mock
ShadowMount exercises registration handoff, not actual console mounting.

Storage tests cover persistent identity, disk removal/replacement, symlinks,
existing targets, space preflight, byte comparison, interrupted-copy preservation,
full-auto external publication, bidirectional transfers and compression output on
a disk different from its source. Native tests cover storage/mode navigation,
command encoding, and disconnection between choice and confirmation.
Screenshots `storage-choice-1.3.png` and `download-mode-1.3.png` render the actual
native UI with synthetic data.

The attached Mac Crucial X9 was inspected read-only: GUID partition map, exFAT,
1,000,187,363,328-byte partition, 1,000,143,323,136 available bytes at inspection.
No formatting or test writes were made to it. This does not establish PS5 USB
performance, hot-unplug behavior, executable permissions or title launch support.
Before deployment, preserve rollback packages, inspect active work and keep a
running service untouched. Console acceptance should use a small owned fixture
on the external disk, exercise the selected paths and verify Library launch.

Validation completed locally: native model/packaging tests, actual renderer
previews, native-to-service action/compression integration, service core/HTTP/
search/storage/compressor/Library tests, 105 portal Node tests and 8 release
Python tests. PS5 service, native title and library-1.3 worker cross-builds
succeeded. Applying all three worker patches to the pinned clean archive
reproduced the compiled worker sources exactly. No console was modified.

## Processing 01.003.001

Replaces Extracted with a combined Processing view. Host model tests cover task
parsing, active-first ordering, waiting phases without false ETA, read-only task
rows, and independent progress reads while the command worker operates.
Service fixture tests verify authenticated progress reads while a deletion holds
the catalog lock, terminal deletion counters, and source preservation. The native
renderer screenshot `docs/screenshots/processing-1.3.1.png` uses synthetic data.
This is host validation; PS5 runtime acceptance is pending.

## Processing stack correction (01.003.002)

The first 01.003.001 console launch failed with SIGSEGV in the Processing monitor.
The captured exception wrote the return address below RSP after the parser frame
was entered. The monitor reserved about 59 KiB and its parser another 20 KiB.
The monitor now owns its response and candidate snapshot on the heap and handles
allocation failure without dereferencing a null pointer.
A regression runs the monitor on a 64 KiB pthread stack: the previous source
segfaults and the corrected source passes. Model, package, preview and native
action integration tests pass. Console launch acceptance is recorded separately.
Private kernel/core artifacts and the original executable are retained in ignored
backups; no game files or torrent processes were changed for this diagnosis.

Console acceptance for 01.003.002: the user confirmed that Botty+ opens and
Processing works. ShadowMount recorded the corrected title process starting
without the previous immediate exit. All 13 installed native files and their
rollback copies were hash-verified in confirmed raw SELF mode. Service 1.3.1
and the active torrent were preserved without restarting either process.

## Library copy labels (1.3.2 / native 01.003.003)

Library covers show Compressed, Uncompressed or Both formats. Each copy has its
own storage label: PS5 SSD, named external SSD, or Offline. Verified compressed
copies and retained originals use their separate target/source storage IDs.
Restored originals retain a second compressed-copy label; pending
compression does not claim a completed compressed copy. Service remains 1.3.1.

Host model/parser regressions cover split storage, removed originals, pending
compression, restoration and missing disks. The actual renderer preview covers
six representative cards; native integration and package checks pass. On firmware
13.00 the user reported successful LAUNCH, and console reads confirmed native
01.003.003 with service 1.3.1 active. Visual acceptance of all copy/storage
combinations on console remains separate from the host previews.

The portal accepts the two historical completed manual-update journals and
bounds native file reads at 32 MiB, allowing the current executable above 16 MiB.
Installer tests enforce actual read limits and cover larger previous executables.

## Provider merge and tracker chooser (release 1.3.6)

Service 1.3.5 queries all enabled Prowlarr torrent indexers, combines Console
results and sorts the returned subset by seeders, grabs or date. Explore retains
source alternatives and the native 01.003.006 chooser displays tracker, release,
size, seeders, leechers, grabs and date before storage, mode and confirmation.
Host integration covers multiple providers, grouping, missing counts, source
selection, opaque IDs, HTTPS origin confinement, durable queueing and original
archive preservation. Native model, packaging, preview and action integration
checks pass. The screenshot in `docs/screenshots/explore-trackers-1.3.6.png`
uses synthetic data; it is not a console screenshot.

The initial native 01.003.005 build reported cached raster allocation failure
and a black screen on firmware 13.00 after allocating large source collections
on the runtime heap. The corrected 01.003.006 stores these bounded collections
in static Catalog storage; no source collection consumes the renderer's heap.
Workflow panel logging also handles all panel values without indexing beyond
its labels. The exact platform heap limit was not independently measured.

Console acceptance on 2026-10-03: the user confirmed that 01.003.006 opens and
the tracker chooser works. All 13 native files were read back and hash-verified
in confirmed raw SELF mode, with complete rollback files retained. A live
service search returned 97 games and 129 sources from IPTorrents configurations
and TorrentLeech; 28 games offered multiple sources, and grabs order was checked.
No real download was added as part of this live acceptance check; selected-source
download and extraction were validated with isolated homebrew fixtures.

## Background move/delete UX (release 1.4.0)

Move and Library-delete requests lead to Processing, with asynchronous acceptance
messages instead of claiming completion. The controller remains available after
acceptance; tasks show progress and retain failures. Confirmation text wraps to
two lines and no longer promises a full byte comparison or an unconditional
Botty restart. Task details do not label transfer/deletion bytes as extracted.

Host model, renderer preview and real-service action integration checks cover
these changes. The ShadowMount background capability must be deployed separately;
compression activation/restoration and new-title registration still have their
existing session requirements. No console acceptance is claimed here.
