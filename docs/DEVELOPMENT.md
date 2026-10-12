# Botty+ development

This repository contains the native PS5 app, Botty background manager,
rTorrent engine, compression worker and optional Prowlarr/artwork services.
Portal+, jailbreak payloads, DNS and console installers are maintained in
https://github.com/Portablelle/Portal-Plus. A Botty+ checkout builds and tests
without a Portal+ checkout.

## Host validation

Use Python 3.10+, Node.js 20+, make, a C++20 compiler, curl/OpenSSL development
libraries and Pillow for artwork. CI uses Clang, matching the PS5 toolchains. Generate isolated RAR fixtures before testing.

```sh
python3 homebrew/botty/tests/make_fixtures.py
make -C homebrew/botty NATIVE_CXX=clang++ TEST_JOBS=4 test   # TEST_JOBS defaults to 1 (sequential)
make -C homebrew/botty-native CXX=clang++ test integration
python3 deployment/botty-artwork/test_server.py
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 scripts/botty-packages.py --check
```

`bash scripts/botty-host-checks.sh` runs the full Linux suite used by CI and the
pre-push loop. It compiles through ccache whenever `ccache` is on `PATH`, so a
local cache makes repeated runs much faster:

```sh
sudo apt-get install ccache    # or: brew install ccache
bash scripts/botty-host-checks.sh
```

The script caps the cache at `CCACHE_MAXSIZE` (default 1G) and keeps the default
cache directory unless `CCACHE_DIR` is set. When the checks run in a throwaway
container, mount a persistent volume there, for example `docker run -v botty-ccache:/ccache -e CCACHE_DIR=/ccache ...`. The script
sets `CCACHE_BASEDIR` to the checkout, so entries are shared between worktrees.
`BOTTY_CHANGED_FILES` may name a file listing changed paths, one per line, to skip
the compiled suites those paths cannot affect, as PR checks do.
`BOTTY_HOST_CHECK_BUILD_ONLY=1` compiles the selected suites without running their
tests; default-branch checks use it for merges whose tree was already validated.

macOS renderer preview: `make -C homebrew/botty-native preview` (requires `sips`).
PS5 builds use the component Dockerfiles and pinned SDKs; host tests do not prove
hardware compatibility. See the component READMEs and native VALIDATION.md.

## Publish packages for Portal+

Build the service with `homebrew/botty/Dockerfile`, the compression worker by
following its [component build instructions](../homebrew/game-compressor/README.md#architecture-and-build),
the native title with `homebrew/botty-native/Dockerfile`, and rTorrent with its
own Dockerfile. Copy the worker binary to `homebrew/botty/build/game-compressor.elf`
before packaging the service. Keep service and native versions distinct. Then run:

```sh
python3 scripts/package-botty.py
python3 scripts/package-native.py
# After rebuilding the torrent engine:
python3 scripts/package-rtorrent.py
python3 scripts/botty-packages.py --check
```

Packaging writes `packages/botty`, `packages/botty-native` and `packages/rtorrent`,
including corresponding source and licenses. Each packaging command regenerates
`packages/botty-release.json`, the complete hashed delivery contract. Commit the
updated packages and contract together on `main`. Portal+'s VPS synchronizer
fetches this exact commit, checks the contract, updates its installer pins and
publishes an atomic portal release. It follows changes in either repository.
A source-only commit does not build new PS5 binaries automatically: package the
build before publishing a runtime change.

The [versioning policy](VERSIONING.md) defines the single public Botty+
version, generated PS5 identity, exact component inventory and GitHub tag
checks. Use `python3 scripts/package-release.py` for the complete
`botty-plus-<version>.tar.gz` release asset.

For documentation-only changes inside packaged components, use
`package-botty.py --source-only` and `package-native.py --source-only`; never use
source-only packaging to represent changed executables. Credentials, console
state and private logs stay outside Git and packages. Running console jobs are
preserved; a VPS publication does not restart console services.
