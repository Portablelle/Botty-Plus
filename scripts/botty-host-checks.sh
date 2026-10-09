#!/usr/bin/env bash
# Host validation matching .github/workflows/botty-checks.yml; no console access.
set -euo pipefail
cd "${BOTTY_HOST_CHECK_ROOT:-$(dirname "$0")/..}"

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

run_check "Runner infrastructure" python3 -m unittest discover -s infra/ci-runner -p 'test_*.py' -v
run_check "Release packages" python3 scripts/botty-packages.py --check
run_check "Repository contracts" python3 -m unittest discover -s tests -p 'test_*.py' -v
run_check "Archive fixtures" python3 homebrew/botty/tests/make_fixtures.py
run_check "Service UI" node homebrew/botty/tests/ui-test.js
# Bound compilation rather than using the host CPU count from nproc.
# Build prerequisites can run in parallel; the test recipe stays sequential.
run_check "Service build and tests" make -C homebrew/botty -j4 NATIVE_CXX=clang++ test
run_check "Native build" make -C homebrew/botty-native -j4 CXX=clang++ host-test-build
run_check "Native tests" make -C homebrew/botty-native CXX=clang++ test integration
run_check "Compression worker" python3 -m unittest discover -s homebrew/game-compressor/tests -p 'test_*.py' -v
clang++ -std=c++20 -Wall -Wextra -Werror homebrew/rtorrent/tests/runtime-identity-harness.cc -o /tmp/rtorrent-runtime-identity-test
RT_IDENTITY_HARNESS=/tmp/rtorrent-runtime-identity-test run_check "rTorrent" python3 -m unittest discover -s homebrew/rtorrent/tests -v
clang++ -std=c++20 -Wall -Wextra -Werror homebrew/rtorrent/tests/runtime-at-host-test.cc -o /tmp/rtorrent-runtime-at-host-test
/tmp/rtorrent-runtime-at-host-test
run_check "Artwork service" python3 deployment/botty-artwork/test_server.py
