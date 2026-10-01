# Repository Guidelines

## Project Structure & Module Organization

- `homebrew/botty/`: C++17 download/extraction service. Implementation lives in `src/`, browser assets in `ui/`, and UnRAR dependencies in `vendor/`.
- `homebrew/botty-native/`: C++20 controller-driven PS5 application. Use `src/`, `assets/`, `sce_sys/`, `tests/`, and packaging utilities in `tools/`.
- `vps-site/`: browser portal, JavaScript ES modules in `src/`, and deployable packages in `apps/`.
- `tests/`: portal and installer tests. Homebrew components have separate tests.
- `scripts/` contains release packaging; `deployment/` contains server configuration. `Relapse-Exploit/` and `payloads/` hold exploit/payload components.

## Build, Test, and Development Commands

From the workspace root:

- `node --test tests/*.test.mjs`: portal, installer, and Transmission integration contracts.
- `python3 -m unittest discover -s tests -p 'test_*.py' -v`: public export and source-package regressions.
- `python3 scripts/portal-manifest.py --check`: verify public files, package hashes and installer pins.
- `python3 scripts/portal-manifest.py --output dist/portal`: export a verified site to a new directory.
- `make -C homebrew/botty native`: build the host service.
- `python3 homebrew/botty/tests/make_fixtures.py`: generate original RAR test fixtures.
- `make -C homebrew/botty test`: C++ core tests and Python HTTP integration tests.
- `make -C homebrew/botty-native test preview integration`: native model tests, Python packaging tests, macOS renderer preview, and native-client integration tests. Preview requires `sips`.
- `make -C homebrew/botty ps5`: cross-build with `PS5_PAYLOAD_SDK`. Build the native title with its separate Dockerfile and pinned runtime.
- `python3 scripts/package-botty.py`: package the compiled service and update the installer’s manifest hash.

## Coding Style & Naming Conventions

Match surrounding formatting. Use spaces in source files and tabs for Make recipes. Follow existing camelCase C++/JavaScript names, Python snake_case names, and kebab-case script filenames. No shared formatter is configured; native host builds enforce `-Wall -Wextra -Werror`.

Keep UI text in English unless explicitly requested otherwise or the existing application uses another language.

## Testing Guidelines

Use Node’s built-in test runner (`*.test.mjs`), Python `unittest` (`test_*.py`), and C++ assertion tests. No coverage threshold is defined. Add focused regressions for changed behavior, covering CRC failures, cancellation, multivolume handling, path confinement, and source preservation. Use isolated fixtures, not user downloads. Distinguish host validation from actual PS5 testing.

## Commit & Pull Request Guidelines

Use concise imperative subjects. PRs should describe the problem, resulting behavior, tests, and deployment implications. Include screenshots for UI changes and link relevant issues.

## Configuration & Deployment

Keep credentials and archive passwords out of logs. Preserve original torrents and archives. Keep service and native-title versions distinct; regenerate package hashes after binary changes. Verify console transfers and retain rollback copies. Check extraction state before restarting the service; never interrupt active work merely to deploy an update.

## Console crash diagnostics (FTP and kernel trace)

- Discover the current console address; do not assume a previous DHCP address still applies. FTP uses port 2121 with anonymous login in the tested setup. Limit discovery to known LAN devices. Keep console addresses, logs, executables and memory dumps in ignored `backups/` directories, not public packages.
- Read `/data/botty/jobs/*.json` for extraction bytes/total, error, status and destination. Compare the affected title with working titles. `moved` / `Ready in Library` indicates pipeline completion, not runtime compatibility or an independent full-file integrity check.
- Retrieve `/data/shadowmount/debug.log` and, if needed, `debug.log.1`. Correlate title ID, PID, launch, mounted libraries, crash and teardown. Inspect FTP `LIST` permissions on the executable and modules, including nested `Media/Modules`, `sce_module` and `fakelib` paths. Do not infer that a mounted library caused the crash merely because its message preceded it.
- `/system_data/priv/error/history/*.json` contains the user-facing error code, title ID and firmware. `CE-108255-1` alone does not identify the underlying exception. Avoid printing unrelated account/system information from logs.
- The binary `/user/devlog/system/sce_logs/log.bin` did not yield useful text with a printable-strings extraction in the LEGO investigation. Prefer a kernel text trace. Where the installed ShadowMount supports it and its listener is reachable, POST `{"max_bytes":131072}` to `/api/v1/kernel-log` on port 10101. The default listener may be console-local; connection refusal does not mean the log is empty. Do not expose the API to the LAN merely to read a log.
- A tested fallback is a short-lived payload opening `/dev/klog` with `O_RDONLY | O_NONBLOCK`, reading bounded chunks (at most 8 MiB), and writing them to a private diagnostic file. The previously inspected helper source was `/tmp/botty-launch-log.c`, with binary `/tmp/botty-launch-log.elf`; it writes `/data/botty/manager/game-launch-kernel.log`. These temporary files are not guaranteed to persist. Inspect/rebuild the helper before reuse; preserve any existing output first. Send a verified helper through the existing ELF loader on TCP 9021, then retrieve its output over FTP. This helper takes one snapshot and exits; it is NOT an attached debugger or continuous memory capture. Kernel reads may consume buffered messages, so save every capture locally.
- In the kernel trace, extract the fatal-signal block: signal, thread/PID, reason, fault address, registers (especially RIP/RSP/RBP), backtrace, and dynamic-library address ranges/fingerprints. Match it to the target title's launch/PID; unrelated system crashes and network failures can coexist in the same log.
- For executable analysis, retrieve the exact installed binary and record its hash and FTP transfer mode. ftpsrv can convert SELF to ELF on retrieval. Check the received magic; do not compare a converted ELF hash with a raw SELF hash. For raw verification, issue `SELF` and confirm `SELF transfer mode disabled` in its response rather than assuming its current toggle state.
- A retrieved ELF can have a stale section-header table beyond EOF; this was observed for LEGO and is not by itself evidence of corruption. Parse ELF64 program headers (`PT_LOAD`) to map runtime addresses to file offsets: subtract the observed load base, find the matching segment, then use `p_offset + relative_address - p_vaddr`, within `p_filesz`. Disassemble bounded ranges with Capstone if objdump rejects missing section headers. Respect instruction boundaries; distinguish executable strings from messages actually emitted at runtime. Never patch the installed executable merely to inspect it.
- Observed LEGO case: title `PPSA23732`, version `01.001.000`, firmware `13.00`; fatal SIGSEGV in `eboot.bin`, instruction-fetch page fault with RIP and fault address both zero. The captured stack included `0xc37b95`, `0x115fa2c`, `0x116295c`, `0x11631d7`, `0x1162f51`, `0x18ba523`, `0x18c7a64`, `0x4000af`; executable text started at `0x400000`. Disassembly showed an indirect call at `0xc37b93` and a startup path referencing `globalgamemanagers`. This establishes the immediate execution fault and startup context, NOT which library or data defect originally produced it. These addresses apply only to that captured binary. A crash before Kstuff's auto-pause excludes that pause as its trigger, not every possible Kstuff interaction.

### Capturing crash memory on a new launch

- Before asking the user to relaunch, start and verify the capture mechanism, record its lifetime/output directory, and distinguish kernel-log capture from actual memory capture. Do not claim a debugger is attached unless attachment has succeeded.
- Inspect `/user/devlog/system/sce_coredumps.0/` for directories matching the target title. The kernel log identifies the requested `.prosperodmp` path via `sceApplicationKickCoredump3()`. The crash-report service can quickly delete the directory (`CleanUpCore:DELETE`); an empty directory after the fact does not prove a dump was never attempted.
- A narrowly scoped FTP watcher can attempt to copy target-title dump files immediately while they exist. Preserve partial transfers, track remote sizes, and retry growing files. This is best-effort collection of system-generated dumps, not a guaranteed RAM snapshot: the system may produce no usable dump or delete it before transfer completes. Keep screenshots and unrelated titles out of collection unless necessary.
- If no usable core is captured, the next step is a supported debugger that stops the target process on the exception, then reads registers, the stack around RSP/RBP, module mappings and relevant indirect-call targets before exit. This live attachment/memory workflow was NOT validated in the initial LEGO investigation. Verify available tools and firmware support before promising it; do not invent memory contents or symbols from a text backtrace.
- Preserve game files, saves, original archives and existing services. Do not stop extraction, restart the console, change global crash settings, or disable cleanup merely to obtain a trace without establishing the need and scope. Document what was actually captured and any remaining uncertainty.
- Follow-up LEGO capture succeeded: a 200 ms FTP directory watcher recovered the final `.prosperodmp` (332073 bytes, matching its manifest) before deletion. The file is an LZ4 frame; `lz4.frame.decompress` produced a 3997960-byte ELF64 `ET_CORE` with `PT_NOTE` and `PT_LOAD` segments. This is a complete transferred crash artifact, not all process RAM. Parse program headers to read captured stack addresses, and search the embedded diagnostic records as well as the standalone kernel log.
- The captured stack at RSP `0x7eeff7588` exposed return address `0x10f7441`, omitted by the text backtrace. The preceding call at `0x10f743b` uses pointer slot `0x246a9e0`; its initializer resolves `il2cpp_set_find_plugin_callback`. The core's internal log reports `unable to load /app0/Media/Modules/Il2CppUserAssemblies.prx, error:0x80020002`. FTP lists only `Il2cppUserAssemblies.prx` (lowercase `c`), with executable permissions and size 167960676. Check exact filename case when a module exists but loading reports it missing. The expected spelling returned an invalid UINT64_MAX size from ftpsrv, so do not interpret every numeric SIZE response as a real file size. The user subsequently authorized renaming it to `Il2CppUserAssemblies.prx`; FTP confirmed the exact new name, absence of the old name, unchanged size (167960676 bytes) and mode 0755. The user subsequently confirmed that the game launches successfully after this rename.
