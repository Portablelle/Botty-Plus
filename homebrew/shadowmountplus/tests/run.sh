#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -pthread -Itests/stubs -Ibuild/include \
  tests/test_title_dir.c build/src/sm_shellcore_bridge.S -o build/test-title-dir
./build/test-title-dir
