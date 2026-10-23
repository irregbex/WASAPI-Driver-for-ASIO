// WASAPI Driver for ASIO - ASIO channel names from device names (header-only, portable)
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
//   Builds the names reported by getChannelInfo: "<short device name> <position>", e.g. "AX-22 In 1",
//   "Kestrel E10 L", "Loopback L" (fictional example devices: a USB audio interface and Bluetooth earbuds). Kept free of Windows types so tests/chnames_test.cpp can check it on Linux; the
//   driver converts the wide device name to the ANSI code page first (CP_ACP, no best fit, '?' replacement).
//   Rules:
//     - ShortName: endpoint names look like "Speakers (Nordvik AX-22)". If the part before the parentheses is a
//       generic endpoint word (Speakers, Headphones, Headset, Microphone, Line, ...), the text inside is used;
//       otherwise the whole name. Surrounding blanks are trimmed.
//     - Positions (outputs): from the WAVEFORMATEXTENSIBLE channel mask, bit order FL FR FC LFE BL BR FLC FRC BC SL
//       SR TC ... ("L", "R", "C", "LFE", "BL", "BR", ...); without a usable mask: "Out n". Inputs: "In n".
//     - Fit: at most 31 characters (ASIOChannelInfo.name is 32 bytes): the device part is shortened, the position
//       is never cut.
//     - Unique: a repeated name gets " #2", " #3"... (again within 31 characters).
//     - 0.9.1 (fix 9): every cut happens on a character boundary of the ANSI code page: Fit and MakeUnique take
//       a lead-byte predicate (the driver passes IsDBCSLeadByteEx(CP_ACP, ...)); the text is walked from its
//       start, so a double-byte character is kept whole or dropped whole, never split.
//
// Threading
//   Pure functions (allocate std::string): host thread only (resolveDevices), never the audio thread.
//
// Pipeline in driver.cpp (buildChannelNames)
//   ShortName decides the device part on an ASCII copy of the wide name (only the generic-word test needs it);
//   that substring of the wide name is converted with FitConverted (via AnsiFit) to fit before the position;
//   Fit joins "<device> <position>"; finally MakeUnique runs over all outputs and over all inputs. The loopback
//   inputs are named "Loopback L" / "Loopback R" directly. Names are copied into ASIOChannelInfo.name (32 bytes,
//   NUL-terminated, hence the 31-byte limit everywhere).
//
// Pitfall
//   All lengths here are BYTES of the ANSI string, not characters. Passing isLead = nullptr treats every byte as
//   a character (correct for single-byte code pages and for the tests).
#pragma once
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace chn {

// Removes leading and trailing spaces and tabs.
inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

// ASCII lower case (bytes >= 0x80 are passed through the C locale's tolower, i.e. unchanged by default).
inline std::string Lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// "Speakers (Nordvik AX-22)" -> "Nordvik AX-22"; "Headphones (Kestrel E10)" -> "Kestrel E10" (fictional devices).
// The generic list also holds "realtek" because onboard codecs of that vendor commonly expose endpoints named
// "Realtek (<connector>)"; the word is a naming rule for that common pattern, not a reference to any device.
// Only the first " (" is considered and the name must end with ')'; the head must match a generic word exactly
// (case-insensitive). A name like "My Speakers (USB)" is kept whole because "my speakers" is not generic.
inline std::string ShortName(const std::string& full) {
    static const char* generic[] = {"speakers", "speaker", "headphones", "headphone", "headset", "headset earphone",
                                    "headset microphone", "microphone", "microphone array", "line", "line in",
                                    "line out", "digital output", "digital audio", "spdif", "s/pdif", "hands-free",
                                    "hands-free ag audio", "stereo mix", "input", "output", "analog", "realtek"};
    const size_t open = full.find(" (");
    if (open != std::string::npos && !full.empty() && full.back() == ')') {
        const std::string head = Lower(Trim(full.substr(0, open)));
        const std::string inner = Trim(full.substr(open + 2, full.size() - open - 3));
        for (const char* g : generic) if (head == g && !inner.empty()) return inner;
    }
    return Trim(full);
}

// 0.9.10: the ASCII copy of a wide device name on which ShortName runs (driver.cpp, buildChannelNames): exactly one
// char per UTF-16 unit, so an index into the copy is the same index into the wide name; ASCII units are copied, every
// other unit (accented letters, CJK, each half of a surrogate pair) becomes '?'. Before 0.9.10 the driver built the
// copy with std::string(w.begin(), w.end()), which keeps only the low byte of each unit: MSVC warned (C4244,
// wchar_t -> char, reported from <xutility>), and a character such as U+4E28 or U+FF09 turned into '(' or a tab,
// which could change the " (" / ")" / blank parsing. '?' is never a blank, a parenthesis or part of a generic word.
inline std::string AsciiShadow(const std::wstring& w) {
    std::string a(w.size(), '?');
    for (size_t i = 0; i < w.size(); ++i) {
        const unsigned long u = (unsigned long)w[i];        // wchar_t: 16-bit unsigned (Windows) or 32-bit (Linux)
        if (u < 0x80) a[i] = static_cast<char>(u);
    }
    return a;
}

// Speaker position label for bit `bit` of a channel mask (SPEAKER_FRONT_LEFT = bit 0 ... SPEAKER_TOP_BACK_RIGHT =
// bit 17, as in ksmedia.h); "" for bits outside that range.
inline const char* PosLabel(int bit) {
    static const char* names[] = {"L", "R", "C", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR", "TC",
                                  "TFL", "TFC", "TFR", "TBL", "TBC", "TBR"};
    return bit >= 0 && bit < 18 ? names[bit] : "";
}

// Position of the channel at `index` for a device with `channels` channels and channel mask `mask`. Returns an
// empty string if the mask does not describe this channel (the caller then uses a number).
// The mask is usable only if its number of set bits equals `channels`; channels are assigned to set bits in
// ascending bit order (the WAVEFORMATEXTENSIBLE rule). A set bit above 17 yields "" for that channel.
inline std::string PositionFromMask(uint32_t mask, int channels, int index) {
    int seen = 0, bits = 0;
    for (int b = 0; b < 32; ++b) if (mask & (1u << b)) ++bits;
    if (bits != channels) return "";
    for (int b = 0; b < 18; ++b) if (mask & (1u << b)) { if (seen == index) return PosLabel(b); ++seen; }
    return "";
}

// Wide text converted with `conv` (int conv(const wchar_t* w, int len, char* out, int cap): bytes written, 0 if
// the text does not fit `cap` or cannot be converted) and shortened by whole UTF-16 characters (a surrogate pair
// is removed as one) until it fits `room` bytes. 0.9.1 (fix 9): 0 for a non-empty text means "too long".
// Shortening in the wide domain and reconverting guarantees that a multi-byte ANSI character is never split.
// Cost is O(len^2) conversions in the worst case; names are short. Returns "" if nothing fits.
template <class Conv> std::string FitConverted(const std::wstring& w, size_t room, Conv conv) {
    std::wstring t = w;
    while (!t.empty()) {
        char b[256] = "";
        const int n = conv(t.c_str(), (int)t.size(), b, 255);
        if (n > 0 && (size_t)n <= room) return Trim(std::string(b, (size_t)n));
        const unsigned last = (unsigned)t.back();
        t.pop_back();
        if (last >= 0xDC00 && last <= 0xDFFF && !t.empty() && (unsigned)t.back() >= 0xD800 && (unsigned)t.back() <= 0xDBFF) t.pop_back();
    }
    return "";
}

// Lead byte of a double-byte character in the ANSI code page (nullptr: single-byte code page).
using LeadByteFn = bool (*)(unsigned char);

// The longest prefix of `s` of at most `n` bytes that ends on a character boundary.
// Walks from the start because in DBCS code pages a trail byte can look like a lead byte; only a forward scan
// knows where characters begin.
inline std::string CutBytes(const std::string& s, size_t n, LeadByteFn isLead = nullptr) {
    if (s.size() <= n) return s;
    size_t i = 0;
    while (i < s.size()) {
        const size_t len = (isLead && isLead((unsigned char)s[i]) && i + 1 < s.size()) ? 2 : 1;
        if (i + len > n) break;
        i += len;
    }
    return s.substr(0, i);
}

// "<device> <suffix>" within maxLen bytes: the device part is shortened (on a character boundary), the suffix kept.
// If the suffix alone does not leave room for a space and one byte, only the (cut) suffix is returned; an empty
// device part also yields the suffix alone.
inline std::string Fit(const std::string& device, const std::string& suffix, size_t maxLen = 31, LeadByteFn isLead = nullptr) {
    if (suffix.size() + 1 >= maxLen) return suffix.substr(0, maxLen);
    std::string d = device;
    const size_t room = maxLen - suffix.size() - 1;
    if (d.size() > room) d = Trim(CutBytes(d, room, isLead));
    return d.empty() ? suffix : d + " " + suffix;
}

// Makes every name unique by appending " #2", " #3"... (fitted within maxLen).
// The first occurrence keeps its name; each later duplicate gets the smallest number not yet taken by an earlier
// name. O(n^2) comparisons, fine for <= a few dozen channels.
inline void MakeUnique(std::vector<std::string>& names, size_t maxLen = 31, LeadByteFn isLead = nullptr) {
    for (size_t i = 0; i < names.size(); ++i) {
        int k = 1;
        auto taken = [&](const std::string& n) { for (size_t j = 0; j < i; ++j) if (names[j] == n) return true; return false; };
        if (!taken(names[i])) continue;
        const std::string base = names[i];
        std::string cand;
        do {
            ++k;
            const std::string tag = " #" + std::to_string(k);
            cand = (base.size() + tag.size() > maxLen ? CutBytes(base, maxLen - tag.size(), isLead) : base) + tag;
        } while (taken(cand));
        names[i] = cand;
    }
}

}  // namespace chn
