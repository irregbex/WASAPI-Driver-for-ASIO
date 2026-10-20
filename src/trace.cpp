// WASAPI Driver for ASIO - TraceLogging (ETW) provider of the driver DLL
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
//   Registers the provider declared in trace.h with ETW (EventRegister + the provider traits through
//   EventSetInformation, as TraceLoggingRegister does), tracks which level and keywords the listening sessions
//   enabled (enable callback -> atomics), and writes events encoded by tlg.h with EventWriteTransfer.
//
// Life cycle
//   Register() runs on the first driver object or class-factory request (never inside DllMain: the loader lock is
//   held there), once per DLL load (INIT_ONCE). Unregister() runs in DllMain DLL_PROCESS_DETACH when the DLL is
//   unloaded dynamically: after that, ETW must never call the enable callback again, because its code is about to
//   be unmapped. Every thread that can still write events (update worker, dump thread, loopback activation) holds
//   its own reference to the DLL, so no event can be in flight during the unload. At process exit (DllMain with a
//   non-null `reserved`) nothing is done: the process is ending and ETW cleans up.
//
// Cost when nobody listens
//   Enabled() is one relaxed atomic load of g_level (-1 = disabled). Call sites use WB_TRACE, which evaluates the
//   field arguments (and any string conversion) only after Enabled() returned true.
//
// Threading
//   The enable callback runs on an ETW thread; it only stores atomics. Write() runs on the calling thread with stack
//   buffers only. Never called from the audio thread (rule of the driver, see trace.h).
#include "common.h"
#include "trace.h"
#include "tlg.h"
#include "privacy.h"
#include <evntprov.h>
#include <atomic>

namespace {

const GUID kProvider = WB_TRACE_PROVIDER_GUID_INIT;

REGHANDLE              g_reg = 0;
INIT_ONCE              g_once = INIT_ONCE_STATIC_INIT;
std::atomic<int>       g_level{-1};                 // -1: no session; otherwise the enabled level (255 = all levels)
std::atomic<uint64_t>  g_any{0}, g_all{0};          // MatchAnyKeyword / MatchAllKeyword of the sessions
uint8_t                g_provMeta[64];              // provider traits blob (UINT16 size + name)
size_t                 g_provMetaSize = 0;

// ETW enable callback (any thread). IsEnabled: 0 = disable, 1 = enable, 2 = capture state (ignored). The values are
// the combination of every session that enabled this provider.
void NTAPI EnableCallback(LPCGUID, ULONG isEnabled, UCHAR level, ULONGLONG any, ULONGLONG all,
                          PEVENT_FILTER_DESCRIPTOR, PVOID) {
    if (isEnabled == 0) { g_level.store(-1, std::memory_order_release); return; }
    if (isEnabled != 1) return;
    g_any.store(any, std::memory_order_relaxed);
    g_all.store(all, std::memory_order_relaxed);
    g_level.store(level == 0 ? 255 : (int)level, std::memory_order_release);   // level 0 = every level
}

BOOL CALLBACK RegisterOnce(PINIT_ONCE, PVOID, PVOID*) {
    g_provMetaSize = tlg::ProviderMeta(g_provMeta, sizeof g_provMeta, WB_TRACE_PROVIDER_NAME);
    REGHANDLE h = 0;
    if (EventRegister(&kProvider, EnableCallback, nullptr, &h) != ERROR_SUCCESS) return TRUE;   // tracing stays off
    // Provider traits (the provider name for decoders); EventProviderSetTraits = 2. Failure is harmless: each event
    // also carries the provider metadata.
    // Resolved at run time: the 32-bit mingw import library of advapi32 does not export it (Windows 8+ API).
    typedef ULONG (WINAPI* PFN_EventSetInformation)(REGHANDLE, EVENT_INFO_CLASS, PVOID, ULONG);
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    auto setInfo = adv ? (PFN_EventSetInformation)(void*)GetProcAddress(adv, "EventSetInformation") : nullptr;
    if (setInfo && g_provMetaSize) setInfo(h, (EVENT_INFO_CLASS)2, g_provMeta, (ULONG)g_provMetaSize);
    g_reg = h;
    return TRUE;
}

}  // namespace

namespace wbtrace {

bool Enabled(uint8_t level, uint64_t kw) noexcept {
    const int l = g_level.load(std::memory_order_acquire);
    if (l < 0 || !g_reg || level > l) return false;
    const uint64_t any = g_any.load(std::memory_order_relaxed), all = g_all.load(std::memory_order_relaxed);
    // ETW rules: MatchAnyKeyword 0 = every keyword; an event must match one bit of Any and every bit of All.
    if (kw && any && !(kw & any)) return false;
    return (kw & all) == all;
}

void Register() noexcept { InitOnceExecuteOnce(&g_once, RegisterOnce, nullptr, nullptr); }

void Unregister() noexcept {
    g_level.store(-1, std::memory_order_release);
    if (g_reg) { EventUnregister(g_reg); g_reg = 0; }
}

void Write(const char* event, uint8_t level, uint64_t kw, std::initializer_list<Field> fields) noexcept {
    if (!Enabled(level, kw) || !g_provMetaSize) return;
    tlg::Event<512, 1024> e(event);
    for (const Field& f : fields) {
        switch (f.kind) {
        case Field::I32:     e.i32(f.name, (int32_t)f.v.i); break;
        case Field::U32:     e.u32(f.name, (uint32_t)f.v.u); break;
        case Field::I64:     e.i64(f.name, f.v.i); break;
        case Field::U64:     e.u64(f.name, f.v.u); break;
        case Field::F64:     e.f64(f.name, f.v.d); break;
        case Field::Bool:    e.boolean(f.name, f.v.u != 0); break;
        case Field::Str:     e.str(f.name, f.v.s); break;
        case Field::HResult: e.hresult(f.name, (int32_t)f.v.i); break;
        }
    }
    if (!e.finish()) return;                     // too large: dropped rather than truncated metadata
    EVENT_DESCRIPTOR d = {};
    d.Channel = tlg::kChannelTraceLogging;
    d.Level = level;
    d.Keyword = kw;
    EVENT_DATA_DESCRIPTOR dd[3];
    EventDataDescCreate(&dd[0], g_provMeta, (ULONG)g_provMetaSize); dd[0].Reserved = 2;   // provider metadata
    EventDataDescCreate(&dd[1], e.meta(), (ULONG)e.metaSize());     dd[1].Reserved = 1;   // event metadata
    EventDataDescCreate(&dd[2], e.data(), (ULONG)e.dataSize());
    EventWriteTransfer(g_reg, &d, nullptr, nullptr, e.dataSize() ? 3 : 2, dd);
}

std::string SafeText(const std::wstring& w) noexcept {
    try {
        std::string s = Utf8(SanitizeText(w.size() > 200 ? w.substr(0, 200) : w));
        return s;
    } catch (...) {
        return std::string();                    // empty std::string construction does not allocate
    }
}

}  // namespace wbtrace
