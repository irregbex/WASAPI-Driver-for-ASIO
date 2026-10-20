// WASAPI Driver for ASIO - which sample rate an instance uses (0.9.5, header-only, portable)
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
//   The rate the host chose with setSampleRate must survive every reset the driver itself causes (device change,
//   Bluetooth switch, panel changes that do not touch the rate), a release + re-creation of the instance in the
//   same process and, through config.ini (LastHostRate), a new process. Precedence when an instance starts or
//   reloads its settings:
//     host rate of this process (any instance) -> "host"
//     SampleRate=Auto                        -> "auto" (the Windows rate; the caller applies it)
//     LastHostRate of config.ini             -> "last host" (not in crash safe mode: a crash starts a new process,
//                                               which then uses the panel's rate until the host sets one)
//     otherwise                              -> "panel"
//   setSampleRate stores the rate as host rate of the process and as LastHostRate. A Sample rate changed by the
//   user in the panel clears both (the panel value is used from the next reset on). With SampleRate=Auto a change
//   of the Windows rate clears the process host rate, so the reset it causes follows Windows (its own behaviour).
//   The process value is a plain number owned by the caller (the driver keeps it in a per-process named memory
//   block that survives a DLL unload); the persisted value is read and written by the caller.
//   Tested offline in tests/ratepolicy_test.cpp.
//
// Why (KNOWN-ISSUES KI-3, fixed in 0.9.5)
//   A host project at 44.1 kHz, panel SampleRate left at 96000: when a Bluetooth output disappeared, the
//   device-change reset re-read SampleRate from config.ini and reopened at 96 kHz, and the host (which also
//   re-creates the driver object after a reset, so a per-instance flag was not enough) showed "The ASIO Sample
//   Rate has changed". The rate the host chose must therefore outlive the instance, hence the per-process value
//   and the persisted LastHostRate.
//
// Where the caller keeps the values (driver.cpp)
//   processHost: a named file mapping "Local\WasapiDriverForAsio.HostRate.<pid>" (one double), so it survives
//   instance release and DLL unload/reload within the process and is never shared between processes.
//   lastHost: LastHostRate in config.ini (shared by all hosts; after one host used 48 kHz the next one starts at
//   48 kHz until it sets its own rate, which is expected).
//   WasapiBridge::chooseRate calls Choose() at init and on every settings reload; setSampleRate performs the
//   hostSet transition. Everything here runs on the host thread; nothing is real-time.
#pragma once
#include <cmath>

namespace rp {

// Where the chosen rate came from (logged, and used by the driver: Src::Host means a host choice that a
// SampleRate=Auto Windows rate must not override).
enum class Src { Host, LastHost, Panel, Auto };

// Short name for the log ("Sample rate 48000 Hz (source: host; ...)").
inline const char* SrcName(Src s) {
    return s == Src::Host ? "host" : s == Src::LastHost ? "last host" : s == Src::Auto ? "auto" : "panel";
}

// A rate the driver accepts as remembered (the standard ASIO rates, +-0.5 Hz); anything else counts as unknown.
// 0 (never set / key absent) and garbage values from a hand-edited config.ini are therefore ignored.
inline bool Valid(double r) {
    static const double rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    for (double x : rates) if (std::fabs(x - r) < 0.5) return true;
    return false;
}

struct Decision {
    double rate;       // for Src::Auto: the panel value; the caller replaces it with the Windows rate
    Src    src;
};

// Rate of an instance at init and whenever it reloads its settings (createBuffers after a device change, panel).
//   panelRate    SampleRate from config.ini (Hz)           panelAuto  SampleRate=Auto
//   processHost  host rate of this process (0 = none)       lastHost   LastHostRate from config.ini (0 = none)
//   safe         crash safe mode (started with safe settings after a crash): LastHostRate is skipped
// The order of the tests below IS the precedence; do not reorder without updating KI-3 and the tests.
inline Decision Choose(double panelRate, bool panelAuto, double processHost, double lastHost, bool safe) {
    if (Valid(processHost)) return {processHost, Src::Host};
    if (panelAuto) return {panelRate, Src::Auto};
    if (!safe && Valid(lastHost)) return {lastHost, Src::LastHost};
    return {panelRate, Src::Panel};
}

// The remembered values (process and persisted) and what changes them. The driver uses the same transitions.
// This struct is the reference model for the tests; driver.cpp applies the same transitions to its own storage
// (file mapping and config.ini) rather than holding a Memory object.
struct Memory {
    double processHost = 0;   // per process, shared by every instance
    double lastHost = 0;      // config.ini LastHostRate (0 = key absent)
    // setSampleRate from the host
    void hostSet(double r) { if (Valid(r)) { processHost = r; lastHost = r; } }
    // the user changed Sample rate (value or Auto) in the panel
    void panelRateChanged() { processHost = 0; lastHost = 0; }
    // SampleRate=Auto and the Windows rate changed: the driver's own reset follows Windows
    void autoWindowsChanged() { processHost = 0; }
};

}  // namespace rp
