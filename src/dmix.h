// WASAPI Driver for ASIO - direct-monitoring mixer primitives (header-only, portable)
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
//   Small, allocation-free helpers used by the audio thread to mix an input directly into the output (0.7.0):
//   per-sample gain ramps (no clicks when the level, pan or on/off state changes), the pan / balance laws, the
//   ASIO kAsioSetInputMonitor value conversions, a soft clipper applied before the output conversion and dither,
//   and denormal flushing. Kept in a header without Windows dependencies so that tests/dmix_test.cpp can verify
//   them on Linux.
//
// Threading
//   Pure functions and plain structs. Real-time safe: no locks, no allocation, no system calls.
//   Exceptions by caller, not by cost: RampCoef, PanGains / BalanceGains and the Asio* conversions are also called
//   on the host thread (stream setup, applying LIVE settings, kAsioSetInputMonitor); the resulting gains reach the
//   audio thread through atomics or the LIVE publication (live.h). Ramp state lives on the audio thread only.
//
// Where each piece is used (driver.cpp)
//   - Host-requested monitoring (kAsioSetInputMonitor): gain / pan converted with AsioGainToLinear / AsioPanTo01,
//     split with PanGains, smoothed per input channel with Ramp (hostRampL / hostRampR).
//   - Panel direct monitoring: PanGains for a mono source (pan -1..+1 mapped to 0..1), BalanceGains for stereo,
//     smoothed with Ramp (dmRamp).
//   - Output path: FlushDenormal on the monitor mix, then SoftClip on program + monitor (only while a monitor
//     signal is present), before the limiter (limiter.h), conversion and dither.
//
// Pitfalls
//   - RampCoef must match the rate of the loop that calls step(): the driver uses the output device rate.
//   - SoftClip shapes everything above 0.9 (about -0.9 dBFS), the program included, whenever the monitor path is
//     being mixed (even if the monitored input is silent); with monitoring off it is not applied at all.
#pragma once
#include <cmath>

namespace dmix {

// Values this small are flushed to exact zero: they are inaudible and, as denormals, can be very slow on x86.
inline float FlushDenormal(float x) { return (x > -1e-15f && x < 1e-15f) ? 0.f : x; }

// One-pole smoothing coefficient for a time constant of `ms` milliseconds at `rate` Hz (5 ms by default: fast
// enough to follow a fader, slow enough that a step from 0 to 1 never produces an audible click).
// n = time constant in samples, k = 1 - exp(-1/n): after n samples ~63 % of a step is done, after 5n ~99 %.
inline float RampCoef(double rate, double ms = 5.0) {
    const double n = rate * ms / 1000.0;
    return n > 1.0 ? (float)(1.0 - std::exp(-1.0 / n)) : 1.f;
}

// Gain that moves towards its target by a fraction k of the remaining distance at every sample. It snaps to the
// target once closer than 1e-5 (-100 dB, inaudible) or when float precision stops the approach (near 1.0 the
// remaining step can become smaller than half an ulp), so a ramp always ends exactly at its target: a ramp to 0
// really ends at 0 and never leaves a denormal tail.
// Recurrence: cur[n] = cur[n-1] + (target - cur[n-1]) * k (exponential approach, k from RampCoef).
struct Ramp {
    float cur = 0.f;                 // current gain; starts at 0 so a newly enabled monitor fades in
    // Advances one sample and returns the gain to apply to it. Audio thread.
    float step(float target, float k) {
        const float next = cur + (target - cur) * k;
        cur = (next == cur || std::fabs(target - next) < 1e-5f) ? target : next;
        return cur;
    }
};

// Constant-power pan of a mono source: pan01 = 0 (left) .. 0.5 (center) .. 1 (right). l^2 + r^2 = gain^2, so the
// perceived loudness does not change while panning; at the center both sides get gain / sqrt(2) (-3 dB).
// Math: angle a = pan01 * pi/2; l = gain * cos(a), r = gain * sin(a). Out-of-range pan01 is clamped.
inline void PanGains(float gain, float pan01, float& l, float& r) {
    if (pan01 < 0.f) pan01 = 0.f;
    if (pan01 > 1.f) pan01 = 1.f;
    const float a = pan01 * 1.5707963f;
    l = gain * std::cos(a);
    r = gain * std::sin(a);
}

// Balance of a stereo pair: bal = -1 (left only) .. 0 (both at `gain`) .. +1 (right only). The side being
// favored stays at `gain` (never boosted, so balancing can never clip more than the source); the other side
// follows a constant-power curve towards 0.
// Math: a = (bal + 1) * pi/4; l = gain * min(1, sqrt(2) cos(a)), r = gain * min(1, sqrt(2) sin(a)). At the center
// sqrt(2) cos(pi/4) = 1, so both sides are exactly `gain` (unlike PanGains, a centered stereo pair is not cut).
inline void BalanceGains(float gain, float bal, float& l, float& r) {
    if (bal < -1.f) bal = -1.f;
    if (bal > 1.f) bal = 1.f;
    const float a = (bal + 1.f) * 0.78539816f;           // 0 .. pi/2, pi/4 at the center
    l = gain * std::fmin(1.f, std::cos(a) * 1.41421356f);
    r = gain * std::fmin(1.f, std::sin(a) * 1.41421356f);
}

// ASIOInputMonitor.gain: 0x20000000 = 0 dB (unity), 0x7fffffff = +12 dB, 0 = -inf. Negative values are invalid
// and treated as silence.
inline float AsioGainToLinear(long g) { return g <= 0 ? 0.f : (float)((double)g / (double)0x20000000); }
// ASIOInputMonitor.pan: 0 = left, 0x7fffffff = right (0x3fffffff ~ center) -> 0..1.
inline float AsioPanTo01(long p) { return p <= 0 ? 0.f : (float)((double)p / 2147483647.0); }

// Soft clipper: identity up to |x| = 0.9 (-0.9 dBFS), then a tanh knee that approaches 1.0 asymptotically with a
// continuous slope. Applied to the output while direct monitoring is active, before the integer conversion and the
// dither, so that program + monitor never wrap or hard-clip harshly.
// Same shape as the limiter's knee (limiter.h KneeCurve) with a fixed knee 0.9 and ceiling 1.0, but per sample
// and without memory, so it is a static waveshaper (adds some harmonics above the knee), not a gain control.
inline float SoftClip(float x) {
    const float t = 0.9f;
    const float a = std::fabs(x);
    if (a <= t) return x;
    const float y = t + (1.f - t) * std::tanh((a - t) / (1.f - t));
    return x < 0.f ? -y : y;
}

}  // namespace dmix
