#!/bin/sh
# Cross-compiles from Linux with mingw-w64 (x86_64). Produces WasapiDriverTest.exe (console, statically linked).
# WASAPI Driver for ASIO build script. SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")"
mkdir -p obj
x86_64-w64-mingw32-windres -DWB_TOOL_TEST --include-dir src src/tools.rc -O coff -o obj/testres.o   # 0.9.3: application icon; 0.9.6: version info
x86_64-w64-mingw32-g++ -O2 -ffile-prefix-map="$PWD"=. -fmacro-prefix-map="$PWD"=. $WB_FLAGS -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -municode -Ithird_party/asiosdk \
  -static -static-libgcc -static-libstdc++ -s \
  src/hosttest.cpp src/config.cpp obj/testres.o \
  -o WasapiDriverTest.exe \
  -lole32 -luuid -ladvapi32 -lshell32 -luser32
echo "OK: WasapiDriverTest.exe"
