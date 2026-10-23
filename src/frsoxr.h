// WASAPI Driver for ASIO - soxr engine of the fixed-ratio converter (0.9.10, header-only, portable)
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
//   Wraps one instance of the SoX Resampler library (soxr 0.1.3, LGPL-2.1-or-later, vendored in third_party/soxr,
//   see its README-vendored.txt) so that fr::Converter (fixedratio.h) can use it as a third engine next to its own
//   polyphase sinc and Hermite: "soxr HQ" = soxr_quality_spec(SOXR_HQ, 0): 20-bit precision, linear phase, pass
//   band to 91.3 % of the lower Nyquist frequency with <= 0.01 dB ripple, stop band from 100 % (no aliasing),
//   engine "cr32" (single precision, plain C). The numbers measured offline are in tests/fixedratio_test.cpp and
//   ARCHITECTURE.txt section 60.
//
// How soxr maps onto the converter's contract (fixedratio.h: missingFor / producible / produce)
//   soxr pre-loads its filters with silence, so its output is time-aligned with its input: output frame m is the
//   input signal at m * num / den input frames (num/den = inRate/outRate in lowest terms), like the sinc engine.
//   What soxr adds is buffering: its first stage filters by FFT (overlap-save) and only computes a block when a
//   whole block of input has arrived, so an output frame becomes available a variable number of input frames
//   after its position. Let O(R) be the number of output frames soxr can deliver after R input frames. Then
//     look-ahead A = max over all output frames j of (first R with O(R) > j) - 1 - floor(j * num / den)
//   is the soxr counterpart of the sinc engine's ahead() = 64: output frame j can always be produced once
//   floor(j * num / den) + A + 1 input frames were pushed, and the converter promises exactly that (missingFor /
//   producible use the same formulas as for sinc, with ahead() = A). A depends only on the rate pair and the
//   recipe; O(R) does not depend on how the input was chunked (soxr computes every complete block it can, and the
//   output is drained completely after every piece).
//   Measure() measures A by pushing silence one frame at a time into a separate mono instance for 2 s of input
//   (kProbeSeconds; the pattern repeats much sooner for the standard rates, see the test) and adds kAheadMargin
//   frames (the largest growth seen after 2 s, over 64 s of probing, was 3 frames at 44.1 -> 192 kHz); the result
//   is cached per rate pair and process (review item 2, see LookupCache). Measured
//   look-ahead including the margin: 429 input frames at 44.1 -> 48 kHz (9.7 ms), 420 at 48 -> 44.1 kHz, 845 at
//   96 -> 48 kHz (8.8 ms); sinc: 64, 70, 128 (about 1.4 ms). The driver reports A to the host exactly like the
//   sinc look-ahead (sched.h), so the reported latencies stay exact; tests/fixedratio_test.cpp measures an
//   impulse through the whole model to prove it.
//   "soxr_delay()" is not used for the latency: it returns the output owed for the input consumed so far, which
//   varies with the block phase; A is its worst case expressed in input frames.
//   Recipe choice (ARCHITECTURE.txt section 60): HQ rather than VHQ (28-bit needs the double-precision engine,
//   cr64, which this build leaves out: more CPU and a longer filter; the float engine cannot deliver 28 bits), and
//   soxr_runtime_spec with log2_min_dft_size = log2_large_dft_size = 10: the smallest DFT length soxr allows for
//   each FFT stage's filter (1.7 to 3.4 times its length, rounded to a power of two, at least 1024 points;
//   1024 at 44.1 -> 48 kHz). soxr's default (up to 2^17) would add about 11 ms of look-ahead for ~15 % less
//   CPU.
//
// Real-time safety (where soxr allocates)
//   soxr_create allocates everything it needs (filter design, FFT tables, one FIFO per stage and channel) except
//   that its FIFOs (fifo.h) grow with realloc() when a write does not fit. soxr_process never allocates otherwise
//   (no malloc in the stage functions, the FFT or the (de)interleaving). soxr_clear frees and re-creates
//   everything, so it is never used. Measures taken:
//     - Pieces: Feed() passes the input to soxr_process in pieces of at most kPiece = 128 frames and collects the
//       output after each piece, so every FIFO write and occupancy is bounded by soxr's own block structure
//       (an FFT block plus the output of one small piece), whatever block sizes the driver uses.
//     - Warm-up: Open() (host thread, after Measure) pushes kWarmSeconds of silence in pieces of 1..kPiece frames
//       (fixed pseudo-random order), so soxr's blocks complete at every fill level and every FIFO grows to its
//       steady-state size on the host thread. (With constant 512-frame pieces and a 1 s warm-up the stress test
//       still found growth on the audio thread at 32 -> 192 kHz: the final FIFO's worst case, one piece of
//       output plus an FFT block, was never reached during that warm-up.)
//     - FIFO policy (third_party/soxr/soxr-config.h and fifo-headroom.c; soxr's own files are unchanged):
//       FIFO_MIN = 64 bytes makes a FIFO compact instead of grow whenever compacting makes room (soxr's default only
//       compacts after 16 KB were read, so a FIFO could grow again at an unlucky read offset; the test found that
//       case with the default), and a FIFO that grows gets twice the size it asked for, so a later need of up to
//       twice the warmed-up size is met in place.
//     - Restart() (host thread, the converter's reset() at start()): instead of soxr_clear, pushes silence until
//       every frame of the previous run has left the filters (2 A + 64 frames, rounded up to a multiple of num so
//       the output grid stays exact) and discards the output up to that point. The instance keeps its warmed-up
//       FIFOs across start/stop.
//     - The output buffer takes everything soxr can produce for a piece, so soxr's last FIFO never accumulates.
//   tests/fixedratio_test.cpp links the soxr objects with -Wl,--wrap=malloc,calloc,realloc and counts every
//   allocation soxr makes on the audio-thread path after configure() / reset(): zero over 52 million frames of
//   driver-like traffic with random block sizes (5 rate pairs, output and input patterns). The same counting over
//   every exact pair of the standard rates 22.05 .. 192 kHz with 1..4 channels and random buffer / request sizes
//   also found none (CODE-REVIEW.txt, 0.9.10).
//   soxr's FFT tables are global and shared by all instances (fft4g_cache.h, filter.c); without OpenMP their
//   reader/writer lock is a no-op. A table only grows (realloc) when an instance needs a longer FFT than any before,
//   which happens in soxr_create on the host thread; if another stream's audio thread used the table at that
//   moment it could read freed memory. EnsureFftTables() therefore grows the table once per process to 2^17
//   points before the first soxr_create (soxr's FFT length is at most 2^17 with these recipes: log2_large_dft_size
//   = 10 caps it at 1024 unless a filter is longer than 2^15 taps), so it never grows again.
//   soxr_create also reads a few SOXR_* environment variables (DFT sizes, threads) and calls time(); both are
//   harmless on the host thread, and A is measured with the same settings, so the latency stays exact.
//   No locks, no system calls, no allocation on the audio thread; loops are bounded by the chunk sizes.
//
// Threading
//   Measure, Open, Close, Restart: host thread, never while the audio thread uses the instance. Feed, Drain, Peek,
//   Consume: audio thread (or the host thread while no stream runs). Not copyable.
#pragma once
#include <algorithm>
#include <atomic>      // look-ahead cache (0.9.10 review)
#include <cstddef>
#include <cstdint>
#include <cstdio>      // snprintf (error text)
#include <cstring>     // memmove
#include <utility>     // std::pair (Probe)
#include <vector>
#include "soxr.h"

// soxr's single-precision real FFT with its global table cache (filter.c; "lsx_safe_rdft_f" in aliases.h). Called
// once by EnsureFftTables to grow the table before any instance exists.
extern "C" void _soxr_safe_rdft_f(int len, int type, float* d);

namespace fr {

class SoxrCore {
public:
    static constexpr double kProbeSeconds = 2.0;    // length of the look-ahead measurement (input time)
    static constexpr int    kAheadMargin = 16;      // frames added to the measured look-ahead (see Measure)
    static constexpr double kWarmSeconds = 2.0;     // warm-up with silence in pieces of 1..kPiece frames (input time)
    static constexpr int    kFftTableLog2 = 17;     // EnsureFftTables: 2^17-point table
    static constexpr size_t kPiece = 128;           // frames per soxr_process call (bounds soxr's FIFO writes)

    SoxrCore() = default;
    ~SoxrCore() { Close(); }
    SoxrCore(const SoxrCore&) = delete;
    SoxrCore& operator=(const SoxrCore&) = delete;

    // The recipe (one place, so the driver, the probe and the tests always agree).
    static soxr_quality_spec_t Quality() { return soxr_quality_spec(SOXR_HQ, 0); }
    static soxr_runtime_spec_t Runtime() {
        soxr_runtime_spec_t r = soxr_runtime_spec(1);   // one thread (no OpenMP in this build anyway)
        r.log2_min_dft_size = 10;
        r.log2_large_dft_size = 10;
        return r;
    }
    static const char* Name() { return "soxr HQ"; }

    // Grows soxr's global FFT table once per process (thread-safe static initialization), before the first
    // soxr_create. Host thread.
    static void EnsureFftTables() {
        static const bool done = [] {
            std::vector<float> z((size_t)1 << kFftTableLog2, 0.f);
            _soxr_safe_rdft_f(1 << kFftTableLog2, 1, z.data());
            return true;
        }();
        (void)done;
    }

    // Look-ahead measurement (pure apart from soxr's own allocations; host thread): a mono instance of the same
    // recipe gets `frames` frames of silence one at a time; after each frame every new output is drained.
    //   ahead:   max over output frames j of (input frames pushed when j appeared) - 1 - floor(j * num / den)
    //   surplus: max over time of (outputs available) - (outputs the converter may promise with that ahead), i.e.
    //            how far soxr runs ahead of the promise (sizes the output buffer).
    // False (and err set) if soxr refuses the pair.
    static bool Probe(long a, long b, long num, long den, size_t frames, int& ahead, size_t& surplus,
                      char* err, size_t errN) {
        EnsureFftTables();
        const soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        const soxr_quality_spec_t q = Quality();
        const soxr_runtime_spec_t rt = Runtime();
        soxr_error_t e = nullptr;
        soxr_t s = soxr_create((double)a, (double)b, 1, &e, &io, &q, &rt);
        if (!s) { if (err && errN) snprintf(err, errN, "%s", e ? e : "soxr_create failed"); return false; }
        std::vector<float> out(65536);
        std::vector<std::pair<uint64_t, uint64_t>> ev;   // (input frames pushed, outputs available) at each burst
        ev.reserve(4096);
        long long best = -(1LL << 40);
        uint64_t made = 0;
        const float zero = 0.f;
        for (uint64_t r = 1; r <= frames; ++r) {
            size_t idone = 0, odone = 0;
            e = soxr_process(s, &zero, 1, &idone, out.data(), out.size(), &odone);
            if (e || idone != 1) { if (err && errN) snprintf(err, errN, "%s", e ? e : "soxr_process consumed no input"); soxr_delete(s); return false; }
            if (odone) {
                const long long cand = (long long)r - 1 - (long long)(made * (uint64_t)num / (uint64_t)den);
                if (cand > best) best = cand;
                made += odone;
                ev.push_back({r, made});
            }
        }
        soxr_delete(s);
        if (made == 0 || best < 0) { if (err && errN) snprintf(err, errN, "soxr produced no output in the probe"); return false; }
        ahead = (int)best + kAheadMargin;
        // Outputs the converter promises after R inputs: j with floor(j*num/den) + ahead + 1 <= R, i.e.
        // j < (R - ahead) * den / num  ->  ceil((R - ahead) * den / num) frames (0 if R <= ahead).
        size_t sur = 0;
        for (const auto& p : ev) {
            const uint64_t prom = p.first > (uint64_t)ahead ? ((p.first - (uint64_t)ahead) * (uint64_t)den + (uint64_t)num - 1) / (uint64_t)num : 0;
            if (p.second > prom && (size_t)(p.second - prom) > sur) sur = (size_t)(p.second - prom);
        }
        surplus = sur;
        return true;
    }

    // Host thread, step 1. a/b: rates in Hz (integers), num/den = a/b in lowest terms. Closes any previous
    // instance and measures the look-ahead (Probe over kProbeSeconds of input). False (err() says why) if soxr
    // refuses the pair.
    // 0.9.10 (independent review, item 2): the result depends only on the rate pair (the recipe is fixed and the
    // SOXR_* environment of a process does not change), so it is kept in a small per-process cache and later
    // stream opens of the same pair skip the probe (tens of milliseconds at high rates). See LookupCache.
    bool Measure(long a, long b, long num, long den) {
        Close();
        err_[0] = 0;
        a_ = a; b_ = b; num_ = num; den_ = den;
        if (LookupCache(a, b, ahead_, surplus_)) { measured_ = true; return true; }
        const size_t probeFrames = std::max<size_t>((size_t)(kProbeSeconds * (double)a), 32768);
        measured_ = Probe(a, b, num, den, probeFrames, ahead_, surplus_, err_, sizeof err_);
        if (measured_) StoreCache(a, b, ahead_, surplus_);
        return measured_;
    }
    // Look-ahead cache: kCacheSlots slots per process, key = (a << 32) | b (never 0, rates are positive). Lock-free
    // and safe for several host threads (several driver instances): a writer claims a slot with fetch_add, fills
    // the values, then publishes the key with a release store; a reader that sees the key with an acquire load
    // also sees the values. Two threads measuring the same new pair at once store two equal entries (harmless,
    // the probe is deterministic). When the slots are used up, nothing more is cached (the probe simply runs).
    // Never touched by the audio thread.
    static constexpr int kCacheSlots = 32;
    struct CacheSlot { std::atomic<uint64_t> key{0}; std::atomic<int> ahead{0}; std::atomic<uint64_t> surplus{0}; };
    static CacheSlot* Cache() { static CacheSlot slots[kCacheSlots]; return slots; }
    static std::atomic<int>& CacheUsed() { static std::atomic<int> used{0}; return used; }
    static uint64_t CacheKey(long a, long b) { return ((uint64_t)(uint32_t)a << 32) | (uint64_t)(uint32_t)b; }
    static bool LookupCache(long a, long b, int& ahead, size_t& surplus) {
        const uint64_t k = CacheKey(a, b);
        const int n = std::min(CacheUsed().load(std::memory_order_acquire), kCacheSlots);
        for (int i = 0; i < n; ++i) {
            CacheSlot& c = Cache()[i];
            if (c.key.load(std::memory_order_acquire) == k) {
                ahead = c.ahead.load(std::memory_order_relaxed);
                surplus = (size_t)c.surplus.load(std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }
    static void StoreCache(long a, long b, int ahead, size_t surplus) {
        const int i = CacheUsed().fetch_add(1, std::memory_order_acq_rel);
        if (i >= kCacheSlots) return;
        CacheSlot& c = Cache()[i];
        c.ahead.store(ahead, std::memory_order_relaxed);
        c.surplus.store((uint64_t)surplus, std::memory_order_relaxed);
        c.key.store(CacheKey(a, b), std::memory_order_release);
    }
    // Host thread, step 2 (after Measure). ch channels, maxChunk: the largest number of input frames ever passed
    // to one Feed (the converter's ring capacity, sized from ahead()), maxOut: the largest output request.
    // Creates the instance, sizes the output buffer and warms the FIFOs up. The caller then calls Restart()
    // before the first Feed. False (err() says why) on any soxr error.
    bool Open(int ch, size_t maxChunk, size_t maxOut) {
        if (!measured_) return false;
        if (s_) soxr_delete(s_);
        s_ = nullptr;
        const long a = a_, b = b_, num = num_, den = den_;
        ch_ = ch;
        const soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
        const soxr_quality_spec_t q = Quality();
        const soxr_runtime_spec_t rt = Runtime();
        soxr_error_t e = nullptr;
        s_ = soxr_create((double)a, (double)b, (unsigned)ch, &e, &io, &q, &rt);
        if (!s_) { snprintf(err_, sizeof err_, "%s", e ? e : "soxr_create failed"); return false; }
        maxChunk_ = std::max<size_t>(maxChunk, 1);
        // Output buffer: what soxr may hold ready beyond the promise (surplus), one full request, and the output
        // of the largest chunk (so a Feed always has room for all of it and soxr consumes the whole chunk).
        outCap_ = surplus_ + maxOut + (size_t)(((uint64_t)maxChunk_ * (uint64_t)den + (uint64_t)num - 1) / (uint64_t)num) + 64;
        out_.assign(outCap_ * (size_t)ch, 0.f);
        zeros_.assign(kPiece * (size_t)ch, 0.f);
        rd_ = wr_ = 0; skip_ = 0; fed_ = 0; drained_ = 0; short_ = 0;
        // Warm-up (host thread): silence in pieces of 1..kPiece frames in a fixed pseudo-random order, everything
        // drained after each piece, so soxr's blocks complete at every possible fill level of the FIFOs and every
        // FIFO reaches its largest steady-state size here rather than on the audio thread.
        const uint64_t warm = std::max<uint64_t>((uint64_t)(kWarmSeconds * (double)a), 256 * (uint64_t)kPiece);
        uint32_t lcg = 0x2545F491u;
        while (fed_ < warm) {
            lcg = lcg * 1664525u + 1013904223u;
            if (!pushZeros(1 + (size_t)((lcg >> 8) % kPiece))) { Close(); return false; }
        }
        return true;
    }
    void Close() {
        if (s_) soxr_delete(s_);
        s_ = nullptr;
        measured_ = false;
        out_.clear(); out_.shrink_to_fit(); zeros_.clear(); zeros_.shrink_to_fit();
        rd_ = wr_ = 0; skip_ = 0;
    }
    bool   open() const { return s_ != nullptr; }
    int    ahead() const { return ahead_; }          // look-ahead in input frames (measured + margin)
    size_t surplus() const { return surplus_; }
    const char* err() const { return err_; }
    uint64_t shortfalls() const { return short_; }   // output frames that were promised but missing (0 expected)

    // Host thread, at the converter's reset(): flushes the previous run out of the filters with silence and
    // aligns the output grid: after this, the next output frame is the input signal at exactly the next input
    // frame pushed (relative position 0), like a fresh instance. No soxr_clear (see Real-time safety).
    void Restart() {
        if (!s_) return;
        rd_ = wr_ = 0; skip_ = 0;
        // T0: absolute input index where the new run starts; a multiple of num, so T0 * den / num is an integer
        // output index m0; at least 2 A + 64 frames after the last frame of the previous run.
        uint64_t t0 = fed_ + 2 * (uint64_t)ahead_ + 64;
        t0 = (t0 + (uint64_t)num_ - 1) / (uint64_t)num_ * (uint64_t)num_;
        for (;;) {
            while (fed_ < t0) { if (!pushZeros((size_t)std::min<uint64_t>(t0 - fed_, maxChunk_))) return; }
            const uint64_t m0 = t0 / (uint64_t)num_ * (uint64_t)den_;
            if (drained_ <= m0) { skip_ = m0 - drained_; break; }
            t0 += (uint64_t)num_;                         // soxr already ran past m0 (not expected): one more cycle
        }
        rd_ = wr_ = 0;                                    // the silence drained so far is not part of the new run
    }

    // Audio thread. Passes `frames` interleaved frames to soxr, in pieces of at most kPiece frames, and collects
    // every output after each piece. Returns the frames consumed (all of them unless the output buffer was full,
    // which the sizing prevents). Small pieces keep every soxr FIFO write small and independent of the driver's
    // block sizes, so the FIFO sizes reached during the warm-up (same pieces) are the steady-state sizes.
    size_t Feed(const float* in, size_t frames) {
        if (!s_) return 0;
        size_t used = 0;
        while (used < frames) {
            compact();
            const size_t piece = std::min(frames - used, kPiece);
            size_t idone = 0, odone = 0;
            soxr_error_t e = soxr_process(s_, in + used * (size_t)ch_, piece, &idone, &out_[wr_ * (size_t)ch_], outCap_ - wr_, &odone);
            if (e) break;
            landed(odone);
            fed_ += idone;
            used += idone;
            if (idone < piece) break;                    // output buffer full (not expected): the caller retries
        }
        return used;
    }
    // Audio thread. Collects output soxr computed but could not return (only if a Feed's output did not fit).
    // ilen = 0 with a non-null input is "no input" for soxr_process (a null input would mean end of stream).
    void Drain() {
        if (!s_) return;
        compact();
        if (wr_ >= outCap_) return;
        size_t idone = 0, odone = 0;
        const float dummy = 0.f;
        if (soxr_process(s_, &dummy, 0, &idone, &out_[wr_ * (size_t)ch_], outCap_ - wr_, &odone)) return;
        landed(odone);
    }
    // Output frames ready, their storage (interleaved, ch() floats per frame), and consuming them.
    size_t available() const { return wr_ - rd_; }
    const float* peek() const { return &out_[rd_ * (size_t)ch_]; }
    void consume(size_t n) { rd_ += std::min(n, wr_ - rd_); }
    void countShort(size_t n) { short_ += n; }

private:
    // Moves the unread frames to the front of the output buffer (bounded: at most surplus + one request).
    void compact() {
        if (rd_ == 0) return;
        const size_t n = wr_ - rd_;
        if (n) std::memmove(&out_[0], &out_[rd_ * (size_t)ch_], n * (size_t)ch_ * sizeof(float));
        rd_ = 0; wr_ = n;
    }
    // odone new frames were written at wr_: account for them and drop the ones that precede the run (Restart).
    void landed(size_t odone) {
        wr_ += odone;
        drained_ += odone;
        if (skip_) {
            const size_t d = (size_t)std::min<uint64_t>(skip_, (uint64_t)(wr_ - rd_));
            rd_ += d; skip_ -= d;
        }
    }
    // Host thread: n frames of silence in pieces of at most kPiece frames, all output discarded.
    bool pushZeros(size_t n) {
        compact();
        while (n) {
            const size_t piece = std::min(n, kPiece);
            size_t idone = 0, odone = 0;
            soxr_error_t e = soxr_process(s_, zeros_.data(), piece, &idone, &out_[wr_ * (size_t)ch_], outCap_ - wr_, &odone);
            if (e || !idone) { snprintf(err_, sizeof err_, "%s", e ? e : "soxr_process consumed no input"); return false; }
            fed_ += idone;
            drained_ += odone;                           // output not kept (wr_ unchanged)
            n -= idone;
        }
        return true;
    }

    soxr_t s_ = nullptr;
    bool measured_ = false;
    long a_ = 1, b_ = 1, num_ = 1, den_ = 1;
    int ch_ = 1, ahead_ = 0;
    size_t surplus_ = 0, maxChunk_ = 1, outCap_ = 0;
    std::vector<float> out_, zeros_;
    size_t rd_ = 0, wr_ = 0;           // output buffer: frames [rd_, wr_) are ready
    uint64_t skip_ = 0;                // output frames still to discard (Restart)
    uint64_t fed_ = 0, drained_ = 0;   // input frames given to soxr / output frames received, since Open
    uint64_t short_ = 0;
    char err_[96] = {0};
};

}  // namespace fr
