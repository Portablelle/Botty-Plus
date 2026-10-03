# Botty+ native application — 01.002.001

Botty+ is a C++20 native PS5 title with a 1920×1080 software renderer, bundled
Manrope font and DualSense navigation. Its title ID is `PPSA99071`. It connects to
Botty API v1 on loopback port 8088; download and extraction work runs in separate
services. The native title does not register another web shortcut.

For hosting and first installation, use the [root README](../../README.md) and
[deployment guide](../../deployment/README.md). Select **LAUNCH** on the PS5 portal,
then open **Botty+** after discovery. The installer updates recognized older Botty versions with the app closed, after
verifying staging and retaining a private rollback copy. It refuses unrelated
titles and downgrades. A journal recovers interrupted swaps. See the deployment
guide for recovery and manual rollback.

## Screens and controls

L1/R1 switches **Explore → Search → Downloads → Processing → Library → Connections**.
D-pad/stick moves selection; Cross opens details and Circle goes back. Options
opens actions for the selected item. Each destructive action uses a separate
confirmation with Cancel selected initially.

- **Explore:** six-cover pages. Triangle cycles Most seeded / Most completed /
  Newest; Square refreshes; Cross selects **Download and prepare**. Recognized
  owned/downloaded titles are hidden. Matching is conservative and artwork may
  remain unavailable for some titles.
- **Search:** Square enters a query. Results show size, seeders and leechers.
  Prowlarr configuration is optional and provisioned privately on the console.
- **Downloads:** progress, ETA, speeds, peers and file details; pause, resume,
  verify, extract and confirmed removal of original download files. Square adds
  a magnet. Left/right changes filters.
- **Processing:** job status, errors, throughput and ETA; cancellation, cleanup
  and moving recognized content to the library. Moved jobs appear in Library.
- **Library:** three-column grid, left/right moves one card, up/down one row,
  Cross opens details. This screen does not automatically launch games.
- **Connections:** Transmission address, username and password for LAN access.

In details, up/down changes pages. The controller keyboard uses L1/R1 to switch
key sets, Square to erase and Triangle to reveal/hide an archive password.
Options accepts a hexadecimal Unicode code point. Characters outside the font
atlas are sent as UTF-8 but displayed as `?`. Magnets allow up to 16,384 bytes,
archive passwords up to 1,024. Empty archive passwords can be submitted with Done.

One action request can be in flight at a time. A lost response is reported as
uncertain and is not automatically repeated. Network work is bounded and runs
outside the render loop. Input-release guards prevent one button press from both
opening and confirming a dialog. Missing service capabilities disable related
actions instead of pretending they succeeded.

## Build and validation

From this directory:

```sh
make test
make preview       # macOS only: uses sips to produce build/preview.png
make integration   # first build the service in ../botty
```

Preview uses the actual renderer with simulated network/controller APIs. It is
not a console screenshot. To select another state:

```sh
BOTTY_PREVIEW_MODE=connections ./build/preview
```

Other scenarios include `explore`, `extracted`, `library`, `details`, `offline`,
`quit` and `exit`. `BOTTY_PREVIEW_STATE_FILE` can provide synthetic state for
long-name and full-list layout checks. Font and illustration tools use Pillow;
the recorded asset-generation version is 12.0.0. Assets are already bundled.

Cross-build the native title with its own pinned runtime:

```sh
docker build --platform linux/amd64 -t botty-native-build .
docker run --rm --platform linux/amd64 -v "$PWD:/work" botty-native-build
python3 tools/verify_package.py
```

The output includes `dist/PPSA99071/`, `PPSA99071.zip`, `manifest.json`,
`SHA256SUMS`, source archive and resolved tool versions. Python 3.12+ is used by
the PS5 builder's safe tar extraction; the Docker image supplies it.
`BUILD-ENVIRONMENT.json` and `vendor/NOTICE.md` record pinned dependencies.
The native runtime uses SDK v0.42 and is independent of the service's v0.43
runtime. Resolved apt package versions can change between image builds.

From the repository root, publish that verified build into the local portal:

```sh
python3 scripts/package-native.py
python3 scripts/portal-manifest.py --release YOUR_RELEASE_LABEL
python3 scripts/portal-manifest.py --check
```

For documentation-only changes, `package-native.py --source-only` updates the
source archive and license notices without changing the native executable.
The title manifest deliberately retains `hardwareValidated: false` and
`registrationVerified: false`; see [the validation record](VALIDATION.md).

## Runtime and installation constraints

The title is installed as a complete directory at `/data/homebrew/PPSA99071`, not
as a ZIP or ELF payload. Keep `downloadDataSize` at **256**: firmware 13.00 rejected
the earlier value 16 before entering `main`. Inventory the target title ID before
publication; this is a provisional homebrew identity, not a globally reserved ID.

The thirteen runtime files, native FSELF signatures, metadata, hashes and matching
ZIP are validated by `tools/verify_package.py`. Transfer through a staging path
outside `/data/homebrew`, read back and hash every file, then publish atomically
with the app closed. Preserve the full previous title for rollback.

Diagnostics begin at `main` in `/download0/botty-native-network.log`, with at most
256 fixed messages per launch. They exclude tokens, passwords and raw responses.
Loader failures before `main` require system logs. Quit frees rendering resources,
joins the network thread and requests system exit; suspension, quitting and
service survival still require the remaining hardware acceptance checks.

Closing the UI is intended to leave Transmission and extraction running. Do not
remove `/data/botty` to uninstall the native title. Keep the console awake during
work; rest-mode support is not established.

During a slow local refresh, the app keeps its last catalog with a Reconnecting indicator and disables mutations until a fresh response arrives. Transmission RPC failures preserve the previous downloads while extraction jobs continue updating. Transient failures retry automatically after one second; the receive timeout allows five seconds under disk load.

### Library deletion

For a moved game folder, Library → Options offers **Delete game**, with Cancel selected by default. This deletes the installed game files while keeping the torrent and original archives. Close the game and remove its home-screen entry first; mounted games are refused. The action requires a service advertising `libraryDeletionSupported`. Image-based games require manual unmounting/removal. **Remove from Processing** only hides an extraction row and is no longer offered in Library.

## Home-screen music

`sce_sys/snd0.at9` contains the user-provided music excerpt from 01:19 to
02:19, normalized toward -28 LUFS with short fades and an embedded whole-file
loop. It is stereo ATRAC9 at 48 kHz / 192 kbit/s. The installer preserves its
metadata backup indices and appends the sound to registered metadata updates.
This asset-only update keeps the native executable version unchanged. The
console Home Screen Music option must be enabled for playback.

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


## Storage selection (1.3)

Native 01.003.002 reads the service's storage catalog and free capacity. Adding
a game opens storage selection, Full auto / Download only, then confirmation.
Extraction, publication and compression default to the current disk and let the
user choose another one. Options exposes Transfer to another disk for torrent
data, ready extractions and Library content. Transfer progress and recovery
errors remain visible; closing the app leaves the background operation running.
These features require service 1.3.0 and its library-1.3 compression worker for
external compression. Host previews are not PS5 acceptance.

## Processing (01.003.002 / service 1.3.1)

Processing replaces Extracted and combines active extractions, compression and
deletion with extraction results. Active tasks come first. A separate authenticated
`/api/processing` connection polls every second while long deletion requests run.
The current phase, counters, rate, elapsed time and phase ETA are shown. Deletion
counts checked file/directory removals; compression and verification count bytes.
ETA resets between phases and is unavailable while waiting, reconnecting or before
a useful rate sample. Original archives and saves keep their existing protections.
The panel retains the latest deletion/compression result in this service session;
it is not a durable history of every past operation.

Validated with host tests and the native renderer preview. After the 01.003.001
stack overflow was corrected in 01.003.002, the user confirmed that Botty+ opens
and Processing works on PS5.

When external storage is connected, its name and free space appear below the
PS5 free-space card in Downloads and Processing. Disconnected disks are hidden.

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
