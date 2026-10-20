// WASAPI Driver for ASIO - privacy helpers for printed text (header-only)
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
//   Reports, logs and the panel's "Copy status" text may be posted publicly. Before any text is printed or
//   written, SanitizeText() replaces the per-user folders with environment variable names (%LOCALAPPDATA%,
//   %APPDATA%, %TEMP%, %USERPROFILE%) and masks the Windows user name and computer name. ShortHash() turns
//   machine-specific identifiers (endpoint Ids, ContainerIds) into an 8-digit FNV-1a hash, which still lets two
//   lines of the same report be compared but does not identify the machine.
//
// Threading
//   Pure functions over local data, but SanitizeText() calls GetEnvironmentVariableW and allocates, so it must
//   never be called from the real-time audio thread (the audio thread never prints anything anyway).
//
// Order of the replacements in SanitizeText (it matters)
//   1. Known per-user folders from the environment, longest path first, so the most specific name wins
//      (C:\Users\x\AppData\Local\Temp -> %TEMP%, not %LOCALAPPDATA%\Temp). TMP is reported as %TEMP%.
//   2. Any other "X:\Users\<name>" prefix (another profile) -> %USERPROFILE% (Public and Default are kept).
//   3. Remaining occurrences of the user name and computer name as whole words -> <user> / <computer>.
//   Names shorter than 3 characters are not masked (they would match too much unrelated text); this is a
//   best-effort filter, not a guarantee (e.g. a user name inside a device name would be masked too, and a name
//   that differs from USERNAME is not found).
//
// 0.9.8 (hardening): the text functions are pure and portable (no Windows call; the environment is passed in as a
// callable to SanitizeTextWith), so tests/fuzz_test.cpp runs them on Linux under AddressSanitizer and
// UndefinedBehaviorSanitizer. The Windows-only parts (EnvVar, SanitizeText, ReadMicPrivacy) are under _WIN32.
#pragma once
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace wbpriv {

// Strips trailing backslashes / slashes (the form in which the environment's folders are compared).
inline std::wstring StripSlashes(std::wstring s) {
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

#ifdef _WIN32
// Value of an environment variable, without a trailing backslash; empty if not set.
// Values of 1024 characters or more are treated as unset (fixed stack buffer, no allocation for the read).
inline std::wstring EnvVar(const wchar_t* name) {
    wchar_t b[1024];
    DWORD n = GetEnvironmentVariableW(name, b, 1024);
    if (n == 0 || n >= 1024) return {};
    return StripSlashes(std::wstring(b, n));
}
#endif

// Case-insensitive comparison of n characters (towlower, like _wcsnicmp in the C locale). 0.9.8: portable.
inline bool EqNI(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; ++i) if (towlower((wint_t)a[i]) != towlower((wint_t)b[i])) return false;
    return true;
}
inline bool EqI(const std::wstring& a, const wchar_t* b) { return a.size() == wcslen(b) && EqNI(a.c_str(), b, a.size()); }

// Case-insensitive search of `what` in `s` starting at `from`. Returns npos if absent.
// Naive O(n * m) scan; fine for log-sized text.
inline size_t FindI(const std::wstring& s, const std::wstring& what, size_t from) {
    if (what.empty() || s.size() < what.size()) return std::wstring::npos;
    for (size_t i = from; i + what.size() <= s.size(); ++i)
        if (EqNI(s.c_str() + i, what.c_str(), what.size())) return i;
    return std::wstring::npos;
}

// Replaces every case-insensitive occurrence of `what` with `with`. wholeWord: only when not surrounded by letters
// or digits (used for the user and computer names, so that "Al" never rewrites "All").
inline void ReplaceAllI(std::wstring& s, const std::wstring& what, const std::wstring& with, bool wholeWord) {
    size_t i = 0;
    while ((i = FindI(s, what, i)) != std::wstring::npos) {
        const bool leftOk = i == 0 || !iswalnum((wint_t)s[i - 1]);
        const bool rightOk = i + what.size() >= s.size() || !iswalnum((wint_t)s[i + what.size()]);
        if (wholeWord && !(leftOk && rightOk)) { i += 1; continue; }
        s.replace(i, what.size(), with);
        i += with.size();
    }
}

}  // namespace wbpriv

// Returns `text` with per-user folders replaced by %VAR% names (longest prefix first, so %LOCALAPPDATA%\Temp
// becomes %TEMP%) and the user and computer names masked. Safe to call repeatedly (idempotent).
// 0.9.8: env(name) returns the value of an environment variable ("" if unset; trailing slashes are stripped here);
// the Windows build passes EnvVar (SanitizeText below), the fuzz test passes random values.
template <class Env>
inline std::wstring SanitizeTextWith(std::wstring text, Env env) {
    using namespace wbpriv;
    struct Rep { std::wstring path, name; };
    std::vector<Rep> reps;
    const wchar_t* vars[] = {L"TEMP", L"TMP", L"LOCALAPPDATA", L"APPDATA", L"USERPROFILE"};
    for (const wchar_t* v : vars) {
        std::wstring p = StripSlashes(env(v));
        if (p.size() >= 3) reps.push_back({p, std::wstring(L"%") + (wcscmp(v, L"TMP") ? v : L"TEMP") + L"%"});
    }
    std::sort(reps.begin(), reps.end(), [](const Rep& a, const Rep& b) { return a.path.size() > b.path.size(); });
    for (const Rep& r : reps) ReplaceAllI(text, r.path, r.name, false);
    // Any remaining "<drive>:\Users\<name>" style prefix (e.g. another account's profile).
    // The name ends at a backslash, quote, space or line break; the replacement starts at the drive letter (i - 1)
    // and the scan resumes just after the inserted "%USERPROFILE%" (13 characters).
    const std::wstring users = L":\\Users\\";
    for (size_t i = 0; (i = FindI(text, users, i)) != std::wstring::npos;) {
        size_t a = i + users.size(), b = a;
        while (b < text.size() && text[b] != L'\\' && text[b] != L'"' && text[b] != L' ' && text[b] != L'\r' && text[b] != L'\n') ++b;
        const std::wstring who = text.substr(a, b - a);
        if (b > a && i > 0 && !EqI(who, L"Public") && !EqI(who, L"Default")) { text.replace(i - 1, b - (i - 1), L"%USERPROFILE%"); i = i - 1 + 13; }
        else i = a;
    }
    const std::wstring user = StripSlashes(env(L"USERNAME")), host = StripSlashes(env(L"COMPUTERNAME"));
    if (user.size() >= 3) ReplaceAllI(text, user, L"<user>", true);
    if (host.size() >= 3) ReplaceAllI(text, host, L"<computer>", true);
    return text;
}

#ifdef _WIN32
// The Windows form: the real environment of this process. Host/console threads only.
inline std::wstring SanitizeText(std::wstring text) { return SanitizeTextWith(std::move(text), wbpriv::EnvVar); }
#endif

// 32-bit FNV-1a hash of `s` (case-insensitive) as 8 hex digits prefixed with '#'. Not cryptographic: it only keeps
// identifiers comparable inside one report without printing them.
// FNV-1a: h = 2166136261; for each char: h ^= c; h *= 16777619 (32-bit wrap). Stable across runs and machines,
// so the same Id also hashes the same in two reports of the same machine.
inline std::wstring ShortHash(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) { h ^= (uint32_t)towlower((wint_t)c); h *= 16777619u; }
    wchar_t b[16]; swprintf(b, 16, L"#%08X", h);
    return b;
}

#ifdef _WIN32
// 0.7.0 (F9): Windows microphone privacy for desktop (non-packaged) programs, read from
// Software\Microsoft\Windows\CurrentVersion\CapabilityAccessManager\ConsentStore\microphone ("Value") in HKLM
// (device-wide switch) and HKCU (user switch), and its "NonPackaged" subkey in HKCU ("Let desktop apps access your
// microphone"). Each value is "Allow", "Deny" or missing (missing = allowed). Read-only, any thread except the
// audio thread (registry access).
struct MicPrivacy {
    std::wstring machine, user, desktop;   // raw values ("" = missing)
    bool denied = false;                   // desktop apps cannot use recording devices
};
// Reads the three values; denied if any of them is "Deny" (case-insensitive). A missing key or value reads as "".
inline MicPrivacy ReadMicPrivacy() {
    MicPrivacy r;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
    auto rd = [](HKEY root, const std::wstring& k) {
        wchar_t b[64] = L""; DWORD n = sizeof b;
        if (RegGetValueW(root, k.c_str(), L"Value", RRF_RT_REG_SZ, nullptr, b, &n) != ERROR_SUCCESS) return std::wstring();
        return std::wstring(b);
    };
    r.machine = rd(HKEY_LOCAL_MACHINE, key);
    r.user = rd(HKEY_CURRENT_USER, key);
    r.desktop = rd(HKEY_CURRENT_USER, std::wstring(key) + L"\\NonPackaged");
    r.denied = !_wcsicmp(r.machine.c_str(), L"Deny") || !_wcsicmp(r.user.c_str(), L"Deny") || !_wcsicmp(r.desktop.c_str(), L"Deny");
    return r;
}
#endif  // _WIN32
