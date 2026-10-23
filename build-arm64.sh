#!/bin/sh
# ARM64 (Windows on ARM) cross-compile with Zig (pip install ziglang), which bundles mingw-w64 and its rc compiler.
# WASAPI Driver for ASIO build script. SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")"
# 0.9.10: the SoX Resampler library (third_party/soxr, LGPL-2.1-or-later; see its README-vendored.txt) as C, one
# object per file in obj/soxra64/. Third-party code: warnings are switched off (-w) for these files ONLY; src/ keeps
# -Wall -Wextra and must stay at 0 warnings. data-io-portable.c replaces src/data-io.c (portable rounding);
# fifo-headroom.c is the FIFO allocator named in third_party/soxr/soxr-config.h.
SOXR_SRC="third_party/soxr/src/soxr.c third_party/soxr/data-io-portable.c third_party/soxr/fifo-headroom.c third_party/soxr/src/dbesi0.c
  third_party/soxr/src/filter.c third_party/soxr/src/fft4g32.c third_party/soxr/src/fft4g64.c third_party/soxr/src/cr.c third_party/soxr/src/cr32.c"
mkdir -p obj/soxra64
SOXR_OBJ=""
for f in $SOXR_SRC; do
  o="obj/soxra64/$(basename "$f" .c).o"
  python3 -m ziglang cc -target aarch64-windows-gnu -O2 -ffile-prefix-map="$PWD"=. -fmacro-prefix-map="$PWD"=. -std=gnu99 -w -DSOXR_LIB -DNDEBUG -Ithird_party/soxr -c "$f" -o "$o"
  SOXR_OBJ="$SOXR_OBJ $o"
done
python3 -m ziglang c++ -target aarch64-windows-gnu -O2 -ffile-prefix-map="$PWD"=. -fmacro-prefix-map="$PWD"=. $WB_FLAGS -std=c++17 -Wall -Wno-unused-parameter -Wno-missing-braces -Wno-nullability-completeness \
  -DUNICODE -D_UNICODE -Ithird_party/asiosdk -Ithird_party/soxr/src -shared -s \
  src/dll.cpp src/driver.cpp src/config.cpp src/panel.cpp src/crash.cpp src/update.cpp src/trace.cpp $SOXR_OBJ src/WasapiDriver.rc src/WasapiBridge.def \
  -o WasapiDriverARM64.dll \
  -lole32 -luuid -lavrt -lwinmm -lshell32 -luser32 -ladvapi32 -lgdi32
rm -f WasapiDriverARM64.lib WasapiDriverARM64.pdb dll.lib    # import libraries and debug file zig writes next to the DLL (0.9.8: dll.lib too)
echo "OK: WasapiDriverARM64.dll"
