// WASAPI Driver for ASIO - diagnostics tool (console)
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
//   WasapiDriverDiag.exe collects what is needed to troubleshoot an installation: Windows version and CPU,
//   driver registration in both registry views (64-bit and WOW6432Node), leftovers of the old name (WasapiBridge),
//   every active audio endpoint with its mix format, periods, IAudioClient3 engine periods, Exclusive format
//   support and hardware volume, the default devices, MMCSS status, config.ini and the last 200 lines of log.txt.
//
// Output
//   Writes WasapiDriver-diagnostics.txt (UTF-8 with BOM) next to the executable, or in
//   %APPDATA%\WASAPI Driver for ASIO, or in %TEMP%, and prints the same text to the console.
//
// Safety
//   Read-only: it never changes the registry, the configuration or any device, and uses no network. The report
//   contains Windows and driver data only (device names, formats, settings); the user is asked to review it before
//   sending it. 0.6.1: the whole report goes through SanitizeText (user folders as %APPDATA% etc., no user or
//   computer name) and endpoint Ids / ContainerIds are printed as short hashes unless --full-ids is given.
//
// Threading
//   Single-threaded console program. wmain initializes COM as multithreaded (CoInitializeEx with
//   COINIT_MULTITHREADED; 0.9.6, D8: this comment used to say apartment-threaded). That is fine here: the tool only
//   uses MMDevice / IAudioClient / IAudioEndpointVolume (usable from any apartment) and never loads the driver DLL
//   (the driver is only inspected through the registry and its files), so no apartment rules of ASIO apply.
//
// Order of the report (wmain)
//   header (versions, date, privacy note) -> System -> Driver registration (both views) -> Leftovers of the old
//   name -> render / capture endpoints -> default devices -> MMCSS -> microphone privacy -> crash reports ->
//   config.ini -> log.txt (tail) -> End of report. Sections that need COM run between CoInitializeEx and
//   CoUninitialize; the registry sections run before COM is initialized.
//
// How the report is built
//   Every line goes into the global g_out (P, RawLine, Section). Nothing is printed until the end: the whole text
//   is passed once through SanitizeText (privacy.h) and only then printed and saved, so no unsanitized line ever
//   reaches the console or the file. File contents (config.ini, log.txt) go through RawLine, never through a
//   printf format, so a '%' inside a user file cannot be misread as a format specifier.
//
// Pitfalls
//   - Nothing here opens an audio stream: Exclusive support is probed with IsFormatSupported only, so the tool can
//     run while a DAW holds the device (the matrix may then show "err" cells instead of yes/no).
//   - Registry reads take an explicit view (KEY_WOW64_64KEY / KEY_WOW64_32KEY), so the result does not depend on
//     the bitness of this executable.
#include "common.h"
#include "privacy.h"
#include "conicon.h"
#include "appdir.h"      // WbSettingsBase: the same settings folder as the driver (0.9.6, R7d)
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <avrt.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

// CLSID of the driver's COM class (the same value that dll.cpp registers and that HKLM\SOFTWARE\ASIO\<name>\CLSID
// must contain). The other GUIDs / PROPERTYKEYs are spelled out locally with the values of the Windows SDK:
// CLSID_MMDeviceEnumerator, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM, PKEY_Device_FriendlyName (pid 14) and
// PKEY_Device_ContainerId (pid 2).
static const wchar_t* kClsidStr = L"{A63175BD-B872-4FA5-9595-464F0236E1D0}";
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID  kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID  kSubtypePcm   = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const PROPERTYKEY kPkeyContainerId  = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

// Report text (UTF-16, CRLF line ends). Written only by the main thread; sanitized once at the end of wmain.
static std::wstring g_out;   // full report
static bool g_fullIds = false;   // --full-ids: print endpoint Ids and ContainerIds unmasked (0.6.1)

// Appends one formatted line (printf-style) to the report.
// A formatted line longer than 4095 characters is truncated (vswprintf returns -1; the buffer is then terminated).
static void P(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list a; va_start(a, fmt); int n = vswprintf(buf, 4096, fmt, a); va_end(a);
    if (n < 0) buf[4095] = 0;           // truncated or format error: keep what was written
    g_out += buf; g_out += L"\r\n";
}

// A line copied from a file (config.ini, log.txt): appended verbatim, without formatting, limited to 2000 characters.
static void RawLine(const std::wstring& line) {
    g_out += L"  ";
    if (line.size() > 2000) { g_out.append(line, 0, 2000); g_out += L" [...]"; }
    else g_out += line;
    g_out += L"\r\n";
}
// Section header: an empty line followed by "== title ==". The title is used as a format string (P(L"== %ls ==")
// passes it as an argument, so a '%' in it is printed literally).
static void Section(const wchar_t* t) { P(L""); P(L"== %ls ==", t); }

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// GUID in registry format {XXXXXXXX-XXXX-...}.
static std::wstring GuidStr(const GUID& g) {
    wchar_t b[64];
    swprintf(b, 64, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

// Converts bytes in code page cp to UTF-16.
static std::wstring FromBytes(const std::string& s, UINT cp) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------------------------------------------------------- registry
// Reads a REG_SZ / REG_DWORD value from root\sub in the given registry view. Returns false if absent.
// Note: only REG_SZ and REG_EXPAND_SZ are accepted here (REG_EXPAND_SZ is returned unexpanded); DWORD values are
// read by RegDword below. The buffer keeps room for one extra terminator because registry strings are not
// guaranteed to be NUL-terminated. Values longer than 1023 characters fail (ERROR_MORE_DATA) and count as absent.
static bool RegStr(HKEY root, const wchar_t* sub, const wchar_t* name, REGSAM view, std::wstring& out) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024]; DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    LONG r = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    buf[sz / sizeof(wchar_t)] = 0;
    out = buf;
    return true;
}

// Reads a REG_DWORD value from root\sub in the given registry view. Returns false if absent or of another type.
static bool RegDword(HKEY root, const wchar_t* sub, const wchar_t* name, REGSAM view, DWORD& out) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return false;
    DWORD sz = sizeof(out), type = 0;
    LONG r = RegQueryValueExW(k, name, nullptr, &type, (BYTE*)&out, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}

// Windows version (RtlGetVersion, not subject to manifest lies), product, CPU, native architecture and memory.
// The 19041 warning mirrors the driver's minimum supported Windows version. ProductName alone is not reliable
// (Windows 11 still stores "Windows 10" there); the build number is the value to look at.
static void ReportOs() {
    Section(L"System");
    typedef LONG (WINAPI* PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW v = {}; v.dwOSVersionInfoSize = sizeof(v);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PFN_RtlGetVersion fn = nt ? (PFN_RtlGetVersion)(void*)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (fn && fn(&v) == 0) P(L"Windows: %lu.%lu build %lu (product type %u)", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, (unsigned)v.wProductType);
    else P(L"Windows: RtlGetVersion not available");
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring s; DWORD ubr = 0;
    if (RegStr(HKEY_LOCAL_MACHINE, cv, L"ProductName", KEY_WOW64_64KEY, s)) P(L"Product: %ls", s.c_str());
    if (RegStr(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion", KEY_WOW64_64KEY, s)) P(L"Version: %ls", s.c_str());
    if (RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR", KEY_WOW64_64KEY, ubr)) P(L"Revision (UBR): %lu", ubr);
    if (fn && v.dwBuildNumber < 19041) P(L"WARNING: " WB_NAME_W L" requires Windows 10 2004 (build 19041) or later");

    SYSTEM_INFO si = {}; GetNativeSystemInfo(&si);
    const wchar_t* arch = L"unknown";
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = L"x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = L"ARM64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = L"x86"; break;
    default: break;
    }
    // On ARM64 an emulated x64 process sees x64 in GetNativeSystemInfo: IsWow64Process2 gives the real native machine.
    typedef BOOL (WINAPI* PFN_IsWow64Process2)(HANDLE, USHORT*, USHORT*);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_IsWow64Process2 w2 = k32 ? (PFN_IsWow64Process2)(void*)GetProcAddress(k32, "IsWow64Process2") : nullptr;
    USHORT pm = 0, nm = 0;
    if (w2 && w2(GetCurrentProcess(), &pm, &nm)) {
        if (nm == 0xAA64) arch = L"ARM64";
        else if (nm == 0x8664) arch = L"x64";
    }
    P(L"Native architecture: %ls   Logical processors: %lu", arch, si.dwNumberOfProcessors);
    if (RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", KEY_WOW64_64KEY, s))
        P(L"CPU: %ls", s.c_str());
    MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) P(L"Memory: %.1f GB", ms.ullTotalPhys / 1073741824.0);
}

// Driver registration in one registry view: ASIO key, CLSID match, InprocServer32 path and whether the DLL file
// exists, plus every other ASIO driver registered in that view.
// Reading the output: a 64-bit host only sees the 64-bit view, a 32-bit host only WOW6432Node. "NOT registered" in
// the view of the host's bitness means the host cannot list the driver; "DIFFERENT from expected" means the ASIO
// key points to another CLSID (stale or foreign registration); "DOES NOT EXIST" means the host lists the driver
// but loading it fails (the DLL was moved or deleted after registration).
static void ReportView(REGSAM view, const wchar_t* label) {
    P(L"[View %ls]", label);
    std::wstring clsid, desc, path, tm;
    bool asio = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\" WB_NAME_W, L"CLSID", view, clsid);
    RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\" WB_NAME_W, L"Description", view, desc);
    P(L"  HKLM\\SOFTWARE\\ASIO\\%ls: %ls", WB_NAME_W, asio ? L"present" : L"NOT registered");
    if (asio) {
        P(L"    CLSID = %ls%ls", clsid.c_str(), _wcsicmp(clsid.c_str(), kClsidStr) ? L"  (DIFFERENT from expected)" : L"");
        P(L"    Description = %ls", desc.c_str());
    }
    std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr + L"\\InprocServer32";
    if (RegStr(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, view, path)) {
        RegStr(HKEY_LOCAL_MACHINE, key.c_str(), L"ThreadingModel", view, tm);
        DWORD at = GetFileAttributesW(path.c_str());
        P(L"  InprocServer32 = %ls", path.c_str());
        P(L"    ThreadingModel = %ls   DLL file: %ls", tm.c_str(), at != INVALID_FILE_ATTRIBUTES ? L"exists" : L"DOES NOT EXIST");
    } else {
        P(L"  InprocServer32: NOT registered");
    }
    // Other ASIO drivers registered in this view
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | view, &k) == ERROR_SUCCESS) {
        std::wstring list;
        for (DWORD i = 0;; ++i) {
            wchar_t n[256]; DWORD nl = 256;
            if (RegEnumKeyExW(k, i, n, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (!list.empty()) list += L", ";
            list += n;
        }
        RegCloseKey(k);
        P(L"  ASIO drivers in this view: %ls", list.empty() ? L"(none)" : list.c_str());
    }
}

// Leftovers of the old name (WasapiBridge, up to 0.5.x): ASIO keys, uninstall entry, program folder and settings folder.
// Each LEFTOVER line increments n; the old %APPDATA% settings folder is only informational (the driver migrates
// it) and therefore does not count. "None." is printed when nothing was found.
static void ReportLeftovers() {
    Section(L"Leftovers of the previous version (WasapiBridge)");
    int n = 0;
    std::wstring s;
    const REGSAM views[2] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    const wchar_t* vn[2] = {L"64-bit", L"32-bit"};
    for (int i = 0; i < 2; ++i) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\" WB_OLDNAME_W, 0, KEY_READ | views[i], &k) == ERROR_SUCCESS) {
            RegCloseKey(k); ++n;
            P(L"  LEFTOVER: HKLM\\SOFTWARE\\ASIO\\%ls (%ls view): the host would list two drivers; reinstall or run uninstall.bat", WB_OLDNAME_W, vn[i]);
        }
    }
    if (RegStr(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" WB_OLDNAME_W, L"DisplayVersion", KEY_WOW64_64KEY, s)) {
        ++n; P(L"  LEFTOVER: uninstall entry of %ls %ls", WB_OLDNAME_W, s.c_str());
    }
    wchar_t pf[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH) || GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
        std::wstring d = std::wstring(pf) + L"\\" WB_OLDNAME_W;
        if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) { ++n; P(L"  LEFTOVER: folder %%ProgramFiles%%\\%ls", WB_OLDNAME_W); }
    }
    wchar_t ad[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, ad);
    if (GetFileAttributesW((std::wstring(ad) + L"\\" WB_OLDNAME_W).c_str()) != INVALID_FILE_ATTRIBUTES)
        P(L"  Info: %%APPDATA%%\\%ls exists (old settings; copied to the new folder on first load, it can be deleted)", WB_OLDNAME_W);
    if (!n) P(L"  None.");
}

// Installed version (from the uninstall entry) and the registration in both registry views.
// The installed version is looked up in both views because the installer may have written its uninstall entry
// in either one; a portable copy registered with regsvr32 has none.
static void ReportRegistration() {
    Section(L"Driver registration");
    std::wstring ver;
    const wchar_t* un = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" WB_NAME_W;
    if (RegStr(HKEY_LOCAL_MACHINE, un, L"DisplayVersion", KEY_WOW64_64KEY, ver) ||
        RegStr(HKEY_LOCAL_MACHINE, un, L"DisplayVersion", KEY_WOW64_32KEY, ver))
        P(L"Installed version (installer): %ls", ver.c_str());
    else P(L"Installed version (installer): not found (portable install or not installed)");
    ReportView(KEY_WOW64_64KEY, L"64-bit (x64 / ARM64 hosts)");
    ReportView(KEY_WOW64_32KEY, L"32-bit (x86 hosts, WOW6432Node)");
}

// ---------------------------------------------------------------- devices
// Endpoint property helpers: friendly name, ContainerId (physical device) and endpoint id.
// Each helper returns an empty string (DevName, DevEnumerator, DevId) or "(unknown)" (DevContainer) when the
// property is missing; they never fail the report. Endpoints of one physical device share a ContainerId.
static std::wstring DevName(IMMDevice* d) {
    std::wstring name;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    return name;
}

static std::wstring DevContainer(IMMDevice* d) {
    std::wstring s = L"(unknown)";
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && v.puuid) s = GuidStr(*v.puuid);
        PropVariantClear(&v); ps->Release();
    }
    return s;
}

// 0.8.0 (G1): enumerator of the endpoint's device node. BTHENUM (Bluetooth classic: A2DP / hands-free), BTHHFENUM
// (hands-free) and BTHLEDEVICE (LE Audio) are Bluetooth; the same rule as the driver (driver.cpp IsBluetoothDevice).
static const PROPERTYKEY kPkeyEnumeratorName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 24};
static std::wstring DevEnumerator(IMMDevice* d) {
    std::wstring s;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyEnumeratorName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) s = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    return s;
}

// Endpoint id string (unique per endpoint, stable across reboots); printed as a short hash unless --full-ids.
static std::wstring DevId(IMMDevice* d) {
    std::wstring s; LPWSTR id = nullptr;
    if (SUCCEEDED(d->GetId(&id)) && id) { s = id; CoTaskMemFree(id); }
    return s;
}

// Human-readable description of a WAVEFORMATEX(TENSIBLE): rate, channels, sample type, bits, mask.
// The EXTENSIBLE fields are only read when cbSize >= 22 (the size of the extension), so a malformed short
// WAVE_FORMAT_EXTENSIBLE header is described from the base WAVEFORMATEX fields only.
static std::wstring FmtDesc(const WAVEFORMATEX* w) {
    bool isFloat = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    int valid = w->wBitsPerSample;
    DWORD mask = 0;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)w;
        isFloat = IsEqualGUID(x->SubFormat, kSubtypeFloat);
        valid = x->Samples.wValidBitsPerSample;
        mask = x->dwChannelMask;
    }
    wchar_t b[160];
    swprintf(b, 160, L"%lu Hz, %u channels, %ls %u bits (valid %d), mask 0x%lX", (unsigned long)w->nSamplesPerSec,
             (unsigned)w->nChannels, isFloat ? L"float" : L"PCM", (unsigned)w->wBitsPerSample, valid, (unsigned long)mask);
    return b;
}

// Builds a WAVEFORMATEXTENSIBLE for IsFormatSupported tests.
// Channel mask: front left/right for stereo, 0 (unspecified) otherwise. int24/32 = 32-bit container with 24 valid
// bits; int24 = packed 3-byte samples.
static WAVEFORMATEXTENSIBLE MakeFmt(DWORD rate, int ch, int bits, int valid, bool isFloat) {
    WAVEFORMATEXTENSIBLE x = {};
    x.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels       = (WORD)ch;
    x.Format.nSamplesPerSec  = rate;
    x.Format.wBitsPerSample  = (WORD)bits;
    x.Format.nBlockAlign     = (WORD)(ch * bits / 8);
    x.Format.nAvgBytesPerSec = rate * x.Format.nBlockAlign;
    x.Format.cbSize          = 22;
    x.Samples.wValidBitsPerSample = (WORD)valid;
    x.dwChannelMask = ch == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    x.SubFormat     = isFloat ? kSubtypeFloat : kSubtypePcm;
    return x;
}

// Exclusive-mode format matrix (2 channels, standard rates x float32/int32/int24/int24-in-32/int16) using
// IsFormatSupported only (no stream is opened). Reports when Exclusive mode is disabled in the device properties.
// Cell meaning: "yes" = S_OK, "no" = AUDCLNT_E_UNSUPPORTED_FORMAT, "err" = any other HRESULT (e.g. Exclusive mode
// disabled, device in use or invalidated). In Exclusive mode IsFormatSupported takes no closest-match pointer
// (nullptr), so the answer is strictly yes or no for that exact format. A "yes" does not guarantee that
// Initialize succeeds at every buffer period; it only tells which formats the driver may try in Exclusive mode.
static void ReportExclusive(IAudioClient* c) {
    static const DWORD rates[] = {44100, 48000, 96000};
    struct F { const wchar_t* name; int bits, valid; bool fl; };
    static const F fmts[] = {{L"float32", 32, 32, true}, {L"int32", 32, 32, false}, {L"int24", 24, 24, false},
                             {L"int24/32", 32, 24, false}, {L"int16", 16, 16, false}};
    P(L"    Exclusive 2ch (IsFormatSupported):   float32  int32  int24  int24/32  int16");
    bool notAllowed = false;
    for (DWORD r : rates) {
        std::wstring line;
        wchar_t h[48]; swprintf(h, 48, L"      %6lu Hz                         ", (unsigned long)r); line = h;
        for (const F& f : fmts) {
            WAVEFORMATEXTENSIBLE x = MakeFmt(r, 2, f.bits, f.valid, f.fl);
            HRESULT hr = c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &x.Format, nullptr);
            if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) notAllowed = true;
            const wchar_t* t = hr == S_OK ? L"yes" : hr == AUDCLNT_E_UNSUPPORTED_FORMAT ? L"no" : L"err";
            wchar_t cell[16]; swprintf(cell, 16, L"%-*ls", (int)wcslen(f.name) + 2, t);
            line += cell;
        }
        P(L"%ls", line.c_str());
    }
    if (notAllowed) P(L"    WARNING: exclusive mode is NOT allowed on this device (Properties > Advanced)");
}

// One endpoint: name, ids, mix format, device periods, IAudioClient3 engine periods, Exclusive matrix, volume.
// Everything is best effort: a call that fails prints a "not available" / "failed" line and the report goes on.
// GetDevicePeriod values are REFERENCE_TIME (100 ns units), printed in ms. The IAudioClient3 engine periods are
// in frames at the mix rate. The Bluetooth line matters for latency: Windows does not include the Bluetooth link
// delay in what it reports, so the latency an ASIO host sees for such a device is lower than the real one.
// The mix format is released only after the Exclusive matrix and the IAudioClient3 query, which both use it.
static void ReportEndpoint(IMMDevice* d, UINT index) {
    P(L"");
    P(L"  [%u] %ls", index, DevName(d).c_str());
    // 0.6.1: endpoint Ids and ContainerIds identify the machine; by default only a short hash is printed (enough
    // to see which devices share a ContainerId). --full-ids prints them unmasked for local troubleshooting.
    const std::wstring id = DevId(d), cont = DevContainer(d);
    P(L"    Id: %ls", g_fullIds ? id.c_str() : ShortHash(id).c_str());
    P(L"    ContainerId: %ls", g_fullIds ? cont.c_str() : ShortHash(cont).c_str());
    {
        const std::wstring en = DevEnumerator(d);
        const bool bt = en.size() >= 3 && !_wcsnicmp(en.c_str(), L"BTH", 3);
        P(L"    Bluetooth: %ls  (enumerator %ls)", bt ? L"yes - Windows does not report the real Bluetooth delay" : L"no",
          en.empty() ? L"unknown" : en.c_str());
    }
    IAudioClient* c = nullptr;
    if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) { P(L"    IAudioClient: could not be activated"); return; }
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(c->GetMixFormat(&mix)) && mix) P(L"    Mix format: %ls", FmtDesc(mix).c_str());
    else P(L"    Mix format: not available");
    REFERENCE_TIME defP = 0, minP = 0;
    if (SUCCEEDED(c->GetDevicePeriod(&defP, &minP)))
        P(L"    GetDevicePeriod: default %.3f ms, minimum %.3f ms", defP / 10000.0, minP / 10000.0);
    IAudioClient3* c3 = nullptr;
    if (mix && SUCCEEDED(c->QueryInterface(__uuidof(IAudioClient3), (void**)&c3))) {
        UINT32 defF = 0, fundF = 0, minF = 0, maxF = 0;
        if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &defF, &fundF, &minF, &maxF))) {
            P(L"    Shared engine (IAudioClient3): default %u, fundamental %u, min %u (%.2f ms), max %u frames", defF, fundF, minF,
              mix->nSamplesPerSec ? 1000.0 * minF / mix->nSamplesPerSec : 0.0, maxF);
            // 0.9.4: the driver uses the minimum period with LowLatencyShared=1 at any ASIO rate
            P(L"    Low-latency period below the default: %ls (the driver opens at %u Hz and converts other ASIO rates itself)",
              minF < defF ? L"available" : L"not available (minimum = default)", (unsigned)mix->nSamplesPerSec);
        } else P(L"    Shared engine (IAudioClient3): GetSharedModeEnginePeriod failed");
        c3->Release();
    } else P(L"    IAudioClient3: not available");
    ReportExclusive(c);
    if (mix) CoTaskMemFree(mix);
    c->Release();
    IAudioEndpointVolume* v = nullptr;
    if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&v))) {
        DWORD hw = 0; v->QueryHardwareSupport(&hw);
        float mn = 0, mx = 0, st = 0, db = 0; BOOL mute = FALSE;
        v->GetVolumeRange(&mn, &mx, &st);
        v->GetMasterVolumeLevel(&db);
        v->GetMute(&mute);
        P(L"    Hardware volume: volume=%ls mute=%ls meter=%ls   range %.1f..%.1f dB, current %.1f dB%ls",
          hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME ? L"yes" : L"no", hw & ENDPOINT_HARDWARE_SUPPORT_MUTE ? L"yes" : L"no",
          hw & ENDPOINT_HARDWARE_SUPPORT_METER ? L"yes" : L"no", mn, mx, db, mute ? L" (muted)" : L"");
        v->Release();
    }
}

// Every active render and capture endpoint, then the default device for each role.
// Indexes [n] are positions in the enumeration (not stable ids); only DEVICE_STATE_ACTIVE endpoints are listed,
// so disabled, unplugged or not-present devices do not appear.
static void ReportDevices(IMMDeviceEnumerator* e) {
    const EDataFlow flows[2] = {eRender, eCapture};
    const wchar_t* names[2] = {L"Active output (render) devices", L"Active input (capture) devices"};
    for (int f = 0; f < 2; ++f) {
        Section(names[f]);
        IMMDeviceCollection* col = nullptr;
        if (FAILED(e->EnumAudioEndpoints(flows[f], DEVICE_STATE_ACTIVE, &col))) { P(L"  EnumAudioEndpoints failed"); continue; }
        UINT n = 0; col->GetCount(&n);
        if (!n) P(L"  (none)");
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(col->Item(i, &d))) { ReportEndpoint(d, i); d->Release(); }
        }
        col->Release();
    }
    Section(L"Default devices");
    const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
    const wchar_t* rn[3] = {L"console", L"multimedia", L"communications"};
    for (int f = 0; f < 2; ++f) for (int r = 0; r < 3; ++r) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(flows[f], roles[r], &d)) && d) {
            P(L"  %ls / %ls: %ls", f == 0 ? L"output" : L"input", rn[r], DevName(d).c_str());
            d->Release();
        } else P(L"  %ls / %ls: (none)", f == 0 ? L"output" : L"input", rn[r]);
    }
}

// ---------------------------------------------------------------- MMCSS
// MMCSS service state, whether this process can join the "Pro Audio" task with critical priority (joined and
// reverted immediately), and the task's registry settings.
// The join/revert of the "Pro Audio" task is done on this (main) thread and undone at once, so it leaves no
// lasting change. "rejected" critical priority or "NOT available" explains audio threads that run without MMCSS
// boost (the driver's audio thread uses the same task name). The registry values are shown as found; absent
// values are simply not printed.
static void ReportMmcss() {
    Section(L"MMCSS (multimedia class scheduler)");
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE s = OpenServiceW(scm, L"MMCSS", SERVICE_QUERY_STATUS);
        SERVICE_STATUS st = {};
        if (s && QueryServiceStatus(s, &st)) P(L"MMCSS service: %ls", st.dwCurrentState == SERVICE_RUNNING ? L"running" : L"stopped");
        else P(L"MMCSS service: could not be queried (it starts on demand)");
        if (s) CloseServiceHandle(s);
        CloseServiceHandle(scm);
    }
    DWORD idx = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
    if (h) {
        BOOL pr = AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
        P(L"Task \"Pro Audio\": available (critical priority %ls)", pr ? L"accepted" : L"rejected");
        AvRevertMmThreadCharacteristics(h);
    } else P(L"Task \"Pro Audio\": NOT available (error %lu)", GetLastError());
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Pro Audio";
    DWORD prio = 0, clk = 0, resp = 0; std::wstring cat;
    if (RegDword(HKEY_LOCAL_MACHINE, key, L"Priority", KEY_WOW64_64KEY, prio)) P(L"  Priority = %lu", prio);
    if (RegDword(HKEY_LOCAL_MACHINE, key, L"Clock Rate", KEY_WOW64_64KEY, clk)) P(L"  Clock Rate = %lu", clk);
    if (RegStr(HKEY_LOCAL_MACHINE, key, L"Scheduling Category", KEY_WOW64_64KEY, cat)) P(L"  Scheduling Category = %ls", cat.c_str());
    if (RegDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                 L"SystemResponsiveness", KEY_WOW64_64KEY, resp)) P(L"  SystemResponsiveness = %lu", resp);
}

// ---------------------------------------------------------------- settings and log
// Reads a file into memory (shared read, so a running driver can keep writing the log).
// 0.9.6 (D10): a file larger than maxBytes is read from its END: the last maxBytes bytes. Before, only the first
// 64 MB were read, so the "last 200 lines" of a huge log.txt came from the start of the file, not from its real
// end. When the file was cut, *cut is set (if given) and the caller must drop the first (partial) line. A BOM at
// the start of the file is kept: it is read separately and put in front of the tail, so DecodeText still sees the
// right encoding; for UTF-16 the tail offset is rounded to an even byte so the characters stay aligned.
// A short read keeps what was read.
static bool ReadFileBytes(const std::wstring& path, std::string& out, LONGLONG maxBytes = 64LL * 1024 * 1024,
                          bool* cut = nullptr) {
    if (cut) *cut = false;
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(f, &sz);
    std::string prefix;                      // BOM of a file that is read from its tail
    if (sz.QuadPart > maxBytes) {
        char bom[3] = {};
        DWORD br = 0;
        ReadFile(f, bom, 3, &br, nullptr);   // the first bytes: is there a BOM?
        LONGLONG ofs = sz.QuadPart - maxBytes;
        if (br >= 2 && (unsigned char)bom[0] == 0xFF && (unsigned char)bom[1] == 0xFE) {
            prefix.assign(bom, 2);
            ofs &= ~1LL;                     // UTF-16LE: start on a character boundary
        } else if (br == 3 && (unsigned char)bom[0] == 0xEF && (unsigned char)bom[1] == 0xBB && (unsigned char)bom[2] == 0xBF) {
            prefix.assign(bom, 3);           // UTF-8: a split character only damages the first line (dropped)
        }
        LARGE_INTEGER to; to.QuadPart = ofs;
        if (!SetFilePointerEx(f, to, nullptr, FILE_BEGIN)) { CloseHandle(f); return false; }
        sz.QuadPart -= ofs;
        if (cut) *cut = true;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty() || ReadFile(f, &out[0], (DWORD)out.size(), &rd, nullptr);
    CloseHandle(f);
    out.resize(rd);
    if (!prefix.empty()) out.insert(0, prefix);
    return ok != FALSE;
}

// Decodes UTF-16LE (BOM), UTF-8 (BOM) or BOM-less UTF-8 text.
// A trailing odd byte of UTF-16 text is ignored. Text without a BOM is assumed to be UTF-8 (the driver writes its
// files as UTF-8); invalid UTF-8 sequences become U+FFFD.
static std::wstring DecodeText(const std::string& b) {
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE)
        return std::wstring((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF)
        return FromBytes(b.substr(3), CP_UTF8);
    return FromBytes(b, CP_UTF8);
}

// The driver's settings folder: %APPDATA%\WASAPI Driver for ASIO, or the same fallback the driver uses
// (%LOCALAPPDATA%, then %TEMP%; appdir.h WbSettingsBase, 0.9.6 review R7d), or an empty string if no per-user
// folder exists (0.9.6, D6: before, the result was "\WASAPI Driver for ASIO", a folder in the root of the
// current drive; callers skip an empty path).
static std::wstring AppDataDir() {
    const std::wstring base = WbSettingsBase();
    return base.empty() ? std::wstring() : base + L"\\" WB_NAME_W;
}

// 0.7.0 (F9): Windows microphone privacy for desktop programs (registry, read-only) with a clear verdict.
// 0.9.6 (D8): the misplaced comment about config.ini / log.txt that was here belongs to ReportFiles (below).
// 0.9.6 (KI-4, review R7a): the verdict follows the panel's rule. The driver's input selection is read from
// config.ini [General] (read-only): InputDevice ("none" = no input; empty or missing = Windows default, i.e. an
// input IS selected, the driver default) and ExtraInputs (non-empty = at least one extra input). A missing
// config.ini means the defaults, so an input counts as selected. Profiles applied per program can change the
// selection at run time; the report says which values it used.
static void ReportMicPrivacy() {
    Section(L"Microphone privacy (desktop apps)");
    const MicPrivacy mp = ReadMicPrivacy();
    auto v = [](const std::wstring& x) { return x.empty() ? L"(missing = allowed)" : x.c_str(); };
    P(L"  Device-wide switch (HKLM ...\\ConsentStore\\microphone): %ls", v(mp.machine));
    P(L"  User switch        (HKCU ...\\ConsentStore\\microphone): %ls", v(mp.user));
    P(L"  Desktop apps       (HKCU ...\\microphone\\NonPackaged):   %ls", v(mp.desktop));
    // The driver's input selection (config.ini [General]); "\x01" = key missing.
    bool inputSelected = true;
    const std::wstring dir = AppDataDir();
    if (!dir.empty()) {
        const std::wstring ini = dir + L"\\config.ini";
        wchar_t in[512] = L"", ex[2048] = L"";
        GetPrivateProfileStringW(L"General", L"InputDevice", L"\x01", in, 512, ini.c_str());
        GetPrivateProfileStringW(L"General", L"ExtraInputs", L"", ex, 2048, ini.c_str());
        const bool none = !_wcsicmp(in, L"none");
        inputSelected = !none || ex[0] != 0;
        P(L"  Driver inputs (config.ini [General]): InputDevice = %ls, ExtraInputs = %ls",
          in[0] == 1 ? L"(missing = Windows default)" : in[0] ? in : L"(Windows default)", ex[0] ? L"(set)" : L"(none)");
    }
    if (mp.denied && inputSelected)
        P(L"  VERDICT: desktop apps CANNOT use recording devices, and the driver has an input selected: that input "
          L"records silence. Change it in Settings > Privacy & security > Microphone (\"Microphone access\" and "
          L"\"Let desktop apps access your microphone\"), or set Input device = (None - no inputs) in the panel.");
    else if (mp.denied)
        P(L"  VERDICT: Windows blocks recording for desktop apps, but no input is selected in the driver "
          L"(Input = None, no extra inputs), so this does not matter for the driver.");
    else
        P(L"  VERDICT: desktop apps can use recording devices.");
}

// 0.7.0 (F8): crash reports written by the driver (minidumps + marker), newest first, at most 5 kept.
// The crash folder is under %LOCALAPPDATA% (not %APPDATA%). Its contents are written by the driver's crash
// handler (crash.cpp): crash-marker.txt makes the next driver session start in safe mode; crash-*.dmp are
// minidumps. This function only lists them (name, size, local time), it never opens or deletes a dump.
static void ReportCrashes() {
    Section(L"Crash reports");
    wchar_t p[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p)) || !p[0]) { P(L"  (local application data folder not available)"); return; }
    const std::wstring dir = std::wstring(p) + L"\\" WB_NAME_W L"\\crash";
    P(L"  Folder: %ls", dir.c_str());
    std::string raw;
    if (ReadFileBytes(dir + L"\\crash-marker.txt", raw)) {
        P(L"  Crash marker present (the next session starts in safe mode):");
        RawLine(DecodeText(raw));
    } else {
        P(L"  No crash marker.");
    }
    struct D { FILETIME t; std::wstring name; ULONGLONG size; };
    std::vector<D> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\crash-*.dmp").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do v.push_back({fd.ftLastWriteTime, fd.cFileName, ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow}); while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(v.begin(), v.end(), [](const D& a, const D& b) { return CompareFileTime(&a.t, &b.t) > 0; });
    if (v.empty()) P(L"  No minidumps.");
    for (const D& d : v) {
        SYSTEMTIME st; FILETIME lt; FileTimeToLocalFileTime(&d.t, &lt); FileTimeToSystemTime(&lt, &st);
        P(L"  %ls  %llu KB  %04u-%02u-%02u %02u:%02u", d.name.c_str(), (unsigned long long)(d.size / 1024), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }
    if (!v.empty())
        P(L"  To report a crash, attach the newest .dmp file together with this report. A minidump contains part of the "
          L"host's process memory (it may include audio data, file names or other data of the DAW); see PRIVACY.txt.");
}

// ReportFiles: config.ini verbatim (every line through RawLine), the last 200 lines of log.txt, and whether
// log-previous.txt exists (the driver keeps the previous session's log there, useful after a crash).
static void ReportFiles() {
    const std::wstring dir = AppDataDir();
    if (dir.empty()) { Section(L"Settings and log"); P(L"  (application data folder not available)"); return; }   // 0.9.6
    Section(L"%APPDATA%\\" WB_NAME_W L"\\config.ini");
    std::string raw;
    if (ReadFileBytes(dir + L"\\config.ini", raw)) {
        std::wstring t = DecodeText(raw), line;
        for (size_t i = 0; i <= t.size(); ++i) {
            if (i == t.size() || t[i] == L'\n') { if (!line.empty() && line.back() == L'\r') line.pop_back(); RawLine(line); line.clear(); }
            else line += t[i];
        }
    } else P(L"  (does not exist: the driver has not been loaded by a host with this user yet)");

    Section(L"%APPDATA%\\" WB_NAME_W L"\\log.txt (last 200 lines)");
    // 0.9.6 (D10): only the last 4 MB are read (far more than 200 lines); a larger log is read from its real end.
    bool cut = false;
    if (ReadFileBytes(dir + L"\\log.txt", raw, 4LL * 1024 * 1024, &cut)) {
        std::wstring t = DecodeText(raw);
        std::vector<std::wstring> lines;
        std::wstring line;
        for (wchar_t c : t) {
            if (c == L'\n') { if (!line.empty() && line.back() == L'\r') line.pop_back(); lines.push_back(line); line.clear(); }
            else line += c;
        }
        if (!line.empty()) lines.push_back(line);
        if (cut && !lines.empty()) lines.erase(lines.begin());   // the tail starts in the middle of a line
        size_t from = lines.size() > 200 ? lines.size() - 200 : 0;
        if (cut) P(L"  (large file: only its last 4 MB were read)");
        if (from) P(L"  (%u lines omitted)", (unsigned)from);
        for (size_t i = from; i < lines.size(); ++i) RawLine(lines[i]);
        if (lines.empty()) P(L"  (empty)");
    } else P(L"  (does not exist)");
    DWORD at = GetFileAttributesW((dir + L"\\log-previous.txt").c_str());
    P(L"");
    P(L"log-previous.txt: %ls", at != INVALID_FILE_ATTRIBUTES ? L"exists (attach it when reporting a crash)" : L"does not exist");
}

// ---------------------------------------------------------------- output
// Folder of this executable.
static std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

// Writes the report as UTF-8 with BOM. Returns false if the file cannot be created or written.
// CREATE_ALWAYS: an older report in the same folder is overwritten. Used by wmain as a probe too: the first
// folder where this succeeds is the one reported to the user.
static bool WriteReport(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), (int)g_out.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), (int)g_out.size(), &u[0], n, nullptr, nullptr);
    DWORD w = 0;
    static const char bom[3] = {'\xEF', '\xBB', '\xBF'};
    BOOL ok = WriteFile(f, bom, 3, &w, nullptr) && WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

// Prints to the console (WriteConsoleW, chunked) or, when redirected, writes UTF-8 to the handle.
// Chunks of 8192 characters: large single WriteConsoleW calls can fail on some console hosts. When the output is
// redirected (pipe or file) GetConsoleMode fails and the text is written as UTF-8 bytes instead of UTF-16.
static void Console(const std::wstring& t) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, w = 0;
    if (GetConsoleMode(h, &mode)) {
        for (size_t i = 0; i < t.size(); i += 8192)
            WriteConsoleW(h, t.data() + i, (DWORD)(t.size() - i < 8192 ? t.size() - i : 8192), &w, nullptr);
    } else {                       // redirected output: UTF-8
        int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), &u[0], n, nullptr, nullptr);
        WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    }
}

// Builds the report, prints it, saves it to the first writable folder (exe folder, %APPDATA%, %TEMP%) and waits
// for Enter when started from a console window. Exit code 0 = report saved, 1 = could not save.
// Command line: --full-ids (case-insensitive) prints endpoint Ids and ContainerIds unmasked; any other argument
// is ignored. The "Press Enter" wait only happens when stdin is a console (GetConsoleMode on the input handle
// succeeds), so the tool does not block when started from a script with redirected input. The saved path is
// sanitized before it is printed, like the report itself.
int wmain(int argc, wchar_t** argv) {
    ConsoleIcon conIcon;                 // 0.9.3: application icon on the console window while running
    for (int i = 1; i < argc; ++i) if (!_wcsicmp(argv[i], L"--full-ids")) g_fullIds = true;
    SYSTEMTIME t; GetSystemTime(&t);
    P(L"%ls %ls - Diagnostics", WB_NAME_W, WB_VERSION_W);
    P(L"Date (UTC): %04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    P(L"This report only contains Windows and " WB_NAME_W L" data. Review it before sending it.");
    P(L"ASIO is a registered trademark of Steinberg Media Technologies GmbH.");
    P(L"User folders, user name and computer name are masked; device Ids are %ls.", g_fullIds ? L"shown in full (--full-ids)" : L"shown as short hashes");
    ReportOs();
    ReportRegistration();
    ReportLeftovers();
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* e = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e))) {
        ReportDevices(e);
        e->Release();
    } else {
        Section(L"Devices");
        P(L"  Could not create IMMDeviceEnumerator (is the Windows Audio service stopped?)");
    }
    ReportMmcss();
    ReportMicPrivacy();
    ReportCrashes();
    ReportFiles();
    if (SUCCEEDED(hr)) CoUninitialize();
    P(L"");
    P(L"== End of report ==");

    g_out = SanitizeText(g_out);   // 0.6.1: %APPDATA% etc. instead of expanded user paths; no user/computer name
    Console(g_out);
    const wchar_t* name = L"\\" WB_FILEBASE_W L"-diagnostics.txt";
    std::wstring dirs[3] = {ExeDir(), AppDataDir(), L""};
    wchar_t tmp[MAX_PATH] = L"";
    if (GetTempPathW(MAX_PATH, tmp)) { dirs[2] = tmp; while (!dirs[2].empty() && (dirs[2].back() == L'\\' || dirs[2].back() == L'/')) dirs[2].pop_back(); }
    std::wstring done;
    for (const std::wstring& d : dirs) {
        if (d.empty()) continue;
        if (WriteReport(d + name)) { done = d + name; break; }
    }
    if (!done.empty()) Console(L"\r\nReport saved to:\r\n  " + SanitizeText(done) + L"\r\nAttach that file to your report.\r\n");
    else Console(L"\r\nCould not save the report to disk; copy the text from this window.\r\n");

    DWORD mode = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(in, &mode)) {          // started from the Start menu: do not close the window right away
        Console(L"\r\nPress Enter to exit...");
        wchar_t b[8]; DWORD r = 0;
        ReadConsoleW(in, b, 8, &r, nullptr);
    }
    return done.empty() ? 1 : 0;
}
