// WASAPI Driver for ASIO - offline test of the live-settings publication (src/live.h, SeqPublished)
// (Linux: g++ -O2 -std=c++17 -pthread tests/live_test.cpp -o lt && ./lt)
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
//   One writer thread publishes 200,000 versions of a struct whose fields all carry the same version number (and a
//   checksum) while a reader thread polls tryRead() as fast as it can, like the audio thread does once per
//   buffer. The reader must never see a torn struct (fields from two versions), must never see versions going
//   backwards, and must eventually see the last version. Prints "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/live.h"
#include <atomic>
#include <cstdio>
#include <thread>

struct Blob {             // about the size of LiveParams
    uint32_t version;
    float    gain;
    int8_t   route[32];
    int      a, b;
    float    g[2];
    uint32_t check;       // version ^ 0xA5A5A5A5
};

int main() {
    SeqPublished<Blob> pub;
    const uint32_t kN = 200000;
    std::atomic<bool> done{false};
    long torn = 0, backwards = 0, reads = 0;
    uint32_t lastSeen = 0;
    std::thread reader([&] {
        Blob b{}; uint32_t seq = 0;
        while (true) {
            const bool fin = done.load(std::memory_order_acquire);
            if (pub.tryRead(b, seq)) {
                ++reads;
                bool ok = b.check == (b.version ^ 0xA5A5A5A5u) && b.gain == (float)b.version && b.a == (int)b.version &&
                          b.b == -(int)b.version && b.g[0] == (float)(b.version & 0xFFFF) && b.g[1] == b.g[0];
                for (int i = 0; i < 32; ++i) if (b.route[i] != (int8_t)(b.version + i)) ok = false;
                if (!ok) ++torn;
                if (b.version < lastSeen) ++backwards;
                lastSeen = b.version;
            }
            if (fin && lastSeen == kN) break;
            if (fin && !pub.tryRead(b, seq) && lastSeen != kN) {
                // writer finished: one more read must deliver the final version
                Blob f{}; uint32_t s0 = 0;
                if (pub.tryRead(f, s0)) { lastSeen = f.version; if (f.check != (f.version ^ 0xA5A5A5A5u)) ++torn; }
                break;
            }
        }
    });
    for (uint32_t v = 1; v <= kN; ++v) {
        Blob b{};
        b.version = v; b.gain = (float)v; b.a = (int)v; b.b = -(int)v; b.g[0] = b.g[1] = (float)(v & 0xFFFF);
        for (int i = 0; i < 32; ++i) b.route[i] = (int8_t)(v + i);
        b.check = v ^ 0xA5A5A5A5u;
        pub.publish(b);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    printf("  versions published %u, successful reads %ld, torn reads %ld, backwards %ld, last seen %u\n", kN, reads, torn, backwards, lastSeen);
    const bool ok = torn == 0 && backwards == 0 && lastSeen == kN && reads > 0;
    printf("RESULT: %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
