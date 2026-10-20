// WASAPI Driver for ASIO - TraceLogging (ETW) instrumentation: provider, levels, keywords and the event API
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
// Purpose (0.9.8)
//   The driver DLL is an ETW provider of self-describing (TraceLogging) events, so a user can record what the
//   driver did with Windows Performance Recorder (Source-Code/trace/WasapiDriver.wprp, TRACING.txt) and attach the
//   .etl file to a bug report. Nothing is recorded unless a trace session is running: every event first checks
//   Enabled(), which is one atomic load when nobody listens, and the payload is only built after that check.
//
// Provider
//   Name "WASAPIDriverForASIO" (this project's own name; never a Microsoft provider name or GUID).
//   GUID {52b9f111-957d-5973-b76d-97bdf8f813df}, derived from the name with the TraceLogging / EventSource rule
//   (tlg::GuidFromName; tests/trace_test.cpp checks the constant). To verify on Windows (PowerShell):
//     [System.Diagnostics.Tracing.EventSource]::new("WASAPIDriverForASIO").Guid
//   (python3: see TRACING.txt; SHA-1 over the namespace 482C2DB2-C390-47C8-87F8-1A15BFC130FB + "WASAPIDRIVERFORASIO"
//   as UTF-16BE, byte 7 = (byte 7 & 0x0F) | 0x50).
//
// What is traced (see TRACING.txt for the user-facing list)
//   Driver load / unload, init / exit, every IASIO call made on a non-real-time thread (name and result),
//   device resolution, stream opening (mode, rates, period, buffer, sample format, conversion), resets and their
//   reasons, device-change notifications (device Ids only as a short hash), errors with their HRESULT, contained
//   exceptions, a statistics summary every 10 s while running (counters only), crash-protection events, panel
//   open / apply and update-check results.
//   NEVER traced: audio samples or any audio-derived data (levels, meters), file paths, user or computer names
//   (device names pass through SanitizeText, which masks them), the host's window titles, network addresses.
//   The real-time audio thread never writes an event: it only maintains counters, which the host thread
//   summarizes (event "Stats").
//
// Threading
//   Enabled() and Write() may be called from any thread except the audio thread; Write() uses only the stack
//   (about 1.6 KB) and never throws or allocates. Register() / Unregister(): see trace.cpp.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>

#define WB_TRACE_PROVIDER_NAME "WASAPIDriverForASIO"
// {52b9f111-957d-5973-b76d-97bdf8f813df} = name hash of WB_TRACE_PROVIDER_NAME (see above)
#define WB_TRACE_PROVIDER_GUID_INIT {0x52b9f111, 0x957d, 0x5973, {0xb7, 0x6d, 0x97, 0xbd, 0xf8, 0xf8, 0x13, 0xdf}}

namespace wbtrace {

// ETW levels (winmeta.h values).
enum : uint8_t { kLvlCritical = 1, kLvlError = 2, kLvlWarning = 3, kLvlInfo = 4, kLvlVerbose = 5 };
// Keywords (bit mask; a session can enable a subset, see TRACING.txt).
enum : uint64_t {
    kKwLifecycle = 0x1,     // DLL load/unload, driver object creation/destruction, init
    kKwAsio      = 0x2,     // IASIO calls and their results
    kKwDevice    = 0x4,     // device resolution and Windows device-change notifications
    kKwStream    = 0x8,     // stream opening / closing, engine and conversion choices
    kKwReset     = 0x10,    // reset requests and their reasons
    kKwError     = 0x20,    // errors (with HRESULT) and contained exceptions
    kKwStats     = 0x40,    // periodic counter summary while running
    kKwCrash     = 0x80,    // crash protection, safe mode
    kKwPanel     = 0x100,   // control panel open / apply
    kKwUpdate    = 0x200    // optional update check results
};

// One field of an event. Built with the helpers below; a string field only points to the caller's text, which must
// stay valid until Write returns (temporaries in the same full expression do).
struct Field {
    enum Kind : uint8_t { I32, U32, I64, U64, F64, Bool, Str, HResult } kind;
    const char* name;
    union { int64_t i; uint64_t u; double d; const char* s; } v;
};
inline Field I32(const char* n, int32_t x)  { Field f{Field::I32, n, {}}; f.v.i = x; return f; }
inline Field U32(const char* n, uint32_t x) { Field f{Field::U32, n, {}}; f.v.u = x; return f; }
inline Field I64(const char* n, int64_t x)  { Field f{Field::I64, n, {}}; f.v.i = x; return f; }
inline Field U64(const char* n, uint64_t x) { Field f{Field::U64, n, {}}; f.v.u = x; return f; }
inline Field F64(const char* n, double x)   { Field f{Field::F64, n, {}}; f.v.d = x; return f; }
inline Field Bool(const char* n, bool x)    { Field f{Field::Bool, n, {}}; f.v.u = x ? 1 : 0; return f; }
inline Field Str(const char* n, const char* s) { Field f{Field::Str, n, {}}; f.v.s = s; return f; }
inline Field HR(const char* n, long hr)     { Field f{Field::HResult, n, {}}; f.v.i = (int32_t)hr; return f; }

// True if a session listens to this provider at `level` with one of the keywords in `kw` (one atomic load when
// nobody listens). Always false before Register().
bool Enabled(uint8_t level, uint64_t kw) noexcept;
// Writes one event (no-op unless Enabled). Never throws, never allocates.
void Write(const char* event, uint8_t level, uint64_t kw, std::initializer_list<Field> fields) noexcept;
// Registers the provider once per DLL load (thread-safe, idempotent; never from DllMain).
void Register() noexcept;
// Unregisters it (DllMain DLL_PROCESS_DETACH on a dynamic unload: ETW must not call into unloaded code).
void Unregister() noexcept;
// Device names and similar text for an event: UTF-8, passed through SanitizeText (user and computer names masked),
// at most 200 characters. Returns "" on any failure (never throws). Allocates: only call after Enabled().
std::string SafeText(const std::wstring& w) noexcept;

}  // namespace wbtrace

// Usage: WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwStream, "StreamOpen", wbtrace::U32("Period", p), ...);
// The field arguments are only evaluated when a session listens.
#define WB_TRACE(level, kw, name, ...) \
    do { if (::wbtrace::Enabled((level), (kw))) ::wbtrace::Write((name), (level), (kw), {__VA_ARGS__}); } while (0)
