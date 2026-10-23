#!/bin/sh
# Cross-compiles from Linux with mingw-w64 (x86_64). Produces WasapiDriver64.dll with no mingw runtime dependencies.
# WASAPI Driver for ASIO build script. SPDX-License-Identifier: GPL-3.0-or-later
# Optional: WB_FLAGS="-DWB_SHOW_ASIO_LOGO=0" sh build.sh removes the ASIO Compatible logo (see TRADEMARKS.txt).
set -e
cd "$(dirname "$0")"
mkdir -p obj
# 0.9.10: the SoX Resampler library (third_party/soxr, LGPL-2.1-or-later; see its README-vendored.txt) as C, one
# object per file in obj/soxr64/. Third-party code: warnings are switched off (-w) for these files ONLY; src/ keeps
# -Wall -Wextra and must stay at 0 warnings. data-io-portable.c replaces src/data-io.c (portable rounding);
# fifo-headroom.c is the FIFO allocator named in third_party/soxr/soxr-config.h.
SOXR_SRC="third_party/soxr/src/soxr.c third_party/soxr/data-io-portable.c third_party/soxr/fifo-headroom.c third_party/soxr/src/dbesi0.c
  third_party/soxr/src/filter.c third_party/soxr/src/fft4g32.c third_party/soxr/src/fft4g64.c third_party/soxr/src/cr.c third_party/soxr/src/cr32.c"
mkdir -p obj/soxr64
SOXR_OBJ=""
for f in $SOXR_SRC; do
  o="obj/soxr64/$(basename "$f" .c).o"
  x86_64-w64-mingw32-gcc -O2 -ffile-prefix-map="$PWD"=. -fmacro-prefix-map="$PWD"=. -std=gnu99 -w -DSOXR_LIB -DNDEBUG -Ithird_party/soxr -c "$f" -o "$o"
  SOXR_OBJ="$SOXR_OBJ $o"
done
x86_64-w64-mingw32-windres $WB_FLAGS --include-dir src src/WasapiDriver.rc -O coff -o obj/res64.o
x86_64-w64-mingw32-g++ -O2 -ffile-prefix-map="$PWD"=. -fmacro-prefix-map="$PWD"=. $WB_FLAGS -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Ithird_party/asiosdk -Ithird_party/soxr/src \
  -shared -static -static-libgcc -static-libstdc++ -s \
  src/dll.cpp src/driver.cpp src/config.cpp src/panel.cpp src/crash.cpp src/update.cpp src/trace.cpp $SOXR_OBJ obj/res64.o src/WasapiBridge.def \
  -o WasapiDriver64.dll \
  -lole32 -luuid -lavrt -lwinmm -lshell32 -luser32 -ladvapi32 -lgdi32
echo "OK: WasapiDriver64.dll"
