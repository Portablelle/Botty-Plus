# Botty+ Library compression

Botty+ 1.2 integrates a pinned PS5-Game-Compressor worker with a separate,
journaled Library workflow. Upstream: gcoding97/PS5-Game-Compressor,
commit `7769e8526e286a354f25c77ca64ebb27981c0228`. See `NOTICE.md` for permission
and dependency notices, and `provenance.json` for the source archive digest.

## Use

Select a PS5 folder game in Library, open Options and select **Compress game**.
Botty creates exFAT-in-FFPFSC without deleting the original. Downloads remain
available; extraction and conflicting file operations wait for compression.
Space for both copies plus a 1 GiB reserve is required.

When prompted, close Botty+ and any game. The service continues running and asks
ShadowMount to select the compressed image. It reads every mounted file and
compares its exact name, length and bytes with the original. Library reports
ready only after that verification succeeds. Open Botty+ again to check progress.

Test the compressed game. **Delete uncompressed copy** is a separate confirmed
action: close Botty+ again, then Botty revalidates every mounted file before
removing only its tracked original backup. Compressed content, saves and download
archives remain intact. **Restore uncompressed game** selects a retained original
again and keeps the compressed image for inspection.

Only tracked PS5 folders are supported. Existing images, Botty's own title,
links/special files and APR games without an existing nonempty `ampr_emu.index` are refused. Existing indexes are preserved and verified with the game files. LEGO
Voyagers was used for console acceptance; this is not a compatibility guarantee
for every title. See `VALIDATION.md` for precise test results.

Interrupted activation or deletion stays locked with a recovery message. Do not
repeat a request whose outcome is unknown or manually delete its backup. A retained
original can be restored through Library. No automatic repair, forced unmount,
ShadowMount restart or Kstuff pointer manipulation is performed.

## Architecture and build

The worker binds only loopback port 5910 and authenticates with a random token.
Its API exposes copying, status, cancellation and idle shutdown. Source deletion
and source activation are owned by Botty, using ShadowMount's loopback API.
Per-game records and operation phases persist under `/data/botty/compressor`.
The native app can close while the service continues the operation.

Run `python3 prepare.py` in a clean component directory to verify and unpack the
pinned archive and apply both patches. Build `build/` with SDK v0.43:

```sh
docker run --rm -v "$PWD/build:/work" -w /work botty-ps5-build:0.43 \
  make -j2 BUILD_VERSION=botty-library-1.2
```

Host checks:

```sh
make -C ../botty native test-compressor test-compression-library
make -C ../botty-native test preview compression-integration
python3 -m unittest discover -s tests -v
```

The source-only validator independently decodes PFSC and nested exFAT and compares
SHA-256 against a separate source manifest. Its success does not establish game
runtime compatibility. Runtime Library verification reads through the actual PS5
kernel mount and compares all files byte for byte.

Private console evidence and rollback binaries are excluded from public source
packages. App/service rollback preserves installed games, torrent jobs and archives.
