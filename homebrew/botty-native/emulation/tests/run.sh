#!/usr/bin/env bash
# Run on the dedicated Linux build host against a prepared engine checkout.
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'Run engine checks on dedie.' >&2; exit 1; }
engine=$(cd "${1:?prepared engine checkout}" && pwd)
mkdir -p "${2:?test output directory}"
output=$(cd "$2" && pwd)
tests=$(cd "$(dirname "$0")" && pwd)
cache=$(cat "$engine/.local/headless-cache")
json=$(find "$cache/source/.cache/cpm/nlohmann_json" -maxdepth 2 -type d -name single_include -print -quit)
[[ -n $json ]]
cxx=${CXX:-c++}
cc=${CC:-cc}
flags=(-std=c++20 -I"$engine/headless" -I"$json")
"$cxx" "${flags[@]}" "$tests/content_paths_check.cpp" -o "$output/content-paths-check"
"$output/content-paths-check"
"$cxx" "${flags[@]}" "$tests/config_check.cpp" -o "$output/config-check"
"$output/config-check"
"$cc" -I"$engine/headless" -c "$engine/headless/update_check/self_update_sha256.c" -o "$output/sha256.o"
"$cxx" "${flags[@]}" "$tests/ultracam_check.cpp" "$output/sha256.o" -o "$output/ultracam-check"
fixture=$(mktemp -d "$output/ultracam.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
"$output/ultracam-check" "$tests/../ultracam/botw" "$fixture"
"$cc" "$tests/lifecycle_check.c" "$engine/src/lifecycle.c" -o "$output/lifecycle-check"
"$output/lifecycle-check"
python3 -m unittest discover -s "$tests" -p 'test_*.py' -v
echo 'Engine configuration, content roots, UltraCam, lifecycle and provenance checks PASS'
