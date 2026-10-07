#!/usr/bin/env bash
# A single-use GitHub JIT identity and a fresh bounded container for every job.
set -euo pipefail
umask 077
slot=$1
[[ $slot == botty || $slot == portal ]] || exit 2
container=plus-$slot
network=plus-ci-$slot
state="${XDG_RUNTIME_DIR:?}/plus-runner-$slot"
mkdir -p "$state"
runner_id=
env_file=
unregister() {
  if [[ -n "$runner_id" ]]; then
    if ! sudo -n /usr/local/sbin/plus-runner-api "$slot" delete "$runner_id"; then
      echo "JIT_CLEANUP_FAILED: preserving runner $runner_id for retry." >&2
      return 1
    fi
    runner_id=
  fi
  rm -f "$state/runner-id"
}
cleanup() {
  [[ -z "$env_file" ]] || rm -f "$env_file"
  timeout 25 docker stop --time 20 "$container" >/dev/null 2>&1 || true
  timeout 10 docker rm -f "$container" >/dev/null 2>&1 || true
  unregister || true
}
trap 'exit 0' TERM INT
trap cleanup EXIT
docker rm -f "$container" >/dev/null 2>&1 || true
retry_delay=15
backoff() {
  echo "SLOT_RETRY: retrying in $retry_delay s." >&2
  sleep "$retry_delay" & wait $!
  retry_delay=$((retry_delay < 150 ? retry_delay * 2 : 300))
}
while true; do
  # Recover a failed deletion after service/process restart before registering again.
  if [[ -f "$state/runner-id" ]]; then
    runner_id=$(cat "$state/runner-id")
    if [[ ! "$runner_id" =~ ^[1-9][0-9]*$ ]]; then
      echo "Discarding malformed local runner-id; no API deletion attempted." >&2
      rm -f "$state/runner-id"
      runner_id=
      continue
    fi
    if ! unregister; then
      backoff
      continue
    fi
  fi
  # Never allocate a JIT identity while the container daemon/network is unavailable.
  if ! docker info >/dev/null 2>&1; then
    echo "DOCKER_NOT_READY" >&2
    backoff
    continue
  fi
  if ! docker network inspect "$network" >/dev/null 2>&1 &&
      ! docker network create --opt com.docker.network.bridge.enable_icc=false "$network" >/dev/null; then
    echo "NETWORK_NOT_READY: could not create isolated Docker bridge $network." >&2
    backoff
    continue
  fi
  docker rm -f "$container" >/dev/null 2>&1 || true
  if ! reply=$(sudo -n /usr/local/sbin/plus-runner-api "$slot" create); then
    echo "JIT_REGISTRATION_FAILED: retrying in $retry_delay s; check the broker error above." >&2
    backoff
    continue
  fi
  if ! runner_id=$(jq -er '.runner.id | select(type == "number" and . > 0)' <<<"$reply"); then
    unset reply
    backoff
    continue
  fi
  printf '%s\n' "$runner_id" > "$state/runner-id"
  env_file=$(mktemp "$state/jit.XXXXXX")
  if ! jit_config=$(jq -er '.encoded_jit_config | select(type == "string" and length > 0)' <<<"$reply"); then
    unset reply
    rm -f "$env_file"
    env_file=
    unregister || true
    backoff
    continue
  fi
  printf 'ACTIONS_RUNNER_INPUT_JITCONFIG=%s\n' "$jit_config" > "$env_file"
  unset jit_config
  unset reply
  if ! docker create --name "$container" --env-file "$env_file" \
    --env HOME=/home/runner --env RUNNER_MANUALLY_TRAP_SIG=1 --env AGENT_TOOLSDIRECTORY=/home/runner/_toolcache \
    --env RUNNER_TOOL_CACHE=/home/runner/_toolcache --network "$network" \
    --cpus 4 --memory 8g --memory-swap 8g --pids-limit 4096 \
    --read-only --cap-drop ALL --security-opt no-new-privileges \
    --tmpfs /home/runner:rw,exec,nosuid,nodev,size=4g,uid=1001,gid=1001,mode=0700 \
    --tmpfs /tmp:rw,exec,nosuid,nodev,size=2g,mode=1777 \
    --log-driver local --log-opt max-size=10m --log-opt max-file=2 \
    --entrypoint bash plus-runner:latest -c '
      set -euo pipefail
      cp -a /opt/actions-runner/. /home/runner/
      cp -a /opt/hostedtoolcache /home/runner/_toolcache
      exec ./run.sh
    ' >/dev/null; then
    rm -f "$env_file"
    env_file=
    unregister || true
    backoff
    continue
  fi
  rm -f "$env_file"
  env_file=
  docker start --attach "$container" &
  session_status=0
  wait $! || session_status=$?
  docker rm -f "$container" >/dev/null 2>&1 || true
  unregister || true
  if ((session_status != 0)); then
    echo "RUNNER_SESSION_FAILED: exit $session_status" >&2
    backoff
    continue
  fi
  retry_delay=15
  sleep 5 & wait $!
done
