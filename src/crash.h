// WASAPI Driver for ASIO - crash protection: minidump on a fatal exception in this DLL, safe-mode marker
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
// Purpose (0.7.0, F8)
//   A crash inside an ASIO driver takes the whole DAW down. This module cannot prevent that (faults are never
//   swallowed: mingw has no __try, and continuing after an access violation would corrupt the host), but it
//   records what happened so the next session can start safely and the user can send a report:
//     - a vectored exception handler (AddVectoredExceptionHandler, not first in the chain) reacts only to fatal
//       exception codes whose faulting address lies inside this DLL;
//     - it hands the EXCEPTION_POINTERS to a dedicated, pre-created dump thread (writing a minidump from the
//       faulting thread itself is unreliable, e.g. after a stack overflow) and waits for it (max 5 s; 0.9.6, D1:
//       this comment said 15 s, the handler in crash.cpp has always waited 5 s);
//     - the dump thread writes %LOCALAPPDATA%\WASAPI Driver for ASIO\crash\crash-<yyyymmdd-hhmmss>.dmp with
//       MiniDumpWriteDump (dbghelp.dll loaded with LoadLibraryEx from System32, never an import) and a marker
//       file crash-marker.txt (version, exception code, module offset);
//     - the handler then returns EXCEPTION_CONTINUE_SEARCH: the host and Windows decide what happens next.
//   At the next init() the driver sees the marker and starts in safe mode for that session (see driver.cpp).
//
// Threading
//   CrashInstall / CrashUninstall / marker functions: host thread. The handler runs on the faulting thread and
//   uses only preallocated static data and kernel objects created at install time. The dump thread holds a
//   reference to the DLL and ends with FreeLibraryAndExitThread, so the DLL can unload safely after uninstall.
#pragma once
#include <string>
#include <vector>

// Installs the handler and the dump thread once per process (reference counted: one per driver object). Also
// keeps at most 5 dumps in the crash folder (oldest deleted). Host thread.
void CrashInstall() noexcept;     // 0.9.8: never throws (exception barrier)
// Releases one reference; the last one removes the handler and lets the dump thread exit. Host thread.
void CrashUninstall() noexcept;   // 0.9.8: never throws
// %LOCALAPPDATA%\WASAPI Driver for ASIO\crash (not created).
std::wstring CrashDir();
// True if crash-marker.txt exists; `info` receives its first line (version and exception code).
bool CrashMarkerPresent(std::wstring* info = nullptr);
// Deletes crash-marker.txt (the dumps are kept for the user to send).
void CrashClearMarker();
// Fix 4. Session tracking: Begin writes crash\session-<pid>.txt (init), End deletes it (clean destruction).
// CrashUncleanExitFound() is true if a session file of a process that is no longer running exists (it is deleted).
// 0.9.6 (D3): the session file also stores the process creation time, so a reused pid is recognized, and a process
// that exists but cannot be opened (access denied) is treated as alive.
void CrashSessionBegin();
void CrashSessionEnd();
bool CrashUncleanExitFound();

