#!/usr/bin/env bash
# Durable per-PR attempt ledger, independent of rebases and commit subjects.
# Reservation runs in the serialized autofix job, before invoking Sonnet.
# Needs GH_TOKEN and REPO. Exit 3 means the two-attempt budget is exhausted.
set -euo pipefail
mode=$1 pr=$2
[[ "$pr" =~ ^[0-9]+$ ]] || exit 2

keys=$(gh api "repos/$REPO/issues/$pr/comments" --paginate \
  --jq '.[] | select(.user.login == "github-actions[bot]" and (.body | startswith("<!-- claude-autofix-attempt ")))' \
  | jq -s '[.[].body | split("\n")[0] | capture("^<!-- claude-autofix-attempt run=(?<run>[0-9]+) attempt=(?<attempt>[0-9]+) -->$")? | .run + ":" + .attempt] | unique')
count=$(jq length <<<"$keys")
if [ "$mode" = count ]; then
  echo "$count"
  exit 0
fi
[ "$mode" = reserve ] || exit 2
run=$3 attempt=$4
[[ "$run" =~ ^[0-9]+$ && "$attempt" =~ ^[0-9]+$ ]] || exit 2
key="$run:$attempt"
# An uncertain comment-post outcome can be retried without spending another slot.
if jq -e --arg key "$key" 'index($key) != null' <<<"$keys" >/dev/null; then
  echo "$count"
  exit 0
fi
[ "$count" -lt 2 ] || exit 3
round=$((count + 1))
printf '<!-- claude-autofix-attempt run=%s attempt=%s -->\nClaude autofix attempt %s of 2 reserved before execution. This attempt remains counted if the run fails or is cancelled.\n' \
  "$run" "$attempt" "$round" | gh pr comment "$pr" -R "$REPO" --body-file - >/dev/null
echo "$round"
