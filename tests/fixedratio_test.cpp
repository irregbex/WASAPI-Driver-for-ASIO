// WASAPI Driver for ASIO - offline test of the fixed-ratio converter and the on-demand scheduling (0.9.4)
// (Linux: g++ -O2 -std=c++17 tests/fixedratio_test.cpp -o ft && ./ft)
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
//   1. Quality of src/fixedratio.h for 44.1k<->48k, 48k<->96k, 44.1k<->96k: SNR of a 1 kHz sine (one way and
//      round trip, > 100 dB for sinc), pass band (18 kHz at 44.1/48 within 0.05 dB), stop band when the rate goes
//      down (a tone 5 % above the new Nyquist frequency below -95 dB), CPU time per second of stereo audio.
//   2. Scheduling model of the driver's audio thread (src/sched.h: the converters, the on-demand callbacks, the
//      fixed input delay and the latency formulas the driver uses): six rate pairs, device periods 128/144/256/
//      480, ASIO buffers 32..1024, 10 simulated minutes each: exact sample accounting (the held frames never
//      grow), no underrun or overrun, input always in time, ASIO callback timing (deviation from an even grid);
//      then 5 s with real samples: an impulse goes out, through the output converter, the device buffer and the
//      stream latency, a loop-back cable, the input converter and the input FIFO; its measured round trip must
//      equal the reported input + output latency within 1 frame.
//   0.9.6 (KI-5): the quality policy fr::Choose no longer takes ResamplerQuality: sinc whenever the CPU budget
//   allows (both directions), Hermite only for an up-conversion over the budget, Windows for a down-conversion
//   over the budget or a pair without an exact ratio. Checked on fixed cases, on a sweep of the standard rates
//   x 1..32 channels against the rule, with a custom budget, and the log text of fr::Describe word for word.
//   Review R2: one budget per stream open shared by the output and the input converters (fr::Cost).
//   Prints the numbers and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/fixedratio.h"
#include "../src/sched.h"
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %-70s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }

// Runs a mono sine through a converter; returns the output.
static std::vector<float> Convert(fr::Converter& c, const std::vector<float>& in, size_t outN) {
    std::vector<float> out;
    out.reserve(outN);
    size_t ip = 0;
    while (out.size() < outN) {
        size_t want = outN - out.size() < 512 ? outN - out.size() : 512;
        while (c.missingFor(want) > 0) { float v = ip < in.size() ? in[ip] : 0.f; ++ip; *c.pushSlot() = v; }
        c.produce(want, [&](size_t, int, float v) { out.push_back(v); });
    }
    return out;
}
static std::vector<float> Sine(double f, double rate, size_t n, double amp = 0.5) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (float)(amp * std::sin(2.0 * 3.14159265358979323846 * f * (double)i / rate));
    return v;
}
// SNR of `got` against the ideal sine at `rate` (frames [skip, n - skip)).
static double Snr(const std::vector<float>& got, double f, double rate, size_t skip, double amp = 0.5) {
    double s = 0, e = 0;
    for (size_t i = skip; i + skip < got.size(); ++i) {
        const double ideal = amp * std::sin(2.0 * 3.14159265358979323846 * f * (double)i / rate);
        s += ideal * ideal; e += (got[i] - ideal) * (got[i] - ideal);
    }
    return 10.0 * std::log10(s / (e > 1e-30 ? e : 1e-30));
}
static double RmsDb(const std::vector<float>& v, size_t skip, double ref) {
    double s = 0; size_t n = 0;
    for (size_t i = skip; i + skip < v.size(); ++i) { s += (double)v[i] * v[i]; ++n; }
    return 10.0 * std::log10((s / (n ? n : 1)) / (ref * ref / 2.0) + 1e-30);
}

int main() {
    printf("Fixed-ratio converter and scheduling offline test\n");
    const double pairs[][2] = {{44100, 48000}, {48000, 44100}, {48000, 96000}, {96000, 48000}, {44100, 96000}, {96000, 44100}};
    // ---------------------------------------------------------------- 1. quality
    printf("Quality (sinc):\n");
    printf("    %-18s %5s %9s %9s %10s %10s %10s %9s\n", "pair", "taps", "SNR 1k", "trip 1k", "pass dB", "20k dB", "stop dB", "ms/s");
    bool snrOk = true, tripOk = true, passOk = true, stopOk = true;
    for (auto& pr : pairs) {
        const double a = pr[0], b = pr[1];
        fr::Converter c, back;
        c.configure(a, b, 1, 4096, 4096, true); back.configure(b, a, 1, 4096, 4096, true);
        const size_t nIn = (size_t)a * 2, nOut = (size_t)b * 2;
        const auto x = Sine(1000, a, nIn + 4096);
        auto y = Convert(c, x, nOut);
        const double snr = Snr(y, 1000, b, 2048);
        auto z = Convert(back, y, nIn);
        const double trip = Snr(z, 1000, a, 4096);
        const double lowNyq = (a < b ? a : b) / 2;
        c.reset(); auto yp = Convert(c, Sine(0.85 * lowNyq, a, nIn + 4096), nOut);
        const double pass = RmsDb(yp, 2048, 0.5);
        double g20 = 0;
        if (lowNyq > 21000) { c.reset(); g20 = RmsDb(Convert(c, Sine(20000, a, nIn + 4096), nOut), 2048, 0.5); }
        double stop = -999;
        if (b < a) { c.reset(); stop = RmsDb(Convert(c, Sine(1.05 * b / 2, a, nIn + 4096), nOut), 2048, 0.5); }
        // CPU: 60 s of stereo
        fr::Converter cs; cs.configure(a, b, 2, 1024, 1024, true);
        const auto t0 = std::chrono::steady_clock::now();
        size_t made = 0; float sink = 0; uint32_t rnd = 1;
        while (made < (size_t)b * 60) {
            while (cs.missingFor(480) > 0) { float* f = cs.pushSlot(); rnd = rnd * 1664525u + 1013904223u; f[0] = (float)(rnd >> 9) * 1e-7f; f[1] = -f[0]; }
            cs.produce(480, [&](size_t, int, float v) { sink += v; });
            made += 480;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 60.0;
        char name[32]; snprintf(name, 32, "%.0f->%.0f", a, b);
        printf("    %-18s %5d %9.1f %9.1f %10.3f %10.3f %10.1f %9.2f%s\n", name, c.taps(), snr, trip, pass, g20, stop, ms, sink == 12345.f ? " " : "");
        if (snr < 100) snrOk = false;
        if (trip < 100) tripOk = false;
        if (std::fabs(pass) > 0.05) passOk = false;
        if (b < a && stop > -95) stopOk = false;
    }
    Check(snrOk, "1 kHz sine, one way: SNR > 100 dB");
    Check(tripOk, "1 kHz sine, round trip: SNR > 100 dB");
    Check(passOk, "pass band (85 % of the lower Nyquist) within 0.05 dB");
    Check(stopOk, "stop band when the rate goes down (105 % of the new Nyquist) < -95 dB");
    {
        fr::Converter h; h.configure(44100, 48000, 1, 4096, 4096, false);
        auto y = Convert(h, Sine(1000, 44100, 90000), 96000);
        printf("    Hermite 44100->48000: 1 kHz SNR %.1f dB (low-CPU option, no anti-aliasing)\n", Snr(y, 1000, 48000, 64));
    }

    {   // the generic multichannel loop and the stereo loop give the same samples as mono conversions
        const auto x = Sine(997, 44100, 20000), y = Sine(3001, 44100, 20000, 0.3);
        fr::Converter m1, m2, s2, c3;
        m1.configure(44100, 48000, 1, 4096, 4096, true); m2.configure(44100, 48000, 1, 4096, 4096, true);
        s2.configure(44100, 48000, 2, 4096, 4096, true); c3.configure(44100, 48000, 3, 4096, 4096, true);
        const auto a = Convert(m1, x, 20000), b = Convert(m2, y, 20000);
        std::vector<float> st, tr;
        size_t ip = 0;
        while (st.size() < 40000) {
            while (s2.missingFor(500) > 0) { float* f = s2.pushSlot(); f[0] = ip < x.size() ? x[ip] : 0.f; f[1] = ip < y.size() ? y[ip] : 0.f; ++ip; }
            s2.produce(500, [&](size_t, int, float v) { st.push_back(v); });
        }
        ip = 0;
        while (tr.size() < 60000) {
            while (c3.missingFor(500) > 0) { float* f = c3.pushSlot(); f[0] = ip < x.size() ? x[ip] : 0.f; f[1] = ip < y.size() ? y[ip] : 0.f; f[2] = -f[0]; ++ip; }
            c3.produce(500, [&](size_t, int, float v) { tr.push_back(v); });
        }
        bool same = true;
        for (size_t i = 0; i < 20000; ++i)
            if (st[2 * i] != a[i] || st[2 * i + 1] != b[i] || tr[3 * i] != a[i] || tr[3 * i + 1] != b[i] || tr[3 * i + 2] != -a[i]) same = false;
        Check(same, "stereo and 3-channel conversion = mono conversion of each channel (bit-exact)");
    }
    {   // quality policy (0.9.4 review fixes 4 and 7; 0.9.6 KI-5: sinc whenever the budget allows, both directions)
        using fr::Quality;
        bool ok = fr::Choose(48000, 44100, 2) == Quality::Sinc &&                 // down, 140 x 2 x 44100 = 12 M
                  fr::Choose(44100, 48000, 2) == Quality::Sinc &&                 // KI-5: up is sinc too (was Hermite)
                  fr::Choose(44100, 48000, 8) == Quality::Sinc &&                 // 128 x 8 x 48000 = 49 M
                  fr::Choose(44100, 48000, 24) == Quality::Sinc &&                // 147 M: still within 150 M
                  fr::Choose(44100, 48000, 25) == Quality::Hermite &&             // 154 M: over budget, up -> Hermite
                  fr::Choose(44100, 48000, 32) == Quality::Hermite &&             // 128 x 32 x 48000 = 197 M MAC/s
                  fr::Choose(44100, 96000, 12) == Quality::Sinc &&                // 128 x 12 x 96000 = 147 M
                  fr::Choose(44100, 96000, 13) == Quality::Hermite &&             // 160 M
                  fr::Choose(96000, 44100, 16) == Quality::None &&                // 280 x 16 x 44100 = 198 M: Windows
                  fr::Choose(96000, 44100, 8) == Quality::Sinc &&
                  fr::Choose(44100, 44099, 2) == Quality::None &&                 // no exact ratio
                  fr::Choose(44100, 44099, 2, 1e12) == Quality::None;             // ... whatever the budget
        printf("    budget %.0f M MAC/s: 44.1->48k stereo %.1f M, 8 ch %.1f M; 96->44.1k stereo %.1f M\n", fr::kMacBudget / 1e6,
               fr::SincMacs(44100, 48000, 2) / 1e6, fr::SincMacs(44100, 48000, 8) / 1e6, fr::SincMacs(96000, 44100, 2) / 1e6);
        Check(ok, "quality policy: sinc within the CPU budget (up and down)");
        // The budget argument is honoured (a tiny budget forces the fallbacks, a huge one always allows sinc).
        const bool custom = fr::Choose(44100, 48000, 2, 10e6) == Quality::Hermite &&   // 12.3 M > 10 M
                            fr::Choose(48000, 44100, 2, 10e6) == Quality::None &&
                            fr::Choose(44100, 48000, 32, 1e12) == Quality::Sinc;
        Check(custom, "quality policy: custom budget (fallback below, sinc above)");
        // Sweep: every pair of standard rates x 1..32 channels follows the rule exactly.
        const double rates[] = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
        bool rule = true; int nSinc = 0, nHerm = 0, nNone = 0;
        for (double a : rates) for (double b : rates) {
            if (a == b) continue;
            for (int ch = 1; ch <= 32; ++ch) {
                const Quality q = fr::Choose(a, b, ch);
                const bool exact = fr::SincTaps(a, b) > 0, fits = fr::SincMacs(a, b, ch) <= fr::kMacBudget;
                const Quality want = !exact ? Quality::None : fits ? Quality::Sinc : b < a ? Quality::None : Quality::Hermite;
                if (q != want) { rule = false; printf("    %.0f -> %.0f, %d ch: got %d, want %d\n", a, b, ch, (int)q, (int)want); }
                (q == Quality::Sinc ? nSinc : q == Quality::Hermite ? nHerm : nNone)++;
            }
        }
        printf("    sweep: %d sinc, %d Hermite, %d Windows\n", nSinc, nHerm, nNone);
        Check(rule && nSinc > 0 && nHerm > 0 && nNone > 0, "quality policy: rate sweep x 1..32 channels follows the rule");
        // The log text (driver.cpp: "Output: rate conversion 44100 -> 48000 Hz, 2 channels: <Describe>").
        char t1[128], t2[128], t3[128], t4[128];
        fr::Describe(t1, sizeof t1, fr::Choose(44100, 48000, 2), 44100, 48000, 2);
        fr::Describe(t2, sizeof t2, fr::Choose(44100, 48000, 32), 44100, 48000, 32);
        fr::Describe(t3, sizeof t3, fr::Choose(96000, 44100, 16), 96000, 44100, 16);
        fr::Describe(t4, sizeof t4, fr::Choose(44100, 44099, 2), 44100, 44099, 2);
        printf("    log: %s | %s | %s | %s\n", t1, t2, t3, t4);
        const bool text = !strcmp(t1, "sinc (12 M MAC/s, budget 150 M)") &&
                          !strcmp(t2, "Hermite (sinc would cost 197 M MAC/s, over the budget of 150 M)") &&
                          !strcmp(t3, "Windows converts (sinc would cost 198 M MAC/s, over the budget of 150 M)") &&
                          !strcmp(t4, "Windows converts (no exact ratio)");
        char tiny[8];
        const int len = fr::Describe(tiny, sizeof tiny, Quality::Sinc, 44100, 48000, 2);   // truncated, terminated
        Check(text && len > 7 && strlen(tiny) == 7, "log text of the decision (fr::Describe)");
        // Review R2: ONE budget per stream open, shared by the output and every input converter, in the order of
        // driver.cpp (output, main input, extra inputs): each Choose gets what is left, each configured converter
        // subtracts fr::Cost. Output 44.1 -> 48 kHz with 20 channels takes 123 M; a 48 -> 44.1 kHz stereo input
        // (12 M) still fits; a second 8-channel input up-conversion (49 M) no longer fits -> Hermite; a 4-channel
        // down-conversion (25 M) -> Windows. With separate budgets all of them would have been sinc.
        double left = fr::kMacBudget;
        auto take = [&](double in, double out, int ch) {
            const Quality q = fr::Choose(in, out, ch, left);
            left = std::max(0.0, left - fr::Cost(q, in, out, ch));
            return q;
        };
        const Quality o = take(44100, 48000, 20), i1 = take(48000, 44100, 2), i2 = take(44100, 48000, 8), i3 = take(48000, 44100, 4);
        printf("    shared budget: output %d, input 1 %d, input 2 %d, input 3 %d, left %.1f M\n", (int)o, (int)i1, (int)i2, (int)i3, left / 1e6);
        const bool shared = o == Quality::Sinc && i1 == Quality::Sinc && i2 == Quality::Hermite && i3 == Quality::None &&
                            fr::Choose(48000, 44100, 4) == Quality::Sinc && fr::Choose(44100, 48000, 8) == Quality::Sinc &&
                            fr::Cost(Quality::Hermite, 44100, 48000, 2) == 4.0 * 2 * 48000 && fr::Cost(Quality::None, 44100, 48000, 2) == 0.0 &&
                            left >= 0.0;
        Check(shared, "one CPU budget shared by all converters of a stream open (R2)");
    }

    // ---------------------------------------------------------------- 2. scheduling
    printf("Scheduling, 10 simulated minutes per case (sinc), frames kept queued as in the driver (FillTarget), stream latency half a period:\n");
    const double sp[][2] = {{44100, 48000}, {48000, 44100}, {48000, 96000}, {44100, 96000}, {96000, 48000}, {96000, 44100}};
    const int periods[] = {128, 144, 256, 480};
    const int bufs[] = {32, 64, 128, 256, 512, 1024};
    bool acct = true, noXrun = true, latOk = true, formulaOk = true, resyncOk = true;
    long long worstExMin = (long long)1 << 60;
    double worstJit = 0, worstLat = 0;
    int cases = 0, measured = 0;
    for (auto& r : sp) for (int P : periods) for (int B : bufs) {
        sched::Config cfg; cfg.asioRate = r[0]; cfg.devRate = r[1]; cfg.inRate = r[1]; cfg.period = P; cfg.devBuffer = sched::FillTarget(P, 4 * P, B, r[0], r[1]);
        cfg.streamLat = P / 2; cfg.bufSize = B; cfg.sinc = true; cfg.inPeriod = P;
        sched::Sim s;
        if (!s.init(cfg, false)) { acct = false; printf("    init failed %.0f/%.0f P %d B %d\n", r[0], r[1], P, B); continue; }
        const sched::Result res = s.run(600.0);
        ++cases;
        if (!res.accountingOk) acct = false;
        if (res.underruns || res.overruns || res.inMissing) noXrun = false;
        if (res.maxJitterMs > worstJit) worstJit = res.maxJitterMs;
        if (std::fabs(res.reportedTrip - res.expectedTrip) > 1.0) formulaOk = false;
        // the measured round trip with real samples (5 s)
        sched::Sim m; m.init(cfg, true);
        const sched::Result rm = m.run(5.0);
        ++measured;
        const double err = rm.measuredTrip < 0 ? 1e9 : std::fabs(rm.measuredTrip - rm.reportedTrip);
        if (err > worstLat) worstLat = err;
        if (err > 1.0) latOk = false;
        const sched::Resync rs = sched::InResync(B, r[0], r[1], P, r[1], P);
        if (res.exMin < 0 || res.exMax > (long long)rs.hi) { resyncOk = false; printf("    re-alignment window: excess %lld..%lld, hi %u\n", res.exMin, res.exMax, rs.hi); }
        if (res.exMin < worstExMin) worstExMin = res.exMin;
        if ((P == 480 && (B == 64 || B == 256)) || (P == 144 && B == 128) || err > 1.0 || !res.accountingOk || res.underruns || res.inMissing)
            printf("    %5.0f->%-5.0f P %3d B %4d: callbacks %8llu, callback deviation %.3f ms, held %zu..%zu, round trip measured %.2f, reported %.0f (in delay %u)%s\n",
                   r[0], r[1], P, B, (unsigned long long)res.callbacks, res.maxJitterMs, res.heldMin, res.heldMax,
                   rm.measuredTrip, rm.reportedTrip, m.delay(), res.inMissing ? " INPUT MISSING" : "");
    }
    printf("    %d cases (+%d measured with real samples); largest callback deviation from an even grid %.3f ms;\n"
           "    largest |measured - reported| round trip %.2f frames\n", cases, measured, worstJit, worstLat);
    Check(acct, "exact sample accounting: held frames bounded, no drift (all cases)");
    Check(noXrun, "no underrun, no overrun, input always in time (all cases)");
    Check(formulaOk, "getLatencies in + out (sched::ReportLatencies) = model round trip (all cases)");
    printf("    smallest input margin (frames ready beyond the delivered block): %lld\n", worstExMin);
    Check(resyncOk, "input never leaves the re-alignment window (no spurious re-alignment)");
    Check(latOk, "measured round trip (impulse through both converters) = reported, within 1 frame");
    {   // expected output latency at 44.1 kHz ASIO, 48 kHz device, buffer 128, stream latency assumed half a period
        const uint32_t f480 = sched::FillTarget(480, 1056, 128, 44100, 48000), f144 = sched::FillTarget(144, 1056, 128, 44100, 48000);
        const long l480 = sched::OutLatency(128, 44100, 48000, f480, 240, 64), l144 = sched::OutLatency(128, 44100, 48000, f144, 72, 64);
        printf("    44.1 kHz ASIO, 48 kHz device, buffer 128: output latency with a 480-frame engine period %ld frames (%.1f ms),\n"
               "    with a 144-frame period %ld frames (%.1f ms); before 0.9.4 (AUTOCONVERTPCM, typical 22 ms buffer + 5 ms):\n"
               "    %ld frames (%.1f ms) plus the unreported delay of the Windows converter\n",
               l480, l480 / 44.1, l144, l144 / 44.1, 128 + (long)(0.027 * 44100), (128 + 0.027 * 44100) / 44.1);
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
