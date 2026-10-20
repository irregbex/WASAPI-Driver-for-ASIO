// WASAPI Driver for ASIO - per-user settings folder (header-only, 0.9.6)
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
// Purpose (0.9.6, CODE-REVIEW D6 and independent review R7d)
//   One definition of WHERE the settings folder lives, shared by the driver (config.cpp ConfigDir, which reads
//   and writes config.ini and log.txt) and the diagnostics tool (diag.cpp AppDataDir, which only reads them), so
//   the tool always looks in the folder the driver really used. Fallback chain, first one that resolves wins:
//     1. %APPDATA%       (CSIDL_APPDATA, roaming: the normal case, settings follow the user)
//     2. %LOCALAPPDATA%  (CSIDL_LOCAL_APPDATA: profile without a usable roaming folder)
//     3. %TEMP%          (GetTempPathW: the user's temporary folder)
//   The result never ends with a separator. An empty result means no per-user folder exists at all; callers must
//   then avoid "\" + name, which would be a folder in the ROOT of the current drive (the 0.9.5 behaviour, D6).
//
// Threading
//   Pure Win32 queries, no state: any thread. Nothing is created here (the caller decides).
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>

// Base folder (without the product name) and, in *roaming, whether it is the real roaming AppData folder (only
// then does config.cpp run its one-time migration from the old WasapiBridge folder).
inline std::wstring WbSettingsBase(bool* roaming = nullptr) {
    if (roaming) *roaming = false;
    wchar_t p[MAX_PATH] = L"";
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, p)) && p[0]) {
        if (roaming) *roaming = true;
        return p;
    }
    std::wstring base;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p)) && p[0]) base = p;
    else if (GetTempPathW(MAX_PATH, p) && p[0]) base = p;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    return base;
}
