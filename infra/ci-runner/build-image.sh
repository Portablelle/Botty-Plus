#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
runner=$(curl -fsS https://api.github.com/repos/actions/runner/releases/latest | jq -r .tag_name)
node=$(curl -fsS https://nodejs.org/dist/index.json | jq -r '[.[] | select(.version | startswith("v24."))][0].version')
docker build --pull --quiet --tag plus-runner:latest \
  --build-arg "RUNNER_VERSION=${runner#v}" --build-arg "NODE_VERSION=${node#v}" .
echo "plus-runner: runner ${runner#v}, node ${node#v}"
