// WASAPI Driver for ASIO - configuration (config.ini, UTF-16LE), profiles and diagnostic log
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
//   Reads and writes %APPDATA%\WASAPI Driver for ASIO\config.ini with the Win32 private-profile API (the file is
//   created with a UTF-16LE BOM so that the *W functions store Unicode), manages named profiles, migrates data from
//   older versions and implements the diagnostic log (log.txt, previous session in log-previous.txt).
//
// Threading
//   Everything here runs on the host thread (IASIO calls and the control panel), except Log(), which may also be
//   called from Windows notification threads; it is serialized by a critical section. Nothing in this file may be
//   called from the real-time audio thread (file I/O, locks, allocation).
//
// Migrations performed (each one is idempotent)
//   - 0.6.0 rename: if the new config.ini does not exist yet and %APPDATA%\WasapiBridge\config.ini does, it is
//     copied once. The old folder is never deleted.
//   - Profile sections "[Perfil:<name>]" (Spanish prefix used by 0.4.0 to 0.5.1) are moved to
//     "[Profile:<name>]". The three factory profiles (Estudio, Notebook, Streaming) are renamed to Studio, Laptop and
//     Streaming and their empty device keys are removed, but only while they still look like the factory version
//     (OutputDevice, InputDevice, ExtraOutputs and ExtraInputs absent or empty). User profiles keep their names and
//     contents.
//
// How the private-profile API is used here (reviewer notes)
//   - Every Read*/Write* call opens, parses and (for writes) rewrites config.ini; there is no in-memory model of
//     the file. WritePrivateProfileStringW(nullptr, nullptr, nullptr, path) is the documented way to flush the
//     API's cache for that file; every public writer does it before RewriteConfigFile reads the file raw.
//   - The *W functions only store Unicode if the file already starts with a UTF-16LE BOM (EnsureFile); a file
//     without BOM is read and written in the ANSI code page.
//   - There is no cross-process locking: two hosts saving at the same moment can lose one writer's change (last
//     writer wins). The atomic replace in RewriteConfigFile prevents torn files, not lost updates.
//   - Every writer ends with RewriteConfigFile, so the file on disk is always in the normalized layout.
#include "common.h"
#include "privacy.h"
#include "appdir.h"      // WbSettingsBase (0.9.6: shared with the diagnostics tool)
#include "parse.h"       // 0.9.8: pure parsers (fuzzed offline, tests/fuzz_test.cpp)
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cstdlib>
#include <cmath>      // std::isfinite (0.9.6, D4)

// Comment block + "[General]" written into a new config.ini. CRLF line ends because the file is meant to be
// edited by hand in Notepad. Keep this list in sync with the keys handled by ReadSection/WriteSection.
static const wchar_t* kHeader =
L"; " WB_NAME_W L" - configuration. Edit it from the driver control panel (Control Panel / Hardware Setup button in the host).\r\n"
L"; Mode=Shared|Exclusive  LowLatencyShared=0|1  OutputDevice/InputDevice=substring of the device name (empty=Windows default, InputDevice=none for no input)\r\n"
L"; Loopback=Off|ExcludeDaw|All|App  LoopbackApp=program.exe  BufferSize=32..4096  SampleRate=Hz  Gain=linear  FollowDeviceChanges=0|1\r\n"
L"; DriftCorrection=Auto|On|Off  ResamplerQuality=Hermite|Sinc  Dither=0|1  MaxChannels=1..32  Log=0|1  ActiveProfile=name\r\n"
L"; FixedRatioEngine=Auto|Soxr|Sinc|Hermite (rate conversion when the ASIO rate differs from the device rate)\r\n"
L"; ExtraOutputs/ExtraInputs=name1|name2 (up to 3)  OutputMap=asio:dev,... (e.g. 1:1,2:2,3:1)  AlignDevices=0|1\r\n"
L"; OutputLatencyOffsetMs/InputLatencyOffsetMs=-50..500 (added to the reported latency)\r\n"
L"; DirectMonitor=0|1  DirectMonitorInput/DirectMonitorOutput=first channel (1-based)  DirectMonitorLevelDb=-60..6  DirectMonitorPan=-100..100  DirectMonitorMute=0|1\r\n"
L"; MonitorWindow=0|1  MonitorTopmost=0|1  MonitorX/MonitorY/MonitorW/MonitorH=floating window placement (pixels)\r\n"
L"; SampleRate=Auto follows the Windows rate (Shared)  ProtectA2dp=0|1  TrayIcon=0|1  DeviceChannelNames=0|1\r\n"
L"; Limiter=0|1  LimiterCeilingDb=-20..-0.1  LimiterGainCap=0|1  LimiterGainCapDb=-20..24\r\n"
L"; Visuals=Off|Plasma|Starfield|Copper|Tunnel|Reactive|Metaballs|Fire|Vector|Twister  VisualsIntensity=0..100  VisualsMonitor=0|1  VisualsSpeed=25..400\r\n"
L"; Theme=Dark|RetroBlue|Retro8|Terminal|Neon|Light  Language=Auto|en|es|pt|fr|de\r\n"
L"; UpdateCheck=0|1 (weekly, off by default)  UpdateRepo=owner/repository  UpdateLastCheck=seconds since 1970\r\n"
L"; [HostProfiles] program.exe=profile name (applied when that program loads the driver)\r\n"
L"; Profiles: sections [Profile:name] with the engine keys (and the device keys when saved with \"Include devices\")\r\n"
L"[General]\r\n";
// The comment block alone (kHeader without its final "[General]" line); RewriteConfigFile puts it on top.
static std::wstring CommentBlock() {
    std::wstring h(kHeader);
    return h.substr(0, h.rfind(L"[General]"));
}

// %APPDATA%\<WB_NAME_W> (roaming profile: settings follow the user). Called by every public function, so the
// migration check and CreateDirectoryW run each time (cheap no-ops after the first call). CopyFileW with
// bFailIfExists=TRUE never overwrites a file that appeared concurrently.
// 0.9.6 (D6): if the roaming AppData folder cannot be resolved (SHGetFolderPathW fails or returns an empty path,
// e.g. a broken profile or a service account), the folder falls back to %LOCALAPPDATA%, then to the user's
// temporary folder (GetTempPathW), always followed by \<WB_NAME_W>. Before, the empty path produced
// "\<WB_NAME_W>", a folder in the ROOT of the current drive. The migration from the old name only runs with the
// real roaming AppData folder (`roaming` below). The chain lives in appdir.h (WbSettingsBase) so that
// WasapiDriverDiag.exe looks in the same folder (review R7d).
std::wstring ConfigDir() {
    bool roaming = false;
    const std::wstring base = WbSettingsBase(&roaming);                         // 0.9.6 (D6, R7d): appdir.h
    // Last resort (no per-user folder at all): a relative name, i.e. the current directory, never the drive root.
    std::wstring d = base.empty() ? std::wstring(WB_NAME_W) : base + L"\\" WB_NAME_W;
    // 0.6.0: one-time copy of config.ini from %APPDATA%\WasapiBridge when the new file does not exist yet.
    // Skipped unless the roaming AppData folder was resolved. The old folder is left untouched.
    if (roaming && GetFileAttributesW((d + L"\\config.ini").c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring old = base + L"\\" WB_OLDNAME_W + L"\\config.ini";
        if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(d.c_str(), nullptr);
            CopyFileW(old.c_str(), (d + L"\\config.ini").c_str(), TRUE);
        }
    }
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
// Full path of config.ini (see ConfigDir for the migration side effect).
std::wstring ConfigPath() { return ConfigDir() + L"\\config.ini"; }

// Reads key `key` of section `sec` (default `def`), trimming surrounding blanks.
// Values longer than 511 characters are truncated by the fixed buffer. GetPrivateProfileStringW also strips
// surrounding quotes; the trimming here additionally removes tabs. Used with sentinel defaults (L"\x01", L"?")
// elsewhere in this file to tell "key absent" from "key present but empty".
static std::wstring ReadStrS(const wchar_t* sec, const wchar_t* key, const wchar_t* def, const std::wstring& path) {
    wchar_t buf[512];
    GetPrivateProfileStringW(sec, key, def, buf, 512, path.c_str());
    std::wstring s(buf);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
    return s.substr(i);
}

// Creates config.ini with a UTF-16LE BOM and the commented header if it does not exist.
// CREATE_NEW (not CREATE_ALWAYS): if another process creates the file between the GetFileAttributesW check and
// this call, the create fails and that process's file is kept. Write errors are ignored; the profile API then
// works on whatever exists (in the worst case an ANSI file without BOM).
static void EnsureFile(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; const WORD bom = 0xFEFF;   // UTF-16LE: Write/GetPrivateProfileStringW then work in Unicode
    WriteFile(f, &bom, 2, &w, nullptr);
    WriteFile(f, kHeader, (DWORD)(wcslen(kHeader) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
}

// 0.6.1: regenerates the comment lines of config.ini in English. Files written by 0.1.0 to 0.5.x carry Spanish
// comment lines; the private-profile API never touches comments, so they would stay forever.
// The file is parsed line by line: every comment (';' or '#') and blank line is dropped, every section header and
// key=value line is kept verbatim and in order (unknown keys and every [Profile:x] section included), and the
// English comment block is put on top. The result is written to a temporary file in the same folder and moved over
// config.ini with MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), so a crash leaves either the
// old or the new file, never a truncated one. Idempotent: if the regenerated text equals the current content,
// nothing is written. Host thread only (file I/O). Call it after flushing the profile API cache.
// Pitfalls: the temporary name includes the PID so two processes never share a temp file; MoveFileExW fails if
// another program holds config.ini open without FILE_SHARE_DELETE (e.g. some editors), in which case the
// temporary file is deleted and the original stays as it was. A UTF-16 file with an odd byte count loses its last
// byte. Comparing `out` with `text` assumes the file already uses CRLF; an LF-only file is rewritten once.
static void RewriteConfigFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > (8 << 20)) { CloseHandle(f); return; }   // 8 MB sanity limit
    std::string raw((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    const BOOL ok = raw.empty() || ReadFile(f, &raw[0], (DWORD)raw.size(), &got, nullptr);
    CloseHandle(f);
    if (!ok || got != raw.size()) return;
    // Decode: UTF-16LE with BOM (what this driver writes), UTF-8 with BOM, otherwise the ANSI code page (what the
    // profile API writes into a file without a BOM).
    std::wstring text;
    if (cfgp::DecodeUtf16Le(raw, text)) {
        // 0.9.8: decoded byte pair by byte pair (parse.h), no unaligned wchar_t read of the string's buffer
    } else {
        UINT cp = CP_ACP; size_t skip = 0;
        if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) { cp = CP_UTF8; skip = 3; }
        const int n = MultiByteToWideChar(cp, 0, raw.data() + skip, (int)(raw.size() - skip), nullptr, 0);
        if (n > 0) { text.resize(n); MultiByteToWideChar(cp, 0, raw.data() + skip, (int)(raw.size() - skip), &text[0], n); }
    }
    const std::wstring out = cfgp::NormalizeIni(text, CommentBlock());   // 0.9.8: parse.h (fuzzed offline)
    if (out == text) return;                                                         // already normalized
    const std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId());
    HANDLE t = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (t == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; const WORD bom = 0xFEFF;
    BOOL wr = WriteFile(t, &bom, 2, &w, nullptr) &&
              WriteFile(t, out.data(), (DWORD)(out.size() * sizeof(wchar_t)), &w, nullptr) && FlushFileBuffers(t);
    CloseHandle(t);
    if (!wr || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(tmp.c_str());
}

// 0.9.8: JoinList, SplitList, LbStr and DriftStr moved to parse.h (pure, fuzzed offline).
using cfgp::JoinList;
using cfgp::SplitList;
using cfgp::LbStr;
using cfgp::DriftStr;
using cfgp::FrEngineStr;    // 0.9.10

// Reads a section ([General] or [Profile:x]) on top of `c`: missing keys keep the current value of `c`.
// 0.9.8: the logic (every key, defaults and range checks) is cfgp::ReadSectionWith in parse.h, so it can be fuzzed
// offline; here it only receives the Win32 reader of this section (ReadStrS, trimmed values).
static void ReadSection(const wchar_t* sec, Config& c, const std::wstring& path) {
    cfgp::ReadSectionWith(c, [&](const wchar_t* key, const wchar_t* def) { return ReadStrS(sec, key, def, path); });
}

// Hardware-dependent keys. In a profile saved without devices they are removed (SecKind::ProfileEngine); in a
// profile saved with devices they are written. Also used by MigrateProfiles and ProfileHasDevices.
// Device keys: a profile stores them only when saved with "Include devices" (0.6.0).
static const wchar_t* const kDeviceKeys[] = {L"OutputDevice", L"InputDevice", L"LoopbackApp", L"ExtraOutputs",
                                             L"ExtraInputs", L"OutputMap", L"AlignDevices",
                                             // 0.7.0: latency offsets and direct monitoring depend on the hardware
                                             L"OutputLatencyOffsetMs", L"InputLatencyOffsetMs", L"DirectMonitor",
                                             L"DirectMonitorInput", L"DirectMonitorOutput", L"DirectMonitorLevelDb",
                                             L"DirectMonitorPan", L"DirectMonitorMute", L"ProtectA2dp"};
// General settings that never belong to a profile (machine preferences).
// Never written to (and actively removed from) [Profile:x] sections; see WriteSection.
static const wchar_t* const kGeneralOnlyKeys[] = {L"FollowDeviceChanges", L"MaxChannels", L"Log",
                                                  L"MonitorWindow", L"MonitorTopmost", L"MonitorX", L"MonitorY",
                                                  L"MonitorW", L"MonitorH", L"TrayIcon", L"DeviceChannelNames",
                                                  L"Visuals", L"VisualsIntensity", L"VisualsMonitor", L"VisualsSpeed",
                                                  L"Theme", L"Language", L"UpdateCheck", L"UpdateRepo", L"UpdateLastCheck",
                                                  L"HelpTips"};

// Which subset of keys WriteSection writes: everything ([General]), engine keys only, engine + device keys.
enum class SecKind { General, ProfileEngine, ProfileDevices };

// Writes `c` into section `sec`. General = every key; ProfileEngine = engine keys only (device keys and general-only
// keys are removed from the section); ProfileDevices = engine keys plus device keys.
// One WritePrivateProfileStringW per key (each call rewrites the file; acceptable for a settings dialog).
// Write errors are ignored. Passing nullptr as value deletes a key. Number formats are chosen so that a
// round-trip through ReadSection is stable (e.g. %.6f for gain, 1-based direct monitor channels).
// MonitorX/Y/W/H are written only once a size is known; Visuals only once chosen explicitly (see ReadSection).
static void WriteSection(const wchar_t* sec, const Config& c, const std::wstring& path, SecKind kind = SecKind::General) {
    auto W = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(sec, k, v.c_str(), path.c_str()); };
    wchar_t b[64];
    // Engine settings (always)
    W(L"Mode", c.exclusive ? L"Exclusive" : L"Shared");
    W(L"LowLatencyShared", c.lowLatencyShared ? L"1" : L"0");
    W(L"Loopback", LbStr(c.loopback));
    swprintf(b, 64, L"%ld", c.bufferSize); W(L"BufferSize", b);
    if (c.sampleRateAuto) W(L"SampleRate", L"Auto"); else { swprintf(b, 64, L"%.0f", c.sampleRate); W(L"SampleRate", b); }
    // 0.8.0 (G2): the limiter is an engine setting (stored in every profile)
    W(L"Limiter", c.limiterOn ? L"1" : L"0");
    swprintf(b, 64, L"%.1f", c.limiterCeilDb); W(L"LimiterCeilingDb", b);
    W(L"LimiterGainCap", c.limiterCapOn ? L"1" : L"0");
    swprintf(b, 64, L"%.1f", c.limiterCapDb); W(L"LimiterGainCapDb", b);
    swprintf(b, 64, L"%.6f", c.gain); W(L"Gain", b);
    W(L"DriftCorrection", DriftStr(c.drift));
    W(L"Dither", c.dither ? L"1" : L"0");
    W(L"ResamplerQuality", c.resampler == ResamplerQ::Sinc ? L"Sinc" : L"Hermite");
    W(L"FixedRatioEngine", FrEngineStr(c.frEngine));               // 0.9.10
    if (kind == SecKind::General) {
        W(L"FollowDeviceChanges", c.followDevices ? L"1" : L"0");
        swprintf(b, 64, L"%d", c.maxChannels); W(L"MaxChannels", b);
        W(L"Log", c.log ? L"1" : L"0");
        W(L"MonitorWindow", c.monitorWindow ? L"1" : L"0");
        W(L"TrayIcon", c.trayIcon ? L"1" : L"0");
        W(L"DeviceChannelNames", c.deviceChannelNames ? L"1" : L"0");
        // 0.8.1: Visuals is written only once the user chose it (keeps the remote-session default working)
        static const wchar_t* const kVisW[] = {L"Off", L"Plasma", L"Starfield", L"Copper", L"Tunnel", L"Reactive",
                                               L"Metaballs", L"Fire", L"Vector", L"Twister"};
        if (c.visualsExplicit && c.visuals >= 0 && c.visuals < 10) W(L"Visuals", kVisW[c.visuals]);
        // 0.9.0
        static const wchar_t* const kThW[] = {L"Dark", L"RetroBlue", L"Retro8", L"Terminal", L"Neon", L"Light"};
        static const wchar_t* const kLgW[] = {L"Auto", L"en", L"es", L"pt", L"fr", L"de"};
        W(L"Theme", kThW[c.theme >= 0 && c.theme < 6 ? c.theme : 0]);
        W(L"Language", kLgW[c.language >= 0 && c.language < 6 ? c.language : 0]);
        W(L"UpdateCheck", c.updateCheck ? L"1" : L"0");
        W(L"HelpTips", c.helpTips ? L"1" : L"0");                  // 0.9.2
        if (!c.updateRepo.empty()) W(L"UpdateRepo", c.updateRepo);
        swprintf(b, 64, L"%lld", c.updateLastCheck); W(L"UpdateLastCheck", b);
        swprintf(b, 64, L"%d", c.visualsIntensity); W(L"VisualsIntensity", b);
        W(L"VisualsMonitor", c.visualsMonitor ? L"1" : L"0");
        swprintf(b, 64, L"%d", c.visualsSpeed); W(L"VisualsSpeed", b);
        W(L"MonitorTopmost", c.monitorTopmost ? L"1" : L"0");
        if (c.monitorW > 0 && c.monitorH > 0) {
            swprintf(b, 64, L"%d", c.monitorX); W(L"MonitorX", b);
            swprintf(b, 64, L"%d", c.monitorY); W(L"MonitorY", b);
            swprintf(b, 64, L"%d", c.monitorW); W(L"MonitorW", b);
            swprintf(b, 64, L"%d", c.monitorH); W(L"MonitorH", b);
        }
    } else {
        for (const wchar_t* k : kGeneralOnlyKeys) WritePrivateProfileStringW(sec, k, nullptr, path.c_str());
    }
    if (kind == SecKind::ProfileEngine) {        // profile without devices: drop keys written by an older version
        for (const wchar_t* k : kDeviceKeys) WritePrivateProfileStringW(sec, k, nullptr, path.c_str());
        return;
    }
    W(L"OutputDevice", c.outputDevice);
    W(L"InputDevice", c.inputDevice);
    W(L"LoopbackApp", c.loopbackApp);
    W(L"ExtraOutputs", JoinList(c.extraOutputs));
    W(L"ExtraInputs", JoinList(c.extraInputs));
    W(L"OutputMap", c.outputMap);
    W(L"AlignDevices", c.alignDevices ? L"1" : L"0");
    swprintf(b, 64, L"%.2f", c.outLatOffsetMs); W(L"OutputLatencyOffsetMs", b);
    swprintf(b, 64, L"%.2f", c.inLatOffsetMs); W(L"InputLatencyOffsetMs", b);
    W(L"DirectMonitor", c.dmEnabled ? L"1" : L"0");
    swprintf(b, 64, L"%d", c.dmInput + 1); W(L"DirectMonitorInput", b);
    swprintf(b, 64, L"%d", c.dmOutput + 1); W(L"DirectMonitorOutput", b);
    swprintf(b, 64, L"%.1f", c.dmLevelDb); W(L"DirectMonitorLevelDb", b);
    swprintf(b, 64, L"%.0f", c.dmPan); W(L"DirectMonitorPan", b);
    W(L"DirectMonitorMute", c.dmMute ? L"1" : L"0");
    W(L"ProtectA2dp", c.protectA2dp ? L"1" : L"0");
}

// 0.7.0 (F1). RESET settings: everything that decides which streams are opened and how (devices, formats, buffer
// sizes, channel layout, resampling). Every other key is LIVE (see ARCHITECTURE.txt section 28).
// Compares the two configurations on RESET keys only. Device names compare case-insensitively (they are matched
// as substrings of endpoint names anyway); sample rates compare exactly as doubles (both come from the same
// formatting/parsing path). The caller (WasapiBridge::applyPanelChanges) uses the result to choose between a live
// update and a driver reset requested from the host (kAsioResetRequest).
bool NeedsReset(const Config& a, const Config& b) {
    return a.exclusive != b.exclusive || a.lowLatencyShared != b.lowLatencyShared ||
           _wcsicmp(a.outputDevice.c_str(), b.outputDevice.c_str()) || _wcsicmp(a.inputDevice.c_str(), b.inputDevice.c_str()) ||
           a.loopback != b.loopback || _wcsicmp(a.loopbackApp.c_str(), b.loopbackApp.c_str()) ||
           a.bufferSize != b.bufferSize || a.sampleRate != b.sampleRate || a.drift != b.drift ||
           a.maxChannels != b.maxChannels || a.resampler != b.resampler || a.frEngine != b.frEngine ||   // 0.9.10
           JoinList(a.extraOutputs) != JoinList(b.extraOutputs) || JoinList(a.extraInputs) != JoinList(b.extraInputs) ||
           a.alignDevices != b.alignDevices ||
           a.protectA2dp != b.protectA2dp || a.sampleRateAuto != b.sampleRateAuto ||          // 0.8.0
           a.deviceChannelNames != b.deviceChannelNames;
}

// ---------------------------------------------------------------- profiles (host thread)
static const wchar_t kProfPrefix[] = L"Profile:";
// Legacy literal (Spanish, "profile"): the section prefix written by 0.4.0 to 0.5.1. Kept only so that
// MigrateProfiles can find and convert those sections; never written.
static const wchar_t kOldProfPrefix[] = L"Perfil:";

// Names of all sections in config.ini (the API returns a double-NUL-terminated list).
// GetPrivateProfileSectionNamesW returns the number of characters copied; with more than 32767 characters of
// section names the list is truncated (not expected for a hand-sized config file).
static std::vector<std::wstring> SectionNames(const std::wstring& path) {
    std::vector<std::wstring> out;
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), path.c_str());
    out = cfgp::ParseMultiSz(buf.data(), n, buf.size());   // 0.9.8: bounded by n (parse.h)
    return out;
}

// Note: returns false for a section that exists but has no key=value lines (the API then copies nothing).
static bool SectionExists(const std::wstring& sec, const std::wstring& path) {
    wchar_t probe[8];
    return GetPrivateProfileSectionW(sec.c_str(), probe, 8, path.c_str()) != 0;
}

// A factory profile still "looks factory" when the user never stored devices in it: OutputDevice, InputDevice,
// ExtraOutputs and ExtraInputs are absent or empty. Only then may a migration rename or clean it.
static bool LooksFactory(const std::wstring& sec, const std::wstring& path) {
    static const wchar_t* const keys[] = {L"OutputDevice", L"InputDevice", L"ExtraOutputs", L"ExtraInputs"};
    for (const wchar_t* k : keys) if (!ReadStrS(sec.c_str(), k, L"", path).empty()) return false;
    return true;
}

// Moves every [Perfil:x] section to [Profile:x] without losing any user profile. Factory profiles that still look
// factory get their English name (Estudio -> Studio, Notebook -> Laptop, Streaming -> Streaming) and lose the empty
// device keys that 0.4/0.5 wrote (they used to switch the devices back to the Windows defaults). ActiveProfile is
// updated if it pointed to a renamed profile. Idempotent: after the first run no "Perfil:" section is left.
// Pitfalls handled: a target name that already exists gets " (2)" .. " (99)" appended; if all are taken the old
// section is left untouched. The old section is deleted only after the copy succeeded. The sentinel default
// L"\x01" distinguishes absent keys from empty ones: only keys that are present and empty (plus the
// AlignDevices=1 that 0.4/0.5 wrote with them) are removed from a cleaned factory profile.
static void MigrateProfiles(const std::wstring& path) {
    // Legacy literals: the Spanish factory profile names of 0.4.0 to 0.5.1 ("Estudio" = studio, "Notebook" =
    // laptop), needed only to recognize and rename those profiles.
    static const struct { const wchar_t* oldName; const wchar_t* newName; } factory[] = {
        {L"Estudio", L"Studio"}, {L"Notebook", L"Laptop"}, {L"Streaming", L"Streaming"}};
    const size_t plen = wcslen(kOldProfPrefix);
    bool changed = false;
    for (const std::wstring& sec : SectionNames(path)) {
        if (sec.size() <= plen || _wcsnicmp(sec.c_str(), kOldProfPrefix, plen)) continue;
        const std::wstring name = sec.substr(plen);
        std::wstring target = name;
        bool cleanFactory = false;
        for (auto& f : factory)
            if (!_wcsicmp(name.c_str(), f.oldName) && LooksFactory(sec, path)) { target = f.newName; cleanFactory = true; }
        // Copy the whole section (key=value pairs, double-NUL terminated) to the new name.
        std::vector<wchar_t> data(32768, 0);
        const DWORD n = GetPrivateProfileSectionW(sec.c_str(), data.data(), (DWORD)data.size(), path.c_str());
        std::wstring dst = std::wstring(kProfPrefix) + target;
        for (int k = 2; SectionExists(dst, path) && k < 100; ++k)            // never overwrite an existing profile
            dst = std::wstring(kProfPrefix) + target + L" (" + std::to_wstring(k) + L")";
        if (SectionExists(dst, path)) continue;                             // 98 duplicates: leave the old section alone
        (void)n;                                                            // n == 0: empty section, copied as such
        if (!WritePrivateProfileSectionW(dst.c_str(), data.data(), path.c_str())) continue;   // keep the old one on failure
        if (cleanFactory)
            for (const wchar_t* k : kDeviceKeys) {
                const std::wstring v = ReadStrS(dst.c_str(), k, L"\x01", path);
                if (v.empty() || (!wcscmp(k, L"AlignDevices") && v == L"1")) WritePrivateProfileStringW(dst.c_str(), k, nullptr, path.c_str());
            }
        WritePrivateProfileStringW(sec.c_str(), nullptr, nullptr, path.c_str());   // delete the old section
        if (!_wcsicmp(ReadStrS(L"General", L"ActiveProfile", L"", path).c_str(), name.c_str()))
            WritePrivateProfileStringW(L"General", L"ActiveProfile", dst.substr(wcslen(kProfPrefix)).c_str(), path.c_str());
        changed = true;
    }
    if (changed) WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

// Factory profiles: created once (marker ProfilesInit=1 in [General]) and only if there is no profile at all.
// Engine settings only (0.6.0): applying a factory profile never changes the devices.
// Called from LoadConfig on every load (host thread). Order: migrate old sections first, then create the factory
// profiles once. ProfilesInit=1 is written even if user profiles already existed, so deleting all profiles later
// does not bring the factory ones back. Ends with a cache flush.
static void EnsureDefaultProfiles(const std::wstring& path) {
    MigrateProfiles(path);
    if (ReadStrS(L"General", L"ProfilesInit", L"0", path) != L"1") {
        if (ListProfiles().empty()) {
            Config st;  st.exclusive = true;  st.bufferSize = 128; st.lowLatencyShared = true;
            Config nb;  nb.exclusive = false; nb.bufferSize = 256;
            Config sm;  sm.exclusive = false; sm.bufferSize = 512; sm.loopback = LoopbackMode::ExcludeDaw;
            WriteSection(L"Profile:Studio", st, path, SecKind::ProfileEngine);
            WriteSection(L"Profile:Laptop", nb, path, SecKind::ProfileEngine);
            WriteSection(L"Profile:Streaming", sm, path, SecKind::ProfileEngine);
        }
        WritePrivateProfileStringW(L"General", L"ProfilesInit", L"1", path.c_str());
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
}

// Host thread (driver init / reload and the panel). Sequence: ensure the file exists, run migrations, reset `c`
// to built-in defaults and overlay [General]. The active profile name is only read here, not applied: the caller
// decides whether and how to apply it (ActiveProfile / host profile). A brand-new file is then fully written by
// SaveConfig; an existing one only gets its comment block normalized.
void LoadConfig(Config& c) {
    std::wstring path = ConfigPath();
    bool isNew = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    EnsureFile(path);
    EnsureDefaultProfiles(path);          // migrations first (they may update ActiveProfile)
    c = Config{};                         // [General] on top of the built-in defaults
    ReadSection(L"General", c, path);
    c.activeProfile = ReadStrS(L"General", L"ActiveProfile", L"", path);
    // 0.8.0 (G6): device channel names change the port names a DAW has saved. An existing config.ini without the
    // key (upgrade from 0.7.x or older) resolves to 0 (old names kept); only a brand-new config.ini gets 1. The
    // resolved value is written at once, so the choice is stable from then on.
    if (!isNew && ReadStrS(L"General", L"DeviceChannelNames", L"?", path) == L"?") {
        c.deviceChannelNames = false;
        WritePrivateProfileStringW(L"General", L"DeviceChannelNames", L"0", path.c_str());
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    }
    if (isNew) SaveConfig(c);
    else RewriteConfigFile(path);         // English comments (SaveConfig already does it for a new file)
}

// Host thread. Writes every [General] key from `c` (profiles are untouched), flushes and normalizes.
void SaveConfig(const Config& c) {
    std::wstring path = ConfigPath();
    EnsureFile(path);
    WriteSection(L"General", c, path);
    WritePrivateProfileStringW(L"General", L"ActiveProfile", c.activeProfile.c_str(), path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());   // flush the profile cache
    RewriteConfigFile(path);
}

// Profile names in file order, without the "Profile:" prefix. Legacy "Perfil:" sections are not listed (they
// are migrated by LoadConfig).
std::vector<std::wstring> ListProfiles() {
    std::vector<std::wstring> out;
    const size_t plen = wcslen(kProfPrefix);
    for (const std::wstring& s : SectionNames(ConfigPath()))
        if (s.size() > plen && !_wcsnicmp(s.c_str(), kProfPrefix, plen)) out.push_back(s.substr(plen));
    return out;
}

// Profile names become part of a section header "[Profile:<name>]" (rules in parse.h, 0.9.8).
bool ValidProfileName(const std::wstring& n) { return cfgp::ValidProfileName(n); }

// Overlays the profile section on `c`: keys the profile does not contain keep their current value (so a profile
// without devices keeps the current devices). See ReadSection for the UpdateLastCheck exception.
bool LoadProfile(const std::wstring& name, Config& c) {
    if (!ValidProfileName(name)) return false;
    std::wstring path = ConfigPath();
    std::wstring sec = kProfPrefix + name;
    if (!SectionExists(sec, path)) return false;   // missing or empty
    ReadSection(sec.c_str(), c, path);
    return true;
}

// Invalid names are ignored silently (the panel validates before calling). Saving with includeDevices=false also
// removes device keys that an earlier save with devices had written.
void SaveProfile(const std::wstring& name, const Config& c, bool includeDevices) {
    if (!ValidProfileName(name)) return;
    std::wstring path = ConfigPath();
    EnsureFile(path);
    WriteSection((kProfPrefix + name).c_str(), c, path, includeDevices ? SecKind::ProfileDevices : SecKind::ProfileEngine);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    RewriteConfigFile(path);
}

// 0.7.0 (F2). Host thread. Writes only the placement keys (the rest of [General] is untouched), then normalizes
// the file like every other writer.
void SaveMonitorPlacement(int x, int y, int w, int h, bool topmost) {
    std::wstring path = ConfigPath();
    EnsureFile(path);
    wchar_t b[32];
    swprintf(b, 32, L"%d", x); WritePrivateProfileStringW(L"General", L"MonitorX", b, path.c_str());
    swprintf(b, 32, L"%d", y); WritePrivateProfileStringW(L"General", L"MonitorY", b, path.c_str());
    swprintf(b, 32, L"%d", w); WritePrivateProfileStringW(L"General", L"MonitorW", b, path.c_str());
    swprintf(b, 32, L"%d", h); WritePrivateProfileStringW(L"General", L"MonitorH", b, path.c_str());
    WritePrivateProfileStringW(L"General", L"MonitorTopmost", topmost ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    RewriteConfigFile(path);
}

// 0.8.0. Host thread. One key of [General], then the usual normalization.
void SaveGeneralFlag(const wchar_t* key, bool on) {
    std::wstring path = ConfigPath();
    EnsureFile(path);
    WritePrivateProfileStringW(L"General", key, on ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    RewriteConfigFile(path);
}

void SaveGeneralInt64(const wchar_t* key, long long v) {
    std::wstring path = ConfigPath();
    EnsureFile(path);
    wchar_t b[32]; swprintf(b, 32, L"%lld", v);
    WritePrivateProfileStringW(L"General", key, b, path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    RewriteConfigFile(path);
}

// Profile/device names for log.txt: ASCII whitelist (rules in parse.h, 0.9.8).
std::string SafeLogName(const std::wstring& name) { return cfgp::SafeLogName(name); }

// ---------------------------------------------------------------- host profiles (0.8.0, G3)
// Executable file name of the host process (e.g. "Reaper.exe"); empty if the path does not fit in MAX_PATH.
std::wstring HostExeName() {
    wchar_t p[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);       // the process executable, not this DLL
    if (!n || n >= MAX_PATH) return L"";
    const wchar_t* slash = wcsrchr(p, L'\\');
    return slash ? slash + 1 : p;
}

// The exe name is a key of [HostProfiles] (rules in parse.h, 0.9.8).
using cfgp::ValidExeKey;

// Profile name assigned to a host executable, or empty. Lookup is case-insensitive (profile API behavior).
std::wstring HostProfileFor(const std::wstring& exe) {
    if (!ValidExeKey(exe)) return L"";
    return ReadStrS(L"HostProfiles", exe.c_str(), L"", ConfigPath());     // the profile API is case-insensitive
}

// An empty `profile` removes the assignment (nullptr value deletes the key).
void SetHostProfile(const std::wstring& exe, const std::wstring& profile) {
    if (!ValidExeKey(exe)) return;
    std::wstring path = ConfigPath();
    EnsureFile(path);
    WritePrivateProfileStringW(L"HostProfiles", exe.c_str(), profile.empty() ? nullptr : profile.c_str(), path.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    RewriteConfigFile(path);
}

// All [HostProfiles] entries as (exe, profile) pairs in file order. Same truncation limit as SectionNames.
std::vector<std::pair<std::wstring, std::wstring>> ListHostProfiles() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionW(L"HostProfiles", buf.data(), (DWORD)buf.size(), ConfigPath().c_str());
    for (const std::wstring& line : cfgp::ParseMultiSz(buf.data(), n, buf.size())) {   // 0.9.8: bounded (parse.h)
        std::wstring exe, prof;
        if (cfgp::ParseHostProfileEntry(line, exe, prof)) out.push_back({exe, prof});
    }
    return out;
}

// True if at least one device key is present in the profile (sentinel default "\x01" = absent).
bool ProfileHasDevices(const std::wstring& name) {
    if (!ValidProfileName(name)) return false;
    std::wstring path = ConfigPath(), sec = kProfPrefix + name;
    for (const wchar_t* k : kDeviceKeys) if (ReadStrS(sec.c_str(), k, L"\x01", path) != L"\x01") return true;
    return false;
}

// Deletes the whole [Profile:<name>] section (nullptr key). ActiveProfile and [HostProfiles] entries that refer
// to it are not changed here.
void DeleteProfile(const std::wstring& name) {
    if (!ValidProfileName(name)) return;
    std::wstring path = ConfigPath();
    WritePrivateProfileStringW((kProfPrefix + name).c_str(), nullptr, nullptr, path.c_str());   // deletes the section
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    RewriteConfigFile(path);
}

// ---------------------------------------------------------------- log
// Log state. g_logCs is initialized lazily by the first LogInit and never deleted (lives until process exit or
// DLL unload). Pitfall: the g_logInit check itself is not synchronized, so the first LogInit must happen before
// any other thread can call Log (it does: driver init on the host thread).
static CRITICAL_SECTION g_logCs;
static bool  g_logInit = false;
static FILE* g_logFile = nullptr;

// UTF-16 -> UTF-8 for the log; returns an empty string on conversion failure.
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Host thread. (Re)opens the log: rotates log.txt to log-previous.txt (replacing the older one) and creates an
// empty log.txt; enabled=false just closes the current file. Calling it again within the same process (driver
// re-init) rotates again. Errors are ignored: g_logFile stays null and Log() becomes a no-op.
void LogInit(bool enabled) {
    if (!g_logInit) { InitializeCriticalSection(&g_logCs); g_logInit = true; }
    EnterCriticalSection(&g_logCs);
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    if (enabled) {
        // log.txt = current session; log-previous.txt = previous session (survives a host reset of the driver)
        std::wstring d = ConfigDir();
        MoveFileExW((d + L"\\log.txt").c_str(), (d + L"\\log-previous.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_logFile = _wfopen((d + L"\\log.txt").c_str(), L"w");
    }
    LeaveCriticalSection(&g_logCs);
}

// Never call from the audio thread (lock + fflush).
// Any non-audio thread. Lines are "hh:mm:ss.mmm  <text>" in local time, formatted into a fixed 2 KiB buffer
// (longer messages are truncated), interpreted as UTF-8, passed through SanitizeText (privacy.h) and flushed
// immediately so the log survives a crash of the host.
// 0.9.8 (exception barrier): Log is called from every entry point, including the catch handlers of the barrier
// itself, so it must never throw. The critical section is released by a scope guard, and a failed allocation while
// sanitizing (std::bad_alloc) only loses that one line (the raw text is NOT written instead: it could contain
// user names that SanitizeText would have masked).
void Log(const char* fmt, ...) noexcept {
    if (!g_logInit || !fmt) return;
    EnterCriticalSection(&g_logCs);
    struct Leave { ~Leave() { LeaveCriticalSection(&g_logCs); } } leave;
    if (!g_logFile) return;
    SYSTEMTIME t; GetLocalTime(&t);
    // 0.6.1: user folders, user name and computer name are masked before the line is written (SanitizeText).
    char msg[2048];
    va_list a; va_start(a, fmt); vsnprintf(msg, sizeof msg, fmt, a); va_end(a);
    try {
        const int n = MultiByteToWideChar(CP_UTF8, 0, msg, -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, msg, -1, &w[0], n);
        const std::string line = Utf8(SanitizeText(w));
        fprintf(g_logFile, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        fputs(line.c_str(), g_logFile);
        fputc('\n', g_logFile); fflush(g_logFile);
    } catch (...) {
        // out of memory: the line is dropped (see above); nothing may escape into the caller
    }
}

// 0.9.5: the rate the host last set with setSampleRate ([General] LastHostRate; 0 = none). Read at every init /
// reload and written on setSampleRate; cleared when the user changes Sample rate in the panel. Host thread only.
// Value is stored with no decimals. Neither function calls RewriteConfigFile nor flushes the profile cache
// explicitly; the key is ordinary private-profile data in [General] and is not part of WriteSection.
double ReadLastHostRate() {
    const std::wstring v = ReadStrS(L"General", L"LastHostRate", L"", ConfigPath());
    const double r = v.empty() ? 0.0 : wcstod(v.c_str(), nullptr);
    return std::isfinite(r) && r > 0 ? r : 0.0;          // 0.9.6 (D4): "nan" / "inf" / negative mean "none"
}
void WriteLastHostRate(double r) {
    const std::wstring path = ConfigPath();
    if (r <= 0) { WritePrivateProfileStringW(L"General", L"LastHostRate", nullptr, path.c_str()); return; }
    wchar_t b[32]; swprintf(b, 32, L"%.0f", r);
    WritePrivateProfileStringW(L"General", L"LastHostRate", b, path.c_str());
}
