# Botty+ contributor and reviewer instructions

Read and follow `AGENTS.md` for repository ownership, release contracts and console
safety. This repository owns the native app, Botty manager, rTorrent, compression
worker and optional artwork/Prowlarr services. Portal+, jailbreak, DNS and
installers belong to `Portablelle/Portal-Plus`.

## Host checks

Run `bash scripts/botty-host-checks.sh` on Linux with Node 24, Clang, Python 3.12+/Pillow,
libcurl, OpenSSL and zlib headers (the `botty-plus-ci` image provides these).
It mirrors `.github/workflows/botty-checks.yml`. Use focused component tests when
investigating a change. Do not invent npm lint/typecheck commands: this is a
C++/Python/JavaScript repository, without a root npm project.

Host validation does not establish PS5 runtime acceptance. Review jobs must not
access a console, send payloads, deploy, restart services or modify downloads,
archives or saves. Keep diagnostics and credentials out of public files.

## Review invariants

- Preserve original downloads, archives, saves and running extraction/compression.
- Check asynchronous jobs, cancellation, recovery and error handling for data loss,
  races and unbounded resource usage.
- Native/service API changes must agree on endpoints, ports and response formats.
- Preserve package path, hash, license and version contracts. Native and service
  versions are distinct; source and generated release artifacts ship together.
- Validate release changes with `python3 scripts/botty-packages.py --check`.
- Check exact filename case, permissions and raw SELF versus converted ELF where
  relevant. Never infer console compatibility from successful host tests.
- Match surrounding code and keep UI text, code, commits and docs in English.

Before pushing, run the host checks and review all changes using
`.claude/commands/review-pr.md`. Claude replaces Cubic for this repository;
Cubic is not required. Never force-push, merge, approve a PR or enable auto-merge
as part of a review or automatic fix.

All Claude reviews, local or on GitHub, must be written in English, including
inline comments, summaries and replies. Never review in French, regardless of
the language of the request.

## GitHub review setup

The Claude GitHub app must have access to this repository. Add a long-lived token
from `claude setup-token` as the Actions secret `CLAUDE_CODE_OAUTH_TOKEN`.
Automatic PR reviews use Opus 5.5 with high effort; `@claude` and optional autofix
use Sonnet 5.5 with high effort, matching ciaobella. Reviews publish inline
findings and a verdict for the current PR head; forks require a local review.
For a manual override, a maintainer leaves a commented review on the current
head whose first line is `claude-review-override`, then adds that label. The
verdict verifies write permission and the review's exact `commit_id`, so a later
push invalidates the override even on forks or reopened PRs. Same-repo pushes
also remove the label. Commands: `gh pr review <n> --comment --body claude-review-override`,
then `gh pr edit <n> --add-label claude-review-override`. Use a single-line review
body. If the label already exists, remove it before adding it again so the
verdict runs on the new authorization. CI reads review policy and verdict scripts
from the PR base. Policy-changing PRs require human inspection; automated fixes
do not rewrite the review controls. Ordinary PR checks are not tamper-proof
against repository writers editing workflows, so use GitHub rulesets/required
workflows if enforcement against privileged PR authors is required.

Autofix makes at most two attempts on the same PR. Each attempt is reserved
before Sonnet runs in a persistent `github-actions[bot]` PR comment keyed by
workflow run/attempt. Squashing or rebasing commits does not reset the budget;
failed/cancelled attempts remain counted. Reservations are serialized. Stale
checkouts stop before invoking Claude; a head change during editing makes Claude
stop before pushing. The action never asks Claude to pull or rebase. Configure
`Claude review verdict` as a required branch check after activation if merge
enforcement is wanted; the status alone does not enforce branch protection.

On the initial workflow installation PR targeting the default branch, the base
has no reviewer workflow yet.
CI reports first-time setup without calling Claude or claiming an automatic
verdict; local review and maintainer approval are required. Once the workflow
exists on the base, missing policy files remain an error. This bootstrap path
cannot be selected by changing files only on a PR branch.
