# CheatRunner integration

Portal carries **0.17.2-botty.1**, based on upstream v0.17.2 revision
`59dcb9efaba71af00dc29d6cfe35da7d1ea43651`.
Copyright remains with CheatRunner's authors; GPL-3.0.

The source-search worker uses an explicit 2 MiB stack. On the tested PS5 13.00
Relapse setup, the default pthread stack is 64 KiB, while the compiled search
worker and nested source loader need about 238 KiB before network calls.
The original search stopped the entire process; increasing the worker stack
allowed the same search to finish while health and live status stayed responsive.
Attribute initialization/stack sizing failures return an error without starting
an undersized worker. No gameplay memory primitives or hotkeys are changed.
The filename parser also accepts an underscore suffix after the version, so
hashed MC4 filenames retain their version and exact matches sort first.

`patches/source-worker-stack.patch` is the complete patch against the pinned
upstream source. The distributed source archive includes this patch applied and
the original embedded home-screen package, copied byte-for-byte from the official
ELF (tile SHA-256 `22091bb243335bfca5d4e0e4fd1a6684138acca67bd778c1481bd7f23b3a58d2`).
Upstream does not publish the tile's build recipe; its binary is preserved.

Build the distributed source using `homebrew/botty/Dockerfile` (pinned SDK v0.43),
with `/usr/bin/cc` pointing to clang-18, then `make -j4`. To package the pinned ELF:

    python3 scripts/package-cheatrunner.py --elf /path/CheatRunner.elf --source /path/clean-upstream-checkout --upstream-elf /path/official-v0.17.2.elf
    python3 scripts/portal-manifest.py

The packager verifies both binaries, applies the local patch to a clean upstream
archive and includes the original tile. The installer verifies all package hashes.
Run host regressions with `python3 homebrew/cheatrunner/tests/run.py /path/patched-source`.

Portal stores verified ELF versions under /data/botty/cheatrunner/releases/.
CheatRunner owns /data/cheatrunner; cheats, patches and profiles are preserved.
Before a new start, Portal backs up any changed config and sets only http_port=9999,
tile_autoinstall_enabled=1 and hotkey_enabled=0. Existing running instances are
reused without restarts or config changes. Different versions require a future
console session; their running process is preserved. A process with no responding
HTTP service is never blindly reinjected.

The payload installs CHTR09999 into Media / Media Players. Portal polls
for its appmeta directory and reports registration separately from HTTP health.
Registration is not proof of successful launch. The tile opens the local dashboard;
the payload must first be started through LAUNCH after each console reboot.
No CheatRunner page is added to Botty+. No legacy Kstuff toggles or ShellUI hotkey
hooks are enabled by the integration.

Startup is deferred during Botty extraction, transfer or compression, or if its
work state cannot be established. CheatRunner failures are reported without
turning a successful Botty session into a restart instruction.

Host tests validate worker error paths, version parsing, the installer and hashes.
Console acceptance on 13.00 + Relapse confirms HTTP health, live state and remote
search completion. Applying a cheat to a running game remains a separate test. Do not send test
payloads during active compression. The upstream SDK memory primitives and the
embedded PKG have not been validated on this console by these host tests.
