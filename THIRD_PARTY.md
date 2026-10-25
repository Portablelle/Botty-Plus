# Third-party inventory

The repository contains multiple components with their own license texts and
notices. This inventory points to the bundled records; it does not replace them
or assign a blanket license to all files.

| Component | Provenance / bundled notice |
| --- | --- |
| Botty service | [GPL-3.0 license text](homebrew/botty/LICENSE), [dependency provenance](homebrew/botty/README.md#third-party-provenance) |
| Botty+ native title | [GPL-3.0 license text](homebrew/botty-native/LICENSE), [native dependencies](homebrew/botty-native/vendor/NOTICE.md) |
| rTorrent / Rakshasa libtorrent | [GPL-2.0-or-later license text](packages/rtorrent/LICENSE), [pinned upstream provenance](homebrew/rtorrent/README.md#source-and-licenses) |
| Native runtime / boilerplate | Pinned source archive `homebrew/botty-native/vendor/boilerplate-dd44bbd.tar.gz`, including upstream notices |
| UnRAR | [UnRAR license](homebrew/botty/vendor/unrar/license.txt), including its additional restrictions |
| Experimental rest-mode companion | [PS5Tailscale and pinned ps5debug-NG provenance](homebrew/rest-mode-companion/README.md#provenance) |
| cpp-httplib | [MIT notice](homebrew/botty/vendor/HTTPLIB-LICENSE) |
| nlohmann/json | [MIT notice](homebrew/botty/vendor/JSON-LICENSE) |
| stb_image | [MIT / public-domain notice](homebrew/botty/vendor/stb_image.h), pinned to nothings/stb `f0569113c93ad095470c54bf34a17b36646bbbb5`; used to decode local Switch icons |
| PS5 controller ABI | [SDL-derived header and zlib notice](homebrew/botty-native/vendor/NOTICE.md) |
| Manrope | [SIL Open Font License](homebrew/botty-native/assets/Manrope-OFL.txt) and vendored font source |
| SDK distributions | Component Dockerfiles and [native build environment](homebrew/botty-native/BUILD-ENVIRONMENT.json) |
| UI artwork / PS5 mark | [Artwork provenance](homebrew/botty-native/artwork/README.md) |

The packages contain corresponding Botty service and native source archives beside
their application packages. Keep the notices, source archives and bundled license
files when redistributing. Upstream binaries and third-party marks retain their
own terms; review the relevant upstream records before publishing a new bundle.

Game cover images are fetched at runtime by the optional resolver and are not
bundled as a game catalog. The PS5 wordmark belongs to Sony Interactive
Entertainment; this project is not affiliated with or endorsed by Sony.

## Library compressor

Botty+ 1.2 includes PS5-Game-Compressor by gcoding97 and upstream contributors.
See [permission and dependency notices](homebrew/game-compressor/NOTICE.md) and
the supplied corresponding-source archive.

## PS5 Forwarder Format

Home-screen shortcuts use the unchanged template from
[heydemoura/ps5-forwarder-format](https://github.com/heydemoura/ps5-forwarder-format),
revision `22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4`, GPL-3.0-or-later. The complete
pinned repository, license, launcher, forwarder sources and build instructions are
in `homebrew/botty/vendor/ps5-forwarder-format/source.tar.gz` and are included in
Botty's corresponding-source archives. See its adjacent NOTICE.md.
