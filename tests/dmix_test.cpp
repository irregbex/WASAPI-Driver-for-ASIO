// WASAPI Driver for ASIO - offline test of the direct-monitoring mixer primitives (src/dmix.h)
// (Linux: g++ -O2 -std=c++17 tests/dmix_test.cpp -o dt && ./dt)
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
// Purpose
//   Checks, without Windows, the properties the audio thread relies on when it mixes an input into the output:
//   gain ramps without clicks, pan and balance laws, ASIO value conversions, the soft clipper and denormal-free
//   ramps. Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED"; the exit code is 0 only if all
//   checks pass. Single-threaded host program.
#include "../src/dmix.h"
#include <cstdio>
#include <cmath>
#include <cfloat>

static int g_fail = 0;
static void Check(bool ok, const char* what, double v) {
    printf("  %-58s %s  (%.6g)\n", what, ok ? "OK" : "FAILED", v);
    if (!ok) ++g_fail;
}

int main() {
    const double rate = 48000.0;
    const float k = dmix::RampCoef(rate);   // 5 ms
    // 1) Ramp 0 -> 1 on a full-scale DC signal: the largest sample-to-sample step must stay tiny (no click) and
    //    the ramp must settle (to exactly 1.0) within 100 ms.
    {
        dmix::Ramp r; float prev = 0.f, maxStep = 0.f; int settle = -1;
        for (int i = 0; i < 48000; ++i) {
            const float g = r.step(1.f, k);
            if (g - prev > maxStep) maxStep = g - prev;
            prev = g;
            if (settle < 0 && g == 1.f) settle = i;
        }
        Check(maxStep <= k + 1e-6f && maxStep < 0.005f, "ramp 0->1: largest step per sample <= 0.5 % of full scale", maxStep);
        Check(settle > 0 && settle < (int)(0.1 * rate), "ramp 0->1: exactly 1.0 within 100 ms (samples)", settle);
    }
    // 2) Ramp 1 -> 0 ends at exact zero (no denormal tail) and every intermediate value is normal or zero.
    {
        dmix::Ramp r; r.cur = 1.f; bool normal = true; int zero = -1;
        for (int i = 0; i < 48000; ++i) {
            const float g = r.step(0.f, k);
            if (g != 0.f && std::fabs(g) < FLT_MIN) normal = false;
            if (zero < 0 && g == 0.f) zero = i;
        }
        Check(normal, "ramp 1->0: no denormal values", 0);
        Check(zero > 0 && zero < (int)(0.1 * rate), "ramp 1->0: exactly 0 within 100 ms (samples)", zero);
        Check(dmix::FlushDenormal(1e-30f) == 0.f && dmix::FlushDenormal(0.5f) == 0.5f, "FlushDenormal", 0);
    }
    // 3) Constant-power pan: l^2 + r^2 = g^2 everywhere; center = -3 dB; ends = one side only.
    {
        double worst = 0;
        for (int i = 0; i <= 100; ++i) {
            float l, r; dmix::PanGains(0.8f, i / 100.f, l, r);
            worst = std::fmax(worst, std::fabs(l * l + r * r - 0.64));
        }
        Check(worst < 1e-5, "pan: l^2 + r^2 = gain^2 (worst error)", worst);
        float l, r; dmix::PanGains(1.f, 0.5f, l, r);
        Check(std::fabs(l - 0.70710678f) < 1e-5f && std::fabs(r - l) < 1e-6f, "pan: center = -3 dB on both sides", l);
        dmix::PanGains(1.f, 0.f, l, r);
        Check(l > 0.99999f && r < 1e-6f, "pan: hard left", r);
    }
    // 4) Balance: center keeps both sides at the gain, never boosts, extremes mute one side.
    {
        float l, r, mx = 0.f;
        dmix::BalanceGains(0.5f, 0.f, l, r);
        Check(std::fabs(l - 0.5f) < 1e-6f && std::fabs(r - 0.5f) < 1e-6f, "balance: center = unity on both sides", l);
        for (int i = -100; i <= 100; ++i) { dmix::BalanceGains(1.f, i / 100.f, l, r); mx = std::fmax(mx, std::fmax(l, r)); }
        Check(mx <= 1.f + 1e-6f, "balance: never above the gain", mx);
        dmix::BalanceGains(1.f, 1.f, l, r);
        Check(l < 1e-6f && r > 0.99999f, "balance: full right mutes the left side", l);
    }
    // 5) ASIO conversions.
    Check(std::fabs(dmix::AsioGainToLinear(0x20000000) - 1.f) < 1e-6f, "ASIO gain 0x20000000 = 0 dB", dmix::AsioGainToLinear(0x20000000));
    Check(std::fabs(20.0 * std::log10(dmix::AsioGainToLinear(0x7fffffff)) - 12.04) < 0.01, "ASIO gain 0x7fffffff = +12 dB", 20.0 * std::log10(dmix::AsioGainToLinear(0x7fffffff)));
    Check(dmix::AsioGainToLinear(-5) == 0.f && dmix::AsioPanTo01(-1) == 0.f && dmix::AsioPanTo01(0x7fffffff) > 0.99999f, "ASIO invalid values clamped", 0);
    // 6) Soft clipper: identity below 0.9, monotonic, |y| < 1 for any input, continuous at the knee.
    {
        bool ident = true, mono = true, bounded = true; float prev = -2.f;
        for (int i = -4000; i <= 4000; ++i) {
            const float x = i / 1000.f, y = dmix::SoftClip(x);
            if (std::fabs(x) <= 0.9f && y != x) ident = false;
            if (y < prev) mono = false;
            if (std::fabs(y) > 1.f) bounded = false;
            prev = y;
        }
        Check(ident, "soft clip: identity up to 0.9", 0);
        Check(mono, "soft clip: monotonic", 0);
        Check(bounded && dmix::SoftClip(1e9f) <= 1.f && dmix::SoftClip(-1e9f) >= -1.f, "soft clip: |output| <= 1", dmix::SoftClip(1e9f));
        Check(std::fabs(dmix::SoftClip(0.9001f) - 0.9001f) < 1e-4f, "soft clip: continuous at the knee", dmix::SoftClip(0.9001f));
    }
    // 7) Mixing a full-scale program with a full-scale monitor through the clipper never exceeds full scale.
    {
        float mx = 0.f;
        for (int i = 0; i < 4800; ++i) {
            const float prog = std::sin(i * 0.05f), mon = std::sin(i * 0.071f);
            mx = std::fmax(mx, std::fabs(dmix::SoftClip(prog + mon)));
        }
        Check(mx <= 1.f, "program + monitor after the clipper stays within full scale", mx);
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
