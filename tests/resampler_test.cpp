// WASAPI Driver for ASIO - offline resampler test (Linux: g++ -O2 -std=c++17 tests/resampler_test.cpp -o rt && ./rt)
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
//   Compares 4-point Hermite and the polyphase sinc resampler (src/sinc.h) at a fixed ratio: the error against the
//   ideal signal in dB relative to the signal (equivalent to the THD+N of the resampling). Uses the same arithmetic
//   as WasapiBridge::readSource (float samples, double position). Also checks the sinc read bounds used by
//   readSource. Prints "OK" or "FAILED" for the bounds check and a table of errors per test frequency.
//   Single-threaded host program; no Windows dependencies.
#include "../src/sinc.h"
#include <cstdio>
#include <vector>
#include <cmath>

// 4-point, 3rd-order Hermite interpolation between x0 and x1 (t in [0, 1)), identical to the driver's.
static float Hermite(float xm1, float x0, float x1, float x2, float t) {
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// Resamples a sine of freq Hz at the given ratio with Hermite (sinc = false) or sinc and returns the RMS error
// against the analytically resampled sine, in dB relative to the signal RMS (ignoring the edges).
static double Run(bool sinc, double freq, double ratio, const std::vector<float>& tab) {
    const double fs = 48000.0, pi = 3.14159265358979323846;
    const int n = 48000 * 4;
    std::vector<float> x(n + 64);
    for (size_t i = 0; i < x.size(); ++i) x[i] = (float)(0.5 * sin(2 * pi * freq * i / fs));
    double p = sinc ? kSincHalf - 1 : 1.0;
    double err2 = 0, sig2 = 0; float c[kSincTaps];
    for (int i = 0; i < n - 2000; ++i) {
        size_t k = (size_t)p; double t = p - (double)k;
        float y;
        if (sinc) {
            SincCoefs(tab.data(), t, c);
            float acc = 0.f; const float* src = &x[k - kSincHalf + 1];
            for (int m = 0; m < kSincTaps; ++m) acc += c[m] * src[m];
            y = acc;
        } else y = Hermite(x[k - 1], x[k], x[k + 1], x[k + 2], (float)t);
        double ideal = 0.5 * sin(2 * pi * freq * p / fs);
        if (i > 1000) { err2 += (y - ideal) * (y - ideal); sig2 += ideal * ideal; }
        p += ratio;
    }
    return 10 * log10(err2 / sig2);
}

// readSource read bounds with sinc: with need = pos + buf*ratio + kSincHalf + 1 the highest index read
// must be < available frames and the lowest >= 0, for the extreme ratios 0.997 and 1.003.
static bool BoundsCheck() {
    const long buf = 64;
    for (double ratio : {0.997, 1.0, 1.003}) {
        double pos = kSincHalf - 1;
        size_t fill = 0; long maxSeen = 0;
        for (int blk = 0; blk < 200000; ++blk) {
            fill += 64 + (blk % 7);                        // irregular arrival
            double need = pos + buf * ratio + kSincHalf + 1.0;
            if ((double)fill < need) continue;
            double p = pos;
            for (long i = 0; i < buf; ++i) {
                size_t k = (size_t)p;
                long lo = (long)k - (kSincHalf - 1), hi = (long)k + kSincHalf;
                if (lo < 0 || hi >= (long)fill) { printf("OUT OF RANGE ratio %.3f block %d\n", ratio, blk); return false; }
                if (hi > maxSeen) maxSeen = hi;
                p += ratio;
            }
            size_t consumed = (size_t)p - (size_t)(kSincHalf - 1);
            fill -= consumed; pos = p - (double)consumed;
            if (fill > 4096) fill = 2048;                   // drop as in pullSource
        }
    }
    return true;
}

int main() {
    printf("Sinc read bounds: %s\n", BoundsCheck() ? "OK" : "FAILED");
    std::vector<float> tab; BuildSincTable(tab);
    const double freqs[] = {1000, 5000, 10000, 15000, 18000, 20000};
    printf("Error vs ideal (dB relative to the signal), ratio 1.0002, 48 kHz\n");
    printf("  Frequency    Hermite    Sinc 16x256 Kaiser 8\n");
    for (double f : freqs) printf("  %6.0f Hz   %7.1f    %7.1f\n", f, Run(false, f, 1.0002, tab), Run(true, f, 1.0002, tab));
    return 0;
}
