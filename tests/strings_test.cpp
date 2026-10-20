// WASAPI Driver for ASIO - offline test of the interface string tables (src/strings*.h)
// (Linux: g++ -O2 -std=c++17 tests/strings_test.cpp -o st && ./st)
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
// Purpose (0.9.0, H4)
//   1. every string id has a non-empty text in every language;
//   2. the printf placeholders (%ls, %ld, %.1f, %%, ...) are the same, in the same order, as in English, so a
//      translation can never crash swprintf or show the wrong value;
//   3. language resolution: Auto follows the Windows UI language (Spanish, Portuguese, French, German), anything
//      else falls back to English; explicit choices win;
//   4. "ASIO" and "WASAPI" are never translated away: an English text containing them keeps them everywhere.
//   Also prints the longest translation relative to English per language (layout information).
//   5. 0.9.2 help tips (src/tips.h): every control and painted item of kRequired has a tip, every tip id points
//      to a string, every tip is at most 220 characters in every language, no key is listed twice, and the
//      tags are valid.
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/strings.h"
#include "../src/tips.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace lang;
static int g_fail = 0;
static void Check(bool ok, const char* what, long v) {
    printf("  %-60s %s  (%ld)\n", what, ok ? "OK" : "FAILED", v);
    if (!ok) ++g_fail;
}
// Sequence of conversion specifications, e.g. "%ls|%.1f|%%".
static std::vector<std::wstring> Specs(const wchar_t* s) {
    std::vector<std::wstring> v;
    for (const wchar_t* p = s; *p; ++p) {
        if (*p != L'%') continue;
        std::wstring sp = L"%";
        ++p;
        while (*p && wcschr(L"-+ #0123456789.", *p)) sp += *p++;
        while (*p && wcschr(L"hlLqjzt", *p)) sp += *p++;
        if (!*p) { v.push_back(sp + L"<end>"); break; }
        sp += *p;
        v.push_back(sp);
    }
    return v;
}
static std::string Narrow(const wchar_t* s) { std::string r; for (; *s; ++s) r += *s < 128 ? (char)*s : '?'; return r; }

int main() {
    printf("Interface strings offline test (%d ids, %d languages)\n", (int)S_COUNT, (int)kLangCount);
    const char* names[kLangCount] = {"en", "es", "pt", "fr", "de"};
    for (int l = 0; l < kLangCount; ++l) {
        long missing = 0, badSpec = 0, lostName = 0; double worst = 0; int worstId = -1;
        for (int i = 0; i < S_COUNT; ++i) {
            const wchar_t* s = All().t[l][i];
            const wchar_t* e = All().t[kEn][i];
            if (!s || !*s) { ++missing; printf("    %s: id %d missing\n", names[l], i); continue; }
            if (Specs(s) != Specs(e)) { ++badSpec; printf("    %s: placeholders differ in \"%s\"\n", names[l], Narrow(e).c_str()); }
            for (const wchar_t* keep : {L"ASIO", L"WASAPI"})
                if (wcsstr(e, keep) && !wcsstr(s, keep)) { ++lostName; printf("    %s: \"%ls\" lost in id %d\n", names[l], keep, i); }
            const double r = (double)wcslen(s) / (double)(wcslen(e) ? wcslen(e) : 1);
            if (wcslen(e) >= 6 && r > worst) { worst = r; worstId = i; }
        }
        std::string w = std::string(names[l]) + ": every id present and not empty";
        Check(missing == 0, w.c_str(), missing);
        w = std::string(names[l]) + ": placeholders identical to English";
        Check(badSpec == 0, w.c_str(), badSpec);
        w = std::string(names[l]) + ": ASIO / WASAPI kept";
        Check(lostName == 0, w.c_str(), lostName);
        if (worstId >= 0) printf("    %s: longest relative to English x%.2f: \"%s\"\n", names[l], worst, Narrow(All().t[l][worstId]).c_str());
    }
    Check(Resolve(0, 0x0C0A) == kEs && Resolve(0, 0x2C0A) == kEs, "Auto: Spanish (Spain, Argentina) -> es", Resolve(0, 0x0C0A));
    Check(Resolve(0, 0x0416) == kPt && Resolve(0, 0x040C) == kFr && Resolve(0, 0x0407) == kDe, "Auto: pt-BR, fr-FR, de-DE", 0);
    Check(Resolve(0, 0x0411) == kEn && Resolve(0, 0x0409) == kEn, "Auto: Japanese / English -> en", Resolve(0, 0x0411));
    Check(Resolve(5, 0x0C0A) == kDe && Resolve(1, 0x0407) == kEn, "explicit language wins over Windows", Resolve(5, 0x0C0A));
    // 0.9.2: help tips
    {
        long noTip = 0, tooLong = 0, badId = 0, dup = 0, badTag = 0;
        for (const tips::Item& it : tips::kRequired) {
            const int key = tips::KeyFor(it.ctx, it.id);
            if (!tips::Find(key)) { ++noTip; printf("    control %d (context %d) has no tip\n", it.id, it.ctx); }
        }
        const int n = (int)(sizeof tips::kDefs / sizeof tips::kDefs[0]);
        for (int i = 0; i < n; ++i) {
            const tips::Def& d = tips::kDefs[i];
            for (int k = i + 1; k < n; ++k) if (tips::kDefs[k].key == d.key) ++dup;
            if (d.tag < tips::kNone || d.tag > tips::kReset) ++badTag;
            if (d.str < 0 || d.str >= S_COUNT) { ++badId; continue; }
            for (int l = 0; l < kLangCount; ++l) {
                const wchar_t* t = All().t[l][d.str];
                if (!t || wcslen(t) > 220) { ++tooLong; printf("    %s: tip of key %d longer than 220 characters\n", names[l], d.key); }
            }
        }
        for (int l = 0; l < kLangCount; ++l)
            for (int id : {S_TIP_TAG_LIVE, S_TIP_TAG_RESET, S_HELP_TIPS}) if (!All().t[l][id] || !*All().t[l][id]) ++badId;
        Check(noTip == 0, "help tips: every control and item has a tip", noTip);
        Check(tooLong == 0, "help tips: at most 220 characters in every language", tooLong);
        Check(badId == 0 && dup == 0 && badTag == 0, "help tips: valid string ids, no duplicate key, valid tags", badId + dup + badTag);
        printf("    %d tips for %d controls and items\n", n, (int)(sizeof tips::kRequired / sizeof tips::kRequired[0]));
    }
    SetLang(kFr);
    Check(wcscmp(T(S_APPLY), L"Appliquer") == 0 && wcscmp(TIn(kEn, S_APPLY), L"Apply") == 0, "T() / TIn() lookups", 0);
    SetLang(99);
    Check(Current() == kEn && *T(S_COUNT) == 0, "invalid language and id are safe", Current());
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
