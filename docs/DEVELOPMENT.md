# Development and release preparation

Run these commands from the repository root. PS5 cross-builds and hardware tests
are separate from host validation. No command below deploys to a console or VPS.

## Host prerequisites

Use Python 3.10+, Node.js 20+, make, a C++20 compiler, curl development
headers/libraries and OpenSSL. On macOS, install the Command Line Tools. The native
preview also requires macOS `sips`; Linux can run the other checks. Artwork tests
and asset generation need Pillow (recorded asset-generation version: 12.0.0).

For Debian/Ubuntu host tests:

```sh
sudo apt-get install build-essential libcurl4-openssl-dev python3 python3-pil openssl
```

Install a suitable Node.js version separately if the distribution's version is
older. PS5 builds use the component Dockerfiles with Linux amd64 toolchains. On
Apple Silicon, Docker must support that platform through emulation or a remote
amd64 builder. Do not install the PS5 SDK into the host to run the documented
container workflow.

## Tests

```sh
node --test tests/*.test.mjs
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 homebrew/botty/tests/make_fixtures.py
make -C homebrew/botty test
make -C homebrew/botty-native test integration
python3 deployment/botty-artwork/test_server.py
python3 scripts/portal-manifest.py --check
```

On macOS, additionally run:

```sh
make -C homebrew/botty-native preview
```

The fixture generator creates original synthetic RAR data in an ignored directory.
It does not require user downloads. Service integration uses isolated temporary
storage, mock Transmission, and local HTTPS fixtures. Native action integration
requires the host service binary built by the earlier service test command.

## Build PS5 packages

### Service

```sh
docker build --platform linux/amd64 -t botty-service-build homebrew/botty
docker run --rm --platform linux/amd64 \
  -v "$PWD/homebrew/botty:/work" botty-service-build
python3 scripts/package-botty.py
```

Output: `homebrew/botty/build/botty-manager.elf`. Packaging copies the six runtime
files, writes the service manifest, updates its hash pin in the portal and creates
a corresponding source archive. Service version comes from `/health` in
`src/server.cpp`; for a version bump, update its default UI path and the portal's
`VERSION` constant consistently as well.

### Native title

```sh
docker build --platform linux/amd64 -t botty-native-build homebrew/botty-native
docker run --rm --platform linux/amd64 \
  -v "$PWD/homebrew/botty-native:/work" botty-native-build
python3 homebrew/botty-native/tools/verify_package.py
python3 scripts/package-native.py
```

The builder uses Python 3.12+ inside the container. The native output includes a
complete title directory and ZIP, manifest, `SHA256SUMS`, corresponding source and
resolved build-tool versions. Packaging verifies the twelve-file title/ZIP before
copying runtime files and updating the portal's native manifest pin. Increment
`sce_sys/param.json`'s `contentVersion` for a native release and update build labels
as appropriate. Do not change the provisional title ID casually or reduce
`downloadDataSize` from 256.

The service and native SDK versions differ intentionally. Preserve hashes and
notices in each toolchain. The native base image is pinned; distribution apt
packages are not a guarantee of byte-for-byte reproducible container rebuilds.

### Documentation-only changes

When executable source and runtime assets have not changed, retain the verified
binaries and refresh their source/notices:

```sh
python3 scripts/package-botty.py --source-only
python3 scripts/package-native.py --source-only
```

Source archives omit local build caches, fixtures, credentials and private notes;
tar ownership and timestamps are normalized. Do not use this mode after changing
executable source: rebuild first so the source and binary correspond.

### ShadowMountPlus recovery build

The portal ships `1.7beta2-botty.1`, with guarded TitleDir hook recovery. Follow
[its build and test instructions](../homebrew/shadowmountplus/README.md), then run
`python3 scripts/package-shadowmount.py` and regenerate the portal manifest.
This packages the complete source recipe and GPL notices alongside the payload.
Keep upstream provenance distinct from the modified binary digest.

### Transmission and upstream payloads

Transmission is the pinned 4.0.6 distribution from websrv v0.33. Its notice file
records artifact URLs, hashes, source references and licenses. Given the original
upstream artifacts:

```sh
python3 scripts/package-transmission.py /path/to/Transmission.zip /path/to/websrv-ps5.elf
```

The script refuses different input hashes. Its output manifest hash must match
`MANIFEST_HASH` in `vps-site/src/transmission.js`; it does not automatically adopt
a newer upstream release. Keep all notices and web UI legal files alongside it.

`payloads/versions.json` records manual payload provenance. The portal carries its
own runtime copies plus the Relapse kernel stage and loader. Updating the
`Relapse-Exploit` submodule alone does not update the customized portal: reconcile
its browser code, offsets and payloads explicitly and rerun installer tests.

`serve-local.py` and `Start-Relapse.command` serve the upstream checkout, not the
Botty portal. `send-payload.py HOST payload.elf` and `send-prospero.py HOST` are
manual tools for a running ELF loader on 9021. They are not required by the
one-button workflow and a successful send does not prove payload startup.

## Prepare a public release

1. Complete the relevant host tests and package checks. Review
   [hardware acceptance](../homebrew/botty-native/VALIDATION.md) and report its
   remaining limits instead of changing validation flags based on host tests.
2. Refresh changed packages and corresponding source. Preserve upstream notices;
   see [third-party inventory](../THIRD_PARTY.md).
3. Generate a release label and verify the full package/hash chain:

   ```sh
   python3 scripts/portal-manifest.py --release public-preview-1
   python3 scripts/portal-manifest.py --check
   python3 scripts/portal-manifest.py --output dist/public-preview-1
   ```

4. Inspect `git diff --check`, `git diff --stat` and the files to be committed.
   The manifest covers only the public site: current packages, their source and
   notices, exact payloads, browser modules and offset files. It excludes old
   package copies, local previews, backups and development servers.
5. Publish the complete export with the [deployment procedure](../deployment/README.md).
   Include native ZIP/source/checksums and build metadata as separate release
   attachments if distributing native builds outside the portal.
6. Keep the previous portal and console packages. Confirm file read-back hashes,
   registration and health on the intended firmware before announcing acceptance.

Private keys, Prowlarr config, tracker cookies, Transmission credentials, logs,
console state and local backups must stay outside public commits and exports.
Public history excludes retired private operational notes and earlier source
archives that contained them. Commit authors use a public noreply address. Keep
any pre-cleanup Git bundles outside public refs and exports; old clones still
retain the original history. Review future commits before publication.
