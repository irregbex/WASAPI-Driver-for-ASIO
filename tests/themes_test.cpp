// WASAPI Driver for ASIO - offline test of the panel themes (src/themes.h)
// (Linux: g++ -O2 -std=c++17 tests/themes_test.cpp -o th && ./th)
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
//   Readability of every theme: WCAG 2.x contrast ratio >= 4.5:1 for each pair the panel uses for normal text:
//   text / dim text on the window background, cards, tiles, inputs and buttons; accent text (selected tab,
//   badges) on cards and background; warning (amber) and error (red) text on cards and tiles; the text of the
//   primary button (onAccent) on the accent color. Also checks the config keys and the WCAG formula itself
//   (black on white = 21:1). Prints one line per theme and pair group and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/themes.h"
#include <cstdio>

static int g_fail = 0;
int main() {
    printf("Theme contrast offline test (WCAG 2.x, normal text >= 4.5:1)\n");
    const double bw = thm::Contrast(0x000000, 0xFFFFFF);
    printf("  formula: black on white = %.2f:1 %s\n", bw, bw > 20.99 && bw < 21.01 ? "OK" : "FAILED");
    if (!(bw > 20.99 && bw < 21.01)) ++g_fail;
    struct Pair { const char* what; uint32_t thm::Palette::*fg; uint32_t thm::Palette::*bg; };
    const Pair pairs[] = {
        {"text / background", &thm::Palette::text, &thm::Palette::bg},   {"text / card", &thm::Palette::text, &thm::Palette::card},
        {"text / tile", &thm::Palette::text, &thm::Palette::tile},       {"text / input", &thm::Palette::text, &thm::Palette::input},
        {"text / button", &thm::Palette::text, &thm::Palette::btn},      {"dim / card", &thm::Palette::dim, &thm::Palette::card},
        {"dim / background", &thm::Palette::dim, &thm::Palette::bg},     {"dim / tile", &thm::Palette::dim, &thm::Palette::tile},
        {"accent / card", &thm::Palette::accent, &thm::Palette::card},   {"accent / background", &thm::Palette::accent, &thm::Palette::bg},
        {"amber / card", &thm::Palette::amber, &thm::Palette::card},     {"red / card", &thm::Palette::red, &thm::Palette::card},
        {"red / tile", &thm::Palette::red, &thm::Palette::tile},         {"button text / accent", &thm::Palette::onAccent, &thm::Palette::accent},
    };
    for (int i = 0; i < thm::kCount; ++i) {
        const thm::Palette& p = thm::kPalettes[i];
        double worst = 100; const char* worstWhat = "";
        int bad = 0;
        for (const Pair& q : pairs) {
            const double c = thm::Contrast(p.*(q.fg), p.*(q.bg));
            if (c < worst) { worst = c; worstWhat = q.what; }
            if (c < 4.5) { ++bad; printf("    %s: %s only %.2f:1\n", p.key, q.what, c); }
        }
        printf("  %-10s lowest %.2f:1 (%s)  %s\n", p.key, worst, worstWhat, bad ? "FAILED" : "OK");
        if (bad) ++g_fail;
        if (thm::FromKey(p.key) != i) { printf("  %s: key does not round-trip  FAILED\n", p.key); ++g_fail; }
    }
    const bool keys = thm::FromKey("light") == thm::kLight && thm::FromKey("") == thm::kDark && thm::FromKey("nothing") == thm::kDark &&
                      thm::FromKey(nullptr) == thm::kDark;
    printf("  config keys (case-insensitive, unknown = Dark) %s\n", keys ? "OK" : "FAILED");
    if (!keys) ++g_fail;
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
