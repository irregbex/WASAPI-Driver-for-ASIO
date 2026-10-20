#!/bin/sh
# ARM64 (Windows on ARM) cross-compile with Zig (pip install ziglang), which bundles mingw-w64 and its rc compiler.
# WASAPI Driver for ASIO build script. SPDX-License-Identifier: GPL-3.0-or-later
set -e
cd "$(dirname "$0")"
python3 -m ziglang c++ -target aarch64-windows-gnu -O2 -ffile-prefix-map="$PWD"=. -fmacro-prefix-map="$PWD"=. $WB_FLAGS -std=c++17 -Wall -Wno-unused-parameter -Wno-missing-braces -Wno-nullability-completeness \
  -DUNICODE -D_UNICODE -Ithird_party/asiosdk -shared -s \
  src/dll.cpp src/driver.cpp src/config.cpp src/panel.cpp src/crash.cpp src/update.cpp src/trace.cpp src/WasapiDriver.rc src/WasapiBridge.def \
  -o WasapiDriverARM64.dll \
  -lole32 -luuid -lavrt -lwinmm -lshell32 -luser32 -ladvapi32 -lgdi32
rm -f WasapiDriverARM64.lib WasapiDriverARM64.pdb dll.lib    # import libraries and debug file zig writes next to the DLL (0.9.8: dll.lib too)
echo "OK: WasapiDriverARM64.dll"
