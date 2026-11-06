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
"$cxx" "${flags[@]}" "$tests/botw_profile_check.cpp" "$output/sha256.o" -o "$output/botw-profile-check"
"$output/botw-profile-check" "$tests/../ultracam/botw" "$tests/../ultracam/botw190" "$fixture/profiles"
python3 "$tests/check_botw190_export.py" "$fixture/profiles/patch-cases.json"
python3 "$tests/../generate_botw190.py" --check
"$cc" "$tests/lifecycle_check.c" "$engine/src/lifecycle.c" -o "$output/lifecycle-check"
"$output/lifecycle-check"
python3 -m unittest discover -s "$tests" -p 'test_*.py' -v
python3 "$tests/check_performance.py" "$engine" "$output/performance"
python3 "$tests/check_copy_coherence.py" "$engine" "$output/coherence"
python3 "$tests/check_patch_series.py" "$engine"
python3 "$tests/check_bitmap_performance.py" "$engine"
python3 "$tests/check_astc_performance.py" "$engine"
echo 'Engine configuration, content roots, UltraCam, lifecycle and provenance checks PASS'
