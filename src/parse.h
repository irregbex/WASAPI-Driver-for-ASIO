// WASAPI Driver for ASIO - pure parsers of untrusted text (config.ini values, OutputMap, names), header-only
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
//   config.ini is a text file the user (or any program running as the user) can edit, so every value read from it
//   is untrusted input. Before 0.9.8 the parsing lived inside config.cpp and driver.cpp, interleaved with the
//   Win32 private-profile API, and could only be exercised on Windows. This header holds the same logic as pure
//   functions with no Windows dependency, so tests/fuzz_test.cpp can run them thousands of times on Linux under
//   AddressSanitizer and UndefinedBehaviorSanitizer with random and mutated input:
//     - ReadSectionWith: every key of a [General] / [Profile:x] section, range-checked (was config.cpp ReadSection;
//       the Win32 reader is now passed in as a callable, so the logic is identical on both sides).
//     - SplitList / JoinList: the "a|b|c" lists of ExtraOutputs / ExtraInputs.
//     - NormalizeIni: the line pass of RewriteConfigFile (comments dropped, sections and keys kept in order).
//     - DecodeUtf16Le: the raw bytes of a UTF-16LE file with BOM, portable (no unaligned wchar_t reads).
//     - ParseMultiSz: the double-NUL lists returned by GetPrivateProfileSectionNamesW / GetPrivateProfileSectionW,
//       bounded by the character count the API reported (never reads past it).
//     - ParseOutputMap: OutputMap "asio:dev,..." (was driver.cpp buildRoute, with swscanf; now a bounded
//       hand-written number parser, because scanf's %ld has undefined behavior on overflow).
//     - ValidProfileName, ValidExeKey, SafeLogName, ParseHostProfileEntry: names that become INI syntax or log text.
//   Fixes found while moving the code (0.9.8 review, CODE-REVIEW.txt H-list): DirectMonitorInput/Output computed
//   "value - 1" in int, which overflowed for INT_MIN (signed overflow, undefined behavior); OutputMap numbers
//   beyond the range of long were undefined behavior in swscanf.
//
// Threading
//   Pure functions over their arguments; they allocate (std::wstring), so never on the real-time audio thread.
#pragma once
#include "cfgtypes.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace cfgp {

constexpr int kMaxExtraList = 3;      // = kMaxExtra (common.h); entries kept by SplitList
constexpr int kRouteSlots = 32;       // = kMaxCh (driver.h); size of the routing table filled by ParseOutputMap

// ASCII case-insensitive equality (the keywords of config.ini are ASCII; non-ASCII characters compare exactly).
// Replaces _wcsicmp, which does not exist on Linux, with the same result for every keyword this file accepts.
inline wchar_t LowerA(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c; }
inline bool EqI(const std::wstring& a, const wchar_t* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) if (LowerA(a[i]) != LowerA(b[i])) return false;
    return i == a.size() && !b[i];
}

// Trims blanks and tabs at both ends (what ReadStrS does after GetPrivateProfileStringW).
inline std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) --b;
    return s.substr(a, b - a);
}

// "a|b|c" for ExtraOutputs/ExtraInputs. Device names containing '|' cannot be represented (no escaping).
inline std::wstring JoinList(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += L"|"; s += v[i]; }
    return s;
}
// Splits "a|b|c" into at most kMaxExtra trimmed (blanks), non-empty entries.
// Note: entries beyond kMaxExtra are dropped silently; a list made only of blanks yields an empty vector.
inline std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a <= s.size() && v.size() < (size_t)kMaxExtraList) {
        size_t b = s.find(L'|', a); if (b == std::wstring::npos) b = s.size();
        std::wstring t = s.substr(a, b - a);
        while (!t.empty() && t.back() == L' ') t.pop_back();
        size_t i = 0; while (i < t.size() && t[i] == L' ') ++i;
        if (i < t.size()) v.push_back(t.substr(i));
        a = b + 1;
    }
    return v;
}

// Enum <-> INI text. Unknown text read back maps to the first/neutral value (Off / Auto), see ReadSectionWith.
inline const wchar_t* LbStr(LoopbackMode m) {
    return m == LoopbackMode::ExcludeDaw ? L"ExcludeDaw" : m == LoopbackMode::All ? L"All" : m == LoopbackMode::App ? L"App" : L"Off";
}
inline const wchar_t* DriftStr(DriftMode d) { return d == DriftMode::On ? L"On" : d == DriftMode::Off ? L"Off" : L"Auto"; }
// 0.9.10: FixedRatioEngine values (the order of FrEngine).
inline const wchar_t* FrEngineStr(FrEngine e) {
    return e == FrEngine::Soxr ? L"Soxr" : e == FrEngine::Sinc ? L"Sinc" : e == FrEngine::Hermite ? L"Hermite" : L"Auto";
}

// Integer parse clamped into int (wcstol returns a long, which is 64 bits on Linux and 32 bits on Windows; the
// clamp makes both platforms give the same, defined result for out-of-range text).
inline int ToIntClamped(const std::wstring& s) {
    const long long v = wcstoll(s.c_str(), nullptr, 10);
    return v > INT_MAX ? INT_MAX : v < INT_MIN ? INT_MIN : (int)v;
}

// Reads a section ([General] or [Profile:x]) on top of `c`: missing keys keep the current value of `c`.
// get(key, default) returns the trimmed value of `key`, or `default` when the key is absent (config.cpp passes the
// Win32 private-profile reader ReadStrS; the fuzz test passes a map built from random text).
// Values are range-checked so a hand-edited file cannot produce invalid settings.
// Pattern used throughout: the current value of `c` is formatted as the default, so the reader returns it unchanged
// when the key is absent. Booleans are "anything but 0 is true". Numbers are parsed with wcstol/wcstod (garbage ->
// 0, then clamped below). Exceptions to the "missing keeps current" rule: Visuals, Theme and Language use an empty
// default and only change on a recognized name.
// 0.9.6 (D5): UpdateLastCheck defaults to the current value like the other keys (applying a profile keeps it).
// 0.9.6 (D4): the sampleRate and gain checks are written as "!(inside range)", so a NaN read from "nan" (or an
// infinity from "inf") fails them and gets the default, like the clampD lambda does for the keys it covers.
// 0.9.8: integers go through ToIntClamped (defined result for any text), and DirectMonitorInput/Output subtract
// the 1-based offset in 64-bit arithmetic (before, "DirectMonitorInput=-2147483648" overflowed an int).
template <class Get>
inline void ReadSectionWith(Config& c, Get get) {
    wchar_t b[64];
    c.exclusive        = EqI(get(L"Mode", c.exclusive ? L"Exclusive" : L"Shared"), L"Exclusive");
    c.lowLatencyShared = get(L"LowLatencyShared", c.lowLatencyShared ? L"1" : L"0") != L"0";
    c.outputDevice     = get(L"OutputDevice", c.outputDevice.c_str());
    c.inputDevice      = get(L"InputDevice", c.inputDevice.c_str());
    const std::wstring lb = get(L"Loopback", LbStr(c.loopback));
    c.loopback = EqI(lb, L"ExcludeDaw") ? LoopbackMode::ExcludeDaw
               : EqI(lb, L"All") ? LoopbackMode::All
               : EqI(lb, L"App") ? LoopbackMode::App : LoopbackMode::Off;
    c.loopbackApp      = get(L"LoopbackApp", c.loopbackApp.c_str());
    swprintf(b, 64, L"%ld", c.bufferSize);
    {
        const long long v = wcstoll(get(L"BufferSize", b).c_str(), nullptr, 10);
        c.bufferSize = v < 32 ? 32 : v > 4096 ? 4096 : (long)v;
    }
    swprintf(b, 64, L"%.0f", c.sampleRate);
    {
        const std::wstring sr = get(L"SampleRate", c.sampleRateAuto ? L"Auto" : b);
        c.sampleRateAuto = EqI(sr, L"Auto");                                       // 0.8.0 (G4)
        if (!c.sampleRateAuto) c.sampleRate = wcstod(sr.c_str(), nullptr);
    }
    swprintf(b, 64, L"%.6f", c.gain);
    c.gain             = wcstod(get(L"Gain", b).c_str(), nullptr);
    c.followDevices    = get(L"FollowDeviceChanges", c.followDevices ? L"1" : L"0") != L"0";
    const std::wstring dm = get(L"DriftCorrection", DriftStr(c.drift));
    c.drift = EqI(dm, L"On") ? DriftMode::On : EqI(dm, L"Off") ? DriftMode::Off : DriftMode::Auto;
    swprintf(b, 64, L"%d", c.maxChannels);
    c.maxChannels      = ToIntClamped(get(L"MaxChannels", b));
    c.log              = get(L"Log", c.log ? L"1" : L"0") != L"0";
    c.dither           = get(L"Dither", c.dither ? L"1" : L"0") != L"0";
    c.resampler        = EqI(get(L"ResamplerQuality", c.resampler == ResamplerQ::Sinc ? L"Sinc" : L"Hermite"), L"Sinc")
                         ? ResamplerQ::Sinc : ResamplerQ::Hermite;
    {   // 0.9.10: unknown text keeps the current value (like a missing key)
        const std::wstring fe = get(L"FixedRatioEngine", FrEngineStr(c.frEngine));
        c.frEngine = EqI(fe, L"Auto") ? FrEngine::Auto : EqI(fe, L"Soxr") ? FrEngine::Soxr
                   : EqI(fe, L"Sinc") ? FrEngine::Sinc : EqI(fe, L"Hermite") ? FrEngine::Hermite : c.frEngine;
    }
    c.extraOutputs     = SplitList(get(L"ExtraOutputs", JoinList(c.extraOutputs).c_str()));
    c.extraInputs      = SplitList(get(L"ExtraInputs", JoinList(c.extraInputs).c_str()));
    c.outputMap        = get(L"OutputMap", c.outputMap.c_str());
    c.alignDevices     = get(L"AlignDevices", c.alignDevices ? L"1" : L"0") != L"0";
    // 0.7.0
    auto rdD = [&](const wchar_t* k, double v) { swprintf(b, 64, L"%.3f", v); return wcstod(get(k, b).c_str(), nullptr); };
    auto rdI = [&](const wchar_t* k, long long v) { swprintf(b, 64, L"%lld", v); return (long long)ToIntClamped(get(k, b)); };
    auto rdB = [&](const wchar_t* k, bool v) { return get(k, v ? L"1" : L"0") != L"0"; };
    c.outLatOffsetMs   = rdD(L"OutputLatencyOffsetMs", c.outLatOffsetMs);
    c.inLatOffsetMs    = rdD(L"InputLatencyOffsetMs", c.inLatOffsetMs);
    c.dmEnabled        = rdB(L"DirectMonitor", c.dmEnabled);
    {
        // stored 1-based; 64-bit arithmetic, then the 0..31 range check below
        const long long in = rdI(L"DirectMonitorInput", (long long)c.dmInput + 1) - 1;
        const long long out = rdI(L"DirectMonitorOutput", (long long)c.dmOutput + 1) - 1;
        c.dmInput  = in < 0 || in > 31 ? 0 : (int)in;
        c.dmOutput = out < 0 || out > 31 ? 0 : (int)out;
    }
    c.dmLevelDb        = rdD(L"DirectMonitorLevelDb", c.dmLevelDb);
    c.dmPan            = rdD(L"DirectMonitorPan", c.dmPan);
    c.dmMute           = rdB(L"DirectMonitorMute", c.dmMute);
    c.monitorWindow    = rdB(L"MonitorWindow", c.monitorWindow);
    c.monitorTopmost   = rdB(L"MonitorTopmost", c.monitorTopmost);
    c.monitorX         = (int)rdI(L"MonitorX", c.monitorX);
    c.monitorY         = (int)rdI(L"MonitorY", c.monitorY);
    c.monitorW         = (int)rdI(L"MonitorW", c.monitorW);
    c.monitorH         = (int)rdI(L"MonitorH", c.monitorH);
    // 0.8.0
    c.protectA2dp      = rdB(L"ProtectA2dp", c.protectA2dp);
    c.limiterOn        = rdB(L"Limiter", c.limiterOn);
    c.limiterCeilDb    = rdD(L"LimiterCeilingDb", c.limiterCeilDb);
    c.limiterCapOn     = rdB(L"LimiterGainCap", c.limiterCapOn);
    c.limiterCapDb     = rdD(L"LimiterGainCapDb", c.limiterCapDb);
    c.trayIcon         = rdB(L"TrayIcon", c.trayIcon);
    c.deviceChannelNames = rdB(L"DeviceChannelNames", c.deviceChannelNames);
    // 0.8.1: panel visuals. A missing Visuals key keeps the default (Plasma) but is not "explicit": in a remote
    // desktop session the panel then shows no effect (see panel.cpp, VisualsEffective).
    {
        const std::wstring vs = get(L"Visuals", L"");
        static const wchar_t* const kVis[] = {L"Off", L"Plasma", L"Starfield", L"Copper", L"Tunnel", L"Reactive",
                                              L"Metaballs", L"Fire", L"Vector", L"Twister"};   // 0.9.0: four more
        for (int i = 0; i < 10; ++i) if (!vs.empty() && EqI(vs, kVis[i])) { c.visuals = i; c.visualsExplicit = true; }
    }
    // 0.9.0 (H1): theme keys. "Amiga" and "C64" are accepted as aliases of RetroBlue / Retro8 (short names some
    // users may type); they are never shown or written.
    {
        const std::wstring th = get(L"Theme", L"");
        static const wchar_t* const kTh[] = {L"Dark", L"RetroBlue", L"Retro8", L"Terminal", L"Neon", L"Light"};
        for (int i = 0; i < 6; ++i) if (!th.empty() && EqI(th, kTh[i])) c.theme = i;
        if (EqI(th, L"Amiga")) c.theme = 1;
        if (EqI(th, L"C64")) c.theme = 2;
        const std::wstring lg = get(L"Language", L"");
        static const wchar_t* const kLg[] = {L"Auto", L"en", L"es", L"pt", L"fr", L"de"};
        for (int i = 0; i < 6; ++i) if (!lg.empty() && EqI(lg, kLg[i])) c.language = i;
        c.updateCheck = rdB(L"UpdateCheck", c.updateCheck);
        c.helpTips = rdB(L"HelpTips", c.helpTips);                 // 0.9.2
        c.updateRepo = get(L"UpdateRepo", c.updateRepo.c_str());
        swprintf(b, 64, L"%lld", c.updateLastCheck);                // 0.9.6 (D5): missing keeps the current value
        c.updateLastCheck = wcstoll(get(L"UpdateLastCheck", b).c_str(), nullptr, 10);
        if (c.updateLastCheck < 0) c.updateLastCheck = 0;
    }
    c.visualsIntensity = (int)rdI(L"VisualsIntensity", c.visualsIntensity);
    c.visualsMonitor   = rdB(L"VisualsMonitor", c.visualsMonitor);
    c.visualsSpeed     = (int)rdI(L"VisualsSpeed", c.visualsSpeed);
    if (c.visualsSpeed < 25) c.visualsSpeed = 25;
    if (c.visualsSpeed > 400) c.visualsSpeed = 400;
    if (c.visualsIntensity < 0) c.visualsIntensity = 0;
    if (c.visualsIntensity > 100) c.visualsIntensity = 100;
    if (!(c.sampleRate >= 8000 && c.sampleRate <= 384000)) c.sampleRate = 48000;   // 0.9.6 (D4): NaN -> 48000
    if (!(c.gain >= 0 && c.gain <= 16)) c.gain = 1.0;                              // 0.9.6 (D4): NaN -> 1.0
    if (c.maxChannels < 1 || c.maxChannels > 32) c.maxChannels = 32;
    auto clampD = [](double& v, double lo, double hi) { if (!(v >= lo)) v = lo; if (v > hi) v = hi; };   // NaN -> lo
    clampD(c.outLatOffsetMs, -50.0, 500.0);
    clampD(c.inLatOffsetMs, -50.0, 500.0);
    clampD(c.dmLevelDb, -60.0, 6.0);
    clampD(c.dmPan, -100.0, 100.0);
    clampD(c.limiterCeilDb, -20.0, -0.1);
    clampD(c.limiterCapDb, -20.0, 24.0);
    if (c.monitorW < 0 || c.monitorW > 10000) c.monitorW = 0;
    if (c.monitorH < 0 || c.monitorH > 10000) c.monitorH = 0;
}

// The line pass of RewriteConfigFile (0.6.1): every comment (';' or '#') and blank line is dropped, every section
// header and key=value line is kept verbatim and in order (unknown keys and every [Profile:x] section included),
// a blank line separates sections, and `comments` (the English comment block) is put on top. CRLF line ends.
// Lines may end in LF or CRLF in `text`.
inline std::wstring NormalizeIni(const std::wstring& text, const std::wstring& comments) {
    std::wstring out = comments;
    bool firstSection = true;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find(L'\n', a); if (b == std::wstring::npos) b = text.size();
        std::wstring line = text.substr(a, b - a);
        a = b + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t i = 0; while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) ++i;
        if (i == line.size() || line[i] == L';' || line[i] == L'#') continue;          // blank or comment
        if (line[i] == L'[') { if (!firstSection) out += L"\r\n"; firstSection = false; }   // blank line between sections
        out += line; out += L"\r\n";
    }
    return out;
}

// Raw bytes of a file that starts with the UTF-16LE BOM (FF FE) -> text. Returns false without that BOM. Each code
// unit is assembled from two bytes (no unaligned wchar_t read); an odd trailing byte is dropped.
inline bool DecodeUtf16Le(const std::string& raw, std::wstring& out) {
    out.clear();
    if (raw.size() < 2 || (unsigned char)raw[0] != 0xFF || (unsigned char)raw[1] != 0xFE) return false;
    const size_t n = (raw.size() - 2) / 2;
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = (wchar_t)((unsigned)(unsigned char)raw[2 + 2 * i] | ((unsigned)(unsigned char)raw[3 + 2 * i] << 8));
    return true;
}

// A double-NUL-terminated list ("a\0b\0\0") as returned by GetPrivateProfileSectionNamesW and
// GetPrivateProfileSectionW: `n` is the number of characters the API reported (it excludes the final NUL).
// Never reads at or beyond buf[cap]; an entry not terminated before `n` ends at `n`.
inline std::vector<std::wstring> ParseMultiSz(const wchar_t* buf, size_t n, size_t cap) {
    std::vector<std::wstring> out;
    if (!buf) return out;
    if (n > cap) n = cap;
    size_t i = 0;
    while (i < n && buf[i]) {
        size_t e = i;
        while (e < n && buf[e]) ++e;
        out.emplace_back(buf + i, e - i);
        i = e + 1;
    }
    return out;
}

// One "exe=profile" entry of [HostProfiles]. False for an entry without '=' or with an empty exe name.
inline bool ParseHostProfileEntry(const std::wstring& line, std::wstring& exe, std::wstring& profile) {
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos || eq == 0) return false;
    exe = line.substr(0, eq); profile = line.substr(eq + 1);
    return true;
}

// Profile names become part of a section header "[Profile:<name>]", so characters that would break INI syntax
// ('[', ']', '=', ';', control characters) and leading/trailing blanks (trimmed by the API) are refused.
inline bool ValidProfileName(const std::wstring& n) {
    if (n.empty() || n.size() > 40) return false;
    for (wchar_t ch : n) if (ch == L'[' || ch == L']' || ch == L'=' || ch == L';' || (unsigned)ch < 32) return false;
    return n.front() != L' ' && n.back() != L' ';
}

// The exe name is a key of [HostProfiles]; '=' or ';' cannot appear in a file name, '[' and ']' are rejected.
inline bool ValidExeKey(const std::wstring& e) {
    if (e.empty() || e.size() > 100) return false;
    for (wchar_t ch : e) if (ch == L'[' || ch == L']' || ch == L'=' || ch == L';' || (unsigned)ch < 32) return false;
    return true;
}

// Profile/device names for log.txt: ASCII whitelist (others -> '_'), at most 64 characters plus "...". Keeps the
// log free of non-ASCII names that could contain personal data and avoids encoding issues.
inline std::string SafeLogName(const std::wstring& name) {
    std::string out;
    for (wchar_t ch : name) {
        if (out.size() >= 64) { out += "..."; break; }
        const bool ok = (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') ||
                        ch == L' ' || ch == L'.' || ch == L'_' || ch == L'+' || ch == L'(' || ch == L')' || ch == L'-';
        out += ok ? (char)ch : '_';
    }
    return out;
}

// Decimal number with an optional sign at s[i] (after optional blanks), as swscanf's " %ld" accepted it. At most
// 18 digits are accumulated (a longer number saturates; it can never be in range anyway), so the arithmetic never
// overflows. Returns false if no digit follows.
inline bool ParseLongAt(const std::wstring& s, size_t& i, long long& v) {
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n')) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == L'+' || s[i] == L'-')) { neg = s[i] == L'-'; ++i; }
    if (i >= s.size() || s[i] < L'0' || s[i] > L'9') return false;
    long long x = 0; int digits = 0;
    while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') {
        if (digits < 18) { x = x * 10 + (s[i] - L'0'); ++digits; } else x = 999999999999999999LL;
        ++i;
    }
    v = neg ? -x : x;
    return true;
}

// OutputMap "asio:dev,asio:dev,..." (1-based; primary output only). route[d] receives the 0-based ASIO channel for
// device channel d; route must have kRouteSlots entries and is filled with -1 first. nOut = ASIO output channels,
// dch = device channels (both clamped to kRouteSlots by the caller). Entries outside the ranges or malformed are
// passed to bad(entry) (logged by the driver) and ignored; an empty entry (",,") is skipped silently. Text after the
// second number of an entry is ignored, as with the former swscanf(" %ld : %ld"). Returns the accepted pairs.
template <class Bad>
inline int ParseOutputMap(const std::wstring& m, long nOut, int dch, int* route, Bad bad) {
    for (int d = 0; d < kRouteSlots; ++d) route[d] = -1;
    if (dch > kRouteSlots) dch = kRouteSlots;
    size_t i = 0; int pairs = 0;
    while (i < m.size()) {
        size_t j = m.find(L',', i); if (j == std::wstring::npos) j = m.size();
        const std::wstring t = m.substr(i, j - i);
        size_t k = 0; long long a = 0, d = 0;
        bool ok = ParseLongAt(t, k, a);
        if (ok) {
            while (k < t.size() && (t[k] == L' ' || t[k] == L'\t')) ++k;
            ok = k < t.size() && t[k] == L':';
            if (ok) { ++k; ok = ParseLongAt(t, k, d); }
        }
        if (ok && a >= 1 && a <= nOut && d >= 1 && d <= dch) { route[d - 1] = (int)(a - 1); ++pairs; }
        else if (!t.empty()) bad(t);
        i = j + 1;
    }
    return pairs;
}

}  // namespace cfgp
