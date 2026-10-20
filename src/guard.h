// WASAPI Driver for ASIO - exception barrier for every entry point called by the host or by Windows
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
// Purpose (0.9.8, hardening)
//   The driver is C++ (the standard library allocates and may throw std::bad_alloc or std::length_error) but every
//   function the outside world calls is a C / COM / Win32 boundary: IASIO and IUnknown methods (host), the class
//   factory and the four DLL exports (COM, regsvr32), window procedures, hook and enumeration callbacks (USER32),
//   COM notification callbacks (MMDevice, endpoint volume, audio interface activation) and thread procedures. A
//   C++ exception crossing such a boundary is undefined behavior: with mingw it reaches std::terminate (or unwinds
//   through foreign frames) and takes the host process down. -fno-exceptions is not an option (the standard
//   library containers report allocation failure only by throwing), so every boundary catches everything and
//   turns it into the boundary's own error value:
//     IASIO -> ASE_HWMalfunction (ASIOFalse for init), COM -> E_FAIL / E_OUTOFMEMORY, window procedures ->
//     DefWindowProcW, hooks -> CallNextHookEx, enumeration callbacks -> stop enumerating, threads -> clean exit.
//   NoteException logs the first 16 contained exceptions (and a final note), and emits an ETW event.
//
// The audio thread
//   Nothing on the real-time path allocates (see driver.h), so nothing there can throw; the only guard is at the
//   thread procedure itself (WasapiBridge::threadProc), outside the loop, which costs nothing while no exception
//   is in flight (table-based unwinding) and ends the thread cleanly if one ever escapes from a host callback that
//   was compiled with a compatible runtime.
//
// Threading
//   NoteException may be called from any thread except the real-time loop (it logs).
#pragma once
#include "common.h"
#include "trace.h"
#include <atomic>

// Records one contained exception at boundary `where` (string literal). Never throws.
inline void NoteException(const char* where) noexcept {
    static std::atomic<int> count{0};
    const int n = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16) Log("ERROR: C++ exception contained at the boundary of %s (the call failed safely, #%d)", where, n);
    else if (n == 17) Log("ERROR: more contained exceptions are not logged");
    WB_TRACE(wbtrace::kLvlError, wbtrace::kKwError, "ExceptionContained", wbtrace::Str("Where", where), wbtrace::I32("Count", n));
}

// Window procedure wrapper: GuardedWndProc<MyProc> is registered instead of MyProc.
template <LRESULT (CALLBACK* P)(HWND, UINT, WPARAM, LPARAM)>
LRESULT CALLBACK GuardedWndProc(HWND h, UINT m, WPARAM w, LPARAM l) noexcept {
    try { return P(h, m, w, l); }
    catch (...) { NoteException("a window procedure"); return DefWindowProcW(h, m, w, l); }
}

// EnumWindows / EnumChildWindows callback wrapper: an exception stops the enumeration.
template <BOOL (CALLBACK* P)(HWND, LPARAM)>
BOOL CALLBACK GuardedEnumProc(HWND h, LPARAM l) noexcept {
    try { return P(h, l); }
    catch (...) { NoteException("a window enumeration callback"); return FALSE; }
}
