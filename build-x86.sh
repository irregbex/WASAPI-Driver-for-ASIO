#!/bin/sh
# Cross-compiles from Linux with mingw-w64 (i686, 32-bit). Produces WasapiDriver32.dll with no mingw runtime dependencies.
# WASAPI Driver for ASIO build script. SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")"
mkdir -p obj
i686-w64-mingw32-windres $WB_FLAGS --include-dir src src/WasapiDriver.rc -O coff -o obj/res32.o
i686-w64-mingw32-g++ -O2 -ffile-prefix-map="$PWD"=. -fmacro-prefix-map="$PWD"=. $WB_FLAGS -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Ithird_party/asiosdk \
  -shared -Wl,--enable-stdcall-fixup -static -static-libgcc -static-libstdc++ -s \
  src/dll.cpp src/driver.cpp src/config.cpp src/panel.cpp src/crash.cpp src/update.cpp src/trace.cpp obj/res32.o src/WasapiBridge.def \
  -o WasapiDriver32.dll \
  -lole32 -luuid -lavrt -lwinmm -lshell32 -luser32 -ladvapi32 -lgdi32
echo "OK: WasapiDriver32.dll"
