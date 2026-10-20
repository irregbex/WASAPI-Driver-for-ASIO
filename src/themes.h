// WASAPI Driver for ASIO - color themes of the control panel (header-only, portable)
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
// Purpose (0.9.0, H1)
//   Every color the panel, its dialogs, the monitor window and the meters use, for six themes. The retro themes
//   are ORIGINAL palettes loosely inspired by the look of home computers and terminals of the 1980s / 1990s; no
//   palette, logo, icon, font or graphic of any product is copied and no product name is shown (the UI names are
//   "Retro Blue (16-bit)", "Retro 8-bit", "Green Terminal", "Neon", "Dark" and "Light"; config values are short
//   keys). Colors are 0x00RRGGBB.
//   Readability rule (tests/themes_test.cpp): every pair used for normal text has a WCAG 2.x contrast ratio of at
//   least 4.5:1 (text, dim text, accent, warning and error colors on the backgrounds they are drawn on, and the
//   text of primary buttons on the accent color).
//
// Threading
//   Constant data; the panel copies the selected palette into its Theme on the host thread.
//   panel.cpp converts each 0x00RRGGBB value to a COLORREF (0x00BBGGRR) when copying; do not pass these values to
//   GDI directly. When Windows high-contrast mode is on, the panel ignores the palette and uses system colors.
//
// Pitfalls
//   - kPalettes is indexed by Id: keep the initializer order equal to the enum order (Dark first = default).
//   - The aggregate initializer is positional: a new field in Palette must be added to all six entries in the
//     same position, and every new text/background pair should be added to tests/themes_test.cpp.
#pragma once
#include <cmath>
#include <cstdint>

namespace thm {

// Theme index (also the order of the Theme combo box entries in the panel).
enum Id { kDark = 0, kRetroBlue, kRetro8, kTerminal, kNeon, kLight, kCount };

struct Palette {
    const char* key;            // config value (Theme=...)
    bool dark;                  // native controls and the title bar use the dark system style
    bool dotHeader;             // the header title is drawn with the 5x7 dot font
    // bg: window background; card: grouped sections; input: edit / combo / check box fill; tile: status tiles;
    // text / dim: normal and secondary text; accent (+Hi hover, +Lo pressed): primary buttons, checked boxes,
    // highlights; amber / red: warning / error text and marks; line: separators; btn (+Hi hover, +Lo pressed or
    // disabled): normal buttons; onAccent: text drawn on the accent color.
    uint32_t bg, card, input, tile, text, dim, accent, accentHi, accentLo, amber, red, line, btn, btnHi, btnLo,
             onAccent;
    uint32_t meterLo, meterMid, meterHi;     // meter gradient: low, -18..-6 dB, near 0 dB
    // Meter details: bar background of an active / inactive channel, unlit clip LED, peak-hold marker, dB guide
    // lines of the meter scale.
    uint32_t barOn, barOff, clipOff, hold, guide;
    uint32_t fx1, fx2, fx3;     // effect palette anchors (visuals follow the theme)
};

static const Palette kPalettes[kCount] = {
    // Dark (the 0.6.0 .. 0.8.x look)
    {"Dark", true, false, 0x1E1F22, 0x2B2D31, 0x1A1B1E, 0x232428, 0xE8E8EA, 0x9A9DA3, 0x3DDC97, 0x62E8AE, 0x2AB077,
     0xF5A524, 0xF76D73, 0x3A3C42, 0x383A40, 0x454850, 0x2F3136, 0x0E1411,
     0x2ED57E, 0xF5C542, 0xF76D73, 0x26282C, 0x212225, 0x3A2A2C, 0xF0F0F0, 0x34363B,
     0x2050A0, 0x6030A0, 0x20A0A0},
    // Retro Blue (16-bit): deep blue desktop, lighter blue panels, white text, orange accent
    {"RetroBlue", true, true, 0x0B3A7E, 0x1A4E9C, 0x082C60, 0x0E4189, 0xFFFFFF, 0xC9D6EE, 0xFFB463, 0xFFC888, 0xD98A3A,
     0xFFD24A, 0xFFB4B4, 0x3A6CB8, 0x2C63B3, 0x3A74C6, 0x2457A0, 0x1A1000,
     0x7FD7FF, 0xFFD24A, 0xFF6060, 0x0D3878, 0x0A3068, 0x4A2A50, 0xFFFFFF, 0x2F5FA8,
     0x1040A0, 0xD07020, 0xF0F0F0},
    // Retro 8-bit: blue-violet screen with a lighter border tone, lavender text, mint accent
    {"Retro8", true, true, 0x2A2470, 0x3B3490, 0x221D5C, 0x332D82, 0xD8D2FF, 0xB0A8F0, 0x9CF0A0, 0xBEFAC0, 0x6CC070,
     0xF3E37C, 0xFFA6A0, 0x524AA8, 0x4C44A8, 0x5A52BC, 0x433C98, 0x101830,
     0x9CF0A0, 0xF3E37C, 0xFF8070, 0x2C2676, 0x26206A, 0x4A2A60, 0xF0EEFF, 0x4A42A0,
     0x4038B0, 0x8080F0, 0x60C070},
    // Green Terminal: phosphor green on black
    {"Terminal", true, true, 0x050805, 0x0C140C, 0x020402, 0x091009, 0x45F27A, 0x2FB257, 0x9CFF6A, 0xC0FF9A, 0x5FBF3A,
     0xE9D85A, 0xFF7A6A, 0x1E3A22, 0x16261A, 0x203826, 0x101C12, 0x031006,
     0x2FB257, 0x9CFF6A, 0xE9D85A, 0x0A140B, 0x071008, 0x2A140C, 0xC0FFC8, 0x183020,
     0x0A4010, 0x20A040, 0x70F090},
    // Neon: near-black violet with magenta and cyan
    {"Neon", true, false, 0x0D0B1A, 0x1A1630, 0x0A0815, 0x141127, 0xF2EEFF, 0xA9A0D0, 0xFF4FD8, 0xFF84E4, 0xC23AA4,
     0xFFC24A, 0xFF6B81, 0x2E2850, 0x2A2448, 0x383060, 0x221D3C, 0x16001A,
     0x30E0FF, 0xFF4FD8, 0xFF6B81, 0x161229, 0x110E20, 0x3A1630, 0xFFFFFF, 0x2A2448,
     0xC020C0, 0x2060FF, 0x20E0E0},
    // Light
    {"Light", false, false, 0xE9EBEF, 0xFFFFFF, 0xF6F7F9, 0xF2F4F7, 0x1C1E22, 0x5A5F68, 0x0B7A4B, 0x0E9A5E, 0x086038,
     0x8A5A00, 0xB3261E, 0xD5D9E0, 0xE3E6EB, 0xD6DAE1, 0xEEF0F3, 0xFFFFFF,
     0x1FA35E, 0xD4A017, 0xD0342C, 0xE4E7EC, 0xEEF0F3, 0xF2D6D4, 0x30343A, 0xD5D9E0,
     0x90B0E0, 0xC0A0E0, 0x90D0C0},
};

// Config key -> id (case-insensitive ASCII; unknown or empty = Dark).
inline int FromKey(const char* k) {
    if (!k) return kDark;
    for (int i = 0; i < kCount; ++i) {
        const char* a = kPalettes[i].key; const char* b = k;
        while (*a && *b && ((*a | 32) == (*b | 32))) { ++a; ++b; }
        if (!*a && !*b) return i;
    }
    return kDark;
}

// WCAG 2.x relative luminance and contrast ratio.
//   Each sRGB channel is linearized (v/12.92 below 0.03928, else ((v + 0.055)/1.055)^2.4), then
//   L = 0.2126 R + 0.7152 G + 0.0722 B. Contrast = (L_light + 0.05) / (L_dark + 0.05), from 1:1 to 21:1;
//   4.5:1 is the WCAG AA threshold for normal text. Used by tests/themes_test.cpp (not by the panel at run time).
inline double Luminance(uint32_t c) {
    auto ch = [](double v) { v /= 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * ch((c >> 16) & 255) + 0.7152 * ch((c >> 8) & 255) + 0.0722 * ch(c & 255);
}
inline double Contrast(uint32_t a, uint32_t b) {
    double la = Luminance(a), lb = Luminance(b);
    if (la < lb) { const double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

}  // namespace thm
