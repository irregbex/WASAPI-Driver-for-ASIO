// WASAPI Driver for ASIO - ASIOTime validation used by the test host (header-only, portable)
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
// Purpose (0.8.3)
//   WasapiDriverTest.exe checks the ASIOTime of every bufferSwitchTimeInfo call: not null, kSamplePositionValid,
//   kSystemTimeValid, kSampleRateValid and sampleRate equal to the rate the running driver instance reports
//   (getSampleRate just before start()). 0.8.2's tester compared against one global rate, which is wrong for the
//   fresh driver instances of T6: they start at the rate of config.ini (e.g. 44100) while the global was 48000
//   after T5, so every T6 callback counted as "bad sampleRate" (about 870 in the real report) although the driver
//   reported its own rate correctly. This header keeps the counting per test phase and per kind, plus the first
//   8 bad callbacks, so a report shows exactly where and what.
//
// Threading
//   check() runs on the driver's audio thread (lock-free: relaxed atomics; the bad-sample slots are written by the
//   thread that reserved them). beginPhase() and every reader run on the main thread only while no stream runs
//   (before start() / after stop(), which joins the audio thread).
//   That ordering (thread join / start) is what makes the plain fields (expected_, bad_, nPhases_) safe: the
//   audio thread only reads expected_ and writes its reserved bad_ slot, and nothing else touches them while it
//   runs. Do not call the readers while a stream is running except as rough progress information.
//
// Usage in hosttest.cpp
//   One global Checker. Each test phase calls beginPhase(), then setExpected(getSampleRate()) on the instance
//   under test; the host's bufferSwitchTimeInfo callback calls check(); the report prints per-phase counts and
//   the stored bad callbacks.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace tichk {

// Same values as AsioTimeInfoFlags in asio.h (hosttest.cpp static_asserts them).
constexpr unsigned long kSystemTimeValid = 1, kSamplePositionValid = 1 << 1, kSampleRateValid = 1 << 2;
// Kinds of defect; check() returns a bitmask with bit (1 << Kind) set for each one found. kNull excludes the
// others (a null ASIOTime has no fields to check).
enum Kind { kNull = 0, kNoPos, kNoTime, kBadRate, kKinds };
constexpr int kMaxPhases = 32, kMaxBad = 8;   // phase slots (phase 0 included); bad callbacks kept in detail

// One bad callback as stored for the report: phase, callback number within the phase, the flags and rate seen
// (0 for a null ASIOTime), the rate expected, and the Kind bitmask.
struct Bad { int phase; long callback; unsigned long flags; double rate, expected; unsigned kinds; };

class Checker {
public:
    Checker() { reset(); }
    // Main thread, no stream running: clears everything back to phase 0.
    void reset() {
        nPhases_ = 1; phase_.store(0);
        for (int p = 0; p < kMaxPhases; ++p) { expected_[p] = 0; calls_[p] = 0; for (auto& c : cnt_[p]) c = 0; }
        nBad_ = 0;
    }
    // Main thread, no stream running: starts a new phase (returns its index; the last slot is reused when full).
    int beginPhase(double expectedRate) {
        int p = nPhases_ < kMaxPhases ? nPhases_++ : kMaxPhases - 1;
        expected_[p] = expectedRate;
        phase_.store(p, std::memory_order_release);
        return p;
    }
    // Main thread, no stream running: the expected rate of the current phase (e.g. after getSampleRate).
    void setExpected(double r) { expected_[phase_.load(std::memory_order_relaxed)] = r; }

    // Audio thread. Returns the bitmask of kinds found (0 = valid).
    // The rate matches if it is within 0.5 Hz of the phase's expected rate (the negated <= also catches NaN).
    // Bad-slot reservation: nBad_.fetch_add gives each bad callback a unique index, so concurrent callers (if a
    // driver ever called back from two threads) never write the same slot; only the first kMaxBad are kept.
    unsigned check(bool isNull, unsigned long flags, double rate) {
        const int p = phase_.load(std::memory_order_acquire);
        const long n = calls_[p].fetch_add(1, std::memory_order_relaxed);
        unsigned k = 0;
        if (isNull) k |= 1u << kNull;
        else {
            if (!(flags & kSamplePositionValid)) k |= 1u << kNoPos;
            if (!(flags & kSystemTimeValid)) k |= 1u << kNoTime;
            if (!(flags & kSampleRateValid) || !(std::fabs(rate - expected_[p]) <= 0.5)) k |= 1u << kBadRate;
        }
        if (!k) return 0;
        for (int i = 0; i < kKinds; ++i) if (k & (1u << i)) cnt_[p][i].fetch_add(1, std::memory_order_relaxed);
        const long slot = nBad_.fetch_add(1, std::memory_order_relaxed);
        if (slot < kMaxBad) bad_[slot] = Bad{p, n, isNull ? 0ul : flags, isNull ? 0.0 : rate, expected_[p], k};
        return k;
    }

    // Readers (main thread, after stop()). p is a phase index < phases(); badCallbacks sums per kind, so a
    // callback with two defects counts twice there (calls() counts callbacks).
    long calls(int p) const { return calls_[p].load(); }
    long count(int p, Kind k) const { return cnt_[p][k].load(); }
    long badCallbacks(int p) const { long s = 0; for (int i = 0; i < kKinds; ++i) s += cnt_[p][i].load(); return s; }
    long total(Kind k) const { long s = 0; for (int p = 0; p < nPhases_; ++p) s += cnt_[p][k].load(); return s; }
    long totalAll() const { long s = 0; for (int i = 0; i < kKinds; ++i) s += total((Kind)i); return s; }
    long totalCalls() const { long s = 0; for (int p = 0; p < nPhases_; ++p) s += calls_[p].load(); return s; }
    double expected(int p) const { return expected_[p]; }
    int phases() const { return nPhases_; }
    int current() const { return phase_.load(); }
    long badStored() const { const long n = nBad_.load(); return n < kMaxBad ? n : kMaxBad; }
    long badSeen() const { return nBad_.load(); }
    const Bad& bad(int i) const { return bad_[i]; }

private:
    int nPhases_ = 1;                       // phase 0 = before any test (should stay empty)
    std::atomic<int> phase_{0};
    double expected_[kMaxPhases];
    std::atomic<long> calls_[kMaxPhases];
    std::atomic<long> cnt_[kMaxPhases][kKinds];
    std::atomic<long> nBad_{0};
    Bad bad_[kMaxBad];
};

}  // namespace tichk
