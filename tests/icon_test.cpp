// WASAPI Driver for ASIO - offline test of the application icon and the tray status icons
// (Linux: g++ -O2 -std=c++17 tests/icon_test.cpp -o it && ./it [src/res/app.ico] [--dump <folder>])
// Copyright (C) 2026 The WASAPI Driver for ASIO contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
// Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License (LICENSE.txt) for more details.
//
// ASIO is a registered trademark of Steinberg Media Technologies GmbH.
//
// Purpose (0.9.3)
//   1. src/res/app.ico holds exactly the sizes 16, 20, 24, 32, 40, 48, 64, 96, 128, 256; 256 is PNG, the others
//      are 32-bit DIBs (height = 2 x size, with an AND mask) whose images are not empty.
//   2. Tray icons (src/trayicon.h) built from the real 16, 20, 24 and 32 px images for the four states: every
//      changed pixel lies in the bottom-right quadrant, the dot's center has the state color, its rim has the
//      dark outline, the other quadrants are untouched; Pick() follows the rules (red held 10 s).
//   --dump <folder> writes the composed icons as PAM (RGBA) files for the previews.
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/trayicon.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %-64s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }
static uint32_t U32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t U16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }

struct Entry { int size; std::vector<unsigned char> data; };

// Top-down straight-alpha ARGB pixels of a 32-bit DIB icon image.
static bool Decode(const Entry& e, std::vector<uint32_t>& px) {
    const unsigned char* d = e.data.data();
    if (e.data.size() < 40 || U32(d) != 40) return false;
    const int w = (int)U32(d + 4), h2 = (int)U32(d + 8), bpp = U16(d + 14);
    if (w != e.size || h2 != 2 * e.size || bpp != 32) return false;
    const size_t need = 40 + (size_t)w * w * 4 + (size_t)((w + 31) / 32) * 4 * w;
    if (e.data.size() < need) return false;
    px.assign((size_t)w * w, 0);
    for (int y = 0; y < w; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char* q = d + 40 + ((size_t)(w - 1 - y) * w + x) * 4;      // bottom-up BGRA
            px[(size_t)y * w + x] = (uint32_t)q[3] << 24 | (uint32_t)q[2] << 16 | (uint32_t)q[1] << 8 | q[0];
        }
    return true;
}

static void Dump(const std::string& folder, int size, int st, const std::vector<uint32_t>& px) {
    const std::string p = folder + "/tray-" + std::to_string(size) + "-" + std::to_string(st) + ".pam";
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", size, size);
    for (uint32_t v : px) { unsigned char b[4] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v, (unsigned char)(v >> 24)}; fwrite(b, 1, 4, f); }
    fclose(f);
}

int main(int argc, char** argv) {
    std::string path = "src/res/app.ico", dump;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else path = argv[i];
    }
    printf("Application icon and tray icon offline test\n");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("  cannot open %s\nRESULT: FAILED\n", path.c_str()); return 1; }
    std::vector<unsigned char> b;
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    std::vector<Entry> es;
    bool hdr = b.size() >= 6 && U16(&b[0]) == 0 && U16(&b[2]) == 1;
    const int count = hdr ? U16(&b[4]) : 0;
    for (int i = 0; hdr && i < count; ++i) {
        const size_t o = 6 + 16 * (size_t)i;
        if (o + 16 > b.size()) { hdr = false; break; }
        const int s = b[o] ? b[o] : 256;
        const uint32_t len = U32(&b[o + 8]), off = U32(&b[o + 12]);
        if ((size_t)off + len > b.size()) { hdr = false; break; }
        es.push_back(Entry{s, std::vector<unsigned char>(b.begin() + off, b.begin() + off + len)});
    }
    Check(hdr, "ICO header and directory");
    const int want[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    bool all = es.size() == 10;
    for (int k = 0; all && k < 10; ++k) all = es[(size_t)k].size == want[k];
    Check(all, "sizes 16, 20, 24, 32, 40, 48, 64, 96, 128, 256");
    bool png = !es.empty() && es.back().size == 256 && es.back().data.size() > 8 && !memcmp(es.back().data.data(), "\x89PNG\r\n\x1a\n", 8);
    Check(png, "256 px image stored as PNG");
    bool dibs = true, opaque = true;
    for (const Entry& e : es) {
        if (e.size == 256) continue;
        std::vector<uint32_t> px;
        if (!Decode(e, px)) { dibs = false; continue; }
        int solid = 0;
        for (uint32_t v : px) if ((v >> 24) > 200) ++solid;
        if (solid < (int)px.size() / 2) opaque = false;         // the tile covers most of the square
    }
    Check(dibs, "smaller sizes are 32-bit DIBs with an AND mask");
    Check(opaque, "every image has a visible tile");

    bool quadrant = true, center = true, rim = true, untouched = true;
    for (const Entry& e : es) {
        if (e.size > 32) continue;
        std::vector<uint32_t> base;
        if (!Decode(e, base)) { quadrant = false; continue; }
        const int s = e.size;
        for (int st = 0; st < tray::kStateCount; ++st) {
            std::vector<uint32_t> px = base;
            tray::AddStatusDot(px.data(), s, tray::kColor[st]);
            int minX = s, minY = s, changedOther = 0;
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x)
                    if (px[(size_t)y * s + x] != base[(size_t)y * s + x]) {
                        if (x < minX) minX = x;
                        if (y < minY) minY = y;
                        if (x < s / 2 || y < s / 2) ++changedOther;
                    }
            if (minX < s / 2 || minY < s / 2) quadrant = false;
            if (changedOther) untouched = false;
            const int D = tray::DotDiameter(s);
            const int c = s - (D + 1) / 2;                                  // a pixel at the dot's center
            const uint32_t v = px[(size_t)c * s + c] & 0xFFFFFF, w = tray::kColor[st];
            for (int k = 0; k < 3; ++k) {
                const int d = (int)((v >> (16 - 8 * k)) & 255) - (int)((w >> (16 - 8 * k)) & 255);
                if (d < -24 || d > 24) center = false;
            }
            // rim: the pixel on the dot's left edge (row of the center) is dark and opaque
            const uint32_t r = px[(size_t)c * s + (s - D)];
            const int lum = (int)(((r >> 16) & 255) * 3 + ((r >> 8) & 255) * 6 + (r & 255)) / 10;
            if ((r >> 24) < 180 || lum > 110) rim = false;
            if (!dump.empty()) Dump(dump, s, st, px);
        }
    }
    Check(quadrant, "status dot only in the bottom-right quadrant (16/20/24/32)");
    Check(untouched, "other quadrants unchanged");
    Check(center, "dot center has the state color (4 states)");
    Check(rim, "dot has a dark outline");
    using namespace tray;
    Check(Pick(false, 99.f, 50000, 49000) == kStopped && Pick(true, 10.f, 50000, 0) == kOk &&
          Pick(true, 85.f, 50000, 0) == kHighLoad && Pick(true, 10.f, 50000, 41000) == kDropouts &&
          Pick(true, 85.f, 50000, 39999) == kHighLoad && Pick(true, 10.f, 50000, 40000) == kOk,
          "state rules (red held 10 s after the last dropout)");
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
