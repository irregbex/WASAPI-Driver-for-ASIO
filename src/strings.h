// WASAPI Driver for ASIO - interface languages (0.9.0, H4)
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
//   String table of everything the user reads in the control panel, its dialogs, the monitor window, the tray
//   icon and the status texts. Ids: strings_ids.inc; one table per language: strings_en.h (reference),
//   strings_es.h, strings_pt.h (Brazil), strings_fr.h, strings_de.h. T(id) returns the text of the current
//   language, falling back to English for a missing entry (tests/strings_test.cpp makes sure there is none).
//   NOT translated, on purpose: log.txt, WasapiDriverDiag.exe and WasapiDriverTest.exe reports and the
//   documentation (support needs one language), "ASIO", "WASAPI", the product name and the trademark sentence.
//
// Threading
//   The tables are built once, on first use (host thread: the panel, the monitor window and the tray all run
//   there); SetLang / T are host-thread only. The audio thread never uses strings.
#pragma once
#include <cwchar>

namespace lang {

enum StrId : int {
#include "strings_ids.inc"
    S_COUNT
};

enum Lang : int { kEn = 0, kEs, kPt, kFr, kDe, kLangCount };

}  // namespace lang

#include "strings_en.h"
#include "strings_es.h"
#include "strings_pt.h"
#include "strings_fr.h"
#include "strings_de.h"

namespace lang {

// Language codes as stored in config.ini (Language=Auto|en|es|pt|fr|de) and their names shown in the panel
// (each language in its own name, never translated).
static const wchar_t* const kCodes[kLangCount] = {L"en", L"es", L"pt", L"fr", L"de"};
static const wchar_t* const kNative[kLangCount] = {L"English", L"Español", L"Português (Brasil)", L"Français", L"Deutsch"};

struct Tables {
    const wchar_t* t[kLangCount][S_COUNT];
    Tables() {
        for (auto& l : t) for (auto& s : l) s = nullptr;
        Fill_en(t[kEn]); Fill_es(t[kEs]); Fill_pt(t[kPt]); Fill_fr(t[kFr]); Fill_de(t[kDe]);
    }
};
inline const Tables& All() { static const Tables tables; return tables; }
inline int& Current() { static int cur = kEn; return cur; }
inline void SetLang(int l) { Current() = (l >= 0 && l < kLangCount) ? l : kEn; }

// Text of `id` in the current language (English if that entry is missing; never null).
inline const wchar_t* T(int id) {
    if (id < 0 || id >= S_COUNT) return L"";
    const Tables& a = All();
    const wchar_t* s = a.t[Current()][id];
    if (!s || !*s) s = a.t[kEn][id];
    return s ? s : L"";
}
// Text of `id` in a given language (used for the English "Copy status" report).
inline const wchar_t* TIn(int l, int id) {
    if (id < 0 || id >= S_COUNT || l < 0 || l >= kLangCount) return L"";
    const wchar_t* s = All().t[l][id];
    return s && *s ? s : (All().t[kEn][id] ? All().t[kEn][id] : L"");
}

// Config value -> language (0 = Auto: primary language of the Windows UI language, English if not translated).
// cfg: 0 Auto, 1 en, 2 es, 3 pt, 4 fr, 5 de. uiLangId: the LANGID of the Windows UI (GetUserDefaultUILanguage).
inline int Resolve(int cfg, unsigned short uiLangId) {
    if (cfg >= 1 && cfg <= kLangCount) return cfg - 1;
    switch (uiLangId & 0x3FF) {               // PRIMARYLANGID
    case 0x0A: return kEs;                    // LANG_SPANISH
    case 0x16: return kPt;                    // LANG_PORTUGUESE
    case 0x0C: return kFr;                    // LANG_FRENCH
    case 0x07: return kDe;                    // LANG_GERMAN
    default:   return kEn;
    }
}

}  // namespace lang
