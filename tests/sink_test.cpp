// WASAPI Driver for ASIO - offline test of the PI loop of an extra output (sink)
// (Linux: g++ -O2 -std=c++17 tests/sink_test.cpp -o st && ./st)
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
//   Simulates the aggregate-device sink of WasapiBridge::serviceSink. Master clock at 48000 Hz: it wakes every
//   masterPeriod frames and the host delivers blocks of bufSize until the request is covered (like run()/outFifo).
//   The secondary device consumes devPeriod frames every devPeriod/(48000*(1+ppm)) seconds (Shared engine). On each
//   wake-up the same policy as serviceSink() is applied: SinkFramesToWrite, priming, need (Hermite +3 / sinc +17),
//   consumption w*ratio and PiUpdate (drift.h). Measured: FIFO underflows, overflows, device starvation (padding
//   below what it consumes) and the mean ratio over the last minute (ppm), compared with the expected ratio.
//   0.9.4: cases with a low-latency master (period in ASIO frames, e.g. 132 = 144 frames at 48 kHz seen from
//   44.1 kHz) and "RESULT: PASSED" when every case with IAudioClock has no underflow/overflow and the ratio
//   is within 20 ppm of the expected one.
//   Single-threaded host program; no Windows dependencies.
#include "../src/drift.h"
#include <cstdio>
#include <cmath>

// Result of one simulated case.
struct Res { long under, over, starve; double ppm, expect; };

// clock = true: the PI fill adds what is queued in the device according to its interpolated clock (IAudioClock:
// written - played now) minus targetPad; false: FIFO only (fallback without IAudioClock).
// Simulates `seconds` of operation with the given device drift (ppm), buffer size, periods and resampler.
static Res Sim(double ppm, long bufSize, uint32_t masterPeriod, uint32_t devPeriod, bool sinc, double seconds, bool clock) {
    const double fs = 48000.0, rd = fs * (1.0 + ppm * 1e-6);
    const uint32_t bufFrames = masterPeriod * 2 + devPeriod * 4 < 1920 ? 1920 : masterPeriod * 2 + devPeriod * 4;
    const uint32_t targetPad = SinkTargetPad(masterPeriod, devPeriod, bufFrames);
    const int look = sinc ? 16 : 0;
    const double sp = SinkSetpoint(bufSize, masterPeriod, look);
    const double posBase = sinc ? 15.0 : 1.0, extra = sinc ? 17.0 : 3.0;
    const double overLimit = sp * 2 + bufSize * 4 + targetPad;
    double fifo = 0, pos = posBase, ratio = 1, fillEma = sp, integ = 0;
    bool primed = false;
    double pad = 0;                         // frames queued in the device
    uint64_t produced = 0; uint64_t wake = 0; uint64_t devTick = 0;
    double written = 0;                     // frames delivered to the device
    Res r = {0, 0, 0, 0, 0};
    double sumR = 0; long nR = 0;
    const double tEnd = seconds;
    for (;;) {
        double tWake = (double)(wake + 1) * masterPeriod / fs;
        double tDev = (double)(devTick + 1) * devPeriod / rd;
        if (tWake > tEnd && tDev > tEnd) break;
        if (tDev <= tWake) {                // the device engine consumes one period
            ++devTick;
            if (pad < devPeriod) { ++r.starve; pad = 0; } else pad -= devPeriod;
            continue;
        }
        ++wake;
        uint64_t needed = wake * masterPeriod;
        while (produced < needed) { produced += bufSize; fifo += bufSize; }
        uint32_t w = SinkFramesToWrite((uint32_t)pad, bufFrames, targetPad);
        if (fifo > overLimit) { fifo = sp + posBase; pos = posBase; ++r.over; }
        if (!w) continue;
        if (!primed) {
            double need = posBase + w * 1.003 + extra;
            if (fifo >= sp + need) { if (fifo > sp + need) fifo = sp + need; primed = true; pos = posBase; fillEma = sp; integ = 0; ratio = 1; }
            else { pad += w; written += w; continue; }    // silence
        }
        double need = pos + w * ratio + extra;
        if (fifo < need) { ++r.under; primed = false; pad += w; written += w; continue; }
        double p = pos + w * ratio;
        double consumed = floor(p) - posBase;
        fifo -= consumed; pos = p - consumed;
        pad += w; written += w;
        const double queued = clock ? written - tWake * rd : (double)targetPad;   // interpolated device clock
        ratio = PiUpdate(fillEma, integ, (fifo - pos) + (queued - targetPad), sp, (double)w / fs);
        if (tWake > tEnd - 60.0) { sumR += ratio; ++nR; }
    }
    r.ppm = nR ? (sumR / nR - 1.0) * 1e6 : 0;
    r.expect = (fs / rd - 1.0) * 1e6;
    return r;
}

int main() {
    printf("Sink: 10 min per case; expected ratio = 48000/(48000*(1+ppm)) - 1\n");
    printf("  clock  drift   buffer  period master/dev     resampler   under over starve   ratio ppm (expected)\n");
    const double ppms[] = {-300, -100, 0, 100, 300};
    struct C { long buf; uint32_t mp, dp; bool sinc; } cases[] = {{256, 480, 480, false}, {64, 128, 480, false}, {1024, 1024, 480, true}, {256, 480, 480, true},
        // 0.9.4: low-latency master (IAudioClient3) with internal conversion: the master period in ASIO frames
        // (144 and 128 frames at 48 kHz seen from 44.1 kHz), callbacks produced on demand in bursts
        {128, 132, 480, true}, {32, 118, 480, true}, {512, 132, 480, false}};
    long bad = 0;
    for (int clk = 1; clk >= 0; --clk) for (const C& c : cases) for (double ppm : ppms) {
        Res r = Sim(ppm, c.buf, c.mp, c.dp, c.sinc, 600.0, clk != 0);
        printf("  %-5s  %+5.0f   %5ld   %5u/%-5u            %-7s    %4ld %4ld %5ld   %+8.1f (%+.1f)\n",
               clk ? "yes" : "no", ppm, c.buf, c.mp, c.dp, c.sinc ? "sinc" : "Hermite", r.under, r.over, r.starve, r.ppm, r.expect);
        if (clk && (r.under || r.over || std::fabs(r.ppm - r.expect) > 20.0)) ++bad;   // with IAudioClock: no xrun, ratio within 20 ppm
    }
    printf("RESULT: %s\n", bad ? "FAILED" : "PASSED");
    return bad ? 1 : 0;
}
