# Game Compressor integration notices

Botty+ uses PS5-Game-Compressor by gcoding97 and upstream contributors,
https://github.com/gcoding97/PS5-Game-Compressor, pinned at
7769e8526e286a354f25c77ca64ebb27981c0228. The Botty integration confines the worker
to non-destructive Library copies and uses Botty's journaled activation workflow.

The Botty maintainer confirmed on 2026-10-02 that the author granted permission by
direct message to redistribute the modified source and binary with Botty+.
No general open-source license grant is asserted for the upstream project.
Contact the respective authors before further redistribution under other terms.

The supplied corresponding source includes the exact upstream archive, digest,
Botty patches and preparation tool. Upstream embeds zlib 1.3.1; its license and
copyright notices are preserved in third_party/zlib in that archive. Preserve all
upstream file-level notices and credits. The PS5 payload SDK and its runtime have
separate notices at https://github.com/ps5-payload-dev/sdk.

No game content, console credentials, diagnostics or private authorization messages
are included in this package.
