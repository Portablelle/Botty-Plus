# Botty+ versioning

Botty+ has one public `major.minor.patch` version. The app header, footer,
GitHub release title and tag use it. The first unified release is **1.6.0**.
Historical releases keep their original names and contents.

The single public-version source is
`homebrew/botty-native/release.json`. It lives with the native sources so
the corresponding-source archive remains independently buildable.
`tools/release_version.py` generates the C++ identities, PS5
`sce_sys/param.json` contentVersion and `assets/build.txt`.
For example, `1.6.0` becomes PS5 `01.006.000`. Major is limited to 99,
minor and patch to 999; prerelease suffixes and leading zeroes are rejected.
The native builder regenerates these files before compiling. CI rejects
stale checked-in identities or a package built for another public version.

## Component identities

The public version identifies a complete delivery bundle. Components retain
independent technical versions; a new public release does not rename an
unchanged manager, worker or upstream dependency. System shows the
compiled PS5 native version and the manager's reported installed service
versions when current state is available. Unavailable evidence is labelled
as unavailable rather than replaced with bundled versions.

`packages/botty-release.json` contains the public `version`, exact
`components` (native, manager, worker and rTorrent), `apis`, and the existing
SHA-256 delivery inventory. Native `requires` still describes minimum
compatible versions, not the exact bundle. The index retains schema 1 and
its original `sha256` structure so existing Portal+ and manager readers
continue to consume it.

Portal+ is a separate product/repository with its own release identity.
Jailbreak payloads, ShadowMount, SDKs, Prowlarr and optional companion apps
keep their upstream or integration versions; they do not adopt Botty+'s
public version. Publishing or changing Portal+ is outside a Botty+ release.

## Prepare and publish a release

1. Set the public version in `homebrew/botty-native/release.json`.
2. Run `python3 homebrew/botty-native/tools/release_version.py` and rebuild
   the native title. Every public version changes native metadata and
   compiled identity, including releases whose functional change is only
   in a service. Rebuild changed services with their own technical versions.
3. Package the corresponding sources, binaries, notices and manifests
   together with the component packaging scripts.
4. Run `python3 scripts/botty-packages.py --check --tag v1.6.0`
   (substitute the intended version), then run
   `python3 scripts/package-release.py` to create the complete bundle and
   its checksum in `dist/`.
5. Review and merge the PR. Publish **Botty+ 1.6.0** with tag **v1.6.0**
   and asset **botty-plus-1.6.0.tar.gz**. CI also checks pushed `v*` tags
   against the public version. Follow the same convention for later releases.

Preparing a PR does not create a GitHub release, publish Portal+ or update
a running PS5. Package validation and host tests are separate from console
acceptance.
