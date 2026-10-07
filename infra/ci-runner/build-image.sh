#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

for command in curl jq docker; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Missing required host command: $command" >&2
    echo "Install curl and jq with: sudo apt-get install -y curl jq" >&2
    exit 1
  fi
done

curl_args=(--fail --silent --show-error --location --retry 3 --retry-all-errors --connect-timeout 15 --max-time 120)
release=$(curl "${curl_args[@]}" https://api.github.com/repos/actions/runner/releases/latest)
runner=$(jq -er '.tag_name // empty' <<<"$release")
node=$(curl "${curl_args[@]}" https://nodejs.org/dist/index.json | jq -er '[.[] | select(.version | test("^v24\\.[0-9]+\\.[0-9]+$"))][0].version // empty')

if [[ ! "$runner" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Invalid Actions runner version: ${runner:-<empty>}" >&2
  exit 1
fi
if [[ ! "$node" =~ ^v24\.[0-9]+\.[0-9]+$ ]]; then
  echo "Invalid Node 24 version: ${node:-<empty>}" >&2
  exit 1
fi

runner_archive="actions-runner-linux-x64-${runner#v}.tar.gz"
runner_sha256=$(jq -er --arg name "$runner_archive" '.assets[] | select(.name == $name) | (.digest // empty) | sub("^sha256:"; "")' <<<"$release")
node_archive="node-v${node#v}-linux-x64.tar.xz"
node_sha256=$(curl "${curl_args[@]}" "https://nodejs.org/dist/${node}/SHASUMS256.txt" \
  | jq -Rer --arg name "$node_archive" 'split(" ") | map(select(length > 0)) | select(.[1] == $name) | .[0]')

if [[ ! "$runner_sha256" =~ ^[0-9a-fA-F]{64}$ ]]; then
  echo "Invalid Actions runner SHA-256 for $runner_archive" >&2
  exit 1
fi
if [[ ! "$node_sha256" =~ ^[0-9a-fA-F]{64}$ ]]; then
  echo "Invalid Node SHA-256 for $node_archive" >&2
  exit 1
fi

image_tag="runner-${runner#v}-node-${node#v}"
# Legacy Docker builder applies these limits to build containers: 2 CPUs and 4 GiB RAM/swap.
docker build --pull --quiet --cpu-period 100000 --cpu-quota 200000 --memory 4g --memory-swap 4g \
  --tag plus-runner:latest --tag "plus-runner:$image_tag" \
  --build-arg "RUNNER_VERSION=${runner#v}" --build-arg "NODE_VERSION=${node#v}" \
  --build-arg "RUNNER_SHA256=$runner_sha256" --build-arg "NODE_SHA256=$node_sha256" .

# Keep the current versioned image and remove only older Plus runner image tags.
while IFS= read -r tag; do
  if [[ "$tag" != "$image_tag" ]] && ! docker image rm "plus-runner:$tag"; then
    # A running job can still use the previous image; leave it for next week.
    echo "Keeping in-use Plus runner image: plus-runner:$tag" >&2
  fi
done < <(docker image ls --filter 'label=com.portablelle.plus-runner=true' --format '{{.Tag}}' plus-runner | grep '^runner-.*-node-.*$' || true)
if ! docker image prune --force --filter 'label=com.portablelle.plus-runner=true' --filter 'dangling=true'; then
  echo "Could not prune unused Plus runner images; they will be retried next week." >&2
fi

echo "plus-runner: runner ${runner#v}, node ${node#v}"
