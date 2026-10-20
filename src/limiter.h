// WASAPI Driver for ASIO - hearing-safety output limiter (header-only, portable)
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
//   Optional peak limiter at the very end of the output chain (after gain, routing, monitoring; before the
//   integer conversion and dither). It adds no latency: there is no lookahead, so the gain must react within the
//   sample that would exceed the ceiling. Design:
//     - linked: one gain for all channels of a frame (keeps the stereo image), computed from the frame's largest
//       absolute sample;
//     - soft knee: below knee = ceiling * 0.708 (-3 dB under the ceiling) the signal passes unchanged; between the
//       knee and +infinity the output follows knee + (ceiling - knee) * tanh((x - knee) / (ceiling - knee)), which
//       approaches the ceiling but never reaches it;
//     - instant attack: the gain needed for the current frame is applied immediately (the output can never exceed
//       the ceiling), release is a one-pole recovery (default 150 ms) so the gain comes back without clicks;
//     - a final hard clamp at the ceiling guards against float rounding (it is only reached by tiny amounts).
//   Gain reduction (dB) is reported for the meters. Denormals cannot appear: the release runs on the deficit
//   (1 - gain), which snaps to 0 (gain exactly 1) below 1e-6.
//
// Threading
//   Plain struct used only by the audio thread (state) and pure functions; real-time safe.
//   driver.cpp keeps one Limiter for the main output (limMain) and one per additional output (limSink[]); they are
//   reset to a default-constructed state when streams are created. The ceiling comes from the LIVE parameters
//   (limCeil = 10^(LimiterCeilingDb / 20), config range -20..-0.1 dB), the release coefficient from ReleaseCoef at
//   the rate the limiter runs at. After each block the audio thread folds maxReductionDb into a shared atomic
//   (AtomicMax into limGr) and clears it; the meter snapshot code exchanges limGr with 0 and hands the peak to
//   each meter consumer (the panel shows it as the gain-reduction readout).
//
// Math of the gain
//   Let p = frame peak, k = 0.708 * ceiling, y = KneeCurve(p). The gain needed this frame is y / p, so the
//   deficit needed is need = 1 - y / p. The state deficit decays as d[n] = d[n-1] * (1 - rel) (exponential release
//   with time constant ms) and is raised to `need` whenever the frame demands more reduction (instant attack).
//   Because the same gain is applied to every channel and the peak channel maps exactly onto the knee curve, all
//   channels stay <= ceiling.
//
// Pitfalls
//   - Instant attack without lookahead distorts transients slightly (it is a safety limiter, not a mastering one).
//   - rel must be computed for the rate at which frame() is called; when the main output runs at the device rate
//     (rate conversion, 0.9.4) a separate coefficient (limRelMain) is used.
#pragma once
#include <cmath>

namespace lim {

// Release coefficient for a time constant of `ms` milliseconds at `rate` Hz.
//   n = time constant in samples; coef = 1 - exp(-1/n), so after n frames the deficit has decayed to 1/e (~37 %).
//   For a degenerate n <= 1 the coefficient is 1 (immediate recovery). Host thread, computed at stream setup.
inline float ReleaseCoef(double rate, double ms = 150.0) {
    const double n = rate * ms / 1000.0;
    return n > 1.0 ? (float)(1.0 - std::exp(-1.0 / n)) : 1.f;
}

// Output level for an input peak `x` (>= 0) with the soft-knee curve described above.
//   Continuous and with slope 1 at the knee (tanh'(0) = 1), so there is no audible corner; asymptote = ceiling.
//   Linear units (1.0 = 0 dBFS).
inline float KneeCurve(float x, float ceiling) {
    const float knee = ceiling * 0.708f;
    if (x <= knee) return x;
    return knee + (ceiling - knee) * std::tanh((x - knee) / (ceiling - knee));
}

struct Limiter {
    float gain = 1.f;               // current gain (A); always 1 - deficit
    float deficit = 0.f;            // 1 - gain. The release works on this value: it decays geometrically with full
                                    // float precision near 0, whereas gain += (1 - gain) * rel stalls a few 1e-4
                                    // below 1.0 (the increment falls under half an ulp of 1.0f). Found by the test.
    float maxReductionDb = 0.f;     // largest reduction since the caller last cleared it (A writes, the caller publishes it)

    // Processes one frame of n samples in place. ceiling: linear (e.g. 0.891 for -1 dBFS); rel: ReleaseCoef.
    // x: the frame's samples after gain (linked across all n channels). Called per frame from the audio thread;
    // O(n), no allocation. maxReductionDb tracks the largest reduction in dB = -20 * log10(gain).
    void frame(float* x, int n, float ceiling, float rel) {
        float peak = 0.f;
        for (int c = 0; c < n; ++c) { const float a = std::fabs(x[c]); if (a > peak) peak = a; }
        float d = deficit * (1.f - rel);                          // release towards gain 1
        if (d < 1e-6f) d = 0.f;                                   // snap: no denormals, exact 1.0 at rest
        if (peak > ceiling * 0.708f) {                            // instant attack: the gain this frame needs
            const float need = 1.f - KneeCurve(peak, ceiling) / peak;
            if (need > d) d = need;
        }
        deficit = d;
        const float g = 1.f - d;
        gain = g;
        for (int c = 0; c < n; ++c) {
            float v = x[c] * g;
            if (v > ceiling) v = ceiling; else if (v < -ceiling) v = -ceiling;   // rounding guard
            x[c] = v;
        }
        if (d > 0.f) { const float db = -20.f * std::log10(g > 1e-6f ? g : 1e-6f); if (db > maxReductionDb) maxReductionDb = db; }
    }
};

}  // namespace lim
