// WASAPI Driver for ASIO - offline test of the ASIO channel names (src/chnames.h)
// (Linux: g++ -O2 -std=c++17 tests/chnames_test.cpp -o ct && ./ct)
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
// Purpose (0.8.0, G6)
//   Checks the naming rules used for ASIOChannelInfo.name: the short device name (generic words such as
//   "Speakers" or "Headphones" replaced by the text in parentheses), speaker positions from the channel mask,
//   numbering when there is no usable mask, the 31-character limit (the position is never cut) and uniqueness.
//   The device names are fictional examples (a USB audio interface "Nordvik AX-22" and Bluetooth earbuds
//   "Kestrel E10") chosen to exercise the same rules as real endpoint names: brand + model inside parentheses,
//   a bare model name, and a long name that has to be cut at exactly 31 bytes. Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/chnames.h"
#include <cstdio>

static int g_fail = 0;
static void Eq(const std::string& got, const std::string& want, const char* what) {
    const bool ok = got == want;
    printf("  %-44s %s  (\"%s\")\n", what, ok ? "OK" : "FAILED", got.c_str());
    if (!ok) { printf("      expected \"%s\"\n", want.c_str()); ++g_fail; }
}
static void True(bool ok, const char* what) { printf("  %-44s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }

int main() {
    printf("Channel name offline test\n");
    Eq(chn::ShortName("Speakers (Nordvik AX-22)"), "Nordvik AX-22", "generic word -> name in parentheses");
    Eq(chn::ShortName("Headphones (Kestrel E10)"), "Kestrel E10", "headphones");
    Eq(chn::ShortName("Line (AX-22)"), "AX-22", "line input");
    Eq(chn::ShortName("Studio Monitors (USB Audio)"), "Studio Monitors (USB Audio)", "non-generic name kept whole");
    Eq(chn::ShortName("  AX-22  "), "AX-22", "blanks trimmed");
    Eq(chn::ShortName("Speakers (Nordvik AX-22"), "Speakers (Nordvik AX-22", "no closing parenthesis: kept whole");
    // Positions from WAVEFORMATEXTENSIBLE masks
    Eq(chn::PositionFromMask(0x3, 2, 0), "L", "stereo mask, channel 1");
    Eq(chn::PositionFromMask(0x3, 2, 1), "R", "stereo mask, channel 2");
    Eq(chn::PositionFromMask(0x3F, 6, 3), "LFE", "5.1 mask, channel 4");
    Eq(chn::PositionFromMask(0x60F, 6, 4), "SL", "5.1 side mask, channel 5");
    Eq(chn::PositionFromMask(0x3, 4, 2), "", "mask does not match channel count");
    Eq(chn::PositionFromMask(0, 2, 0), "", "no mask");
    // Fit within 31 characters
    Eq(chn::Fit("AX-22", "In 1"), "AX-22 In 1", "device + number");
    Eq(chn::Fit("Kestrel E10", "L"), "Kestrel E10 L", "device + position");
    // Exact 31-byte boundary: 26 + 1 + 4 = 31 bytes is kept as is; one more byte cuts the device part only.
    Eq(chn::Fit("Nordvik AX-22 Studio Inter", "In 1"), "Nordvik AX-22 Studio Inter In 1", "exactly 31 bytes kept");
    Eq(chn::Fit("Nordvik AX-22 Studio Interface", "In 1"), "Nordvik AX-22 Studio Inter In 1", "32+ bytes: device cut, suffix kept");
    Eq(chn::Fit("Nordvik AX-22 Studio Interface", "Out 10"), "Nordvik AX-22 Studio Int Out 10", "longer suffix: shorter device part");
    // A cut that ends right after a blank (room 21 = "Nordvik AX-22 Studio ") drops that blank: 30 bytes, not 31.
    Eq(chn::Fit("Nordvik AX-22 Studio Interface", "Monitor L"), "Nordvik AX-22 Studio Monitor L", "cut on a blank: blank trimmed");
    const std::string longName = chn::Fit("A very long audio interface name with many words", "Out 12");
    True(longName.size() <= 31, "long name fits 31 characters");
    True(longName.size() >= 6 && longName.compare(longName.size() - 6, 6, "Out 12") == 0, "position kept at the end");
    Eq(chn::Fit("", "Loopback L"), "Loopback L", "empty device");
    // Uniqueness
    std::vector<std::string> v = {"USB Audio L", "USB Audio R", "USB Audio L", "USB Audio R", "USB Audio L"};
    chn::MakeUnique(v);
    Eq(v[2], "USB Audio L #2", "duplicate gets #2");
    Eq(v[4], "USB Audio L #3", "third copy gets #3");
    bool unique = true;
    for (size_t i = 0; i < v.size(); ++i) for (size_t j = i + 1; j < v.size(); ++j) if (v[i] == v[j]) unique = false;
    True(unique, "all names unique");
    std::vector<std::string> w = {std::string(31, 'x'), std::string(31, 'x')};
    chn::MakeUnique(w);
    True(w[1].size() <= 31 && w[1] != w[0], "unique within 31 characters");

    // 0.9.1 (fix 9): character boundaries in a double-byte code page (Shift-JIS-like model: lead 0x81..0x9F or
    // 0xE0..0xFC followed by one trail byte) and the "0 = does not fit" rule of the converter.
    printf("Double-byte code page and converter rules (0.9.1)\n");
    auto lead = [](unsigned char c) { return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC); };
    chn::LeadByteFn isLead = +lead;
    auto okBoundaries = [&](const std::string& s) {      // every lead byte is followed by its trail byte
        for (size_t i = 0; i < s.size(); ++i) if (isLead((unsigned char)s[i])) { if (i + 1 >= s.size()) return false; ++i; }
        return true;
    };
    std::string dbcs;                                     // "AB" + 14 double-byte characters = 30 bytes
    dbcs += "AB";
    for (int i = 0; i < 14; ++i) { dbcs += (char)0x82; dbcs += (char)0xA0; }
    for (size_t n = 0; n <= dbcs.size(); ++n) {
        const std::string c = chn::CutBytes(dbcs, n, isLead);
        if (c.size() > n || !okBoundaries(c) || c.size() + 1 < n - (n > 0 ? 1 : 0)) { True(false, "CutBytes boundary"); break; }
    }
    True(chn::CutBytes(dbcs, 3, isLead) == "AB" && chn::CutBytes(dbcs, 4, isLead).size() == 4, "CutBytes never splits a character");
    const std::string fd = chn::Fit(dbcs, "In 1", 31, isLead);
    True(fd.size() <= 31 && okBoundaries(fd) && fd.substr(fd.size() - 4) == "In 1", "Fit keeps whole characters and the suffix");
    std::vector<std::string> dv = {dbcs + "C", dbcs + "C"};
    chn::MakeUnique(dv, 31, isLead);
    True(dv[1].size() <= 31 && dv[1] != dv[0] && okBoundaries(dv[1]) && dv[1].substr(dv[1].size() - 3) == " #2", "MakeUnique cuts on character boundaries");
    std::vector<std::string> dv2 = {dbcs + "C", dbcs + "C"};
    chn::MakeUnique(dv2);                                  // without the predicate a split is possible (documents why)
    True(dv2[1].size() <= 31, "MakeUnique without predicate stays within 31 bytes");
    // converter model: ASCII = 1 byte, anything else = 2 bytes, 0 if it does not fit `cap` (like WideCharToMultiByte)
    int calls = 0;
    auto conv = [&](const wchar_t* t, int len, char* out, int cap) {
        ++calls;
        int n = 0;
        for (int i = 0; i < len; ++i) n += t[i] < 0x80 ? 1 : 2;
        if (n > cap) return 0;
        int k = 0;
        for (int i = 0; i < len; ++i) { if (t[i] < 0x80) out[k++] = (char)t[i]; else { out[k++] = (char)0x82; out[k++] = (char)0xA0; } }
        return k;
    };
    std::wstring wide = L"Device ";
    for (int i = 0; i < 20; ++i) wide += (wchar_t)0x3042;  // 7 + 40 bytes
    const std::string fc = chn::FitConverted(wide, 20, conv);
    True(fc.size() <= 20 && fc.size() >= 19 && okBoundaries(fc), "FitConverted shortens to the room");
    std::wstring huge(300, (wchar_t)0x3042);                 // 600 bytes: the converter returns 0 (buffer too small)
    calls = 0;
    const std::string fh = chn::FitConverted(huge, 20, conv);
    True(!fh.empty() && fh.size() <= 20 && calls > 1, "FitConverted: 0 for a non-empty text means too long");
    std::wstring sur = L"Mic ";
    sur += (wchar_t)0xD83C; sur += (wchar_t)0xDFA4;         // one surrogate pair (4 bytes in this model)
    const std::string fs = chn::FitConverted(sur, 5, conv);
    True(fs == "Mic", "FitConverted removes a surrogate pair as one character");
    True(chn::FitConverted(L"", 10, conv).empty(), "FitConverted of an empty text");
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
