#!/usr/bin/env bash
# Reproducible upstream inputs; run inside the companion Docker image.
set -euo pipefail
cd "$(dirname "$0")"
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
mkdir -p build/downloads build/src build/prefix
prefix="$PWD/build/prefix"
fetch() {
    local name=$1 url=$2 hash=$3
    if ! test -f "build/downloads/$name"; then
        curl --fail --location --retry 3 "$url" -o "build/downloads/$name.tmp"
        mv "build/downloads/$name.tmp" "build/downloads/$name"
    fi
    echo "$hash  build/downloads/$name" | sha256sum -c -
}
fetch libtorrent-0.16.24.tar.gz https://codeload.github.com/rakshasa/libtorrent/tar.gz/refs/tags/v0.16.24 626cb6e7272296e7f52fd2310744e080104cf00fe4562a1c253fe327dcfc401c
fetch rtorrent-0.16.24.tar.gz https://github.com/rakshasa/rtorrent/releases/download/v0.16.24/rtorrent-0.16.24.tar.gz 269d82054bdf3862194722c5e27bddc12abb51d45045bf6df1bd7ee3a2837bab
export CC="$PS5_PAYLOAD_SDK/bin/prospero-clang"
export CXX="$PS5_PAYLOAD_SDK/bin/prospero-clang++"
export AR=llvm-ar-18 RANLIB=llvm-ranlib-18 STRIP=llvm-strip-18
export CFLAGS="-O2" CXXFLAGS="-O2 -std=c++20"
export PKG_CONFIG="pkg-config --static"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$PS5_PAYLOAD_SDK/target/user/homebrew/lib/pkgconfig"
export CPPFLAGS="-I$prefix/include -include /work/ps5-compat.hpp"
export LDFLAGS="-L$prefix/lib"
for component in libtorrent rtorrent; do
    src="$PWD/build/src/$component-0.16.24"
    if ! test -d "$src"; then tar xf "build/downloads/$component-0.16.24.tar.gz" -C build/src; fi
    (
        cd "$src"
        if test "$component" = libtorrent; then
            python3 /work/patch-libtorrent.py "$src"
            python3 - <<'PY'
from pathlib import Path
p = Path('src/torrent/net/socket_address.cc')
s = p.read_text()
old = 'if (ret == EAI_NONAME || ret == EAI_ADDRFAMILY)'
if old in s:
    s = s.replace(old, 'if (ret == EAI_NONAME\n#ifdef EAI_ADDRFAMILY\n      || ret == EAI_ADDRFAMILY\n#endif\n  )')
    p.write_text(s)
PY
        fi
        if test "$component" = rtorrent; then
            cp /work/ps5-entry.hpp src/ps5-entry.hpp
            cp /work/runtime-identity.hpp src/runtime-identity.hpp
            python3 - <<'PY'
from pathlib import Path
p = Path('src/main.cc')
s = p.read_text()
if 'botty_rtorrent_init' not in s:
    needle = 'main(int argc, char** argv) {'
    assert s.count(needle) == 1
    s = s.replace(needle, needle + '\n  if (botty_rtorrent_init(argc, argv)) return 1;')
    s = s.replace('#include "config.h"', '#include "config.h"\n#include "ps5-entry.hpp"', 1)
    p.write_text(s)
PY
        fi
        if ! test -f configure; then autoreconf -fi; fi
        flags=(--host=x86_64-pc-freebsd --prefix="$prefix" --disable-shared --enable-static --disable-execinfo --disable-debug)
        if test "$component" = libtorrent; then
            flags+=(--without-epoll --without-inotify)
        else
            flags+=(--without-ncurses --without-lua --without-systemd)
        fi
        ./configure "${flags[@]}"
        make -j"${JOBS:-4}"
        make install
    )
done
cp "$prefix/bin/rtorrent" build/rtorrent.elf
sha256sum build/rtorrent.elf
