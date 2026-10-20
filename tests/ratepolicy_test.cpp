// WASAPI Driver for ASIO - offline test of the sample-rate precedence (0.9.5)
// (Linux: g++ -O2 -std=c++17 tests/ratepolicy_test.cpp -o rpt && ./rpt)
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
//   src/ratepolicy.h as the driver uses it: a model instance (init / setSampleRate / reload after a driver
//   reset) on a model process (the per-process host rate) and a model config.ini (SampleRate, Auto,
//   LastHostRate). Reproduces the 0.9.4 field bug (config 96000, Cubase at 44100, Bluetooth device removed ->
//   reset -> the driver came back at 96000) and checks every precedence rule. Prints "RESULT: PASSED" or
//   "RESULT: FAILED".
#include "../src/ratepolicy.h"
#include <cstdio>

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %-74s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }

struct Ini { double sampleRate = 96000; bool autoRate = false; };   // config.ini [General]; LastHostRate in Memory
struct Process { rp::Memory mem; };                                    // mem.lastHost stands for config.ini

// Same steps as WasapiBridge::init / reload / setSampleRate / applyPanelChanges / onFormatChanged.
struct Instance {
    double rate = 0; rp::Src src = rp::Src::Panel;
    void choose(const Ini& ini, Process& p, bool safe, double windowsRate) {
        const rp::Decision d = rp::Choose(ini.sampleRate, ini.autoRate, p.mem.processHost, p.mem.lastHost, safe);
        rate = d.src == rp::Src::Auto ? windowsRate : d.rate; src = d.src;
    }
    void init(const Ini& ini, Process& p, bool safe = false, double windowsRate = 48000) { choose(ini, p, safe, windowsRate); }
    void reload(const Ini& ini, Process& p, bool safe = false, double windowsRate = 48000) { choose(ini, p, safe, windowsRate); }
    void setSampleRate(Process& p, double r) { if (rp::Valid(r)) { p.mem.hostSet(r); rate = r; src = rp::Src::Host; } }
    void panelChangesRate(Ini& ini, Process& p, double r, bool autoRate = false) {
        ini.sampleRate = r; ini.autoRate = autoRate; p.mem.panelRateChanged();
    }
};

int main() {
    printf("Sample-rate precedence offline test\n");
    {   // the field bug and its follow-ups
        Ini ini; Process proc;
        Instance a; a.init(ini, proc);
        Check(a.rate == 96000 && a.src == rp::Src::Panel, "fresh install: config SampleRate=96000 -> 96000 (panel)");
        a.setSampleRate(proc, 44100);
        Check(a.rate == 44100 && proc.mem.lastHost == 44100, "Cubase sets 44100 -> 44100, LastHostRate=44100 written");
        a.reload(ini, proc);
        Check(a.rate == 44100 && a.src == rp::Src::Host, "device-change reset (same instance) -> still 44100 (host)");
        Instance b; b.init(ini, proc);
        Check(b.rate == 44100 && b.src == rp::Src::Host, "host releases and re-creates the instance (same process) -> 44100");
        Process next; next.mem.lastHost = proc.mem.lastHost;          // new process: only config.ini survives
        Instance c; c.init(ini, next);
        Check(c.rate == 44100 && c.src == rp::Src::LastHost, "new process with LastHostRate=44100 -> 44100 (last host)");
        c.panelChangesRate(ini, next, 48000);
        c.reload(ini, next);
        Check(c.rate == 48000 && c.src == rp::Src::Panel && next.mem.lastHost == 0, "user sets 48000 in the panel -> 48000, LastHostRate cleared");
        Process third; third.mem.lastHost = next.mem.lastHost;
        Instance d; d.init(ini, third);
        Check(d.rate == 48000 && d.src == rp::Src::Panel, "then a new process -> 48000 (panel)");
        d.setSampleRate(third, 88200); d.reload(ini, third);
        Check(d.rate == 88200, "host sets 88200 afterwards -> survives the next reset");
    }
    {   // panel changes that do not touch the rate keep the host rate
        Ini ini; Process proc; Instance a; a.init(ini, proc); a.setSampleRate(proc, 44100);
        a.reload(ini, proc);                                          // e.g. buffer size changed in the panel
        Check(a.rate == 44100, "panel RESET-class change without a rate change -> 44100 kept");
    }
    {   // SampleRate=Auto keeps its own behaviour
        Ini ini; ini.autoRate = true; Process proc; Instance a;
        a.init(ini, proc, false, 48000);
        Check(a.rate == 48000 && a.src == rp::Src::Auto, "Auto: fresh instance follows Windows (48000)");
        proc.mem.lastHost = 44100; Instance b; b.init(ini, proc, false, 48000);
        Check(b.rate == 48000, "Auto: LastHostRate is not used at a fresh start");
        b.setSampleRate(proc, 44100); b.reload(ini, proc, false, 48000);
        Check(b.rate == 44100 && b.src == rp::Src::Host, "Auto: the host's 44100 wins over Windows across a reset");
        Instance c; c.init(ini, proc, false, 48000);
        Check(c.rate == 44100, "Auto: ... and across a re-created instance");
        proc.mem.autoWindowsChanged(); c.reload(ini, proc, false, 96000);
        Check(c.rate == 96000 && c.src == rp::Src::Auto, "Auto: Windows changes to 96000 -> the driver's reset follows Windows");
    }
    {   // crash safe mode: a new process; LastHostRate is not used, a rate the host sets in the session is kept
        Ini ini; Process proc; proc.mem.lastHost = 44100;
        Instance a; a.init(ini, proc, true);
        Check(a.rate == 96000 && a.src == rp::Src::Panel, "safe mode after a crash -> panel rate, LastHostRate ignored");
        a.setSampleRate(proc, 44100); a.reload(ini, proc, true);
        Check(a.rate == 44100 && a.src == rp::Src::Host, "safe mode: a rate the host sets survives the next reset");
    }
    {   // invalid remembered values are ignored
        Ini ini; Process proc; proc.mem.lastHost = 12345; Instance a; a.init(ini, proc);
        Check(a.rate == 96000 && a.src == rp::Src::Panel, "hand-edited LastHostRate=12345 -> ignored (panel)");
        a.setSampleRate(proc, 22050);
        Check(proc.mem.processHost == 0 && proc.mem.lastHost == 12345, "a rate the driver rejects is not remembered");
    }
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
