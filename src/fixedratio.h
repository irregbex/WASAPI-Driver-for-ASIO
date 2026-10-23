// WASAPI Driver for ASIO - fixed-ratio sample-rate converter (0.9.4, header-only, portable)
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
//   Converts between the ASIO rate and a device's mix rate when both run on the same clock (the device is the
//   clock master): output = ASIO rate -> device rate, input = device rate -> ASIO rate. The ratio is an exact
//   fraction (44100 -> 48000 = 147/160 input frames per output frame), stepped with integers, so there is no
//   drift, no accumulated rounding and no control loop: output frame j is the input signal at exactly
//   j * num / den input frames (num/den = inRate/outRate in lowest terms).
//   0.9.10: three engines. Quality "soxr" (only when chosen explicitly, FixedRatioEngine=Soxr): the SoX Resampler
//   library, recipe HQ (20-bit, linear phase), wrapped by SoxrCore in frsoxr.h; it filters by FFT blocks, so its look-ahead
//   is larger (429 input frames = 9.7 ms at 44.1 -> 48 kHz instead of 64 = 1.45 ms) but its stop band starts at
//   the lower Nyquist frequency itself, its pass band reaches 91.3 % with 0.004 dB ripple, and it measures about
//   132 dB SNR / -136 dB THD+N at 1 kHz (sinc: 115 / -122 dB; tests/fixedratio_test.cpp prints the full table).
//   Quality "sinc": polyphase windowed sinc (Kaiser, beta 10) with one exact coefficient set per phase (den
//   phases, computed when the stream opens), cut-off at 95 % of the lower Nyquist frequency (pass band to 90 %,
//   >= 100 dB stop band from 100 %: anti-aliasing when the rate goes down). 64 taps per side at the lower rate
//   (more input taps when decimating). Quality "hermite": 4-point Hermite, low CPU, no anti-aliasing.
//   Latency: the converter must see `ahead()` input frames after the position it evaluates (64 for sinc at equal
//   or rising rates, 2 for Hermite, the measured SoxrCore::ahead() for soxr); every engine is linear phase and
//   time-aligned, so the signal is not shifted.
//   Tested offline in tests/fixedratio_test.cpp (accounting, SNR, pass/stop band, CPU).
//
// Use
//   configure() (allocates; host thread), reset() before each start; then on the audio thread only:
//   pushSlot() appends one input frame; missingFor(n) = input frames still needed before n output frames can be
//   made; producible() = output frames available now; produce(n, out) makes n frames (out(i, channel, value)).
//   No allocation, no locks, bounded loops; denormals cannot build up (FIR on finite input, no feedback).
//   Choose() (host thread, before opening) applies the quality policy: sinc whenever the CPU budget allows
//   (kMacBudget multiply-accumulates per second), in both directions; above the budget the driver uses Hermite
//   (rate up) or lets Windows convert (rate down). 0.9.6 (KI-5): ResamplerQuality no longer plays a part here; it
//   only selects the drift resampler (sinc.h). Describe() formats the decision for the log.
//   0.9.10: Choose() takes the FixedRatioEngine preference (Engine): Auto (default) and Sinc are the 0.9.9 policy
//   above (low latency); Soxr tries soxr first (its cost, SoxrMacs, against the same budget), then the sinc rule;
//   Hermite uses Hermite
//   whenever the rate goes up (lowest latency and CPU) and the sinc rule when it goes down (Hermite cannot
//   anti-alias). The soxr engine allocates in configure() and reset() (host thread) only; see frsoxr.h for where
//   soxr allocates and why the audio thread never makes it allocate.
//
// Difference from sinc.h
//   sinc.h corrects drift: the ratio is ~1 and varies continuously, so a 256-phase table with linear
//   interpolation between phases is used and the cut-off sits at Nyquist. Here the ratio is a fixed rational
//   num/den, so the output phase only ever takes den distinct values (ph_ = 0..den-1): each gets its own exact
//   coefficient row (no interpolation between phases), and the cut-off is lowered for down-conversion.
//
// Position bookkeeping (all integers, no floating-point accumulation)
//   The evaluated input position of the next output frame is k_ + ph_ / den_ (k_ = integer input frame index,
//   ph_ = numerator of the fraction). Each output frame adds num_ to ph_ and carries whole frames into k_:
//     ph_ += num_; k_ += ph_ / den_; ph_ %= den_;
//   written_ counts input frames ever pushed (including hist() frames of initial silence). Indexes are uint64_t
//   and wrap into the power-of-two ring with mask_. The window for k_ is
//     sinc:    k_ - half_ + 1 .. k_ + half_       Hermite: k_ - 1 .. k_ + 2
//   so producing needs written_ >= k_ + ahead() + 1, and frames older than k_ - hist() may be overwritten.
//
// Mirror trick
//   The ring has taps_ extra frames after its end that duplicate frames 0..taps_-1. A window that starts near the
//   end of the ring therefore runs contiguously into the mirror, and the inner FIR loop needs no "& mask" per tap.
//   The copy is deferred (last_) because pushSlot() returns a pointer the caller fills after the call.
//
// Users (driver.cpp)
//   Main output: outConv converts ASIO rate -> device rate; the output thread runs ASIO callbacks
//   (bufferSwitch pushes the ASIO frames) until missingFor(frames) == 0, then produces the device frames into
//   outFifo. Inputs: captured device frames are pushed into each input's conv and producible() frames are
//   drained into the input FIFO at the ASIO rate. sched.h turns ahead() into latency figures for the host.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>      // snprintf (Describe, 0.9.6)
#include <vector>
#include "frsoxr.h"    // 0.9.10: soxr engine (SoX Resampler library, third_party/soxr)

namespace fr {

// Greatest common divisor (Euclid); reduces inRate/outRate to num/den in lowest terms (44100/48000 = 147/160).
inline long Gcd(long a, long b) { while (b) { const long t = a % b; a = b; b = t; } return a < 0 ? -a : a; }

// Modified Bessel I0 for the Kaiser window (same series as sinc.h, tighter stop criterion for beta = 10).
inline double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double y = x * x / 4.0;
    for (int k = 1; k < 200; ++k) { term *= y / ((double)k * k); sum += term; if (term < 1e-17 * sum) break; }
    return sum;
}

constexpr int    kHalfUnit = 64;       // taps per side at the lower rate (sinc)
constexpr double kCutoff   = 0.95;     // cut-off relative to the lower Nyquist frequency
constexpr double kBeta     = 10.0;     // Kaiser beta (~100 dB)
constexpr long   kMaxDen   = 4096;     // largest phase count accepted (all standard rate pairs are far below)
constexpr int    kMaxTaps  = 1024;     // longest filter (decimation by more than 4.3 is refused)
constexpr double kMacBudget = 150e6;   // multiply-accumulates per second above which the driver avoids sinc
// 0.9.10: soxr's cost expressed in the same unit (multiply-accumulates of the sinc engine that take the same CPU
// time), per channel and per input + output frame. soxr runs an FFT stage at one rate and a short polyphase stage
// at the other, so its work grows with both rates. Measured on x64 (tests/fixedratio_test.cpp, "soxr cost" lines):
// 13 to 38 per channel and input + output frame depending on the pair (48 -> 44.1 kHz the highest, 96 -> 48 kHz
// the lowest) on the development machine, and 52.1 for 48 -> 44.1 kHz on another x64 machine (independent review
// of 0.9.10: the ratio of soxr's FFT work to sinc's multiply-accumulates differs between CPUs). 64 is a
// conservative round value (about 1.2 x the worst measured), so the budget keeps its meaning (150 M MAC/s of sinc
// work, roughly 10 % of one core). 0.9.10 shipped with 48 during development; the review raised it.
constexpr double kSoxrMacsPerFrame = 64.0;

// Filter length of the sinc converter for a rate pair (without building it); 0 if the pair is not representable.
//   Decimating (out < in) stretches the filter by in/out so that it still spans kHalfUnit periods of the lower
//   rate: e.g. 96000 -> 48000 gives 2 * 128 = 256 taps. The same formula is used by configure().
inline int SincTaps(double inRate, double outRate) {
    const long a = (long)std::lround(inRate), b = (long)std::lround(outRate);
    if (a <= 0 || b <= 0) return 0;
    const long g = Gcd(a, b);
    if (b / g > kMaxDen || a / g > 64 * kMaxDen) return 0;
    const double down = (double)b / (double)a;
    const int t = 2 * (int)std::ceil(kHalfUnit / (down < 1.0 ? down : 1.0));
    return t > kMaxTaps ? 0 : t;
}
// Sinc cost: taps x channels x output frames per second (compared with kMacBudget).
inline double SincMacs(double inRate, double outRate, int channels) {
    return (double)SincTaps(inRate, outRate) * (double)channels * outRate;
}
// Quality policy (host thread, before opening). Pure function, tested offline (tests/fixedratio_test.cpp).
//   - No exact ratio (SincTaps == 0): None, the caller lets Windows convert.
//   - Sinc cost within the budget: Sinc, whatever the direction. 0.9.6 (KI-5): before, an UP-conversion used sinc
//     only with ResamplerQuality=Sinc and Hermite otherwise (the default), so the common 44.1 -> 48 kHz case
//     (12 M MAC/s for stereo, far below the 150 M budget) ran on Hermite (~89 dB SNR at 1 kHz instead of ~115 dB).
//     ResamplerQuality now only selects the drift resampler; the fixed-ratio converter is never "Hermite by
//     choice". The latency cost of sinc (ahead() = 64 input frames instead of 2) is reported to the host through
//     sched.h like before, so the reported latencies stay exact.
//   - Over the budget: an up-conversion falls back to Hermite (cheap, no anti-aliasing needed when the rate goes
//     up); a down-conversion is not done by the driver at all (Hermite has no anti-aliasing): None.
// 0.9.10: Quality::Soxr (the SoX Resampler library, frsoxr.h) and the FixedRatioEngine preference:
//   Engine::Auto (default) and Engine::Sinc: the sinc rule above, i.e. exactly the 0.9.9 behaviour (lowest latency
//     with full anti-aliasing). Auto never picks soxr: the default stays low-latency (maintainer's decision; soxr
//     adds about 8-10 ms per converted direction at 44.1-192 kHz, more at 16-32 kHz). Auto is a separate value so that a later version may refine the
//     automatic choice without changing what an explicit "Sinc" means.
//   Engine::Soxr (explicit choice only): soxr when SoxrMacs fits the budget, otherwise the sinc rule above (the
//     driver logs that soxr was over the budget, see Describe).
//   Engine::Hermite: up-conversion always Hermite (lowest latency and CPU, ~89 dB SNR at 1 kHz); a down-conversion
//     still follows the sinc rule (Hermite has no anti-aliasing filter, so it is never used to lower the rate).
//   The exact-ratio condition (SincTaps > 0: den <= kMaxDen, decimation <= 4.3) is the same for every engine, so
//   a pair is either converted by the driver with every preference or by Windows with every preference.
enum class Quality { None, Hermite, Sinc, Soxr };
enum class Engine { Auto = 0, Soxr = 1, Sinc = 2, Hermite = 3 };
// soxr's cost (kSoxrMacsPerFrame x channels x (input rate + output rate)); 0 if the pair is not representable.
inline double SoxrMacs(double inRate, double outRate, int channels) {
    if (SincTaps(inRate, outRate) == 0) return 0.0;
    return kSoxrMacsPerFrame * (double)channels * (inRate + outRate);
}
inline Quality Choose(double inRate, double outRate, int channels, double budget = kMacBudget, Engine pref = Engine::Auto) {
    if (SincTaps(inRate, outRate) == 0) return Quality::None;            // no exact ratio
    if (pref == Engine::Hermite && outRate > inRate) return Quality::Hermite;
    if (pref == Engine::Soxr && SoxrMacs(inRate, outRate, channels) <= budget) return Quality::Soxr;
    if (SincMacs(inRate, outRate, channels) <= budget) return Quality::Sinc;
    return outRate < inRate ? Quality::None : Quality::Hermite;          // over budget: Windows (down) / Hermite (up)
}

// 0.9.10 (independent review, item 6): what to use when soxr was chosen but cannot be configured (soxr refused
// the pair; not seen in the tests). The sinc rule with the SAME remaining budget, so a fallback never exceeds the
// budget the soxr choice was checked against: sinc if it fits, otherwise Hermite (rate up) or Windows (None, rate
// down). soxr is cheaper than sinc in the cost model, so "soxr fitted but sinc does not" is possible. Pure.
inline Quality FallbackFromSoxr(double inRate, double outRate, int channels, double budget) {
    return Choose(inRate, outRate, channels, budget, Engine::Sinc);
}

// 0.9.6 (independent review R2): cost in multiply-accumulates per second of a converter of quality q (Sinc:
// SincMacs; Hermite: 4 points x channels x output rate; None: 0, Windows converts). The driver subtracts the cost
// of every converter it configures from ONE budget per stream open (output first, then the main input, then the
// extra inputs), and passes the remaining budget to Choose, so that all converters together stay within
// kMacBudget (before, each converter could take the whole 150 M on its own: up to 5 x 150 M). Pure.
// 0.9.10: Soxr: SoxrMacs.
inline double Cost(Quality q, double inRate, double outRate, int channels) {
    if (q == Quality::Soxr) return SoxrMacs(inRate, outRate, channels);
    if (q == Quality::Sinc) return SincMacs(inRate, outRate, channels);
    if (q == Quality::Hermite) return 4.0 * (double)channels * outRate;
    return 0.0;
}

// 0.9.6 (KI-5): the decision as log text (English, ASCII), written to buf (always NUL-terminated if n > 0);
// returns snprintf's result. driver.cpp logs "<tag>: rate conversion <in> -> <out> Hz, <ch> channels: " + this.
// `budget` is the budget passed to Choose: since review R2 the REMAINING budget of this stream open (150 M for
// the output, less for the inputs once the output converter has taken its share).
//   Sinc:    "sinc (12 M MAC/s, budget 150 M)"
//   Hermite: "Hermite (sinc would cost 197 M MAC/s, over the budget of 150 M)"
//   None:    "Windows converts (no exact ratio)" or "Windows converts (sinc would cost ... over the budget ...)"
// Pure (no allocation), so the exact wording is checked by tests/fixedratio_test.cpp.
// 0.9.10: `pref` is the FixedRatioEngine preference given to Choose. New and changed texts:
//   Soxr:    "soxr HQ (12 M MAC/s equivalent, budget 150 M)"
//   FixedRatioEngine=Soxr but soxr over the budget (exact ratio): the usual text of the fallback followed by
//            "; soxr HQ not used: it would cost 215 M MAC/s equivalent, over the budget of 150 M"
//   Hermite chosen in the settings: "Hermite (FixedRatioEngine=Hermite)"
//   Auto and Sinc give exactly the 0.9.6 / 0.9.9 texts.
inline int Describe(char* buf, size_t n, Quality q, double inRate, double outRate, int channels,
                    double budget = kMacBudget, Engine pref = Engine::Auto) {
    const double m = SincMacs(inRate, outRate, channels) / 1e6, b = budget / 1e6;
    const double xm = SoxrMacs(inRate, outRate, channels) / 1e6;
    if (q == Quality::Soxr) return snprintf(buf, n, "%s (%.0f M MAC/s equivalent, budget %.0f M)", SoxrCore::Name(), xm, b);
    if (SincTaps(inRate, outRate) == 0) return snprintf(buf, n, "Windows converts (no exact ratio)");
    // soxr chosen explicitly but over the budget: the fallback's text plus why soxr was not used
    char sx[96] = "";
    if (pref == Engine::Soxr)
        snprintf(sx, sizeof sx, "; %s not used: it would cost %.0f M MAC/s equivalent, over the budget of %.0f M", SoxrCore::Name(), xm, b);
    if (q == Quality::Sinc) return snprintf(buf, n, "sinc (%.0f M MAC/s, budget %.0f M)%s", m, b, sx);
    if (q == Quality::Hermite && pref == Engine::Hermite && outRate > inRate) return snprintf(buf, n, "Hermite (FixedRatioEngine=Hermite)");
    if (q == Quality::Hermite) return snprintf(buf, n, "Hermite (sinc would cost %.0f M MAC/s, over the budget of %.0f M)%s", m, b, sx);
    return snprintf(buf, n, "Windows converts (sinc would cost %.0f M MAC/s, over the budget of %.0f M)%s", m, b, sx);
}

class Converter {
public:
    Converter() = default;
    Converter(const Converter&) = delete;              // 0.9.10: owns a soxr instance (SoxrCore)
    Converter& operator=(const Converter&) = delete;
    // inRate/outRate in Hz (integers), channels <= 32. maxPush / maxOut: largest number of input frames pushed
    // and output frames requested between two produce() calls (sizes the ring). False if the pair cannot be
    // represented (den > kMaxDen) or the arguments are invalid.
    // Host thread only (allocates the table and the ring). Ring capacity: a power of two >= 2 x (maxPush + taps +
    // input frames for maxOut outputs + 16), so a push burst plus the read window never overwrite unread frames.
    // fc_ is the cut-off relative to the input Nyquist: 0.95 when up-converting, 0.95 * out/in when decimating.
    // 0.9.10: `q` selects the engine (Sinc, Hermite or Soxr; None is refused). Soxr: the ring only stages the
    // frames pushed since the last produce(); SoxrCore measures the look-ahead (taps_ = 2 x ahead, the window it
    // stands for) and warms the soxr instance up (frsoxr.h). If soxr refuses the pair or fails, the converter
    // falls back to Sinc and soxrError() keeps soxr's message (quality() then says Sinc). With soxr it takes about
    // 10-20 ms for stereo and 150-300 ms for 32 channels (warm-up; the look-ahead probe runs once per rate pair and
    // process, SoxrCore caches it): host thread, at stream open only.
    bool configure(double inRate, double outRate, int channels, size_t maxPush, size_t maxOut, Quality q) {
        active_ = false;
        soxr_.Close();
        soxrErr_[0] = 0;
        if (q == Quality::None) return false;
        const long a = (long)std::lround(inRate), b = (long)std::lround(outRate);
        if (a <= 0 || b <= 0 || channels <= 0 || channels > 32) return false;
        const long g = Gcd(a, b);
        num_ = a / g; den_ = b / g;
        if (den_ > kMaxDen || num_ > 64 * kMaxDen) return false;
        ch_ = channels; q_ = q; sinc_ = q == Quality::Sinc;
        const double down = (double)b / (double)a;                 // < 1: decimation (cut-off moves down)
        if (q_ == Quality::Soxr) {
            // Look-ahead first (it sizes the ring), then the instance. The ring stages at most one produce() worth
            // of input: the same bound as for sinc with 2 x look-ahead in place of the taps; its capacity is the
            // largest chunk SoxrCore is ever fed (the warm-up uses it).
            if (soxr_.Measure(a, b, num_, den_)) {
                half_ = soxr_.ahead(); taps_ = 2 * half_;
                size_t need = maxPush + (size_t)taps_ + (size_t)std::ceil((double)maxOut * num_ / den_) + 16, cap = 64;
                while (cap < need * 2) cap <<= 1;
                if (soxr_.Open(ch_, cap, maxOut)) {
                    ring_.assign(cap * (size_t)ch_, 0.f);           // staging only: no mirror needed
                    mask_ = cap - 1;
                    reset();
                    active_ = true;
                    return true;
                }
            }
            snprintf(soxrErr_, sizeof soxrErr_, "%s", soxr_.err());   // soxr refused or failed: sinc instead
            soxr_.Close();
            q_ = Quality::Sinc; sinc_ = true;
        }
        fc_ = kCutoff * (down < 1.0 ? down : 1.0);                  // relative to the input Nyquist frequency
        half_ = sinc_ ? (int)std::ceil(kHalfUnit / (down < 1.0 ? down : 1.0)) : 2;
        taps_ = sinc_ ? 2 * half_ : 4;
        if (taps_ > kMaxTaps) return false;
        if (sinc_) buildTable();
        size_t need = maxPush + (size_t)taps_ + (size_t)std::ceil((double)maxOut * num_ / den_) + 16, cap = 64;
        while (cap < need * 2) cap <<= 1;
        ring_.assign((cap + (size_t)taps_) * (size_t)ch_, 0.f);   // + mirror of the first taps_ frames
        mask_ = cap - 1;
        reset();
        active_ = true;
        return true;
    }
    // 0.9.4 form: sinc (true) or Hermite (false).
    bool configure(double inRate, double outRate, int channels, size_t maxPush, size_t maxOut, bool useSinc) {
        return configure(inRate, outRate, channels, maxPush, maxOut, useSinc ? Quality::Sinc : Quality::Hermite);
    }
    // Back to the start state: `ahead()` frames of silence as history, first real input frame next.
    // (Precisely: written_ = k_ = hist() = ahead() - 1 for sinc, 1 for Hermite; the ring is zeroed, so the taps
    // left of the first real frame read silence.) Not real-time critical, but allocation-free.
    // 0.9.10, soxr: written_ = k_ = 0 (soxr keeps its own history) and SoxrCore::Restart flushes the previous run
    // with silence and re-aligns the output grid (host thread; soxr_process on silence, see frsoxr.h).
    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.f);
        last_ = kNone;
        written_ = (uint64_t)hist();
        k_ = (uint64_t)hist(); ph_ = 0;
        fedTo_ = written_;
        if (q_ == Quality::Soxr) soxr_.Restart();
    }
    bool   active() const { return active_; }
    long   num() const { return num_; }
    long   den() const { return den_; }
    int    taps() const { return taps_; }
    bool   sinc() const { return sinc_; }
    Quality quality() const { return q_; }            // 0.9.10: the engine in use (Sinc after a soxr fallback)
    const char* soxrError() const { return soxrErr_; }   // why soxr was not used ("" if it was, or not asked for)
    uint64_t soxrShortfalls() const { return soxr_.shortfalls(); }   // promised soxr frames that were missing (0)
    // Engine name for the log ("soxr HQ", "sinc", "Hermite").
    const char* engineName() const { return q_ == Quality::Soxr ? SoxrCore::Name() : sinc_ ? "sinc" : "Hermite"; }
    // Input frames the converter must hold after the evaluated position (its latency, in input frames).
    // soxr: the measured look-ahead (half_ holds it).
    int    ahead() const { return q_ == Quality::Soxr ? half_ : sinc_ ? half_ : 2; }
    // Frames held: written but not yet behind the read window.
    size_t held() const { return (size_t)(written_ - (k_ - (uint64_t)hist())); }
    size_t capacity() const { return mask_ + 1; }
    uint64_t written() const { return written_; }

    // The frame is written by the caller after this returns; its mirror copy (frames 0..taps-1 are repeated after
    // the ring so a filter window is always contiguous) is made on the next push or produce.
    float* pushSlot() {
        if (q_ == Quality::Soxr) { const size_t i = (size_t)(written_ & mask_); ++written_; return &ring_[i * ch_]; }
        flushMirror();
        const size_t i = (size_t)(written_ & mask_);
        last_ = i; ++written_;
        return &ring_[i * ch_];
    }
    // Input frames still missing before n output frames can be produced.
    // The last of the n outputs is evaluated at input frame kl = k_ + (ph_ + (n-1) * num_) / den_; its window ends
    // at kl + ahead(), so written_ must reach kl + ahead() + 1.
    size_t missingFor(size_t n) const {
        if (!n) return 0;
        const uint64_t kl = k_ + (uint64_t)((ph_ + (uint64_t)(n - 1) * (uint64_t)num_) / (uint64_t)den_);
        const uint64_t need = kl + (uint64_t)ahead() + 1;
        return need > written_ ? (size_t)(need - written_) : 0;
    }
    // Output frames that can be produced now. Inverse of missingFor: the largest n whose last evaluated frame kl
    // satisfies kl <= maxK = written_ - ahead() - 1, i.e. (ph_ + (n-1) * num_) < (maxK - k_ + 1) * den_.
    size_t producible() const {
        const uint64_t a = (uint64_t)ahead();
        if (written_ < k_ + a + 1) return 0;
        const uint64_t maxK = written_ - a - 1;
        const uint64_t lim = (maxK - k_ + 1) * (uint64_t)den_ - 1 - (uint64_t)ph_;
        return (size_t)(lim / (uint64_t)num_ + 1);
    }
    // Produces n output frames (the caller checked missingFor(n) == 0). out(i, channel, value).
    template <class Out> void produce(size_t n, Out out) {
        if (q_ == Quality::Soxr) { produceSoxr(n, out); return; }
        flushMirror();
        float acc[32];
        for (size_t i = 0; i < n; ++i) {
            if (sinc_) {
                // Contiguous window (mirror): frames first..first+taps-1, channels interleaved; the inner loop over
                // the channels has a fixed stride and no index masking (same summation order as a per-channel loop).
                const float* cf = &table_[(size_t)ph_ * (size_t)taps_];
                const uint64_t first = k_ - (uint64_t)half_ + 1;
                const float* p = &ring_[(size_t)(first & mask_) * ch_];
                if (ch_ == 2) {
                    float a0 = 0.f, a1 = 0.f;
                    for (int m = 0; m < taps_; ++m, p += 2) { a0 += cf[m] * p[0]; a1 += cf[m] * p[1]; }
                    out(i, 0, a0); out(i, 1, a1);
                } else {
                    for (int c = 0; c < ch_; ++c) acc[c] = 0.f;
                    for (int m = 0; m < taps_; ++m, p += ch_) { const float w = cf[m]; for (int c = 0; c < ch_; ++c) acc[c] += w * p[c]; }
                    for (int c = 0; c < ch_; ++c) out(i, c, acc[c]);
                }
            } else {
                // Hermite (Catmull-Rom) between x[k] and x[k+1] at t = ph/den, from x[k-1..k+2]; same formula as
                // Hermite() in driver.cpp. Interpolates only: no low-pass, hence aliasing if the rate went down.
                const float t = (float)ph_ / (float)den_;
                const float* a = &ring_[(size_t)((k_ - 1) & mask_) * ch_];
                const float* b = &ring_[(size_t)(k_ & mask_) * ch_];
                const float* c2 = &ring_[(size_t)((k_ + 1) & mask_) * ch_];
                const float* d = &ring_[(size_t)((k_ + 2) & mask_) * ch_];
                for (int c = 0; c < ch_; ++c) {
                    const float c1 = 0.5f * (c2[c] - a[c]);
                    const float q2 = a[c] - 2.5f * b[c] + 2.f * c2[c] - 0.5f * d[c];
                    const float q3 = 0.5f * (d[c] - a[c]) + 1.5f * (b[c] - c2[c]);
                    out(i, c, ((q3 * t + q2) * t + c1) * t + b[c]);
                }
            }
            ph_ += num_;
            k_ += (uint64_t)(ph_ / den_);
            ph_ %= den_;
        }
    }
    // Advances n output frames without computing them (offline scheduling simulation only).
    void advance(size_t n) {
        const uint64_t t = (uint64_t)ph_ + (uint64_t)n * (uint64_t)num_;
        k_ += t / (uint64_t)den_; ph_ = (long)(t % (uint64_t)den_);
    }
    // Pushes n frames without writing them (offline scheduling simulation only).
    void skipPush(size_t n) { written_ += n; }
    // Exact input position of the next output frame relative to the next input frame to be pushed (<= 0 when
    // the converter is starved); used by the tests.
    double positionLag() const { return (double)k_ + (double)ph_ / den_ - (double)written_; }

private:
    static constexpr size_t kNone = (size_t)-1;
    // Frames kept to the left of k_ (the window's left part): half_ - 1 for sinc, 1 for Hermite, 0 for soxr.
    int hist() const { return q_ == Quality::Soxr ? 0 : sinc_ ? half_ - 1 : 1; }
    // 0.9.10, soxr engine (audio thread). Gives soxr every frame pushed since the last call (the ring part
    // [fedTo_, written_), in at most two contiguous pieces), then hands out n frames from SoxrCore's output buffer.
    // The caller checked missingFor(n) == 0, so by the definition of the look-ahead soxr has produced them; if it
    // has not (never seen in the tests) the missing frames are silence and counted (soxrShortfalls), so the
    // stream timing stays exact whatever happens.
    template <class Out> void produceSoxr(size_t n, Out out) {
        while (fedTo_ < written_) {
            const size_t at = (size_t)(fedTo_ & mask_);
            const size_t len = (size_t)std::min<uint64_t>(written_ - fedTo_, (uint64_t)(mask_ + 1 - at));
            const size_t used = soxr_.Feed(&ring_[at * ch_], len);
            if (!used) break;
            fedTo_ += used;
        }
        if (soxr_.available() < n) soxr_.Drain();
        const size_t have = std::min(n, soxr_.available());
        const float* src = soxr_.peek();
        for (size_t i = 0; i < have; ++i)
            for (int c = 0; c < ch_; ++c) out(i, c, src[i * (size_t)ch_ + (size_t)c]);
        for (size_t i = have; i < n; ++i)
            for (int c = 0; c < ch_; ++c) out(i, c, 0.f);
        soxr_.consume(have);
        if (have < n) soxr_.countShort(n - have);
        advance(n);
    }
    // Copies the frame pushed last into the mirror area if it lies in the first taps_ ring slots.
    void flushMirror() {
        if (last_ != kNone && last_ < (size_t)taps_)
            for (int c = 0; c < ch_; ++c) ring_[(last_ + mask_ + 1) * ch_ + c] = ring_[last_ * ch_ + c];
        last_ = kNone;
    }
    // One row per phase p = 0..den-1 (fraction t = p/den): h(d) = sinc(fc * d) * kaiser(d / half), i.e. a
    // low-pass at fc times the input Nyquist, normalized to unity DC gain per row (the normalization also absorbs
    // the fc pass-band gain factor). Row size taps_; table size den_ * taps_ floats (e.g. 160 x 128 at 44.1 -> 48k).
    void buildTable() {
        const double pi = 3.14159265358979323846, i0b = BesselI0(kBeta);
        table_.assign((size_t)den_ * (size_t)taps_, 0.f);
        std::vector<double> c((size_t)taps_);
        for (long p = 0; p < den_; ++p) {
            const double t = (double)p / (double)den_;
            double sum = 0.0;
            for (int m = 0; m < taps_; ++m) {
                const double d = t + half_ - 1 - m;                  // distance to the evaluated point (input frames)
                const double x = pi * fc_ * d;
                const double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x;
                const double r = d / half_;
                const double w = std::fabs(r) >= 1.0 ? 0.0 : BesselI0(kBeta * std::sqrt(1.0 - r * r)) / i0b;
                c[(size_t)m] = s * w; sum += c[(size_t)m];
            }
            for (int m = 0; m < taps_; ++m) table_[(size_t)p * taps_ + m] = (float)(c[(size_t)m] / sum);
        }
    }
    bool active_ = false, sinc_ = true;
    Quality q_ = Quality::Sinc;         // 0.9.10: engine in use
    SoxrCore soxr_;                     // 0.9.10: the soxr instance (open only with Quality::Soxr)
    uint64_t fedTo_ = 0;                // 0.9.10, soxr: ring frames [fedTo_, written_) not yet given to soxr
    char soxrErr_[96] = {0};
    int ch_ = 2, half_ = 2, taps_ = 4;
    long num_ = 1, den_ = 1;
    double fc_ = 1.0;
    std::vector<float> table_, ring_;
    size_t mask_ = 0;
    uint64_t written_ = 0, k_ = 0;
    long ph_ = 0;
    size_t last_ = (size_t)-1;          // ring index of the frame pushed last (mirror pending)
};

}  // namespace fr
