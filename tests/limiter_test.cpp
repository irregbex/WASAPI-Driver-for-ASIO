// WASAPI Driver for ASIO - offline test of the hearing-safety limiter (src/limiter.h)
// (Linux: g++ -O2 -std=c++17 tests/limiter_test.cpp -o lt && ./lt)
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
// Purpose (0.8.0, G2)
//   Checks, without Windows, the properties promised for the limiter at the end of the output chain:
//     1. no overshoot: whatever the input (sine bursts up to +24 dBFS, full-scale square waves, single-sample
//        spikes, random noise), no output sample exceeds the ceiling;
//     2. transparency: material peaking below the knee (-3 dB under the ceiling) passes bit-exact;
//     3. linked channels: one gain per frame (the L/R ratio of every frame is preserved);
//     4. no denormals: the gain and every output sample are either 0 or normal floats during a long fade to silence;
//     5. no clicks: after a loud burst the gain recovers smoothly (the largest gain step per sample during release
//        stays tiny) and returns to exactly 1.0;
//     6. gain reduction is reported in dB and is positive while limiting;
//     7. every ceiling of the allowed range (-0.1 .. -20 dBFS) holds.
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED"; the exit code is 0 only if all pass.
#include "../src/limiter.h"
#include <cstdio>
#include <cmath>
#include <cfloat>
#include <cstdint>

static int g_fail = 0;
static void Check(bool ok, const char* what, double v) {
    printf("  %-62s %s  (%.6g)\n", what, ok ? "OK" : "FAILED", v);
    if (!ok) ++g_fail;
}
static bool Denormal(float x) { return x != 0.f && std::fabs(x) < FLT_MIN; }

int main() {
    const double rate = 48000.0, pi = 3.14159265358979323846;
    const float rel = lim::ReleaseCoef(rate);
    const float ceil1 = (float)std::pow(10.0, -1.0 / 20.0);   // -1 dBFS (default)
    printf("Limiter offline test (rate %.0f Hz, release coefficient %.6g)\n", rate, rel);

    // 1) Overshoot: sine bursts of increasing level on two channels, with silence in between.
    {
        lim::Limiter l; float peak = 0.f;
        for (int burst = 0; burst < 8; ++burst) {
            const float amp = (float)std::pow(10.0, (burst * 4.0 - 6.0) / 20.0);   // -6 .. +22 dBFS
            for (int i = 0; i < 24000; ++i) {
                float f[2] = {amp * (float)std::sin(2 * pi * 997.0 * i / rate), -0.7f * amp * (float)std::sin(2 * pi * 997.0 * i / rate)};
                if (i >= 12000) f[0] = f[1] = 0.f;
                l.frame(f, 2, ceil1, rel);
                for (float v : f) if (std::fabs(v) > peak) peak = std::fabs(v);
            }
        }
        Check(peak <= ceil1, "sine bursts -6..+22 dBFS: peak <= ceiling (-1 dBFS)", peak);
    }
    // 1b) Square wave at +24 dBFS and single-sample spikes (worst case for a limiter without lookahead).
    {
        lim::Limiter l; float peak = 0.f;
        for (int i = 0; i < 96000; ++i) {
            float f[2];
            const float sq = ((i / 24) & 1) ? 15.8f : -15.8f;
            const float spike = (i % 4801 == 0) ? 20.f : 0.01f;
            f[0] = i < 48000 ? sq : spike; f[1] = i < 48000 ? -sq : -spike;
            l.frame(f, 2, ceil1, rel);
            for (float v : f) if (std::fabs(v) > peak) peak = std::fabs(v);
        }
        Check(peak <= ceil1, "square +24 dBFS and single-sample spikes: no overshoot", peak);
    }
    // 1c) Pseudo-random noise with large peaks (LCG, reproducible), 8 channels.
    {
        lim::Limiter l; float peak = 0.f; uint32_t s = 12345u;
        for (int i = 0; i < 48000; ++i) {
            float f[8];
            for (float& v : f) { s = s * 1664525u + 1013904223u; v = ((int32_t)s / 2147483648.f) * 6.f; }
            l.frame(f, 8, ceil1, rel);
            for (float v : f) if (std::fabs(v) > peak) peak = std::fabs(v);
        }
        Check(peak <= ceil1, "noise peaks up to +15 dBFS on 8 channels: no overshoot", peak);
    }
    // 2) Transparency below the knee (-3 dB under the ceiling): bit-exact.
    {
        lim::Limiter l; double maxErr = 0.0;
        const float amp = ceil1 * 0.70f;
        for (int i = 0; i < 48000; ++i) {
            const float x = amp * (float)std::sin(2 * pi * 440.0 * i / rate);
            float f[2] = {x, x * 0.5f};
            l.frame(f, 2, ceil1, rel);
            maxErr = std::fmax(maxErr, std::fabs(f[0] - x));
            maxErr = std::fmax(maxErr, std::fabs(f[1] - x * 0.5f));
        }
        Check(maxErr == 0.0 && l.gain == 1.f, "below the knee: output identical to input", maxErr);
    }
    // 3) Linked channels: the ratio between channels is preserved frame by frame while limiting.
    {
        lim::Limiter l; double maxDev = 0.0;
        for (int i = 1; i < 48000; ++i) {
            const float x = 4.f * (float)std::sin(2 * pi * 200.0 * i / rate);
            float f[2] = {x, 0.25f * x};
            l.frame(f, 2, ceil1, rel);
            if (std::fabs(f[0]) > 1e-3f) maxDev = std::fmax(maxDev, std::fabs(f[1] / f[0] - 0.25));
        }
        Check(maxDev < 1e-5, "linked: channel ratio preserved while limiting", maxDev);
    }
    // 4) Denormals: a loud signal followed by an exponential fade far below FLT_MIN, then silence.
    {
        lim::Limiter l; int den = 0;
        float x = 8.f;
        for (int i = 0; i < 480000; ++i) {
            float f[2] = {x, -x};
            l.frame(f, 2, ceil1, rel);
            if (Denormal(f[0]) || Denormal(f[1]) || Denormal(l.gain)) ++den;
            x *= 0.9995f;
            if (x < 1e-30f) x = 0.f;     // the driver flushes its own signal the same way (FlushDenormal)
        }
        Check(den == 0, "no denormal output or gain during a 10 s fade", den);
        Check(l.gain == 1.f, "gain back to exactly 1.0 after the fade", l.gain);
    }
    // 5) No clicks: largest gain change per sample during the release after a +20 dBFS burst, and recovery.
    {
        lim::Limiter l; float maxRelStep = 0.f; int back = -1;
        for (int i = 0; i < 4800; ++i) { float f[1] = {10.f}; l.frame(f, 1, ceil1, rel); }
        float prev = l.gain;
        for (int i = 0; i < 48000 * 3; ++i) {
            float f[1] = {0.1f};
            l.frame(f, 1, ceil1, rel);
            if (l.gain - prev > maxRelStep) maxRelStep = l.gain - prev;
            prev = l.gain;
            if (back < 0 && l.gain == 1.f) back = i;
        }
        Check(maxRelStep < 0.001f, "release: gain step per sample < 0.001 (no click)", maxRelStep);
        Check(back > 0 && back < 48000 * 3, "release: gain back to 1.0 within 3 s (samples)", back);
    }
    // 6) Reported gain reduction.
    {
        lim::Limiter l;
        for (int i = 0; i < 480; ++i) { float f[2] = {2.f, 2.f}; l.frame(f, 2, ceil1, rel); }
        const double expect = 20.0 * std::log10(2.0 / ceil1);    // at least the level above the ceiling
        Check(l.maxReductionDb >= expect - 0.01 && l.maxReductionDb < expect + 3.0, "gain reduction reported for +6 dBFS (dB)", l.maxReductionDb);
    }
    // 7) Every ceiling of the range holds.
    {
        bool ok = true; double worst = 0.0;
        for (double db = -0.1; db >= -20.0; db -= 0.7) {
            const float c = (float)std::pow(10.0, db / 20.0);
            lim::Limiter l;
            for (int i = 0; i < 9600; ++i) {
                float f[2] = {3.f * (float)std::sin(2 * pi * 1500.0 * i / rate), 3.f * (float)std::cos(2 * pi * 1500.0 * i / rate)};
                l.frame(f, 2, c, rel);
                for (float v : f) { if (std::fabs(v) > c) { ok = false; worst = std::fmax(worst, std::fabs(v) - c); } }
            }
        }
        Check(ok, "ceilings -0.1 .. -20 dBFS: no overshoot", worst);
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
