#!/usr/bin/env bash
# Read a short-lived repository registration token from stdin; never persist a PAT.
set -euo pipefail
slot=$1
case "$slot" in
  botty) repo=Portablelle/Botty-Plus ;;
  portal) repo=Portablelle/Portal-Plus ;;
  *) exit 2 ;;
esac
config="$HOME/.config/plus-runner/$slot"
mkdir -p "$config"
chmod 700 "$HOME/.config/plus-runner" "$config"
if [[ -f "$config/.runner" ]]; then
  echo "Runner identity already exists; revoke it in GitHub before re-registering." >&2
  exit 1
fi
docker run --rm -i --user root --env RUNNER_ALLOW_RUNASROOT=1 \
  --env "PLUS_REPO=$repo" --env "PLUS_SLOT=$slot" \
  --volume "$config:/identity" --entrypoint bash plus-runner:latest -c '
    token=$(cat)
    ./config.sh --unattended --disableupdate --url "https://github.com/$PLUS_REPO" \
      --token "$token" --name "dedie-$PLUS_SLOT-plus" --labels "$PLUS_SLOT-plus-ci" --work _work
    cp .runner .credentials .credentials_rsaparams /identity/
    chmod 600 /identity/.runner /identity/.credentials /identity/.credentials_rsaparams
  '
