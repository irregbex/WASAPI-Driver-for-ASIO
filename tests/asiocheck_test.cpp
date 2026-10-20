// WASAPI Driver for ASIO - offline test of the host-argument validation (src/asiocheck.h), with garbage input
// (g++ -O1 -g -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -D_GLIBCXX_ASSERTIONS
//  tests/asiocheck_test.cpp -o asiocheck_test && ./asiocheck_test)
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
// Purpose (0.9.8, hardening) - the "ASIO host scaffold"
//   driver.cpp checks every IASIO call with asiocheck.h before it touches anything. This test plays a buggy or
//   hostile host against those checks without Windows:
//     1. Fixed cases: each rule of asiocheck.h (call order, rates, buffer sizes, callbacks, channel lists, channel
//        indices, clock sources, the engine-info request).
//     2. A state-machine scaffold (Scaffold below): a minimal model of the driver's state (initialized, buffers,
//        running) driven by asiocheck.h exactly as driver.cpp does, receiving 200000 random calls with random
//        arguments. Invariants: the driver never "runs" without buffers, never has buffers before init, every
//        accepted channel list is in range and free of duplicates, and every write into the model's buffer table
//        stays in bounds (ASan / _GLIBCXX_ASSERTIONS).
//     3. A TOCTOU check: CopyInfos reads each host field once, so a host array that changes between two reads
//        cannot make the copy differ from what was validated.
//   The real driver is exercised with the same kind of calls on Windows by WasapiDriverTest.exe, test T11.
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED"; exit code 0 = passed.
#include "../src/asiocheck.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace asiochk;
static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %-70s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }

// Same layout as ASIOBufferInfo (asio.h): ASIOBool isInput; long channelNum; void* buffers[2].
struct BufferInfo { long isInput; long channelNum; void* buffers[2]; };

struct Rng {
    uint64_t s;
    uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    long anyLong() {
        static const long edge[] = {0, 1, -1, 2, 31, 32, 33, 63, 64, 65, 4096, 4097, -4096, 2147483647L, -2147483647L - 1};
        return (next() & 3) ? edge[next() % (sizeof edge / sizeof edge[0])] : (long)(int32_t)next();
    }
    double anyRate() {
        static const double edge[] = {0, 44100, 48000, 88200, 96000, 176400, 192000, 44099.6, 48000.4, 8000, 384000, -48000,
                                      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity(), 1e308, -0.0, std::numeric_limits<double>::denorm_min()};
        if (next() & 1) return edge[next() % (sizeof edge / sizeof edge[0])];
        uint64_t b = next(); double d; memcpy(&d, &b, 8); return d;   // any bit pattern, NaNs included
    }
};

// Minimal model of the driver's state, using asiocheck.h like driver.cpp does.
struct Scaffold {
    bool initialized = false, buffers = false, running = false;
    long nIn = 0, nOut = 0;
    float* table[2][kMaxCh][2] = {};                     // [input?][channel][half] like inBufs / outBufs
    std::vector<float> mem;
    State state() const { return StateOf(initialized, buffers, running); }

    Err init(long in, long out) { initialized = true; nIn = in; nOut = out; return kOk; }
    Err createBuffers(const BufferInfo* infos, long num, long size, bool cbs, bool sw, bool ti) {
        if (!initialized) return kNotPresent;
        Err e = CheckCallbacks(cbs, sw, ti);
        if (e == kOk) e = CheckBufferSize(size);
        if (e == kOk && num < 1) e = kInvalidMode;
        if (e == kOk && !infos) e = kInvalidParameter;
        if (e != kOk) return e;
        if (buffers) dispose();
        Chan req[2 * kMaxCh];
        e = CopyInfos(infos, num, nIn, nOut, req, 2 * kMaxCh);
        if (e != kOk) return e;
        mem.assign((size_t)num * 2 * (size_t)size, 0.f);
        for (long i = 0; i < num; ++i) {
            float* a = &mem.at((size_t)i * 2 * size);
            table[req[i].input ? 1 : 0][req[i].ch][0] = a;   // out-of-range ch would be caught by ASan
            table[req[i].input ? 1 : 0][req[i].ch][1] = a + size;
            a[size - 1] = 1.f; a[2 * size - 1] = 1.f;        // touch the last sample of both halves
        }
        buffers = true;
        return kOk;
    }
    Err dispose() { running = false; buffers = false; mem.clear(); memset(table, 0, sizeof table); return kOk; }
    Err disposeBuffers() { const Err e = Allowed(Call::DisposeBuffers, state()); return e == kOk ? dispose() : e; }
    Err start() { const Err e = Allowed(Call::Start, state()); if (e == kOk) running = true; return e; }
    Err stop() { running = false; return kOk; }
};

int main() {
    printf("ASIO argument validation offline test\n");
    // ---- 1. fixed cases
    Check(Allowed(Call::Start, State::Created) == kNotPresent && Allowed(Call::Start, State::Initialized) == kInvalidMode &&
          Allowed(Call::Start, State::Prepared) == kOk && Allowed(Call::Start, State::Running) == kOk, "start: before init / before createBuffers / prepared / running");
    Check(Allowed(Call::DisposeBuffers, State::Created) == kOk && Allowed(Call::DisposeBuffers, State::Initialized) == kOk &&
          Allowed(Call::DisposeBuffers, State::Running) == kOk, "disposeBuffers is a no-op without buffers (lenient, review R2)");
    Check(Allowed(Call::GetChannels, State::Created) == kNotPresent && Allowed(Call::CreateBuffers, State::Created) == kNotPresent,
          "queries and createBuffers before init");
    Check(Allowed(Call::GetSamplePosition, State::Prepared) == kSPNotAdvancing && Allowed(Call::GetSamplePosition, State::Running) == kOk,
          "getSamplePosition while stopped / running");
    Check(Allowed(Call::Stop, State::Created) == kOk && Allowed(Call::OutputReady, State::Running) == kNotPresent, "stop always, outputReady never");
    bool nc = false;
    Check(CheckCanSampleRate(48000) == kOk && CheckCanSampleRate(48000.3) == kOk && CheckCanSampleRate(47000) == kNoClock,
          "standard rates only (+-0.5 Hz)");
    Check(CheckCanSampleRate(std::nan("")) == kNoClock && CheckCanSampleRate(INFINITY) == kNoClock && CheckCanSampleRate(-48000) == kNoClock &&
          CheckCanSampleRate(0) == kNoClock, "NaN, infinity, negative and 0 refused by canSampleRate");
    Check(CheckSetSampleRate(0, nc) == kOk && nc && CheckSetSampleRate(96000, nc) == kOk && !nc &&
          CheckSetSampleRate(std::nan(""), nc) == kNoClock && CheckSetSampleRate(-INFINITY, nc) == kNoClock, "setSampleRate: 0 = no change; NaN / -inf refused");
    Check(CheckBufferSize(32) == kOk && CheckBufferSize(4096) == kOk && CheckBufferSize(300) == kOk && CheckBufferSize(31) == kInvalidMode &&
          CheckBufferSize(4097) == kInvalidMode && CheckBufferSize(0) == kInvalidMode && CheckBufferSize(-256) == kInvalidMode, "buffer size range");
    Check(CheckCallbacks(false, true, true) == kInvalidMode && CheckCallbacks(true, false, false) == kInvalidParameter &&
          CheckCallbacks(true, false, true) == kOk && CheckCallbacks(true, true, false) == kOk, "callbacks: null structure, no switch callback");
    Chan out[2 * kMaxCh];
    BufferInfo ok2[2] = {{1, 0, {}}, {0, 1, {}}};
    Check(CopyInfos(ok2, 2, 2, 2, out, 64) == kOk && out[0].input && out[0].ch == 0 && !out[1].input && out[1].ch == 1, "valid channel list copied");
    BufferInfo dup[2] = {{0, 1, {}}, {0, 1, {}}};
    Check(CopyInfos(dup, 2, 2, 2, out, 64) == kInvalidParameter, "the same channel twice");
    BufferInfo bad[1] = {{0, 2, {}}};
    Check(CopyInfos(bad, 1, 2, 2, out, 64) == kInvalidParameter, "channel index = channel count");
    BufferInfo neg[1] = {{1, -1, {}}};
    Check(CopyInfos(neg, 1, 2, 2, out, 64) == kInvalidParameter, "negative channel index");
    BufferInfo oddIn[1] = {{-7, 0, {}}};
    Check(CopyInfos(oddIn, 1, 2, 2, out, 64) == kOk && out[0].input, "isInput: any non-zero value is an input (ASIOBool)");
    Check(CopyInfos(ok2, 0, 2, 2, out, 64) == kInvalidMode && CopyInfos(ok2, -5, 2, 2, out, 64) == kInvalidMode, "numChannels <= 0");
    Check(CopyInfos((const BufferInfo*)nullptr, 2, 2, 2, out, 64) == kInvalidParameter, "null channel array");
    Check(CopyInfos(ok2, 5, 2, 2, out, 64) == kInvalidParameter, "more channels than the driver has");
    Check(CopyInfos(ok2, 2, 1000, 1000, out, 64) == kOk, "channel counts above 32 are clamped (no overflow of the seen tables)");
    Check(CheckChannelIndex(0, true, 2, 2) == kOk && CheckChannelIndex(2, true, 2, 2) == kInvalidParameter &&
          CheckChannelIndex(-1, false, 2, 2) == kInvalidParameter && CheckChannelIndex(40, false, 64, 64) == kInvalidParameter, "getChannelInfo index");
    Check(CheckClockSources(true, true, 1) == kOk && CheckClockSources(true, true, 0) == kInvalidParameter &&
          CheckClockSources(false, true, 4) == kInvalidParameter && CheckClockSources(true, false, 4) == kInvalidParameter, "getClockSources arguments");
    EngineInfo ei = {kEngineInfoMagic, sizeof(EngineInfo), {}};
    char small[8] = {};
    Check(CheckEngineInfo(&ei) == kOk && CheckEngineInfo(nullptr) == kInvalidParameter &&
          CheckEngineInfo((const EngineInfo*)(const void*)small) == kInvalidParameter, "engine-info request needs its header");

    // ---- 2. random calls against the scaffold
    Rng r{0xA510C4ECull};
    Scaffold s;
    long accepted = 0, refused = 0;
    std::vector<BufferInfo> infos(80);
    for (long it = 0; it < 200000; ++it) {
        const unsigned op = (unsigned)(r.next() % 8);
        Err e = kOk;
        switch (op) {
        case 0: if (!s.initialized || (r.next() % 50) == 0) e = s.init((long)(r.next() % 40), (long)(r.next() % 40)); break;
        case 1: case 2: {
            const long num = (r.next() & 1) ? (long)(r.next() % 70) : r.anyLong();
            for (auto& b : infos) { b.isInput = (r.next() & 1) ? (long)(r.next() % 3) : r.anyLong(); b.channelNum = (r.next() & 3) ? (long)(r.next() % 36) : r.anyLong(); }
            const long size = (r.next() & 1) ? 32 + (long)(r.next() % 4100) : r.anyLong();
            e = s.createBuffers((r.next() % 20) ? infos.data() : nullptr, num > 80 ? 80 : num, size > 8192 ? 8192 : size,
                                (r.next() % 10) != 0, (r.next() & 1) != 0, (r.next() & 1) != 0);
            break;
        }
        case 3: e = s.start(); break;
        case 4: e = s.stop(); break;
        case 5: e = s.disposeBuffers(); break;
        case 6: { bool n2; e = CheckSetSampleRate(r.anyRate(), n2); (void)CheckCanSampleRate(r.anyRate()); break; }
        default: e = CheckChannelIndex(r.anyLong(), (r.next() & 1) != 0, s.nIn, s.nOut); break;
        }
        (e == kOk ? accepted : refused)++;
        if (s.running && !s.buffers) { Check(false, "scaffold: running without buffers"); break; }
        if (s.buffers && !s.initialized) { Check(false, "scaffold: buffers before init"); break; }
    }
    printf("  random host calls: %ld accepted, %ld refused\n", accepted, refused);
    Check(accepted > 1000 && refused > 1000, "200000 random calls survived (both accepted and refused calls exercised)");

    // ---- 3. TOCTOU: each field read once
    struct Flip {                                        // a "host" whose channelNum changes every time it is read
        mutable int reads = 0;
        long isInput = 0;
        struct Ch { const Flip* f; operator long() const { return f->reads++ == 0 ? 0 : 999; } } channelNum{this};
    };
    Flip flip[1];
    Chan c1[2];
    const Err e1 = CopyInfos(flip, 1, 2, 2, c1, 2);
    Check(e1 == kOk && c1[0].ch == 0 && flip[0].reads == 1, "channelNum read exactly once (copy equals the validated value)");

    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
