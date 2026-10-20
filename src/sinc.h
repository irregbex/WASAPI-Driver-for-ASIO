// WASAPI Driver for ASIO - polyphase windowed-sinc resampler (Kaiser window) for drift correction
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
// Design
//   Table: (kSincPhases + 1) phases x (2 * kSincHalf) coefficients. Phase ph (0..kSincPhases) corresponds to the
//   fraction t = ph / kSincPhases. For an output at position p, with k = floor(p) and t = p - k, the input samples
//   x[k - kSincHalf + 1 .. k + kSincHalf] are used:
//     coef[m] = h(t + kSincHalf - 1 - m),  m = 0 .. 2*kSincHalf - 1,  h(d) = sinc(d) * kaiser(d / kSincHalf)
//   with linear interpolation between the two neighbouring phases (the extra phase kSincPhases makes this valid
//   for t close to 1). The cut-off is at Nyquist: the ratio is always ~1 (no decimation), so the response is flat to
//   ~20 kHz at 48 kHz and -6 dB at Nyquist. Every phase is normalized to unity DC gain. Offline measurements are in
//   ARCHITECTURE.txt section 19 (tests/resampler_test.cpp).
//
// Threading
//   BuildSincTable allocates and runs on the host thread when streams are opened. The audio thread only reads the
//   table through SincCoefs (no allocation, no locks).
//
// Math, step by step
//   Ideal band-limited interpolation reconstructs x(p) = sum_n x[n] * sinc(p - n), with sinc(d) = sin(pi d)/(pi d).
//   The sum is infinite, so it is truncated to the 2 * kSincHalf = 32 nearest samples. A plain truncation (a
//   rectangular window) causes ripple and poor stop-band rejection (Gibbs), so each tap is multiplied by a Kaiser
//   window w(r) = I0(beta * sqrt(1 - r^2)) / I0(beta), r = d / kSincHalf in [-1, 1]. beta trades main-lobe width
//   (transition band) against side-lobe level; beta = 8 gives roughly 80 dB attenuation.
//   Evaluating sin() and I0() per output sample would be far too slow for the audio thread, so h(d) is sampled at
//   kSincPhases = 256 fractional offsets (polyphase table) and the 32 coefficients for an arbitrary fraction t are
//   obtained by linear interpolation between phase floor(t * 256) and the next one.
//   DC normalization: after windowing and truncation the taps of a phase do not sum exactly to 1; dividing by the
//   sum makes a constant input come out unchanged for every fraction (no "zipper" amplitude modulation when the
//   fraction moves slowly, which is exactly what drift correction does).
//
// Indexing convention (shared with ResampleFifo in driver.cpp)
//   The read position p is kept in [kSincHalf - 1, kSincHalf) after each block (posBase = kSincHalf - 1), so the
//   leftmost tap k - (kSincHalf - 1) never goes below FIFO index 0. The look-ahead is kSincHalf frames, which the
//   driver adds to the FIFO target / sink set point (it is part of the reported latency in sinc mode).
//
// Pitfalls
//   - SincCoefs expects t in [0, 1); values outside are clamped to the first/last phase interval, not wrapped.
//   - The table has kSincPhases + 1 rows; the last row (t = 1) only serves as the right neighbour for
//     interpolation and is never selected as ph.
//   - The cut-off is at Nyquist of the input rate. This is fine only because the ratio stays within +-0.3 %
//     (drift.h clamps it); it is NOT an anti-aliasing filter for real rate conversion (see fixedratio.h).
#pragma once
#include <vector>
#include <cmath>
#include <cstddef>

constexpr int    kSincHalf   = 16;     // taps per side
constexpr int    kSincTaps   = 2 * kSincHalf;   // 32 taps per output sample (per channel: 32 multiply-adds)
constexpr int    kSincPhases = 256;              // fractional resolution of the table (1/256 sample)
constexpr double kSincBeta   = 8.0;    // Kaiser beta (~80 dB side-lobe attenuation)

// Modified Bessel function of the first kind, order 0 (power series), used by the Kaiser window.
//   I0(x) = sum_k ((x/2)^(2k) / (k!)^2); each term is the previous one times (x^2/4) / k^2. The series converges
//   fast for the arguments used here (x <= beta = 8); it stops once a term is below 1e-12 of the sum.
static inline double SincBesselI0(double x) {
    double sum = 1.0, term = 1.0, y = x * x / 4.0;
    for (int k = 1; k < 50; ++k) { term *= y / ((double)k * k); sum += term; if (term < 1e-12 * sum) break; }
    return sum;
}

// Builds the coefficient table into `tab` ((kSincPhases + 1) * kSincTaps floats). Host thread (allocates).
//   Layout: row-major, row ph holds the 32 taps for fraction ph / kSincPhases; tap m multiplies input sample
//   x[k - kSincHalf + 1 + m]. Computed in double, stored as float (~33 KB). The driver builds it once
//   (when sincTab is still empty) and keeps it for the life of the driver instance.
static inline void BuildSincTable(std::vector<float>& tab) {
    tab.assign((size_t)(kSincPhases + 1) * kSincTaps, 0.f);
    const double pi = 3.14159265358979323846;
    const double i0b = SincBesselI0(kSincBeta);
    for (int ph = 0; ph <= kSincPhases; ++ph) {
        const double t = (double)ph / kSincPhases;
        double c[kSincTaps], sum = 0.0;
        for (int m = 0; m < kSincTaps; ++m) {
            const double d = t + kSincHalf - 1 - m;            // distance in samples to the evaluated point
            const double sinc = fabs(d) < 1e-12 ? 1.0 : sin(pi * d) / (pi * d);
            const double r = d / kSincHalf;                      // window argument in [-1, 1]
            const double w = fabs(r) >= 1.0 ? 0.0 : SincBesselI0(kSincBeta * sqrt(1.0 - r * r)) / i0b;
            c[m] = sinc * w; sum += c[m];
        }
        for (int m = 0; m < kSincTaps; ++m) tab[(size_t)ph * kSincTaps + m] = (float)(c[m] / sum);
    }
}

// Interpolated coefficients for the fraction t in [0, 1) written to out[kSincTaps]. Real-time safe.
//   f = t * 256 selects rows ph = floor(f) and ph + 1; a = f - ph is the blend weight (out = (1-a)*c0 + a*c1).
//   Called once per output frame; the same 32 coefficients are then reused for every channel of that frame.
static inline void SincCoefs(const float* tab, double t, float* out) {
    double f = t * kSincPhases;
    int ph = (int)f;
    if (ph >= kSincPhases) ph = kSincPhases - 1;
    if (ph < 0) ph = 0;
    const float a = (float)(f - ph), b = 1.f - a;
    const float* c0 = tab + (size_t)ph * kSincTaps;
    const float* c1 = c0 + kSincTaps;
    for (int m = 0; m < kSincTaps; ++m) out[m] = b * c0[m] + a * c1[m];
}
