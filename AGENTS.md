# Repository Guidelines

## Project Structure & Module Organization

- `homebrew/botty/`: C++17 download/extraction service. Implementation lives in `src/`, browser assets in `ui/`, and UnRAR dependencies in `vendor/`.
- `homebrew/botty-native/`: C++20 controller-driven PS5 application. Use `src/`, `assets/`, `sce_sys/`, `tests/`, and packaging utilities in `tools/`.
- `vps-site/`: browser portal, JavaScript ES modules in `src/`, and deployable packages in `apps/`.
- `tests/`: portal and installer tests. Homebrew components have separate tests.
- `scripts/` contains release packaging; `deployment/` contains server configuration. `Relapse-Exploit/` and `payloads/` hold exploit/payload components. Consult `PLAN-BOTTY-NATIVE.md` before architectural changes.

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
