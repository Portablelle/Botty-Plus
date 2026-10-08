#!/usr/bin/env bash
# Host validation matching .github/workflows/botty-checks.yml; no console access.
set -euo pipefail
cd "$(dirname "$0")/.."

python3 -m unittest discover -s infra/ci-runner -p 'test_*.py' -v
python3 scripts/botty-packages.py --check
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 homebrew/botty/tests/make_fixtures.py
node homebrew/botty/tests/ui-test.js
make -C homebrew/botty NATIVE_CXX=clang++ test
make -C homebrew/botty-native CXX=clang++ test integration
python3 -m unittest discover -s homebrew/game-compressor/tests -p 'test_*.py' -v
clang++ -std=c++20 -Wall -Wextra -Werror homebrew/rtorrent/tests/runtime-identity-harness.cc -o /tmp/rtorrent-runtime-identity-test
RT_IDENTITY_HARNESS=/tmp/rtorrent-runtime-identity-test python3 -m unittest discover -s homebrew/rtorrent/tests -v
clang++ -std=c++20 -Wall -Wextra -Werror homebrew/rtorrent/tests/runtime-at-host-test.cc -o /tmp/rtorrent-runtime-at-host-test
/tmp/rtorrent-runtime-at-host-test
python3 deployment/botty-artwork/test_server.py
