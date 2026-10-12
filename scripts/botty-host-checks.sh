#!/usr/bin/env bash
# Host validation matching .github/workflows/botty-checks.yml; no console access.
# BOTTY_CHANGED_FILES may name a file listing the PR's changed paths, one per line.
# Fast checks always run; the compiled suites run only when a changed path can
# affect them. Without a readable list, every suite runs. `--print-scope` prints
# the selected compiled suites and exits. BOTTY_HOST_CHECK_BUILD_ONLY=1 compiles
# the selected suites without running their tests: used on the default branch
# when the merged tree was already validated, so the shared ccache stays warm.
set -euo pipefail
cd "${BOTTY_HOST_CHECK_ROOT:-$(dirname "$0")/..}"

service=true native=true rtorrent=true
if [ -n "${BOTTY_CHANGED_FILES:-}" ] && [ -r "$BOTTY_CHANGED_FILES" ]; then
  service=false native=false rtorrent=false
  while IFS= read -r path || [ -n "$path" ]; do
    [ -n "$path" ] || continue
    case "$path" in
      homebrew/botty/*|deployment/botty-remote/*)
        # botty-native builds against ../botty.
        service=true native=true ;;
      homebrew/botty-native/*)
        native=true ;;
      homebrew/rtorrent/*|packages/rtorrent/*)
        rtorrent=true ;;
      # Covered by the fast checks below, or not exercised by host checks.
      *.md|LICENSE|docs/*|.github/*|.claude/*|infra/*|tests/*|deployment/*|\
      homebrew/game-compressor/*|homebrew/rest-mode-companion/*|homebrew/rest-mode-probe/*|\
      packages/botty/*|packages/botty-native/*|packages/botty-release.json|\
      scripts/botty-packages.py|scripts/package-*.py|scripts/release_sources.py)
        ;;
      # Unknown paths, including this script, keep the full suite.
      *)
        service=true native=true rtorrent=true ;;
    esac
  done < "$BOTTY_CHANGED_FILES"
fi
build_only=false
[ "${BOTTY_HOST_CHECK_BUILD_ONLY:-}" = 1 ] && build_only=true
if [ "${1:-}" = --print-scope ]; then
  echo "service=$service native=$native rtorrent=$rtorrent"
  exit 0
fi

# ccache reuses objects restored from the default branch; paths stay relative to
# the checkout so the PR export and the main workspace share cache entries.
cxx=clang++
if command -v ccache >/dev/null; then
  cxx="ccache clang++"
  export CCACHE_BASEDIR=$PWD CCACHE_COMPILERCHECK=content CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-1G}
  ccache --zero-stats >/dev/null
fi

run_check() {
  local label=$1
  shift
  local started=$SECONDS
  echo "::group::$label"
  local result=0
  "$@" || result=$?
  echo "Host check: $label ($((SECONDS - started))s, exit $result)"
  echo "::endgroup::"
  return "$result"
}

skip_check() {
  echo "Host check: $1 (skipped: ${2:-no relevant changes})"
}

run_check "Runner infrastructure" python3 -m unittest discover -s infra/ci-runner -p 'test_*.py' -v
run_check "Release packages" python3 scripts/botty-packages.py --check
run_check "Repository contracts" python3 -m unittest discover -s tests -p 'test_*.py' -v
run_check "Archive fixtures" python3 homebrew/botty/tests/make_fixtures.py
run_check "Rest companion protocol" make -C homebrew/rest-mode-companion test
run_check "Service UI" node homebrew/botty/tests/ui-test.js
# Bound compilation rather than using the host CPU count from nproc.
# Build prerequisites and independent service tests run four at a time.
if [ "$service" = true ] && [ "$build_only" = true ]; then
  run_check "Service build" make -C homebrew/botty -j4 NATIVE_CXX="$cxx" test-build
elif [ "$service" = true ]; then
  run_check "Service build and tests" make -C homebrew/botty -j4 NATIVE_CXX="$cxx" TEST_JOBS=4 test
else
  skip_check "Service build and tests"
fi
if [ "$native" = true ]; then
  # Native integration launches the service executable even when its suite is skipped.
  if [ "$service" != true ]; then
    run_check "Service executable" make -C homebrew/botty -j4 NATIVE_CXX="$cxx" native
  fi
  run_check "Native build" make -C homebrew/botty-native -j4 CXX="$cxx" host-test-build
  if [ "$build_only" = true ]; then
    skip_check "Native tests" "build only"
  else
    run_check "Native tests" make -C homebrew/botty-native CXX="$cxx" test integration
  fi
else
  skip_check "Native build"
  skip_check "Native tests"
fi
run_check "Compression worker" python3 -m unittest discover -s homebrew/game-compressor/tests -p 'test_*.py' -v
if [ "$rtorrent" = true ]; then
  $cxx -std=c++20 -Wall -Wextra -Werror homebrew/rtorrent/tests/runtime-identity-harness.cc -o /tmp/rtorrent-runtime-identity-test
  $cxx -std=c++20 -Wall -Wextra -Werror homebrew/rtorrent/tests/runtime-at-host-test.cc -o /tmp/rtorrent-runtime-at-host-test
  if [ "$build_only" = true ]; then
    skip_check "rTorrent" "build only"
  else
    RT_IDENTITY_HARNESS=/tmp/rtorrent-runtime-identity-test run_check "rTorrent" python3 -m unittest discover -s homebrew/rtorrent/tests -v
    /tmp/rtorrent-runtime-at-host-test
  fi
else
  skip_check "rTorrent"
fi
run_check "Artwork service" python3 deployment/botty-artwork/test_server.py
if [ "$cxx" != clang++ ]; then
  ccache --show-stats
fi
