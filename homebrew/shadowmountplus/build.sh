#!/bin/sh
set -eu
cd "$(dirname "$0")"
if [ ! -d build ]; then python3 prepare.py; fi
CC=clang-18 tests/run.sh
make -C build -j2 VERSION_TAG=1.7beta3-botty.1 \
  PS5_SCE_STUBS_DIR="$(pwd)/vendor/sdk-stubs"
