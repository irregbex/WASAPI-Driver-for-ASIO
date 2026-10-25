// WASAPI Driver for ASIO - control panel (plain Win32, no dialog resources; hand-drawn dark theme)
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
//   The settings window opened by IASIO::controlPanel(). It edits Config (config.ini), manages profiles, shows live
//   meters, DSP load and latency tiles, a full status report ("Copy status"), extra devices and output routing,
//   and the About page with the Steinberg ASIO Compatible logo.
//
// Responsibilities
//   - Dark tabbed UI (Main, Advanced, Devices, Status, About). Rounded cards are painted in WM_PAINT; buttons and
//     toggles are owner-drawn (BS_OWNERDRAW); native controls get the dark theme through uxtheme
//     (SetWindowTheme "DarkMode_CFD" / "DarkMode_Explorer") and WM_CTLCOLOR*; the title bar is darkened with
//     DwmSetWindowAttribute (attribute 20, or 19 before Windows 10 20H1). uxtheme/dwmapi are loaded at runtime.
//   - The ASIO Compatible logo is embedded as a PNG (RCDATA 101) and decoded with WIC into a premultiplied 32-bit
//     DIB drawn with GdiAlphaBlend.
//   - Per-monitor DPI: all layout is in logical units scaled by S(); WM_DPICHANGED rebuilds fonts, logos and
//     controls and moves the window to the rectangle Windows suggests.
//   - Message loop: until 0.7.0 the owner window was disabled while the panel was open. 0.7.1: the panel is an
//     unowned top-level window and no host window is disabled; the loop only provides keyboard navigation and
//     returns when the panel closes. WM_QUIT is re-posted to the host loop.
//   - 0.7.0: "Apply" keeps the panel open and hands the saved Config to WasapiBridge::applyPanelChanges (LIVE
//     settings at once, RESET settings through a deferred kAsioResetRequest); RESET settings carry a dot marker.
//     Latency offsets (Advanced), direct monitoring and the microphone privacy verdict (Devices), the monitor
//     window toggle (Main) and the safe-mode banner (bottom bar).
//   - 0.7.0: the floating monitor window (end of this file) shares the theme, the meter child and the ballistics.
//
// Threading
//   Everything runs on the host thread that called controlPanel(). The panel talks to the engine only through
//   WasapiBridge::readMeters (atomic exchange/load), getStatus (atomic loads) and resetCounters (request bits),
//   and applies changes with applyPanelChanges when "Apply" is pressed. It never touches audio-thread state and
//   never calls asioMessage itself. The monitor window follows the same rules (H only, lock-free reads).
#include "driver.h"
#include "privacy.h"
#include "visuals.h"
#include "themes.h"
#include "strings.h"
#include "update.h"
#include "a11y.h"
#include "trayicon.h"
#include "guard.h"                       // 0.9.8: exception barrier of every window procedure and callback
#include "trace.h"                       // 0.9.8: panel open / close events
#include <shellapi.h>
#include <windowsx.h>
#include <audiopolicy.h>
#include <wincodec.h>
#include <algorithm>
#include <memory>
#include <vector>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cstdarg>
#include <ctime>

#include "panel_ids.h"                   // 0.9.2: control ids (shared with tips.h and the strings test)
#include "tips.h"
using namespace wbid;
static_assert(IDOK == ID_DLG_OK && IDCANCEL == ID_DLG_CANCEL, "dialog ids");
// Marker appended to the label of every RESET setting (0.7.0, F1): changing it makes the DAW restart the driver.
#define WB_RESET_MARK L" \x2022"
using namespace lang;                    // 0.9.0 (H4): T(S_...) = text in the current interface language
// Text of `id` followed by the RESET marker (" \x2022"). The result lives in a per-id cache that is rebuilt
// when the language changes (host thread only).
// printf-style formatting into a std::wstring (host thread; used with translated format strings).
static std::wstring FmtW(const wchar_t* f, ...) {
    wchar_t b[4096];
    va_list a; va_start(a, f); const int n = vswprintf(b, 4096, f, a); va_end(a);
    return n >= 0 ? std::wstring(b) : std::wstring(f);
}
static const wchar_t* TM(int id) {
    static std::wstring cache[S_COUNT];
    static int cacheLang[S_COUNT];
    static bool init = false;
    if (!init) { for (int& l : cacheLang) l = -1; init = true; }
    if (id < 0 || id >= S_COUNT) return L"";
    if (cacheLang[id] != Current()) { cache[id] = std::wstring(T(id)) + WB_RESET_MARK; cacheLang[id] = Current(); }
    return cache[id].c_str();
}

enum { TAB_MAIN = 0, TAB_ADV, TAB_DEV, TAB_STATE, TAB_UI, TAB_ABOUT, TAB_COUNT, TAB_ALL = -1 };
// 0.9.0: tab names in the current language (TAB_UI = "Interface": theme, language, visuals, updates)
static const wchar_t* TabName(int i) {
    static const int ids[TAB_COUNT] = {S_TAB_MAIN, S_TAB_ADV, S_TAB_DEV, S_TAB_STATE, S_TAB_UI, S_TAB_ABOUT};
    return i >= 0 && i < TAB_COUNT ? T(ids[i]) : L"";
}

// ---------------------------------------------------------------- theme
// 0.9.0 (H1): the colors come from the selected palette of src/themes.h (ThemeSelect); the defaults below are
// the Dark palette, used until a theme is selected.
struct Theme {
    COLORREF bg = RGB(0x1E, 0x1F, 0x22), card = RGB(0x2B, 0x2D, 0x31), input = RGB(0x1A, 0x1B, 0x1E),
             tile = RGB(0x23, 0x24, 0x28), text = RGB(0xE8, 0xE8, 0xEA), dim = RGB(0x9A, 0x9D, 0xA3),
             accent = RGB(0x3D, 0xDC, 0x97), accentHi = RGB(0x62, 0xE8, 0xAE), accentLo = RGB(0x2A, 0xB0, 0x77),
             amber = RGB(0xF5, 0xA5, 0x24), red = RGB(0xF7, 0x6D, 0x73), line = RGB(0x3A, 0x3C, 0x42),
             btn = RGB(0x38, 0x3A, 0x40), btnHi = RGB(0x45, 0x48, 0x50), btnLo = RGB(0x2F, 0x31, 0x36),
             onAccent = RGB(0x0E, 0x14, 0x11);
    COLORREF focus = RGB(0x3D, 0xDC, 0x97), btnText = RGB(0xE8, 0xE8, 0xEA);   // 0.9.1 (B1): focus ring, button text
    COLORREF meterLo = RGB(0x2E, 0xD5, 0x7E), meterMid = RGB(0xF5, 0xC5, 0x42), meterHi = RGB(0xF7, 0x6D, 0x73),
             barOn = RGB(0x26, 0x28, 0x2C), barOff = RGB(0x21, 0x22, 0x25), clipOff = RGB(0x3A, 0x2A, 0x2C),
             hold = RGB(0xF0, 0xF0, 0xF0), guide = RGB(0x34, 0x36, 0x3B);
    uint32_t fx[3] = {0x2050A0, 0x6030A0, 0x20A0A0};   // effect palette anchors (0xRRGGBB)
    bool dark = true, dotHeader = false;
    bool hc = false;                                   // 0.9.1 (B1): Windows high contrast is on (system colors)
    int id = thm::kDark;
    HBRUSH bgBr = nullptr, cardBr = nullptr, inputBr = nullptr;
};
static Theme g_th;
static COLORREF Cref(uint32_t v) { return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); }
// 0.9.1 (B1): Windows high contrast (SPI_GETHIGHCONTRAST). While it is on, the panel and the monitor window use
// the system colors only, no visuals, no gradients and no dark styles, whatever theme is configured.
static bool HighContrastOn() {
    HIGHCONTRASTW hc = {}; hc.cbSize = sizeof hc;
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}
// Host thread. Copies palette `id` into g_th and recreates the long-lived brushes that already exist (they are
// only handed out in WM_CTLCOLOR*, so replacing them between paints is safe). Windows must be repainted after.
// With high contrast on, the palette is built from GetSysColor instead (the configured id is kept in g_th.id).
static void ThemeSelect(int id) {
    if (id < 0 || id >= thm::kCount) id = thm::kDark;
    const thm::Palette& t = thm::kPalettes[id];
    g_th.bg = Cref(t.bg); g_th.card = Cref(t.card); g_th.input = Cref(t.input); g_th.tile = Cref(t.tile);
    g_th.text = Cref(t.text); g_th.dim = Cref(t.dim); g_th.accent = Cref(t.accent); g_th.accentHi = Cref(t.accentHi);
    g_th.accentLo = Cref(t.accentLo); g_th.amber = Cref(t.amber); g_th.red = Cref(t.red); g_th.line = Cref(t.line);
    g_th.btn = Cref(t.btn); g_th.btnHi = Cref(t.btnHi); g_th.btnLo = Cref(t.btnLo); g_th.onAccent = Cref(t.onAccent);
    g_th.meterLo = Cref(t.meterLo); g_th.meterMid = Cref(t.meterMid); g_th.meterHi = Cref(t.meterHi);
    g_th.barOn = Cref(t.barOn); g_th.barOff = Cref(t.barOff); g_th.clipOff = Cref(t.clipOff);
    g_th.hold = Cref(t.hold); g_th.guide = Cref(t.guide);
    g_th.fx[0] = t.fx1; g_th.fx[1] = t.fx2; g_th.fx[2] = t.fx3;
    g_th.dark = t.dark; g_th.dotHeader = t.dotHeader; g_th.id = id;
    g_th.focus = g_th.accent; g_th.btnText = g_th.text; g_th.hc = false;
    if (HighContrastOn()) {
        const COLORREF win = GetSysColor(COLOR_WINDOW), wtx = GetSysColor(COLOR_WINDOWTEXT);
        const COLORREF hil = GetSysColor(COLOR_HIGHLIGHT), hit = GetSysColor(COLOR_HIGHLIGHTTEXT);
        const COLORREF face = GetSysColor(COLOR_BTNFACE), btx = GetSysColor(COLOR_BTNTEXT), gray = GetSysColor(COLOR_GRAYTEXT);
        const COLORREF hot = GetSysColor(COLOR_HOTLIGHT);
        g_th.bg = g_th.card = g_th.input = g_th.tile = win;
        g_th.text = g_th.dim = wtx;                    // secondary text is not dimmed in high contrast
        g_th.accent = g_th.accentHi = g_th.accentLo = hil; g_th.onAccent = hit;
        g_th.amber = g_th.red = hot;
        g_th.line = wtx;
        g_th.btn = g_th.btnHi = face; g_th.btnLo = gray; g_th.btnText = btx;
        g_th.meterLo = g_th.meterMid = g_th.meterHi = hil;   // flat meters, no gradient
        g_th.barOn = g_th.barOff = win; g_th.clipOff = win; g_th.hold = wtx; g_th.guide = wtx;
        g_th.focus = wtx;
        const int l = (GetRValue(win) * 299 + GetGValue(win) * 587 + GetBValue(win) * 114) / 1000;
        g_th.dark = l < 128; g_th.dotHeader = false; g_th.hc = true;
    }
    if (g_th.bgBr) { DeleteObject(g_th.bgBr); g_th.bgBr = CreateSolidBrush(g_th.bg); }
    if (g_th.cardBr) { DeleteObject(g_th.cardBr); g_th.cardBr = CreateSolidBrush(g_th.card); }
    if (g_th.inputBr) { DeleteObject(g_th.inputBr); g_th.inputBr = CreateSolidBrush(g_th.input); }
}
static int g_thRefs = 0;               // 0.7.0: the panel and the monitor window share the brushes (H only)
// Creates the long-lived theme brushes (background, card, input) once; used by WM_CTLCOLOR* handlers.
// Reference counted: every ThemeInit is paired with a ThemeFree.
static void ThemeInit() {
    ++g_thRefs;
    if (!g_th.bgBr) g_th.bgBr = CreateSolidBrush(g_th.bg);
    if (!g_th.cardBr) g_th.cardBr = CreateSolidBrush(g_th.card);
    if (!g_th.inputBr) g_th.inputBr = CreateSolidBrush(g_th.input);
}
// Deletes the theme brushes. Called after the panel window (and every control using them) is destroyed.
static void ThemeFree() {
    if (--g_thRefs > 0) return;
    g_thRefs = 0;
    if (g_th.bgBr) DeleteObject(g_th.bgBr);
    if (g_th.cardBr) DeleteObject(g_th.cardBr);
    if (g_th.inputBr) DeleteObject(g_th.inputBr);
    g_th.bgBr = g_th.cardBr = g_th.inputBr = nullptr;
}

// uxtheme / dwmapi loaded at runtime (no new import dependencies; missing functions are simply skipped)
typedef HRESULT (WINAPI* PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
typedef HRESULT (WINAPI* PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
// Applies a dark visual style to a native control through uxtheme!SetWindowTheme (resolved once at runtime).
// theme: "DarkMode_CFD" (combo boxes) or "DarkMode_Explorer" (edits, list boxes, scroll bars). No-op if missing.
static void DarkCtl(HWND h, const wchar_t* theme) {
    static PFN_SetWindowTheme fn = nullptr; static bool tried = false;
    if (!tried) { tried = true; if (HMODULE m = LoadLibraryW(L"uxtheme.dll")) fn = (PFN_SetWindowTheme)(void*)GetProcAddress(m, "SetWindowTheme"); }
    if (!g_th.dark || g_th.hc) theme = L"Explorer";   // 0.9.0: the Light theme (0.9.1: and high contrast) use the normal system style
    if (fn && h) fn(h, theme, nullptr);
}
// Requests a dark title bar with DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE); falls back from attribute
// 20 to 19 (Windows 10 before 20H1). Silently ignored on systems without support.
static void DarkTitle(HWND h) {
    static PFN_DwmSetWindowAttribute fn = nullptr; static bool tried = false;
    if (!tried) { tried = true; if (HMODULE m = LoadLibraryW(L"dwmapi.dll")) fn = (PFN_DwmSetWindowAttribute)(void*)GetProcAddress(m, "DwmSetWindowAttribute"); }
    BOOL on = (g_th.dark && !g_th.hc) ? TRUE : FALSE;   // 0.9.0: light title bar for the Light theme (0.9.1: system one in high contrast)
    if (fn && FAILED(fn(h, 20, &on, sizeof on))) fn(h, 19, &on, sizeof on);   // DWMWA_USE_IMMERSIVE_DARK_MODE (20; 19 before 20H1)
}

// ---------------------------------------------------------------- application icon (0.9.3)
// ICON resource 100 of the DLL (src/res/app.ico, drawn by scripts/make_icon.py). Icons are loaded at the exact
// pixel size asked for (LoadImageW picks the nearest image of the file and scales it) and kept in a small cache
// for the life of the DLL (they stay assigned to windows with WM_SETICON); FreeAppIcons releases them when the
// DLL is unloaded. Host threads only (the panel's and the driver's message thread; the cache is not shared with
// the audio thread).
enum { kIdiApp = 100 };
struct IconCache { int size = 0; HICON ic = nullptr; };
static IconCache g_appIcons[12];
static HICON AppIcon(int size) {
    if (size < 8) size = 8;
    if (size > 256) size = 256;
    for (const IconCache& c : g_appIcons) if (c.ic && c.size == size) return c.ic;
    HICON ic = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(kIdiApp), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    if (!ic) return LoadIcon(nullptr, IDI_APPLICATION);
    for (IconCache& c : g_appIcons) if (!c.ic) { c.size = size; c.ic = ic; return ic; }
    // full (more than 12 different sizes in one session): reuse the closest cached size, free the new one
    DestroyIcon(ic);
    IconCache* best = &g_appIcons[0];
    for (IconCache& c : g_appIcons) if (abs(c.size - size) < abs(best->size - size)) best = &c;
    return best->ic;
}
void FreeAppIcons() {
    for (IconCache& c : g_appIcons) if (c.ic) { DestroyIcon(c.ic); c.ic = nullptr; c.size = 0; }
}
// GetSystemMetricsForDpi (Windows 10 1607+, loaded at run time), else the system value scaled to dpi.
static int MetricForDpi(int idx, int dpi) {
    typedef int (WINAPI* PFN)(int, UINT);
    static PFN fn = (PFN)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    if (dpi <= 0) return GetSystemMetrics(idx);
    if (fn) return fn(idx, (UINT)dpi);
    HDC dc = GetDC(nullptr); const int sys = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    return MulDiv(GetSystemMetrics(idx), dpi, sys > 0 ? sys : 96);
}
// Title bar / Alt+Tab icons of a window for its DPI (dpi 0 = system DPI).
static void SetWindowIcons(HWND w, int dpi) {
    if (!w) return;
    SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)AppIcon(MetricForDpi(SM_CXICON, dpi)));
    SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)AppIcon(MetricForDpi(SM_CXSMICON, dpi)));
}
// Message boxes get the application icon too (taskbar and Alt+Tab; Windows draws no icon in a message box's
// caption): a thread-local CBT hook sets it when the box is activated.
static HHOOK g_mbHook = nullptr;
// 0.9.8: exception barrier (a hook procedure is called by USER32; nothing may escape into it).
static LRESULT CALLBACK MsgBoxHook(int code, WPARAM wp, LPARAM lp) {
    try {
        if (code == HCBT_ACTIVATE) {
            wchar_t c[16] = L"";
            if (GetClassNameW((HWND)wp, c, 16) && !wcscmp(c, L"#32770")) SetWindowIcons((HWND)wp, 0);
        }
    } catch (...) {
        NoteException("the message box hook");
    }
    return CallNextHookEx(g_mbHook, code, wp, lp);
}
static int AppMessageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT flags) {
    const bool own = g_mbHook == nullptr;
    if (own) g_mbHook = SetWindowsHookExW(WH_CBT, MsgBoxHook, nullptr, GetCurrentThreadId());
    const int r = MessageBoxW(owner, text, caption, flags);
    if (own && g_mbHook) { UnhookWindowsHookEx(g_mbHook); g_mbHook = nullptr; }
    return r;
}

// Small GDI helpers: solid rectangle (empty rectangles are rejected), rounded rectangle, and text in a font/color.
static void FillRectC(HDC dc, int l, int t, int r, int b, COLORREF c) {
    if (r <= l || b <= t) return;
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(c); FillRect(dc, &rc, br); DeleteObject(br);
}
static void RoundRectC(HDC dc, int l, int t, int r, int b, int rad, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, l, t, r, b, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pen);
}
static void TextC(HDC dc, HFONT f, COLORREF c, const wchar_t* t, RECT rc, UINT fmt) {
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}

// ASIO Compatible logo (RCDATA 101, PNG) -> premultiplied 32-bit DIB of height h (WIC, Fant scaling).
// Host thread; uses the caller's COM apartment (if COM is not initialized, creation fails and no logo is drawn).
// Every WIC interface is released on every path. w receives the scaled width. Returns nullptr on any failure.
static HBITMAP LoadLogo(int h, int& w) {
    w = 0;
#if !WB_SHOW_ASIO_LOGO
    return nullptr;                        // logo disabled at build time (common.h); nothing is drawn
#endif
    static const CLSID kClsidWic = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    static const GUID kFmtPBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};
    HRSRC r = FindResourceW(g_hInst, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));   // 10 = RT_RCDATA
    if (!r) return nullptr;
    HGLOBAL g = LoadResource(g_hInst, r);
    const BYTE* data = g ? (const BYTE*)LockResource(g) : nullptr;
    const DWORD size = SizeofResource(g_hInst, r);
    if (!data || !size) return nullptr;
    HBITMAP bmp = nullptr;
    IWICImagingFactory* f = nullptr; IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidWic, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)&f)) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)data, size)) &&
        SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr))) {
        UINT iw = 0, ih = 0; fr->GetSize(&iw, &ih);
        if (iw && ih && h > 0) {
            w = (int)((double)iw * h / ih + 0.5);
            if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(sc, kFmtPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bmp && FAILED(cv->CopyPixels(nullptr, (UINT)w * 4, (UINT)w * 4 * h, (BYTE*)bits))) { DeleteObject(bmp); bmp = nullptr; }
            }
        }
    }
    IUnknown* rel[] = {cv, sc, fr, dec, st, f};
    for (IUnknown* u : rel) if (u) u->Release();
    if (!bmp) w = 0;
    return bmp;
}
// Draws a premultiplied 32-bit logo bitmap with per-pixel alpha (GdiAlphaBlend). No-op if bmp is null.
// 0.9.1: the logo is white; on a light background (Light theme, high contrast white) it is drawn on a dark
// plate so it stays visible and unaltered (TRADEMARKS.txt).
static void DrawLogo(HDC dc, HBITMAP bmp, int w, int h, int x, int y, COLORREF under) {
    if (!bmp) return;
    if ((GetRValue(under) * 299 + GetGValue(under) * 587 + GetBValue(under) * 114) / 1000 >= 128) {
        const int pad = h / 10 > 2 ? h / 10 : 2;
        RoundRectC(dc, x - pad, y - pad, x + w + pad, y + h + pad, pad * 2, RGB(0x1C, 0x1E, 0x22), RGB(0x1C, 0x1E, 0x22));
    }
    HDC m = CreateCompatibleDC(dc);
    HGDIOBJ o = SelectObject(m, bmp);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc, x, y, w, h, m, 0, 0, w, h, bf);
    SelectObject(m, o); DeleteDC(m);
}

// ---------------------------------------------------------------- owner-drawn buttons and toggles
// Button window properties: "wbkind" 1 normal, 2 accent, 3 toggle; "wbhov" mouse is over it; "wbbg" background
// color + 1 (so that 0 means "not set"). The properties are removed in WM_NCDESTROY by the subclass procedure.
// Toggle state: GWLP_USERDATA (0/1).
static WNDPROC g_btnProc = nullptr;
// Subclass procedure for owner-drawn buttons: tracks hover ("wbhov") with TrackMouseEvent for the hover color
// and removes the window properties on WM_NCDESTROY.
static LRESULT CALLBACK BtnHoverProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETOBJECT && (INT_PTR)GetPropW(h, L"wbkind") == 3) {   // 0.9.1 (B1): a toggle is a check box
        LRESULT r = 0;
        if (a11y::OnGetObject(h, w, l, a11y::kToggle, r)) return r;
    }
    if (m == WM_MOUSEMOVE && !GetPropW(h, L"wbhov")) {
        SetPropW(h, L"wbhov", (HANDLE)1);
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE) { RemovePropW(h, L"wbhov"); InvalidateRect(h, nullptr, FALSE); }
    else if (m == WM_NCDESTROY) { RemovePropW(h, L"wbhov"); RemovePropW(h, L"wbkind"); RemovePropW(h, L"wbbg"); RemovePropW(h, L"wbmnem"); }
    return CallWindowProcW(g_btnProc, h, m, w, l);
}
// Creates an owner-drawn button. kind: 1 normal, 2 accent (default action), 3 toggle. bg: background color the
// button sits on (used to paint the rounded corners). Coordinates are physical pixels. Returns the button.
static HWND MakeButton(HWND parent, HFONT font, const wchar_t* text, int x, int y, int w, int h, int id, int kind, COLORREF bg) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (!b) return nullptr;
    SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
    SetPropW(b, L"wbkind", (HANDLE)(INT_PTR)kind);
    SetPropW(b, L"wbbg", (HANDLE)(INT_PTR)(bg + 1));
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(b, GWLP_WNDPROC, (LONG_PTR)GuardedWndProc<BtnHoverProc>);   // 0.9.8: barrier
    if (!g_btnProc) g_btnProc = old;
    return b;
}
// Toggle helpers: set / get the on-off state kept in GWLP_USERDATA, and test whether a window is a toggle.
// 0.9.1 (B1): every state change is announced to accessibility clients (EVENT_OBJECT_STATECHANGE).
static void SetToggle(HWND h, bool on) {
    if (!h) return;
    const bool was = GetWindowLongPtrW(h, GWLP_USERDATA) != 0;
    SetWindowLongPtrW(h, GWLP_USERDATA, on ? 1 : 0);
    InvalidateRect(h, nullptr, FALSE);
    if (was != on) a11y::NotifyState(h);
}
static void SetChk(HWND parent, int id, bool on) { SetToggle(GetDlgItem(parent, id), on); }
static bool GetChk(HWND parent, int id) { HWND h = GetDlgItem(parent, id); return h && GetWindowLongPtrW(h, GWLP_USERDATA) != 0; }
static bool IsToggle(HWND h) { return h && (INT_PTR)GetPropW(h, L"wbkind") == 3; }

// WM_DRAWITEM handler for buttons and toggles (panel and dialogs). Paints the rounded body, hover/pressed
// states, focus ring, check box and label, scaled for dpi.
// 0.9.1 (B1): the focus ring has its own contrasting color in every theme (on an accent button the accent
// border was invisible) and is drawn whenever the control has the focus; a button with a mnemonic ("wbmnem")
// draws its "&" prefix, underlined only while Windows shows keyboard cues (Alt pressed or keyboard use).
static void DrawOwnerButton(const DRAWITEMSTRUCT* d, int dpi) {
    HDC dc = d->hDC; RECT rc = d->rcItem;
    const int kind = (int)(INT_PTR)GetPropW(d->hwndItem, L"wbkind");
    const COLORREF bg = (COLORREF)((INT_PTR)GetPropW(d->hwndItem, L"wbbg") - 1);
    const bool hov = GetPropW(d->hwndItem, L"wbhov") != nullptr, down = (d->itemState & ODS_SELECTED) != 0;
    const bool dis = (d->itemState & ODS_DISABLED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
    const int r = MulDiv(8, dpi, 96), ring = MulDiv(2, dpi, 96) > 1 ? MulDiv(2, dpi, 96) : 1;
    FillRectC(dc, rc.left, rc.top, rc.right, rc.bottom, bg);
    wchar_t t[128]; GetWindowTextW(d->hwndItem, t, 128);
    HFONT f = (HFONT)SendMessageW(d->hwndItem, WM_GETFONT, 0, 0);
    UINT prefix = DT_NOPREFIX;
    if (GetPropW(d->hwndItem, L"wbmnem")) {
        prefix = 0;
        if (SendMessageW(d->hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) prefix = DT_HIDEPREFIX;
    }
    auto label = [&](COLORREF c, RECT tr, UINT fmt) {
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, c);
        DrawTextW(dc, t, -1, &tr, fmt | prefix);
        SelectObject(dc, of);
    };
    auto focusRing = [&](RECT fr, COLORREF c) {          // rounded ring `ring` pixels wide
        HPEN pen = CreatePen(PS_SOLID, ring, c);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, fr.left, fr.top, fr.right, fr.bottom, r, r);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
    };
    if (kind == 3) {                                   // toggle: box + text
        const bool on = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA) != 0;
        const int bs = MulDiv(18, dpi, 96), y0 = (rc.top + rc.bottom - bs) / 2, x0 = rc.left + MulDiv(4, dpi, 96);
        RoundRectC(dc, x0, y0, x0 + bs, y0 + bs, MulDiv(6, dpi, 96), on ? (hov ? g_th.accentHi : g_th.accent) : g_th.input,
                   on ? g_th.accent : (hov ? g_th.accent : g_th.line));
        if (on) {                                      // check mark
            HPEN pen = CreatePen(PS_SOLID, MulDiv(2, dpi, 96), g_th.onAccent);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, x0 + bs * 22 / 100, y0 + bs * 52 / 100, nullptr);
            LineTo(dc, x0 + bs * 42 / 100, y0 + bs * 72 / 100);
            LineTo(dc, x0 + bs * 78 / 100, y0 + bs * 30 / 100);
            SelectObject(dc, op); DeleteObject(pen);
        }
        RECT tr = {x0 + bs + MulDiv(10, dpi, 96), rc.top, rc.right - ring, rc.bottom};
        label(dis ? g_th.dim : g_th.text, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (focus) focusRing({rc.left + ring / 2, rc.top + ring / 2, rc.right - ring / 2, rc.bottom - ring / 2}, g_th.focus);
        return;
    }
    COLORREF fill = kind == 2 ? (down ? g_th.accentLo : hov ? g_th.accentHi : g_th.accent) : (down ? g_th.btnLo : hov ? g_th.btnHi : g_th.btn);
    if (dis) fill = g_th.btnLo;
    RoundRectC(dc, rc.left, rc.top, rc.right, rc.bottom, r, fill, g_th.hc ? g_th.text : fill);
    label(dis ? g_th.dim : (kind == 2 ? g_th.onAccent : g_th.btnText), rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (focus) {                                       // outer ring in the focus color, inner ring on accent buttons
        focusRing({rc.left + ring / 2, rc.top + ring / 2, rc.right - ring / 2, rc.bottom - ring / 2}, kind == 2 ? g_th.onAccent : g_th.focus);
        if (kind == 2) focusRing({rc.left + ring * 2, rc.top + ring * 2, rc.right - ring * 2, rc.bottom - ring * 2}, g_th.onAccent);
    }
}

// Colors of native controls drawn on cards (shared by the panel and the dialogs). Returns a long-lived theme brush.
static LRESULT CtlColor(UINT msg, WPARAM wp, LPARAM lp) {
    HDC dc = (HDC)wp; HWND c = (HWND)lp;
    const int id = GetDlgCtrlID(c);
    const bool roEdit = id == ID_STATUS || id == ID_EXSTATUS;
    if (msg == WM_CTLCOLORSTATIC && !roEdit) {
        SetTextColor(dc, GetPropW(c, L"wbwarn") ? g_th.amber : (id == ID_APPLBL || GetPropW(c, L"wbdim")) ? g_th.dim : g_th.text);
        SetBkColor(dc, g_th.card);
        return (LRESULT)g_th.cardBr;
    }
    SetTextColor(dc, g_th.text); SetBkColor(dc, g_th.input);
    return (LRESULT)g_th.inputBr;
}

// ---------------------------------------------------------------- panel state
struct MeterUi { float disp = -90.f, hold = -90.f; DWORD holdTick = 0; };
struct Card { int tab; RECT r; std::wstring title; };

// All state of one open panel (one instance on the stack of ShowControlPanel, stored in GWLP_USERDATA).
// Owned and used only by the host thread.
// 0.9.2: hover help tip of one top-level window (panel, with its dialogs, or monitor window). Host thread.
struct TipState {
    HWND wnd = nullptr;                      // the tip popup ("WasapiBridgeTip"), created on first use
    HWND top = nullptr;                      // window the current item belongs to (panel, dialog or monitor)
    int ctx = 0;                             // tips::Ctx of that window
    int key = 0;                             // tip key of the item under the cursor / focused item (0 = none)
    RECT itemRc = {};                        // its screen rectangle
    DWORD hoverTick = 0, shownTick = 0;
    bool shown = false, byKeyboard = false, suppressed = false;   // suppressed: after a click, Esc or 15 s
    int tag = 0;                             // tips::Tag of the shown text
    std::wstring text, tagText;
};

struct PanelState {
    WasapiBridge* drv = nullptr;
    HWND wnd = nullptr;
    HFONT font = nullptr, fontB = nullptr, title = nullptr, big = nullptr, fontSmall = nullptr, mono = nullptr;   // 0.9.9: not "small" (rpcndr.h: #define small char under MSVC)
    HWND meter = nullptr;
    MeterSnapshot snap;
    MeterUi mIn[kMaxCh], mOut[kMaxCh];
    DWORD lastMeterTick = 0;
    HBITMAP grad = nullptr; int gradH = 0;            // vertical green->yellow->red gradient of the meter bars
    HBITMAP logoS = nullptr, logoL = nullptr; int logoSW = 0, logoLW = 0, logoSH = 0, logoLH = 0;
    long extraBuf = 0;                 // non-standard buffer size kept as item 8 of the combo
    std::vector<std::wstring> profiles;
    std::vector<std::wstring> exOutShown, exInShown;   // names shown in the extra-device lists
    std::wstring outputMap;            // pending OutputMap (saved on Apply or in a profile)
    std::vector<std::wstring> outs, ins;
    std::vector<HWND> tabCtl[TAB_COUNT];
    std::vector<Card> cards;
    RECT tabRc[TAB_COUNT] = {};
    wchar_t tabKey[TAB_COUNT] = {};          // 0.9.1 (B1): Alt+letter of each tab (0 = none), see AssignMnemonics
    int tabKeyPos[TAB_COUNT] = {};           // its index in the tab name
    RECT tilesRc = {};
    int tab = TAB_MAIN;
    int cw = 840, ch = 700;            // logical client size
    StatusInfo st{}; bool stValid = false;
    bool applied = false, closed = false;
    HWND owner = nullptr;
    int dpi = 96;
    // 0.7.0
    int meterConsumer = WB_METERS_PANEL;     // the monitor window uses its own PanelState with WB_METERS_MONITOR
    std::wstring applyMsg;                   // result of the last Apply (bottom bar, a few seconds)
    DWORD applyTick = 0;
    int statusTicks = 0;
    uint32_t generation = 0;                 // driver stream generation seen last (0.7.1: refresh on change)
    // 0.7.1 (B): rebind to the driver instance that replaces ours. 0 = bound, 1 = waiting ("Restarting
    // driver..."), 2 = no new instance after 5 s (Apply saves config.ini only).
    int rebind = 0;
    DWORD rebindTick = 0;
    bool micDenied = false;                  // 0.7.1 (C): Windows denies microphone access to desktop apps
    bool topState = false;                   // 0.7.1 (A): HWND_TOPMOST currently applied
    HWND dlg = nullptr; bool dlgTop = false; // 0.7.1 (A): open child dialog (name / routing) and its state
    // 0.8.0
    float grDisp = 0.f; DWORD grTick = 0;    // G2: limiter gain reduction shown (held for 1 s)
    std::wstring hostExe;                    // G3: file name of this program (shown, never logged here)
    std::vector<std::wstring> hpKeys;        // G3: program names of the mappings listed in ID_HPLIST
    // 0.8.1: animated background (section "panel visuals" below). Host thread only.
    std::unique_ptr<vis::Renderer> vr, vrAbout;   // window background / About-tab scroller strip
    int visEffect = 0;                       // effect in use (after the remote-session rule), vis::Effect
    int visIntensity = 35;
    bool visTouched = false;                 // the user changed the Visuals combo in this panel
    bool visAnimate = false;                 // timer running (effect on, animations allowed, not paused)
    int visSpeed = 100;                      // 0.8.2: speed in percent (25..400)
    int visPeriod = 15;                      // 0.8.2: animation timer 15 ms (~60 fps) or 31 ms (~30 fps, throttled)
    // 0.8.2 cost accounting over 2 s windows: background render + blit only (never the rest of WM_PAINT)
    double visWinMs = 0.0; int visWinFrames = 0; uint64_t visWinStartUs = 0;
    double visFps = 0.0, visMsPerFrame = 0.0;   // last window's result, shown on the Status tab
    uint64_t visT0Us = 0; int visLevel = 0;
    RECT aboutStrip = {};                    // About tab: scroller strip (device pixels)
    HBITMAP back = nullptr; int backW = 0, backH = 0;   // cached back buffer of WM_PAINT
    vis::Renderer* meterBg = nullptr;        // monitor window only: background renderer of the meter child
    // 0.9.0
    upd::Job* upd = nullptr;                 // H3: running or finished update job (shared with its worker thread)
    std::wstring updMsg;                     // H3: update status line
    std::wstring updSetup;                   // H3: verified Setup ready to run ("" = none)
    std::string updSetupHash;                // H3: its SHA-256 (checked again just before it is started)
    bool updSetupSigned = false;             // 0.9.1: it was signed when downloaded (then it must still be at launch)
    upd::Release updRel;                     // H3: last release found
    bool updNewer = false;                   // H3: updRel is newer than this version
    // 0.9.1
    int modalDepth = 0;                      // fix 7: nested modal loops running (MessageBox, DlgLoop): no rebind meanwhile
    // 0.9.2
    TipState tip;                            // hover help tip (TipShow / TipHide / TipDestroy)
    RECT tileRc[4] = {};                     // Main tab status tiles (client coordinates, set by DrawTiles)
};

static int S(PanelState* p, int v) { return MulDiv(v, p->dpi, 96); }
// 0.9.1 (fix 7): every message box of the panel goes through here. While it runs, the panel timer keeps firing
// inside the box's own message loop; modalDepth makes CheckDriverInstance wait, so p->drv cannot be swapped (and
// the old instance released) under the code that opened the box.
static int PanelMsgBox(PanelState* p, HWND owner, const wchar_t* text, const wchar_t* caption, UINT flags) {
    ++p->modalDepth;
    const int r = AppMessageBox(owner, text, caption, flags);   // 0.9.3: with the application icon
    --p->modalDepth;
    return r;
}
// 0.8.1 panel visuals (defined in the "panel visuals" section)
static void VisBlit(HDC dc, const vis::Renderer& r, const RECT& dst);
static void VisRender(PanelState* p);
static void VisResize(PanelState* p);
static void VisApply(PanelState* p);
static std::wstring StatusText(const StatusInfo& s, const Config& c);
// 0.9.0
#define WM_WB_UPDDONE    (WM_APP + 50)    // update worker finished (posted to the panel)
#define WM_WB_RETHEME    (WM_APP + 61)    // theme or language changed: the monitor window rebuilds itself
static void UpdUpdateUi(PanelState* p);
static void RebuildPanel(PanelState* p);
static void NotifyMonitorWindows();
static std::wstring UpdateRepoOf(const Config& c);
static void LayoutAudit(PanelState* p);
static int VisSpeedFromPos(int v);
static int VisPosFromSpeed(int sp);
static std::wstring VisStatusLine(PanelState* p);

// Creates a child control at logical coordinates (scaled by S), assigns the UI font and dark theme, and records it
// in the control list of tab (TAB_ALL = always visible). Returns the control.
static HWND Ctl(PanelState* p, int tab, const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | style, S(p, x), S(p, y), S(p, w), S(p, h),
                             p->wnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (!c) return nullptr;
    SendMessageW(c, WM_SETFONT, (WPARAM)p->font, TRUE);
    if (!_wcsicmp(cls, L"COMBOBOX")) DarkCtl(c, L"DarkMode_CFD");
    else if (!_wcsicmp(cls, L"EDIT") || !_wcsicmp(cls, L"LISTBOX")) DarkCtl(c, L"DarkMode_Explorer");
    if (tab == TAB_ALL || tab == p->tab) ShowWindow(c, SW_SHOW);
    if (tab >= 0) p->tabCtl[tab].push_back(c);
    return c;
}
// Layout helpers built on Ctl: static label (dim = secondary color), drop-down combo, button/toggle and card.
static void Label(PanelState* p, int tab, const wchar_t* t, int x, int y, int w, bool dim = false) {
    HWND l = Ctl(p, tab, L"STATIC", t, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x, y + 5, w, 20, -1);   // 0.9.0: long translations end with "..."
    if (l && dim) SetPropW(l, L"wbdim", (HANDLE)1);
}
static HWND Combo(PanelState* p, int tab, int x, int y, int w, int id) {
    return Ctl(p, tab, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 320, id);
}
static HWND Btn(PanelState* p, int tab, const wchar_t* t, int x, int y, int w, int h, int id, int kind = 1, bool onBg = false) {
    HWND b = MakeButton(p->wnd, kind == 2 ? p->fontB : p->font, t, S(p, x), S(p, y), S(p, w), S(p, h), id, kind, onBg ? g_th.bg : g_th.card);
    if (!b) return nullptr;
    if (!(tab == TAB_ALL || tab == p->tab)) ShowWindow(b, SW_HIDE);
    if (tab >= 0) p->tabCtl[tab].push_back(b);
    return b;
}
static void AddCard(PanelState* p, int tab, int x, int y, int w, int h, const wchar_t* title) {
    Card c; c.tab = tab; c.r = {S(p, x), S(p, y), S(p, x + w), S(p, y + h)}; c.title = title;
    p->cards.push_back(c);
}

static void UpdateAppRow(PanelState* p);
static void FillHostProfiles(PanelState* p, const std::wstring* want);
static void UpdateBtRow(PanelState* p, bool privacyShown);
static std::wstring CurHostProfile(PanelState* p);
static void UpdatePrivacyRow(PanelState* p);
static void FillDmPairs(PanelState* p, const Config& c);
static void UpdateDmNote(PanelState* p, const StatusInfo& st);
static void FillControls(PanelState* p, const Config& c);
static void ReadControls(PanelState* p, Config& c);

// 0.9.1 (B1): Alt+letter access keys for the tabs and the two main buttons, chosen at run time per language so
// they never collide: for each name (Apply, Close, then the tabs in order) the first letter of a word that is
// still free, else any free letter or digit. The buttons get the "&" in their text (property "wbmnem": their
// text is drawn with the prefix, see DrawOwnerButton); the tabs are drawn with it (PaintPanel) and handled in
// HandleTabKeys (WM_SYSCHAR).
static int PickMnemonic(const std::wstring& name, std::wstring& used) {
    auto ok = [&](wchar_t c) { return iswalnum(c) && used.find((wchar_t)towupper(c)) == std::wstring::npos; };
    for (size_t i = 0; i < name.size(); ++i)
        if ((i == 0 || name[i - 1] == L' ') && ok(name[i])) { used += (wchar_t)towupper(name[i]); return (int)i; }
    for (size_t i = 0; i < name.size(); ++i)
        if (ok(name[i])) { used += (wchar_t)towupper(name[i]); return (int)i; }
    return -1;
}
static std::wstring WithPrefix(const std::wstring& name, int pos) {   // "&" before `pos`, literal "&" doubled
    std::wstring r;
    for (size_t i = 0; i < name.size(); ++i) { if ((int)i == pos) r += L'&'; r += name[i]; if (name[i] == L'&') r += L'&'; }
    return r;
}
static void AssignMnemonics(PanelState* p) {
    std::wstring used;
    const int ids[2] = {IDOK, IDCANCEL};
    const wchar_t* names[2] = {T(S_APPLY), T(S_CLOSE)};
    for (int k = 0; k < 2; ++k) {
        HWND b = GetDlgItem(p->wnd, ids[k]);
        const int pos = PickMnemonic(names[k], used);
        if (b && pos >= 0) { SetWindowTextW(b, WithPrefix(names[k], pos).c_str()); SetPropW(b, L"wbmnem", (HANDLE)1); }
    }
    for (int i = 0; i < TAB_COUNT; ++i) {
        const std::wstring n = TabName(i);
        p->tabKeyPos[i] = PickMnemonic(n, used);
        p->tabKey[i] = p->tabKeyPos[i] >= 0 ? (wchar_t)towupper(n[(size_t)p->tabKeyPos[i]]) : 0;
    }
}
// Enter on a focused button or toggle activates it (IsDialogMessage would press the default button, "Apply",
// instead). Panel and dialogs. True if handled.
static bool EnterOnButton(HWND root, const MSG& m) {
    if (m.message != WM_KEYDOWN || m.wParam != VK_RETURN) return false;
    HWND f = GetFocus();
    if (!f || (f != root && !IsChild(root, f))) return false;
    wchar_t cls[16] = L"";
    if (!GetClassNameW(f, cls, 16) || _wcsicmp(cls, L"BUTTON")) return false;
    if (IsWindowEnabled(f)) SendMessageW(f, BM_CLICK, 0, 0);
    return true;
}
// Accessible names given by Direct Annotation (a11y::AccName) are marked with "wbacc" and removed again before
// the controls are destroyed (RebuildPanel, WM_DESTROY).
static void AccNamed(HWND h, const std::wstring& name) { if (h) { a11y::AccName(h, name.c_str()); SetPropW(h, L"wbacc", (HANDLE)1); } }
static BOOL CALLBACK AccClearProc(HWND h, LPARAM) {
    if (GetPropW(h, L"wbacc")) { a11y::AccClear(h); RemovePropW(h, L"wbacc"); }
    return TRUE;
}
static void AccClearChildren(HWND parent) { if (parent) EnumChildWindows(parent, GuardedEnumProc<AccClearProc>, 0); }

// ---------------------------------------------------------------- help tips (0.9.2)
// Own tip window (no comctl32): WS_POPUP + WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, transparent to
// the mouse (HTTRANSPARENT), drawn with the theme colors (system colors in high contrast), at most 360 logical
// pixels wide with word wrap, in the DPI of its window. Tracking without hooks or threads: every mouse move over
// a child or the window itself arrives as WM_SETCURSOR at the top-level window (DefWindowProc of every control
// asks its parent first), which identifies the item (TipItemAt); a 100 ms host-thread timer (ID_TIPTIMER on the
// panel or monitor window) shows the tip after 600 ms on the same item, hides it when the cursor leaves the item
// and after 15 s. A click, a key, the wheel, a tab switch or a rebuild hides it. F1 / Shift+F1 show the tip of
// the focused control (TipKey; WM_HELP in the monitor window), Esc hides it. The same text is each control's
// accessible description (TipAnnotate; the toggle and slider wrapper asks with a11y::kMsgDescription).
static int TipIdOf(HWND c) { const int id = (int)GetWindowLongPtrW(c, GWLP_ID); return id; }
static bool TipsEnabled(PanelState* p) {
    if (!p || !p->wnd) return false;
    if (HWND t = GetDlgItem(p->wnd, ID_HELPTIPS)) return GetWindowLongPtrW(t, GWLP_USERDATA) != 0;   // panel: live preview
    return p->drv && p->drv->config().helpTips;
}
// Text of a tip key with its tag line (for the accessible description); false if the key has no tip.
static bool TipFullText(int key, std::wstring& out) {
    const tips::Def* d = tips::Find(key);
    if (!d) return false;
    out = T(d->str);
    if (d->tag == tips::kLive) out += std::wstring(L" (") + T(S_TIP_TAG_LIVE) + L")";
    else if (d->tag == tips::kReset) out += std::wstring(L" (") + T(S_TIP_TAG_RESET) + L")";
    return true;
}
// Direct child of `top` that contains `w` (a combo box's edit -> the combo box), or nullptr.
static HWND TipChildOf(HWND top, HWND w) {
    while (w && w != top) { HWND par = GetParent(w); if (par == top) return w; w = par; }
    return nullptr;
}
// Tip key of control `c` (a direct child of a window of context ctx). A label without an id gives the tip of
// the control it introduces (the next visible window in z order).
static int TipKeyOfControl(int ctx, HWND c) {
    int id = TipIdOf(c);
    if (id <= 0 || id == 0xFFFF) {
        wchar_t cls[16] = L"";
        if (!GetClassNameW(c, cls, 16) || _wcsicmp(cls, L"STATIC")) return 0;
        HWND n = GetWindow(c, GW_HWNDNEXT);
        while (n && !IsWindowVisible(n)) n = GetWindow(n, GW_HWNDNEXT);
        if (!n) return 0;
        id = TipIdOf(n);
    }
    const int key = tips::KeyFor(ctx, id);
    return tips::Find(key) ? key : 0;
}
// Item under screen point pt inside `top` (hit = the window WM_SETCURSOR names). Returns the key, rc = screen rect.
static int TipItemAt(PanelState* p, HWND top, int ctx, HWND hit, POINT pt, RECT& rc) {
    if (hit && hit != top && !IsChild(top, hit)) return 0;        // e.g. an open combo box list
    POINT cp = pt; ScreenToClient(top, &cp);
    HWND c = hit && hit != top ? TipChildOf(top, hit) : nullptr;
    auto toScreen = [&](RECT r) { MapWindowPoints(top, nullptr, (POINT*)&r, 2); rc = r; };
    if (!c) {                                                       // painted items, then labels
        if (ctx == tips::kPanel && top == p->wnd) {
            for (int i = 0; i < TAB_COUNT; ++i) if (PtInRect(&p->tabRc[i], cp)) { toScreen(p->tabRc[i]); return ID_TIP_TAB0 + i; }
            if (p->tab == TAB_MAIN) for (int i = 0; i < 4; ++i) if (PtInRect(&p->tileRc[i], cp)) { toScreen(p->tileRc[i]); return ID_TIP_TILE0 + i; }
        }
        if (ctx == tips::kMonitor) {
            RECT cr; GetClientRect(top, &cr);
            RECT hr = {0, 0, cr.right, S(p, 46)};
            if (PtInRect(&hr, cp)) { toScreen(hr); return ID_TIP_MWSUM; }
        }
        c = ChildWindowFromPointEx(top, cp, CWP_SKIPINVISIBLE);
        if (c == top) c = nullptr;
    }
    if (!c) return 0;
    const int key = TipKeyOfControl(ctx, c);
    if (key) GetWindowRect(c, &rc);
    return key;
}
// Size of the tip for the current text (device pixels) and the two text rectangles inside it.
static SIZE TipMeasure(PanelState* p, HDC dc, RECT& body, RECT& tag) {
    TipState& t = p->tip;
    const int pad = S(p, 8), maxW = S(p, 360);
    RECT b = {0, 0, maxW - 2 * pad, 0};
    HGDIOBJ of = SelectObject(dc, p->font);
    DrawTextW(dc, t.text.c_str(), -1, &b, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    RECT g = {0, 0, 0, 0};
    if (!t.tagText.empty()) { SelectObject(dc, p->fontSmall); DrawTextW(dc, t.tagText.c_str(), -1, &g, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX); }
    SelectObject(dc, of);
    const int w = (b.right > g.right ? b.right : g.right) + 2 * pad;
    body = {pad, pad, w - pad, pad + b.bottom};
    tag = {pad, body.bottom + S(p, 4), w - pad, body.bottom + S(p, 4) + g.bottom};
    SIZE sz = {w, (t.tagText.empty() ? body.bottom : tag.bottom) + pad};
    return sz;
}
static LRESULT CALLBACK TipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PanelState* p = (PanelState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;                         // never takes the mouse from the controls
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRectC(dc, 0, 0, rc.right, rc.bottom, g_th.tile);
        HBRUSH br = CreateSolidBrush(g_th.hc ? g_th.text : g_th.accent); FrameRect(dc, &rc, br); DeleteObject(br);
        if (p) {
            RECT body, tag;
            TipMeasure(p, dc, body, tag);
            HGDIOBJ of = SelectObject(dc, p->font);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, g_th.text);
            DrawTextW(dc, p->tip.text.c_str(), -1, &body, DT_WORDBREAK | DT_NOPREFIX);
            if (!p->tip.tagText.empty()) {
                SelectObject(dc, p->fontSmall);
                SetTextColor(dc, p->tip.tag == tips::kReset ? g_th.amber : g_th.accent);
                DrawTextW(dc, p->tip.tagText.c_str(), -1, &tag, DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(dc, of);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); --g_objects; break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void TipHide(PanelState* p) {
    TipState& t = p->tip;
    if (t.wnd && t.shown) ShowWindow(t.wnd, SW_HIDE);
    t.shown = false; t.byKeyboard = false;
}
// Hides the tip and forgets the item (tab switch, rebuild, DPI change, dialog closed).
static void TipCancel(PanelState* p) {
    TipHide(p);
    p->tip.key = 0; p->tip.top = nullptr;
    if (p->wnd) KillTimer(p->wnd, ID_TIPTIMER);
}
static void TipDestroy(PanelState* p) {
    TipCancel(p);
    if (p->tip.wnd && IsWindow(p->tip.wnd)) DestroyWindow(p->tip.wnd);   // --g_objects in its WM_NCDESTROY
    p->tip.wnd = nullptr;
}
// Shows the tip of p->tip.key next to its item (keyboard: below or above the focused control).
static void TipShow(PanelState* p, bool keyboard) {
    TipState& t = p->tip;
    const tips::Def* d = tips::Find(t.key);
    if (!d || !p->wnd) return;
    t.text = T(d->str); t.tag = d->tag;
    t.tagText = d->tag == tips::kLive ? T(S_TIP_TAG_LIVE) : d->tag == tips::kReset ? T(S_TIP_TAG_RESET) : L"";
    if (!t.wnd || !IsWindow(t.wnd)) {
        t.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"WasapiBridgeTip", L"", WS_POPUP,
                                0, 0, 1, 1, p->wnd, nullptr, g_hInst, nullptr);
        if (!t.wnd) return;
        ++g_objects;                                                 // the window keeps the DLL loaded (WM_NCDESTROY)
        SetWindowLongPtrW(t.wnd, GWLP_USERDATA, (LONG_PTR)p);
    }
    HDC dc = GetDC(t.wnd);
    RECT body, tag;
    const SIZE sz = TipMeasure(p, dc, body, tag);
    ReleaseDC(t.wnd, dc);
    POINT cur; GetCursorPos(&cur);
    const RECT& ir = t.itemRc;
    POINT ref = keyboard ? POINT{(ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2} : cur;
    MONITORINFO mi = {}; mi.cbSize = sizeof mi;
    RECT wa = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(ref, MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
    const int gap = S(p, 6);
    int x = keyboard ? ir.left : cur.x - S(p, 4), y;
    if (keyboard || ir.bottom - ir.top <= S(p, 60)) {               // small item: below it, else above it (never over it)
        y = ir.bottom + gap;
        if (y + sz.cy > wa.bottom) y = ir.top - gap - sz.cy;
    } else {                                                        // large item (meter, status text): next to the cursor
        y = cur.y + S(p, 22);
        if (y + sz.cy > wa.bottom) y = cur.y - S(p, 10) - sz.cy;
    }
    if (x + sz.cx > wa.right) x = wa.right - sz.cx;
    if (x < wa.left) x = wa.left;
    if (y + sz.cy > wa.bottom) y = wa.bottom - sz.cy;
    if (y < wa.top) y = wa.top;
    SetWindowPos(t.wnd, HWND_TOPMOST, x, y, sz.cx, sz.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(t.wnd, nullptr, FALSE);
    t.shown = true; t.byKeyboard = keyboard; t.shownTick = GetTickCount();
    SetTimer(p->wnd, ID_TIPTIMER, 100, nullptr);
}
// WM_SETCURSOR of a top-level window (panel, dialog, monitor): hit = window under the cursor, msg = mouse message.
static void TipOnSetCursor(PanelState* p, HWND top, int ctx, HWND hit, UINT mouseMsg) {
    TipState& t = p->tip;
    if (mouseMsg == WM_LBUTTONDOWN || mouseMsg == WM_RBUTTONDOWN || mouseMsg == WM_MBUTTONDOWN || mouseMsg == WM_XBUTTONDOWN ||
        mouseMsg == WM_NCLBUTTONDOWN) { TipHide(p); t.suppressed = true; return; }
    if (mouseMsg != WM_MOUSEMOVE) return;
    if (!IsWindowEnabled(top)) { if (t.top == top) TipCancel(p); return; }   // the panel while one of its dialogs runs
    if (!TipsEnabled(p)) { if (t.shown && !t.byKeyboard) TipHide(p); t.key = 0; return; }
    POINT pt; GetCursorPos(&pt);
    RECT rc = {};
    const int key = TipItemAt(p, top, ctx, hit, pt, rc);
    if (key == t.key && top == t.top && (key == 0 || PtInRect(&t.itemRc, pt))) return;   // still on the same item
    if (t.shown) TipHide(p);
    t.key = key; t.top = top; t.ctx = ctx; t.itemRc = rc; t.hoverTick = GetTickCount(); t.suppressed = false;
    if (key) SetTimer(p->wnd, ID_TIPTIMER, 100, nullptr);
}
// ID_TIPTIMER (100 ms) of the panel or monitor window.
static void TipTick(PanelState* p) {
    TipState& t = p->tip;
    const DWORD now = GetTickCount();
    if (!t.byKeyboard) {
        POINT pt; GetCursorPos(&pt);
        HWND under = WindowFromPoint(pt);
        const bool inside = t.key && t.top && IsWindow(t.top) && PtInRect(&t.itemRc, pt) && under &&
                            (under == t.top || IsChild(t.top, under) || under == t.wnd);
        if (!inside) { TipCancel(p); return; }
        if (!t.shown && !t.suppressed && now - t.hoverTick >= 600 && TipsEnabled(p)) TipShow(p, false);
    }
    if (t.shown && now - t.shownTick >= 15000) { TipHide(p); t.suppressed = true; }
    if (!t.shown && !t.key) KillTimer(p->wnd, ID_TIPTIMER);
}
// Tip of control c (F1, Shift+F1, WM_HELP), shown below or above it whatever the setting (explicit request).
static void TipForControl(PanelState* p, HWND top, int ctx, HWND c) {
    c = TipChildOf(top, c);
    if (!c) return;
    const int key = TipKeyOfControl(ctx, c);
    if (!key) return;
    TipHide(p);
    TipState& t = p->tip;
    t.key = key; t.top = top; t.ctx = ctx; GetWindowRect(c, &t.itemRc); t.suppressed = false;
    TipShow(p, true);
}
// Keyboard part of the message loops of the panel and its dialogs (before IsDialogMessage). True = consumed.
static bool TipKey(PanelState* p, HWND top, int ctx, const MSG& m) {
    if (m.hwnd != top && !IsChild(top, m.hwnd)) return false;
    TipState& t = p->tip;
    if (m.message == WM_KEYDOWN && m.wParam == VK_F1) {             // F1 or Shift+F1: tip of the focused control
        HWND f = GetFocus();
        if (f && (f == top || IsChild(top, f))) TipForControl(p, top, ctx, f);
        return true;
    }
    if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && t.shown) { TipHide(p); t.suppressed = true; return true; }
    if ((m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) && t.shown) TipHide(p);
    if ((m.message == WM_MOUSEWHEEL || m.message == WM_MOUSEHWHEEL) && t.shown) { TipHide(p); t.suppressed = true; }
    return false;
}
// a11y::kMsgDescription: the tip of control c as its accessible description.
static LRESULT TipDescribe(int ctx, HWND top, HWND c, wchar_t* buf) {
    HWND ch = TipChildOf(top, c);
    std::wstring d;
    if (!buf || !ch || !TipFullText(TipKeyOfControl(ctx, ch), d)) return 0;
    wcsncpy(buf, d.c_str(), a11y::kDescLen - 1); buf[a11y::kDescLen - 1] = 0;
    return 1;
}
// Gives every child of `top` that has a tip its accessible description (Direct Annotation, cleared with
// AccClearChildren) and logs the controls without a tip (should be none; tests/strings_test.cpp checks the
// table).
struct TipAnnotateCtx { int ctx; int missing; };
static BOOL CALLBACK TipAnnotateProc(HWND c, LPARAM lp) {
    TipAnnotateCtx* a = (TipAnnotateCtx*)lp;
    HWND top = GetParent(c);
    if (!top) return TRUE;
    std::wstring d;
    const int id = TipIdOf(c);
    if (TipFullText(TipKeyOfControl(a->ctx, c), d)) {
        if (id > 0 && id != 0xFFFF) { a11y::AccDescription(c, d.c_str()); SetPropW(c, L"wbacc", (HANDLE)1); }
    } else if (id > 0 && id != 0xFFFF) ++a->missing;
    return TRUE;
}
static void TipAnnotate(HWND top, int ctx) {
    TipAnnotateCtx a = {ctx, 0};
    for (HWND c = GetWindow(top, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) TipAnnotateProc(c, (LPARAM)&a);
    if (a.missing) Log("Help tips: %d control(s) without a tip (context %d)", a.missing, ctx);
}


// Shows the controls of tab t and hides the others. Moves the keyboard focus off hidden controls so that keyboard
// input never goes to an invisible control, and repaints.
static void ShowTab(PanelState* p, int t) {
    TipCancel(p);                          // 0.9.2
    // If the focus is on a control about to be hidden, move it to the first tab stop of the new tab (or OK)
    HWND f = GetFocus();
    bool focusHidden = false;
    for (int i = 0; i < TAB_COUNT; ++i) if (i != t) for (HWND h : p->tabCtl[i]) if (h == f) focusHidden = true;
    p->tab = t;
    for (int i = 0; i < TAB_COUNT; ++i)
        for (HWND h : p->tabCtl[i]) ShowWindow(h, i == t ? SW_SHOW : SW_HIDE);
    UpdateAppRow(p);
    if (p->vr) VisRender(p);               // 0.8.1: the About strip is rendered only while its tab is shown
    if (focusHidden) {
        HWND first = nullptr;
        for (HWND h : p->tabCtl[t]) if (IsWindowVisible(h) && (GetWindowLongPtrW(h, GWL_STYLE) & WS_TABSTOP)) { first = h; break; }
        SetFocus(first ? first : GetDlgItem(p->wnd, IDOK));
    }
    InvalidateRect(p->wnd, nullptr, TRUE);
}

static std::wstring LowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
// Lists the friendly names of the active endpoints of flow (render or capture). Host thread.
static void ListDevices(IMMDeviceEnumerator* e, EDataFlow flow, std::vector<std::wstring>& out) {
    static const PROPERTYKEY kName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    out.clear();
    IMMDeviceCollection* col = nullptr;
    if (!e || FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr; IPropertyStore* ps = nullptr;
        if (FAILED(col->Item(i, &d))) continue;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kName, &v)) && v.vt == VT_LPWSTR) out.push_back(v.pwszVal);
            PropVariantClear(&v); ps->Release();
        }
        d->Release();
    }
    col->Release();
}

static void Add(HWND c, const wchar_t* t) { SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t); }
static void Sel(HWND c, int i) { SendMessageW(c, CB_SETCURSEL, i, 0); }
static int  Cur(HWND w, int id) { return (int)SendDlgItemMessageW(w, id, CB_GETCURSEL, 0, 0); }
// Index of the first name containing want (case-insensitive substring, the same rule the driver uses), or -1.
static int MatchIndex(const std::vector<std::wstring>& list, const std::wstring& want) {
    if (want.empty()) return -1;
    std::wstring w = LowerW(want);
    for (size_t i = 0; i < list.size(); ++i) if (LowerW(list[i]).find(w) != std::wstring::npos) return (int)i;
    return -1;
}

static const long kBufs[]  = {32, 64, 128, 256, 512, 1024, 2048, 4096};
static const long kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};

// Lists the executable names of processes that currently have an audio session (for Loopback = one application),
// excluding this process, sorted and without duplicates.
static void ListAudioApps(IMMDeviceEnumerator* e, std::vector<std::wstring>& out) {
    out.clear();
    std::vector<DWORD> pids;
    ListAudioSessionPids(e, pids);
    const DWORD self = GetCurrentProcessId();
    for (DWORD pid : pids) {
        if (pid == self) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;
        wchar_t path[MAX_PATH]; DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            std::wstring nm(path, len);
            size_t sl = nm.find_last_of(L"\\/");
            if (sl != std::wstring::npos) nm = nm.substr(sl + 1);
            bool dup = false;
            for (auto& x : out) if (!_wcsicmp(x.c_str(), nm.c_str())) dup = true;
            if (!dup && !nm.empty()) out.push_back(nm);
        }
        CloseHandle(h);
    }
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
}

// Text of a control, trimmed of leading/trailing spaces and tabs.
static std::wstring GetText(HWND w, int id) {
    wchar_t b[512] = L""; GetDlgItemTextW(w, id, b, 512);
    std::wstring s(b);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
    return s.substr(i);
}

// Refills the application combo, keeping what the user typed or selected.
static void RefreshApps(PanelState* p) {
    HWND cb = GetDlgItem(p->wnd, ID_APP);
    std::wstring keep = GetText(p->wnd, ID_APP);
    std::vector<std::wstring> apps;
    ListAudioApps(p->drv->enumerator(), apps);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (auto& a : apps) Add(cb, a.c_str());
    SetWindowTextW(cb, keep.c_str());          // keep what was selected or typed
}

// Refills the profile combo from config.ini ("(no profile)" first) and selects the profile named select.
static void RefreshProfiles(PanelState* p, const std::wstring& select) {
    HWND cb = GetDlgItem(p->wnd, ID_PROFILE);
    p->profiles = ListProfiles();
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    Add(cb, T(S_NO_PROFILE));
    int sel = 0;
    for (size_t i = 0; i < p->profiles.size(); ++i) {
        Add(cb, p->profiles[i].c_str());
        if (!select.empty() && !_wcsicmp(p->profiles[i].c_str(), select.c_str())) sel = (int)i + 1;
    }
    Sel(cb, sel);
    // 0.8.0 (G3): the program-profile combo lists the same profiles (keeps the pending choice when it still exists)
    if (GetDlgItem(p->wnd, ID_HPROF) && SendDlgItemMessageW(p->wnd, ID_HPROF, CB_GETCOUNT, 0, 0) > 0) {
        const std::wstring keep = CurHostProfile(p);
        FillHostProfiles(p, &keep);
    }
}

// Name of the profile selected in the combo, or empty for "(no profile)".
static std::wstring CurProfile(PanelState* p) {
    int i = Cur(p->wnd, ID_PROFILE);
    return i > 0 && (size_t)(i - 1) < p->profiles.size() ? p->profiles[i - 1] : L"";
}

// ---------------------------------------------------------------- extra devices (0.5.0)
static std::wstring DefaultDeviceName(IMMDeviceEnumerator* e, EDataFlow flow) {
    static const PROPERTYKEY kName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    std::wstring name;
    IMMDevice* d = nullptr; IPropertyStore* ps = nullptr;
    if (!e || FAILED(e->GetDefaultAudioEndpoint(flow, eConsole, &d)) || !d) return name;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    d->Release();
    return name;
}

// Name of the main device currently selected on screen (the Windows default if "(Windows default)" is selected)
static std::wstring PrimaryName(PanelState* p, bool input) {
    int i = Cur(p->wnd, input ? ID_IN : ID_OUT);
    if (!input) return i > 0 ? p->outs[i - 1] : DefaultDeviceName(p->drv->enumerator(), eRender);
    // 0.7.1 order of the input combo: 0 = "(None - no inputs)", 1 = "(Windows default)", 2.. = devices
    if (i <= 0) return L"";
    return i > 1 ? p->ins[i - 2] : DefaultDeviceName(p->drv->enumerator(), eCapture);
}

// Extra-device list without the main device; selects the names in sel (substring match, like the driver)
static void FillExtraList(PanelState* p, bool input, const std::vector<std::wstring>& sel) {
    HWND lb = GetDlgItem(p->wnd, input ? ID_EXIN : ID_EXOUT);
    const std::vector<std::wstring>& all = input ? p->ins : p->outs;
    std::vector<std::wstring>& shown = input ? p->exInShown : p->exOutShown;
    const std::wstring prim = PrimaryName(p, input);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    shown.clear();
    for (auto& n : all) {
        if (!prim.empty() && !_wcsicmp(n.c_str(), prim.c_str())) continue;
        shown.push_back(n);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)n.c_str());
    }
    for (auto& w : sel) { int i = MatchIndex(shown, w); if (i >= 0) SendMessageW(lb, LB_SETSEL, TRUE, i); }
}

// Names of the selected extra outputs (input = false) or inputs (true), at most kMaxExtra.
static std::vector<std::wstring> SelectedExtras(PanelState* p, bool input) {
    std::vector<std::wstring> out;
    HWND lb = GetDlgItem(p->wnd, input ? ID_EXIN : ID_EXOUT);
    const std::vector<std::wstring>& shown = input ? p->exInShown : p->exOutShown;
    int idx[64];
    int n = (int)SendMessageW(lb, LB_GETSELITEMS, 64, (LPARAM)idx);
    for (int i = 0; i < n && (int)out.size() < kMaxExtra; ++i) if (idx[i] >= 0 && (size_t)idx[i] < shown.size()) out.push_back(shown[idx[i]]);
    return out;
}

static void LimitExtraSel(PanelState* p, int id) {                // at most kMaxExtra selected
    HWND lb = GetDlgItem(p->wnd, id);
    if (SendMessageW(lb, LB_GETSELCOUNT, 0, 0) > kMaxExtra) {
        int caret = (int)SendMessageW(lb, LB_GETCARETINDEX, 0, 0);
        SendMessageW(lb, LB_SETSEL, FALSE, caret);
        MessageBeep(MB_ICONWARNING);
    }
}

// Channel count of the mix format of the render device whose name contains name (default device if empty).
// Used to predict the ASIO channel layout before applying. Returns 2 if the device cannot be queried.
static int MixChannelsByName(IMMDeviceEnumerator* e, const std::wstring& name) {
    static const PROPERTYKEY kName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    int n = 2;
    if (!e) return n;
    IMMDevice* dev = nullptr;
    IMMDeviceCollection* col = nullptr;
    if (!name.empty() && SUCCEEDED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT cnt = 0; col->GetCount(&cnt);
        for (UINT i = 0; i < cnt && !dev; ++i) {
            IMMDevice* d = nullptr; IPropertyStore* ps = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            bool match = false;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kName, &v)) && v.vt == VT_LPWSTR) match = !_wcsicmp(v.pwszVal, name.c_str());
                PropVariantClear(&v); ps->Release();
            }
            if (match) dev = d; else d->Release();
        }
        col->Release();
    }
    if (!dev) e->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (dev) {
        IAudioClient* c = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) {
            WAVEFORMATEX* m = nullptr;
            if (SUCCEEDED(c->GetMixFormat(&m)) && m) { n = m->nChannels; CoTaskMemFree(m); }
            c->Release();
        }
        dev->Release();
    }
    return n < 1 ? 1 : n;
}

// ASIO output layout for what is selected on screen (not applied yet), with the same rule as the driver:
// main device, then extras in order, at most 32 channels, names "N:Out L" when extras exist.
static void PendingOutLayout(PanelState* p, int& devCh, std::vector<std::wstring>& names) {
    const int maxCh = p->drv->config().maxChannels;
    IMMDeviceEnumerator* e = p->drv->enumerator();
    std::vector<int> counts;
    devCh = MixChannelsByName(e, PrimaryName(p, false));
    if (devCh > maxCh) devCh = maxCh;
    counts.push_back(devCh);
    int used = devCh;
    for (auto& n : SelectedExtras(p, false)) {
        int c = MixChannelsByName(e, n); if (c > maxCh) c = maxCh;
        if (used + c > kMaxCh) c = kMaxCh - used;
        if (c <= 0) break;
        counts.push_back(c); used += c;
    }
    names.clear();
    for (size_t d = 0; d < counts.size(); ++d)
        for (int i = 0; i < counts[d]; ++i) {
            wchar_t b[32];
            if (counts[d] == 2) swprintf(b, 32, L"Out %ls", i == 0 ? L"L" : L"R"); else swprintf(b, 32, L"Out %d", i + 1);
            wchar_t n[40];
            if (counts.size() > 1) swprintf(n, 40, L"%d:%ls", (int)d + 1, b); else swprintf(n, 40, L"%ls", b);
            names.push_back(n);
        }
}

// Deletes the selected profile after confirmation.
static void DeleteCurProfile(PanelState* p) {
    std::wstring name = CurProfile(p);
    if (name.empty()) return;
    std::wstring q = FmtW(T(S_DEL_PROFILE_Q), name.c_str());
    if (PanelMsgBox(p, p->wnd, q.c_str(), WB_NAME_W, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    DeleteProfile(name);
    RefreshProfiles(p, L"");
}

// Loads the selected profile on top of what is on screen (keys absent from the profile keep their values).
// Nothing is saved until "Apply".
static void OnProfileSelected(PanelState* p) {
    std::wstring name = CurProfile(p);
    if (name.empty()) return;
    Config c = p->drv->config();
    ReadControls(p, c);                    // base: what is on screen
    if (LoadProfile(name, c)) FillControls(p, c);
}

// Copies the Status text (including the extra-device lines) to the clipboard (CF_UNICODETEXT).
// Shows a warning if the clipboard is busy. The report contains no user paths (device names only).
static bool CopyToClipboard(HWND owner, const std::wstring& text) {
    bool ok = false;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (h) {
        if (void* m = GlobalLock(h)) { memcpy(m, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); }
        if (OpenClipboard(owner)) {
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h)) { ok = true; h = nullptr; }   // the system owns h now
            CloseClipboard();
        }
        if (h) GlobalFree(h);
    }
    return ok;
}

// 0.9.0: the copied report is always English (support reads one language), whatever the interface language.
static std::wstring EnglishStatus(WasapiBridge* d, PanelState* p) {
    const int cur = Current();
    SetLang(kEn);
    StatusInfo s{};
    d->getStatus(s);                       // the driver's per-device lines use the same table
    std::wstring text = std::wstring(WB_NAME_W L" ") + WB_VERSION_W + L" - Status\r\n" + StatusText(s, d->config());
    if (p) text += VisStatusLine(p);
    text += L"\r\n";
    SetLang(cur);
    return text;
}

static void CopyStatus(PanelState* p) {
    std::wstring text = EnglishStatus(p->drv, p);
    text = SanitizeText(text);             // 0.6.1: no user folders, user name or computer name in a pasted report
    if (!CopyToClipboard(p->wnd, text))
        PanelMsgBox(p, p->wnd, T(S_CLIP_FAIL), WB_NAME_W, MB_ICONWARNING);
}

// Window that must own the panel: the active window of the calling thread (e.g. the host's device setup dialog),
// else the foreground window when it belongs to this thread, else the window passed to init(). Always the
// top-level ancestor.
// Fix 8: never the floating monitor window (it can be destroyed by the driver while the panel is open); then the
// root owner of hostWnd is used.
static bool IsMonitorWindow(HWND w) {
    wchar_t cls[64] = L"";
    return w && GetClassNameW(w, cls, 64) && !wcscmp(cls, L"WasapiBridgeMonitor");
}
static HWND PickOwner(HWND hostWnd) {
    HWND a = GetActiveWindow();
    if (!a) {
        HWND f = GetForegroundWindow();
        if (f && GetWindowThreadProcessId(f, nullptr) == GetCurrentThreadId()) a = f;   // only windows of this thread
    }
    if (a) { HWND root = GetAncestor(a, GA_ROOT); if (root) a = root; }
    if (!a || IsMonitorWindow(a)) {
        a = hostWnd;
        if (a) { HWND root = GetAncestor(a, GA_ROOTOWNER); if (root) a = root; }
    }
    return a;
}

// 0.7.0 (F1). Saves the on-screen settings (and the active profile name) to config.ini and hands them to the
// driver, which applies LIVE settings at once and asks the host to restart it for RESET settings. The panel stays
// open; the result is shown in the bottom bar for a few seconds.
// 0.9.11 ("Apply and Close", independent review R3): returns true when the panel may close right away (the changes
// are live, or the DAW restarts the driver to apply them). False when something must stay visible: the changes
// apply only at the next driver load (ResetOnReload), no instance is held by the host (saved only), or the
// host-profile mapping was saved; the panel then stays open and shows applyMsg as 0.7.0-0.9.10 always did.
static bool Apply(PanelState* p) {
    Config c = p->drv->config();
    ReadControls(p, c);
    c.activeProfile = CurProfile(p);
    SaveConfig(c);
    p->applied = true;
    bool mayClose = false;
    // 0.8.0 (G3): the mapping for this program is stored in [HostProfiles]; it is used at the next driver load.
    bool mapChanged = false;
    if (!p->hostExe.empty()) {
        const std::wstring hp = CurHostProfile(p);
        if (_wcsicmp(hp.c_str(), HostProfileFor(p->hostExe).c_str())) { SetHostProfile(p->hostExe, hp); mapChanged = true; FillHostProfiles(p, &hp); }
    }
    if (!p->drv->hostHoldsRef() || p->rebind) {
        // 0.7.1 (B): no instance is held by the host right now: save only (the next driver load uses it).
        p->applyMsg = T(S_MSG_SAVED_RELEASED);
    } else {
        switch (p->drv->applyPanelChanges(c)) {
        case ApplyResult::Live:           p->applyMsg = T(S_MSG_LIVE); mayClose = true; break;
        case ApplyResult::ResetRequested: p->applyMsg = T(S_MSG_RESET); mayClose = true; break;
        case ApplyResult::ResetOnReload:  p->applyMsg = T(S_MSG_RELOAD); break;
        }
    }
    if (mapChanged) { p->applyMsg += T(S_MSG_HP_SAVED); mayClose = false; }
    p->applyTick = GetTickCount();
    if (HWND b = GetDlgItem(p->wnd, ID_SAFEBTN); b && !p->drv->safeMode()) ShowWindow(b, SW_HIDE);
    RECT bar = {0, S(p, p->ch - 64), S(p, p->cw), S(p, p->ch)};
    InvalidateRect(p->wnd, &bar, FALSE);
    return mayClose;
}

// 0.9.6 (KI-4): true when the panel currently selects at least one recording device: the main Input device is
// "(Windows default)" or a named device (combo index >= 1; index 0 is "(None - no inputs)"), or at least one
// extra input is selected in the list. It reads the controls on screen, not config.ini, so the answer follows
// the user's choice immediately (before Apply), exactly like the "Set to None" button that changes the controls.
// A combo that is not filled yet (Cur returns -1) counts as "no input". Host (panel) thread only.
static bool PanelInputSelected(PanelState* p) {
    if (Cur(p->wnd, ID_IN) >= 1) return true;
    return !SelectedExtras(p, true).empty();
}

// Shows the application row only on the Advanced tab with Loopback = one application; fills the list lazily.
// 0.7.1 (C): the privacy warning row under "Input device" (Main tab) is visible only while Windows denies
// microphone access to desktop apps, or the last open failed with E_ACCESSDENIED.
// 0.9.6 (KI-4): ... and only while an input is actually selected (PanelInputSelected). The Windows setting alone
// is not a reason to warn: with Input = "(None - no inputs)" and no extra inputs the driver never opens a capture
// stream, so the privacy switch cannot affect it and the old warning (with its "Set to None" button, which would
// do nothing) was only confusing. The row also hides as soon as the user picks None, before Apply.
static void UpdatePrivacyRow(PanelState* p) {
    const bool denied = p->micDenied || (p->stValid && p->st.captureDenied);
    const bool show = p->tab == TAB_MAIN && denied && PanelInputSelected(p);
    ShowWindow(GetDlgItem(p->wnd, ID_PRIVLBL), show ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(p->wnd, ID_SETNONE), show ? SW_SHOW : SW_HIDE);
    UpdateBtRow(p, show);                  // 0.8.0 (G1): same place, only when the privacy row is hidden
}

static void UpdateAppRow(PanelState* p) {
    UpdatePrivacyRow(p);
    const bool show = p->tab == TAB_ADV && Cur(p->wnd, ID_LB) == (int)LoopbackMode::App;
    const int ids[3] = {ID_APPLBL, ID_APP, ID_APPREF};
    for (int id : ids) ShowWindow(GetDlgItem(p->wnd, id), show ? SW_SHOW : SW_HIDE);
    if (show && SendDlgItemMessageW(p->wnd, ID_APP, CB_GETCOUNT, 0, 0) == 0) RefreshApps(p);
}

// Selects `v` in the buffer combo; a non-standard value becomes item 8 "(non-standard)".
static void SetBufferCombo(PanelState* p, long v) {
    HWND bf = GetDlgItem(p->wnd, ID_BUF);
    int bsel = -1;
    for (int i = 0; i < 8; ++i) if (kBufs[i] == v) bsel = i;
    if (bsel < 0) {
        wchar_t t[32]; swprintf(t, 32, T(S_NONSTD), v);
        if (SendMessageW(bf, CB_GETCOUNT, 0, 0) > 8) SendMessageW(bf, CB_DELETESTRING, 8, 0);
        SendMessageW(bf, CB_INSERTSTRING, 8, (LPARAM)t);
        p->extraBuf = v;
        bsel = 8;
    }
    Sel(bf, bsel);
}
// Buffer currently selected in the combo (0 if none).
static long CurBuffer(PanelState* p) {
    const int b = Cur(p->wnd, ID_BUF);
    return b >= 0 && b < 8 ? kBufs[b] : b == 8 ? p->extraBuf : 0;
}

// 0.8.0 (G3). Fills "Profile for this program" (selecting `want`, or the stored mapping when `want` is null) and
// the list of all stored mappings. Host thread.
static void FillHostProfiles(PanelState* p, const std::wstring* want) {
    HWND pc = GetDlgItem(p->wnd, ID_HPROF), lc = GetDlgItem(p->wnd, ID_HPLIST);
    if (!pc || !lc) return;
    const std::wstring sel = want ? *want : HostProfileFor(p->hostExe);
    SendMessageW(pc, CB_RESETCONTENT, 0, 0);
    Add(pc, T(S_HP_NONE));
    int si = 0, i = 1;
    for (const std::wstring& n : ListProfiles()) { Add(pc, n.c_str()); if (!_wcsicmp(n.c_str(), sel.c_str())) si = i; ++i; }
    Sel(pc, si);
    EnableWindow(pc, !p->hostExe.empty());
    SendMessageW(lc, CB_RESETCONTENT, 0, 0);
    p->hpKeys.clear();
    for (const auto& m : ListHostProfiles()) { Add(lc, (m.first + L"  \x2192  " + m.second).c_str()); p->hpKeys.push_back(m.first); }
    if (p->hpKeys.empty()) Add(lc, T(S_HP_EMPTY));
    Sel(lc, 0);
    EnableWindow(GetDlgItem(p->wnd, ID_HPDEL), !p->hpKeys.empty());
}
// Profile chosen for this program ("" = none).
static std::wstring CurHostProfile(PanelState* p) {
    const int i = Cur(p->wnd, ID_HPROF);
    if (i <= 0) return L"";
    HWND pc = GetDlgItem(p->wnd, ID_HPROF);
    const int n = (int)SendMessageW(pc, CB_GETLBTEXTLEN, i, 0);
    if (n <= 0) return L"";
    std::vector<wchar_t> b((size_t)n + 1, 0);
    SendMessageW(pc, CB_GETLBTEXT, i, (LPARAM)b.data());
    return b.data();
}

// 0.8.0 (G1). Main tab row under the device combos (shares the place of the privacy row, which wins): explains a
// blocked Bluetooth microphone, or recommends a buffer for a Bluetooth output with a one-click button.
static void UpdateBtRow(PanelState* p, bool privacyShown) {
    const StatusInfo& s = p->st;
    const bool any = p->stValid && (s.outBluetooth || s.a2dpBlocked);
    const bool show = p->tab == TAB_MAIN && !privacyShown && any;
    HWND l = GetDlgItem(p->wnd, ID_BTLBL), b = GetDlgItem(p->wnd, ID_BTREC);
    if (l && show) {
        wchar_t t[200];
        if (s.a2dpBlocked) swprintf(t, 200, T(S_BT_BLOCKED));
        else swprintf(t, 200, T(S_BT_REC), s.recBuffer);
        wchar_t cur[200] = L""; GetWindowTextW(l, cur, 200);
        if (wcscmp(cur, t)) SetWindowTextW(l, t);
    }
    if (l) ShowWindow(l, show ? SW_SHOW : SW_HIDE);
    const bool btn = show && s.outBluetooth && s.recBuffer > 0 && CurBuffer(p) < s.recBuffer;
    if (b) ShowWindow(b, btn ? SW_SHOW : SW_HIDE);
}

// Config values -> controls (device lists already filled)
static void FillControls(PanelState* p, const Config& c) {
    HWND w = p->wnd;
    Sel(GetDlgItem(w, ID_OUT), MatchIndex(p->outs, c.outputDevice) + 1);
    int mi = MatchIndex(p->ins, c.inputDevice);
    // 0.7.1: (None) first, then (Windows default). Config values are unchanged: "none" / "" / name substring.
    Sel(GetDlgItem(w, ID_IN), !_wcsicmp(c.inputDevice.c_str(), L"none") ? 0 : (mi >= 0 ? mi + 2 : 1));
    Sel(GetDlgItem(w, ID_LB), (int)c.loopback);
    SetDlgItemTextW(w, ID_APP, c.loopbackApp.c_str());
    Sel(GetDlgItem(w, ID_MODE), c.exclusive ? 1 : 0);
    SetBufferCombo(p, c.bufferSize);       // non-standard (config.ini or profile): item 8 with that value
    int rsel = 1;
    for (int i = 0; i < 6; ++i) if (kRates[i] == (long)c.sampleRate) rsel = i;
    if (c.sampleRateAuto) rsel = 6;        // 0.8.0 (G4): "Follow Windows"
    Sel(GetDlgItem(w, ID_RATE), rsel);
    Sel(GetDlgItem(w, ID_DRIFT), (int)c.drift);
    Sel(GetDlgItem(w, ID_RESAMP), (int)c.resampler);
    Sel(GetDlgItem(w, ID_FRENG), (int)c.frEngine);                                    // 0.9.10
    SetChk(w, ID_LL, c.lowLatencyShared);
    SetChk(w, ID_FOLLOW, c.followDevices);
    SetChk(w, ID_LOG, c.log);
    SetChk(w, ID_DITHER, c.dither);
    SetChk(w, ID_ALIGN, c.alignDevices);
    wchar_t g[32]; swprintf(g, 32, L"%.1f", c.gain > 0 ? 20.0 * log10(c.gain) : -60.0);
    SetDlgItemTextW(w, ID_GAIN, g);
    UpdateAppRow(p);
    FillExtraList(p, false, c.extraOutputs);
    FillExtraList(p, true, c.extraInputs);
    p->outputMap = c.outputMap;
    // 0.7.0
    SetChk(w, ID_MONWIN, c.monitorWindow);
    swprintf(g, 32, L"%.1f", c.outLatOffsetMs); SetDlgItemTextW(w, ID_OFSOUT, g);
    swprintf(g, 32, L"%.1f", c.inLatOffsetMs); SetDlgItemTextW(w, ID_OFSIN, g);
    SetChk(w, ID_DMON, c.dmEnabled);
    SetChk(w, ID_DMMUTE, c.dmMute);
    swprintf(g, 32, L"%.1f", c.dmLevelDb); SetDlgItemTextW(w, ID_DMLVL, g);
    swprintf(g, 32, L"%.0f", c.dmPan); SetDlgItemTextW(w, ID_DMPAN, g);
    FillDmPairs(p, c);
    // 0.8.0
    SetChk(w, ID_A2DP, c.protectA2dp);
    SetChk(w, ID_LIMON, c.limiterOn);
    swprintf(g, 32, L"%.1f", c.limiterCeilDb); SetDlgItemTextW(w, ID_LIMCEIL, g);
    SetChk(w, ID_LIMCAP, c.limiterCapOn);
    swprintf(g, 32, L"%.1f", c.limiterCapDb); SetDlgItemTextW(w, ID_LIMCAPDB, g);
    SetChk(w, ID_TRAY, c.trayIcon);
    // 0.8.1
    Sel(GetDlgItem(w, ID_VIS), c.visuals);
    if (HWND sl = GetDlgItem(w, ID_VISINT)) { SetWindowLongPtrW(sl, GWLP_USERDATA, c.visualsIntensity); InvalidateRect(sl, nullptr, FALSE); }
    if (HWND sl = GetDlgItem(w, ID_VISSPD)) { SetWindowLongPtrW(sl, GWLP_USERDATA, VisPosFromSpeed(c.visualsSpeed)); InvalidateRect(sl, nullptr, FALSE); }
    SetChk(w, ID_VISMON, c.visualsMonitor);
    SetChk(w, ID_HELPTIPS, c.helpTips);                   // 0.9.2
    // 0.9.0
    Sel(GetDlgItem(w, ID_THEME), c.theme);
    Sel(GetDlgItem(w, ID_LANG), c.language);
    SetChk(w, ID_UPDWEEKLY, c.updateCheck);
    UpdUpdateUi(p);
}

// Controls -> Config (starts from c to keep keys the panel does not show, e.g. MaxChannels)
static void ReadControls(PanelState* p, Config& c) {
    HWND w = p->wnd;
    int o = Cur(w, ID_OUT);  c.outputDevice = o > 0 ? p->outs[o - 1] : L"";
    int i = Cur(w, ID_IN);   c.inputDevice = i <= 0 ? L"none" : i > 1 ? p->ins[i - 2] : L"";
    c.loopback  = (LoopbackMode)(Cur(w, ID_LB) < 0 ? 0 : Cur(w, ID_LB));
    c.loopbackApp = GetText(w, ID_APP);
    c.exclusive = Cur(w, ID_MODE) == 1;
    int b = Cur(w, ID_BUF);  if (b >= 0 && b < 8) c.bufferSize = kBufs[b]; else if (b == 8 && p->extraBuf > 0) c.bufferSize = p->extraBuf;
    int r = Cur(w, ID_RATE);
    if (r == 6) c.sampleRateAuto = true;                       // 0.8.0 (G4): SampleRate=Auto keeps the last rate
    else if (r >= 0) { c.sampleRateAuto = false; c.sampleRate = (double)kRates[r]; }
    c.drift = (DriftMode)(Cur(w, ID_DRIFT) < 0 ? 0 : Cur(w, ID_DRIFT));
    c.resampler = Cur(w, ID_RESAMP) == 1 ? ResamplerQ::Sinc : ResamplerQ::Hermite;
    {   // 0.9.10
        const int fe = Cur(w, ID_FRENG);
        c.frEngine = fe >= 0 && fe <= 3 ? (FrEngine)fe : FrEngine::Auto;
    }
    c.lowLatencyShared = GetChk(w, ID_LL);
    c.followDevices    = GetChk(w, ID_FOLLOW);
    c.log              = GetChk(w, ID_LOG);
    c.dither           = GetChk(w, ID_DITHER);
    double db = wcstod(GetText(w, ID_GAIN).c_str(), nullptr); if (db < -60) db = -60; if (db > 24) db = 24;
    c.gain = pow(10.0, db / 20.0);
    c.extraOutputs = SelectedExtras(p, false);
    c.extraInputs  = SelectedExtras(p, true);
    c.outputMap    = p->outputMap;
    c.alignDevices = GetChk(w, ID_ALIGN);
    // 0.7.0 (out-of-range values are clamped again by the config reader)
    auto num = [&](int id, double lo, double hi, double def) {
        std::wstring t = GetText(w, id);
        if (t.empty()) return def;
        double v = wcstod(t.c_str(), nullptr);
        return v < lo ? lo : v > hi ? hi : v;
    };
    c.monitorWindow = GetChk(w, ID_MONWIN);
    c.outLatOffsetMs = num(ID_OFSOUT, -50, 500, 0);
    c.inLatOffsetMs = num(ID_OFSIN, -50, 500, 0);
    c.dmEnabled = GetChk(w, ID_DMON);
    c.dmMute = GetChk(w, ID_DMMUTE);
    c.dmLevelDb = num(ID_DMLVL, -60, 6, 0);
    c.dmPan = num(ID_DMPAN, -100, 100, 0);
    const int di = Cur(w, ID_DMIN), dq = Cur(w, ID_DMOUT);
    if (di >= 0) c.dmInput = di * 2;
    if (dq >= 0) c.dmOutput = dq * 2;
    // 0.8.0
    c.protectA2dp = GetChk(w, ID_A2DP);
    c.limiterOn = GetChk(w, ID_LIMON);
    c.limiterCeilDb = num(ID_LIMCEIL, -20, -0.1, -1);
    c.limiterCapOn = GetChk(w, ID_LIMCAP);
    c.limiterCapDb = num(ID_LIMCAPDB, -20, 24, 0);
    c.trayIcon = GetChk(w, ID_TRAY);
    // 0.8.1 (LIVE; the Visuals key becomes explicit once the user changed it here)
    const int vsel = Cur(w, ID_VIS);
    if (vsel >= 0) c.visuals = vsel;
    if (p->visTouched) c.visualsExplicit = true;
    if (HWND sl = GetDlgItem(w, ID_VISINT)) c.visualsIntensity = (int)GetWindowLongPtrW(sl, GWLP_USERDATA);
    if (HWND sl = GetDlgItem(w, ID_VISSPD)) c.visualsSpeed = VisSpeedFromPos((int)GetWindowLongPtrW(sl, GWLP_USERDATA));
    c.visualsMonitor = GetChk(w, ID_VISMON);
    c.helpTips = GetChk(w, ID_HELPTIPS);                  // 0.9.2
    // 0.9.0
    if (Cur(w, ID_THEME) >= 0) c.theme = Cur(w, ID_THEME);
    if (Cur(w, ID_LANG) >= 0) c.language = Cur(w, ID_LANG);
    c.updateCheck = GetChk(w, ID_UPDWEEKLY);
}

// 0.7.0 (F5): fills the direct-monitoring pair lists from the opened layout (device inputs only; main output
// only) and the note line (microphone privacy verdict, or the latency note).
static void FillDmPairs(PanelState* p, const Config& c) {
    StatusInfo st{};
    p->drv->getStatus(st);
    HWND in = GetDlgItem(p->wnd, ID_DMIN), out = GetDlgItem(p->wnd, ID_DMOUT);
    if (!in || !out) return;
    SendMessageW(in, CB_RESETCONTENT, 0, 0);
    SendMessageW(out, CB_RESETCONTENT, 0, 0);
    wchar_t t[32];
    for (long k = 0; k < st.nInDev; k += 2) {
        if (k + 1 < st.nInDev) swprintf(t, 32, T(S_DM_IN_PAIR), k + 1, k + 2); else swprintf(t, 32, T(S_DM_IN_MONO), k + 1);
        Add(in, t);
    }
    for (long k = 0; k + 1 < st.nOutPrim || k == 0; k += 2) {
        if (k + 1 < st.nOutPrim) swprintf(t, 32, T(S_DM_OUT_PAIR), k + 1, k + 2); else swprintf(t, 32, T(S_DM_OUT_ONE), k + 1);
        Add(out, t);
        if (k + 1 >= st.nOutPrim) break;
    }
    if (st.nInDev <= 0) Add(in, T(S_NO_DEV_INPUT_ITEM));
    Sel(in, st.nInDev > 0 && c.dmInput / 2 < (st.nInDev + 1) / 2 ? c.dmInput / 2 : 0);
    Sel(out, c.dmOutput / 2 < (st.nOutPrim + 1) / 2 ? c.dmOutput / 2 : 0);
    UpdateDmNote(p, st);
}

// Note line of the direct-monitoring card. The microphone privacy verdict (F9) wins when access is denied.
// 0.9.6 (KI-4): ... and an input is selected (PanelInputSelected); with no input selected the card says "no
// input" (S_DM_NOIN) like before, instead of blaming the Windows privacy setting that does not matter then.
static void UpdateDmNote(PanelState* p, const StatusInfo& st) {
    HWND n = GetDlgItem(p->wnd, ID_DMNOTE);
    if (!n) return;
    const MicPrivacy mp = ReadMicPrivacy();
    if (p->micDenied != mp.denied) { p->micDenied = mp.denied; UpdatePrivacyRow(p); }
    const bool privWarn = mp.denied && PanelInputSelected(p);   // 0.9.6 (KI-4)
    const wchar_t* text;
    if (privWarn) text = T(S_DM_PRIV);
    else if (st.nInDev <= 0) text = T(S_DM_NOIN);
    else if (st.streamsOpen && st.monAvail == 0) text = T(S_DM_NORATE);   // 0.9.4: converted output, input at another rate
    else text = T(S_DM_LAT);
    wchar_t cur[160] = L""; GetWindowTextW(n, cur, 160);
    if (wcscmp(cur, text)) SetWindowTextW(n, text);
    const bool warn = privWarn || (st.streamsOpen && st.monAvail == 0 && st.nInDev > 0);
    if ((GetPropW(n, L"wbwarn") != nullptr) != warn) {
        if (warn) SetPropW(n, L"wbwarn", (HANDLE)1); else RemovePropW(n, L"wbwarn");
        InvalidateRect(n, nullptr, TRUE);
    }
}


// ---------------------------------------------------------------- status (full text, per device, tiles)
// 0.7.1 (B). Host thread, panel timer. Keeps the panel bound to the driver instance the host actually uses:
//  - same instance, still held by the host, but its buffers were recreated (generation changed): only the
//    direct-monitoring channel lists are refreshed (on-screen edits are kept);
//  - the host released our instance (only the panel holds it now): look for the instance the host created to
//    replace it (FindNewerInstance, which returns it already referenced). Found -> Release the old one (it may be destroyed right
//    here, on the host thread, which is allowed), keep every control and the selected tab. Not found -> show
//    "Restarting driver..." and keep polling; after 5 s show that the DAW released the driver (Apply then only
//    saves config.ini). Polling continues, so a later restart of the audio engine still rebinds.
static void CheckDriverInstance(PanelState* p) {
    if (p->modalDepth > 0) return;           // fix 7: a nested dialog or message box runs; rebind after it closes
    WasapiBridge* d = p->drv;
    if (d->hostHoldsRef() && !d->isDying()) {
        if (p->rebind) { p->rebind = 0; }
        if (d->generation() != p->generation) {
            p->generation = d->generation();
            Config c = d->config(); ReadControls(p, c); FillDmPairs(p, c);
        }
        return;
    }
    if (WasapiBridge* n = FindNewerInstance(d)) {   // fix 3: already referenced (taken under the registry lock)
        p->drv = n;
        p->generation = n->generation();
        p->rebind = 0;
        d->Release();                         // the old instance may be destroyed here
        Config c = n->config(); ReadControls(p, c); FillDmPairs(p, c);
        p->applyMsg = T(S_MSG_REBOUND);
        p->applyTick = GetTickCount();
        if (HWND ok = GetDlgItem(p->wnd, IDOK)) EnableWindow(ok, TRUE);
        return;
    }
    const DWORD now = GetTickCount();
    if (p->rebind == 0) { p->rebind = 1; p->rebindTick = now; }
    else if (p->rebind == 1 && now - p->rebindTick > 5000) p->rebind = 2;
}

// Full status text (Status tab, "Copy status" of the panel and of the tray menu). Host thread.
static std::wstring StatusText(const StatusInfo& s, const Config& c) {
    wchar_t b[3200];
    std::wstring more;                     // 0.8.0 lines, before the per-device lines
    wchar_t t[256];
    if (s.outBluetooth || s.inBluetooth || s.a2dpBlocked) {
        swprintf(t, 256, T(S_ST_BT), s.outBluetooth ? T(S_YES) : T(S_NO), s.inBluetooth ? T(S_YES) : T(S_NO),
                 s.a2dpBlocked ? T(S_ST_BT_BLOCKED) : L"",
                 s.outBluetooth ? T(S_ST_BT_NODELAY) : L"");
        more += t;
        if (s.outBluetooth && s.recBuffer > 0) { swprintf(t, 256, T(S_ST_RECBUF), s.recBuffer); more += t; }
    }
    if (s.limiterOn) {
        swprintf(t, 256, T(S_ST_LIMITER), c.limiterCeilDb, c.limiterCapOn ? T(S_ST_GAINCAP) : L"");
        more += t;
        if (c.limiterCapOn) { swprintf(t, 256, L" %.1f dB", c.limiterCapDb); more += t; }
    }
    if (s.rateAuto) more += T(S_ST_AUTORATE);
    if (!s.hostProfile.empty()) more += FmtW(T(S_ST_HOSTPROF), s.hostProfile.c_str());
    more += s.extras;
    if (!s.streamsOpen) {
        swprintf(b, 3200, T(S_ST_NOSTREAMS),
                 s.nIn, s.nOut);
    } else {
        const wchar_t* modeName = s.outExclusive ? T(S_EXCLUSIVE) : s.outLowLatency ? T(S_SHARED_LL_FULL) : T(S_SHARED_STD);
        // 0.9.11 (KI-12): Mode=Exclusive configured but the stream runs Shared: say why (a device fixed at another
        // rate by its own driver is the usual reason), instead of a bare "Shared" that contradicts the Mode combo.
        // Review R2: exclFailHr == 0 means IsFormatSupported refused every format (no Initialize was reached).
        auto exclNote = [&](const wchar_t* base) -> std::wstring {
            if (s.exclFailMix > 0 && std::fabs(s.exclFailMix - s.exclFailRate) >= 0.5) return FmtW(T(S_ST_EXCL_FIXED), base, s.exclFailRate, s.exclFailMix);
            if (s.exclFailHr == 0) return FmtW(T(S_ST_EXCL_NOFMT), base, s.exclFailRate);
            return FmtW(T(S_ST_EXCL_REFUSED), base, (unsigned long)s.exclFailHr);
        };
        std::wstring modeStr = modeName;
        if (s.exclusiveWanted && !s.outExclusive && s.exclFail == 1) modeStr = exclNote(modeName);
        const wchar_t* mode = modeStr.c_str();
        // Review R1: the output opened Exclusive but the main input did not (exclFail == 2): said on the Input line
        std::wstring inStr = s.hasInput ? s.inName : std::wstring(T(S_NONE_PAREN));
        if (s.exclusiveWanted && s.hasInput && s.exclFail == 2) inStr = exclNote(s.inName.c_str());
        // 0.9.0: one translated format per line (the placeholders are checked by tests/strings_test.cpp)
        // 0.9.4: engine line (low-latency period, device rate, internal rate conversion)
        std::wstring eng;
        if (!s.outExclusive && s.engineKind >= 0) {
            const wchar_t* kind = s.engineKind == 1 ? T(S_ENG_LL) : s.engineKind == 2 ? T(S_ENG_MIX) : T(S_ENG_STD);
            const std::wstring conv = s.outConv ? FmtW(T(S_ENG_CONV), s.rate, s.outDevRate, s.convSoxr ? L"soxr HQ" : s.convSinc ? L"sinc" : L"Hermite")
                                    : std::wstring(s.engineKind == 0 && s.mixRate > 0 && std::fabs(s.mixRate - s.rate) >= 0.5 ? T(S_ENG_WINCONV) : T(S_ENG_NOCONV));
            eng = FmtW(T(S_ST_ENGINE), kind, s.outPeriod, s.outDevRate > 0 ? 1000.0 * s.outPeriod / s.outDevRate : 0.0, s.outDevRate, conv.c_str());
        }
        std::wstring r = FmtW(T(S_ST_MODE), mode) + eng + FmtW(T(S_ST_OUTPUT), s.outName.c_str(), s.outFmt, s.outPeriod) +
            FmtW(T(S_ST_INPUT), inStr.c_str(), s.inFmt) +
            FmtW(T(S_ST_BUFFER), s.bufSize, s.rate, s.nIn, s.nOut) +
            FmtW(T(S_ST_LAT), 1000.0 * s.inLat / s.rate, 1000.0 * s.outLat / s.rate) +
            FmtW(T(S_ST_GLITCH), s.outGlitches, s.timeouts) + FmtW(T(S_ST_INPUTS), s.inUnderruns, s.inDrops, s.inGlitches) +
            // 0.9.11 (KI-13): what the capture client delivered (only with an input; all zero before the first pull)
            (s.hasInput ? FmtW(T(S_ST_CAPTURE), s.capPulls, s.capEmpty, s.capMulti, s.capMaxPkt, s.capMaxWake, s.capMaxFill) : std::wstring()) +
            FmtW(T(S_ST_DRIFT), s.inPpm, s.inResample ? T(S_CORRECTING) : T(S_NO_CORRECTION), s.hasLoopback ? T(S_ON_L) : T(S_OFF_L),
                 s.lbPpm, s.monitorActive ? T(S_ON_L) : T(S_OFF_L)) +
            // 0.9.11: the engine that really converts the main output (0.9.10 printed the drift resampler's kind here,
            // "Hermite" even while the Engine line said sinc or soxr); the drift resampler only when it is active
            FmtW(T(S_ST_DSP), s.loadNow, s.loadMax, s.outConv ? (s.convSoxr ? L"soxr HQ" : s.convSinc ? L"sinc" : L"Hermite") : T(S_CONV_NONE), s.dither ? L"TPDF" : T(S_NO)) +
            (s.inResample ? FmtW(T(S_ST_DRIFTRES), s.sinc ? L"sinc" : L"Hermite") : std::wstring());
        wcsncpy(b, r.c_str(), 3199); b[3199] = 0;
    }
    return std::wstring(b) + more;
}

static void UpdateStatus(PanelState* p) {
    CheckDriverInstance(p);
    StatusInfo s{};
    p->drv->getStatus(s);
    const std::wstring text = StatusText(s, p->drv->config()) + VisStatusLine(p);   // 0.8.2: visuals line
    SetDlgItemTextW(p->wnd, ID_STATUS, text.c_str());
    std::wstring ex = s.extras;
    while (!ex.empty() && (ex[0] == L'\r' || ex[0] == L'\n')) ex.erase(0, 1);
    SetDlgItemTextW(p->wnd, ID_EXSTATUS, ex.empty() ? T(S_NO_EXTRA) : ex.c_str());
    p->st = s; p->stValid = true;
    if ((p->statusTicks++ & 7) == 0) UpdateDmNote(p, s);     // registry read every ~4 s
    UpdatePrivacyRow(p);                                     // 0.8.0: also the Bluetooth row (status dependent)
    if (HWND hint = GetDlgItem(p->wnd, ID_OFSHINT)) {        // G1: suggested offset, never applied automatically
        const wchar_t* t = s.outBluetooth ? T(S_OFS_HINT_BT) : T(S_OFS_HINT);
        wchar_t cur[96] = L""; GetWindowTextW(hint, cur, 96);
        if (wcscmp(cur, t)) SetWindowTextW(hint, t);
    }
    if (!p->applyMsg.empty() && GetTickCount() - p->applyTick > 6000) p->applyMsg.clear();
    InvalidateRect(p->wnd, &p->tilesRc, FALSE);
    RECT bar = {0, S(p, p->ch - 64), S(p, p->cw), S(p, p->ch)};
    InvalidateRect(p->wnd, &bar, FALSE);
}

// ---------------------------------------------------------------- meters
static const float kFloorDb = -60.f;
static float ToDb(float lin) { return lin > 1e-6f ? 20.f * log10f(lin) : -120.f; }

// Host thread (50 ms timer): takes the peaks accumulated by the audio thread (readMeters, lock-free) and updates
// the falling display value and the peak hold of every bar.
static void UpdateMeters(PanelState* p) {
    p->drv->readMeters(p->snap, p->meterConsumer);
    DWORD now = GetTickCount();
    float dt = p->lastMeterTick ? (now - p->lastMeterTick) / 1000.f : 0.05f;
    if (dt > 0.5f) dt = 0.5f;
    p->lastMeterTick = now;
    auto upd = [&](MeterUi& m, float lin) {
        float db = ToDb(lin);
        float fall = m.disp - 24.f * dt;                // falls at 24 dB/s
        m.disp = db > fall ? db : fall;
        if (m.disp < -90.f) m.disp = -90.f;
        if (db >= m.hold || now - m.holdTick > 1500) { m.hold = db > -90.f ? db : -90.f; m.holdTick = now; }   // hold for 1.5 s
    };
    for (int c = 0; c < kMaxCh; ++c) { upd(p->mIn[c], p->snap.in[c]); upd(p->mOut[c], p->snap.out[c]); }
    // 0.8.0 (G2): limiter gain reduction, held for 1 s so short reductions stay readable
    if (!p->snap.limOn) p->grDisp = 0.f;
    else if (p->snap.limGrDb >= p->grDisp || now - p->grTick > 1000) { p->grDisp = p->snap.limGrDb; p->grTick = now; }
    if (p->meter && p->tab == TAB_MAIN) InvalidateRect(p->meter, nullptr, FALSE);
}

// Clears peak-hold and clip indicators of every bar.
static void ClearHolds(PanelState* p) {
    for (int c = 0; c < kMaxCh; ++c) { p->mIn[c] = MeterUi{}; p->mOut[c] = MeterUi{}; }
}

// Maps a level in dB (clamped to [kFloorDb, 0]) to a y coordinate between top (0 dB) and bottom (floor).
static int DbToY(float db, int top, int bottom) {
    if (db < kFloorDb) db = kFloorDb;
    if (db > 0.f) db = 0.f;
    return bottom - (int)((db - kFloorDb) / -kFloorDb * (bottom - top) + 0.5f);
}

// Linear blend of two colors, t in [0, 1].
static COLORREF Mix(COLORREF a, COLORREF b, float t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

// 1 x h vertical gradient: green up to -18 dB, green->yellow up to -6, yellow->red up to 0.
static void EnsureGradient(PanelState* p, int h) {
    if (p->grad && p->gradH == h) return;
    if (p->grad) DeleteObject(p->grad);
    p->grad = nullptr; p->gradH = 0;
    if (h <= 0) return;
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    p->grad = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!p->grad) return;
    const COLORREF green = g_th.meterLo, yellow = g_th.meterMid, red = g_th.meterHi;   // 0.9.0: per theme
    uint32_t* px = (uint32_t*)bits;
    for (int y = 0; y < h; ++y) {
        const float db = kFloorDb * (float)y / (float)(h - 1 > 0 ? h - 1 : 1);   // 0 at the top, -60 at the bottom
        COLORREF c = db < -18.f ? green : db < -6.f ? Mix(green, yellow, (db + 18.f) / 12.f) : Mix(yellow, red, (db + 6.f) / 6.f);
        px[y] = (uint32_t)GetBValue(c) | ((uint32_t)GetGValue(c) << 8) | ((uint32_t)GetRValue(c) << 16);
    }
    p->gradH = h;
}

// Group of bars (inputs or outputs) in [x0, x1); bars from barTop to barBot; channel numbers and group name below.
static void DrawBars(PanelState* p, HDC dc, HDC gdc, int x0, int x1, int barTop, int barBot, int labelY, int bottom,
                     const wchar_t* name, long n, const MeterUi* ui, const bool* act, uint32_t clip) {
    if (n <= 0) {
        RECT r = {x0, barTop, x1, barBot};
        TextC(dc, p->fontSmall, g_th.dim, T(S_NO_CHANNELS), r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const int avail = x1 - x0;
    int slot = avail / (int)n; if (slot > S(p, 34)) slot = S(p, 34);
    if (slot < 1) slot = 1;
    const int gap = slot >= S(p, 10) ? S(p, 3) : 1;
    const int bw = slot - gap > 1 ? slot - gap : 1;
    const int used = slot * (int)n, start = x0 + (avail - used) / 2;
    const int ledH = S(p, 4), barH = barBot - barTop;
    const int every = slot >= S(p, 16) ? 1 : (S(p, 16) + slot - 1) / slot;   // numbers must not overlap
    for (long c = 0; c < n; ++c) {
        const int l = start + (int)c * slot, r = l + bw;
        FillRectC(dc, l, barTop - ledH - S(p, 3), r, barTop - S(p, 3), (clip >> c) & 1 ? g_th.red : g_th.clipOff);
        FillRectC(dc, l, barTop, r, barBot, act[c] ? g_th.barOn : g_th.barOff);
        if (act[c] && ui[c].disp > kFloorDb && p->grad) {
            const int y = DbToY(ui[c].disp, barTop, barBot);
            StretchBlt(dc, l, y, bw, barBot - y, gdc, 0, y - barTop, 1, barBot - y, SRCCOPY);
        }
        if (act[c] && ui[c].hold > kFloorDb) {
            const int hy = DbToY(ui[c].hold, barTop, barBot);
            FillRectC(dc, l, hy, r, hy + S(p, 2) > barBot ? barBot : hy + S(p, 2), g_th.hold);
        }
        if (c % every == 0) {
            wchar_t t[8]; swprintf(t, 8, L"%ld", c + 1);
            RECT tr = {l - S(p, 8), labelY, r + S(p, 8), labelY + S(p, 14)};
            TextC(dc, p->fontSmall, act[c] ? g_th.text : g_th.dim, t, tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
    }
    (void)barH;
    wchar_t g[48]; swprintf(g, 48, L"%ls (%ld)", name, n);
    RECT gr = {x0, labelY + S(p, 14), x1, bottom};
    TextC(dc, p->fontSmall, g_th.dim, g, gr, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

// Window procedure of the meter area: double-buffered painting of the dB scale and both bar groups; a click clears
// the clip indicators and holds.
static LRESULT CALLBACK MeterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PanelState* p = (PanelState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:                                   // click = clear clip indicators and peak hold
        if (p && p->drv) { p->drv->resetCounters(WB_RESET_CLIPS); p->snap.clipIn = p->snap.clipOut = 0; ClearHolds(p); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        if (p && p->meterBg && p->meterBg->pixels()) VisBlit(mem, *p->meterBg, rc);   // 0.8.1: monitor window option
        else FillRectC(mem, 0, 0, rc.right, rc.bottom, g_th.card);
        if (p) {
            // A single scale on the left; inputs and outputs side by side with the same vertical scale
            const int scaleW = S(p, 34), barTop = S(p, 12), labelY = rc.bottom - S(p, 30), barBot = labelY - S(p, 4);
            EnsureGradient(p, barBot - barTop);
            HDC gdc = CreateCompatibleDC(mem);
            HGDIOBJ og = p->grad ? SelectObject(gdc, p->grad) : nullptr;
            static const int marks[] = {0, -6, -12, -24, -48};
            int lastY = -10000;
            const int textH = S(p, 12);
            for (int db : marks) {
                const int y = DbToY((float)db, barTop, barBot);
                FillRectC(mem, scaleW, y, rc.right, y + 1, g_th.guide);                // guide line
                if (y - lastY < textH) continue;                                        // do not overlap labels
                wchar_t t[8]; swprintf(t, 8, L"%d", db);
                RECT tr = {0, y - textH / 2 - 1, scaleW - S(p, 6), y + textH / 2 + 1};
                TextC(mem, p->fontSmall, g_th.dim, t, tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                lastY = y;
            }
            const long nI = p->snap.nIn, nO = p->snap.nOut;
            const int x0 = scaleW + S(p, 6), x1 = rc.right - S(p, 4), sep = S(p, 18);
            const int total = x1 - x0 - (nI > 0 ? sep : 0);
            int wIn = nI > 0 ? (int)((double)total * nI / (double)(nI + (nO > 0 ? nO : 1))) : 0;
            if (nI > 0 && wIn < total / 4) wIn = total / 4;
            if (nI > 0 && wIn > total * 3 / 4) wIn = total * 3 / 4;
            if (nI > 0) {
                DrawBars(p, mem, gdc, x0, x0 + wIn, barTop, barBot, labelY, rc.bottom, T(S_INPUTS), nI, p->mIn, p->snap.inAct, p->snap.clipIn);
                FillRectC(mem, x0 + wIn + sep / 2, barTop, x0 + wIn + sep / 2 + 1, barBot, g_th.line);
            }
            const int ox = nI > 0 ? x0 + wIn + sep : x0;
            DrawBars(p, mem, gdc, ox, x1, barTop, barBot, labelY, rc.bottom, T(S_OUTPUTS), nO, p->mOut, p->snap.outAct, p->snap.clipOut);
            if (og) SelectObject(gdc, og);
            DeleteDC(gdc);
            if (!p->snap.running) {
                RECT t = {0, 0, rc.right - S(p, 6), S(p, 14)};
                TextC(mem, p->fontSmall, g_th.dim, T(S_STOPPED), t, DT_RIGHT | DT_TOP | DT_SINGLELINE);
            }
            if (p->snap.limOn) {                       // 0.8.0 (G2): gain reduction of the hearing-safety limiter
                wchar_t gt[48]; swprintf(gt, 48, T(S_LIM_GR), p->grDisp >= 0.05f ? L"-" : L"", p->grDisp);
                RECT t = {0, 0, rc.right - S(p, 64), S(p, 14)};
                TextC(mem, p->fontSmall, p->grDisp >= 0.05f ? g_th.amber : g_th.dim, gt, t, DT_RIGHT | DT_TOP | DT_SINGLELINE);
            }
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}


// ---------------------------------------------------------------- dialogs (dark, no resources)
// Shared by the dialogs: card background, control colors, owner-drawn buttons and toggles.
static bool DlgCommon(HWND h, UINT msg, WPARAM wp, LPARAM lp, LRESULT& res, int dpi) {
    switch (msg) {
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc); FillRect((HDC)wp, &rc, g_th.cardBr); res = 1; return true; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: res = CtlColor(msg, wp, lp); return true;
    case WM_DRAWITEM: DrawOwnerButton((const DRAWITEMSTRUCT*)lp, dpi); res = TRUE; return true;
    case WM_COMMAND:
        if ((HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED) && IsToggle((HWND)lp)) {
            HWND c = (HWND)lp;
            SetToggle(c, GetWindowLongPtrW(c, GWLP_USERDATA) == 0);
            res = 0; return true;
        }
        break;
    }
    return false;
}

// Creates a dark-themed control inside a dialog (logical coordinates).
static HWND DlgCtl(HWND parent, PanelState* p, const wchar_t* cls, const wchar_t* t, DWORD sty, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | sty, S(p, x), S(p, y), S(p, w), S(p, h), parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
    if (!c) return nullptr;
    SendMessageW(c, WM_SETFONT, (WPARAM)p->font, TRUE);
    if (!_wcsicmp(cls, L"COMBOBOX")) DarkCtl(c, L"DarkMode_CFD");
    else if (!_wcsicmp(cls, L"EDIT")) DarkCtl(c, L"DarkMode_Explorer");
    return c;
}

// Shared modal loop: the panel is disabled while the dialog is open; WM_QUIT is re-posted to the panel loop.
// 0.7.1: the dialog is registered in p->dlg so the panel's 250 ms timer keeps it above the host's windows
// (KeepWithHost), and the loop also ends if the dialog was destroyed from outside (e.g. together with the panel
// when the host destroyed the panel's owner): never wait on a dead window.
static void DlgLoop(PanelState* p, HWND dlg, bool& done) {
    ++p->modalDepth;                     // fix 7: no rebind while the dialog runs
    wchar_t dcls[32] = L"";              // 0.9.2: help tips of the dialog's controls
    GetClassNameW(dlg, dcls, 32);
    const int tctx = !wcscmp(dcls, L"WasapiBridgeRoute") ? tips::kRouteDlg : tips::kNameDlg;
    TipCancel(p);
    TipAnnotate(dlg, tctx);
    EnableWindow(p->wnd, FALSE);
    p->dlg = dlg; p->dlgTop = false;
    MSG msg = {}; BOOL gm = 1;
    while (!done && IsWindow(dlg) && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (TipKey(p, dlg, tctx, msg)) continue;        // 0.9.2
        if (EnterOnButton(dlg, msg)) continue;          // 0.9.1 (B1): Enter activates the focused toggle or button
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    p->dlg = nullptr;
    TipCancel(p);                        // 0.9.2
    if (!done && IsWindow(dlg)) { if (IsWindow(p->wnd)) EnableWindow(p->wnd, TRUE); DestroyWindow(dlg); }
    if (IsWindow(p->wnd)) EnableWindow(p->wnd, TRUE);
    --p->modalDepth;
    if (gm == 0) PostQuitMessage((int)msg.wParam);
}

// 0.7.1 (A). Topmost policy shared by the panel, its dialogs and the pinned monitor window: a window is
// HWND_TOPMOST only while the foreground window belongs to this process (the DAW or one of our windows), so it
// can never disappear behind the host's windows (Cubase's Studio Setup may live on another thread or be a Qt
// window, which defeats owner-based z-order), and it never covers other applications. SetWindowPos is called only
// when the wanted state changes (`state` remembers the last one). Host thread.
static void ApplyTopmostPolicy(HWND w, bool& state, bool allowed = true) {
    if (!w || !IsWindow(w)) return;
    bool want = false;
    if (allowed) {
        DWORD pid = 0;
        HWND f = GetForegroundWindow();
        if (f) GetWindowThreadProcessId(f, &pid);
        want = f && pid == GetCurrentProcessId();
    }
    if (want == state) return;
    state = want;
    SetWindowPos(w, want ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// 0.7.1 (A). Panel timer (250 ms, host thread): if the owner was destroyed, hidden or minimized, the panel
// detaches from it (GWLP_HWNDPARENT = null, owner re-enabled) and stays visible (restored if needed); then the
// topmost policy is applied to the panel and to an open child dialog.
static void KeepWithHost(PanelState* p) {
    HWND w = p->wnd;
    if (!w || !IsWindow(w)) return;
    if (p->owner && (!IsWindow(p->owner) || !IsWindowVisible(p->owner) || IsIconic(p->owner))) {
        if (IsWindow(p->owner)) EnableWindow(p->owner, TRUE);
        SetWindowLongPtrW(w, GWLP_HWNDPARENT, 0);
        p->owner = nullptr;
        p->topState = false;                 // re-evaluate the z-order with the new (absent) owner
    }
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    else if (!IsWindowVisible(w)) ShowWindow(w, SW_SHOWNA);
    ApplyTopmostPolicy(w, p->topState);
    if (p->dlg) ApplyTopmostPolicy(p->dlg, p->dlgTop);
}

// Creates a modal-style dark dialog window of logical size w x h, centered horizontally over the panel and yOff
// below its top. param is stored for the dialog procedure. Returns the dialog window.
static HWND DlgCreate(PanelState* p, const wchar_t* cls, const wchar_t* title, int w, int h, int yOff, void* param) {
    RECT pr; GetWindowRect(p->wnd, &pr);
    RECT rc = {0, 0, S(p, w), S(p, h)};
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rc, style, FALSE, 0);    // 0.9.3: no WS_EX_DLGMODALFRAME, so the caption shows the icon
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    HWND d = CreateWindowExW(0, cls, title, style, pr.left + ((pr.right - pr.left) - ww) / 2, pr.top + S(p, yOff),
                             ww, wh, p->wnd, nullptr, g_hInst, param);
    if (d) { DarkTitle(d); SetWindowIcons(d, p->dpi); }
    return d;
}

// ---------------------------------------------------------------- output routing (0.5.0)
struct RouteDlg { PanelState* p = nullptr; HWND wnd = nullptr; bool done = false, ok = false, identity = false; int nDev = 0, nAsio = 0; };

// Dialog procedure of the output routing dialog (one combo per device channel; Identity, OK, Cancel).
static LRESULT CALLBACK RouteProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    RouteDlg* d = (RouteDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) { SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); return DefWindowProcW(h, msg, wp, lp); }
    LRESULT res = 0;
    if (d && DlgCommon(h, msg, wp, lp, res, d->p->dpi)) return res;
    switch (msg) {
    case WM_SETCURSOR: if (d) TipOnSetCursor(d->p, h, tips::kRouteDlg, (HWND)wp, HIWORD(lp)); break;   // 0.9.2
    case a11y::kMsgDescription: return TipDescribe(tips::kRouteDlg, h, (HWND)wp, (wchar_t*)lp);
    case WM_COMMAND:
        if (!d) break;
        if (LOWORD(wp) == ID_ROUTEID) {            // identity: device channel N <- ASIO channel N
            for (int i = 0; i < d->nDev; ++i) SendDlgItemMessageW(h, ID_ROUTE0 + i, CB_SETCURSEL, i < d->nAsio ? i + 1 : 0, 0);
            d->identity = true;
            return 0;
        }
        if (LOWORD(wp) >= ID_ROUTE0 && LOWORD(wp) < ID_ROUTE0 + kMaxCh && HIWORD(wp) == CBN_SELCHANGE) { d->identity = false; return 0; }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            if (LOWORD(wp) == IDOK) {
                d->ok = true;
                if (d->identity) d->p->outputMap.clear();
                else {
                    std::wstring m;
                    for (int i = 0; i < d->nDev; ++i) {
                        int sel = (int)SendDlgItemMessageW(h, ID_ROUTE0 + i, CB_GETCURSEL, 0, 0);
                        if (sel <= 0) continue;
                        wchar_t t[24]; swprintf(t, 24, L"%s%d:%d", m.empty() ? L"" : L",", sel, i + 1);
                        m += t;
                    }
                    d->p->outputMap = m.empty() ? L"0:1" : m;   // everything silent: an invalid pair the driver ignores
                }
            }
            d->done = true;
            EnableWindow(d->p->wnd, TRUE);
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE: if (d) { d->done = true; EnableWindow(d->p->wnd, TRUE); } DestroyWindow(h); return 0;
    case WM_DESTROY: AccClearChildren(h); if (d) { TipCancel(d->p); d->done = true; } SetWindowLongPtrW(h, GWLP_USERDATA, 0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Opens the output routing dialog for the pending layout and stores the result in p->outputMap (saved on Apply).
static void EditRouting(PanelState* p) {
    StatusInfo st{};
    p->drv->getStatus(st);
    RouteDlg d; d.p = p;
    // Pending layout (what is selected in the panel, even if not applied yet)
    int pendDev = 2; std::vector<std::wstring> pendNames;
    PendingOutLayout(p, pendDev, pendNames);
    const bool changed = pendDev != (int)st.outDevChannels || (long)pendNames.size() != st.nOut;
    d.nDev = pendDev > kMaxCh ? kMaxCh : pendDev;
    d.nAsio = (int)pendNames.size(); if (d.nAsio > kMaxCh) d.nAsio = kMaxCh;
    int src[kMaxCh];
    for (int i = 0; i < kMaxCh; ++i) src[i] = i < d.nAsio ? i + 1 : 0;
    d.identity = p->outputMap.empty();
    if (!d.identity) {
        for (int i = 0; i < kMaxCh; ++i) src[i] = 0;
        const std::wstring& m = p->outputMap;
        size_t i = 0;
        while (i < m.size()) {
            size_t j = m.find(L',', i); if (j == std::wstring::npos) j = m.size();
            long a = 0, dv = 0;
            if (swscanf(m.substr(i, j - i).c_str(), L" %ld : %ld", &a, &dv) == 2 && a >= 1 && a <= d.nAsio && dv >= 1 && dv <= d.nDev) src[dv - 1] = (int)a;
            i = j + 1;
        }
    }
    const int cols = d.nDev > 16 ? 2 : 1, rows = d.nDev > 16 ? 16 : d.nDev;
    const int noteH = changed ? 22 : 0;
    const int W = 24 + cols * 270, H = 48 + noteH + rows * 32 + 56;
    d.wnd = DlgCreate(p, L"WasapiBridgeRoute", T(S_ROUTE_TITLE), W, H, 40, &d);
    if (!d.wnd) return;
    HWND t0 = DlgCtl(d.wnd, p, L"STATIC", T(S_ROUTE_HDR), SS_NOPREFIX, 14, 14, W - 28, 20, -1);
    (void)t0;
    if (changed) { HWND n = DlgCtl(d.wnd, p, L"STATIC", T(S_ROUTE_NOTE), SS_NOPREFIX, 14, 36, W - 28, 20, -1);
                   if (n) SetPropW(n, L"wbdim", (HANDLE)1); }
    for (int i = 0; i < d.nDev; ++i) {
        const int x = 14 + (i / 16) * 270, y = 44 + noteH + (i % 16) * 32;
        wchar_t t[32]; swprintf(t, 32, T(S_ROUTE_CH), i + 1);
        DlgCtl(d.wnd, p, L"STATIC", t, SS_NOPREFIX, x, y + 5, 70, 20, -1);
        HWND cb = DlgCtl(d.wnd, p, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 72, y, 182, 320, ID_ROUTE0 + i);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)T(S_SILENCE));
        for (int a = 0; a < d.nAsio; ++a) {
            wchar_t n[64]; swprintf(n, 64, L"%d  %ls", a + 1, pendNames[a].c_str());
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)n);
        }
        SendMessageW(cb, CB_SETCURSEL, src[i] <= d.nAsio ? src[i] : 0, 0);
    }
    const int by = H - 46;
    MakeButton(d.wnd, p->font, T(S_IDENTITY), S(p, 14), S(p, by), S(p, 100), S(p, 32), ID_ROUTEID, 1, g_th.card);
    MakeButton(d.wnd, p->fontB, T(S_BTN_OK), S(p, W - 14 - 100 - 8 - 100), S(p, by), S(p, 100), S(p, 32), IDOK, 2, g_th.card);
    MakeButton(d.wnd, p->font, T(S_CANCEL), S(p, W - 14 - 100), S(p, by), S(p, 100), S(p, 32), IDCANCEL, 1, g_th.card);
    ShowWindow(d.wnd, SW_SHOW);
    DlgLoop(p, d.wnd, d.done);
}

// ---------------------------------------------------------------- profile name dialog
struct NameDlg { PanelState* p = nullptr; HWND wnd = nullptr; bool done = false, ok = false, devices = false; std::wstring text; };

// Dialog procedure of the profile name dialog.
static LRESULT CALLBACK NameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    NameDlg* d = (NameDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) { SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); return DefWindowProcW(h, msg, wp, lp); }
    LRESULT res = 0;
    if (d && DlgCommon(h, msg, wp, lp, res, d->p->dpi)) return res;
    switch (msg) {
    case WM_SETCURSOR: if (d) TipOnSetCursor(d->p, h, tips::kNameDlg, (HWND)wp, HIWORD(lp)); break;   // 0.9.2
    case a11y::kMsgDescription: return TipDescribe(tips::kNameDlg, h, (HWND)wp, (wchar_t*)lp);
    case WM_COMMAND:
        if (d && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
            if (LOWORD(wp) == IDOK) { d->text = GetText(h, ID_NAMEEDIT); d->devices = GetChk(h, ID_NAMEDEV); d->ok = true; }
            d->done = true;
            EnableWindow(d->p->wnd, TRUE);           // before destroying: focus returns to the panel
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE: if (d) { d->done = true; EnableWindow(d->p->wnd, TRUE); } DestroyWindow(h); return 0;
    case WM_DESTROY: AccClearChildren(h); if (d) { TipCancel(d->p); d->done = true; } SetWindowLongPtrW(h, GWLP_USERDATA, 0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Asks for a profile name. devices receives the "Include devices" toggle (preset from the existing profile
// named initial; unchecked for a new profile, which then stores engine settings only). Returns false on Cancel.
static bool AskName(PanelState* p, const std::wstring& initial, std::wstring& out, bool& devices) {
    const bool presetDevices = !initial.empty() && ProfileHasDevices(initial);   // keep the kind of the existing profile
    NameDlg d; d.p = p;
    const int W = 380, H = 168;
    d.wnd = DlgCreate(p, L"WasapiBridgeName", T(S_SAVE_PROFILE), W, H, 80, &d);
    if (!d.wnd) return false;
    DlgCtl(d.wnd, p, L"STATIC", T(S_PROFILE_NAME), SS_NOPREFIX, 14, 12, W - 28, 20, -1);
    HWND e = DlgCtl(d.wnd, p, L"EDIT", initial.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 14, 36, W - 28, 26, ID_NAMEEDIT);
    SendMessageW(e, EM_LIMITTEXT, 40, 0);
    MakeButton(d.wnd, p->font, T(S_INCLUDE_DEVICES), S(p, 14), S(p, 74), S(p, W - 28), S(p, 26),
               ID_NAMEDEV, 3, g_th.card);
    SetChk(d.wnd, ID_NAMEDEV, presetDevices);
    MakeButton(d.wnd, p->fontB, T(S_SAVE), S(p, W - 14 - 100 - 8 - 100), S(p, H - 46), S(p, 100), S(p, 32), IDOK, 2, g_th.card);
    MakeButton(d.wnd, p->font, T(S_CANCEL), S(p, W - 14 - 100), S(p, H - 46), S(p, 100), S(p, 32), IDCANCEL, 1, g_th.card);
    ShowWindow(d.wnd, SW_SHOW);
    SetFocus(e); SendMessageW(e, EM_SETSEL, 0, -1);
    DlgLoop(p, d.wnd, d.done);
    if (!d.ok) return false;
    out = d.text; devices = d.devices;
    return true;
}

// Asks for a name and saves the on-screen settings as a profile (engine only unless "Include devices" is on).
// Confirms before replacing an existing profile and warns when that would drop its devices.
static void SaveProfileAs(PanelState* p) {
    std::wstring name; bool devices = false;
    if (!AskName(p, CurProfile(p), name, devices) || p->closed) return;
    if (!ValidProfileName(name)) { PanelMsgBox(p, p->wnd, T(S_PROFILE_INVALID), WB_NAME_W, MB_ICONWARNING); return; }
    for (auto& x : p->profiles) if (!_wcsicmp(x.c_str(), name.c_str())) {
        std::wstring q = FmtW(T(S_PROFILE_EXISTS), x.c_str());
        if (ProfileHasDevices(x) && !devices)
            q = FmtW(T(S_PROFILE_EXISTS_DEV), x.c_str());
        if (PanelMsgBox(p, p->wnd, q.c_str(), WB_NAME_W, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        name = x;
    }
    Config c = p->drv->config();
    ReadControls(p, c);
    SaveProfile(name, c, devices);
    RefreshProfiles(p, name);
}


// ---------------------------------------------------------------- building the UI
static const int kCT = 112;            // logical y where tab content starts
static int ContentBottom(PanelState* p) { return p->ch - 76; }

// Creates every control of every tab at the current DPI (called at creation and on WM_DPICHANGED).
static void BuildControls(PanelState* p) {
    const Config& c = p->drv->config();
    ListDevices(p->drv->enumerator(), eRender, p->outs);
    ListDevices(p->drv->enumerator(), eCapture, p->ins);
    const int CB = ContentBottom(p);

    // ---- Main
    AddCard(p, TAB_MAIN, 16, kCT, 412, 236, T(S_CARD_DEVICES));
    {
        const int x = 16, y = kCT;
        // 0.7.1: wider labels ("Output device" / "Input device"); combos start at x + 120
        Label(p, TAB_MAIN, T(S_PROFILE), x + 16, y + 40, 100, true);
        Combo(p, TAB_MAIN, x + 120, y + 40, 132, ID_PROFILE);
        Btn(p, TAB_MAIN, T(S_SAVE_DOTS), x + 258, y + 39, 84, 28, ID_PSAVE);
        Btn(p, TAB_MAIN, T(S_DELETE), x + 346, y + 39, 52, 28, ID_PDEL);
        Label(p, TAB_MAIN, TM(S_OUTPUT_DEVICE), x + 16, y + 78, 104, true);
        HWND o = Combo(p, TAB_MAIN, x + 120, y + 78, 278, ID_OUT); Add(o, T(S_WIN_DEFAULT));
        for (auto& n : p->outs) Add(o, n.c_str());
        Label(p, TAB_MAIN, TM(S_INPUT_DEVICE), x + 16, y + 116, 104, true);
        // 0.7.1: "(None - no inputs)" first so it is easy to find; then the Windows default; then the devices
        HWND in = Combo(p, TAB_MAIN, x + 120, y + 116, 278, ID_IN); Add(in, T(S_NONE_NO_INPUTS)); Add(in, T(S_WIN_DEFAULT));
        for (auto& n : p->ins) Add(in, n.c_str());
        Label(p, TAB_MAIN, TM(S_MODE), x + 16, y + 154, 100, true);
        HWND m = Combo(p, TAB_MAIN, x + 120, y + 154, 106, ID_MODE); Add(m, T(S_SHARED)); Add(m, T(S_EXCLUSIVE));
        Label(p, TAB_MAIN, TM(S_BUFFER), x + 232, y + 154, 56, true);
        HWND bf = Combo(p, TAB_MAIN, x + 290, y + 154, 108, ID_BUF);   // a non-standard value is added by FillControls
        for (int i = 0; i < 8; ++i) { wchar_t t[16]; swprintf(t, 16, L"%ld", kBufs[i]); Add(bf, t); }
        // 0.7.1: shown only while Windows denies microphone access (UpdatePrivacyRow)
        HWND wl = Ctl(p, TAB_MAIN, L"STATIC", T(S_PRIV_ROW),
                      SS_LEFT | SS_NOPREFIX, x + 16, y + 190, 280, 36, ID_PRIVLBL);
        if (wl) SetPropW(wl, L"wbwarn", (HANDLE)1);
        Btn(p, TAB_MAIN, T(S_SET_NONE), x + 304, y + 192, 94, 28, ID_SETNONE);
        // 0.8.0 (G1): Bluetooth row, same place (UpdateBtRow; hidden while the privacy row is shown)
        HWND bl = Ctl(p, TAB_MAIN, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x + 16, y + 190, 268, 40, ID_BTLBL);
        if (bl) { SetPropW(bl, L"wbwarn", (HANDLE)1); SendMessageW(bl, WM_SETFONT, (WPARAM)p->fontSmall, TRUE); }
        Btn(p, TAB_MAIN, T(S_USE_RECOMMENDED), x + 290, y + 192, 108, 28, ID_BTREC);
    }
    AddCard(p, TAB_MAIN, 440, kCT, 384, 236, T(S_CARD_LIVE));
    p->tilesRc = {S(p, 440), S(p, kCT), S(p, 824), S(p, kCT + 236)};
    // 0.7.0: options strip (monitor window toggle + legend of the RESET marker)
    AddCard(p, TAB_MAIN, 16, kCT + 244, 808, 40, L"");
    Btn(p, TAB_MAIN, T(S_MONWIN), 28, kCT + 250, 230, 28, ID_MONWIN, 3);
    Btn(p, TAB_MAIN, T(S_TRAY), 266, kCT + 250, 130, 28, ID_TRAY, 3);          // 0.8.0 (G5)
    Label(p, TAB_MAIN, T(S_RESET_LEGEND), 408, kCT + 249, 408, true);
    const int mTop = kCT + 292;
    AddCard(p, TAB_MAIN, 16, mTop, 808, CB - mTop, T(S_CARD_LEVELS));
    p->meter = CreateWindowExW(0, L"WasapiBridgeMeter", T(S_CARD_LEVELS), WS_CHILD, S(p, 28), S(p, mTop + 36), S(p, 784), S(p, CB - mTop - 46),
                               p->wnd, (HMENU)(INT_PTR)ID_METER, g_hInst, p);
    if (p->meter) { p->tabCtl[TAB_MAIN].push_back(p->meter); if (p->tab == TAB_MAIN) ShowWindow(p->meter, SW_SHOW); }

    // ---- Advanced
    // 0.9.10: the two top cards are 36 px taller (266) for the "Rate conversion" row (FixedRatioEngine); the cards
    // below moved down by 36 (the host-profile card now ends at kCT + 482 = 594, ContentBottom is 624).
    AddCard(p, TAB_ADV, 16, kCT, 400, 266, T(S_CARD_BEHAVIOR));
    Btn(p, TAB_ADV, TM(S_LOWLAT), 32, kCT + 40, 370, 28, ID_LL, 3);
    Btn(p, TAB_ADV, T(S_FOLLOW), 32, kCT + 74, 370, 28, ID_FOLLOW, 3);
    Btn(p, TAB_ADV, T(S_WRITELOG), 32, kCT + 108, 370, 28, ID_LOG, 3);
    Btn(p, TAB_ADV, T(S_DITHER), 32, kCT + 142, 370, 28, ID_DITHER, 3);
    // 0.8.0 (G1): never open the microphone of the Bluetooth headset used as output (it would leave A2DP)
    Btn(p, TAB_ADV, TM(S_A2DP), 32, kCT + 176, 370, 28, ID_A2DP, 3);
    AddCard(p, TAB_ADV, 428, kCT, 396, 266, T(S_CARD_QUALITY));
    {
        const int x = 428, y = kCT;
        Label(p, TAB_ADV, TM(S_DRIFT), x + 16, y + 40, 150, true);
        HWND d = Combo(p, TAB_ADV, x + 170, y + 40, 210, ID_DRIFT); Add(d, T(S_AUTO)); Add(d, T(S_ALWAYS)); Add(d, T(S_NEVER));
        Label(p, TAB_ADV, TM(S_RESAMPLER), x + 16, y + 76, 150, true);
        HWND rq = Combo(p, TAB_ADV, x + 170, y + 76, 210, ID_RESAMP); Add(rq, T(S_HERMITE_LIGHT)); Add(rq, T(S_SINC_HQ));
        // 0.9.10: FixedRatioEngine; item index = FrEngine value (Auto = Sinc rule, soxr only when chosen, Sinc, Hermite)
        Label(p, TAB_ADV, TM(S_FR_ENGINE), x + 16, y + 112, 150, true);
        HWND fe = Combo(p, TAB_ADV, x + 170, y + 112, 210, ID_FRENG);
        Add(fe, T(S_FRE_AUTO)); Add(fe, T(S_FRE_SOXR)); Add(fe, T(S_FRE_SINC)); Add(fe, T(S_HERMITE_LIGHT));
        Label(p, TAB_ADV, TM(S_INIT_RATE), x + 16, y + 148, 154, true);
        HWND r = Combo(p, TAB_ADV, x + 170, y + 148, 210, ID_RATE);
        for (int i = 0; i < 6; ++i) { wchar_t t[16]; swprintf(t, 16, L"%ld", kRates[i]); Add(r, t); }
        Add(r, T(S_FOLLOW_WIN));             // 0.8.0 (G4): SampleRate=Auto, index 6
        Label(p, TAB_ADV, T(S_GAIN_DB), x + 16, y + 184, 150, true);
        Ctl(p, TAB_ADV, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x + 170, y + 184, 100, 26, ID_GAIN);
        // 0.8.0 (G2): hearing-safety limiter (live): on/off, ceiling (dBFS), optional cap on the Gain key (dB)
        Btn(p, TAB_ADV, T(S_LIMITER), x + 16, y + 220, 92, 28, ID_LIMON, 3);
        Label(p, TAB_ADV, T(S_CEILING), x + 116, y + 222, 50, true);
        Ctl(p, TAB_ADV, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x + 168, y + 220, 52, 26, ID_LIMCEIL);
        Btn(p, TAB_ADV, T(S_GAINCAP), x + 228, y + 220, 92, 28, ID_LIMCAP, 3);
        HWND cap = Ctl(p, TAB_ADV, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x + 326, y + 220, 52, 26, ID_LIMCAPDB);
        AccNamed(cap, std::wstring(T(S_GAINCAP)) + L" (dB)");   // 0.9.1 (B1): it follows a toggle, not a label
    }
    AddCard(p, TAB_ADV, 16, kCT + 274, 540, 124, T(S_CARD_LOOPBACK));
    {
        const int y = kCT + 274;
        Label(p, TAB_ADV, TM(S_CAPTURE), 32, y + 40, 100, true);
        HWND lb = Combo(p, TAB_ADV, 140, y + 40, 380, ID_LB);
        Add(lb, T(S_OFF)); Add(lb, T(S_LB_EXCL)); Add(lb, T(S_LB_ALL));
        Add(lb, T(S_LB_APP));
        Ctl(p, TAB_ADV, L"STATIC", TM(S_APPLICATION), SS_LEFT | SS_NOPREFIX, 32, y + 81, 104, 20, ID_APPLBL);
        Ctl(p, TAB_ADV, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, 140, y + 76, 260, 320, ID_APP);
        Btn(p, TAB_ADV, T(S_REFRESH), 410, y + 75, 110, 28, ID_APPREF);
    }
    // 0.7.0 (F4): latency offsets (live; the host is told with kAsioLatenciesChanged)
    AddCard(p, TAB_ADV, 568, kCT + 274, 256, 124, T(S_CARD_OFFSETS));
    {
        const int x = 568, y = kCT + 274;
        Label(p, TAB_ADV, T(S_OUTPUT), x + 16, y + 38, 90, true);
        Ctl(p, TAB_ADV, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x + 110, y + 38, 80, 26, ID_OFSOUT);
        Label(p, TAB_ADV, T(S_INPUT), x + 16, y + 70, 90, true);
        Ctl(p, TAB_ADV, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x + 110, y + 70, 80, 26, ID_OFSIN);
        HWND hint = Ctl(p, TAB_ADV, L"STATIC", T(S_OFS_HINT), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS,
                        x + 16, y + 98, 232, 20, ID_OFSHINT);
        if (hint) SetPropW(hint, L"wbdim", (HANDLE)1);
    }
    // 0.8.0 (G3): automatic profile per program ([HostProfiles], used when the driver is loaded)
    AddCard(p, TAB_ADV, 16, kCT + 406, 808, 76, T(S_CARD_HOSTPROF));
    {
        const int y = kCT + 406;
        p->hostExe = HostExeName();
        const std::wstring lbl = p->hostExe.empty() ? std::wstring(T(S_THIS_PROGRAM_UNKNOWN)) : p->hostExe;
        HWND hl = Ctl(p, TAB_ADV, L"STATIC", lbl.c_str(), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 32, y + 40, 150, 20, 0);
        if (hl) SetPropW(hl, L"wbdim", (HANDLE)1);
        AccNamed(Combo(p, TAB_ADV, 186, y + 36, 200, ID_HPROF), std::wstring(T(S_CARD_HOSTPROF)) + L": " + lbl);   // 0.9.1 (B1)
        Label(p, TAB_ADV, T(S_ALL_PROGRAMS), 400, y + 40, 90, true);
        Combo(p, TAB_ADV, 492, y + 36, 236, ID_HPLIST);
        Btn(p, TAB_ADV, T(S_REMOVE), 736, y + 35, 76, 28, ID_HPDEL);
    }

    // ---- Devices
    AddCard(p, TAB_DEV, 16, kCT, 808, 200, T(S_CARD_EXTRA));
    {
        const int y = kCT;
        Label(p, TAB_DEV, TM(S_EX_OUTPUTS), 32, y + 28, 380, true);
        Ctl(p, TAB_DEV, L"LISTBOX", L"", LBS_MULTIPLESEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 32, y + 54, 376, 100, ID_EXOUT);
        Label(p, TAB_DEV, TM(S_INPUTS), 432, y + 28, 380, true);
        Ctl(p, TAB_DEV, L"LISTBOX", L"", LBS_MULTIPLESEL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 432, y + 54, 376, 100, ID_EXIN);
        Btn(p, TAB_DEV, TM(S_ALIGN), 32, y + 162, 376, 28, ID_ALIGN, 3);
        Btn(p, TAB_DEV, T(S_ROUTING_DOTS), 432, y + 161, 160, 30, ID_ROUTE);
        Label(p, TAB_DEV, T(S_UPTO3), 604, y + 163, 200, true);
    }
    // 0.7.0 (F5): direct monitoring inside the driver (live)
    AddCard(p, TAB_DEV, 16, kCT + 212, 808, 150, T(S_CARD_DM));
    {
        const int y = kCT + 212;
        Btn(p, TAB_DEV, T(S_ENABLED), 32, y + 36, 120, 28, ID_DMON, 3);
        Label(p, TAB_DEV, T(S_INPUTS), 160, y + 36, 100, true);
        Combo(p, TAB_DEV, 262, y + 36, 130, ID_DMIN);
        Label(p, TAB_DEV, T(S_OUTPUTS), 404, y + 36, 110, true);
        Combo(p, TAB_DEV, 516, y + 36, 100, ID_DMOUT);
        Btn(p, TAB_DEV, T(S_MUTE), 626, y + 36, 100, 28, ID_DMMUTE, 3);
        Label(p, TAB_DEV, T(S_LEVEL_DB), 32, y + 74, 80, true);
        Ctl(p, TAB_DEV, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 116, y + 74, 70, 26, ID_DMLVL);
        Label(p, TAB_DEV, T(S_PAN), 202, y + 74, 110, true);
        Ctl(p, TAB_DEV, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 314, y + 74, 70, 26, ID_DMPAN);
        Label(p, TAB_DEV, T(S_DM_NOLB), 400, y + 74, 410, true);
        HWND n = Ctl(p, TAB_DEV, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 32, y + 113, 776, 20, ID_DMNOTE);
        if (n) SetPropW(n, L"wbdim", (HANDLE)1);
    }
    if (CB - (kCT + 374) >= 60) {
        AddCard(p, TAB_DEV, 16, kCT + 374, 808, CB - (kCT + 374), T(S_CARD_PERDEV));
        HWND e = Ctl(p, TAB_DEV, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 28, kCT + 410, 784, CB - (kCT + 410) - 12, ID_EXSTATUS);
        if (e) SendMessageW(e, WM_SETFONT, (WPARAM)p->mono, TRUE);
        AccNamed(e, T(S_CARD_PERDEV));                  // 0.9.1 (B1): no label right before it
    }

    // ---- Status
    AddCard(p, TAB_STATE, 16, kCT, 808, CB - kCT, T(S_CARD_FULLSTATUS));
    {
        HWND e = Ctl(p, TAB_STATE, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 28, kCT + 36, 784, CB - kCT - 36 - 58, ID_STATUS);
        if (e) SendMessageW(e, WM_SETFONT, (WPARAM)p->mono, TRUE);
        AccNamed(e, T(S_CARD_FULLSTATUS));              // 0.9.1 (B1)
        const int by = CB - 48;
        Btn(p, TAB_STATE, T(S_COPY_STATUS), 28, by, 130, 32, ID_COPY);
        Btn(p, TAB_STATE, T(S_OPEN_LOG), 166, by, 110, 32, ID_OPENLOG);
        Btn(p, TAB_STATE, T(S_SETTINGS_FOLDER), 284, by, 190, 32, ID_OPENDIR);
        Btn(p, TAB_STATE, T(S_RESET_COUNTERS), 482, by, 170, 32, ID_RESETCNT);
    }

    // ---- About (painted in PaintPanel)
    AddCard(p, TAB_ABOUT, 16, kCT, 808, CB - kCT, L"");
    // ---- Interface (0.9.0): theme and language (H1, H4), visuals (0.8.1), updates (H3)
    AddCard(p, TAB_UI, 16, kCT, 808, 150, T(S_CARD_APPEARANCE));   // 0.9.2: + Show help tips
    {
        const int y = kCT;
        Label(p, TAB_UI, T(S_THEME), 32, y + 40, 110, true);
        HWND th = Combo(p, TAB_UI, 146, y + 40, 230, ID_THEME);
        const int thIds[thm::kCount] = {S_TH_DARK, S_TH_RETROBLUE, S_TH_RETRO8, S_TH_TERMINAL, S_TH_NEON, S_TH_LIGHT};
        for (int i = 0; i < thm::kCount; ++i) Add(th, T(thIds[i]));
        Label(p, TAB_UI, T(S_LANGUAGE), 400, y + 40, 110, true);
        HWND lg = Combo(p, TAB_UI, 514, y + 40, 294, ID_LANG);
        Add(lg, T(S_LANG_AUTO));
        for (int i = 0; i < kLangCount; ++i) Add(lg, kNative[i]);
        Btn(p, TAB_UI, T(S_HELP_TIPS), 32, y + 74, 400, 28, ID_HELPTIPS, 3);      // 0.9.2 (LIVE)
        HWND n = Ctl(p, TAB_UI, L"STATIC", g_th.hc ? T(S_HC_NOTE) : T(S_APPEAR_NOTE), SS_LEFT | SS_NOPREFIX, 32, y + 110, 776, 34, ID_APPNOTE);
        if (n) { SetPropW(n, L"wbdim", (HANDLE)1); SendMessageW(n, WM_SETFONT, (WPARAM)p->fontSmall, TRUE); }
    }
    AddCard(p, TAB_UI, 16, kCT + 158, 808, 112, T(S_CARD_VISUALS));
    {
        const int y = kCT + 158;
        Label(p, TAB_UI, T(S_VISUALS), 32, y + 38, 110, true);
        HWND v = Combo(p, TAB_UI, 146, y + 38, 230, ID_VIS);
        const int fx[] = {S_OFF, S_FX_PLASMA, S_FX_STARFIELD, S_FX_COPPER, S_FX_TUNNEL, S_FX_REACTIVE, S_FX_METABALLS, S_FX_FIRE,
                          S_FX_VECTOR, S_FX_TWISTER};
        for (int id : fx) Add(v, T(id));
        Label(p, TAB_UI, T(S_INTENSITY), 400, y + 38, 110, true);
        Ctl(p, TAB_UI, L"WasapiBridgeSlider", T(S_INTENSITY), WS_TABSTOP, 514, y + 38, 294, 26, ID_VISINT);   // text = accessible name
        Btn(p, TAB_UI, T(S_IN_MONWIN), 32, y + 72, 344, 28, ID_VISMON, 3);
        Label(p, TAB_UI, T(S_SPEED), 400, y + 72, 110, true);                     // 0.8.2: 25..400 %
        Ctl(p, TAB_UI, L"WasapiBridgeSlider", T(S_SPEED), WS_TABSTOP, 514, y + 72, 294, 26, ID_VISSPD);
    }
    AddCard(p, TAB_UI, 16, kCT + 278, 808, CB - (kCT + 278), T(S_CARD_UPDATES));
    {
        const int y = kCT + 278;
        const bool conf = !upd::IsPlaceholderRepo(UpdateRepoOf(c));
        HWND b = Btn(p, TAB_UI, conf ? T(S_UPD_CHECK) : T(S_UPD_NOTCONF), 32, y + 38, 300, 30, ID_UPDCHECK);
        if (b && !conf) EnableWindow(b, FALSE);
        Btn(p, TAB_UI, T(S_UPD_WEEKLY), 344, y + 39, 464, 28, ID_UPDWEEKLY, 3);
        HWND st = Ctl(p, TAB_UI, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 32, y + 76, 776, 36, ID_UPDSTATUS);
        (void)st;
        Btn(p, TAB_UI, T(S_UPD_DOWNLOAD), 32, y + 116, 220, 30, ID_UPDDL);
        Btn(p, TAB_UI, T(S_UPD_INSTALL), 264, y + 116, 300, 30, ID_UPDRUN);
        HWND n = Ctl(p, TAB_UI, L"STATIC", T(S_UPD_PRIVACY), SS_LEFT | SS_NOPREFIX, 32, y + 154, 776, 36, ID_UPDNOTE);
        if (n) { SetPropW(n, L"wbdim", (HANDLE)1); SendMessageW(n, WM_SETFONT, (WPARAM)p->fontSmall, TRUE); }
        HWND l = Ctl(p, TAB_UI, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 32, y + 194, 776, 20, ID_UPDLAST);
        if (l) SetPropW(l, L"wbdim", (HANDLE)1);
    }

    // ---- Bottom bar (always visible). 0.7.0: "Apply" kept the panel open; "Close" discards unapplied changes.
    // 0.9.11: "Apply and Close" (170 px for the longest translation) saves, applies and closes the panel.
    Btn(p, TAB_ALL, T(S_CLOSE), p->cw - 16 - 110, p->ch - 52, 110, 36, IDCANCEL, 1, true);
    Btn(p, TAB_ALL, T(S_APPLY), p->cw - 16 - 110 - 12 - 170, p->ch - 52, 170, 36, IDOK, 2, true);
    if (p->drv->safeMode()) Btn(p, TAB_ALL, T(S_USE_MY_SETTINGS), p->cw - 16 - 292 - 12 - 200, p->ch - 52, 200, 36, ID_SAFEBTN, 1, true);

    AssignMnemonics(p);                    // 0.9.1 (B1): Alt+letter for the tabs, Apply and Close
    RefreshProfiles(p, c.activeProfile);
    FillControls(p, c);
    FillHostProfiles(p, nullptr);
    LayoutAudit(p);                        // 0.9.0 (H4): no clipped text in any language
    TipAnnotate(p->wnd, tips::kPanel);     // 0.9.2: help tips as accessible descriptions
    VisApply(p);                           // 0.8.1
    UpdateStatus(p);
    UpdateMeters(p);
    UpdatePrivacyRow(p);
}

// ---------------------------------------------------------------- panel painting
static void DrawTile(PanelState* p, HDC dc, RECT r, const wchar_t* title, const wchar_t* value, COLORREF vc, const wchar_t* sub,
                     double bar = -1.0, double barMax = -1.0, COLORREF barC = 0) {
    RoundRectC(dc, r.left, r.top, r.right, r.bottom, S(p, 10), g_th.tile, g_th.tile);
    RECT t = {r.left + S(p, 12), r.top + S(p, 8), r.right - S(p, 8), r.top + S(p, 24)};
    TextC(dc, p->fontSmall, g_th.dim, title, t, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT v = {r.left + S(p, 12), r.top + S(p, 22), r.right - S(p, 8), r.top + S(p, 54)};
    TextC(dc, p->big, vc, value, v, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int subTop = r.bottom - S(p, 22);
    if (bar >= 0) {                                    // load bar with a maximum marker
        const int bl = r.left + S(p, 12), br = r.right - S(p, 12), bt = r.bottom - S(p, 30), bb = bt + S(p, 6);
        RoundRectC(dc, bl, bt, br, bb, S(p, 6), g_th.input, g_th.input);
        double f = bar / 100.0; if (f > 1) f = 1; if (f < 0) f = 0;
        if (f > 0) RoundRectC(dc, bl, bt, bl + (int)((br - bl) * f), bb, S(p, 6), barC, barC);
        if (barMax >= 0) {
            double fm = barMax / 100.0; if (fm > 1) fm = 1;
            const int mx = bl + (int)((br - bl) * fm);
            FillRectC(dc, mx - 1, bt - S(p, 2), mx + 1, bb + S(p, 2), g_th.text);
        }
        subTop = bb + S(p, 2);
    }
    RECT s = {r.left + S(p, 12), subTop, r.right - S(p, 8), r.bottom - S(p, 4)};
    TextC(dc, p->fontSmall, g_th.dim, sub, s, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// Paints the four live tiles of the Main tab (output latency, input latency, DSP load bar, output glitches).
static void DrawTiles(PanelState* p, HDC dc) {
    const RECT c = p->tilesRc;
    const int pad = S(p, 12), gap = S(p, 10), top = c.top + S(p, 36);
    const int tw = (c.right - c.left - 2 * pad - gap) / 2, th = (c.bottom - pad - top - gap) / 2;
    RECT r[4];
    for (int i = 0; i < 4; ++i) {
        const int x = c.left + pad + (i % 2) * (tw + gap), y = top + (i / 2) * (th + gap);
        r[i] = {x, y, x + tw, y + th};
        p->tileRc[i] = r[i];                 // 0.9.2: hover tips of the tiles
    }
    const StatusInfo& s = p->st;
    wchar_t v[64], sub[160];
    // 0.8.0: badges in the card header (right-aligned): Bluetooth, limiter, rate follows Windows, program profile
    if (p->stValid) {
        int bx = c.right - pad;
        auto badge = [&](const wchar_t* t, COLORREF col) {
            SIZE sz = {}; HGDIOBJ of = SelectObject(dc, p->fontSmall); GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz); SelectObject(dc, of);
            const int w = sz.cx + S(p, 14), y0 = c.top + S(p, 10), y1 = y0 + S(p, 18);
            if (bx - w < c.left + S(p, 60)) return;   // no room left next to the card title
            RoundRectC(dc, bx - w, y0, bx, y1, S(p, 9), g_th.tile, col);
            RECT tr = {bx - w, y0, bx, y1};
            TextC(dc, p->fontSmall, col, t, tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            bx -= w + S(p, 6);
        };
        if (s.outBluetooth) badge(T(S_BADGE_BT), g_th.amber);
        if (s.limiterOn) badge(T(S_BADGE_LIM), g_th.accent);
        if (s.rateAuto) badge(T(S_BADGE_RATE), g_th.dim);
        if (!s.hostProfile.empty()) badge(T(S_BADGE_PROF), g_th.dim);
    }
    if (!p->stValid || !s.streamsOpen) {
        DrawTile(p, dc, r[0], T(S_TILE_OUTLAT), L"\x2014", g_th.dim, T(S_TILE_NOSTREAMS));
        DrawTile(p, dc, r[1], T(S_TILE_INLAT), L"\x2014", g_th.dim, T(S_TILE_NOBUFS));
        DrawTile(p, dc, r[2], T(S_TILE_DSP), L"\x2014", g_th.dim, L"", 0.0, -1.0, g_th.accent);
        DrawTile(p, dc, r[3], T(S_TILE_GLITCH), L"\x2014", g_th.dim, L"");
        return;
    }
    swprintf(v, 64, L"%.1f ms", 1000.0 * s.outLat / s.rate);
    swprintf(sub, 160, T(S_TILE_BUFAT), s.bufSize, s.rate);
    DrawTile(p, dc, r[0], T(S_TILE_OUTLAT), v, g_th.text, sub);
    swprintf(v, 64, L"%.1f ms", 1000.0 * s.inLat / s.rate);
    swprintf(sub, 160, L"%ls", s.hasInput ? s.inName.c_str() : T(S_TILE_NODEVIN));
    DrawTile(p, dc, r[1], T(S_TILE_INLAT), v, g_th.text, sub);
    const COLORREF lc = s.loadNow > 100.f ? g_th.red : s.loadNow > 80.f ? g_th.amber : g_th.accent;
    const COLORREF mc = s.loadMax > 100.f ? g_th.red : s.loadMax > 80.f ? g_th.amber : g_th.dim;
    swprintf(v, 64, L"%.0f %%", s.loadNow);
    swprintf(sub, 160, T(S_TILE_MAX), s.loadMax);
    DrawTile(p, dc, r[2], T(S_TILE_DSP), v, lc == g_th.accent ? g_th.text : lc, sub, s.loadNow, s.loadMax, lc);
    (void)mc;
    swprintf(v, 64, L"%u", s.outGlitches);
    swprintf(sub, 160, T(S_TILE_TIMEOUTS), s.timeouts, s.inUnderruns);
    DrawTile(p, dc, r[3], T(S_TILE_GLITCH), v, s.outGlitches > 0 ? g_th.red : g_th.text, sub);
}

// Paints the About tab: version, description, Steinberg trademark and logo notice, license summary.
static void DrawAbout(PanelState* p, HDC dc, const RECT& card) {
    const int x = card.left + S(p, 28);
    int y = card.top + S(p, 28);
    if (p->logoL) DrawLogo(dc, p->logoL, p->logoLW, p->logoLH, card.right - S(p, 28) - p->logoLW, card.top + S(p, 28), g_th.card);
    {   // 0.9.3: the application icon, centered under the ASIO Compatible logo with 24 px of clear space (the
        // logo keeps its own area; nothing touches or overlaps it)
        const int is = S(p, 64), colW = p->logoL ? p->logoLW : is;
        const int ix = card.right - S(p, 28) - colW / 2 - is / 2;
        const int iy = card.top + S(p, 28) + (p->logoL ? p->logoLH + S(p, 24) : 0);
        DrawIconEx(dc, ix, iy, AppIcon(is), is, is, 0, nullptr, DI_NORMAL);
    }
    const int textR = card.right - S(p, 40) - (p->logoLW > S(p, 64) ? p->logoLW : S(p, 64));
    // 0.9.0: each block takes the height it needs in the current language (DT_CALCRECT), at least `h`.
    auto line = [&](HFONT f, COLORREF c, const wchar_t* t, int h) {
        RECT r = {x, y, textR, y + S(p, h)};
        RECT m = r;
        HGDIOBJ of = SelectObject(dc, f);
        DrawTextW(dc, t, -1, &m, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
        SelectObject(dc, of);
        if (m.bottom > r.bottom) r.bottom = m.bottom;
        TextC(dc, f, c, t, r, DT_LEFT | DT_TOP | DT_WORDBREAK);
        y = r.bottom + S(p, 2);
    };
    line(p->title, g_th.text, WB_NAME_W L"\x00AE", 34);
    line(p->fontB, g_th.accent, FmtW(T(S_ABOUT_VERSION), WB_VERSION_W).c_str(), 26);
    line(p->fontSmall, g_th.dim, T(S_ABOUT_BETA), 16);     // 0.9.1 (B5): what is stable and what is still beta
    line(p->font, g_th.text, T(S_ABOUT_DESC), 40);
    y += S(p, 4);
    // The trademark sentence stays in English (Steinberg guidelines); a translated explanation follows it.
    line(p->fontB, g_th.text, L"ASIO is a registered trademark of Steinberg Media Technologies GmbH.", 22);
    if (Current() != kEn) line(p->fontSmall, g_th.dim, T(S_ABOUT_TM_NOTE), 18);
    line(p->font, g_th.dim, T(S_ABOUT_NOTAFFIL), 56);
    y += S(p, 4);
    line(p->fontB, g_th.text, T(S_ABOUT_LICENSE_H), 22);
    line(p->fontSmall, g_th.dim, T(S_ABOUT_LICENSE), 70);
    line(p->fontSmall, g_th.dim, FmtW(T(S_ABOUT_SETTINGS), L"%APPDATA%\\" WB_NAME_W).c_str(), 18);
}

// ---------------------------------------------------------------- panel visuals (0.8.1)
// Animated background drawn by the portable software renderer of src/visuals.h into a low-resolution buffer
// (client size / d, d >= 3 chosen so the buffer is at most 320 x 240) and stretched into the back buffer with
// COLORONCOLOR (nearest neighbor: chunky pixels, no HALFTONE cost) BEFORE the header, the cards and the text are
// drawn. Cards stay opaque, so text contrast is unchanged; the effect shows in the header, the margins, the gaps
// between cards and in a scroller strip on the About tab.
// Cost control (this is the DAW's UI thread; 0.8.2): a dedicated 15 ms timer (~60 fps); each frame renders, then
// invalidates only the background (client minus the inner part of every visible card, plus the About strip), and
// WM_PAINT clips the back buffer to that region. Only the background work (render + stretch) is timed, with
// QueryPerformanceCounter, averaged over 2 s windows: above 6 ms the timer goes to 31 ms (~30 fps), below 3 ms
// back to 15 ms. The animation is time-based (renderer phase = elapsed time x speed), so a lower frame rate never
// slows the movement down. Paused only while the panel is minimized or hidden; a static frame when Windows
// "Show animations" is off; Off in a remote desktop session unless the user chose an
// effect (Visuals key present). The audio-reactive mix uses the meter peaks the panel already reads (readMeters,
// lock-free); nothing here touches the audio thread.
static bool VisAnimationsAllowed() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) on = TRUE;
    return on != FALSE;
}
// Effect to use for a configuration: Off in a remote session unless chosen explicitly.
// sel: the Visuals setting (0 Off, 1 Plasma .. 5 Reactive, 6 Metaballs, 7 Fire, 8 Vector, 9 Twister); returns the
// renderer effect (vis::Effect; the scroller of the About tab sits between them in that enum).
static int VisualsEffective(int sel, bool explicitChoice) {
    if (sel <= 0 || sel > 9) return vis::kOff;
    if (g_th.hc) return vis::kOff;                       // 0.9.1 (B1): no visuals in high contrast
    if (GetSystemMetrics(SM_REMOTESESSION) && !explicitChoice) return vis::kOff;
    return sel <= 5 ? sel : vis::kMetaballs + (sel - 6);
}
static uint32_t ToRgb(COLORREF c) { return (uint32_t)GetRValue(c) << 16 | (uint32_t)GetGValue(c) << 8 | GetBValue(c); }
// Stretches the renderer's pixels into rectangle r of dc (nearest neighbor).
static void VisBlit(HDC dc, const vis::Renderer& r, const RECT& dst) {
    if (!r.pixels() || dst.right <= dst.left || dst.bottom <= dst.top) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = r.width(); bi.bmiHeader.biHeight = -r.height();
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    const int old = SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top, 0, 0, r.width(), r.height(),
                  r.pixels(), &bi, DIB_RGB_COLORS, SRCCOPY);
    SetStretchBltMode(dc, old);
}
// 0.9.0: scroller text in the current language, folded to the dot font (accents removed, uppercase ASCII).
static std::string ScrollerText() {
    const std::wstring w = FmtW(T(S_SCROLLER), WB_NAME_W, WB_VERSION_W);
    static const wchar_t* const from = L"\u00E1\u00E0\u00E2\u00E3\u00E4\u00E9\u00E8\u00EA\u00EB\u00ED\u00EC\u00EE\u00EF\u00F3\u00F2\u00F4\u00F5\u00F6\u00FA\u00F9\u00FB\u00FC\u00E7\u00F1\u00DF";
    static const char to[] = "aaaaaeeeeiiiiooooouuuucns";
    std::string r;
    for (wchar_t ch : w) {
        if (ch < 128) { r += (char)ch; continue; }
        const wchar_t lc = (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) ? (wchar_t)(ch + 32) : ch;
        const wchar_t* f = wcschr(from, lc);
        r += f ? to[f - from] : ' ';
    }
    return r;
}
// Low-resolution size for a w x h area: divisor >= minDiv so that the result fits 320 x 240 (square pixels).
static void VisSizeFor(int w, int h, int minDiv, int& lw, int& lh) {
    int d = minDiv;
    while ((w + d - 1) / d > vis::kMaxW || (h + d - 1) / d > vis::kMaxH) ++d;
    lw = (w + d - 1) / d; lh = (h + d - 1) / d;
}
// Recomputes buffer sizes (creation, WM_SIZE, WM_DPICHANGED) and the About strip rectangle.
static void VisResize(PanelState* p) {
    RECT rc; GetClientRect(p->wnd, &rc);
    int lw, lh;
    if (p->vr) { VisSizeFor(rc.right, rc.bottom, 3, lw, lh); p->vr->resize(lw, lh); }
    const int CB = p->ch - 76;               // ContentBottom
    p->aboutStrip = {S(p, 32), S(p, CB - 84), S(p, p->cw - 32), S(p, CB - 16)};   // 0.9.0: bottom of the About card
    if (p->vrAbout) {
        const int sw = p->aboutStrip.right - p->aboutStrip.left;
        VisSizeFor(sw, p->aboutStrip.bottom - p->aboutStrip.top, 2, lw, lh);
        p->vrAbout->resize(lw, lh);
        // 0.8.2: about 120 px/s at 100 % display scaling (dpi / 96), whatever low-resolution divisor was chosen
        p->vrAbout->setScrollRate(lw > 0 ? (int)(120.0 * p->dpi / 96.0 * lw / (sw > 0 ? sw : 1) + 0.5) : 40);
    }
}
// Invalidates only what the effect covers: the client area minus the inner part of the visible cards (their
// rounded corners and edges are repainted with it), plus the About strip.
static void VisInvalidate(PanelState* p) {
    RECT rc; GetClientRect(p->wnd, &rc);
    HRGN rgn = CreateRectRgn(0, 0, rc.right, rc.bottom);
    if (!rgn) return;
    const int in = S(p, 8);
    for (const Card& c : p->cards) {
        if (c.tab != p->tab) continue;
        HRGN cr = CreateRectRgn(c.r.left + in, c.r.top + in, c.r.right - in, c.r.bottom - in);
        if (cr) { CombineRgn(rgn, rgn, cr, RGN_DIFF); DeleteObject(cr); }
    }
    if (p->tab == TAB_ABOUT && p->vrAbout) {
        HRGN sr = CreateRectRgnIndirect(&p->aboutStrip);
        if (sr) { CombineRgn(rgn, rgn, sr, RGN_OR); DeleteObject(sr); }
    }
    InvalidateRgn(p->wnd, rgn, FALSE);       // children are not invalidated (no RDW_ALLCHILDREN), WS_CLIPCHILDREN
    DeleteObject(rgn);
}
// 0.8.2: a microsecond clock from QueryPerformanceCounter (GetTickCount advances in 15.6 ms steps, which made
// the animation jerky and the frame-cost measurement meaningless).
static uint64_t VisNowUs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}
// Speed slider position (0..100, logarithmic) <-> percent (25..400; 50 = 100 %).
static int VisSpeedFromPos(int v) { return (int)std::lround(25.0 * std::pow(16.0, (v < 0 ? 0 : v > 100 ? 100 : v) / 100.0)); }
static int VisPosFromSpeed(int sp) { sp = sp < 25 ? 25 : sp > 400 ? 400 : sp; return (int)std::lround(100.0 * std::log(sp / 25.0) / std::log(16.0)); }
// Renders one frame of each active renderer and adds the cost to the current 2 s window.
static void VisRender(PanelState* p) {
    const uint64_t a = VisNowUs();
    const uint64_t t = a - p->visT0Us;       // real elapsed time: the renderer is time-based
    if (p->vr) p->vr->frame((vis::Effect)p->visEffect, t, p->visIntensity, p->visLevel, p->visSpeed);
    if (p->vrAbout && p->tab == TAB_ABOUT) p->vrAbout->frame(vis::kScroller, t, p->visIntensity < 25 ? 25 : p->visIntensity, p->visLevel, p->visSpeed);
    p->visWinMs += (double)(VisNowUs() - a) / 1000.0;
}
// (Re)starts or stops the effect from the on-screen choice (preview; Apply saves it).
static void VisApply(PanelState* p) {
    const int sel = Cur(p->wnd, ID_VIS);
    const Config& c = p->drv->config();
    p->visEffect = VisualsEffective(sel < 0 ? c.visuals : sel, c.visualsExplicit || p->visTouched);
    HWND sl = GetDlgItem(p->wnd, ID_VISINT), ss = GetDlgItem(p->wnd, ID_VISSPD);
    p->visIntensity = sl ? (int)GetWindowLongPtrW(sl, GWLP_USERDATA) : c.visualsIntensity;
    p->visSpeed = ss ? VisSpeedFromPos((int)GetWindowLongPtrW(ss, GWLP_USERDATA)) : c.visualsSpeed;
    const bool on = p->visEffect != vis::kOff;
    if (on && !p->vr) {
        p->vr.reset(new vis::Renderer); p->vr->setBackground(ToRgb(g_th.bg));
        p->vrAbout.reset(new vis::Renderer); p->vrAbout->setBackground(ToRgb(g_th.card));
        p->visT0Us = VisNowUs();
    } else if (!on && p->vr) { p->vr.reset(); p->vrAbout.reset(); }
    if (p->vr) {                             // 0.9.0 (H1): background color and effect palette follow the theme
        p->vr->setBackground(ToRgb(g_th.bg)); p->vr->setPalette(g_th.fx[0], g_th.fx[1], g_th.fx[2]);
        p->vrAbout->setBackground(ToRgb(g_th.card)); p->vrAbout->setPalette(g_th.fx[0], g_th.fx[1], g_th.fx[2]);
        p->vrAbout->setText(ScrollerText().c_str());
    }
    VisResize(p);
    const bool animate = on && VisAnimationsAllowed();
    if (animate) {
        if (!p->visAnimate) { p->visWinMs = 0.0; p->visWinFrames = 0; p->visWinStartUs = VisNowUs(); p->visFps = 0.0; }
        SetTimer(p->wnd, ID_VTIMER, (UINT)p->visPeriod, nullptr);
    } else KillTimer(p->wnd, ID_VTIMER);
    p->visAnimate = animate;
    if (on) VisRender(p);                    // static frame (also the first frame of an animation)
    InvalidateRect(p->wnd, nullptr, FALSE);
}
// End of a 2 s accounting window: frames per second and the average background cost (render + blit). Above
// 6 ms per frame the timer goes to 31 ms (~30 fps); below 3 ms it returns to 15 ms (~60 fps). Never paused.
static void VisWindow(PanelState* p, uint64_t now) {
    const double sec = (double)(now - p->visWinStartUs) / 1e6;
    if (sec < 2.0) return;
    p->visFps = p->visWinFrames / sec;
    p->visMsPerFrame = p->visWinFrames ? p->visWinMs / p->visWinFrames : 0.0;
    const int want = p->visMsPerFrame > 6.0 ? 31 : p->visMsPerFrame < 3.0 ? 15 : p->visPeriod;
    if (want != p->visPeriod) {
        p->visPeriod = want;
        SetTimer(p->wnd, ID_VTIMER, (UINT)want, nullptr);
        Log("Panel visuals: %.2f ms per frame, %s", p->visMsPerFrame, want == 31 ? "throttled to 30 fps" : "back to 60 fps");
    }
    p->visWinMs = 0.0; p->visWinFrames = 0; p->visWinStartUs = now;
}
// Timer tick (ID_VTIMER, 15 ms): audio level from the meter snapshot, frame, invalidation of the background.
// The ~60 fps target uses a 15 ms timer: timers fire on the system timer tick (15.6 ms by default, 1 ms while the
// driver's audio runs), so 15 ms gives one frame per tick, whereas 16 ms would round up to two ticks (~31 ms).
static void VisTick(PanelState* p) {
    if (!p->vr || IsIconic(p->wnd) || !IsWindowVisible(p->wnd)) return;
    float pk = 0.f;
    for (int c = 0; c < kMaxCh; ++c) { if (p->snap.out[c] > pk) pk = p->snap.out[c]; if (p->snap.in[c] > pk) pk = p->snap.in[c]; }
    const int lvl = pk >= 1.f ? 255 : (int)(pk * 255.f);
    p->visLevel = lvl > p->visLevel ? lvl : p->visLevel * 7 / 8;   // fast attack, smooth fall
    VisRender(p);
    ++p->visWinFrames;
    VisInvalidate(p);
    VisWindow(p, VisNowUs());
}
// Status tab line (0.8.2).
static std::wstring VisStatusLine(PanelState* p) {
    // indexed by vis::Effect (kScroller is never a background); looked up each time (language can change)
    const wchar_t* const names[] = {T(S_OFF), T(S_FX_PLASMA), T(S_FX_STARFIELD), T(S_FX_COPPER), T(S_FX_TUNNEL), T(S_FX_REACTIVE),
                                    L"", T(S_FX_METABALLS), T(S_FX_FIRE), T(S_FX_VECTOR), T(S_FX_TWISTER)};
    wchar_t b[200];
    if (p->visEffect == vis::kOff) swprintf(b, 200, T(S_VIS_OFF));
    else if (!p->visAnimate) swprintf(b, 200, T(S_VIS_STILL), names[p->visEffect]);
    else swprintf(b, 200, T(S_VIS_LINE), names[p->visEffect], p->visFps,
                  p->visMsPerFrame, p->visPeriod == 31 ? T(S_YES) : T(S_NO));
    return b;
}

// ---------------------------------------------------------------- themes, languages, updates (0.9.0)
// H4: makes sure no text is cut in the current language. Every label and owner-drawn button is measured with its
// own font (GetTextExtentPoint32 / DrawText DT_CALCRECT, word wrap for multi-line labels); a text that does not fit
// gets the small UI font, and if it still does not fit it is shortened with "..." (labels and buttons draw with
// end ellipsis) or wrapped. The result is logged once per build of the panel (language code and counts).
static void LayoutAudit(PanelState* p) {
    int smaller = 0, shortened = 0;
    for (HWND c = GetWindow(p->wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32] = L"";
        GetClassNameW(c, cls, 32);
        const bool isStatic = !_wcsicmp(cls, L"STATIC"), isBtn = !_wcsicmp(cls, L"BUTTON");
        if (!isStatic && !isBtn) continue;
        const int len = GetWindowTextLengthW(c);
        if (len <= 0) continue;
        std::wstring t((size_t)len + 1, L'\0');
        GetWindowTextW(c, &t[0], len + 1); t.resize((size_t)len);
        RECT r; GetClientRect(c, &r);
        const bool multi = isStatic && r.bottom > S(p, 28);
        const int avail = r.right - (isBtn ? (IsToggle(c) ? S(p, 32) : S(p, 10)) : 0);
        auto fits = [&](HFONT f) {
            HDC dc = GetDC(c);
            HGDIOBJ of = SelectObject(dc, f);
            RECT m = {0, 0, avail, 0};
            DrawTextW(dc, t.c_str(), -1, &m, DT_CALCRECT | DT_NOPREFIX | (multi ? DT_WORDBREAK : DT_SINGLELINE));
            SelectObject(dc, of); ReleaseDC(c, dc);
            return multi ? m.bottom <= r.bottom : m.right <= avail;
        };
        HFONT f = (HFONT)SendMessageW(c, WM_GETFONT, 0, 0);
        if (!f || fits(f)) continue;
        if (f != p->fontSmall && p->fontSmall && fits(p->fontSmall)) { SendMessageW(c, WM_SETFONT, (WPARAM)p->fontSmall, TRUE); ++smaller; }
        else ++shortened;
    }
    if (smaller || shortened)
        Log("Panel layout (%ls): %d text(s) set in the small font, %d shortened with an ellipsis", kCodes[Current()], smaller, shortened);
}

// Rebuilds every control of the open panel (theme or language preview): the pending on-screen values, the
// selected profile and tab are kept, exactly like a DPI change.
static void RebuildPanel(PanelState* p) {
    Config pending = p->drv->config();
    ReadControls(p, pending);
    const std::wstring prof = CurProfile(p);
    const int tab = p->tab;
    TipCancel(p);                          // 0.9.2
    AccClearChildren(p->wnd);              // 0.9.1 (B1)
    HWND c = GetWindow(p->wnd, GW_CHILD);
    while (c) { HWND next = GetWindow(c, GW_HWNDNEXT); DestroyWindow(c); c = next; }
    for (auto& v : p->tabCtl) v.clear();
    p->cards.clear(); p->meter = nullptr;
    if (p->grad) { DeleteObject(p->grad); p->grad = nullptr; p->gradH = 0; }   // meter gradient follows the theme
    BuildControls(p);
    RefreshProfiles(p, prof);
    FillControls(p, pending);
    VisApply(p);
    ShowTab(p, tab);
    DarkTitle(p->wnd);
    std::wstring title = std::wstring(WB_NAME_W L" ") + WB_VERSION_W + L" - " + T(S_TITLE_PANEL);
    SetWindowTextW(p->wnd, title.c_str());
    InvalidateRect(p->wnd, nullptr, TRUE);
}
// Tells every floating monitor window of this process to rebuild with the current theme and language. 0.9.1
// (fix 12): all top-level windows of the process, not only this thread's (a host may open the panel on another
// GUI thread than the one that owns the monitor window); a window of another thread gets the message posted, so
// this never waits on a thread that might be waiting on us.
static BOOL CALLBACK NotifyMonProc(HWND w, LPARAM) {
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t cls[64] = L"";
    if (GetClassNameW(w, cls, 64) && !wcscmp(cls, L"WasapiBridgeMonitor")) {
        if (tid == GetCurrentThreadId()) SendMessageW(w, WM_WB_RETHEME, 0, 0);
        else PostMessageW(w, WM_WB_RETHEME, 0, 0);
    }
    return TRUE;
}
static void NotifyMonitorWindows() { EnumWindows(GuardedEnumProc<NotifyMonProc>, 0); }

// H3: repository of the update source: config.ini UpdateRepo if valid, else the build default (a placeholder
// until the project is published, see RELEASING.txt).
static std::wstring UpdateRepoOf(const Config& c) {
    return upd::ValidRepo(c.updateRepo) ? c.updateRepo : std::wstring(WB_UPDATE_REPO);
}
// H3: %LOCALAPPDATA%\WASAPI Driver for ASIO\updates (created; the parent too).
static std::wstring UpdatesFolder() {
    wchar_t b[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH);
    std::wstring base = n && n < MAX_PATH ? std::wstring(b) : ConfigDir();
    base += L"\\" WB_NAME_W;
    CreateDirectoryW(base.c_str(), nullptr);
    base += L"\\updates";
    CreateDirectoryW(base.c_str(), nullptr);
    return base;
}
// H3: status line, last-check line and button states from the current update state. Host thread.
static void UpdUpdateUi(PanelState* p) {
    HWND st = GetDlgItem(p->wnd, ID_UPDSTATUS), last = GetDlgItem(p->wnd, ID_UPDLAST);
    const bool busy = p->upd && !upd::Done(p->upd);
    const bool conf = !upd::IsPlaceholderRepo(UpdateRepoOf(p->drv->config()));
    if (st) SetWindowTextW(st, p->updMsg.c_str());
    if (HWND b = GetDlgItem(p->wnd, ID_UPDCHECK)) EnableWindow(b, conf && !busy);
    if (HWND b = GetDlgItem(p->wnd, ID_UPDDL)) ShowWindow(b, conf && p->updNewer && p->updSetup.empty() ? SW_SHOW : SW_HIDE),
                                              EnableWindow(b, !busy);
    if (HWND b = GetDlgItem(p->wnd, ID_UPDRUN)) ShowWindow(b, !p->updSetup.empty() ? SW_SHOW : SW_HIDE);
    if (last) {
        const long long t = p->drv->config().updateLastCheck;
        std::wstring when = T(S_UPD_NEVER);
        if (t > 0) {
            // FILETIME from seconds since 1970 -> local date and time in the user's format
            ULARGE_INTEGER u; u.QuadPart = (ULONGLONG)(t + 11644473600LL) * 10000000ULL;
            FILETIME ft = {u.LowPart, u.HighPart}, lt; SYSTEMTIME sy;
            if (FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &sy)) {
                wchar_t d[64] = L"", tm[32] = L"";
                GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &sy, nullptr, d, 64);
                GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &sy, nullptr, tm, 32);
                when = std::wstring(d) + L" " + tm;
            }
        }
        SetWindowTextW(last, FmtW(T(S_UPD_LAST), when.c_str()).c_str());
    }
}
// H3: starts a check (download = false) or the download of the release found (download = true) on the worker
// thread. Never blocks: the result arrives as WM_WB_UPDDONE.
static void UpdStart(PanelState* p, bool download) {
    if (p->upd && !upd::Done(p->upd)) { p->updMsg = T(S_UPD_BUSY); UpdUpdateUi(p); return; }
    const std::wstring repo = UpdateRepoOf(p->drv->config());
    if (upd::IsPlaceholderRepo(repo)) { p->updMsg = T(S_UPD_NOTCONF); UpdUpdateUi(p); return; }
    if (p->upd) { upd::Unref(p->upd); p->upd = nullptr; }
    upd::Job* j = download ? upd::StartDownload(p->wnd, WM_WB_UPDDONE, p->updRel, UpdatesFolder())
                           : upd::StartCheck(p->wnd, WM_WB_UPDDONE, repo);
    if (!j) { p->updMsg = FmtW(T(S_UPD_FAILED), T(S_UPD_ERR_UNAVAILABLE)); UpdUpdateUi(p); return; }
    p->upd = j;
    p->updMsg = download ? FmtW(T(S_UPD_DOWNLOADING), upd::Widen(upd::SetupAssetName(p->updRel)).c_str()) : std::wstring(T(S_UPD_CHECKING));
    if (!download) {                                   // remember the time of the check (only this key is written)
        const long long now = (long long)time(nullptr);
        SaveGeneralInt64(L"UpdateLastCheck", now);
        p->drv->noteUpdateCheck(now);
    }
    UpdUpdateUi(p);
}
// H3: the worker finished: show what it found (the job's results are read here, on the host thread).
static void UpdDone(PanelState* p) {
    upd::Job* j = p->upd;
    if (!j || !upd::Done(j)) return;
    const upd::Result& r = upd::ResultOf(j);
    auto reason = [&]() -> std::wstring {
        switch (r.error) {
        case upd::kErrNet: return FmtW(T(S_UPD_ERR_NET), (unsigned long)r.detail);
        case upd::kErrHttp: return FmtW(T(S_UPD_ERR_HTTP), (unsigned long)r.detail);
        case upd::kErrParse: return T(S_UPD_ERR_PARSE);
        case upd::kErrNoAsset: return T(S_UPD_ERR_NOASSET);
        case upd::kErrSize: return T(S_UPD_ERR_SIZE);
        case upd::kErrFile: return T(S_UPD_ERR_FILE);
        case upd::kErrHost: return T(S_UPD_ERR_HOST);
        case upd::kErrTimeout: return T(S_UPD_ERR_TIMEOUT);
        default: return T(S_UPD_ERR_UNAVAILABLE);
        }
    };
    if (r.op == upd::kOpCheck) {
        if (r.error) p->updMsg = FmtW(T(S_UPD_FAILED), reason().c_str());
        else {
            p->updRel = r.release;
            upd::Version cur, got;
            upd::ParseVersion(WB_VERSION_STR, cur);
            const bool ok = upd::ParseVersion(r.release.tag, got);
            p->updNewer = ok && upd::Compare(got, cur) > 0;
            const std::wstring tag = upd::Widen(r.release.tag), name = upd::Widen(r.release.name.empty() ? r.release.tag : r.release.name);
            p->updMsg = p->updNewer ? FmtW(T(S_UPD_AVAILABLE), tag.c_str(), name.c_str()) : FmtW(T(S_UPD_UPTODATE), WB_VERSION_W);
            Log("Update check: latest release %s (%s)", upd::SafeAscii(r.release.tag).c_str(), p->updNewer ? "newer" : "not newer");
        }
    } else {
        if (r.error == upd::kErrHash) p->updMsg = T(S_UPD_BADHASH);
        else if (r.error == upd::kErrSignature) p->updMsg = T(S_UPD_BADSIG);
        else if (r.error) p->updMsg = FmtW(T(S_UPD_FAILED), reason().c_str());
        else {
            p->updSetup = r.path; p->updSetupHash = r.sha256; p->updSetupSigned = r.signedValid;
            p->updMsg = FmtW(T(S_UPD_VERIFIED), r.signedValid ? T(S_UPD_SIGNED) : T(S_UPD_UNSIGNED));
        }
        Log("Update download: %s", r.error ? "refused or failed" : (r.signedValid ? "verified, signed" : "verified, not signed"));
    }
    UpdUpdateUi(p);
}
// H3: runs the verified Setup after an explicit confirmation. Never silent: the Setup shows its own pages.
// 0.9.1 (fix 1): hash, signature check and start all happen through one read-only handle that denies writers
// until the Setup process exists (upd::LaunchVerified), so the file started is the file that was verified.
static void UpdRunSetup(PanelState* p) {
    if (p->updSetup.empty()) return;
    if (PanelMsgBox(p, p->wnd, T(S_UPD_INSTALL_Q), WB_NAME_W, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    const upd::Launch r = upd::LaunchVerified(p->wnd, p->updSetup, p->updSetupHash, p->updSetupSigned);
    if (r == upd::kLaunchHash || r == upd::kLaunchSignature) {
        DeleteFileW(p->updSetup.c_str());
        p->updSetup.clear(); p->updMsg = T(r == upd::kLaunchHash ? S_UPD_BADHASH : S_UPD_BADSIG); UpdUpdateUi(p);
        Log("Update: Setup refused at launch (%s)", r == upd::kLaunchHash ? "hash" : "signature");
        return;
    }
    Log(r == upd::kLaunchOk ? "Update: Setup started by the user" : "Update: the Setup could not be started");
}

// Slider (0..100) without comctl32: value in GWLP_USERDATA; mouse drag or the arrow, Home and End keys; the parent gets
// WM_COMMAND(id, EN_CHANGE) on every change. Host thread.
static LRESULT CALLBACK SliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto setv = [&](int v) {
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        if (v == (int)GetWindowLongPtrW(h, GWLP_USERDATA)) return;
        SetWindowLongPtrW(h, GWLP_USERDATA, v);
        InvalidateRect(h, nullptr, FALSE);
        a11y::NotifyValue(h);                          // 0.9.1 (B1)
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), EN_CHANGE), (LPARAM)h);
    };
    auto fromX = [&](int x) { RECT r; GetClientRect(h, &r); const int m = r.bottom / 2; const int w = r.right - 2 * m;
                              return w > 0 ? (int)((x - m) * 100.0 / w + 0.5) : 0; };
    switch (msg) {
    case WM_GETOBJECT: {                               // 0.9.1 (B1): a slider with a value for screen readers
        LRESULT r = 0;
        if (a11y::OnGetObject(h, wp, lp, a11y::kSlider, r)) return r;
        break;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: SetFocus(h); SetCapture(h); setv(fromX(GET_X_LPARAM(lp))); return 0;
    case WM_MOUSEMOVE: if (GetCapture() == h) setv(fromX(GET_X_LPARAM(lp))); return 0;
    case WM_LBUTTONUP: if (GetCapture() == h) ReleaseCapture(); return 0;
    case WM_KEYDOWN: {
        const int v = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (wp == VK_LEFT || wp == VK_DOWN) setv(v - 5); else if (wp == VK_RIGHT || wp == VK_UP) setv(v + 5);
        else if (wp == VK_HOME) setv(0); else if (wp == VK_END) setv(100);
        else if (wp == VK_PRIOR) setv(v + 20); else if (wp == VK_NEXT) setv(v - 20);
        return 0;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        FillRectC(dc, 0, 0, r.right, r.bottom, g_th.card);
        const int m = r.bottom / 2, w = r.right - 2 * m, v = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
        const int cy = r.bottom / 2, tx = m + w * v / 100, th = r.bottom / 8 > 1 ? r.bottom / 8 : 1;
        FillRectC(dc, m, cy - th, r.right - m, cy + th, g_th.line);
        FillRectC(dc, m, cy - th, tx, cy + th, g_th.accentLo);
        const int k = r.bottom / 3;
        RoundRectC(dc, tx - k, cy - k, tx + k, cy + k, 2 * k, GetFocus() == h ? g_th.accentHi : g_th.accent, g_th.card);
        if (GetFocus() == h) {                         // 0.9.1 (B1): visible focus in every theme
            HPEN pen = CreatePen(PS_SOLID, 1, g_th.focus);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, r.right, r.bottom);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 0.9.0: text in the 5x7 dot font of visuals.h (uppercase, digits, a few signs), one square of `d` pixels per
// dot and one dot of spacing. Returns the x after the last character.
static int DrawDotText(HDC dc, const char* text, int x, int y, int d, COLORREF c) {
    HBRUSH br = CreateSolidBrush(c);
    for (const char* q = text; *q; ++q) {
        const char ch = (*q >= 'a' && *q <= 'z') ? (char)(*q - 32) : *q;
        for (const vis::Glyph& g : vis::kFont) {
            if (g.c != ch) continue;
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 5; ++col)
                    if (g.rows[r] & (0x10 >> col)) { RECT rc = {x + col * d, y + r * d, x + col * d + d - (d > 2 ? 1 : 0), y + r * d + d - (d > 2 ? 1 : 0)}; FillRect(dc, &rc, br); }
            break;
        }
        x += 6 * d;
    }
    DeleteObject(br);
    return x;
}

// WM_PAINT of the panel: header with name and logo, tab strip, cards of the current tab, bottom summary.
// Double-buffered to avoid flicker.
static void PaintPanel(PanelState* p, HDC dc, const RECT& rc) {
    if (p->vr && p->vr->pixels()) {                          // 0.8.1: animated background (0.8.2: blit timed)
        const uint64_t a = VisNowUs();
        VisBlit(dc, *p->vr, rc);
        p->visWinMs += (double)(VisNowUs() - a) / 1000.0;
    } else FillRect(dc, &rc, g_th.bgBr);
    // Header: name (first mention of ASIO carries (R)), subtitle and the ASIO Compatible logo, always visible
    RECT t = {S(p, 20), S(p, 10), S(p, p->cw - 120), S(p, 40)};
    if (g_th.dotHeader) {                  // 0.9.0 (H1): retro themes draw the name with the built-in 5x7 dot font
        const int d = S(p, 3) > 1 ? S(p, 3) : 1;
        const int x1 = DrawDotText(dc, "WASAPI DRIVER FOR ASIO", t.left, (t.top + t.bottom) / 2 - 7 * d / 2, d, g_th.text);
        RECT rr = {x1 + d, t.top, t.right, t.bottom};
        TextC(dc, p->font, g_th.text, L"\x00AE", rr, DT_LEFT | DT_TOP | DT_SINGLELINE);   // the (R) of the first mention
    } else TextC(dc, p->title, g_th.text, WB_NAME_W L"\x00AE", t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT st = {S(p, 20), S(p, 38), S(p, p->cw - 120), S(p, 58)};
    TextC(dc, p->fontSmall, g_th.dim, FmtW(T(S_HDR_SUB), WB_VERSION_W).c_str(), st, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (p->logoS) DrawLogo(dc, p->logoS, p->logoSW, p->logoSH, S(p, p->cw - 16) - p->logoSW, S(p, 32) - p->logoSH / 2, g_th.bg);
    // Tabs
    HGDIOBJ of = SelectObject(dc, p->fontB);
    int x = S(p, 16);
    const int ty = S(p, 68), tb = S(p, 100);
    for (int i = 0; i < TAB_COUNT; ++i) {
        SIZE sz; GetTextExtentPoint32W(dc, TabName(i), (int)wcslen(TabName(i)), &sz);
        RECT r = {x, ty, x + sz.cx + S(p, 28), tb};
        p->tabRc[i] = r;
        const bool sel = i == p->tab;
        if (sel) RoundRectC(dc, r.left, r.top, r.right, r.bottom + S(p, 8), S(p, 10), g_th.card, g_th.card);
        {                                  // 0.9.1 (B1): access key underlined while Windows shows keyboard cues
            const std::wstring lbl = p->tabKeyPos[i] >= 0 ? WithPrefix(TabName(i), p->tabKeyPos[i]) : WithPrefix(TabName(i), -1);
            const UINT pre = (SendMessageW(p->wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, sel ? g_th.accent : g_th.dim);
            RECT lr = r;
            DrawTextW(dc, lbl.c_str(), -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | pre);
            if (sel && GetFocus() == p->wnd) { RECT fr = r; InflateRect(&fr, -S(p, 4), -S(p, 4)); DrawFocusRect(dc, &fr); }
        }
        if (sel) FillRectC(dc, r.left + S(p, 12), r.bottom - S(p, 3), r.right - S(p, 12), r.bottom, g_th.accent);
        x = r.right + S(p, 4);
    }
    SelectObject(dc, of);
    FillRectC(dc, S(p, 16), tb, S(p, p->cw - 16), tb + 1, g_th.line);
    // Cards of the current tab
    for (const Card& c : p->cards) {
        if (c.tab != p->tab) continue;
        RoundRectC(dc, c.r.left, c.r.top, c.r.right, c.r.bottom, S(p, 14), g_th.card, g_th.card);
        if (!c.title.empty()) {
            RECT tr = {c.r.left + S(p, 16), c.r.top + S(p, 10), c.r.right - S(p, 16), c.r.top + S(p, 30)};
            TextC(dc, p->fontB, g_th.text, c.title.c_str(), tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        if (p->tab == TAB_ABOUT) DrawAbout(p, dc, c.r);
    }
    if (p->tab == TAB_ABOUT && p->vrAbout && p->vrAbout->pixels()) {         // scroller
        const uint64_t a = VisNowUs();
        VisBlit(dc, *p->vrAbout, p->aboutStrip);
        p->visWinMs += (double)(VisNowUs() - a) / 1000.0;
    }
    if (p->tab == TAB_MAIN) DrawTiles(p, dc);
    // Bottom bar: summary of the active mode
    FillRectC(dc, S(p, 16), S(p, p->ch - 64), S(p, p->cw - 16), S(p, p->ch - 63), g_th.line);
    wchar_t sum[256];
    const bool safeBanner = p->drv->safeMode() && p->rebind == 0;
    // 0.9.11 (KI-12): Mode=Exclusive configured, stream running Shared (amber, like the privacy notice)
    const bool exclFall = p->stValid && p->st.streamsOpen && p->st.exclusiveWanted && p->st.exclFail != 0;   // review R1: output (1) or input (2)
    if (p->rebind == 1) swprintf(sum, 256, T(S_RESTARTING));
    else if (p->rebind == 2) swprintf(sum, 256, T(S_DAW_RELEASED));
    else if (!p->applyMsg.empty()) swprintf(sum, 256, L"%ls", p->applyMsg.c_str());
    else if (safeBanner) swprintf(sum, 256, T(S_SAFE_BANNER));
    else if (p->stValid && p->st.streamsOpen)
        swprintf(sum, 256, T(S_SUM_FMT),
                 p->st.outExclusive ? T(S_EXCLUSIVE) : p->st.outLowLatency ? T(S_SHARED_LL) : T(S_SHARED_STD),
                 p->st.rate, p->st.nIn, p->st.nOut, p->st.captureDenied ? T(S_SUM_PRIV) : exclFall ? (p->st.exclFail == 2 ? T(S_SUM_EXCLFALL_IN) : T(S_SUM_EXCLFALL)) : L"");
    else swprintf(sum, 256, T(S_SUM_NOSTREAM));
    // 0.9.11: the Apply button is 170 px wide (its text became "Apply and Close"): the summary stops before it
    RECT sr = {S(p, 20), S(p, p->ch - 54), S(p, p->cw - (safeBanner ? 530 : 320)), S(p, p->ch - 16)};
    const COLORREF col = p->rebind ? g_th.amber : !p->applyMsg.empty() ? g_th.accent : (safeBanner || (p->stValid && (p->st.captureDenied || exclFall))) ? g_th.amber : g_th.dim;
    TextC(dc, p->font, col, sum, sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void OnDpiChanged(PanelState* p, int dpi, const RECT* suggested);

// Window procedure of the panel. WM_TIMER refreshes status (500 ms) and meters (50 ms); WM_COMMAND handles
// buttons (BN_CLICKED and BN_DOUBLECLICKED alike, so fast clicks on toggles are not lost), combos and lists.
static LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PanelState* p = (PanelState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        p = (PanelState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
        p->wnd = h;
        BuildControls(p);
        SetTimer(h, ID_TIMER, 500, nullptr);
        SetTimer(h, ID_MTIMER, 50, nullptr);             // meters: only while the panel is open
        SetTimer(h, ID_TOPTIMER, 250, nullptr);          // 0.7.1 (A): stay above the host's windows
        return 0;
    }
    case WM_SHOWWINDOW:
        // 0.7.1 (A): the owner is being minimized or hidden (SW_PARENTCLOSING): do not hide with it; detach
        // from the owner instead and stay visible (not passing the message on prevents the hide).
        if (p && !wp && lp == SW_PARENTCLOSING) {
            if (p->owner && IsWindow(p->owner)) EnableWindow(p->owner, TRUE);
            SetWindowLongPtrW(h, GWLP_HWNDPARENT, 0);
            p->owner = nullptr; p->topState = false;
            return 0;
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        // 0.8.1: the back buffer is cached (no bitmap allocation per animation frame) and drawing is clipped to
        // the update region, so a visuals frame only redraws the background areas it invalidated.
        HRGN upd = CreateRectRgn(0, 0, 0, 0);
        const int ur = upd ? GetUpdateRgn(h, upd, FALSE) : ERROR;
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = nullptr;
        if (p) {
            if (!p->back || p->backW != rc.right || p->backH != rc.bottom) {
                if (p->back) DeleteObject(p->back);
                p->back = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
                p->backW = rc.right; p->backH = rc.bottom;
            }
            bmp = p->back;
        } else bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        if (ur == SIMPLEREGION || ur == COMPLEXREGION) SelectClipRgn(mem, upd);
        if (p) PaintPanel(p, mem, rc); else FillRect(mem, &rc, g_th.bgBr);
        SelectClipRgn(mem, nullptr);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
               mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, ob); DeleteDC(mem);
        if (!p) DeleteObject(bmp);
        if (upd) DeleteObject(upd);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (p) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            for (int i = 0; i < TAB_COUNT; ++i) if (PtInRect(&p->tabRc[i], pt) && i != p->tab) { ShowTab(p, i); break; }
        }
        return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return CtlColor(msg, wp, lp);
    case WM_DRAWITEM:
        if (p) { DrawOwnerButton((const DRAWITEMSTRUCT*)lp, p->dpi); return TRUE; }
        break;
    case WM_SETCURSOR:                                     // 0.9.2: hover tracking for the help tips
        if (p) TipOnSetCursor(p, h, tips::kPanel, (HWND)wp, HIWORD(lp));
        break;                                             // normal cursor handling continues
    case a11y::kMsgDescription:                            // 0.9.2: help tip = accessible description
        return TipDescribe(tips::kPanel, h, (HWND)wp, (wchar_t*)lp);
    case WM_HELP:                                          // 0.9.2: F1 that did not pass the panel loop
        if (p) TipForControl(p, h, tips::kPanel, (HWND)((HELPINFO*)lp)->hItemHandle);
        return TRUE;
    case a11y::kMsgSliderText:                             // 0.9.1 (B1): slider value for screen readers
        if (p && lp) {
            const int id = GetDlgCtrlID((HWND)wp), v = (int)GetWindowLongPtrW((HWND)wp, GWLP_USERDATA);
            swprintf((wchar_t*)lp, 64, L"%d %%", id == ID_VISSPD ? VisSpeedFromPos(v) : v);
            return 1;
        }
        return 0;
    case WM_UPDATEUISTATE: {                               // 0.9.1 (B1): show / hide the tabs' access keys
        const LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (p) { RECT tr = {0, S(p, 60), S(p, p->cw), S(p, 104)}; InvalidateRect(h, &tr, FALSE); }
        return r;
    }
    case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:         // 0.9.1 (B1): high contrast turned on or off
        if (p && (msg == WM_SYSCOLORCHANGE || wp == SPI_SETHIGHCONTRAST) && g_th.hc != HighContrastOn()) {
            ThemeSelect(g_th.id);                          // keeps the theme being previewed
            RebuildPanel(p);
            NotifyMonitorWindows();
        }
        break;
    case WM_TIMER:
        if (p) {
            if (wp == ID_TIPTIMER) TipTick(p);          // 0.9.2
            else if (wp == ID_MTIMER) UpdateMeters(p); else if (wp == ID_TOPTIMER) KeepWithHost(p);
            else if (wp == ID_VTIMER) VisTick(p); else UpdateStatus(p);
        }
        return 0;
    case WM_COMMAND:
        if (p && (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == BN_DOUBLECLICKED) && IsToggle((HWND)lp)) {
            HWND c = (HWND)lp;
            SetToggle(c, GetWindowLongPtrW(c, GWLP_USERDATA) == 0);
            return 0;
        }
        switch (LOWORD(wp)) {
        case IDOK:                                          // 0.9.11: "Apply and Close" (0.7.0-0.9.10: the panel stayed open)
            if (Apply(p)) { if (p->owner) EnableWindow(p->owner, TRUE); DestroyWindow(h); }   // review R3: only when nothing must stay visible
            return 0;
        case ID_SETNONE:                                   // 0.7.1 (C): Input device = (None - no inputs)
            // 0.9.6 (KI-4): also clears the extra inputs (an empty selection): they would be blocked by the same
            // Windows setting, and the warning now stays visible while any input is selected. Then hide the row.
            Sel(GetDlgItem(h, ID_IN), 0); FillExtraList(p, true, {});
            UpdatePrivacyRow(p);
            if (p->stValid) UpdateDmNote(p, p->st);       // review R6: the note follows at once, not at the next ~4 s poll
            p->applyMsg = T(S_MSG_SETNONE); p->applyTick = GetTickCount();
            { RECT bar = {0, S(p, p->ch - 64), S(p, p->cw), S(p, p->ch)}; InvalidateRect(h, &bar, FALSE); }
            return 0;
        case ID_SAFEBTN:                                   // F8: "Use my settings again"
            p->drv->leaveSafeMode();
            ShowWindow((HWND)lp, SW_HIDE);
            p->applyMsg = T(S_MSG_SAFE_LEFT); p->applyTick = GetTickCount();
            { RECT bar = {0, S(p, p->ch - 64), S(p, p->cw), S(p, p->ch)}; InvalidateRect(h, &bar, FALSE); }
            return 0;
        case IDCANCEL: if (p->owner) EnableWindow(p->owner, TRUE); DestroyWindow(h); return 0;
        case ID_PROFILE: if (HIWORD(wp) == CBN_SELCHANGE) OnProfileSelected(p); return 0;
        case ID_PSAVE: SaveProfileAs(p); return 0;
        case ID_PDEL: DeleteCurProfile(p); return 0;
        case ID_LB: if (HIWORD(wp) == CBN_SELCHANGE) UpdateAppRow(p); return 0;
        case ID_VIS: if (HIWORD(wp) == CBN_SELCHANGE) { p->visTouched = true; VisApply(p); } return 0;   // 0.8.1 preview
        case ID_THEME:                                     // 0.9.0 (H1): preview at once, Apply saves
            if (HIWORD(wp) == CBN_SELCHANGE) { ThemeSelect(Cur(h, ID_THEME)); RebuildPanel(p); NotifyMonitorWindows(); }
            return 0;
        case ID_LANG:                                      // 0.9.0 (H4): preview at once, Apply saves
            if (HIWORD(wp) == CBN_SELCHANGE) { SetLang(Resolve(Cur(h, ID_LANG), GetUserDefaultUILanguage())); RebuildPanel(p); NotifyMonitorWindows(); }
            return 0;
        case ID_UPDCHECK: UpdStart(p, false); return 0;    // 0.9.0 (H3)
        case ID_UPDDL: UpdStart(p, true); return 0;
        case ID_UPDRUN: UpdRunSetup(p); return 0;          // asks first; touch nothing afterwards
        case ID_VISINT: if (HIWORD(wp) == EN_CHANGE) { p->visIntensity = (int)GetWindowLongPtrW((HWND)lp, GWLP_USERDATA); if (p->vr && !p->visAnimate) VisApply(p); } return 0;
        case ID_VISSPD: if (HIWORD(wp) == EN_CHANGE) p->visSpeed = VisSpeedFromPos((int)GetWindowLongPtrW((HWND)lp, GWLP_USERDATA)); return 0;   // 0.8.2
        case ID_BUF: if (HIWORD(wp) == CBN_SELCHANGE) UpdatePrivacyRow(p); return 0;   // 0.8.0: "Use recommended" visibility
        case ID_OUT: case ID_IN:                           // the main device cannot be an extra: rebuild the list
            if (HIWORD(wp) == CBN_SELCHANGE) {
                bool in = LOWORD(wp) == ID_IN; FillExtraList(p, in, SelectedExtras(p, in));
                if (in) {                                  // 0.9.6 (KI-4): the warning follows the input choice
                    UpdatePrivacyRow(p);
                    if (p->stValid) UpdateDmNote(p, p->st);   // review R6: and the direct-monitoring note too
                }
            }
            return 0;
        case ID_EXOUT: case ID_EXIN:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                LimitExtraSel(p, LOWORD(wp));
                if (LOWORD(wp) == ID_EXIN) {               // 0.9.6 (KI-4): extra inputs count too
                    UpdatePrivacyRow(p);
                    if (p->stValid) UpdateDmNote(p, p->st);   // review R6
                }
            }
            return 0;
        case ID_ROUTE: EditRouting(p); return 0;
        case ID_COPY: CopyStatus(p); return 0;
        case ID_BTREC:                                     // 0.8.0 (G1): one click: recommended buffer + Apply
            if (p->stValid && p->st.recBuffer > 0) { SetBufferCombo(p, p->st.recBuffer); Apply(p); }
            return 0;
        case ID_HPDEL: {                                   // 0.8.0 (G3): remove the selected program mapping now
            const int i = Cur(h, ID_HPLIST);
            if (i >= 0 && i < (int)p->hpKeys.size()) {
                const std::wstring key = p->hpKeys[(size_t)i];
                SetHostProfile(key, L"");
                const std::wstring keep = _wcsicmp(key.c_str(), p->hostExe.c_str()) ? CurHostProfile(p) : std::wstring();
                FillHostProfiles(p, &keep);
            }
            return 0;
        }
        case ID_APPREF: RefreshApps(p); return 0;
        case ID_OPENLOG: ShellExecuteW(h, L"open", (ConfigDir() + L"\\log.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
        case ID_OPENDIR: ShellExecuteW(h, L"open", ConfigDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
        case ID_RESETCNT:                                  // the audio thread consumes the request (or it applies directly without a thread)
            p->drv->resetCounters(WB_RESET_ALL); p->snap.clipIn = p->snap.clipOut = 0; ClearHolds(p);
            UpdateStatus(p); if (p->meter) InvalidateRect(p->meter, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CLOSE: if (p && p->owner) EnableWindow(p->owner, TRUE); DestroyWindow(h); return 0;
    case WM_DPICHANGED:                                // monitor with a different DPI (host is per-monitor DPI aware)
        if (p) OnDpiChanged(p, LOWORD(wp), (const RECT*)lp);
        return 0;
    case WM_WB_UPDDONE:                                // 0.9.0 (H3): the update worker finished (posted)
        if (p) UpdDone(p);
        return 0;
    case WM_SIZE:                                      // 0.8.1: low-resolution buffers follow the client size
        if (p && p->vr && wp != SIZE_MINIMIZED) { VisResize(p); VisRender(p); InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_ACTIVATEAPP:                               // back in the host: keep the panel in front of its disabled owner
        if (!wp && p) TipHide(p);                      // 0.9.2: another program came to the front
        if (wp && p && !IsIconic(h)) SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    case WM_DESTROY:
        KillTimer(h, ID_TIMER);
        KillTimer(h, ID_MTIMER);
        KillTimer(h, ID_TOPTIMER);
        KillTimer(h, ID_VTIMER);
        if (p && p->back) { DeleteObject(p->back); p->back = nullptr; }
        if (p) TipDestroy(p);                              // 0.9.2
        AccClearChildren(h);                               // 0.9.1 (B1)
        if (p && p->upd) { upd::Cancel(p->upd); upd::Unref(p->upd); p->upd = nullptr; }   // fix 4: abort; the worker keeps its own reference
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (p) p->closed = true;
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// UI font: Segoe UI Variable Text if present (Windows 11), otherwise Segoe UI.
static const wchar_t* UiFace() {
    static const wchar_t* face = nullptr;
    if (face) return face;
    face = L"Segoe UI";
    HDC dc = GetDC(nullptr);
    HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    if (f) {
        HGDIOBJ o = SelectObject(dc, f);
        wchar_t got[LF_FACESIZE] = L"";
        GetTextFaceW(dc, LF_FACESIZE, got);
        if (!_wcsicmp(got, L"Segoe UI Variable Text")) face = L"Segoe UI Variable Text";
        SelectObject(dc, o); DeleteObject(f);
    }
    ReleaseDC(nullptr, dc);
    return face;
}

// Fonts and logos for the current DPI (recreated on WM_DPICHANGED). FreeUiResources releases them.
static void CreateUiResources(PanelState* p) {
    const wchar_t* face = UiFace();
    auto mk = [&](int pt, int weight, const wchar_t* f) {
        return CreateFontW(-MulDiv(pt, p->dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, f);
    };
    p->font = mk(9, FW_NORMAL, face); p->fontB = mk(9, FW_SEMIBOLD, face); p->title = mk(15, FW_SEMIBOLD, face);
    p->big = mk(17, FW_SEMIBOLD, face); p->fontSmall = mk(8, FW_NORMAL, face); p->mono = mk(9, FW_NORMAL, L"Consolas");
    p->logoSH = S(p, 40); p->logoS = LoadLogo(p->logoSH, p->logoSW);
    p->logoLH = S(p, 120); p->logoL = LoadLogo(p->logoLH, p->logoLW);
}
static void FreeUiResources(PanelState* p) {
    HGDIOBJ objs[] = {p->font, p->fontB, p->title, p->big, p->fontSmall, p->mono, p->logoS, p->logoL, p->grad};
    for (HGDIOBJ o : objs) if (o) DeleteObject(o);
    p->font = p->fontB = p->title = p->big = p->fontSmall = p->mono = nullptr;
    p->logoS = p->logoL = p->grad = nullptr; p->gradH = 0;
}

// Window size for a given client size: AdjustWindowRectExForDpi (Windows 10 1607+, loaded at runtime) when
// available, otherwise AdjustWindowRectEx.
static RECT WindowRectFor(int cw, int ch, DWORD style, DWORD ex, int dpi) {
    typedef BOOL (WINAPI* PFN_AdjForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static PFN_AdjForDpi fn = (PFN_AdjForDpi)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    RECT rc = {0, 0, cw, ch};
    if (!(fn && fn(&rc, style, FALSE, ex, (UINT)dpi))) AdjustWindowRectEx(&rc, style, FALSE, ex);
    return rc;
}

// WM_DPICHANGED: new fonts/logos and rebuilt controls, keeping what is on screen (ReadControls before the
// rebuild, FillControls after), then the window moves to the rectangle suggested by Windows.
static void OnDpiChanged(PanelState* p, int dpi, const RECT* suggested) {
    TipCancel(p);                          // 0.9.2
    SetWindowIcons(p->wnd, dpi);           // 0.9.3
    Config pending = p->drv->config();
    ReadControls(p, pending);
    const std::wstring prof = CurProfile(p);
    const int tab = p->tab;
    HWND c = GetWindow(p->wnd, GW_CHILD);
    while (c) { HWND next = GetWindow(c, GW_HWNDNEXT); DestroyWindow(c); c = next; }
    for (auto& v : p->tabCtl) v.clear();
    p->cards.clear(); p->meter = nullptr;
    FreeUiResources(p);
    {   // 0.7.1 (C): reduce the layout scale if the window would not fit the target monitor's work area
        const DWORD st0 = (DWORD)GetWindowLongPtrW(p->wnd, GWL_STYLE), ex0 = (DWORD)GetWindowLongPtrW(p->wnd, GWL_EXSTYLE);
        RECT wa = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
        if (suggested && GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
        for (int guard = 0; guard < 40 && dpi > 72; ++guard) {
            RECT r = WindowRectFor(MulDiv(p->cw, dpi, 96), MulDiv(p->ch, dpi, 96), st0, ex0, dpi);
            if (r.bottom - r.top <= wa.bottom - wa.top && r.right - r.left <= wa.right - wa.left) break;
            dpi -= 4;
        }
    }
    p->dpi = dpi;
    CreateUiResources(p);
    BuildControls(p);
    RefreshProfiles(p, prof);
    FillControls(p, pending);
    ShowTab(p, tab);
    const DWORD style = (DWORD)GetWindowLongPtrW(p->wnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(p->wnd, GWL_EXSTYLE);
    RECT rc = WindowRectFor(S(p, p->cw), S(p, p->ch), style, ex, dpi);
    SetWindowPos(p->wnd, nullptr, suggested ? suggested->left : 0, suggested ? suggested->top : 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE));
    InvalidateRect(p->wnd, nullptr, TRUE);
}

// Ctrl+Tab / Ctrl+Shift+Tab: next / previous tab. Called before IsDialogMessage; returns true if handled.
static bool HandleTabKeys(PanelState* p, const MSG& m) {
    if (TipKey(p, p->wnd, tips::kPanel, m)) return true;   // 0.9.2: F1 / Shift+F1, Esc, keys and wheel hide
    if (m.hwnd == p->wnd || IsChild(p->wnd, m.hwnd)) {
        if (EnterOnButton(p->wnd, m)) return true;       // 0.9.1 (B1)
        if (m.message == WM_SYSCHAR) {                   // 0.9.1 (B1): Alt+letter of a tab or of Apply / Close
            const wchar_t k = (wchar_t)towupper((wint_t)m.wParam);
            for (int i = 0; i < TAB_COUNT; ++i)
                if (p->tabKey[i] && p->tabKey[i] == k) { if (i != p->tab) ShowTab(p, i); return true; }
            const int ids[2] = {IDOK, IDCANCEL};
            for (int id : ids) {
                HWND b = GetDlgItem(p->wnd, id);
                wchar_t t[64] = L"";
                if (!b || !IsWindowVisible(b) || !GetWindowTextW(b, t, 64)) continue;
                for (const wchar_t* q = t; *q; ++q) {
                    if (q[0] == L'&' && q[1] == L'&') { ++q; continue; }
                    if (q[0] == L'&' && q[1] && (wchar_t)towupper(q[1]) == k) { if (IsWindowEnabled(b)) SendMessageW(b, BM_CLICK, 0, 0); return true; }
                }
            }
        }
    }
    if (m.message != WM_KEYDOWN || m.wParam != VK_TAB || GetKeyState(VK_CONTROL) >= 0) return false;
    if (m.hwnd != p->wnd && !IsChild(p->wnd, m.hwnd)) return false;
    const int dir = GetKeyState(VK_SHIFT) < 0 ? TAB_COUNT - 1 : 1;
    ShowTab(p, (p->tab + dir) % TAB_COUNT);
    return true;
}

// Registers the window classes of the panel, the meter child, the dialogs and the monitor window once. H.
// 0.9.8: every class is registered with GuardedWndProc<...> (guard.h), the exception barrier of its procedure.
static LRESULT CALLBACK MonitorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void RegisterPanelClasses() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = GuardedWndProc<PanelProc>; wc.hInstance = g_hInst; wc.lpszClassName = L"WasapiBridgePanel";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = nullptr;
        wc.hIcon = AppIcon(GetSystemMetrics(SM_CXICON));   // 0.9.3
        RegisterClassW(&wc);
        WNDCLASSW mc = {};
        mc.lpfnWndProc = GuardedWndProc<MeterProc>; mc.hInstance = g_hInst; mc.lpszClassName = L"WasapiBridgeMeter";
        mc.hCursor = LoadCursor(nullptr, IDC_HAND);
        RegisterClassW(&mc);
        WNDCLASSW nc = {};
        nc.lpfnWndProc = GuardedWndProc<NameProc>; nc.hInstance = g_hInst; nc.lpszClassName = L"WasapiBridgeName";
        nc.hCursor = LoadCursor(nullptr, IDC_ARROW); nc.hbrBackground = nullptr;
        RegisterClassW(&nc);
        WNDCLASSW rcw = nc; rcw.lpfnWndProc = GuardedWndProc<RouteProc>; rcw.lpszClassName = L"WasapiBridgeRoute";
        RegisterClassW(&rcw);
        WNDCLASSW sc = {};                                 // 0.8.1: intensity slider
        sc.lpfnWndProc = GuardedWndProc<SliderProc>; sc.hInstance = g_hInst; sc.lpszClassName = L"WasapiBridgeSlider";
        sc.hCursor = LoadCursor(nullptr, IDC_HAND);
        RegisterClassW(&sc);
        WNDCLASSW tw = {};                                 // 0.9.2: help tip popup
        tw.style = CS_DROPSHADOW; tw.lpfnWndProc = GuardedWndProc<TipProc>; tw.hInstance = g_hInst; tw.lpszClassName = L"WasapiBridgeTip";
        tw.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&tw);
        WNDCLASSW mw = nc; mw.lpfnWndProc = GuardedWndProc<MonitorProc>; mw.lpszClassName = L"WasapiBridgeMonitor";
        mw.hIcon = AppIcon(GetSystemMetrics(SM_CXICON));
        RegisterClassW(&mw);
        registered = true;
    }
}

static PanelState* g_openPanel = nullptr;      // H: the open panel (one at a time; a second request focuses it)

// Entry point from IASIO::controlPanel (host thread), also used by the monitor window's "Settings" button.
// Registers the window classes, picks the owner window and DPI, creates the panel and runs its modal loop
// (Ctrl+Tab handling, then IsDialogMessage); afterwards it re-enables the owner. Only one panel at a time: a
// second call brings the open one to the front. A WM_QUIT received during the loop is re-posted.
void ShowControlPanel(WasapiBridge* drv) {
    RegisterPanelClasses();
    if (g_openPanel && g_openPanel->wnd && IsWindow(g_openPanel->wnd)) {   // already open (e.g. from the monitor window)
        ShowWindow(g_openPanel->wnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_openPanel->wnd);
        return;
    }
    // 0.9.0: theme and language of the saved configuration (the panel may preview others until it closes)
    ThemeSelect(drv->config().theme);
    SetLang(Resolve(drv->config().language, GetUserDefaultUILanguage()));
    ThemeInit();
    drv->AddRef();                       // 0.7.1 (B): the panel's own reference (moved on rebind, released at the end)
    PanelState st; st.drv = drv; st.generation = drv->generation();
    // 0.7.1: the panel is an UNOWNED top-level window. The window that was active when it was opened (PickOwner) is
    // only used to choose the DPI and the monitor to center on, and to give the focus back at the end. No host
    // window is ever disabled or used as owner, so closing Cubase's Studio Setup (which destroys its windows) can
    // no longer destroy the panel. The z-order is kept by the topmost policy (KeepWithHost). st.owner stays null,
    // so the old owner-detach code is inert.
    HWND owner = PickOwner(drv->hostWindow());
    st.owner = nullptr;
    // DPI of the host window (GetDpiForWindow, Windows 10 1607+, loaded at runtime) or the system DPI
    HDC dc = GetDC(nullptr); st.dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_GetDpiForWindow dpiFn = u32 ? (PFN_GetDpiForWindow)(void*)GetProcAddress(u32, "GetDpiForWindow") : nullptr;
    if (dpiFn && owner) { UINT d = dpiFn(owner); if (d >= 48 && d <= 960) st.dpi = (int)d; }
    HWND ref = owner ? owner : GetForegroundWindow();
    RECT wa = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(ref, MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    CreateUiResources(&st);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, exStyle = WS_EX_APPWINDOW;   // 0.9.3: no modal frame (it hid the icon)
    // Logical height 700; reduced on short screens (minimum 640 since 0.8.0): the meter and status cards shrink.
    // 0.7.1 (C): if even the minimum logical height do not fit the work area at this DPI (e.g. 1366x768 at 150 %), the
    // layout scale is reduced until the whole window, bottom bar included, fits: the bottom bar ("Apply",
    // "Close") is never cut off.
    RECT rc = WindowRectFor(S(&st, st.cw), S(&st, st.ch), style, exStyle, st.dpi);
    int over = (rc.bottom - rc.top) - (wa.bottom - wa.top);
    if (over > 0) {
        st.ch -= MulDiv(over, 96, st.dpi) + 1;
        if (st.ch < 700) st.ch = 700;      // 0.8.1: the About tab (scroller strip + Visuals row) needs 700; the DPI loop below shrinks the scale instead
        rc = WindowRectFor(S(&st, st.cw), S(&st, st.ch), style, exStyle, st.dpi);
    }
    for (int guard = 0; guard < 40 && st.dpi > 72; ++guard) {
        over = (rc.bottom - rc.top) - (wa.bottom - wa.top);
        const int overW = (rc.right - rc.left) - (wa.right - wa.left);
        if (over <= 0 && overW <= 0) break;
        st.dpi -= 4;
        FreeUiResources(&st); CreateUiResources(&st);
        rc = WindowRectFor(S(&st, st.cw), S(&st, st.ch), style, exStyle, st.dpi);
    }
    std::wstring title = std::wstring(WB_NAME_W L" ") + WB_VERSION_W + L" - " + T(S_TITLE_PANEL);
    // WS_EX_APPWINDOW: taskbar button and Alt+Tab entry (recoverable if it ends up behind); no owner (0.7.1)
    HWND w = CreateWindowExW(exStyle, L"WasapiBridgePanel", title.c_str(), style,
                             CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, g_hInst, &st);
    auto freeRes = [&]() { FreeUiResources(&st); ThemeFree(); };
    if (!w) { freeRes(); st.drv->Release(); return; }
    DarkTitle(w);
    SetWindowIcons(w, st.dpi);             // 0.9.3: the application icon at the window's DPI
    RECT wr; GetWindowRect(w, &wr);
    int px = wa.left + ((wa.right - wa.left) - (wr.right - wr.left)) / 2;
    int py = wa.top + ((wa.bottom - wa.top) - (wr.bottom - wr.top)) / 2;
    if (py < wa.top) py = wa.top;
    if (px < wa.left) px = wa.left;
    SetWindowPos(w, nullptr, px, py, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    // 0.7.1: the host windows stay enabled. The loop below is kept (it provides Tab / Ctrl+Tab navigation with
    // IsDialogMessage and returns from controlPanel only when the panel closes, as hosts expect); the host's own
    // windows keep working because this loop dispatches every message of the thread.
    g_openPanel = &st;
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwPanel, "PanelOpen", wbtrace::Bool("SafeMode", st.drv->safeMode()));
    MSG msg = {};
    BOOL gm = 1;
    // 0.9.8 (exception barrier): `st` lives in this frame and the window holds a pointer to it, so an exception must
    // never leave this function between here and the cleanup below (it would leave g_openPanel and the window's
    // data pointing into a dead stack frame). Window procedures have their own barriers; this one covers the update
    // check and the loop itself. On an exception the panel closes through the normal path.
    try {
        // 0.9.0 (H3): weekly check, only if the user turned it on (off by default; never on the first run)
        {
            const Config& c = st.drv->config();
            const long long now = (long long)time(nullptr);
            if (c.updateCheck && !upd::IsPlaceholderRepo(UpdateRepoOf(c)) && now - c.updateLastCheck >= 7LL * 86400) UpdStart(&st, false);
        }
        while (!st.closed && (gm = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            if (HandleTabKeys(&st, msg)) continue;
            if (!IsDialogMessageW(w, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
    } catch (...) {
        NoteException("the control panel loop");
        gm = 1;                                    // not a WM_QUIT
    }
    g_openPanel = nullptr;
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwPanel, "PanelClose", wbtrace::Bool("HostQuit", !st.closed && gm == 0));
    const bool quit = !st.closed && gm == 0;          // WM_QUIT from the host: close the panel and re-post it
    if (!st.closed) DestroyWindow(w);
    if (owner && IsWindow(owner) && IsWindowVisible(owner)) SetForegroundWindow(owner);   // focus back to the host
    freeRes();
    // 0.9.0: back to the saved theme and language (a preview that was not applied ends with the panel).
    // 0.9.1 (fix 12, verified): this runs on every way out of the loop (Close, the window's X, Esc, a host
    // WM_QUIT, the window destroyed from outside); config() is the SAVED configuration, so a previewed but not
    // applied theme is dropped here and every monitor window is re-themed.
    ThemeSelect(st.drv->config().theme);
    SetLang(Resolve(st.drv->config().language, GetUserDefaultUILanguage()));
    NotifyMonitorWindows();
    st.drv->Release();                   // 0.7.1 (B): the instance the panel ended up bound to (may be destroyed here)
    if (quit) { PostQuitMessage((int)msg.wParam); return; }
    // 0.7.0: nothing to apply here any more: "Apply" applies immediately (applyPanelChanges) while the panel is open.
}

// ---------------------------------------------------------------- floating monitor window (0.7.0, F2)
// A small tool window (no owner, WS_EX_TOOLWINDOW: no taskbar button, never disables or follows the DAW window)
// showing the running stream: meters (the same WasapiBridgeMeter child and ballistics as the panel, with its own
// PanelState and its own meter consumer so neither window steals the other's peaks), DSP load, dropouts,
// latencies and a one-line summary, plus Settings / Reset counters / Pin / Close.
// Threading: created, refreshed (WM_TIMER, ~30 Hz for meters, ~2 Hz for the text) and destroyed on the host
// thread; it only calls the lock-free readMeters / getStatus / resetCounters. The audio thread never touches it.
struct MonState {
    WasapiBridge* drv = nullptr;
    PanelState ps;                     // fonts, DPI, meter state and gradient (only the meter-related fields are used)
    HWND wnd = nullptr, set = nullptr, rst = nullptr, pin = nullptr, cls = nullptr, safeBtn = nullptr;
    StatusInfo st{}; bool stValid = false;
    int ticks = 0;
    bool topmost = false;              // "Pin" on
    bool topState = false;             // 0.7.1 (A): HWND_TOPMOST currently applied (only while our process is in front)
    // 0.8.1 (VisualsMonitor): background of the meter child at low intensity
    std::unique_ptr<vis::Renderer> vr;
    int visEff = 0; uint64_t visT0Us = 0; int visLevel = 0;
};

static int MS(MonState* m, int v) { return S(&m->ps, v); }

// Creates (or recreates after a DPI change) the child controls with the current fonts.
static void MonBuild(MonState* m) {
    HWND h = m->wnd;
    TipCancel(&m->ps);                      // 0.9.2
    AccClearChildren(h);
    HWND c = GetWindow(h, GW_CHILD);
    while (c) { HWND next = GetWindow(c, GW_HWNDNEXT); DestroyWindow(c); c = next; }
    m->ps.wnd = h;
    m->ps.meter = CreateWindowExW(0, L"WasapiBridgeMeter", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_METER, g_hInst, &m->ps);
    m->set = MakeButton(h, m->ps.font, T(S_MW_SETTINGS), 0, 0, 10, 10, ID_MW_SET, 1, g_th.bg);
    m->rst = MakeButton(h, m->ps.font, T(S_RESET_COUNTERS), 0, 0, 10, 10, ID_MW_RESET, 1, g_th.bg);
    m->pin = MakeButton(h, m->ps.font, T(S_MW_PIN), 0, 0, 10, 10, ID_MW_PIN, 3, g_th.bg);
    m->cls = MakeButton(h, m->ps.font, T(S_CLOSE), 0, 0, 10, 10, ID_MW_CLOSE, 1, g_th.bg);
    m->safeBtn = m->drv->safeMode() ? MakeButton(h, m->ps.font, T(S_USE_MY_SETTINGS), 0, 0, 10, 10, ID_MW_SAFE, 1, g_th.bg) : nullptr;
    if (m->pin) { SetWindowLongPtrW(m->pin, GWLP_USERDATA, m->topmost ? 1 : 0); }
    TipAnnotate(h, tips::kMonitor);         // 0.9.2
}

// Positions the children for the current client size.
static void MonLayout(MonState* m) {
    RECT rc; GetClientRect(m->wnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int top = MS(m, 48) + (m->safeBtn ? MS(m, 30) : 0);
    const int by = H - MS(m, 38), bh = MS(m, 30);
    if (m->ps.meter) MoveWindow(m->ps.meter, MS(m, 8), top, W - MS(m, 16), by - top - MS(m, 6) > 10 ? by - top - MS(m, 6) : 10, TRUE);
    int x = MS(m, 8);
    auto place = [&](HWND b, int w) { if (b) { MoveWindow(b, x, by, MS(m, w), bh, TRUE); x += MS(m, w) + MS(m, 6); } };
    place(m->set, 84); place(m->rst, 120); place(m->pin, 64); place(m->cls, 70);
    if (m->safeBtn) MoveWindow(m->safeBtn, W - MS(m, 8) - MS(m, 180), MS(m, 46), MS(m, 180), MS(m, 26), TRUE);
}

// Every handler checks m and m->drv (fix 1): DestroyMonitorWindow clears both before destroying the window, so a
// timer or a message that is still in flight never reaches a driver that is going away.
static LRESULT CALLBACK MonitorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MonState* m = (MonState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg != WM_NCCREATE && m && !m->drv) m = nullptr;
    switch (msg) {
    case WM_NCCREATE:
        m = (MonState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)m);
        m->wnd = h;
        break;
    case WM_CREATE:
        if (!m) return -1;
        MonBuild(m); MonLayout(m);
        SetTimer(h, ID_MW_TIMER, 33, nullptr);              // ~30 Hz
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if (m) { MonLayout(m); InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_GETMINMAXINFO:
        if (m) { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = MS(m, 360); mm->ptMinTrackSize.y = MS(m, 220); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        FillRectC(mem, 0, 0, rc.right, rc.bottom, g_th.bg);
        if (m) {
            wchar_t l1[256], l2[256];
            const StatusInfo& s = m->st;
            if (m->stValid && s.streamsOpen) {
                swprintf(l1, 256, T(S_MW_L1),
                         s.outBluetooth ? L"Bluetooth  \x00B7  " : L"",           // 0.8.0 (G1) badge
                         s.outExclusive ? T(S_EXCLUSIVE) : s.outLowLatency ? T(S_SHARED_LL) : T(S_SHARED),
                         s.rate, s.bufSize, s.outName.c_str(), s.hasInput ? L"  /  " : L"", s.hasInput ? s.inName.c_str() : L"");
                wchar_t gr[40] = L"";
                if (m->ps.snap.limOn) swprintf(gr, 40, T(S_MW_LIM), m->ps.grDisp >= 0.05f ? L"-" : L"", m->ps.grDisp);   // G2
                swprintf(l2, 256, T(S_MW_L2),
                         1000.0 * s.inLat / s.rate, 1000.0 * s.outLat / s.rate, s.loadNow, s.loadMax, s.outGlitches,
                         s.inDropouts, s.dmActive ? T(S_MW_MONITORING) : L"", gr);
            } else {
                swprintf(l1, 256, T(S_NO_STREAM)); l2[0] = 0;
            }
            RECT r1 = {MS(m, 10), MS(m, 4), rc.right - MS(m, 10), MS(m, 24)};
            RECT r2 = {MS(m, 10), MS(m, 24), rc.right - MS(m, 10), MS(m, 44)};
            TextC(mem, m->ps.fontB, g_th.text, l1, r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            const bool hot = m->stValid && (s.loadMax > 100.f || s.outGlitches > 0 || s.inDropouts > 0);
            TextC(mem, m->ps.font, hot ? g_th.amber : g_th.dim, l2, r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (m->safeBtn) {
                RECT r3 = {MS(m, 10), MS(m, 46), rc.right - MS(m, 196), MS(m, 72)};
                TextC(mem, m->ps.font, g_th.amber, T(S_SAFE_BANNER), r3,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        if (m) { DrawOwnerButton((const DRAWITEMSTRUCT*)lp, m->ps.dpi); return TRUE; }
        break;
    case WM_SETCURSOR:                      // 0.9.2: help tips
        if (m) TipOnSetCursor(&m->ps, h, tips::kMonitor, (HWND)wp, HIWORD(lp));
        break;
    case a11y::kMsgDescription: return TipDescribe(tips::kMonitor, h, (HWND)wp, (wchar_t*)lp);
    case WM_HELP:
        if (m) TipForControl(&m->ps, h, tips::kMonitor, (HWND)((HELPINFO*)lp)->hItemHandle);
        return TRUE;
    case WM_TIMER:
        if (m && wp == ID_TIPTIMER) { TipTick(&m->ps); return 0; }
        if (m && wp == ID_MW_TIMER) {
            if (m->ticks % 15 == 0) {                        // 0.8.1: settings checked twice a second
                const Config& c = m->drv->config();
                m->visEff = c.visualsMonitor && VisAnimationsAllowed() ? VisualsEffective(c.visuals, c.visualsExplicit) : vis::kOff;
                if (m->visEff == vis::kOff && m->vr) { m->ps.meterBg = nullptr; m->vr.reset(); }
            }
            if (m->visEff != vis::kOff && m->ps.meter && !IsIconic(h)) {
                if (!m->vr) { m->vr.reset(new vis::Renderer); m->vr->setBackground(ToRgb(g_th.card)); m->vr->setPalette(g_th.fx[0], g_th.fx[1], g_th.fx[2]); m->visT0Us = VisNowUs(); }
                RECT mr; GetClientRect(m->ps.meter, &mr);
                int lw, lh; VisSizeFor(mr.right, mr.bottom, 3, lw, lh); m->vr->resize(lw, lh);
                float pk = 0.f;
                for (int c = 0; c < kMaxCh; ++c) if (m->ps.snap.out[c] > pk) pk = m->ps.snap.out[c];
                const int lvl = pk >= 1.f ? 255 : (int)(pk * 255.f);
                m->visLevel = lvl > m->visLevel ? lvl : m->visLevel * 7 / 8;
                m->vr->frame((vis::Effect)m->visEff, VisNowUs() - m->visT0Us, m->drv->config().visualsIntensity * 30 / 100, m->visLevel,
                             m->drv->config().visualsSpeed);
                m->ps.meterBg = m->vr.get();
            }
            UpdateMeters(&m->ps);                            // lock-free peaks (own consumer slot) + ballistics
            if (m->ticks % 8 == 0) ApplyTopmostPolicy(h, m->topState, m->topmost);   // 0.7.1 (A), ~250 ms
            if (m->ticks++ % 15 == 0) {                      // ~2 Hz text refresh
                m->drv->getStatus(m->st); m->stValid = true;
                RECT r = {0, 0, 10000, MS(m, 46)}; InvalidateRect(h, &r, FALSE);
            }
        }
        return 0;
    case WM_COMMAND:
        if (!m) break;
        switch (LOWORD(wp)) {
        case ID_MW_SET: m->drv->openPanelFromMonitor(); return 0;    // nested modal loop: touch nothing afterwards
        case ID_MW_RESET:
            m->drv->resetCounters(WB_RESET_ALL); m->ps.snap.clipIn = m->ps.snap.clipOut = 0; ClearHolds(&m->ps);
            return 0;
        case ID_MW_PIN: {
            m->topmost = !m->topmost;
            SetToggle(m->pin, m->topmost);
            ApplyTopmostPolicy(h, m->topState, m->topmost);  // 0.7.1 (A): topmost only while the DAW is in front
            RECT wr; GetWindowRect(h, &wr);
            m->drv->setMonitorPlacement(wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, m->topmost);
            return 0;
        }
        case ID_MW_CLOSE: m->drv->monitorClosedByUser(); return 0;    // destroys this window: touch nothing afterwards
        case ID_MW_SAFE:
            m->drv->leaveSafeMode();
            if (m->safeBtn) { DestroyWindow(m->safeBtn); m->safeBtn = nullptr; }
            MonLayout(m); InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_EXITSIZEMOVE:
        if (m) { RECT wr; GetWindowRect(h, &wr); m->drv->setMonitorPlacement(wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, m->topmost); }
        return 0;
    case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE:     // 0.9.1 (B1): high contrast on/off while no panel is open
        if (m && !g_openPanel && (msg == WM_SYSCOLORCHANGE || wp == SPI_SETHIGHCONTRAST) && g_th.hc != HighContrastOn()) {
            ThemeSelect(g_th.id);
            PostMessageW(h, WM_WB_RETHEME, 0, 0);
        }
        break;
    case WM_WB_RETHEME:                     // 0.9.0: theme or language changed in the panel
        if (m) {
            TipCancel(&m->ps);                  // 0.9.2
            if (m->ps.grad) { DeleteObject(m->ps.grad); m->ps.grad = nullptr; m->ps.gradH = 0; }
            m->ps.meterBg = nullptr; m->vr.reset();          // recreated with the new colors at the next tick
            MonBuild(m); MonLayout(m); DarkTitle(h);
            std::wstring title = std::wstring(WB_NAME_W) + L" - " + T(S_TITLE_MONITOR);
            SetWindowTextW(h, title.c_str());
            InvalidateRect(h, nullptr, TRUE);
        }
        return 0;
    case WM_DPICHANGED:
        if (m) {
            FreeUiResources(&m->ps);
            m->ps.dpi = LOWORD(wp);
            SetWindowIcons(h, m->ps.dpi);   // 0.9.3
            CreateUiResources(&m->ps);
            MonBuild(m);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            MonLayout(m); InvalidateRect(h, nullptr, TRUE);
        }
        return 0;
    case WM_CLOSE: if (m) m->drv->monitorClosedByUser(); return 0;   // the title bar X = the Close button
    case WM_NCDESTROY: {
        KillTimer(h, ID_MW_TIMER);
        // Normally DestroyMonitorWindow already detached and frees the state; this covers any other destruction.
        MonState* own = (MonState*)GetWindowLongPtrW(h, GWLP_USERDATA);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (own) { TipDestroy(&own->ps); FreeUiResources(&own->ps); delete own; ThemeFree(); --g_objects; }
        return DefWindowProcW(h, msg, wp, lp);
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// H (F2). Creates the window without activating it (it must never steal the focus from the DAW). The saved
// position is used only if it is still on a visible monitor; otherwise the window goes to the top-right corner of
// the primary work area.
void ApplyUiPrefs(const Config& c) {
    if (g_openPanel) return;              // the open panel decides (it may be previewing)
    ThemeSelect(c.theme);
    SetLang(Resolve(c.language, GetUserDefaultUILanguage()));
}

HWND CreateMonitorWindow(WasapiBridge* drv) {
    RegisterPanelClasses();
    ApplyUiPrefs(drv->config());         // 0.9.0
    ThemeInit();
    MonState* m = new MonState;
    m->drv = drv;
    m->ps.drv = drv;
    m->ps.meterConsumer = WB_METERS_MONITOR;
    const Config& c = drv->config();
    m->topmost = c.monitorTopmost;
    HDC dc = GetDC(nullptr); m->ps.dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc);
    CreateUiResources(&m->ps);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
    const DWORD ex = WS_EX_TOOLWINDOW;     // 0.7.1 (A): topmost is applied by the policy timer when pinned
    RECT wr = WindowRectFor(MS(m, 480), MS(m, 300), style, ex, m->ps.dpi);
    int w = wr.right - wr.left, hgt = wr.bottom - wr.top, x = 0, y = 0;
    RECT want = {c.monitorX, c.monitorY, c.monitorX + c.monitorW, c.monitorY + c.monitorH};
    if (c.monitorW > 0 && c.monitorH > 0 && c.monitorX != INT_MIN && MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) {
        x = c.monitorX; y = c.monitorY; w = c.monitorW; hgt = c.monitorH;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.right - w - 24; y = wa.top + 24;
    }
    std::wstring title = std::wstring(WB_NAME_W) + L" - " + T(S_TITLE_MONITOR);
    HWND h = CreateWindowExW(ex, L"WasapiBridgeMonitor", title.c_str(), style, x, y, w, hgt, nullptr, nullptr, g_hInst, m);
    if (!h) { FreeUiResources(&m->ps); delete m; ThemeFree(); return nullptr; }
    ++g_objects;                           // fix 1: the window keeps the DLL loaded until it is gone
    DarkTitle(h);
    SetWindowIcons(h, m->ps.dpi);          // 0.9.3
    // Per-monitor DPI: if the window landed on a monitor with another DPI, rebuild at that DPI.
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow dpiFn = (PFN_GetDpiForWindow)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (dpiFn) {
        const UINT d = dpiFn(h);
        if (d >= 48 && d <= 960 && (int)d != m->ps.dpi) {
            FreeUiResources(&m->ps); m->ps.dpi = (int)d; CreateUiResources(&m->ps); MonBuild(m); MonLayout(m);
        }
    }
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

// H (F2). Accepts nullptr or an already destroyed window.
// Fix 1: detaches the state from the driver and from the window BEFORE destroying it (no message or timer can
// reach the driver any more), destroys the window, then frees the state. msgThread only.
void DestroyMonitorWindow(HWND w) {
    if (!w || !IsWindow(w)) return;
    MonState* m = (MonState*)GetWindowLongPtrW(w, GWLP_USERDATA);
    KillTimer(w, ID_MW_TIMER);
    if (m) TipDestroy(&m->ps);              // 0.9.2
    AccClearChildren(w);
    if (m) { m->drv = nullptr; m->ps.drv = nullptr; }
    SetWindowLongPtrW(w, GWLP_USERDATA, 0);
    DestroyWindow(w);                       // children (meter) are destroyed first; m->ps is still valid here
    if (m) { FreeUiResources(&m->ps); delete m; ThemeFree(); --g_objects; }
}

// H (F2). Applies the MonitorTopmost setting.
void SetMonitorTopmost(HWND w, bool on) {
    if (!w || !IsWindow(w)) return;
    if (MonState* m = (MonState*)GetWindowLongPtrW(w, GWLP_USERDATA)) {
        m->topmost = on;
        if (m->pin) SetToggle(m->pin, on);
        ApplyTopmostPolicy(w, m->topState, on);           // 0.7.1 (A)
    }
}

// ---------------------------------------------------------------- notification-area icon (0.8.0, G5)
// A hidden top-level window (WS_POPUP, never shown; a message-only window would not receive the TaskbarCreated
// broadcast) owns one Shell_NotifyIcon entry with NOTIFYICON_VERSION_4 semantics:
//   - tooltip: version, mode / rate / buffer, DSP load and dropouts; refreshed every second (NIM_MODIFY only when
//     something changed);
//   - icon (0.9.3): the application icon with a status dot in its bottom-right corner (src/trayicon.h), built at
//     run time for the tray's icon size (SM_CXSMICON; rebuilt when it changes or the display / theme settings
//     change), the four states cached, destroyed with the window: gray = no stream, green = running fine, amber =
//     DSP load above 80 %, red = dropouts in the last 10 s (held 10 s so short stutters are seen). The tooltip
//     ends with the state word;
//   - left click (NIN_SELECT / NIN_KEYSELECT): show or hide the monitor window; right click (WM_CONTEXTMENU): menu
//     with Open settings, Show monitor window, Reset counters, Copy status, About;
//   - TaskbarCreated (Explorer restarted): the icon is added again.
// Threading and lifetime: msgThread (= the host thread that called init) only. The driver creates it through
// WM_WB_TRAYCREATE and removes it in disposeBuffers / its destructor (DestroyTrayWindow: NIM_DELETE, then
// DestroyWindow) on the same thread, so the icon never outlives the driver object. Like the monitor window,
// TrayState::drv is cleared before the window is destroyed and every handler checks it. While a nested modal loop
// runs (the context menu, the About box), g_objects is held so the DLL cannot be unloaded under it, and the state
// is looked up again afterwards because the driver may have destroyed the window meanwhile.
enum { WM_WB_TRAYCB = WM_APP + 40 };
struct TrayState {
    WasapiBridge* drv = nullptr;
    HWND wnd = nullptr;
    HICON icons[tray::kStateCount] = {};   // gray, green, amber, red (tray::State order)
    int iconSize = 0;                  // 0.9.3: pixel size the icons were built for
    int shown = -1;                    // index of the icon shown
    uint32_t lastDrops = 0;            // 0.9.3: dropout counters at the previous refresh
    uint64_t lastDropMs = 0;           // 0.9.3: GetTickCount64 of their last increase (0 = never)
    std::wstring tip;                  // tooltip shown
    bool added = false;
};
static UINT g_msgTaskbarCreated = 0;

// 0.9.3: one tray icon = the application icon at `size` plus the status dot of `state`. The icon's own 32-bit
// color bitmap (straight alpha) is read with GetIconInfo + GetDIBits; an image without alpha takes it from the
// mask. Every GDI object is released on every path.
static HICON MakeTrayIcon(int size, int state) {
    std::vector<uint32_t> px((size_t)size * size, 0);
    HICON base = AppIcon(size);
    ICONINFO bi = {};
    bool ok = false;
    if (base && GetIconInfo(base, &bi)) {
        BITMAPINFO h = {};
        h.bmiHeader.biSize = sizeof(h.bmiHeader); h.bmiHeader.biWidth = size; h.bmiHeader.biHeight = -size;
        h.bmiHeader.biPlanes = 1; h.bmiHeader.biBitCount = 32; h.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        if (bi.hbmColor && GetDIBits(dc, bi.hbmColor, 0, (UINT)size, px.data(), &h, DIB_RGB_COLORS) == size) {
            bool alpha = false;
            for (uint32_t v : px) if (v >> 24) { alpha = true; break; }
            if (!alpha && bi.hbmMask) {                               // old-style icon: alpha from the AND mask
                std::vector<uint32_t> m((size_t)size * size, 0);
                if (GetDIBits(dc, bi.hbmMask, 0, (UINT)size, m.data(), &h, DIB_RGB_COLORS) == size)
                    for (size_t i = 0; i < px.size(); ++i) px[i] |= (m[i] & 0xFFFFFF) ? 0u : 0xFF000000u;
            }
            ok = true;
        }
        ReleaseDC(nullptr, dc);
        if (bi.hbmColor) DeleteObject(bi.hbmColor);
        if (bi.hbmMask) DeleteObject(bi.hbmMask);
    }
    if (!ok) std::fill(px.begin(), px.end(), 0u);                     // still a usable dot on its own
    tray::AddStatusDot(px.data(), size, tray::kColor[state]);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader); bmi.bmiHeader.biWidth = size; bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) { if (color) DeleteObject(color); return nullptr; }
    memcpy(bits, px.data(), px.size() * 4);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);   // ignored when the color bitmap has alpha
    ICONINFO ii = {}; ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ic = mask ? CreateIconIndirect(&ii) : nullptr;
    DeleteObject(color); if (mask) DeleteObject(mask);
    return ic;
}
static void TrayFreeIcons(TrayState* t) { for (HICON& ic : t->icons) if (ic) { DestroyIcon(ic); ic = nullptr; } t->iconSize = 0; }
// (Re)builds the four icons when the tray icon size changed (or always with force). The old set is destroyed only
// after the caller showed a new one (it is returned through `old`).
static void TrayBuildIcons(TrayState* t, bool force, HICON (&old)[tray::kStateCount]) {
    const int sz = GetSystemMetrics(SM_CXSMICON) > 0 ? GetSystemMetrics(SM_CXSMICON) : 16;
    for (HICON& o : old) o = nullptr;
    if (!force && sz == t->iconSize && t->icons[0]) return;
    for (int i = 0; i < tray::kStateCount; ++i) { old[i] = t->icons[i]; t->icons[i] = MakeTrayIcon(sz, i); }
    t->iconSize = sz;
}

// Adds (first time or after TaskbarCreated) or updates the icon. force: also when nothing changed.
static void TrayRefresh(TrayState* t, bool force, bool rebuild = false) {
    if (!t || !t->drv) return;
    HICON old[tray::kStateCount];
    TrayBuildIcons(t, rebuild, old);
    if (old[0]) force = true;                                       // new icon set: show it
    StatusInfo s{};
    t->drv->getStatus(s);
    const uint32_t drops = s.outGlitches + s.timeouts + s.inDropouts;
    const uint64_t now = GetTickCount64();
    if (drops > t->lastDrops) t->lastDropMs = now;                  // 0.9.3: a new dropout (red for 10 s)
    t->lastDrops = drops;                                           // also after "Reset counters" (lower)
    const int st = tray::Pick(s.streamsOpen, s.loadNow, now, t->lastDropMs);
    wchar_t tip[128];
    if (s.streamsOpen)
        swprintf(tip, 128, T(S_TRAY_TIP),
                 WB_NAME_W, WB_VERSION_W, s.outBluetooth ? L"Bluetooth, " : L"",
                 s.outExclusive ? T(S_EXCLUSIVE) : T(S_SHARED), s.rate, s.bufSize, s.loadNow, s.loadMax, drops);
    else swprintf(tip, 128, T(S_TRAY_TIP_NOSTREAM), WB_NAME_W, WB_VERSION_W);
    // 0.9.3: the state word at the end; the rest is shortened if the 127 characters of the tooltip do not suffice
    static const int kWord[tray::kStateCount] = {S_TRAY_ST_STOPPED, S_TRAY_ST_OK, S_TRAY_ST_LOAD, S_TRAY_ST_DROPS};
    const std::wstring word = std::wstring(L"\n") + T(kWord[st]);
    std::wstring full = tip;
    if (full.size() + word.size() > 127) full.resize(127 > word.size() ? 127 - word.size() : 0);
    full += word;
    if (!force && t->added && st == t->shown && t->tip == full) return;
    NOTIFYICONDATAW nd = {};
    nd.cbSize = sizeof(nd); nd.hWnd = t->wnd; nd.uID = 1;
    nd.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nd.uCallbackMessage = WM_WB_TRAYCB;
    nd.hIcon = t->icons[st] ? t->icons[st] : AppIcon(t->iconSize > 0 ? t->iconSize : 16);
    wcsncpy(nd.szTip, full.c_str(), ARRAYSIZE(nd.szTip) - 1);
    if (!t->added) {
        if (Shell_NotifyIconW(NIM_ADD, &nd)) {
            t->added = true;
            nd.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nd);
        }
    } else if (!Shell_NotifyIconW(NIM_MODIFY, &nd)) {
        t->added = false;                // Explorer lost it: added again at the next refresh
    }
    for (HICON& o : old) if (o) DestroyIcon(o);                    // the shell copied the new icon
    t->shown = st; t->tip = full;
}

// Right-click menu. Returns after the chosen command ran; the caller must not touch `t` afterwards.
static void TrayMenu(HWND w, TrayState* t, int x, int y) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_TR_SETTINGS, T(S_TR_OPEN));
    AppendMenuW(m, MF_STRING | (t->drv->monitorShown() ? MF_CHECKED : MF_UNCHECKED), IDM_TR_MONITOR, T(S_TR_SHOWMON));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TR_RESET, T(S_RESET_COUNTERS));
    AppendMenuW(m, MF_STRING, IDM_TR_COPY, T(S_COPY_STATUS));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_TR_ABOUT, T(S_TAB_ABOUT));
    SetMenuDefaultItem(m, IDM_TR_SETTINGS, FALSE);
    SetForegroundWindow(w);              // required, or the menu does not close when clicking elsewhere
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    ++g_objects;                         // modal loop: keep the DLL loaded even if the driver goes away meanwhile
    const UINT cmd = (UINT)TrackPopupMenuEx(m, flags, x, y, w, nullptr);
    PostMessageW(w, WM_NULL, 0, 0);
    DestroyMenu(m);
    --g_objects;
    t = IsWindow(w) ? (TrayState*)GetWindowLongPtrW(w, GWLP_USERDATA) : nullptr;   // destroyed during the menu?
    if (!t || !t->drv || !cmd) return;
    WasapiBridge* d = t->drv;
    switch (cmd) {
    case IDM_TR_SETTINGS: d->openPanelFromMonitor(); return;      // nested loop: touch nothing afterwards
    case IDM_TR_MONITOR: d->trayToggleMonitor(); return;
    case IDM_TR_RESET: d->resetCounters(WB_RESET_ALL); TrayRefresh(t, true); return;
    case IDM_TR_COPY: {
        std::wstring text = SanitizeText(EnglishStatus(d, nullptr));   // English; no user folders, user or computer name
        if (!CopyToClipboard(w, text)) MessageBeep(MB_ICONWARNING);
        return;
    }
    case IDM_TR_ABOUT: {
        std::wstring about = std::wstring(WB_NAME_W L" ") + WB_VERSION_W + L"\r\n\r\n" + T(S_TR_ABOUT) +
            L"\r\n\r\nASIO is a registered trademark of Steinberg Media Technologies GmbH.\r\n" + T(S_ABOUT_TM_NOTE);
        ++g_objects;                     // modal loop (see above); the driver is not touched afterwards
        AppMessageBox(nullptr, about.c_str(), WB_NAME_W, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        --g_objects;
        return;
    }
    }
}

static LRESULT CALLBACK TrayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    TrayState* t = (TrayState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg != WM_NCCREATE && t && !t->drv) t = nullptr;
    if (msg == WM_NCCREATE) {
        t = (TrayState*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)t);
        t->wnd = h;
        return DefWindowProcW(h, msg, wp, lp);
    }
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {        // Explorer restarted: add the icon again
        if (t) { t->added = false; TrayRefresh(t, true); }
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (t && wp == ID_TRAY_TIMER) TrayRefresh(t, false);
        return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED: case WM_SETTINGCHANGE: case WM_THEMECHANGED:   // 0.9.3: rebuild
        if (t) TrayRefresh(t, true, true);
        break;
    case WM_WB_TRAYCB:
        if (!t) return 0;
        switch (LOWORD(lp)) {            // NOTIFYICON_VERSION_4: event in LOWORD(lParam), anchor point in wParam
        case NIN_SELECT: case NIN_KEYSELECT: t->drv->trayToggleMonitor(); return 0;
        case WM_CONTEXTMENU: TrayMenu(h, t, GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); return 0;   // touch nothing afterwards
        }
        return 0;
    case WM_NCDESTROY: {
        // Normally DestroyTrayWindow already detached and freed the state; this covers any other destruction.
        KillTimer(h, ID_TRAY_TIMER);
        TrayState* own = (TrayState*)GetWindowLongPtrW(h, GWLP_USERDATA);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (own) {
            if (own->added) { NOTIFYICONDATAW nd = {}; nd.cbSize = sizeof(nd); nd.hWnd = h; nd.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nd); }
            TrayFreeIcons(own);
            delete own; --g_objects;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// msgThread. Returns nullptr on failure (no tray icon; nothing else depends on it).
HWND CreateTrayWindow(WasapiBridge* drv) {
    ApplyUiPrefs(drv->config());         // 0.9.0: tooltip and menu language
    static bool cls = false;
    if (!cls) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = GuardedWndProc<TrayProc>; wc.hInstance = g_hInst; wc.lpszClassName = L"WasapiBridgeTray";
        cls = RegisterClassW(&wc) != 0;
        if (!cls) return nullptr;
    }
    if (!g_msgTaskbarCreated) g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayState* t = new TrayState;
    t->drv = drv;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"WasapiBridgeTray", WB_NAME_W, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_hInst, t);
    if (!h) { delete t; return nullptr; }
    ++g_objects;                         // the window keeps the DLL loaded until it is gone
    // An elevated host would otherwise not receive TaskbarCreated from a non-elevated Explorer (UIPI).
    if (g_msgTaskbarCreated) ChangeWindowMessageFilterEx(h, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    TrayRefresh(t, true);                // builds the icons for the tray's size (0.9.3)
    SetTimer(h, ID_TRAY_TIMER, 1000, nullptr);
    return h;
}

// msgThread. Removes the icon, then destroys the window and frees the state. Accepts nullptr.
void DestroyTrayWindow(HWND w) {
    if (!w || !IsWindow(w)) return;
    TrayState* t = (TrayState*)GetWindowLongPtrW(w, GWLP_USERDATA);
    KillTimer(w, ID_TRAY_TIMER);
    if (t) {
        t->drv = nullptr;
        if (t->added) { NOTIFYICONDATAW nd = {}; nd.cbSize = sizeof(nd); nd.hWnd = w; nd.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nd); t->added = false; }
    }
    SetWindowLongPtrW(w, GWLP_USERDATA, 0);
    DestroyWindow(w);
    if (t) { TrayFreeIcons(t); delete t; --g_objects; }
}
