# Botty+ architecture and maintenance constraints

This document replaces the original implementation proposal with the current
architecture. Read it before changing service boundaries or native lifecycle.
User-facing text is English.

```text
HTTPS portal → Relapse → native package / Kstuff / ShadowMountPlus / FTP
                      → Transmission + Botty service

PS5 home screen → Botty+ native title → loopback Botty API :8088
                                      ├─ Transmission RPC :9091
                                      ├─ RAR extraction and persistent jobs
                                      ├─ automatic preparation queue
                                      └─ optional HTTPS search/artwork proxy

LAN computer or phone → authenticated Transmission web UI :9091
```

## Responsibilities

- The native application is a real title (`PPSA99071`) with FSELF executable,
  metadata, runtime, controller input and rendering. It is not an ELF launcher or
  a browser shortcut. Its initial screen is Explore.
- Botty service (`BTTY00001` API identity) owns torrent operations, extraction,
  filesystem access, permissions, publication and optional search/artwork.
- Transmission owns torrent transfers and resume state. The native title never
  reads its credential file directly; it requests verified connection information
  through the authenticated local API.
- The portal verifies and installs packages and starts services after jailbreak.
  It must preserve matching files, persistent state and already-running services.
  Native upgrades must recognize Botty identity, refuse downgrades/foreign titles,
  verify a separate staged tree, check the app is closed and retain a recoverable
  previous version. Never replace the only copy of an installation in place.

## Contracts to preserve

- Keep service and native title versions independent. API version is 1; new
  capabilities must be advertised and feature-gated for older running services.
- Bootstrap returns a per-process token. Subsequent API requests use that token;
  credentials and raw responses must never enter diagnostics or release assets.
- Network I/O runs outside rendering, with bounded buffers/deadlines. An action
  is successful only after a successful service response. Uncertain outcomes are
  reported without automatically retrying mutations.
- Controller input has edge/release guards and bounded directional repeat.
  Confirmation is a separate interaction, initially selecting Cancel.
- Closing the UI releases renderer/controller resources and joins workers before
  system exit. Downloads and extraction belong to independent processes.
- Source archives/torrents survive extraction, cancellation and library moves.
  Deleting original download data requires the separate explicit removal action.
- Validate paths, links, member metadata, CRC, space and destination collisions.
  Publish only complete recognized outputs, on the same filesystem, with correct
  sandbox-readable permissions. Do not execute downloaded scripts.
- Persist incomplete jobs as interrupted on recovery. Do not automatically replay
  failed/interrupted extractions or enroll unrelated torrents in the automatic queue.
- Never restart an active extraction for deployment. Stage versions separately,
  verify console read-back hashes and retain rollback copies.

## Build and acceptance

The native runtime and service payload SDKs are distinct. Follow each component's
Dockerfile; do not replace one runtime with the other. Native `downloadDataSize`
remains 256 because firmware 13.00 rejected 16 before application startup.

Host checks establish contracts and fixture behavior. Native FSELF verification
establishes package structure, not firmware support. Renderer previews are simulated.
See [development](docs/DEVELOPMENT.md), [deployment](deployment/README.md) and
[hardware acceptance](homebrew/botty-native/VALIDATION.md) before a release.
