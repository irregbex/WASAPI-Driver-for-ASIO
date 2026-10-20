// WASAPI Driver for ASIO - tray status icon composition (0.9.3, header-only, portable)
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
//   The notification-area icon is the application icon with a status dot in its bottom-right corner: a disc of
//   about 40 % of the icon size with a 1 px dark outline, so it reads on light and dark taskbars. Kept free of
//   Windows types so tests/icon_test.cpp can check it on the real icon images; panel.cpp (TrayState) builds the
//   four HICONs from it at the tray's icon size.
//   Pixels: size x size, row 0 at the top, 0xAARRGGBB with straight (not premultiplied) alpha.
//   State: Pick() decides gray / green / amber / red from the status (red stays at least 10 s after the last
//   dropout, so short stutters remain visible).
// Threading: pure functions; the caller owns the buffers. Host (msg) thread only in the driver.
//
// Rendering method (AddStatusDot)
//   Each pixel is supersampled 4 x 4 (16 sample points). inOut = samples inside the outer circle (radius R) gives
//   the coverage a = inOut / 16 (anti-aliased edge); inIn = samples inside the inner circle (R - 1 px) gives the
//   share t of fill color versus outline color within the covered part. The resulting color is then composited
//   "over" the existing pixel with straight alpha:
//     outA = a + dstA * (1 - a);  outC = (srcC * a + dstC * dstA * (1 - a)) / outA
//   Only the bounding square of the dot is visited. Colors are 0xRRGGBB in kColor; alpha is added here.
//   panel.cpp rebuilds the four icons when the tray icon size changes (DPI) and swaps them by state.
#pragma once
#include <cmath>
#include <cstdint>

namespace tray {

// Status shown by the dot; the values index kColor and the panel's icon / tooltip-word arrays.
enum State { kStopped = 0, kOk, kHighLoad, kDropouts, kStateCount };
// Fixed colors (not the theme): they mean the same in every theme. Order: gray, green, amber, red.
constexpr uint32_t kColor[kStateCount] = {0x8A8D93, 0x3DDC97, 0xF5A524, 0xF2545B};
constexpr uint32_t kOutline = 0x101114;
constexpr uint32_t kHoldMs = 10000;       // red at least this long after the last dropout

// Diameter of the dot for an icon of `size` pixels (40 %, at least 6).
inline int DotDiameter(int size) { const int d = (int)std::lround(size * 0.40); return d < 6 ? 6 : d; }

// Draws the status dot of color `rgb` (0xRRGGBB) over `px` (straight alpha, "over" compositing).
// px must hold size * size pixels; the dot touches the right and bottom edges (see the method above).
inline void AddStatusDot(uint32_t* px, int size, uint32_t rgb) {
    const double D = DotDiameter(size), R = D / 2.0, Rin = R - 1.0;      // 1 px outline
    const double cx = size - R, cy = size - R;                           // touching the right and bottom edges
    const int x0 = (int)std::floor(cx - R), y0 = (int)std::floor(cy - R);
    for (int y = y0 < 0 ? 0 : y0; y < size; ++y)
        for (int x = x0 < 0 ? 0 : x0; x < size; ++x) {
            int inOut = 0, inIn = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const double dx = x + (sx + 0.5) / 4.0 - cx, dy = y + (sy + 0.5) / 4.0 - cy, d = std::sqrt(dx * dx + dy * dy);
                    if (d <= R) ++inOut;
                    if (d <= Rin) ++inIn;
                }
            if (!inOut) continue;
            const double a = inOut / 16.0, t = (double)inIn / inOut;    // t: share of fill inside the outline
            double sc[3];
            for (int k = 0; k < 3; ++k) {
                const int sh = 16 - 8 * k;
                sc[k] = ((rgb >> sh) & 255) * t + ((kOutline >> sh) & 255) * (1.0 - t);
            }
            uint32_t& p = px[y * size + x];
            const double da = ((p >> 24) & 255) / 255.0, oa = a + da * (1.0 - a);
            uint32_t out = (uint32_t)std::lround(oa * 255.0) << 24;
            for (int k = 0; k < 3; ++k) {
                const int sh = 16 - 8 * k;
                const double dc = (p >> sh) & 255;
                const double c = oa > 0 ? (sc[k] * a + dc * da * (1.0 - a)) / oa : 0.0;
                out |= (uint32_t)std::lround(c < 0 ? 0 : c > 255 ? 255 : c) << sh;
            }
            p = out;
        }
}

// State from the status: stopped when no stream runs; red while dropouts happened in the last kHoldMs
// (lastDropMs = time of the last increase of the dropout counters, 0 = never); amber above 80 % DSP load.
// Priority: stopped > dropouts > high load > ok. Times in ms from the same monotonic clock; loadNow in percent.
// nowMs - lastDropMs is unsigned: the caller must not pass a lastDropMs later than nowMs.
inline State Pick(bool running, float loadNow, uint64_t nowMs, uint64_t lastDropMs) {
    if (!running) return kStopped;
    if (lastDropMs && nowMs - lastDropMs < kHoldMs) return kDropouts;
    if (loadNow > 80.f) return kHighLoad;
    return kOk;
}

}  // namespace tray
