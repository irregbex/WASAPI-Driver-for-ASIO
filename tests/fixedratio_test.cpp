// WASAPI Driver for ASIO - offline test of the fixed-ratio converter and the on-demand scheduling (0.9.4)
// (Linux: sh tests/run_all.sh, which also builds the soxr objects; by hand, 0.9.10: compile the soxr C files of
// BUILDING.txt with gcc -O2 -w -DSOXR_LIB -DNDEBUG -Ithird_party/soxr -c, then
// g++ -O2 -std=c++17 -Ithird_party/soxr/src tests/fixedratio_test.cpp <soxr objects> -o ft && ./ft
// and add -DWB_COUNT_ALLOC=1 -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc to count soxr's allocations)
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
//   3. 0.9.10, the soxr engine (src/frsoxr.h, SoX Resampler library HQ) next to sinc and Hermite: a table per rate
//      pair with the look-ahead (the latency each engine adds), SNR and THD+N of 1 kHz and 18 kHz sines, pass-band
//      ripple (20 Hz .. 85 % of the lower Nyquist frequency), aliasing (down) / imaging (up) rejection and CPU;
//      the output is time-aligned with the input (an impulse comes out at exactly its input position, so the
//      latency is the look-ahead alone); multichannel = mono per channel; the FixedRatioEngine policy (Auto =
//      the 0.9.9 sinc rule, never soxr; Soxr only when chosen; Sinc; Hermite) on fixed cases, a sweep and the log text; the scheduling model with soxr in both
//      converters: accounting, no xrun, and the impulse round trip measured = reported within 1 frame; no promised
//      soxr frame ever missing; and (built with WB_COUNT_ALLOC, as tests/run_all.sh does) zero allocations by soxr
//      on the audio-thread path (push / produce) after configure() and reset(), over minutes of driver-like traffic.
//   Prints the numbers and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/fixedratio.h"
#include "../src/sched.h"
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

// 0.9.10: allocation counter for soxr (its objects are linked with -Wl,--wrap=malloc,calloc,realloc by run_all.sh;
// the test's own C++ allocations go through libstdc++ and are not wrapped). Only counts while g_countOn is set.
#if defined WB_COUNT_ALLOC
static long g_allocs = 0;
static bool g_countOn = false;
extern "C" {
void* __real_malloc(size_t n);
void* __real_calloc(size_t n, size_t m);
void* __real_realloc(void* p, size_t n);
void* __wrap_malloc(size_t n) { if (g_countOn) ++g_allocs; return __real_malloc(n); }
void* __wrap_calloc(size_t n, size_t m) { if (g_countOn) ++g_allocs; return __real_calloc(n, m); }
void* __wrap_realloc(void* p, size_t n) { if (g_countOn) ++g_allocs; return __real_realloc(p, n); }
}
#endif

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
// 0.9.10: least-squares fit of a sine of frequency f (plus DC) to got[skip, n - skip): amplitude, and the RMS of what
// is left (THD + noise + images / aliases). Solves the 3x3 normal equations, so the result does not depend on a
// whole number of periods.
static void FitTone(const std::vector<float>& got, double f, double rate, size_t skip, double& amp, double& resid) {
    double m[3][3] = {}, r[3] = {};
    const double w = 2.0 * 3.14159265358979323846 * f / rate;
    for (size_t i = skip; i + skip < got.size(); ++i) {
        const double b[3] = {std::sin(w * (double)i), std::cos(w * (double)i), 1.0};
        for (int a = 0; a < 3; ++a) { r[a] += b[a] * got[i]; for (int c = 0; c < 3; ++c) m[a][c] += b[a] * b[c]; }
    }
    double x[3] = {};
    for (int a = 0; a < 3; ++a) {                                   // Gaussian elimination (well conditioned)
        for (int k = a + 1; k < 3; ++k) {
            const double q = m[k][a] / m[a][a];
            for (int c = a; c < 3; ++c) m[k][c] -= q * m[a][c];
            r[k] -= q * r[a];
        }
    }
    for (int a = 2; a >= 0; --a) { double t = r[a]; for (int c = a + 1; c < 3; ++c) t -= m[a][c] * x[c]; x[a] = t / m[a][a]; }
    amp = std::sqrt(x[0] * x[0] + x[1] * x[1]);
    double e = 0; size_t n = 0;
    for (size_t i = skip; i + skip < got.size(); ++i) {
        const double v = x[0] * std::sin(w * (double)i) + x[1] * std::cos(w * (double)i) + x[2];
        e += (got[i] - v) * (got[i] - v); ++n;
    }
    resid = std::sqrt(e / (double)(n ? n : 1));
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
        // 0.9.10: these checks are the sinc rule of 0.9.6 (FixedRatioEngine=Sinc); the soxr preference (Auto, the
        // default) has its own checks in section 3.
        const fr::Engine kS = fr::Engine::Sinc;
        bool ok = fr::Choose(48000, 44100, 2, fr::kMacBudget, kS) == Quality::Sinc &&                 // down, 140 x 2 x 44100 = 12 M
                  fr::Choose(44100, 48000, 2, fr::kMacBudget, kS) == Quality::Sinc &&                 // KI-5: up is sinc too (was Hermite)
                  fr::Choose(44100, 48000, 8, fr::kMacBudget, kS) == Quality::Sinc &&                 // 128 x 8 x 48000 = 49 M
                  fr::Choose(44100, 48000, 24, fr::kMacBudget, kS) == Quality::Sinc &&                // 147 M: still within 150 M
                  fr::Choose(44100, 48000, 25, fr::kMacBudget, kS) == Quality::Hermite &&             // 154 M: over budget, up -> Hermite
                  fr::Choose(44100, 48000, 32, fr::kMacBudget, kS) == Quality::Hermite &&             // 128 x 32 x 48000 = 197 M MAC/s
                  fr::Choose(44100, 96000, 12, fr::kMacBudget, kS) == Quality::Sinc &&                // 128 x 12 x 96000 = 147 M
                  fr::Choose(44100, 96000, 13, fr::kMacBudget, kS) == Quality::Hermite &&             // 160 M
                  fr::Choose(96000, 44100, 16, fr::kMacBudget, kS) == Quality::None &&                // 280 x 16 x 44100 = 198 M: Windows
                  fr::Choose(96000, 44100, 8, fr::kMacBudget, kS) == Quality::Sinc &&
                  fr::Choose(44100, 44099, 2, fr::kMacBudget, kS) == Quality::None &&                 // no exact ratio
                  fr::Choose(44100, 44099, 2, 1e12, kS) == Quality::None;             // ... whatever the budget
        printf("    budget %.0f M MAC/s: 44.1->48k stereo %.1f M, 8 ch %.1f M; 96->44.1k stereo %.1f M\n", fr::kMacBudget / 1e6,
               fr::SincMacs(44100, 48000, 2) / 1e6, fr::SincMacs(44100, 48000, 8) / 1e6, fr::SincMacs(96000, 44100, 2) / 1e6);
        Check(ok, "quality policy: sinc within the CPU budget (up and down)");
        // The budget argument is honoured (a tiny budget forces the fallbacks, a huge one always allows sinc).
        const bool custom = fr::Choose(44100, 48000, 2, 10e6, kS) == Quality::Hermite &&   // 12.3 M > 10 M
                            fr::Choose(48000, 44100, 2, 10e6, kS) == Quality::None &&
                            fr::Choose(44100, 48000, 32, 1e12, kS) == Quality::Sinc;
        Check(custom, "quality policy: custom budget (fallback below, sinc above)");
        // Sweep: every pair of standard rates x 1..32 channels follows the rule exactly.
        const double rates[] = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
        bool rule = true; int nSinc = 0, nHerm = 0, nNone = 0;
        for (double a : rates) for (double b : rates) {
            if (a == b) continue;
            for (int ch = 1; ch <= 32; ++ch) {
                const Quality q = fr::Choose(a, b, ch, fr::kMacBudget, kS);
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
        fr::Describe(t1, sizeof t1, fr::Choose(44100, 48000, 2, fr::kMacBudget, kS), 44100, 48000, 2, fr::kMacBudget, kS);
        fr::Describe(t2, sizeof t2, fr::Choose(44100, 48000, 32, fr::kMacBudget, kS), 44100, 48000, 32, fr::kMacBudget, kS);
        fr::Describe(t3, sizeof t3, fr::Choose(96000, 44100, 16, fr::kMacBudget, kS), 96000, 44100, 16, fr::kMacBudget, kS);
        fr::Describe(t4, sizeof t4, fr::Choose(44100, 44099, 2, fr::kMacBudget, kS), 44100, 44099, 2, fr::kMacBudget, kS);
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
            const Quality q = fr::Choose(in, out, ch, left, kS);
            left = std::max(0.0, left - fr::Cost(q, in, out, ch));
            return q;
        };
        const Quality o = take(44100, 48000, 20), i1 = take(48000, 44100, 2), i2 = take(44100, 48000, 8), i3 = take(48000, 44100, 4);
        printf("    shared budget: output %d, input 1 %d, input 2 %d, input 3 %d, left %.1f M\n", (int)o, (int)i1, (int)i2, (int)i3, left / 1e6);
        const bool shared = o == Quality::Sinc && i1 == Quality::Sinc && i2 == Quality::Hermite && i3 == Quality::None &&
                            fr::Choose(48000, 44100, 4, fr::kMacBudget, kS) == Quality::Sinc && fr::Choose(44100, 48000, 8, fr::kMacBudget, kS) == Quality::Sinc &&
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

    // ---------------------------------------------------------------- 3. soxr engine (0.9.10)
    using fr::Quality;
    printf("soxr engine (%s) compared with sinc and Hermite (mono, -6 dBFS sines; look-ahead = latency the converter adds):\n",
           fr::SoxrCore::Name());
    printf("    %-13s %-7s %13s %8s %8s %8s %8s %8s %9s %7s\n", "pair", "engine", "look-ahead", "SNR 1k", "THD+N", "SNR 18k",
           "THD+N", "ripple", "alias/img", "ms/s");
    struct Row { double snr1, thd1, snr18, thd18, ripple, alias, ms; int ahead; };
    const double qp[][2] = {{44100, 48000}, {48000, 44100}, {48000, 96000}, {96000, 48000}, {44100, 96000}, {96000, 44100}};
    bool sxQuality = true, sxBeatsHermite = true;
    double macEqWorst = 0;
    for (auto& pr : qp) {
        const double a = pr[0], b = pr[1], lowNyq = (a < b ? a : b) / 2;
        Row rows[3] = {};
        const Quality engines[3] = {Quality::Soxr, Quality::Sinc, Quality::Hermite};
        for (int e = 0; e < 3; ++e) {
            fr::Converter c;
            if (!c.configure(a, b, 1, 4096, 4096, engines[e]) || c.quality() != engines[e]) { Check(false, "configure (quality table)"); continue; }
            Row& r = rows[e];
            r.ahead = c.ahead();
            const size_t nIn = (size_t)(a / 2), nOut = (size_t)(b / 2), skip = 2048;
            auto run = [&](double f) { c.reset(); return Convert(c, Sine(f, a, nIn + 8192), nOut); };
            double amp = 0, res = 0;
            auto y1 = run(1000);  r.snr1 = Snr(y1, 1000, b, skip);  FitTone(y1, 1000, b, skip, amp, res);  r.thd1 = 20 * std::log10(res / amp + 1e-30);
            auto y18 = run(18000); r.snr18 = Snr(y18, 18000, b, skip); FitTone(y18, 18000, b, skip, amp, res); r.thd18 = 20 * std::log10(res / amp + 1e-30);
            // pass-band ripple: gain of 16 tones from 20 Hz to 85 % of the lower Nyquist frequency (max - min, dB)
            double gmin = 1e9, gmax = -1e9;
            for (int k = 0; k < 16; ++k) {
                const double f = 20.0 * std::pow(0.85 * lowNyq / 20.0, k / 15.0);
                auto y = run(f); FitTone(y, f, b, skip, amp, res);
                const double g = 20 * std::log10(amp / 0.5);
                gmin = std::min(gmin, g); gmax = std::max(gmax, g);
            }
            r.ripple = gmax - gmin;
            // aliasing (rate down): tones at 102 %, 110 % and 130 % of the new Nyquist frequency (below the old
            // one), output level relative to the input; imaging (rate up): tone at 85 % of the input Nyquist, what
            // is left beside it (images folded back into the output band) relative to the tone.
            r.alias = -999;
            if (b < a) {
                for (double k : {1.02, 1.1, 1.3}) {
                    const double f = k * b / 2;
                    if (f >= 0.98 * a / 2) continue;
                    r.alias = std::max(r.alias, RmsDb(run(f), skip, 0.5));
                }
            } else {
                const double f = 0.85 * a / 2;
                auto y = run(f); FitTone(y, f, b, skip, amp, res);
                r.alias = 20 * std::log10(res / amp + 1e-30);
            }
            // CPU: 20 s of stereo, device-like requests of 480 frames
            fr::Converter cs; cs.configure(a, b, 2, 1024, 1024, engines[e]);
            const auto t0 = std::chrono::steady_clock::now();
            size_t made = 0; float sink = 0; uint32_t rnd = 1;
            while (made < (size_t)b * 20) {
                while (cs.missingFor(480) > 0) { float* f = cs.pushSlot(); rnd = rnd * 1664525u + 1013904223u; f[0] = (float)(rnd >> 9) * 1e-7f; f[1] = -f[0]; }
                cs.produce(480, [&](size_t, int, float v) { sink += v; });
                made += 480;
            }
            r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0 + (sink == 12345.f ? 1e-9 : 0.0);
        }
        static const char* names[3] = {"soxr HQ", "sinc", "Hermite"};
        for (int e = 0; e < 3; ++e) {
            char name[32], la[32];
            snprintf(name, sizeof name, "%.0f->%.0f", a, b);
            snprintf(la, sizeof la, "%d (%.2f ms)", rows[e].ahead, 1000.0 * rows[e].ahead / a);
            printf("    %-13s %-7s %13s %8.1f %8.1f %8.1f %8.1f %8.4f %9.1f %7.2f\n", e ? "" : name, names[e], la, rows[e].snr1, rows[e].thd1,
                   rows[e].snr18, rows[e].thd18, rows[e].ripple, rows[e].alias, rows[e].ms);
        }
        const Row& x = rows[0];
        if (x.snr1 < 120 || x.thd1 > -125 || x.thd18 > -125 || x.ripple > 0.01 || x.alias > -125) sxQuality = false;
        if (x.alias > rows[2].alias - 60) sxBeatsHermite = false;
        // soxr cost in sinc MAC equivalents per channel and per input + output frame (compare kSoxrMacsPerFrame)
        const double eq = rows[1].ms > 0 ? x.ms / rows[1].ms * fr::SincMacs(a, b, 2) / (2.0 * (a + b)) : 0;
        printf("    %-13s soxr cost %.1f sinc MAC equivalents per channel and input + output frame (model: %.0f)\n", "", eq, fr::kSoxrMacsPerFrame);
        macEqWorst = std::max(macEqWorst, eq);
    }
    Check(sxQuality, "soxr HQ: SNR 1k > 120 dB, THD+N (1k, 18k) < -125 dB, ripple < 0.01 dB, alias/image < -125 dB");
    Check(sxBeatsHermite, "soxr HQ: aliasing / imaging at least 60 dB below Hermite (all pairs)");
    printf("    largest measured soxr cost %.1f MAC equivalents per channel and frame (timing information only, not checked)\n", macEqWorst);

    {   // host-thread cost of configure() with soxr (look-ahead probe + instance + warm-up), information only; the
        // look-ahead cache (review item 2): a second configure of a pair skips the probe and gets the same look-ahead
        printf("    configure() time with soxr (host thread, stream open; first / second open of the pair):\n");
        bool cacheOk = true;
        for (auto& pr : {std::pair<double, double>{22050, 16000}, {16000, 22050}, {32000, 44100}}) {   // pairs not used before
            fr::Converter c1, c2;
            const auto t0 = std::chrono::steady_clock::now();
            c1.configure(pr.first, pr.second, 2, 1088, 2048, Quality::Soxr);
            const auto t1 = std::chrono::steady_clock::now();
            c2.configure(pr.first, pr.second, 2, 1088, 2048, Quality::Soxr);
            const auto t2 = std::chrono::steady_clock::now();
            int a3 = 0; size_t s3 = 0;
            fr::SoxrCore::Probe((long)pr.first, (long)pr.second, (long)(pr.first / fr::Gcd((long)pr.first, (long)pr.second)),
                                (long)(pr.second / fr::Gcd((long)pr.first, (long)pr.second)), (size_t)(2 * pr.first), a3, s3, nullptr, 0);
            if (c1.ahead() != c2.ahead() || c1.ahead() != a3 || c1.quality() != Quality::Soxr || c2.quality() != Quality::Soxr) cacheOk = false;
            printf("      %.0f->%.0f stereo: %.0f ms / %.0f ms, look-ahead %d / %d (fresh probe %d) frames\n", pr.first, pr.second,
                   std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
                   c1.ahead(), c2.ahead(), a3);
        }
        Check(cacheOk, "soxr look-ahead cache: a second open of a pair gets the probed look-ahead");
        for (auto& pr : {std::pair<double, double>{44100, 48000}, {48000, 44100}}) for (int ch : {2, 8, 32}) {
            const auto t0 = std::chrono::steady_clock::now();
            fr::Converter c; c.configure(pr.first, pr.second, ch, 1088, 2048, Quality::Soxr);
            printf("      %.0f->%.0f %d ch (look-ahead cached): %.0f ms\n", pr.first, pr.second, ch,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
    }
    {   // time alignment: an impulse at input frame k comes out at output position k * den / num (no shift), so the
        // only latency a converter adds is its look-ahead (what sched.h reports)
        bool aligned = true;
        for (auto& pr : qp) for (Quality q : {Quality::Soxr, Quality::Sinc}) {
            fr::Converter c; c.configure(pr[0], pr[1], 1, 4096, 4096, q);
            std::vector<float> x((size_t)pr[0], 0.f);
            const size_t k = 3001; x[k] = 1.f;
            auto y = Convert(c, x, (size_t)pr[1] / 2);
            size_t best = 0; for (size_t i = 0; i < y.size(); ++i) if (std::fabs(y[i]) > std::fabs(y[best])) best = i;
            double frac = 0;
            if (best > 0 && best + 1 < y.size()) {
                const double p0 = y[best - 1], p1 = y[best], p2 = y[best + 1], d = p0 - 2 * p1 + p2;
                if (std::fabs(d) > 1e-12) frac = 0.5 * (p0 - p2) / d;
            }
            const double want = (double)k * pr[1] / pr[0], got = (double)best + frac;
            if (std::fabs(got - want) > 0.25) { aligned = false; printf("    %.0f->%.0f %s: impulse at %.3f, expected %.3f\n", pr[0], pr[1], q == Quality::Soxr ? "soxr" : "sinc", got, want); }
        }
        Check(aligned, "impulse comes out at its exact input position (soxr and sinc, within 0.25 frame)");
    }
    {   // multichannel = mono per channel (soxr runs one filter per channel)
        const auto x = Sine(997, 44100, 30000), y = Sine(3001, 44100, 30000, 0.3);
        fr::Converter m1, m2, s2;
        m1.configure(44100, 48000, 1, 4096, 4096, Quality::Soxr); m2.configure(44100, 48000, 1, 4096, 4096, Quality::Soxr);
        s2.configure(44100, 48000, 2, 4096, 4096, Quality::Soxr);
        const auto a = Convert(m1, x, 30000), b = Convert(m2, y, 30000);
        std::vector<float> st; size_t ip = 0;
        while (st.size() < 60000) {
            while (s2.missingFor(500) > 0) { float* f = s2.pushSlot(); f[0] = ip < x.size() ? x[ip] : 0.f; f[1] = ip < y.size() ? y[ip] : 0.f; ++ip; }
            s2.produce(500, [&](size_t, int, float v) { st.push_back(v); });
        }
        bool same = true;
        for (size_t i = 0; i < 30000; ++i) if (st[2 * i] != a[i] || st[2 * i + 1] != b[i]) same = false;
        Check(same, "soxr: stereo conversion = mono conversion of each channel (bit-exact)");
        // reset() starts a new run: the same input gives the same output again (no trace of the previous run, same
        // timing). Not bit-exact: soxr's FFT blocks start at another phase of the input after the flush, so the
        // rounding differs (by about -128 dBFS).
        m1.reset();
        const auto a2 = Convert(m1, x, 30000);
        double dmax = 0;
        for (size_t i = 0; i < a.size() && i < a2.size(); ++i) dmax = std::max(dmax, (double)std::fabs(a2[i] - a[i]));
        printf("    soxr after reset(): largest difference from the first run %.1f dBFS\n", 20 * std::log10(dmax + 1e-30));
        Check(a2.size() == a.size() && dmax < 1e-6, "soxr: reset() restarts cleanly (same output within -120 dBFS, same timing)");
    }
    {   // FixedRatioEngine policy (Choose / Cost / Describe with fr::Engine)
        using fr::Engine;
        // Auto (default) = the 0.9.9 sinc rule, never soxr; soxr only when chosen explicitly (Engine::Soxr).
        const Engine X = Engine::Soxr;
        const bool fixed =
            fr::Choose(44100, 48000, 2) == Quality::Sinc &&                                  // Auto: sinc (0.9.9)
            fr::Choose(44100, 48000, 32) == Quality::Hermite &&                              // Auto: as 0.9.9
            fr::Choose(96000, 44100, 16) == Quality::None &&                                 // Auto: as 0.9.9
            fr::Choose(44100, 48000, 2, fr::kMacBudget, X) == Quality::Soxr &&               // soxr 11.8 M
            fr::Choose(44100, 48000, 24, fr::kMacBudget, X) == Quality::Soxr &&              // 64 x 24 x 92100 = 141 M
            fr::Choose(44100, 48000, 32, fr::kMacBudget, X) == Quality::Hermite &&           // soxr 189 M, sinc 197 M: up
            fr::Choose(96000, 44100, 16, fr::kMacBudget, X) == Quality::Soxr &&              // 143 M (sinc: 198 M)
            fr::Choose(96000, 44100, 24, fr::kMacBudget, X) == Quality::None &&              // soxr 215 M, sinc 296 M
            fr::Choose(44100, 96000, 32, fr::kMacBudget, X) == Quality::Hermite &&           // soxr 287 M, sinc 393 M: up
            fr::Choose(44100, 48000, 2, 5e6, X) == Quality::Hermite &&                       // soxr 11.8 M, sinc 12.3 M > 5 M
            fr::Choose(44100, 48000, 2, 12e6, X) == Quality::Soxr &&                         // soxr fits 12 M, sinc does not
            fr::Choose(44100, 48000, 2, fr::kMacBudget, Engine::Sinc) == Quality::Sinc &&
            fr::Choose(44100, 48000, 2, fr::kMacBudget, Engine::Hermite) == Quality::Hermite &&
            fr::Choose(48000, 44100, 2, fr::kMacBudget, Engine::Hermite) == Quality::Sinc &&   // never Hermite down
            fr::Choose(44100, 44099, 2, 1e12, X) == Quality::None &&                         // no exact ratio, any engine
            fr::Choose(44100, 44099, 2, 1e12, Engine::Hermite) == Quality::None &&
            fr::Cost(Quality::Soxr, 44100, 48000, 2) == fr::kSoxrMacsPerFrame * 2 * (44100 + 48000);
        Check(fixed, "FixedRatioEngine policy: fixed cases (Auto = 0.9.9 sinc rule, Soxr only when chosen)");
        const double rates[] = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
        bool rule = true, autoIsSinc = true; int n[4][4] = {};
        for (int pe = 0; pe < 4; ++pe) for (double a : rates) for (double b : rates) {
            if (a == b) continue;
            for (int ch = 1; ch <= 32; ++ch) {
                const Engine pref = (Engine)pe;
                const Quality q = fr::Choose(a, b, ch, fr::kMacBudget, pref);
                const bool exact = fr::SincTaps(a, b) > 0, sxFits = fr::SoxrMacs(a, b, ch) <= fr::kMacBudget,
                           sincFits = fr::SincMacs(a, b, ch) <= fr::kMacBudget, up = b > a;
                Quality want = !exact ? Quality::None : (pref == Engine::Hermite && up) ? Quality::Hermite
                             : (pref == Engine::Soxr && sxFits) ? Quality::Soxr
                             : sincFits ? Quality::Sinc : up ? Quality::Hermite : Quality::None;
                if (q != want) { rule = false; printf("    pref %d %.0f -> %.0f, %d ch: got %d, want %d\n", pe, a, b, ch, (int)q, (int)want); }
                if (pe == 0 && q != fr::Choose(a, b, ch, fr::kMacBudget, Engine::Sinc)) autoIsSinc = false;
                ++n[pe][(int)q];
            }
        }
        printf("    sweep, Auto: %d soxr, %d sinc, %d Hermite, %d Windows; Soxr chosen: %d soxr, %d sinc, %d Hermite, %d Windows\n",
               n[0][3], n[0][2], n[0][1], n[0][0], n[1][3], n[1][2], n[1][1], n[1][0]);
        Check(rule && autoIsSinc && n[0][3] == 0 && n[1][3] > 0,
              "FixedRatioEngine policy: sweep x 1..32 channels x 4 preferences (Auto never soxr, = Sinc)");
        char t1[200], t2[200], t3[200], t4[200], t5[200];
        fr::Describe(t1, sizeof t1, fr::Choose(44100, 48000, 2), 44100, 48000, 2);
        fr::Describe(t2, sizeof t2, fr::Choose(96000, 44100, 24, 300e6, X), 96000, 44100, 24, 300e6, X);
        fr::Describe(t3, sizeof t3, fr::Choose(44100, 48000, 2, fr::kMacBudget, Engine::Hermite), 44100, 48000, 2, fr::kMacBudget, Engine::Hermite);
        fr::Describe(t4, sizeof t4, fr::Choose(96000, 44100, 24, fr::kMacBudget, X), 96000, 44100, 24, fr::kMacBudget, X);
        fr::Describe(t5, sizeof t5, fr::Choose(44100, 48000, 2, 5e6, X), 44100, 48000, 2, 5e6, X);
        printf("    log: %s\n         %s\n         %s\n         %s\n         %s\n", t1, t2, t3, t4, t5);
        const bool text = !strcmp(t1, "sinc (12 M MAC/s, budget 150 M)") &&                      // Auto: the 0.9.9 line
                          !strcmp(t2, "soxr HQ (215 M MAC/s equivalent, budget 300 M)") &&
                          !strcmp(t3, "Hermite (FixedRatioEngine=Hermite)") &&
                          !strcmp(t4, "Windows converts (sinc would cost 296 M MAC/s, over the budget of 150 M); soxr HQ not used: "
                                      "it would cost 215 M MAC/s equivalent, over the budget of 150 M") &&
                          !strcmp(t5, "Hermite (sinc would cost 12 M MAC/s, over the budget of 5 M); soxr HQ not used: "
                                      "it would cost 12 M MAC/s equivalent, over the budget of 5 M");
        // review item 6: soxr chosen and within the budget but not available -> the sinc rule with the same budget
        const bool fb = fr::FallbackFromSoxr(44100, 48000, 2, fr::kMacBudget) == Quality::Sinc &&
                        fr::FallbackFromSoxr(44100, 48000, 2, 12e6) == Quality::Hermite &&    // soxr fitted 12 M, sinc 12.3 M not
                        fr::FallbackFromSoxr(96000, 44100, 16, fr::kMacBudget) == Quality::None &&   // soxr 143 M fitted, sinc 198 M
                        fr::FallbackFromSoxr(44100, 44099, 2, 1e12) == Quality::None &&
                        fr::Choose(44100, 48000, 2, 12e6, X) == Quality::Soxr && fr::Choose(96000, 44100, 16, fr::kMacBudget, X) == Quality::Soxr;
        Check(fb, "soxr not available: fallback = sinc rule with the same budget (Hermite / Windows when sinc does not fit)");
        Check(text, "log text of the decision with soxr (fr::Describe)");
    }

    {   // scheduling model with soxr in both converters (subset of section 2: 6 pairs x 2 periods x 4 buffers)
        printf("Scheduling with soxr (both converters), 10 simulated minutes + 5 s with real samples per case:\n");
        bool sAcct = true, sXrun = true, sLat = true, sForm = true, sResync = true, sShort = true;
        double sWorst = 0; int sCases = 0;
        for (auto& r : sp) for (int P : {144, 480}) for (int B : {32, 128, 512, 1024}) {
            sched::Config cfg; cfg.asioRate = r[0]; cfg.devRate = r[1]; cfg.inRate = r[1]; cfg.period = P;
            cfg.devBuffer = sched::FillTarget(P, 4 * P, B, r[0], r[1]); cfg.streamLat = P / 2; cfg.bufSize = B; cfg.soxr = true; cfg.inPeriod = P;
            sched::Sim s, m;
            if (!s.init(cfg, false) || !m.init(cfg, true)) { sAcct = false; printf("    init failed %.0f/%.0f P %d B %d\n", r[0], r[1], P, B); continue; }
            const sched::Result res = s.run(600.0), rm = m.run(5.0);
            ++sCases;
            if (!res.accountingOk) sAcct = false;
            if (res.underruns || res.overruns || res.inMissing || rm.underruns || rm.overruns || rm.inMissing) sXrun = false;
            if (std::fabs(res.reportedTrip - res.expectedTrip) > 1.0) sForm = false;
            const double err = rm.measuredTrip < 0 ? 1e9 : std::fabs(rm.measuredTrip - rm.reportedTrip);
            sWorst = std::max(sWorst, err);
            if (err > 1.0) sLat = false;
            if (m.shortfalls()) sShort = false;
            const sched::Resync rs = sched::InResync(B, r[0], r[1], P, r[1], P);
            if (res.exMin < 0 || res.exMax > (long long)rs.hi) { sResync = false; printf("    re-alignment window: excess %lld..%lld, hi %u\n", res.exMin, res.exMax, rs.hi); }
            if ((P == 480 && B == 128) || (P == 144 && B == 32) || err > 1.0)
                printf("    %5.0f->%-5.0f P %3d B %4d: look-ahead out %d / in %d frames, round trip measured %.2f, reported %.0f (in delay %u)\n",
                       r[0], r[1], P, B, m.aheadOut(), m.aheadIn(), rm.measuredTrip, rm.reportedTrip, m.delay());
        }
        printf("    %d cases; largest |measured - reported| round trip %.2f frames\n", sCases, sWorst);
        Check(sAcct, "soxr: exact sample accounting (all cases)");
        Check(sXrun, "soxr: no underrun, no overrun, input always in time (all cases)");
        Check(sForm && sResync, "soxr: reported latencies = model round trip; input within the re-alignment window");
        Check(sLat, "soxr: measured round trip (impulse through both converters) = reported, within 1 frame");
        Check(sShort, "soxr: every frame promised by the look-ahead was produced (no shortfall)");
        const uint32_t f144 = sched::FillTarget(144, 1056, 128, 44100, 48000);
        fr::Converter o; o.configure(44100, 48000, 2, 192, 1100, Quality::Soxr);
        const long ls = sched::OutLatency(128, 44100, 48000, f144, 72, o.ahead()), lc = sched::OutLatency(128, 44100, 48000, f144, 72, 64);
        printf("    44.1 kHz ASIO, 48 kHz device, buffer 128, 144-frame period: output latency %ld frames (%.1f ms) with soxr,\n"
               "    %ld frames (%.1f ms) with sinc\n", ls, ls / 44.1, lc, lc / 44.1);
    }
    {   // real-time path: no allocation by soxr after configure() / reset(), no shortfall, over long driver-like runs
        // with random block sizes (output: ASIO blocks pushed until a device request can be served; input: capture
        // packets pushed, everything producible drained)
#if defined WB_COUNT_ALLOC
        long allocs = 0;
#endif
        uint64_t shortf = 0, frames = 0;
        uint32_t rnd = 12345;
        auto rand = [&](uint32_t n) { rnd = rnd * 1664525u + 1013904223u; return (rnd >> 8) % n; };
        const double ap[][2] = {{44100, 48000}, {48000, 44100}, {96000, 44100}, {44100, 96000}, {48000, 96000}};
        for (auto& pr : ap) for (int out = 0; out < 2; ++out) {
            const int ch = 2;
            const long B = 32L << rand(6);                               // ASIO buffer 32..1024
            const size_t devBuf = 2048;
            fr::Converter c;
            if (out) c.configure(pr[0], pr[1], ch, (size_t)B + 64, devBuf + 64, Quality::Soxr);
            else c.configure(pr[0], pr[1], ch, devBuf + 64, (size_t)std::ceil((devBuf + 64) * pr[1] / pr[0]) + 64, Quality::Soxr);
            for (int run = 0; run < 2; ++run) {
                c.reset();                                               // host thread (start): may allocate
#if defined WB_COUNT_ALLOC
                g_countOn = true;
#endif
                const size_t total = (size_t)(pr[1] * (run ? 20.0 : 60.0));
                size_t made = 0;
                while (made < total) {
                    if (out) {
                        const size_t want = 16 + rand((uint32_t)devBuf - 16);
                        while (c.missingFor(want) > 0)
                            for (long i = 0; i < B; ++i) { float* f = c.pushSlot(); f[0] = (float)rand(1000) * 1e-3f; f[1] = -f[0]; }
                        c.produce(want, [&](size_t, int, float) {});
                        made += want;
                    } else {
                        const size_t pk = 16 + rand((uint32_t)devBuf - 16);
                        for (size_t i = 0; i < pk; ++i) { float* f = c.pushSlot(); f[0] = (float)rand(1000) * 1e-3f; f[1] = -f[0]; }
                        const size_t n = c.producible();
                        c.produce(n, [&](size_t, int, float) {});
                        made += n;
                    }
                }
#if defined WB_COUNT_ALLOC
                g_countOn = false;
#endif
                frames += made;
            }
            shortf += c.soxrShortfalls();
        }
#if defined WB_COUNT_ALLOC
        allocs = g_allocs;
        printf("    real-time path: %llu output frames in 20 runs (5 pairs, output and input patterns, two runs each with\n"
               "    reset() between them): %ld allocations by soxr, %llu missing frames\n",
               (unsigned long long)frames, allocs, (unsigned long long)shortf);
        Check(allocs == 0, "soxr: no allocation on the audio-thread path (push / missingFor / producible / produce)");
#else
        printf("    real-time path: %llu output frames, %llu missing frames; allocations NOT counted (build without\n"
               "    WB_COUNT_ALLOC; tests/run_all.sh counts them)\n", (unsigned long long)frames, (unsigned long long)shortf);
#endif
        Check(shortf == 0, "soxr: no promised frame missing in long runs with random block sizes");
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
