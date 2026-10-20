// WASAPI Driver for ASIO - offline test of the test host's ASIOTime validation (src/tichk.h)
// (Linux: g++ -O2 -std=c++17 -pthread tests/timeinfo_test.cpp -o tt && ./tt)
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
//   Reproduces, without Windows, the 0.8.2 report "timeInfo calls 12801, invalid fields 870" with T2 at 0: the
//   tests run at 44100 (config.ini), T5 ends by setting 48000 on the first driver instance, and T6 then loads 10
//   fresh instances that start at 44100 again (config.ini). Checked:
//     1. old rule (one global expected rate = 48000 after T5): every T6 callback counts as "bad sampleRate";
//     2. new rule (expected = getSampleRate() of the running instance, per phase): T6 has no invalid field;
//     3. each kind is counted separately (null, no samplePosition, no systemTime, bad / missing sampleRate);
//     4. the first 8 invalid callbacks are kept with phase, callback index, flags, reported and expected rate,
//        later ones are only counted;
//     5. the counting is done on a second thread (like the driver's audio thread) and read after join.
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/tichk.h"
#include <cstdio>
#include <thread>

using namespace tichk;
static int g_fail = 0;
static void Check(bool ok, const char* what, long v) {
    printf("  %-62s %s  (%ld)\n", what, ok ? "OK" : "FAILED", v);
    if (!ok) ++g_fail;
}
static const unsigned long kAll = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;

// One stream run on a separate thread: n callbacks reporting `reported` with full flags.
static void Run(Checker& c, long n, double reported) {
    std::thread t([&] { for (long i = 0; i < n; ++i) c.check(false, kAll, reported); });
    t.join();
}

int main() {
    printf("ASIOTime validation offline test\n");
    const long t6PerCycle = 87;                       // ~0.5 s at 44100 Hz with a 256-frame buffer
    {   // 1) old rule: a single global rate, 48000 after T5
        Checker c;
        c.beginPhase(44100); Run(c, 1700, 44100);     // T2 at the config rate: fine
        c.beginPhase(48000); Run(c, 560, 48000);      // T5 48000
        const int t6 = c.beginPhase(48000);           // global rate still 48000
        for (int i = 0; i < 10; ++i) Run(c, t6PerCycle, 44100);   // fresh instances at 44100
        Check(c.count(1, kBadRate) == 0, "old rule: T2 has no invalid field", c.count(1, kBadRate));
        Check(c.count(t6, kBadRate) == 10 * t6PerCycle, "old rule: every T6 callback is 'bad sampleRate'", c.count(t6, kBadRate));
    }
    {   // 2) new rule: expected rate = the running instance's getSampleRate, per phase
        Checker c;
        c.beginPhase(0); c.setExpected(44100); Run(c, 1700, 44100);
        c.beginPhase(0); c.setExpected(48000); Run(c, 560, 48000);
        const int t6 = c.beginPhase(0);
        for (int i = 0; i < 10; ++i) { c.setExpected(44100); Run(c, t6PerCycle, 44100); }
        Check(c.badCallbacks(t6) == 0 && c.totalAll() == 0, "new rule: no invalid field anywhere", c.totalAll());
        Check(c.calls(t6) == 10 * t6PerCycle, "new rule: T6 calls counted in its phase", c.calls(t6));
    }
    {   // 3) and 4) kinds and the first 8 samples
        Checker c;
        const int p = c.beginPhase(48000);
        std::thread t([&] {
            c.check(true, 0, 0);                                            // null
            c.check(false, kSystemTimeValid | kSampleRateValid, 48000);     // no samplePosition
            c.check(false, kSamplePositionValid | kSampleRateValid, 48000); // no systemTime
            c.check(false, kSystemTimeValid | kSamplePositionValid, 48000); // sampleRate flag missing
            c.check(false, kAll, 44100);                                    // wrong rate
            for (int i = 0; i < 20; ++i) c.check(false, kAll, 96000);       // more wrong rates
            for (int i = 0; i < 30; ++i) c.check(false, kAll, 48000);       // valid
        });
        t.join();
        Check(c.count(p, kNull) == 1, "null counted", c.count(p, kNull));
        Check(c.count(p, kNoPos) == 1, "missing samplePosition counted", c.count(p, kNoPos));
        Check(c.count(p, kNoTime) == 1, "missing systemTime counted", c.count(p, kNoTime));
        Check(c.count(p, kBadRate) == 22, "missing flag or wrong sampleRate counted", c.count(p, kBadRate));
        Check(c.calls(p) == 55, "all calls counted", c.calls(p));
        Check(c.badStored() == 8 && c.badSeen() == 25, "first 8 invalid callbacks kept, all counted", c.badSeen());
        const Bad& b4 = c.bad(4);
        Check(b4.phase == p && b4.callback == 4 && b4.rate == 44100 && b4.expected == 48000 && b4.kinds == (1u << kBadRate),
              "sample 5: phase, callback 4, reported 44100 vs expected 48000", b4.callback);
        Check(c.bad(0).kinds == (1u << kNull), "sample 1 is the null ASIOTime", (long)c.bad(0).kinds);
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
