# Third-party inventory

The repository contains multiple components with their own license texts and
notices. This inventory points to the bundled records; it does not replace them
or assign a blanket license to all files.

| Component | Provenance / bundled notice |
| --- | --- |
| Relapse browser and kernel chain | Pinned `Relapse-Exploit` submodule, `vps-site/manifest.json` upstream revision, [portal license](vps-site/LICENSE), [credits](vps-site/README.md#upstream-credits) |
| Botty service | [GPL-3.0 license text](homebrew/botty/LICENSE), [dependency provenance](homebrew/botty/README.md#third-party-provenance) |
| Botty+ native title | [GPL-3.0 license text](homebrew/botty-native/LICENSE), [native dependencies](homebrew/botty-native/vendor/NOTICE.md) |
| Native runtime / boilerplate | Pinned source archive `homebrew/botty-native/vendor/boilerplate-dd44bbd.tar.gz`, including upstream notices |
| UnRAR | [UnRAR license](homebrew/botty/vendor/unrar/license.txt), including its additional restrictions |
| cpp-httplib | [MIT notice](homebrew/botty/vendor/HTTPLIB-LICENSE) |
| nlohmann/json | [MIT notice](homebrew/botty/vendor/JSON-LICENSE) |
| PS5 controller ABI | [SDL-derived header and zlib notice](homebrew/botty-native/vendor/NOTICE.md) |
| Manrope | [SIL Open Font License](homebrew/botty-native/assets/Manrope-OFL.txt) and vendored font source |
| Transmission / websrv helper | [Source, artifact hashes and licenses](vps-site/apps/transmission/NOTICE.txt), plus bundled GPL and web UI legal files |
| Modified ShadowMountPlus | [Pinned upstream source, recovery patch and build recipe](homebrew/shadowmountplus/README.md); GPL-3.0 license and full corresponding source ship beside the payload |
| Kstuff-lite, FTP, Prospero, etaHEN | Exact release URLs and SHA-256 values in [payload provenance](payloads/versions.json); these are upstream artifacts, not Botty-authored binaries |
| SDK distributions | Component Dockerfiles and [native build environment](homebrew/botty-native/BUILD-ENVIRONMENT.json) |
| UI artwork / PS5 mark | [Artwork provenance](homebrew/botty-native/artwork/README.md) |

The portal ships corresponding Botty service and native source archives beside
their application packages. Keep the notices, source archives and bundled license
files when redistributing. Upstream binaries and third-party marks retain their
own terms; review the relevant upstream records before publishing a new bundle.

Game cover images are fetched at runtime by the optional resolver and are not
bundled as a game catalog. The PS5 wordmark belongs to Sony Interactive
Entertainment; this project is not affiliated with or endorsed by Sony.
