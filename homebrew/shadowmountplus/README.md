# ShadowMountPlus 1.7beta3-botty.3

Botty's narrow patch to ShadowMountPlus fixes a TitleDir bridge that remained
unusable for the entire session after one failed hook check. This is a modified
upstream payload, distinct from the Botty service and native title versions.

On firmware 12.00+, registration now retries a failed hook read once and retains
its state on read errors. When AppInstallAll has returned to its exact saved
original bytes, registration suspends ShellCore, revalidates its image, targets,
and the complete installed bridge (excluding inactive request data), then
restores only the install jump and verifies it before dispatching the request.
Unknown patches, changed bridge code, an armed request, and failed validation
are refused. Partial writes roll back; unresolved rollback/detach failures block
further dispatch. The daemon, downloads and extracted files are not restarted.

This recovers the recognized missing-hook condition. The original console log
combined unreadable memory and mismatching bytes into the same error, so it does
not establish which component caused the initial failure. Foreign modifications
and persistent I/O failures still require diagnosis; this is not a guarantee
against every installation error. Existing game retry limits remain intact.

## Build and test

The checked-in source archive and SDK import stub are hash-pinned in
`provenance.json`. The upstream modifications are the reviewable patches
in `patches/title-dir-recovery.patch` and `patches/kstuff-lite-no-legacy-control.patch`. The upstream GPL-3.0 license and SDK stub
license are retained.

```sh
cd homebrew/shadowmountplus
# Start with no build/ directory; preserve an existing build before preparing.
python3 prepare.py
docker build --platform linux/amd64 -t botty-shadowmount-build:0.43 .
docker run --rm --platform linux/amd64 -v "$PWD:/work" botty-shadowmount-build:0.43
```

`tests/run.sh` runs on an x86-64 Linux host/compiler inside the build image. It
compiles the production hook implementation and upstream bridge assembly against
isolated memory doubles. Cases cover healthy and lost hooks, transient reads,
foreign patches, cave corruption, armed requests, changed processes/images,
attach/detach failures, partial writes, verification failures and rollback.
Upstream trampoline offsets and assembly are unchanged. These host regressions
are separate from console acceptance.

From the repository root, `python3 scripts/package-shadowmount.py` packages the
built ELF, full corresponding source recipe and notices; then regenerate and
verify the portal manifest. Preserve the previous deployed payload for rollback.
Do not restart ShadowMount while an application or extraction is active.

## Validation recorded on 2026-10-01

- Production-code host regressions passed: 24 serial registration calls plus
  eight concurrent calls, with one repair and eight serialized dispatches.
- Cross-build with the pinned SDK succeeded; prepared and remote-built hook
  sources have matching SHA-256 hashes.
- PS5 firmware 13.00: transferred payload verified byte-for-byte over FTP;
  `1.7beta2-botty.1` started, all three hooks initialized (`install=1`), the
  library synchronized, and the HTTP API started. Original payload retained.
- Recovery from injected hook loss was tested with isolated host memory doubles;
  deliberate ShellCore corruption was not performed on the user's console.
  Long-running hardware recurrence remains unverified.

## 1.7beta3 integration

The upstream 1.7beta3 release is pinned at `f0d15ffc46e9237d41cc3555b1cf11362d9a32e0`.
It corrects custom-path backport discovery, restores the 1.6 ffpfsc mount
parameters, and skips icon creation when the API is disabled. Upstream did not
change the ShellCore hook implementation; the existing Botty recovery patch
applies without changes. Previous hardware results above refer to beta2.

Validation on 2026-10-02: TitleDir production-code regressions and the PS5
cross-build passed, as did 107 portal Node tests and eight Python tests. FTP
read-back matched the built payload. On firmware 13.00, a controlled restart
logged `1.7beta3-botty.1`, all three installed hooks, completed library sync and
API readiness. The console configuration was retained and no active extraction
was interrupted. Game launch after this upgrade remains to be checked.

## Kstuff Lite compatibility guard (botty.2)

The bundled Kstuff Lite v1.11 uses copied syscall tables with canonical pointers.
The legacy ShadowMount pointer-poisoning pause/resume protocol is incompatible:
`0xffff` does not establish that Lite is disabled. This build disables that
legacy runtime control, including game/focus/sleep automatic toggles, and compiles
out its pointer writes. It leaves Kstuff Lite's own installed hooks intact.
This guard does not fix or diagnose the separate Botty launch error 0x80940033.
A manual legacy-control helper caused a console freeze on 2026-10-02 and must
not be reused. The helper was never included in the portal.

## Resident ShellCore hooks (botty.3)

Firmware 13.00 diagnostics found the launch and install entry points back at
their original bytes, and the bridge cave zeroed, despite a successful installation
log. The sandbox call still targeted that cave. The physical-write path did not pin these clean executable
pages. The new build uses the existing Kstuff remote-syscall ABI to mlock the
16 KiB pages covering the hooks and bridge before changing them, as pinned
Kstuff Lite v1.11 does for its own ShellCore patches. A failed lock aborts hook
installation. Duplicate pages are locked once per installation; locks remain
for ShellCore's lifetime to protect any outstanding bridge return addresses.
No legacy Kstuff enable/disable operation is used.

`patches/pin-shellcore-hooks.patch` contains this change. Host tests cover page
boundaries, shared pages, missing Kstuff and failures at every lock step.
Console acceptance must verify the bytes remain installed and Botty+ starts.
