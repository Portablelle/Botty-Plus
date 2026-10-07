#!/usr/bin/env bash
# A single-use GitHub JIT identity and a fresh bounded container for every job.
set -euo pipefail
umask 077
slot=$1
[[ $slot == botty || $slot == portal ]] || exit 2
container=plus-$slot
network=plus-ci-$slot
docker network inspect "$network" >/dev/null 2>&1 || \
  docker network create --opt com.docker.network.bridge.enable_icc=false "$network" >/dev/null
state="${XDG_RUNTIME_DIR:?}/plus-runner-$slot"
mkdir -p "$state"
runner_id=
env_file=
unregister() {
  if [[ -n "$runner_id" ]]; then
    sudo -n /usr/local/sbin/plus-runner-api "$slot" delete "$runner_id" || true
    runner_id=
  fi
  rm -f "$state/runner-id"
}
cleanup() {
  [[ -z "$env_file" ]] || rm -f "$env_file"
  timeout 25 docker stop --time 20 "$container" >/dev/null 2>&1 || true
  timeout 10 docker rm -f "$container" >/dev/null 2>&1 || true
  unregister
}
trap 'exit 0' TERM INT
trap cleanup EXIT
docker rm -f "$container" >/dev/null 2>&1 || true
retry_delay=15
while true; do
  docker rm -f "$container" >/dev/null 2>&1 || true
  if ! reply=$(sudo -n /usr/local/sbin/plus-runner-api "$slot" create); then
    echo "JIT_REGISTRATION_FAILED: retrying in $retry_delay s; check the broker error above." >&2
    sleep "$retry_delay" & wait $!
    retry_delay=$((retry_delay < 150 ? retry_delay * 2 : 300))
    continue
  fi
  runner_id=$(jq -er '.runner.id | select(type == "number" and . > 0)' <<<"$reply")
  printf '%s\n' "$runner_id" > "$state/runner-id"
  env_file=$(mktemp "$state/jit.XXXXXX")
  printf 'ACTIONS_RUNNER_INPUT_JITCONFIG=%s\n' "$(jq -er '.encoded_jit_config | select(type == "string" and length > 0)' <<<"$reply")" > "$env_file"
  unset reply
  docker create --name "$container" --env-file "$env_file" \
    --env RUNNER_MANUALLY_TRAP_SIG=1 --env AGENT_TOOLSDIRECTORY=/home/runner/_toolcache \
    --env RUNNER_TOOL_CACHE=/home/runner/_toolcache --network "$network" \
    --cpus 4 --memory 8g --memory-swap 8g --pids-limit 4096 \
    --read-only --cap-drop ALL --security-opt no-new-privileges \
    --tmpfs /home/runner:rw,exec,nosuid,nodev,size=4g,uid=1001,gid=1001,mode=0700 \
    --tmpfs /tmp:rw,exec,nosuid,nodev,size=512m,mode=1777 \
    --log-driver local --log-opt max-size=10m --log-opt max-file=2 \
    --entrypoint bash plus-runner:latest -c '
      set -euo pipefail
      cp -a /opt/actions-runner/. /home/runner/
      cp -a /opt/hostedtoolcache /home/runner/_toolcache
      exec ./run.sh
    ' >/dev/null
  rm -f "$env_file"
  env_file=
  docker start --attach "$container" &
  wait $! || echo "Runner exited with $?"
  docker rm -f "$container" >/dev/null 2>&1 || true
  unregister
  retry_delay=15
  sleep 5 & wait $!
done
