// WASAPI Driver for ASIO - offline test of the instance registry (src/registry.h)
// (Linux: g++ -O2 -std=c++17 -pthread tests/registry_test.cpp -o rt && ./rt)
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
// Purpose (0.9.1, fix 3)
//   The registry hands out only objects that are alive, with the reference already taken under its lock:
//     - newest first, never the `old` instance, never one whose count reached zero or that is dying;
//     - stress: creator threads add objects and release them (remove first, as the driver destructor does, then
//       free), while finder threads look up and release; an object must never be returned after its last
//       Release, and every reference taken must be balanced. Run it under ThreadSanitizer or AddressSanitizer
//       for the strongest check (the CI runs the plain build).
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/registry.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %-60s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }

struct Obj;
static reg::Registry<Obj> g_reg;
static std::atomic<long> g_live{0}, g_badRef{0};

struct Obj {
    std::atomic<long> refs{1};
    std::atomic<bool> dying{false};
    std::atomic<bool> destroyed{false};
    Obj() { ++g_live; }
    bool tryAddRef() {                    // same rule as WasapiBridge::tryAddRef
        if (dying.load()) return false;
        long r = refs.load();
        while (r > 0) if (refs.compare_exchange_weak(r, r + 1)) return true;
        return false;
    }
    void addRef() { ++refs; }
    void release() {
        if (--refs == 0) {
            g_reg.remove(this);           // the destructor removes the object first
            destroyed = true;
            --g_live;
            delete this;
        }
    }
};

int main() {
    printf("Instance registry offline test\n");
    {
        Obj* a = new Obj; Obj* b = new Obj; Obj* c = new Obj;
        g_reg.add(a); g_reg.add(b); g_reg.add(c);
        Obj* n = g_reg.findNewerAddRef(a);
        Check(n == c && c->refs == 2, "newest instance returned with one reference added");
        n->release();
        Check(g_reg.findNewerAddRef(c) == b, "never the old instance itself");
        b->release();
        c->dying = true;
        Obj* d = g_reg.findNewerAddRef(a);
        Check(d == b, "dying instance skipped");
        d->release();
        c->dying = false;
        c->refs = 0;                      // last Release ran, destructor not yet at remove()
        Obj* e = g_reg.findNewerAddRef(a);
        Check(e == b && c->refs == 0, "instance with count 0 skipped and not resurrected");
        e->release();
        c->refs = 1;
        c->release(); b->release();
        Check(g_reg.findNewerAddRef(a) == nullptr && g_reg.size() == 1, "removed instances are gone");
        a->release();
        Check(g_reg.size() == 0 && g_live == 0, "all objects freed");
    }
    {
        std::atomic<bool> stop{false};
        std::atomic<long> found{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 3; ++t)
            th.emplace_back([&] {
                for (int i = 0; i < 20000; ++i) {
                    Obj* o = new Obj;
                    g_reg.add(o);
                    if (i & 1) std::this_thread::yield();
                    o->release();          // last reference: removed and freed while finders run
                }
            });
        for (int t = 0; t < 3; ++t)
            th.emplace_back([&] {
                while (!stop.load()) {
                    Obj* o = g_reg.findNewerAddRef(nullptr);
                    if (!o) continue;
                    if (o->destroyed.load() || o->refs.load() < 1) ++g_badRef;
                    ++found;
                    o->release();          // may be the last one: then this thread frees it
                }
            });
        for (int t = 0; t < 3; ++t) th[(size_t)t].join();
        stop = true;
        for (size_t t = 3; t < th.size(); ++t) th[t].join();
        printf("    %ld lookups returned an object\n", found.load());
        Check(g_badRef == 0, "stress: never a destroyed or unreferenced object");
        Check(g_reg.size() == 0 && g_live == 0, "stress: every reference balanced, all freed");
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
