// WASAPI Driver for ASIO - offline fuzz test of every parser of untrusted data (Linux, sanitizers)
// (g++ -O1 -g -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all -D_GLIBCXX_ASSERTIONS
//  tests/fuzz_test.cpp -o fuzz_test && ./fuzz_test)
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
//   Deterministic fuzzing (fixed seeds, so every run tests exactly the same inputs and a failure is reproducible)
//   of the code that reads data the driver does not control:
//     config.ini      cfgp::ReadSectionWith (every key), NormalizeIni, DecodeUtf16Le, ParseMultiSz, SplitList   (parse.h)
//     OutputMap       cfgp::ParseOutputMap                                                                        (parse.h)
//     names           ValidProfileName, ValidExeKey, SafeLogName, ParseHostProfileEntry                          (parse.h)
//     update check    upd::ParseVersion / Compare / ParseRelease / FindSha256 / ValidRepo / IsPlaceholderRepo /
//                     AllowedDownloadUrl / AllowedFinalHost / SetupAssetName                                     (update.h)
//     printed text    SanitizeTextWith, ShortHash                                                                (privacy.h)
//     channel names   chn::ShortName / PositionFromMask / FitConverted / CutBytes / Fit / MakeUnique            (chnames.h)
//     trace events    tlg::Event (field names and strings of any length)                                         (tlg.h)
//   Each target gets random inputs AND mutations (byte flips, insertions, deletions, splices, extreme numbers) of
//   valid samples, many thousands of iterations each. Built with AddressSanitizer, UndefinedBehaviorSanitizer
//   (-fno-sanitize-recover: the first finding aborts the run) and libstdc++ assertions, so any out-of-bounds access,
//   overflow, invalid shift, use-after-free or bad container index fails the test. Besides "no crash", each target
//   checks its documented invariants (value ranges after ReadSectionWith, idempotence of NormalizeIni, route table
//   entries in range, output length bounds, ...).
//   Prints one line per target with its iteration count and "RESULT: PASSED" or "RESULT: FAILED"; exit code 0 = passed.
//   Optional argument: an iteration multiplier (e.g. "./fuzz_test 10" for a longer run).
#include "../src/parse.h"
#include "../src/update.h"
#include "../src/privacy.h"
#include "../src/chnames.h"
#include "../src/tlg.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static long g_mult = 1;
static void Fail(const char* target, const char* what, long iter) {
    if (g_fail < 20) printf("  FAILED %s: %s (iteration %ld)\n", target, what, iter);
    ++g_fail;
}
static void Report(const char* target, long iters, int before) {
    printf("  %-34s %8ld iterations  %s\n", target, iters, g_fail == before ? "OK" : "FAILED");
}

// ---------------------------------------------------------------- deterministic random source (SplitMix64)
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }
    bool chance(uint32_t pct) { return below(100) < pct; }
};

// Interesting fragments: numbers at the edges of every integer type, special floats, separators, INI syntax.
static const char* const kAtoms[] = {
    "0", "1", "-1", "31", "32", "33", "4096", "4097", "2147483647", "-2147483648", "2147483648", "-2147483649",
    "9223372036854775807", "-9223372036854775808", "99999999999999999999999999", "nan", "NaN", "-nan", "inf", "-inf",
    "1e308", "1e-308", "-0", "0x10", "+5", "44100", "48000", "192000", "Auto", "Exclusive", "Shared", "On", "Off",
    "Sinc", "Hermite", "ExcludeDaw", "All", "App", "Plasma", "Twister", "Amiga", "C64", "en", "de", ":", ",", "|",
    "=", ";", "#", "[", "]", "[Profile:x]", "\r\n", "\n", "\r", " ", "\t", "\"", "\\", "%", "%s", "%n", "..", "/",
    "1:1", "2:3", "33:1", "1:33", " 1 : 2 ", "-1:-1", "a:b", "1:", ":2", "0:0", "1:1junk"
};
static const size_t kNumAtoms = sizeof kAtoms / sizeof kAtoms[0];

static wchar_t RandWChar(Rng& r) {
    switch (r.below(8)) {
    case 0: return (wchar_t)r.below(32);                          // control characters, NUL included
    case 1: return (wchar_t)(0x80 + r.below(0x780));              // Latin-1 .. Cyrillic
    case 2: return (wchar_t)(0xD800 + r.below(0x800));            // lone surrogates
    case 3: return (wchar_t)(0xE000 + r.below(0x2000));
    case 4: return (wchar_t)(0x4E00 + r.below(0x5000));           // CJK
    default: return (wchar_t)(32 + r.below(95));                  // printable ASCII (most of the time)
    }
}
static std::wstring Widen(const std::string& s) { std::wstring w; for (char c : s) w += (wchar_t)(unsigned char)c; return w; }
static std::wstring RandWide(Rng& r, size_t maxLen) {
    std::wstring s;
    const size_t n = r.below((uint32_t)maxLen + 1);
    while (s.size() < n) {
        if (r.chance(30)) s += Widen(kAtoms[r.below((uint32_t)kNumAtoms)]);
        else s += RandWChar(r);
    }
    return s;
}
static std::string RandBytes(Rng& r, size_t maxLen) {
    std::string s;
    const size_t n = r.below((uint32_t)maxLen + 1);
    while (s.size() < n) {
        if (r.chance(25)) s += kAtoms[r.below((uint32_t)kNumAtoms)];
        else s += (char)r.below(256);
    }
    return s;
}
// Mutations of a valid sample (both string types).
template <class S, class Gen>
static S Mutate(Rng& r, S s, Gen piece) {
    const int ops = 1 + (int)r.below(6);
    for (int k = 0; k < ops; ++k) {
        const size_t pos = s.empty() ? 0 : r.below((uint32_t)s.size() + 1);
        switch (r.below(6)) {
        case 0: if (!s.empty() && pos < s.size()) s[pos] = (typename S::value_type)(s[pos] ^ (1 << r.below(7))); break;   // bit flip
        case 1: s.insert(pos, piece()); break;                                                                       // insertion
        case 2: if (!s.empty()) s.erase(pos < s.size() ? pos : s.size() - 1, 1 + r.below(8)); break;                 // deletion
        case 3: if (!s.empty()) { const size_t a = r.below((uint32_t)s.size()); s.insert(pos, s.substr(a, r.below(16))); } break;   // splice
        case 4: s = s.substr(0, pos); break;                                                                         // truncation
        default: if (!s.empty()) std::swap(s[r.below((uint32_t)s.size())], s[r.below((uint32_t)s.size())]); break;  // swap
        }
    }
    return s;
}

// ---------------------------------------------------------------- config.ini values
static const wchar_t* const kKeys[] = {
    L"Mode", L"LowLatencyShared", L"OutputDevice", L"InputDevice", L"Loopback", L"LoopbackApp", L"BufferSize", L"SampleRate",
    L"Gain", L"FollowDeviceChanges", L"DriftCorrection", L"MaxChannels", L"Log", L"Dither", L"ResamplerQuality",
    L"FixedRatioEngine",
    L"ExtraOutputs", L"ExtraInputs", L"OutputMap", L"AlignDevices", L"OutputLatencyOffsetMs", L"InputLatencyOffsetMs",
    L"DirectMonitor", L"DirectMonitorInput", L"DirectMonitorOutput", L"DirectMonitorLevelDb", L"DirectMonitorPan",
    L"DirectMonitorMute", L"MonitorWindow", L"MonitorTopmost", L"MonitorX", L"MonitorY", L"MonitorW", L"MonitorH",
    L"ProtectA2dp", L"Limiter", L"LimiterCeilingDb", L"LimiterGainCap", L"LimiterGainCapDb", L"TrayIcon",
    L"DeviceChannelNames", L"Visuals", L"Theme", L"Language", L"UpdateCheck", L"HelpTips", L"UpdateRepo",
    L"UpdateLastCheck", L"VisualsIntensity", L"VisualsMonitor", L"VisualsSpeed"};

static bool ConfigInvariants(const Config& c) {
    return (int)c.frEngine >= 0 && (int)c.frEngine <= 3 && c.bufferSize >= 32 && c.bufferSize <= 4096 && c.sampleRate >= 8000 && c.sampleRate <= 384000 &&
           c.gain >= 0 && c.gain <= 16 && c.maxChannels >= 1 && c.maxChannels <= 32 &&
           c.outLatOffsetMs >= -50 && c.outLatOffsetMs <= 500 && c.inLatOffsetMs >= -50 && c.inLatOffsetMs <= 500 &&
           c.dmLevelDb >= -60 && c.dmLevelDb <= 6 && c.dmPan >= -100 && c.dmPan <= 100 &&
           c.limiterCeilDb >= -20 && c.limiterCeilDb <= -0.1 && c.limiterCapDb >= -20 && c.limiterCapDb <= 24 &&
           c.dmInput >= 0 && c.dmInput <= 31 && c.dmOutput >= 0 && c.dmOutput <= 31 &&
           c.monitorW >= 0 && c.monitorW <= 10000 && c.monitorH >= 0 && c.monitorH <= 10000 &&
           c.visualsIntensity >= 0 && c.visualsIntensity <= 100 && c.visualsSpeed >= 25 && c.visualsSpeed <= 400 &&
           c.visuals >= 0 && c.visuals < 10 && c.theme >= 0 && c.theme < 6 && c.language >= 0 && c.language < 6 &&
           c.updateLastCheck >= 0 && c.extraOutputs.size() <= 3 && c.extraInputs.size() <= 3 &&
           std::isfinite(c.sampleRate) && std::isfinite(c.gain);
}

static void FuzzReadSection() {
    const int before = g_fail; Rng r(0xC0FFEE01);
    const long n = 20000 * g_mult;
    for (long it = 0; it < n; ++it) {
        std::map<std::wstring, std::wstring> ini;
        const int keys = (int)r.below(30);
        for (int k = 0; k < keys; ++k) {
            const std::wstring key = r.chance(90) ? kKeys[r.below(sizeof kKeys / sizeof kKeys[0])] : RandWide(r, 12);
            ini[key] = r.chance(50) ? Widen(kAtoms[r.below((uint32_t)kNumAtoms)]) : RandWide(r, 40);
        }
        Config c;
        if (r.chance(20)) { c.dmInput = (int)r.next(); c.monitorX = (int)r.next(); c.bufferSize = (long)(int)r.next(); }   // odd starting state
        auto get = [&](const wchar_t* key, const wchar_t* def) {
            auto f = ini.find(key);
            return cfgp::Trim(f == ini.end() ? std::wstring(def) : f->second);
        };
        cfgp::ReadSectionWith(c, get);
        if (!ConfigInvariants(c)) Fail("ReadSectionWith", "value out of range after reading", it);
        // Round trip: reading the same section again on top of the result must keep it valid.
        cfgp::ReadSectionWith(c, get);
        if (!ConfigInvariants(c)) Fail("ReadSectionWith", "value out of range after a second read", it);
    }
    Report("config.ini sections", n, before);
}

static void FuzzNormalizeIni() {
    const int before = g_fail; Rng r(0xC0FFEE02);
    const std::wstring comments = L"; comment block\r\n; second line\r\n";
    const std::wstring sample = L"; old comment\r\n[General]\r\nMode=Shared\r\nBufferSize=256\r\n\r\n# hash comment\r\n"
                                L"[Profile:Studio]\r\nMode=Exclusive\r\n  [HostProfiles]\r\nreaper.exe=Studio\nlast";
    const long n = 20000 * g_mult;
    for (long it = 0; it < n; ++it) {
        const std::wstring text = r.chance(60) ? Mutate(r, sample, [&] { return RandWide(r, 6); }) : RandWide(r, 300);
        const std::wstring once = cfgp::NormalizeIni(text, comments);
        if (once.compare(0, comments.size(), comments) != 0) Fail("NormalizeIni", "comment block not on top", it);
        if (once.size() > comments.size() + text.size() * 2 + 2 * (text.size() + 1) + 2) Fail("NormalizeIni", "output too large", it);
        if (cfgp::NormalizeIni(once, comments) != once) Fail("NormalizeIni", "not idempotent", it);
    }
    Report("config.ini normalization", n, before);
}

static void FuzzDecodeAndLists() {
    const int before = g_fail; Rng r(0xC0FFEE03);
    const long n = 30000 * g_mult;
    for (long it = 0; it < n; ++it) {
        // UTF-16LE decode of random bytes (with and without the BOM)
        std::string raw = RandBytes(r, 64);
        if (r.chance(60)) raw.insert(0, "\xFF\xFE");
        std::wstring w;
        const bool ok = cfgp::DecodeUtf16Le(raw, w);
        if (ok != (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)) Fail("DecodeUtf16Le", "BOM detection", it);
        if (ok && w.size() != (raw.size() - 2) / 2) Fail("DecodeUtf16Le", "length", it);
        // Double-NUL lists: exact-size heap buffer, so ASan catches any read past `cap`
        const size_t cap = r.below(40);
        std::vector<wchar_t> buf(cap ? cap : 1);
        for (size_t i = 0; i < cap; ++i) buf[i] = r.chance(25) ? 0 : RandWChar(r);
        const size_t reported = r.below((uint32_t)cap + 4);      // the API's count, possibly larger than the buffer
        const auto items = cfgp::ParseMultiSz(cap ? buf.data() : nullptr, reported, cap);
        size_t total = 0; for (const auto& s : items) total += s.size() + 1;
        if (total > cap + 1) Fail("ParseMultiSz", "more characters than the buffer holds", it);
        for (const auto& s : items) {
            std::wstring exe, prof;
            if (cfgp::ParseHostProfileEntry(s, exe, prof) && (exe.empty() || exe.size() + prof.size() + 1 != s.size()))
                Fail("ParseHostProfileEntry", "split", it);
        }
        // ExtraOutputs lists
        const std::wstring list = RandWide(r, 60);
        const auto v = cfgp::SplitList(list);
        if (v.size() > 3) Fail("SplitList", "more than 3 entries", it);
        for (const auto& e : v) if (e.empty() || e.front() == L' ' || e.back() == L' ' || e.find(L'|') != std::wstring::npos) Fail("SplitList", "entry not trimmed", it);
        if (cfgp::SplitList(cfgp::JoinList(v)) != v) Fail("SplitList", "join / split round trip", it);
    }
    Report("UTF-16 decode, lists, host profiles", n, before);
}

static void FuzzOutputMapAndNames() {
    const int before = g_fail; Rng r(0xC0FFEE04);
    const std::wstring sample = L"1:1,2:2,3:1, 4 : 2 ,10:7";
    const long n = 40000 * g_mult;
    for (long it = 0; it < n; ++it) {
        const std::wstring m = r.chance(60) ? Mutate(r, sample, [&] { return Widen(kAtoms[r.below((uint32_t)kNumAtoms)]); }) : RandWide(r, 60);
        const long nOut = (long)r.below(40) - 2;
        const int dch = (int)r.below(40) - 2;
        int route[cfgp::kRouteSlots];
        int bad = 0;
        const int pairs = cfgp::ParseOutputMap(m, nOut, dch, route, [&](const std::wstring&) { ++bad; });
        if (pairs < 0 || pairs > (int)m.size()) Fail("ParseOutputMap", "pair count", it);
        for (int d = 0; d < cfgp::kRouteSlots; ++d) {
            if (route[d] < -1 || route[d] >= (nOut > 0 ? nOut : 0)) Fail("ParseOutputMap", "ASIO channel out of range", it);
            if (route[d] >= 0 && d >= dch) Fail("ParseOutputMap", "device channel out of range", it);
        }
        const std::wstring name = r.chance(50) ? Mutate(r, std::wstring(L"Studio (2)"), [&] { return RandWide(r, 4); }) : RandWide(r, 50);
        if (cfgp::ValidProfileName(name)) {
            for (wchar_t ch : name) if (ch == L'[' || ch == L']' || ch == L'=' || ch == L';' || (unsigned)ch < 32) Fail("ValidProfileName", "INI syntax accepted", it);
            if (name.size() > 40 || name.front() == L' ' || name.back() == L' ') Fail("ValidProfileName", "length / blanks", it);
        }
        if (cfgp::ValidExeKey(name) && (name.size() > 100 || name.find(L'=') != std::wstring::npos)) Fail("ValidExeKey", "accepted", it);
        const std::string safe = cfgp::SafeLogName(name);
        if (safe.size() > 67) Fail("SafeLogName", "too long", it);
        for (char c : safe) if ((unsigned char)c < 32 || (unsigned char)c > 126) Fail("SafeLogName", "non-printable output", it);
    }
    Report("OutputMap and names", n, before);
}

// ---------------------------------------------------------------- update check (update.h)
static void FuzzUpdate() {
    const int before = g_fail; Rng r(0xC0FFEE05);
    const std::string release = R"({"url":"x","tag_name":"v0.9.8","name":"0.9.8","draft":false,"prerelease":false,)"
        R"("assets":[{"name":"WASAPI-Driver-for-ASIO-0.9.8-Setup.exe","size":1423639,)"
        R"("browser_download_url":"https://github.com/o/r/releases/download/v0.9.8/WASAPI-Driver-for-ASIO-0.9.8-Setup.exe"},)"
        R"({"name":"SHA256SUMS.txt","size":200,"browser_download_url":"https://github.com/o/r/releases/download/v0.9.8/SHA256SUMS.txt"}],)"
        R"("body":"line\n\"quoted\" \u00e9 \\ end","nested":{"a":[1,2,{"b":null}],"c":-1.5e3}})";
    const std::string sums = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  WASAPI-Driver-for-ASIO-0.9.8-Setup.exe\r\n"
                             "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789 *SHA256SUMS.txt\n";
    const long n = 20000 * g_mult;
    for (long it = 0; it < n; ++it) {
        // versions: parse, then Compare must be antisymmetric and reflexive
        const std::string a = r.chance(50) ? Mutate(r, std::string("v1.2.3-rc.1+b"), [&] { return std::string(kAtoms[r.below((uint32_t)kNumAtoms)]); }) : RandBytes(r, 24);
        const std::string b = r.chance(50) ? Mutate(r, std::string("1.2.3-rc.10"), [&] { return std::string(1, (char)r.below(128)); }) : RandBytes(r, 24);
        upd::Version va, vb;
        const bool pa = upd::ParseVersion(a, va), pb = upd::ParseVersion(b, vb);
        if (pa && upd::Compare(va, va) != 0) Fail("Compare", "not reflexive", it);
        if (pa && pb && upd::Compare(va, vb) != -upd::Compare(vb, va)) Fail("Compare", "not antisymmetric", it);
        // GitHub release JSON
        const std::string json = r.chance(70) ? Mutate(r, release, [&] { return std::string(kAtoms[r.below((uint32_t)kNumAtoms)]); }) : RandBytes(r, 200);
        upd::Release rel;
        if (upd::ParseRelease(json, rel)) {
            if (rel.tag.empty() || rel.assets.size() > 256) Fail("ParseRelease", "accepted without tag / too many assets", it);
            const std::string setup = upd::SetupAssetName(rel);
            if (!setup.empty() && (setup.find_first_of("/\\:") != std::string::npos || !upd::FindAsset(rel, setup))) Fail("SetupAssetName", "unsafe name", it);
            for (const auto& as : rel.assets) if (upd::AllowedDownloadUrl(as.url) && as.url.compare(0, 19, "https://github.com/") != 0) Fail("AllowedDownloadUrl", "host", it);
        }
        // SHA256SUMS.txt lookup
        std::string hex;
        const std::string text = r.chance(60) ? Mutate(r, sums, [&] { return std::string(1, (char)r.below(256)); }) : RandBytes(r, 160);
        if (upd::FindSha256(text, r.chance(50) ? "WASAPI-Driver-for-ASIO-0.9.8-Setup.exe" : RandBytes(r, 10), hex)) {
            if (hex.size() != 64) Fail("FindSha256", "hash length", it);
            for (char c : hex) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) Fail("FindSha256", "hash characters", it);
        }
        // repository and host rules
        const std::wstring repo = r.chance(50) ? Mutate(r, std::wstring(L"owner/WASAPI-Driver-for-ASIO"), [&] { return RandWide(r, 3); }) : RandWide(r, 30);
        if (upd::ValidRepo(repo) && (repo.find(L"..") == 0 || std::count(repo.begin(), repo.end(), L'/') != 1)) Fail("ValidRepo", "accepted", it);
        if (!upd::ValidRepo(repo) && !upd::IsPlaceholderRepo(repo)) Fail("IsPlaceholderRepo", "invalid repo treated as configured", it);
        (void)upd::AllowedFinalHost(r.chance(50), RandWide(r, 40));
        (void)upd::SafeAscii(RandBytes(r, 100));
    }
    Report("update check (JSON, versions, sums)", n, before);
}

// ---------------------------------------------------------------- printed text (privacy.h)
static void FuzzSanitize() {
    const int before = g_fail; Rng r(0xC0FFEE06);
    const std::wstring sample = L"C:\\Users\\Alice\\AppData\\Local\\Temp\\x.log; D:\\Users\\Bob\\Music \"C:\\Users\\Public\\a\" ALICE-PC alice";
    const long n = 10000 * g_mult;
    for (long it = 0; it < n; ++it) {
        const std::wstring text = r.chance(60) ? Mutate(r, sample, [&] { return RandWide(r, 5); }) : RandWide(r, 200);
        std::map<std::wstring, std::wstring> env;
        if (r.chance(80)) env[L"USERNAME"] = r.chance(50) ? std::wstring(L"Alice") : RandWide(r, 8);
        if (r.chance(80)) env[L"COMPUTERNAME"] = r.chance(50) ? std::wstring(L"ALICE-PC") : RandWide(r, 8);
        if (r.chance(70)) env[L"USERPROFILE"] = r.chance(50) ? std::wstring(L"C:\\Users\\Alice\\") : RandWide(r, 20);
        if (r.chance(50)) env[L"TEMP"] = RandWide(r, 30);
        if (r.chance(50)) env[L"LOCALAPPDATA"] = L"C:\\Users\\Alice\\AppData\\Local";
        if (r.chance(30)) env[L"APPDATA"] = RandWide(r, 3);
        auto getenv = [&](const wchar_t* k) { auto f = env.find(k); return f == env.end() ? std::wstring() : f->second; };
        const std::wstring out = SanitizeTextWith(text, getenv);
        if (out.size() > text.size() * 14 + 64) Fail("SanitizeTextWith", "output too large", it);
        const std::wstring h = ShortHash(text);
        if (h.size() != 9 || h[0] != L'#') Fail("ShortHash", "format", it);
    }
    Report("SanitizeText", n, before);
}

// ---------------------------------------------------------------- channel names (chnames.h)
// A converter like WideCharToMultiByte for a DBCS code page: ASCII = 1 byte, everything else = 2 bytes (lead byte
// 0x81..0xFE); 0 when it does not fit `cap`.
static int ConvDbcs(const wchar_t* t, int len, char* out, int cap) {
    int n = 0;
    for (int i = 0; i < len; ++i) {
        const unsigned c = (unsigned)t[i];
        const int need = c < 0x80 ? 1 : 2;
        if (n + need > cap) return 0;
        if (need == 1) out[n++] = (char)c;
        else { out[n++] = (char)(0x81 + (c % 0x7D)); out[n++] = (char)(0x40 + (c % 0x3F)); }
    }
    return n;
}
static bool LeadDbcs(unsigned char c) { return c >= 0x81 && c <= 0xFE; }

static void FuzzChannelNames() {
    const int before = g_fail; Rng r(0xC0FFEE07);
    const long n = 20000 * g_mult;
    for (long it = 0; it < n; ++it) {
        std::string dev;
        const std::wstring w = RandWide(r, 70);
        for (wchar_t c : w) dev += (char)(c & 0xFF);
        const std::string sh = chn::ShortName(dev);
        if (sh.size() > dev.size()) Fail("ShortName", "longer than the input", it);
        const std::string pos = chn::PositionFromMask((uint32_t)r.next(), (int)r.below(40) - 2, (int)r.below(40) - 2);
        const size_t room = r.below(40);
        const std::string fc = chn::FitConverted(w, room, ConvDbcs);
        if (fc.size() > room) Fail("FitConverted", "does not fit", it);
        const size_t maxLen = 1 + r.below(40);
        const std::string fit = chn::Fit(dev, pos.empty() ? "In " + std::to_string(r.below(40)) : pos, maxLen, r.chance(50) ? LeadDbcs : nullptr);
        if (fit.size() > maxLen) Fail("Fit", "longer than maxLen", it);
        const std::string cut = chn::CutBytes(dev, room, LeadDbcs);
        if (cut.size() > room && cut.size() > dev.size()) Fail("CutBytes", "longer", it);
        std::vector<std::string> names;
        const int count = (int)r.below(12);
        for (int k = 0; k < count; ++k) names.push_back(r.chance(60) && !names.empty() ? names[r.below((uint32_t)names.size())] : chn::Fit(dev, "Out", 31));
        const size_t ml = 8 + r.below(30);
        chn::MakeUnique(names, ml, LeadDbcs);
        for (size_t i = 0; i < names.size(); ++i) for (size_t j = i + 1; j < names.size(); ++j) if (names[i] == names[j]) Fail("MakeUnique", "duplicates left", it);
    }
    Report("channel names", n, before);
}

// ---------------------------------------------------------------- trace event encoder (tlg.h)
static void FuzzTraceEncoder() {
    const int before = g_fail; Rng r(0xC0FFEE08);
    const long n = 20000 * g_mult;
    for (long it = 0; it < n; ++it) {
        const std::string evName = RandBytes(r, r.chance(5) ? 700 : 30);
        tlg::Event<512, 1024> e(evName.c_str());
        const int fields = (int)r.below(40);
        std::deque<std::string> keep;                 // deque: pointers to earlier strings stay valid
        for (int f = 0; f < fields; ++f) {
            keep.push_back(RandBytes(r, r.chance(5) ? 300 : 20));
            const char* name = keep.back().c_str();
            switch (r.below(8)) {
            case 0: e.i32(name, (int32_t)r.next()); break;
            case 1: e.u32(name, (uint32_t)r.next()); break;
            case 2: e.i64(name, (int64_t)r.next()); break;
            case 3: e.u64(name, r.next()); break;
            case 4: { uint64_t bits = r.next(); double d; memcpy(&d, &bits, 8); e.f64(name, d); } break;
            case 5: e.boolean(name, r.chance(50)); break;
            case 6: e.hresult(name, (int32_t)r.next()); break;
            default: { keep.push_back(RandBytes(r, r.chance(10) ? 2000 : 60)); e.str(name, r.chance(5) ? nullptr : keep.back().c_str()); } break;
            }
        }
        if (e.finish()) {
            if (e.metaSize() > 512 || e.dataSize() > 1024 || e.metaSize() < 3) Fail("tlg::Event", "sizes", it);
            if ((size_t)(e.meta()[0] | e.meta()[1] << 8) != e.metaSize()) Fail("tlg::Event", "TotalSize", it);
        }
    }
    Report("trace event encoder", n, before);
}

int main(int argc, char** argv) {
    if (argc > 1) { g_mult = atol(argv[1]); if (g_mult < 1) g_mult = 1; }
    printf("Fuzz test of the parsers of untrusted data (deterministic seeds, multiplier %ld)\n", g_mult);
    FuzzReadSection();
    FuzzNormalizeIni();
    FuzzDecodeAndLists();
    FuzzOutputMapAndNames();
    FuzzUpdate();
    FuzzSanitize();
    FuzzChannelNames();
    FuzzTraceEncoder();
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
