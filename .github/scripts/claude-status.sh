#!/usr/bin/env bash
# Read the latest authentic Actions status, including its creator (absent from /status).
set -euo pipefail
context=$1 field=$2
case "$context" in
  'Claude host checks'|'Claude review completion') ;;
  *) echo 'Unsupported status context' >&2; exit 2 ;;
esac
case "$field" in state|target_url) ;; *) exit 2 ;; esac
[[ "$HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 2
gh api "repos/$REPO/commits/$HEAD/statuses?per_page=100" --paginate |
  jq -sr --arg context "$context" --arg field "$field" \
    '[.[][] | select(.context == $context and .creator.login == "github-actions[bot]")][0][$field] // "missing"'
