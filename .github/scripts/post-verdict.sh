#!/usr/bin/env bash
# Posts $2 as pull request $1's status message, replacing the previous one, so the thread's last comment always
# says what to do next (wait for the review, wait for Claude's fix, merge, or act).
# Needs GH_TOKEN and REPO.
set -euo pipefail
pr=$1 msg=$2

for id in $(gh api "repos/$REPO/issues/$pr/comments" --paginate \
  --jq '.[] | select(.user.login == "github-actions[bot]" and (.body | startswith("<!-- claude-verdict -->"))) | .id'); do
  gh api -X DELETE "repos/$REPO/issues/comments/$id" >/dev/null || true
done
printf '<!-- claude-verdict -->\n%s\n' "$msg" | gh pr comment "$pr" -R "$REPO" --body-file - >/dev/null
