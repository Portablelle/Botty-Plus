# Botty+ 1.2 compression validation — 2026-10-02

## Console acceptance

The user confirmed both that Botty+ shows LEGO in Library and that LEGO still
launches correctly after the production workflow deleted its tracked uncompressed
copy. The native identity is `PPSA99071`, version `01.002.000`; service `1.2.0`;
console firmware `13.00`; ShadowMountPlus `1.7beta3-botty.3` with pinned hooks.

| Measurement | Result |
| --- | --- |
| Game | LEGO Voyagers, PPSA23732 |
| Source files | 132 |
| Source bytes | 3,502,258,527 |
| Compressed image bytes | 2,526,740,480 |
| Potential reduction | 975,518,047 bytes (27.85%) |
| Console compression | Approximately 50 seconds |
| Independent beta PFSC validation | 54,571 blocks |
| Independent beta file comparison | 132 exact paths, lengths and SHA-256 |
| Production activation | Automatic ShadowMount selection and mounted byte comparison passed |
| Original restoration | Production API restored the original, then reactivated and verified the image |
| Original deletion | Second complete mounted comparison passed, tracked backup removed |
| Service restart | Ready/deleted state recovered; rTorrent remained available |
| Final user check | Botty+ displays LEGO; compressed game launches after original deletion |

The source includes an existing `ampr_emu.index`. The integration preserves it,
including its exact bytes, instead of generating an index or changing game files.
The previously corrected `Il2CppUserAssemblies.prx` filename case is preserved.

For this acceptance test only, an additional private hard-link tree was retained
before testing deletion. The accepted beta image is also retained separately.
Therefore the size difference above is potential saving, not claimed reclaimed
space on this development console. Normal Library compression does not create
that additional private test tree. Saves, original download archives, torrent
state and unrelated services were preserved.

## Defects found before release

The first production verification stopped the service at entry to the streaming
comparator. Its two 64 KiB buffers were moved from the limited thread stack to
heap storage. The corrected comparison passed on the PS5 and in a host regression
using a 64 KiB pthread stack. Restart correctly locked the interrupted transaction;
the production restore action recovered the retained original.

Worker shutdown now shuts down its listening socket before closing it. A console
probe confirmed the final worker stopped, then authenticated its identity after a
fresh start. Worker takeover/forced process replacement is unreachable. The native
header and footer were updated from the private beta labels to release 1.2.

## Host checks

- Service compressor unit tests: request authentication, title/source identity,
  symlink refusal, active-operation conflicts, cancellation identity, persistent
  uncertain dispatch, worker completion and restart state.
- Library transaction tests with isolated files and a mock ShadowMount: busy
  refusal, activation, exact file comparison, changed contents, symlink refusal,
  restoration, original deletion, repeated deletion refusal and a 64 KiB stack.
- Native model and packaging tests, real-service/native-client compression
  integration, plus renderer preview of the new Library actions.
- PFSC validator fixtures: compressed blocks, final partial-block hashes, bad
  hashes, mismatching file sets and corrupted image data.
- Botty package-contract regressions, including source packages and package
  hashes. Portal installer and public-export checks run in the separate Portal+
  repository before publication.

The service HTTP, search-to-Library and native action integration suites pass
against a local rTorrent JSON-RPC/SCGI fixture. They exercise the actual service
adapter, framing, peer statistics, completion, pause/resume/hash checking, metadata
submission, extraction, cancellation, deletion safeguards and recovery. Shared-file
deletion also rejects aliased paths resolving to the same directory. The dedicated
compression integration runs against the actual Botty service with an isolated
mock worker. Host tests are not substitutes for console acceptance.

## Rollback and scope

Stable native `01.000.006` and service `1.1.0` were retained and hash verified.
The private beta previously completed a real stable rollback/restore round trip.
The 1.2 game workflow additionally completed original restoration on-console.
A private release rollback script has verified local inputs; that full 1.2-to-1.1
app rollback has not been executed. Private records, helpers and manifests are
under ignored `backups/compressor-beta-20261002/`; none belong in public packages.

Runtime acceptance is limited to this game and firmware. Other titles, long-term
play, rest mode and other firmware combinations remain unvalidated. Existing images,
links/special entries and APR titles without an existing index are refused. Neither
successful file validation nor one successful game launch proves universal game
compatibility.

The user requested release publication and attested to the author's permission by
DM to redistribute modified compressor source and binaries. No private DM is
included; see `NOTICE.md` for the attribution and permission scope.

## 1.2.1 deletion follow-up

Compressed-game deletion no longer depends on retaining the original or torrent
archives. It waits for an unmount, checks the selected image and confined paths,
journals mutation and removes only the image, sidecar and retained original.
The service removes the Library job only after completion. Interrupted deletion
is locked for review. File removal uses the previously validated PS5 syscall
wrappers rather than relying on SDK filesystem removal; absence is checked.
See the native validation record for host and isolated PS5 test scope. LEGO's
production compressed image was preserved throughout these tests.
