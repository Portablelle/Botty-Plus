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
- Portal launch order, installer hashes, credential migration, state preservation,
  service coexistence and temporary launcher cleanup.
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

Host regressions cover verified staging, identity/downgrade checks, closed-app
guards, full-directory backup, metadata refresh, interrupted-swap recovery and
service staging without stopping active work. These update paths have not yet
been exercised on PS5 hardware. Existing hardware observations above do not
validate the new updater.

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
