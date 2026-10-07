# Third-party source

- Native title build/runtime/VideoOut renderer: blackbearreloaded/ps5-native-app-boilerplate,
  commit `dd44bbdc75437332ed22e3ba95126733419ef25a`, GPL-3.0-or-later.
  https://github.com/blackbearreloaded/ps5-native-app-boilerplate
  The complete pinned source and notices are in `boilerplate-dd44bbd.tar.gz`.
  Archive SHA-256: `133b4ec9d21d1f49148c823131d7fb73f93fe653d35407f71702546a88a12831`.
  `src/renderer.cpp` and `.hpp` are modified derivatives: live rendering,
  input callback, flip-event pacing, orderly release, and Botty diagnostics.
- Public PS5 controller ABI: ps5-payload-dev/SDL,
  commit `ee4c47dc0d617b3bc8f35108f9956baf228a1322`, zlib license.
  https://github.com/ps5-payload-dev/SDL
  `ps5-pad.h` is the unmodified `src/joystick/ps5/SDL_ps5joystick.h`;
  license retained in `SDL-LICENSE.txt`. SDL itself is not linked.
- The native builder pins public payload SDK v0.42 and zlib 1.3.2 by SHA-256
  inside its `tools/setup-native-dependencies.sh`. This native-title runtime
  is independent of the existing Botty daemon's v0.43 payload runtime.
  No Sony SDK libraries or firmware files are packaged.
- Manrope variable font from Google Fonts, SIL Open Font License 1.1.
  https://github.com/google/fonts/tree/main/ofl/manrope
  Vendored font SHA-256: `3ae11c49db0455a3cc33e37d380f20fdb8c7f8b41dc07625c177e3d87a9d6ae6`.
  Source and license are in `manrope/`; the license is also included in the title.
  Native alpha masks are baked using Pillow 12.0.0 by `tools/build-font.py`.
