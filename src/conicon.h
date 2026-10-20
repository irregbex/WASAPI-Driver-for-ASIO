// WASAPI Driver for ASIO - console window icon of the tools (0.9.3, header-only)
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
// WasapiDriverDiag.exe and WasapiDriverTest.exe show the application icon (ICON 1 of src/tools.rc) on their
// console window while they run and put the previous icons back when they end (the console may belong to a
// command prompt that started them). Windows Terminal ignores console icons; nothing breaks there.
// Main thread only.
//
// Usage: RAII. Declare one ConsoleIcon at the start of main(); the constructor sets the icons, the destructor
// restores the previous ones (if the window still exists) and frees the loaded icons. Every step is best effort:
// no console window (GUI start, redirected output) or a failed LoadImage simply leaves the icon unchanged.
// Pitfall: the old icons are restored only for the sizes that were actually replaced (big_ / small_ non-null),
// and they are not owned by this object (never destroyed here).
#pragma once
#include <windows.h>

class ConsoleIcon {
public:
    ConsoleIcon() {
        w_ = GetConsoleWindow();
        if (!w_) return;
        oldBig_ = (HICON)SendMessageW(w_, WM_GETICON, ICON_BIG, 0);
        oldSmall_ = (HICON)SendMessageW(w_, WM_GETICON, ICON_SMALL, 0);
        HINSTANCE me = GetModuleHandleW(nullptr);
        big_ = (HICON)LoadImageW(me, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        small_ = (HICON)LoadImageW(me, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (big_) SendMessageW(w_, WM_SETICON, ICON_BIG, (LPARAM)big_);
        if (small_) SendMessageW(w_, WM_SETICON, ICON_SMALL, (LPARAM)small_);
    }
    ~ConsoleIcon() {
        if (w_ && IsWindow(w_)) {
            if (big_) SendMessageW(w_, WM_SETICON, ICON_BIG, (LPARAM)oldBig_);
            if (small_) SendMessageW(w_, WM_SETICON, ICON_SMALL, (LPARAM)oldSmall_);
        }
        if (big_) DestroyIcon(big_);
        if (small_) DestroyIcon(small_);
    }
    // Not copyable: two copies would destroy the same HICONs twice.
    ConsoleIcon(const ConsoleIcon&) = delete;
    ConsoleIcon& operator=(const ConsoleIcon&) = delete;
private:
    HWND w_ = nullptr;                    // console window (GetConsoleWindow), null if none
    // previous icons (borrowed, restored on exit) and our own loaded icons (owned, destroyed on exit)
    HICON oldBig_ = nullptr, oldSmall_ = nullptr, big_ = nullptr, small_ = nullptr;
};
