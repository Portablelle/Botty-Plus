#!/usr/bin/env bash
# Registered runner identity, fresh rootless container for every job.
set -euo pipefail
slot=$1
[[ $slot == botty || $slot == portal ]] || exit 2
container=plus-$slot
config="$HOME/.config/plus-runner/$slot"
trap 'docker rm -f "$container" >/dev/null 2>&1 || true; exit 0' TERM INT
docker rm -f "$container" >/dev/null 2>&1 || true
while true; do
  docker run --rm --name "$container" --cpus 4 --memory 8g --pids-limit 4096 \
    --user root --volume "$config:/runner-config:ro" --entrypoint bash plus-runner:latest -c '
      cp /runner-config/.runner /runner-config/.credentials /runner-config/.credentials_rsaparams /home/runner/
      chown runner:runner /home/runner/.runner /home/runner/.credentials /home/runner/.credentials_rsaparams
      exec runuser -u runner -- ./run.sh --once
    ' &
  wait $! || echo "runner exited with $?"
  sleep 5 &
  wait $!
done
