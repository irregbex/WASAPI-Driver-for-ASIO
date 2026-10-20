// WASAPI Driver for ASIO - animated panel backgrounds (header-only, portable software renderer)
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
// Purpose (0.8.1)
//   Small "demoscene style" backgrounds for the control panel, written from scratch with classic, generic
//   techniques (no code, art, music or font taken from any existing production):
//     Plasma     sum of sines per column / row / diagonal, through a cycling 256-color palette;
//     Starfield  3D points flying towards the viewer, perspective projection, brightness by depth;
//     Copper     horizontal gradient bars moving on sine paths (the "copper / raster bar" look);
//     Tunnel     per-pixel angle / distance lookup table (built on resize) into a procedural texture;
//     Reactive   plasma + stars whose speed and brightness follow an audio level supplied by the caller;
//     Scroller   starfield with a sine-wave text scroller drawn with the built-in 5x7 dot font below.
//   0.9.0 (H2):
//     Metaballs  five soft blobs on sine paths; each adds a precomputed falloff (radius-limited lookup table) to an
//                8-bit field, the field indexes the palette (a contour band makes the surfaces visible);
//     Fire       classic heat buffer: random sparks on the bottom row, each cell becomes the decayed average of the
//                cells below; stepped at a fixed 60 Hz simulation rate from the real elapsed time;
//     Vector     a rotating wireframe icosahedron (abstract polyhedron, 12 vertices / 30 edges), perspective,
//                depth-shaded lines (integer Bresenham with saturated additive color);
//     Twister    a vertical square column twisted along its height; for every row the four edges are rotated by
//                an angle that varies with the row and the time, the visible faces are shaded by their width.
//   Every palette is built from three anchor colors (setPalette) so the effects follow the panel theme.
//   Output: 0x00RRGGBB pixels, top-down rows, at a LOW resolution (at most 320 x 240; the panel scales it up
//   with nearest-neighbor stretching for the chunky-pixel look). The result is the theme background color plus
//   the effect scaled by `intensity` (0 = plain background, 100 = full effect); palettes are kept dark so text
//   drawn on top stays readable.
//
// Real-time rules (this runs on the DAW's UI thread at up to 30 frames per second)
//   - all memory (pixel buffer, tunnel tables) is allocated in resize() only; frame() never allocates;
//   - integer and table math only in frame() (sine table, palettes, lookup tables built in the constructor or
//     in resize());
//   - every pixel write goes through std::vector::operator[], so a build with -D_GLIBCXX_ASSERTIONS (the
//     offline test) checks every index.
//   - deterministic: the output depends only on (effect, size, the sequence of times, intensity, level, speed);
//   - time-based (0.8.2): movement depends on the elapsed time, never on the number of frames.
//
// Threading
//   One Renderer per window, used by one thread (the host thread in the driver). No global state.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vis {

enum Effect { kOff = 0, kPlasma, kStarfield, kCopper, kTunnel, kReactive, kScroller,
              kMetaballs, kFire, kVector, kTwister,      // 0.9.0 (H2)
              kEffectCount };

constexpr int kMaxW = 320, kMaxH = 240;
constexpr int kStars = 192;

// 5x7 dot-matrix font (own design, uppercase, digits and a few signs). Each glyph: 7 rows, bit 4 = left column.
struct Glyph { char c; uint8_t rows[7]; };
static const Glyph kFont[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}}, {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
};

class Renderer {
public:
    Renderer() {
        for (int i = 0; i < 1024; ++i) sinT_[i] = (int16_t)std::lround(127.0 * std::sin(i * 6.283185307179586 / 1024.0));
        // Plasma palette: smooth, cyclic, dark blue / violet / teal (maximum about 0x90 per channel).
        for (int i = 0; i < 256; ++i) {
            const int r = 40 + sinT_[(i * 4 + 0) & 1023] * 40 / 127, g = 34 + sinT_[(i * 4 + 340) & 1023] * 34 / 127,
                      b = 80 + sinT_[(i * 4 + 680) & 1023] * 64 / 127;
            palPlasma_[i] = Rgb(r, g, b);
            const int tr = 60 + sinT_[(i * 8 + 200) & 1023] * 60 / 127, tg = 40 + sinT_[(i * 8 + 600) & 1023] * 40 / 127;
            palTunnel_[i] = Rgb(tr, tg, 110 + sinT_[(i * 8) & 1023] * 90 / 127);
        }
        // 0.9.0: fire / metaball palettes and the vector color for the default (Dark) anchors; setPalette replaces them
        {
            uint32_t keepP[256], keepT[256];
            for (int i = 0; i < 256; ++i) { keepP[i] = palPlasma_[i]; keepT[i] = palTunnel_[i]; }
            setPalette(0x2050A0, 0x6030A0, 0x20A0A0);
            for (int i = 0; i < 256; ++i) { palPlasma_[i] = keepP[i]; palTunnel_[i] = keepT[i]; }
        }
        // Icosahedron: 12 vertices (0, +-1, +-phi) cyclic, edges = pairs at the minimal distance (2).
        {
            const double ph = 1.6180339887498949;
            const double v[12][3] = {{0, 1, ph}, {0, -1, ph}, {0, 1, -ph}, {0, -1, -ph}, {1, ph, 0}, {-1, ph, 0}, {1, -ph, 0}, {-1, -ph, 0},
                                     {ph, 0, 1}, {-ph, 0, 1}, {ph, 0, -1}, {-ph, 0, -1}};
            for (int i = 0; i < 12; ++i) for (int k = 0; k < 3; ++k) ico_[i][k] = (int)std::lround(v[i][k] * 600.0);
            nEdges_ = 0;
            for (int i = 0; i < 12; ++i)
                for (int j = i + 1; j < 12; ++j) {
                    double d2 = 0; for (int k = 0; k < 3; ++k) d2 += (v[i][k] - v[j][k]) * (v[i][k] - v[j][k]);
                    if (std::fabs(d2 - 4.0) < 1e-6 && nEdges_ < 30) { edges_[nEdges_][0] = (uint8_t)i; edges_[nEdges_][1] = (uint8_t)j; ++nEdges_; }
                }
        }
        // Procedural tunnel texture (256 x 256): XOR pattern softened by a ring term.
        for (int v = 0; v < 256; ++v)
            for (int u = 0; u < 256; ++u)
                tex_[v * 256 + u] = (uint8_t)(((u ^ v) & 0xFF) / 2 + (sinT_[(v * 8) & 1023] + 127) / 4);
        for (int i = 0; i < 128; ++i) glyphOf_[i] = -1;
        for (int g = 0; g < (int)(sizeof(kFont) / sizeof(kFont[0])); ++g) glyphOf_[(int)kFont[g].c] = g;
        setText("WASAPI DRIVER FOR ASIO");
        reset();
    }

    int width() const { return w_; }
    int height() const { return h_; }
    const uint32_t* pixels() const { return px_.empty() ? nullptr : px_.data(); }

    // 0.9.0 (H1): rebuilds the plasma / tunnel / effect palettes from three theme colors (0x00RRGGBB): a smooth,
    // cyclic blend a -> b -> c -> a (dark enough that text on top stays readable at the default intensity).
    void setPalette(uint32_t a, uint32_t b, uint32_t c) {
        const uint32_t key[3] = {a, b, c};
        for (int i = 0; i < 256; ++i) {
            const int seg = i * 3 / 256, f = (i * 3) % 256;          // segment and position inside it
            const uint32_t p = key[seg], q = key[(seg + 1) % 3];
            auto ch = [&](int sh) { return (int)((p >> sh) & 255) + ((int)((q >> sh) & 255) - (int)((p >> sh) & 255)) * f / 256; };
            const int r = ch(16), g = ch(8), bl = ch(0);
            palPlasma_[i] = Rgb(r * 3 / 4, g * 3 / 4, bl * 3 / 4);
            palTunnel_[(i + 64) & 255] = Rgb(r, g, bl);
        }
        // fire: black -> c -> b -> a bright -> white-ish (heat), metaballs: blend of b and c
        for (int i = 0; i < 256; ++i) {
            const uint32_t lo = c, hi = b;
            auto mix = [&](uint32_t x, uint32_t y, int t, int sh) { return (int)((x >> sh) & 255) + ((int)((y >> sh) & 255) - (int)((x >> sh) & 255)) * t / 256; };
            if (i < 96) palFire_[i] = Rgb(mix(0, lo, i * 256 / 96, 16), mix(0, lo, i * 256 / 96, 8), mix(0, lo, i * 256 / 96, 0));
            else if (i < 192) palFire_[i] = Rgb(mix(lo, hi, (i - 96) * 256 / 96, 16), mix(lo, hi, (i - 96) * 256 / 96, 8), mix(lo, hi, (i - 96) * 256 / 96, 0));
            else palFire_[i] = Rgb(mix(hi, 0xF0F0E0, (i - 192) * 256 / 64, 16), mix(hi, 0xF0F0E0, (i - 192) * 256 / 64, 8), mix(hi, 0xF0F0E0, (i - 192) * 256 / 64, 0));
            palBlob_[i] = Rgb(mix(b, c, i, 16), mix(b, c, i, 8), mix(b, c, i, 0));
        }
        lineCol_ = a | 0x202020;
    }

    // Background color under the effect (theme background), 0x00RRGGBB.
    void setBackground(uint32_t rgb) { bg_ = rgb & 0xFFFFFF; }
    // Text of the scroller (copied, uppercased, unknown characters become spaces; at most 191 characters).
    void setText(const char* t) {
        int n = 0;
        for (; t && t[n] && n < 191; ++n) {
            char c = t[n];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            text_[n] = (c > 0 && glyphOf_[(int)(unsigned char)c] >= 0) ? c : ' ';
        }
        text_[n] = 0; textLen_ = n;
    }
    // Restarts the animation state (stars, phases) so that a sequence of frames is reproducible.
    void reset() {
        rng_ = 0x2545F491u;
        for (Star& s : stars_) respawn(s, true);
        lastUs_ = 0; haveT_ = false; animUs_ = 0; phaseUs_ = 0; starAcc_ = 0; starTravel_ = 0;
    }

    // Allocates for a new size (clamped to 1..kMaxW x 1..kMaxH). The only function that allocates.
    void resize(int w, int h) {
        w = w < 1 ? 1 : w > kMaxW ? kMaxW : w;
        h = h < 1 ? 1 : h > kMaxH ? kMaxH : h;
        if (w == w_ && h == h_) return;
        w_ = w; h_ = h;
        px_.assign((size_t)w * h, bg_);
        tunLut_.assign((size_t)w * h, 0);
        tunShade_.assign((size_t)w * h, 0);
        heat_.assign((size_t)w * (h + 2), 0);       // 0.9.0: fire (two hidden spark rows below the picture)
        field_.assign((size_t)w * h, 0);           // 0.9.0: metaballs
        fireAccUs_ = 0;
        {   // metaball falloff: v(d2) = R^2 * 96 / (d2 + R^2/4) - 96 (0 at the radius R = 2/5 of the smaller side), so
            // neighbouring blobs add up and merge
            const int R = (w < h ? w : h) * 2 / 5 + 2;
            blobR_ = R;
            blobLut_.assign((size_t)R * R + 1, 0);
            for (int d2 = 0; d2 <= R * R; ++d2) {
                const int v = (int)((long long)R * R * 96 / (d2 + R * R / 4 + 1)) - 96;   // reaches 0 at the radius
                blobLut_[(size_t)d2] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
        const double cx = w * 0.5, cy = h * 0.5, k = (h > w ? w : h) * 24.0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, r = std::sqrt(dx * dx + dy * dy) + 0.5;
                const int ang = (int)std::floor(std::atan2(dy, dx) * 256.0 / 6.283185307179586) & 255;
                const int dist = (int)(k / r) & 255;
                const size_t i = (size_t)y * w + x;
                tunLut_[i] = (uint16_t)(ang | (dist << 8));
                int sh = (int)(r * 512.0 / (h > w ? w : h));             // dark in the middle (far away)
                tunShade_[i] = (uint8_t)(sh > 255 ? 255 : sh);
            }
    }

    // Scroller speed in low-resolution pixels per second at speed 100 % (the panel passes 120 / its pixel size, so
    // the text moves about 120 screen pixels per second whatever the scaling).
    void setScrollRate(int lowResPxPerSec) { scrollRate_ = lowResPxPerSec < 1 ? 1 : lowResPxPerSec; }
    // Total depth travelled by the stars (test hook: must depend only on the animation time, not on the frame rate).
    uint64_t starTravel() const { return starTravel_; }

    // Renders one frame (0.8.2: time-based). tUs: a monotonic clock in microseconds (the panel uses
    // QueryPerformanceCounter); the animation advances by the real elapsed time x speedPct / 100, so a lower frame
    // rate shows fewer frames but never a slower movement. A step longer than 200 ms (pause, minimized window) is
    // clamped so nothing jumps. intensity 0..100; level 0..255 (audio level for kReactive); speedPct 25..400.
    void frame(Effect e, uint64_t tUs, int intensity, int level, int speedPct = 100) {
        if (px_.empty()) return;
        intensity = intensity < 0 ? 0 : intensity > 100 ? 100 : intensity;
        level = level < 0 ? 0 : level > 255 ? 255 : level;
        speedPct = speedPct < 25 ? 25 : speedPct > 400 ? 400 : speedPct;
        const int k = intensity * 256 / 100;                          // 0..256
        uint64_t dt = haveT_ ? (tUs >= lastUs_ ? tUs - lastUs_ : 0) : 0;   // the first frame is time 0
        if (dt > 200000) dt = 200000;
        lastUs_ = tUs; haveT_ = true;
        const uint64_t da = dt * (uint64_t)speedPct / 100;            // animation microseconds of this frame
        animUs_ += da;
        const uint32_t t = (uint32_t)(animUs_ / 1000);                  // animation milliseconds
        switch (e) {
        case kPlasma:    plasma(t, k); break;
        case kStarfield: fill(bg_); stars(da, 1100, k, false); break;
        case kCopper:    copper(t, k); break;
        case kTunnel:    tunnel(t, k); break;
        case kReactive:
            phaseUs_ += da * (uint64_t)(256 + level * 6) / 256;       // faster with the level
            plasma((uint32_t)(phaseUs_ / 1000), k * (160 + level * 96 / 255) / 256);
            stars(da, 700 + level * 6, k, true);
            break;
        case kScroller:  fill(bg_); stars(da, 450, k, false); scroller(t, k, level); break;
        case kMetaballs: metaballs(t, k); break;
        case kFire:      fire(da, k); break;
        case kVector:    fill(bg_); vector(t, k); break;
        case kTwister:   twister(t, k); break;
        default:         fill(bg_); break;
        }
    }

private:
    struct Star { int32_t x, y, z; };

    static uint32_t Rgb(int r, int g, int b) {
        r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
        return (uint32_t)(r << 16 | g << 8 | b);
    }
    // bg + c * k / 256 per channel, saturated.
    uint32_t addScaled(uint32_t c, int k) const {
        const int r = (int)((bg_ >> 16) & 255) + (int)(((c >> 16) & 255) * (uint32_t)k >> 8);
        const int g = (int)((bg_ >> 8) & 255) + (int)(((c >> 8) & 255) * (uint32_t)k >> 8);
        const int b = (int)(bg_ & 255) + (int)((c & 255) * (uint32_t)k >> 8);
        return (uint32_t)((r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b));
    }
    // Saturated addition of c * k / 256 to the pixel at i.
    void addPixel(size_t i, uint32_t c, int k) {
        const uint32_t p = px_[i];
        const int r = (int)((p >> 16) & 255) + (int)(((c >> 16) & 255) * (uint32_t)k >> 8);
        const int g = (int)((p >> 8) & 255) + (int)(((c >> 8) & 255) * (uint32_t)k >> 8);
        const int b = (int)(p & 255) + (int)((c & 255) * (uint32_t)k >> 8);
        px_[i] = (uint32_t)((r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b));
    }
    void fill(uint32_t c) { for (size_t i = 0, n = px_.size(); i < n; ++i) px_[i] = c; }
    int S(int i) const { return sinT_[i & 1023]; }
    uint32_t rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_; }
    void respawn(Star& s, bool anyDepth) {
        s.x = (int32_t)(rnd() % 2048) - 1024;
        s.y = (int32_t)(rnd() % 2048) - 1024;
        s.z = anyDepth ? (int32_t)(rnd() % 1000) + 24 : 1024;
    }

    void plasma(uint32_t t, int k) {
        for (int i = 0; i < 256; ++i) palS_[i] = addScaled(palPlasma_[i], k);
        // 0.8.2: about 3 x the 0.8.1 rates (one sine cycle in ~3 s, palette cycle in ~3.3 s at speed 100 %)
        const int t1 = (int)(t / 3), t2 = (int)(t * 2 / 9), t3 = (int)(t / 6), shift = (int)(t / 13);
        const int sx = 1024 * 3 / (w_ > 1 ? w_ : 1), sy = 1024 * 2 / (h_ > 1 ? h_ : 1);   // ~1.5 / 1 periods
        for (int x = 0; x < w_; ++x) colA_[x] = S(x * sx + t1) + S(x * sx / 2 - t2 + 300);
        for (int y = 0; y < h_; ++y) rowB_[y] = S(y * sy + t2) + S(y * sy * 3 / 2 + t3 + 700);
        for (int d = 0; d < w_ + h_ - 1; ++d) diag_[d] = S(d * (sx + sy) / 3 - t3);
        for (int y = 0; y < h_; ++y) {
            const size_t row = (size_t)y * w_;
            const int rb = rowB_[y];
            for (int x = 0; x < w_; ++x) {
                const int v = colA_[x] + rb + diag_[x + y];               // -635..635
                px_[row + x] = palS_[((v + 640) / 5 + shift) & 255];
            }
        }
    }

    // da: animation microseconds of this frame; vel: depth units per second (the field is 1000 units deep). The
    // fractional part is carried over (starAcc_), so the travelled depth is exactly proportional to the time.
    void stars(uint64_t da, int vel, int k, bool white) {
        const int cx = w_ / 2, cy = h_ / 2, fov = (w_ > h_ ? w_ : h_) / 2 + 1;
        starAcc_ += da * (uint64_t)vel;
        const int step = (int)(starAcc_ / 1000000);
        starAcc_ %= 1000000;
        starTravel_ += (uint64_t)step;
        for (Star& s : stars_) {
            s.z -= step;
            if (s.z < 24) respawn(s, false);
            const int px = cx + s.x * fov / s.z, py = cy + s.y * fov / s.z;
            if (px < 0 || py < 0 || px >= w_ || py >= h_) { if (s.z < 900) respawn(s, false); continue; }
            const int b = 255 - s.z / 4;                                   // brighter when close
            const uint32_t c = white ? Rgb(b, b, b) : Rgb(b * 3 / 4, b * 3 / 4, b);
            addPixel((size_t)py * w_ + px, c, k);
            if (s.z < 260 && px + 1 < w_ && py + 1 < h_) {                 // close stars: 2 x 2
                addPixel((size_t)py * w_ + px + 1, c, k); addPixel((size_t)(py + 1) * w_ + px, c, k);
                addPixel((size_t)(py + 1) * w_ + px + 1, c, k);
            }
        }
    }

    void copper(uint32_t t, int k) {
        fill(bg_);
        static const uint32_t hues[7] = {0xC04040, 0xC08030, 0xB0B040, 0x40B060, 0x40A0C0, 0x5060D0, 0xA050C0};
        const int hb = h_ / 24 > 2 ? h_ / 24 : 2;
        for (int b = 0; b < 7; ++b) {
            const int cy = h_ / 2 + S((int)(t / 2) + b * 110) * (h_ * 36 / 100) / 127;   // one sweep in ~2 s
            for (int d = -hb; d <= hb; ++d) {
                const int y = cy + d;
                if (y < 0 || y >= h_) continue;
                const int a = (hb + 1 - (d < 0 ? -d : d)) * 256 / (hb + 1);  // 0..256 across the bar
                const uint32_t hc = hues[b];
                const uint32_t c = Rgb((int)((hc >> 16) & 255) * a >> 8, (int)((hc >> 8) & 255) * a >> 8, (int)(hc & 255) * a >> 8);
                const uint32_t col = addScaled(c, k);
                const size_t row = (size_t)y * w_;
                for (int x = 0; x < w_; ++x) px_[row + x] = col;               // a bar replaces what is behind it
            }
        }
    }

    void tunnel(uint32_t t, int k) {
        for (int i = 0; i < 256; ++i) palS_[i] = palTunnel_[i];
        int shadeK[256];
        for (int i = 0; i < 256; ++i) shadeK[i] = i * k >> 8;               // depth shade x intensity
        const int du = (int)(t / 4), dv = (int)(t / 13);                // flies one texture length per second
        const int br = (int)((bg_ >> 16) & 255), bgc = (int)((bg_ >> 8) & 255), bb = (int)(bg_ & 255);
        for (size_t i = 0, n = px_.size(); i < n; ++i) {
            const int l = tunLut_[i];
            const uint32_t c = palS_[tex_[((((l >> 8) + du) & 255) << 8) | (((l & 255) + dv) & 255)]];
            const int s = shadeK[tunShade_[i]];
            const int r = br + (int)(((c >> 16) & 255) * (uint32_t)s >> 8), g = bgc + (int)(((c >> 8) & 255) * (uint32_t)s >> 8),
                      b = bb + (int)((c & 255) * (uint32_t)s >> 8);
            px_[i] = (uint32_t)((r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b));
        }
    }

    // ---- 0.9.0 effects
    void metaballs(uint32_t t, int k) {
        for (size_t i = 0, n = field_.size(); i < n; ++i) field_[i] = 0;
        const int R = blobR_, R2 = R * R;
        for (int b = 0; b < 5; ++b) {                                 // five blobs on Lissajous paths
            const int cx = w_ / 2 + S((int)(t / (5 + b)) + b * 200) * (w_ / 2 - R / 2) / 127;
            const int cy = h_ / 2 + S((int)(t / (7 + b)) + b * 330 + 256) * (h_ / 2 - R / 2) / 127;
            const int x0 = cx - R < 0 ? 0 : cx - R, x1 = cx + R >= w_ ? w_ - 1 : cx + R;
            const int y0 = cy - R < 0 ? 0 : cy - R, y1 = cy + R >= h_ ? h_ - 1 : cy + R;
            for (int y = y0; y <= y1; ++y) {
                const int dy = y - cy;
                const size_t row = (size_t)y * w_;
                for (int x = x0; x <= x1; ++x) {
                    const int dx = x - cx, d2 = dx * dx + dy * dy;
                    if (d2 > R2) continue;
                    const int v = field_[row + x] + blobLut_[(size_t)d2];
                    field_[row + x] = (uint8_t)(v > 255 ? 255 : v);
                }
            }
        }
        int pk[256];
        for (int i = 0; i < 256; ++i) {                               // inside: palette; edge band brighter
            const int edge = i >= 96 && i < 112 ? 96 : 0;
            pk[i] = i < 40 ? 0 : (i * k / 256 + edge * k / 256 > 256 ? 256 : i * k / 256 + edge * k / 256);
        }
        for (size_t i = 0, n = px_.size(); i < n; ++i) {
            const int f = field_[i];
            px_[i] = f < 40 ? bg_ : addScaled(palBlob_[(f + (int)(t / 20)) & 255], pk[f]);
        }
    }

    // Fire at a fixed 60 Hz simulation step (time-based: the number of steps follows the elapsed time).
    void fire(uint64_t da, int k) {
        fireAccUs_ += da;
        int steps = (int)(fireAccUs_ / 16667);
        fireAccUs_ %= 16667;
        if (steps > 6) steps = 6;                                    // after a pause: catch up a little, not all
        const int W = w_, H = h_ + 2;
        for (int s = 0; s < steps; ++s) {
            for (int x = 0; x < W; ++x) {                              // sparks in the two hidden rows
                const uint32_t r = rnd();
                heat_[(size_t)(H - 1) * W + x] = (r & 3) ? (uint8_t)(160 + (r >> 8) % 96) : 0;
                heat_[(size_t)(H - 2) * W + x] = (uint8_t)((heat_[(size_t)(H - 2) * W + x] + heat_[(size_t)(H - 1) * W + x]) / 2);
            }
            const int decay = 2 + 24 * 1 / (H > 8 ? H / 8 : 1);       // taller buffers burn a little longer
            for (int y = 0; y < H - 2; ++y)
                for (int x = 0; x < W; ++x) {
                    const int xl = x > 0 ? x - 1 : x, xr = x + 1 < W ? x + 1 : x;
                    const int v = (heat_[(size_t)(y + 1) * W + xl] + heat_[(size_t)(y + 1) * W + x] + heat_[(size_t)(y + 1) * W + xr] +
                                   heat_[(size_t)(y + 2) * W + x]) / 4 - decay;
                    heat_[(size_t)y * W + x] = (uint8_t)(v < 0 ? 0 : v);
                }
        }
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                const int v = heat_[(size_t)y * W + x];
                px_[(size_t)y * w_ + x] = v < 8 ? bg_ : addScaled(palFire_[v], k);
            }
    }

    // Saturated additive line (Bresenham), color c scaled by k.
    void line(int x0, int y0, int x1, int y1, uint32_t c, int k) {
        int dx = x1 - x0 < 0 ? x0 - x1 : x1 - x0, dy = y1 - y0 < 0 ? y0 - y1 : y1 - y0;
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx - dy, guard = 0;
        while (guard++ < 4 * (kMaxW + kMaxH)) {
            if (x0 >= 0 && y0 >= 0 && x0 < w_ && y0 < h_) addPixel((size_t)y0 * w_ + x0, c, k);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    }
    void vector(uint32_t t, int k) {
        const int a = (int)(t / 6), b = (int)(t / 9), c = (int)(t / 14);          // rotation angles (1024 = turn)
        const int ca = S(a + 256), sa = S(a), cb = S(b + 256), sb = S(b), cc = S(c + 256), sc = S(c);
        int px[12], py[12], pz[12];
        const int scale = (w_ < h_ ? w_ : h_) * 3 / 10 + 1;
        for (int i = 0; i < 12; ++i) {
            int x = ico_[i][0], y = ico_[i][1], z = ico_[i][2];
            int y1 = (y * ca - z * sa) / 127, z1 = (y * sa + z * ca) / 127;           // around X
            int x2 = (x * cb + z1 * sb) / 127, z2 = (-x * sb + z1 * cb) / 127;        // around Y
            int x3 = (x2 * cc - y1 * sc) / 127, y3 = (x2 * sc + y1 * cc) / 127;      // around Z
            const int zz = z2 + 3000;                                                 // camera distance
            px[i] = w_ / 2 + x3 * scale * 3 / zz;
            py[i] = h_ / 2 + y3 * scale * 3 / zz;
            pz[i] = z2;
        }
        for (int e = 0; e < nEdges_; ++e) {
            const int i = edges_[e][0], j = edges_[e][1];
            const int depth = (pz[i] + pz[j]) / 2;                                   // -1100 .. 1100
            int shade = 256 - (depth + 1100) * 160 / 2200;                           // near = bright
            shade = shade < 64 ? 64 : shade > 256 ? 256 : shade;
            line(px[i], py[i], px[j], py[j], lineCol_, k * shade / 256);
        }
    }

    void twister(uint32_t t, int k) {
        fill(bg_);
        const int cx = w_ / 2, R = (w_ < h_ ? w_ : h_) / 4 + 2;
        const uint32_t faceCol[4] = {palPlasma_[0], palPlasma_[64], palPlasma_[128], palPlasma_[192]};
        for (int y = 0; y < h_; ++y) {
            const int ang = (int)(t / 3) + S(y * 4 + (int)(t / 5)) * 2 + y * 2;   // twist varies with row and time
            int ex[4];
            for (int f = 0; f < 4; ++f) ex[f] = cx + S(ang + f * 256 + 256) * R / 127;
            const size_t row = (size_t)y * w_;
            for (int f = 0; f < 4; ++f) {
                const int x0 = ex[f], x1 = ex[(f + 1) & 3];
                if (x1 <= x0) continue;                                    // back face
                const int width = x1 - x0;
                const int sh = 64 + width * 192 / (2 * R);                 // wider = facing the viewer = brighter
                const uint32_t col = addScaled(faceCol[f] | 0x303030, k * (sh > 256 ? 256 : sh) / 256);
                for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w_; ++x) px_[row + x] = col;
            }
        }
    }

    void scroller(uint32_t t, int k, int level) {
        if (textLen_ <= 0) return;
        const int sc = h_ / 14 > 3 ? 3 : h_ / 14 > 1 ? h_ / 14 : 1;        // glyph scale (pixels per dot), 1..3
        const int cw = 6 * sc, total = (textLen_ + 8) * cw;                // 8 blank characters between loops
        int amp = (h_ - 7 * sc) / 2 - 1 > 0 ? (h_ - 7 * sc) / 2 - 1 : 0;
        if (amp > h_ / 4) amp = h_ / 4;                                    // a gentle wave keeps the text readable
        const int scroll = (int)(((uint64_t)t * (uint64_t)scrollRate_ / 1000) % (uint64_t)total);
        const int base = (h_ - 7 * sc) / 2;
        int kk = k > 0 ? k * (224 + level / 8) / 256 + 24 : 0;          // text a bit brighter (0 stays invisible)
        if (kk > 256) kk = 256;
        for (int x = 0; x < w_; ++x) {
            const int col = (x + scroll) % total;
            const int ci = col / cw, gx = (col % cw) / sc;
            if (ci >= textLen_ || gx >= 5) continue;
            const int g = glyphOf_[(int)(unsigned char)text_[ci]];
            if (g < 0) continue;
            const int yoff = base + S(x * 3 + (int)(t / 2)) * amp / 127;
            const uint32_t c = palPlasma_[(x * 2 + (int)(t / 10)) & 255] | 0x404040;
            for (int r = 0; r < 7; ++r) {
                if (!(kFont[g].rows[r] & (0x10 >> gx))) continue;
                for (int s = 0; s < sc; ++s) {
                    const int y = yoff + r * sc + s;
                    if (y >= 0 && y < h_) px_[(size_t)y * w_ + x] = addScaled(c, kk);
                }
            }
        }
    }

    int w_ = 0, h_ = 0;
    uint32_t bg_ = 0x1E1F22;
    std::vector<uint32_t> px_;
    std::vector<uint16_t> tunLut_;
    std::vector<uint8_t> tunShade_;
    std::vector<uint8_t> heat_, field_, blobLut_;          // 0.9.0: fire, metaballs (allocated in resize)
    uint32_t palFire_[256], palBlob_[256], lineCol_ = 0x60A0E0;
    int blobR_ = 1;
    uint64_t fireAccUs_ = 0;
    int ico_[12][3];
    uint8_t edges_[30][2];
    int nEdges_ = 0;
    int16_t sinT_[1024];
    uint32_t palPlasma_[256], palTunnel_[256], palS_[256];
    uint8_t tex_[256 * 256];
    int colA_[kMaxW], rowB_[kMaxH], diag_[kMaxW + kMaxH];
    Star stars_[kStars];
    uint32_t rng_ = 1;
    uint64_t lastUs_ = 0, animUs_ = 0, phaseUs_ = 0, starAcc_ = 0, starTravel_ = 0;
    bool haveT_ = false;
    int scrollRate_ = 40;
    int glyphOf_[128];
    char text_[192] = {};
    int textLen_ = 0;
};

}  // namespace vis
