// WASAPI Driver for ASIO - configuration model (plain data, no Windows dependency)
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
// Purpose (0.9.8, hardening)
//   The Config structure and its enums, moved out of common.h unchanged so that the pure parsers of config.ini
//   (src/parse.h) can be compiled and fuzzed on Linux without <windows.h> (tests/fuzz_test.cpp). common.h includes
//   this header, so every existing user of Config is unaffected.
//
// Threading
//   Plain data. Config objects are owned by the host thread (see common.h).
#pragma once
#include <climits>
#include <string>
#include <vector>

enum class LoopbackMode { Off = 0, ExcludeDaw = 1, All = 2, App = 3 };   // App = a single application (0.4.0)
enum class DriftMode    { Auto = 0, On = 1, Off = 2 };
enum class ResamplerQ   { Hermite = 0, Sinc = 1 };
// 0.9.10: engine preference of the fixed-ratio converter (fixedratio.h fr::Engine, same values).
enum class FrEngine     { Auto = 0, Soxr = 1, Sinc = 2, Hermite = 3 };

// Complete driver configuration. Every field maps to a key of config.ini ([General] section).
struct Config {
    bool         exclusive        = false;  // Mode=Shared|Exclusive
    bool         lowLatencyShared = true;   // use IAudioClient3 minimum engine period in Shared (output and input)
    std::wstring outputDevice;              // empty = Windows default; otherwise a substring of the device name
    std::wstring inputDevice;               // empty = Windows default; "none" = no input device
    LoopbackMode loopback         = LoopbackMode::Off;
    long         bufferSize       = 256;    // ASIO buffer size in frames
    double       sampleRate       = 48000.0;
    double       gain             = 1.0;    // additional linear output gain
    bool         followDevices    = true;   // reopen when devices change
    DriftMode    drift            = DriftMode::Auto;
    int          maxChannels      = 32;     // ASIO channel limit per device
    bool         log              = true;
    std::wstring loopbackApp;               // Loopback=App: executable name (e.g. spotify.exe)
    bool         dither           = true;   // TPDF dither on 16/24-bit integer outputs (Exclusive)
    ResamplerQ   resampler        = ResamplerQ::Hermite;   // drift-correction resampler quality
    // 0.9.10: FixedRatioEngine=Auto|Soxr|Sinc|Hermite, the fixed-ratio converter used when the ASIO rate differs
    // from the device rate (fixedratio.h Choose). RESET key, stored in profiles (an engine setting).
    FrEngine     frEngine         = FrEngine::Auto;
    std::wstring activeProfile;             // informational only: last profile applied from the panel
    // 0.5.0 (beta): additional devices (name substrings, at most kMaxExtra per list) and output routing
    std::vector<std::wstring> extraOutputs, extraInputs;
    std::wstring outputMap;                 // "asio:dev,..." (1-based), primary output only; empty = identity
    bool         alignDevices     = true;   // align the latency of all devices (only with additional devices)
    // 0.7.0 (F4): latency offsets added to what getLatencies reports (ms, -50..+500). LIVE settings.
    double       outLatOffsetMs   = 0.0;    // OutputLatencyOffsetMs
    double       inLatOffsetMs    = 0.0;    // InputLatencyOffsetMs
    // 0.7.0 (F5): direct monitoring configured in the panel (independent of the host's ASIO direct monitoring).
    bool         dmEnabled        = false;  // DirectMonitor=0|1
    int          dmInput          = 0;      // DirectMonitorInput: first device input channel of the pair (0-based)
    int          dmOutput         = 0;      // DirectMonitorOutput: first main-output channel of the pair (0-based)
    double       dmLevelDb        = 0.0;    // DirectMonitorLevelDb (-60..+6)
    double       dmPan            = 0.0;    // DirectMonitorPan (-100 left .. 0 .. +100 right)
    bool         dmMute           = false;  // DirectMonitorMute
    // 0.7.0 (F2): floating monitor window (machine preferences, never stored in profiles). LIVE.
    bool         monitorWindow    = false;  // MonitorWindow
    bool         monitorTopmost   = false;  // MonitorTopmost
    int          monitorX = INT_MIN, monitorY = INT_MIN, monitorW = 0, monitorH = 0;   // MonitorX/Y/W/H (0 = default)
    // 0.8.0
    bool         protectA2dp      = true;   // G1 ProtectA2dp: never open the input of the Bluetooth output's own headset (RESET)
    bool         limiterOn        = false;  // G2 Limiter=0|1 (LIVE)
    double       limiterCeilDb    = -1.0;   // G2 LimiterCeilingDb (-20..-0.1 dBFS)
    bool         limiterCapOn     = false;  // G2 LimiterGainCap=0|1: while the limiter is on, Gain never exceeds the cap
    double       limiterCapDb     = 0.0;    // G2 LimiterGainCapDb (-20..+24 dB)
    bool         sampleRateAuto   = false;  // G4 SampleRate=Auto: follow the Windows mix rate in Shared mode (RESET)
    bool         trayIcon         = true;   // G5 TrayIcon=0|1 (LIVE, machine preference)
    bool         deviceChannelNames = true; // G6 DeviceChannelNames=0|1: names from the device (RESET)
    // 0.8.1: animated panel background (all LIVE, machine preferences, never in profiles)
    int          visuals          = 1;      // Visuals=Off|Plasma|Starfield|Copper|Tunnel|Reactive (vis::Effect 0..5)
    bool         visualsExplicit  = false;  // the Visuals key exists (chosen by the user): honored in remote sessions
    int          visualsIntensity = 35;     // VisualsIntensity=0..100
    bool         visualsMonitor   = false;  // VisualsMonitor=0|1: also behind the meters of the monitor window
    int          visualsSpeed     = 100;    // 0.8.2 VisualsSpeed=25..400 (percent of the default speed)
    // 0.9.0 (all general-only machine preferences, never in profiles)
    int          theme            = 0;      // H1 Theme=Dark|RetroBlue|Retro8|Terminal|Neon|Light (thm::Id; LIVE)
    int          language         = 0;      // H4 Language=Auto|en|es|pt|fr|de (0 = Auto; LIVE)
    bool         updateCheck      = false;  // H3 UpdateCheck=0|1: weekly check when the panel opens (default off)
    std::wstring updateRepo;                // H3 UpdateRepo=owner/repository ("" = the build default WB_UPDATE_REPO)
    long long    updateLastCheck  = 0;      // H3 UpdateLastCheck: seconds since 1970 of the last check (0 = never)
    // 0.9.2 (general-only, LIVE)
    bool         helpTips         = true;   // HelpTips=0|1: hover help tips in the panel and the monitor window
};
