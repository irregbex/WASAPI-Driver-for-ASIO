// WASAPI Driver for ASIO - shared definitions: product identity, configuration model, config/log API
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
//   Central header included by every module of the driver DLL and by the companion tools. Defines the product
//   name/version constants, the Config structure (one-to-one with the keys of config.ini) and the functions that
//   load/save the configuration, manage profiles and write the diagnostic log (implemented in config.cpp).
//
// Threading
//   Config objects are owned by the host thread (the thread that called IASIO::init()). The config/profile
//   functions perform file I/O and allocate memory: never call them from the real-time audio thread. Log() takes a
//   critical section and flushes to disk: host and notification threads only, never the audio thread.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <climits>

#define WB_NAME        "WASAPI Driver for ASIO"     // driver name (getDriverName, key HKLM\SOFTWARE\ASIO\...)
#define WB_NAME_W      L"WASAPI Driver for ASIO"
#define WB_OLDNAME_W   L"WasapiBridge"               // name used before 0.6.0 (migration and cleanup only)
#define WB_FILEBASE_W  L"WasapiDriver"               // file name prefix (WasapiDriver64.dll, WasapiDriverDiag.exe, ...)
#define WB_VERSION_STR "0.9.15"
#define WB_VERSION_W   L"0.9.15"
// getDriverVersion(). Up to 0.9.9 the value was major * 1000 + minor * 100 + patch * 10 (0.9.9 -> 990), which
// cannot hold a two-digit patch (0.9.10 would give 1000, the value 1.0.0 would get). 0.9.10: major * 100000 +
// minor * 1000 + patch * 10, so 0.9.10 -> 9100: larger than every earlier value (hosts that compare versions see an
// upgrade), room for minor and patch up to 99, and the last digit stays free as before. tests/update_test.cpp
// checks that WB_VERSION_NUM follows this formula for WB_VERSION_STR and is larger than 990.
#define WB_VERSION_NUM 9150
// 0.9.4: private ASIOFuture selector of the test tool: receives the engine line of the running stream ("Low-latency
// engine period ... ; internal rate conversion ..."). Not part of the ASIO SDK. 0.9.8: opt = asiochk::EngineInfo
// (asiocheck.h: magic, size, char text[256]); any other argument gets ASE_InvalidParameter.
#define WB_FUTURE_ENGINE_INFO 0x57424549L

// Build option (0.6.1): WB_SHOW_ASIO_LOGO=0 removes the Steinberg ASIO Compatible logo from the panel and from the
// DLL resources (pass -DWB_SHOW_ASIO_LOGO=0 to the compiler and to windres / zig rc, e.g. WB_FLAGS in the build
// scripts). Default 1: the logo is shown as required by the Steinberg ASIO Usage Guidelines for dialogs that
// configure ASIO. With 0, only the text attribution remains (see TRADEMARKS.txt).
#ifndef WB_SHOW_ASIO_LOGO
#define WB_SHOW_ASIO_LOGO 1
#endif

// 0.9.8: LoopbackMode, DriftMode, ResamplerQ and Config live in cfgtypes.h (no Windows dependency, fuzzed offline).
#include "cfgtypes.h"

// 0.8.0 (G3): automatic profile per host program. [HostProfiles] maps an executable file name (case-insensitive,
// e.g. Cubase15.exe) to a profile name. Host thread only.
std::wstring HostProfileFor(const std::wstring& exe);           // "" if no mapping
void SetHostProfile(const std::wstring& exe, const std::wstring& profile);   // empty profile removes the mapping
std::vector<std::pair<std::wstring, std::wstring>> ListHostProfiles();
// File name of the executable of the current process (e.g. Cubase15.exe), without the path.
std::wstring HostExeName();
// For the log: `name` with every character outside [A-Za-z0-9 ._+()-] replaced by '_' (at most 64 characters),
// so a file or profile name can never carry anything unexpected into a report.
std::string SafeLogName(const std::wstring& name);
// Writes one boolean key of [General] (e.g. MonitorWindow) without touching the others. Host thread only.
void SaveGeneralFlag(const wchar_t* key, bool on);
// 0.9.0: the same for a 64-bit integer (UpdateLastCheck).
void SaveGeneralInt64(const wchar_t* key, long long v);

// 0.9.0 (H3): GitHub "owner/repository" of the update source; override at build time
// (-DWB_UPDATE_REPO=L\"owner/repo\") or with UpdateRepo= in config.ini.
// 0.9.7: the default names the public repository. In the prepared source tree the owner is a token that is
// replaced with the real GitHub account name before the first public build; as long as the owner still
// contains the word PLACEHOLDER (or is the older "OWNER"), upd::IsPlaceholderRepo() reports the source as not
// configured, so the panel says "Update source not configured yet" and no request is ever sent to GitHub.
#ifndef WB_UPDATE_REPO
#define WB_UPDATE_REPO L"irregbex/WASAPI-Driver-for-ASIO"
#endif

// 0.7.0 (F1): true if going from `a` to `b` changes any RESET setting (one that needs the host to reopen the
// driver: devices, mode, buffer, sample rate, channels, loopback, extra devices, resampler, drift correction,
// alignment). Every other difference is applied live. Host thread.
bool NeedsReset(const Config& a, const Config& b);
// 0.7.0 (F2): stores only the monitor window placement keys (MonitorX/Y/W/H, MonitorTopmost) in [General].
void SaveMonitorPlacement(int x, int y, int w, int h, bool topmost);

constexpr int kMaxExtra = 3;                // additional output and input devices (each)

// Configuration folder: %APPDATA%\WASAPI Driver for ASIO (created on demand; migrated from the old name once).
std::wstring ConfigDir();
// Full path of config.ini inside ConfigDir().
std::wstring ConfigPath();
// Loads [General] over the built-in defaults; creates config.ini and the factory profiles when missing and runs
// the one-time profile migrations. Host thread only.
void LoadConfig(Config& c);
// Writes every key of [General] (and ActiveProfile). Host thread only.
void SaveConfig(const Config& c);
// 0.9.5: [General] LastHostRate, the rate the host last set (0 = none); WriteLastHostRate(0) removes the key.
double ReadLastHostRate();
void WriteLastHostRate(double r);

// Profiles: sections [Profile:<name>] of config.ini holding a subset of the [General] keys. Host thread only.
std::vector<std::wstring> ListProfiles();
// Reads profile `name` on top of `c` (keys missing in the profile keep the value already in `c`).
// Returns false if the profile does not exist or the name is invalid.
bool LoadProfile(const std::wstring& name, Config& c);
// Saves engine settings; device keys only if includeDevices (otherwise they are removed from the profile).
void SaveProfile(const std::wstring& name, const Config& c, bool includeDevices);
// True if the stored profile contains any device key (OutputDevice, InputDevice, extras, routing...).
bool ProfileHasDevices(const std::wstring& name);
void DeleteProfile(const std::wstring& name);
// 1..40 characters, none of [ ] = ; or control characters, no leading/trailing space.
bool ValidProfileName(const std::wstring& name);

// Opens (enabled) or closes the log. log.txt is the current session; the previous one is kept as log-previous.txt.
void LogInit(bool enabled);
// printf-style log line with a timestamp. Takes a lock and flushes: NEVER call it from the audio thread.
void Log(const char* fmt, ...) noexcept;   // 0.9.8: never throws (exception barrier)
// UTF-16 to UTF-8 (for the log). Allocates: not for the audio thread.
std::string Utf8(const std::wstring& w);
