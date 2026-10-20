// WASAPI Driver for ASIO - offline test of the panel background renderer (src/visuals.h)
// (Linux: g++ -O2 -std=c++17 -D_GLIBCXX_ASSERTIONS tests/visuals_test.cpp -o vt && ./vt)
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
// Purpose (0.8.1; 0.9.0: four more effects - metaballs, fire, vector, twister - and theme palettes)
//   For every effect and several sizes (1x1, odd sizes, the 320x240 maximum and an oversize request that must
//   be clamped): renders 300 frames with a fixed time step and checks
//     1. bounds: every pixel write goes through std::vector::operator[]; with -D_GLIBCXX_ASSERTIONS (as in CI)
//        an out-of-range index aborts the test; the buffer size always equals width x height;
//     2. determinism: a second, fresh renderer given the same inputs produces the same FNV-1a hash;
//     3. intensity 0 gives the plain background color everywhere (the effect can be switched to invisible);
//     4. time per frame at 320 x 240 (the largest buffer the panel uses), reported; the check fails above
//        6 ms (the panel's own limit before it drops from 60 to 30 fps), which leaves a wide margin;
//     5. time-based animation (0.8.2): one second rendered at 100 fps and at 50 fps ends on the same frame
//        (effects that depend only on the time) or with the same star travel (starfield, reactive);
//     6. speed: 0.5 s at 200 % equals 1 s at 100 %, and 2 s at 50 % too.
//   With --dump <folder> it also writes one 640x360 PPM preview per effect (160x90 frame scaled by 4).
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/visuals.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
static void Check(bool ok, const char* what, const char* detail) {
    printf("  %-58s %s  (%s)\n", what, ok ? "OK" : "FAILED", detail);
    if (!ok) ++g_fail;
}
static uint64_t Hash(const vis::Renderer& r) {
    uint64_t h = 1469598103934665603ull;
    const uint32_t* p = r.pixels();
    for (int i = 0, n = r.width() * r.height(); i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static const char* kNames[vis::kEffectCount] = {"off", "plasma", "starfield", "copper", "tunnel", "reactive", "scroller",
                                                "metaballs", "fire", "vector", "twister"};
static const char* kText = "WASAPI Driver for ASIO 0.8.1 - free software under GPLv3 - greetings to everyone who makes music";

static uint64_t Run(vis::Effect e, int w, int h, int frames, int intensity) {
    vis::Renderer r;
    r.setText(kText);
    r.resize(w, h);
    for (int f = 0; f < frames; ++f) r.frame(e, (uint64_t)f * 33333u, intensity, (f * 7) & 255);
    return Hash(r);
}

int main(int argc, char** argv) {
    printf("Visuals renderer offline test\n");
    const int sizes[][2] = {{1, 1}, {7, 3}, {3, 7}, {161, 91}, {319, 239}, {320, 240}, {2000, 2000}};
    char d[160];
    for (int e = vis::kPlasma; e < vis::kEffectCount; ++e) {
        bool ok = true;
        for (const auto& s : sizes) {
            vis::Renderer r; r.setText(kText); r.resize(s[0], s[1]);
            for (int f = 0; f < 300; ++f) r.frame((vis::Effect)e, (uint64_t)f * 33333u, 100, (f * 7) & 255);
            const int ew = s[0] > vis::kMaxW ? vis::kMaxW : s[0], eh = s[1] > vis::kMaxH ? vis::kMaxH : s[1];
            if (r.width() != ew || r.height() != eh || !r.pixels()) ok = false;
        }
        snprintf(d, sizeof d, "%d sizes x 300 frames", (int)(sizeof(sizes) / sizeof(sizes[0])));
        std::string w = std::string(kNames[e]) + ": no out-of-bounds write, sizes clamped";
        Check(ok, w.c_str(), d);
        const uint64_t a = Run((vis::Effect)e, 320, 240, 300, 35), b = Run((vis::Effect)e, 320, 240, 300, 35);
        snprintf(d, sizeof d, "%016llx", (unsigned long long)a);
        w = std::string(kNames[e]) + ": deterministic hash";
        Check(a == b, w.c_str(), d);
        {
            vis::Renderer r; r.resize(64, 48);
            for (int f = 0; f < 50; ++f) r.frame((vis::Effect)e, (uint64_t)f * 33333u, 0, 255);
            bool plain = true;
            for (int i = 0; i < 64 * 48; ++i) if (r.pixels()[i] != 0x1E1F22u) plain = false;
            w = std::string(kNames[e]) + ": intensity 0 = plain background";
            Check(plain, w.c_str(), "64x48");
        }
        {
            vis::Renderer r; r.setText(kText); r.resize(320, 240);
            for (int f = 0; f < 20; ++f) r.frame((vis::Effect)e, (uint64_t)f * 33333u, 35, 128);   // warm-up
            const int n = 600;
            const auto t0 = std::chrono::steady_clock::now();
            for (int f = 0; f < n; ++f) r.frame((vis::Effect)e, (uint64_t)f * 33333u, 35, (f * 5) & 255);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
            snprintf(d, sizeof d, "%.3f ms per frame", ms);
            w = std::string(kNames[e]) + ": 320x240 frame time < 6 ms";
            Check(ms < 6.0, w.c_str(), d);
        }
    }
    // 5) and 6): frame-rate independence and speed scaling.
    for (int e = vis::kPlasma; e < vis::kEffectCount; ++e) {
        auto run = [&](uint64_t stepUs, int frames, int speed, uint64_t* travel) {
            vis::Renderer r; r.setText(kText); r.resize(160, 90);
            for (int f = 0; f <= frames; ++f) r.frame((vis::Effect)e, (uint64_t)f * stepUs, 35, 100, speed);
            *travel = r.starTravel();
            return Hash(r);
        };
        uint64_t ta = 0, tb = 0, tc = 0, td = 0;
        const uint64_t ha = run(10000, 100, 100, &ta), hb = run(20000, 50, 100, &tb);   // 1 s at 100 / 50 fps
        const uint64_t hc = run(5000, 100, 200, &tc);                                   // 0.5 s at 200 %
        const uint64_t hd = run(20000, 100, 50, &td);                                   // 2 s at 50 %
        const bool starsOnly = e == vis::kStarfield || e == vis::kReactive || e == vis::kScroller;
        // Scroller: the text depends only on the time, the stars behind it on the step pattern: compare travel.
        bool same = starsOnly ? (ta == tb && ta > 0) : ha == hb;
        if (starsOnly) snprintf(d, sizeof d, "star travel %llu / %llu", (unsigned long long)ta, (unsigned long long)tb);
        else snprintf(d, sizeof d, "hash %016llx", (unsigned long long)ha);
        std::string w = std::string(kNames[e]) + ": 1 s at 100 fps == 1 s at 50 fps";
        Check(same, w.c_str(), d);
        same = starsOnly ? (tc == ta && td == ta) : (hc == ha && hd == ha);
        w = std::string(kNames[e]) + ": speed 200 % / 50 % scale the time";
        Check(same, w.c_str(), starsOnly ? "star travel" : "hash");
    }
    // 0.9.0: a theme palette changes the colors but never the bounds; intensity 0 stays the plain background.
    {
        bool ok = true;
        for (int e = vis::kPlasma; e < vis::kEffectCount; ++e) {
            vis::Renderer r; r.setText(kText); r.setBackground(0x0B3A7E); r.setPalette(0x1040A0, 0xD07020, 0xF0F0F0); r.resize(97, 61);
            for (int f = 0; f < 60; ++f) r.frame((vis::Effect)e, (uint64_t)f * 16667u, 0, 200);
            for (int i = 0; i < 97 * 61; ++i) if (r.pixels()[i] != 0x0B3A7Eu) ok = false;
        }
        Check(ok, "theme palette + background: intensity 0 = theme background", "97x61, all effects");
    }
    if (argc == 3 && !strcmp(argv[1], "--dump")) {
        for (int e = vis::kPlasma; e < vis::kEffectCount; ++e)
            for (int inten : {35, 100}) {
                vis::Renderer r; r.setText(kText); r.resize(160, 90);
                for (int f = 0; f < 90; ++f) r.frame((vis::Effect)e, (uint64_t)f * 33333u, inten, 160);
                std::string fn = std::string(argv[2]) + "/" + kNames[e] + (inten == 100 ? "-intensity100" : "") + ".ppm";
                FILE* fp = fopen(fn.c_str(), "wb");
                if (!fp) { printf("cannot write %s\n", fn.c_str()); return 1; }
                fprintf(fp, "P6\n640 360\n255\n");
                for (int y = 0; y < 360; ++y)
                    for (int x = 0; x < 640; ++x) {
                        const uint32_t c = r.pixels()[(y / 4) * 160 + x / 4];
                        const unsigned char rgb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
                        fwrite(rgb, 1, 3, fp);
                    }
                fclose(fp);
            }
        printf("  previews written\n");
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
