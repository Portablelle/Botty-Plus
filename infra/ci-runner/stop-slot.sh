#!/usr/bin/env bash
# ExecStopPost also runs after an unexpected or forced death of slot.sh.
set -euo pipefail
slot=$1
[[ $slot == botty || $slot == portal ]] || exit 2
container=plus-$slot
timeout 25 docker stop --time 20 "$container" >/dev/null 2>&1 || true
timeout 10 docker rm -f "$container" >/dev/null 2>&1 || true
state="${XDG_RUNTIME_DIR:?}/plus-runner-$slot"
rm -f "$state"/jit.*
if [[ -f "$state/runner-id" ]]; then
  runner_id=$(cat "$state/runner-id")
  if [[ ! "$runner_id" =~ ^[1-9][0-9]*$ ]]; then
    rm -f "$state/runner-id"
    echo "Discarding malformed local runner-id; no API deletion attempted." >&2
    exit 2
  fi
  sudo -n /usr/local/sbin/plus-runner-api "$slot" delete "$runner_id"
fi
rm -f "$state/runner-id" "$state"/jit.*
