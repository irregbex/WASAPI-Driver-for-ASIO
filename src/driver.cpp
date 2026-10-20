// WASAPI Driver for ASIO - IASIO implementation over WASAPI (driver core)
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
//   Implements the ASIO driver object: device resolution, WASAPI stream setup (Shared, Shared low-latency through
//   IAudioClient3, Exclusive), the real-time audio thread, drift correction for inputs and additional outputs,
//   direct monitoring, loopback capture (device and per-process), output routing, latency alignment, metering and
//   the marshalling of host notifications.
//   0.7.0: LIVE settings published to the audio thread (SeqPublished<LiveParams>, applyPanelChanges /
//   publishLive), latency offsets in getLatencies (kAsioLatenciesChanged through WM_WB_LATCHG), the direct
//   monitoring mixer (mixMonitor, click-free ramps, soft clipper), the monitor window life cycle
//   (updateMonitorWindow), crash handling (CrashInstall in the constructor, safe mode in init) and validation of
//   every host-provided pointer and index.
//   0.9.8 (hardening, ARCHITECTURE.txt section 58): every IASIO / IUnknown method and every callback is an exception
//   barrier (AsioCall, guard.h); host arguments and the call order are checked with asiocheck.h and copied before
//   use (callbacks, channel lists, channel info, input monitor); device-reported formats, periods and buffer sizes
//   are bounded (DevFmtUsable, RefTimeFrames, MultiFifo::init); no wait on the audio thread or on device IO is
//   unbounded (start, run); notification callbacks detach under a lock (no use after free); TraceLogging events
//   on every non-real-time path (trace.h).
//
// Threading model (see ARCHITECTURE.txt section 3; these are invariants)
//  - Host thread (H): every IASIO method, the control panel and the hidden message window (msgWnd). It is the ONLY
//    thread that calls asioMessage(kAsioResetRequest / kAsioOverload / kAsioLatenciesChanged) and the only one that
//    modifies configuration, devices and streams.
//  - Audio thread (A): MMCSS "Pro Audio" at AVRT_PRIORITY_CRITICAL, woken by the WASAPI render event of the primary
//    output. No locks, no allocation, no logging, no asioMessage(). It publishes errors and dropouts as atomics plus
//    PostMessage() to msgWnd, which is processed on H.
//  - Notification threads (N: MMDevice, endpoint volume, process-loopback activation): they only queue events and
//    PostMessage() to msgWnd.
//
// asioMessage marshalling
//   The ASIO specification requires asioMessage() to be called on the host's thread. Requests born on A or N are
//   turned into a pending atomic value (pendingReset / pendingOverload / evQueue) plus a WM_APP message to the
//   message-only window created in init() on H; MsgWndProc runs on H and performs the call. The window procedure
//   wraps the call with AddRef/Release because the host may destroy the driver from inside asioMessage().
#include "driver.h"
#include "registry.h"
#include "sinc.h"
#include "drift.h"
#include "privacy.h"
#include "crash.h"
#include "chnames.h"
#include "strings.h"
#include "guard.h"      // 0.9.8: exception barrier (NoteException, GuardedWndProc)
#include "asiocheck.h"  // 0.9.8: validation of host arguments and call order (pure, tested offline)
#include "parse.h"      // 0.9.8: OutputMap parser (pure, fuzzed offline)
#include "trace.h"      // 0.9.8: TraceLogging events
#include <avrt.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <cmath>
#include <new>        // std::bad_alloc, std::nothrow (0.9.8)
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <tlhelp32.h>
#include <audiopolicy.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// Private messages of the hidden message window (all processed on the host thread)
#define WM_WB_RESET    (WM_APP + 1)     // pendingReset holds a reason -> requestReset()
#define WM_WB_DEVEVENT (WM_APP + 2)     // evQueue has device events -> handleDeviceEvent()
#define WM_WB_OVERLOAD (WM_APP + 3)     // pendingOverload -> asioMessage(kAsioOverload)
#define WM_WB_DELETE   (WM_APP + 4)     // the last Release happened on the audio thread -> teardown() on H
#define WM_WB_LATCHG   (WM_APP + 5)     // 0.7.0: latency offsets changed -> kAsioLatenciesChanged (or reset)
#define WM_WB_MONCREATE  (WM_APP + 6)   // fix 1: create the monitor window on msgThread
#define WM_WB_MONDESTROY (WM_APP + 7)   // fix 1: destroy it on msgThread (sent synchronously)
#define WM_WB_TRAYCREATE  (WM_APP + 8)  // 0.8.0 (G5): create the tray icon on msgThread
#define WM_WB_TRAYDESTROY (WM_APP + 9)  // 0.8.0 (G5): remove it on msgThread (sent synchronously)
#define WB_STATS_TIMER    0x5742        // 0.9.8: WM_TIMER id of the 10 s statistics trace event (msgWnd)

// 0.9.8: the numeric results of asiocheck.h are the ASIOError values of asio.h.
static_assert((long)asiochk::kOk == (long)ASE_OK && (long)asiochk::kNotPresent == (long)ASE_NotPresent &&
              (long)asiochk::kHWMalfunction == (long)ASE_HWMalfunction && (long)asiochk::kInvalidParameter == (long)ASE_InvalidParameter &&
              (long)asiochk::kInvalidMode == (long)ASE_InvalidMode && (long)asiochk::kSPNotAdvancing == (long)ASE_SPNotAdvancing &&
              (long)asiochk::kNoClock == (long)ASE_NoClock && (long)asiochk::kNoMemory == (long)ASE_NoMemory,
              "asiocheck.h error values must match asio.h");
static_assert(asiochk::kMaxCh == kMaxCh, "asiocheck.h channel limit must match driver.h");
static_assert(asiochk::kEngineInfoSelector == WB_FUTURE_ENGINE_INFO, "engine-info selector");
static_assert(cfgp::kRouteSlots == kMaxCh && cfgp::kMaxExtraList == kMaxExtra, "parse.h limits must match driver.h / common.h");

// 0.9.8: exception barrier + trace of one IASIO call. `traceable` is computed by the caller BEFORE the call (the
// call may destroy the driver, e.g. controlPanel / a reset inside asioMessage): false on the audio thread, which
// never traces. `f` is the method body; whatever it throws is contained and reported as ASE_HWMalfunction.
template <class F>
static ASIOError AsioCall(bool traceable, const char* name, uint8_t level, F&& f) noexcept {
    ASIOError r;
    try { r = f(); }
    catch (const std::bad_alloc&) { if (traceable) NoteException(name); r = ASE_NoMemory; }
    catch (...) { if (traceable) NoteException(name); r = ASE_HWMalfunction; }
    if (traceable) WB_TRACE(level, wbtrace::kKwAsio, "AsioCall", wbtrace::Str("Method", name), wbtrace::I32("Result", (int32_t)r));
    return r;
}
const CLSID CLSID_WasapiBridge = {0xa63175bd, 0xb872, 0x4fa5, {0x95, 0x95, 0x46, 0x4f, 0x02, 0x36, 0xe1, 0xd0}};
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID  kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID  kSubtypePcm   = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID  kIID_IAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const PROPERTYKEY kPkeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const PROPERTYKEY kPkeyContainerId  = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

// From audioclientactivationparams.h (not shipped with mingw)
enum WB_AUDIOCLIENT_ACTIVATION_TYPE { WB_ACT_DEFAULT = 0, WB_ACT_PROCESS_LOOPBACK = 1 };
enum WB_PROCESS_LOOPBACK_MODE { WB_LB_INCLUDE_TREE = 0, WB_LB_EXCLUDE_TREE = 1 };
struct WB_AUDIOCLIENT_ACTIVATION_PARAMS {
    WB_AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    struct { DWORD TargetProcessId; WB_PROCESS_LOOPBACK_MODE ProcessLoopbackMode; } ProcessLoopbackParams;
};

std::atomic<long> g_objects{0};

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static const char* ReasonStr(int r) {
    switch (r) { case RR_WASAPI: return "WASAPI error"; case RR_CAPTURE: return "capture invalidated";
                 case RR_DEVICE: return "device change"; case RR_PANEL: return "panel"; default: return "?"; }
}

// ---------------------------------------------------------------- sample formats
// Describes a WAVEFORMATEX(TENSIBLE) as a DevFmt (container size, float/int, valid bits).
static DevFmt ParseFmt(const WAVEFORMATEX* w) {
    DevFmt f;
    f.channels = w->nChannels;
    f.block    = w->nBlockAlign;
    f.bytes    = w->nChannels ? w->nBlockAlign / w->nChannels : 4;
    f.isFloat  = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    f.validBits = f.bytes * 8;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)w;
        f.isFloat = IsEqualGUID(x->SubFormat, kSubtypeFloat);
        if (x->Samples.wValidBitsPerSample) f.validBits = x->Samples.wValidBitsPerSample;
    }
    return f;
}

// Standard speaker mask for a channel count (0 = let Windows decide).
static DWORD DefaultMask(int ch) {
    switch (ch) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default: return 0;
    }
}

// Builds a WAVEFORMATEXTENSIBLE. mask 0xFFFFFFFF = DefaultMask(ch).
static WAVEFORMATEXTENSIBLE MakeFmt(double rate, int ch, int bits, int valid, bool isFloat, DWORD mask = 0xFFFFFFFF) {
    WAVEFORMATEXTENSIBLE x = {};
    x.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels       = (WORD)ch;
    x.Format.nSamplesPerSec  = (DWORD)rate;
    x.Format.wBitsPerSample  = (WORD)bits;
    x.Format.nBlockAlign     = (WORD)(ch * bits / 8);
    x.Format.nAvgBytesPerSec = x.Format.nSamplesPerSec * x.Format.nBlockAlign;
    x.Format.cbSize          = 22;
    x.Samples.wValidBitsPerSample = (WORD)valid;
    x.dwChannelMask = mask == 0xFFFFFFFF ? DefaultMask(ch) : mask;
    x.SubFormat     = isFloat ? kSubtypeFloat : kSubtypePcm;
    return x;
}

// 0.9.8 (review H-list, untrusted device data): a format reported by a device (mix format) is only used when
// writeOut / pullSource can handle it without leaving the frame: 1..1024 channels, a block of exactly
// channels * bytes, integer containers of 2, 3 or 4 bytes, float containers of 4 bytes. Before, a mix format that
// claimed IEEE float with 16-bit samples made PutSample write 4 bytes into a 2-byte slot (past the last frame).
// Formats built by the driver itself (MakeFmt) always pass.
static bool DevFmtUsable(const DevFmt& f) {
    if (f.channels < 1 || f.channels > 1024 || f.bytes < 2 || f.block != f.channels * f.bytes) return false;
    return f.isFloat ? f.bytes == 4 : (f.bytes >= 2 && f.bytes <= 4);
}

// 0.9.8 (review H-list, untrusted device data): device periods and latencies (REFERENCE_TIME, 100 ns) converted to
// frames. A driver reporting a negative, huge or NaN-producing value made the double -> UINT32 conversion undefined
// behavior; now the result is clamped to [0, 2^24] frames (about 6 minutes at 48 kHz, far above any real period).
static UINT32 RefTimeFrames(double rate, double refTime100ns) {
    const double f = rate * refTime100ns / 10000000.0 + 0.5;
    if (!(f > 0)) return 0;                                // NaN and negatives
    return f > 16777216.0 ? 16777216u : (UINT32)f;
}

static void FmtStr(const DevFmt& f, char* buf, size_t n) {
    snprintf(buf, n, "%dch %s%d", f.channels, f.isFloat ? "float" : "int", f.bytes * 8);
}

static inline float Clamp1(float x) { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); }

// Writes one sample in the device format (float, int16, packed int24, int32). Real-time safe.
static inline void PutSample(BYTE* p, const DevFmt& f, float x) {
    if (f.isFloat) { *(float*)p = f.clamp ? Clamp1(x) : x; return; }
    x = Clamp1(x);
    switch (f.bytes) {
    case 2: *(int16_t*)p = (int16_t)lrintf(x * 32767.f); break;
    case 3: { int32_t v = (int32_t)lrintf(x * 8388607.f); p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); } break;
    case 4: *(int32_t*)p = (int32_t)llrint((double)x * 2147483647.0); break;
    default: break;
    }
}

// Quantization with TPDF dither (tri in (-1, 1) LSB of the valid bits). q = 2^(valid-1) - 1,
// v = lrint(x*q + tri) clamped to [-q-1, q], written shifted left by (container - valid) bits: covers 16 bits,
// 20 and 24 bits in packed 24, 20/24 bits in 32. Double arithmetic (in float, tri would be lost near full scale).
static inline void PutSampleDither(BYTE* p, const DevFmt& f, float x, float tri) {
    const int cont = f.bytes * 8;
    const int valid = f.validBits < cont ? f.validBits : cont;
    if (valid < 8 || valid > 24 || (cont != 16 && cont != 24 && cont != 32)) { PutSample(p, f, x); return; }
    x = Clamp1(x);
    const long q = (1L << (valid - 1)) - 1;
    long v = lrint((double)x * (double)q + tri);
    if (v > q) v = q;
    if (v < -q - 1) v = -q - 1;
    const uint32_t u = (uint32_t)v << (cont - valid);
    if (cont == 16) *(int16_t*)p = (int16_t)(uint16_t)u;
    else if (cont == 24) { p[0] = (BYTE)u; p[1] = (BYTE)(u >> 8); p[2] = (BYTE)(u >> 16); }
    else *(int32_t*)p = (int32_t)u;
}

// xorshift32 -> difference of two 16-bit uniforms = triangular density in (-1, 1). No global state.
static inline float TriDither(uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return ((float)(s & 0xFFFFu) - (float)(s >> 16)) * (1.f / 65536.f);
}

// Reads one sample of a captured buffer as float in [-1, 1).
static inline float GetSample(const BYTE* p, const DevFmt& f) {
    if (f.isFloat) return *(const float*)p;
    switch (f.bytes) {
    case 2: return *(const int16_t*)p * (1.f / 32768.f);
    case 3: { int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); return (float)(v * (1.0 / 2147483648.0)); }
    case 4: return (float)(*(const int32_t*)p * (1.0 / 2147483648.0));
    default: return 0.f;
    }
}

// Lock-free atomic maximum (relaxed CAS). Used only by the audio thread; the host drains with exchange(0).
static inline void AtomicMax(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

static inline float PeakAbs(const float* b, long n) {
    float m = 0.f;
    for (long i = 0; i < n; ++i) { float v = fabsf(b[i]); if (v > m) m = v; }
    return m;
}

// 4-point, 3rd-order Hermite (Catmull-Rom) interpolation between x0 and x1 at fraction t.
static inline float Hermite(float xm1, float x0, float x1, float x2, float t) {
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// Resamples an interleaved FIFO (inputs and additional outputs). Hermite reads x[k-1..k+2]; sinc reads
// x[k-kSincHalf+1..k+kSincHalf]. The caller guarantees the bounds (its `need` check) and afterwards consumes
// floor(p) - posBase frames. out(i, channel, value) receives every output sample. No allocation: audio thread.
template <class Out>
static inline void ResampleFifo(MultiFifo& f, int n, double& p, double ratio, bool sinc, const float* tab, long frames, Out out) {
    if (sinc) {
        float coef[kSincTaps];
        const float* row[kSincTaps];
        for (long i = 0; i < frames; ++i) {
            size_t k = (size_t)p;
            SincCoefs(tab, p - (double)k, coef);
            for (int m = 0; m < kSincTaps; ++m) row[m] = f.slot(k - (kSincHalf - 1) + (size_t)m);   // 32 frame pointers, reused for all channels
            for (int c = 0; c < n; ++c) {
                float acc = 0.f;
                for (int m = 0; m < kSincTaps; ++m) acc += coef[m] * row[m][c];
                out(i, c, acc);
            }
            p += ratio;
        }
    } else {
        for (long i = 0; i < frames; ++i) {
            size_t k = (size_t)p; float t = (float)(p - (double)k);
            const float* a = f.slot(k - 1); const float* b = f.slot(k);
            const float* c2 = f.slot(k + 1); const float* d = f.slot(k + 2);
            for (int c = 0; c < n; ++c) out(i, c, Hermite(a[c], b[c], c2[c], d[c], t));
            p += ratio;
        }
    }
}

static inline void ToAsio64(uint64_t v, unsigned long& hi, unsigned long& lo) {
    hi = (unsigned long)(v >> 32); lo = (unsigned long)(v & 0xFFFFFFFFull);
}

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// ---------------------------------------------------------------- device helpers (host thread)
static std::wstring DeviceName(IMMDevice* d) {
    std::wstring name;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v); ps->Release();
    }
    return name;
}

// PKEY_Device_ContainerId: identifies the physical device an endpoint belongs to (same box = same clock, usually).
static GUID DeviceContainer(IMMDevice* d) {
    GUID g = {};
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && v.puuid) g = *v.puuid;
        PropVariantClear(&v); ps->Release();
    }
    return g;
}

static std::wstring DeviceId(IMMDevice* d) {
    std::wstring s; LPWSTR id = nullptr;
    if (SUCCEEDED(d->GetId(&id)) && id) { s = id; CoTaskMemFree(id); }
    return s;
}

// Channels of the device mix format, limited to maxCh.
static int MixChannels(IMMDevice* d, int maxCh) {
    IAudioClient* c = nullptr; int n = 2;
    if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) {
        WAVEFORMATEX* m = nullptr;
        if (SUCCEEDED(c->GetMixFormat(&m)) && m) { n = m->nChannels; CoTaskMemFree(m); }
        c->Release();
    }
    if (n < 1) n = 1;
    return n > maxCh ? maxCh : n;
}

// Finds the first active endpoint whose name contains `want` (case-insensitive); falls back to the eConsole default
// (fallback = true when `want` was given but not found). Logs the list of devices.
// 0.8.0 (G1). PKEY_Device_EnumeratorName of the endpoint: "BTHENUM" (Bluetooth classic: A2DP / hands-free),
// "BTHHFENUM" (hands-free profile) or "BTHLEDEVICE" (Bluetooth LE Audio) identify Bluetooth devices. Host thread.
static const PROPERTYKEY kPkeyEnumeratorName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 24};
static bool IsBluetoothDevice(IMMDevice* d) {
    bool bt = false;
    IPropertyStore* ps = nullptr;
    if (d && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyEnumeratorName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            bt = _wcsnicmp(v.pwszVal, L"BTH", 3) == 0;
        PropVariantClear(&v); ps->Release();
    }
    return bt;
}
// Mix-format details of an endpoint (G4/G6): sample rate and channel mask (0 when not extensible). Host thread.
static void MixInfo(IMMDevice* d, double& rate, uint32_t& mask) {
    rate = 0; mask = 0;
    IAudioClient* c = nullptr;
    if (d && SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) {
        WAVEFORMATEX* m = nullptr;
        if (SUCCEEDED(c->GetMixFormat(&m)) && m) {
            rate = m->nSamplesPerSec;
            if (m->wFormatTag == WAVE_FORMAT_EXTENSIBLE && m->cbSize >= 22) mask = ((WAVEFORMATEXTENSIBLE*)m)->dwChannelMask;
            CoTaskMemFree(m);
        }
        c->Release();
    }
}
// True when two ContainerIds identify the same physical device (unknown / built-in containers never match).
static bool SameContainer(const GUID& a, const GUID& b) {
    static const GUID zero = {}, builtin = {0x00000000, 0x0000, 0x0000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    if (IsEqualGUID(a, zero) || IsEqualGUID(a, builtin)) return false;
    return IsEqualGUID(a, b) != 0;
}

static IMMDevice* FindDevice(IMMDeviceEnumerator* e, EDataFlow flow, const std::wstring& want, bool& fallback) {
    IMMDevice* found = nullptr;
    IMMDeviceCollection* col = nullptr;
    const char* kind = flow == eRender ? "output" : "input";
    fallback = false;
    if (SUCCEEDED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        std::wstring w = Lower(want);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            std::wstring nm = DeviceName(d);
            Log("  %s device [%u]: %s", kind, i, Utf8(nm).c_str());
            if (!found && !w.empty() && Lower(nm).find(w) != std::wstring::npos) { found = d; d = nullptr; }
            SafeRelease(d);
        }
        col->Release();
    }
    if (!want.empty() && !found) { Log("  '%s' (%s) not found: using the default device", Utf8(want).c_str(), kind); fallback = true; }
    if (!found) e->GetDefaultAudioEndpoint(flow, eConsole, &found);
    if (found) Log("  -> selected %s: %s", kind, Utf8(DeviceName(found)).c_str());
    return found;
}

// First active device whose name contains `want` (case-insensitive); no default fallback. Host thread.
// skip: Ids already in use (primary and extras chosen so far); the search continues with the next match.
static IMMDevice* FindDeviceSub(IMMDeviceEnumerator* e, EDataFlow flow, const std::wstring& want, const std::vector<std::wstring>& skip) {
    if (want.empty()) return nullptr;
    IMMDevice* found = nullptr;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        std::wstring w = Lower(want);
        for (UINT i = 0; i < n && !found; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            if (Lower(DeviceName(d)).find(w) != std::wstring::npos) {
                const std::wstring id = DeviceId(d);
                bool used = false;
                for (auto& x : skip) if (x == id) used = true;
                if (!used) { found = d; d = nullptr; }
            }
            SafeRelease(d);
        }
        col->Release();
    }
    return found;
}

// PIDs owning an audio session on any active render endpoint (IAudioSessionManager2). Host thread.
// Used by the panel (application list) and by FindProcessByExe.
void ListAudioSessionPids(IMMDeviceEnumerator* e, std::vector<DWORD>& pids) {
    pids.clear();
    if (!e) return;
    IMMDeviceCollection* col = nullptr;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d))) continue;
        IAudioSessionManager2* sm = nullptr;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&sm))) {
            IAudioSessionEnumerator* se = nullptr;
            if (SUCCEEDED(sm->GetSessionEnumerator(&se))) {
                int cnt = 0; se->GetCount(&cnt);
                for (int k = 0; k < cnt; ++k) {
                    IAudioSessionControl* sc = nullptr; IAudioSessionControl2* sc2 = nullptr;
                    if (FAILED(se->GetSession(k, &sc))) continue;
                    if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2))) {
                        DWORD pid = 0;
                        if (SUCCEEDED(sc2->GetProcessId(&pid)) && pid) {
                            bool dup = false;
                            for (DWORD x : pids) if (x == pid) dup = true;
                            if (!dup) pids.push_back(pid);
                        }
                        sc2->Release();
                    }
                    sc->Release();
                }
                se->Release();
            }
            sm->Release();
        }
        d->Release();
    }
    col->Release();
}

// Chooses the PID for per-application loopback (host thread). One Toolhelp snapshot; the driver's own process (the
// DAW) is ignored. Preference: the single root of the process tree (a match whose parent is not a match); with
// several roots or none, the match that owns an active audio session; otherwise the first root or the first match.
// `why` describes the criterion used (for the log).
static DWORD FindProcessByExe(IMMDeviceEnumerator* e, const std::wstring& exe, const char*& why) {
    why = "not found";
    if (exe.empty()) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    struct M { DWORD pid, parent; };
    std::vector<M> m;
    const DWORD self = GetCurrentProcessId();
    bool selfMatched = false;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe.c_str())) continue;
        if (pe.th32ProcessID == self) { selfMatched = true; continue; }
        m.push_back(M{pe.th32ProcessID, pe.th32ParentProcessID});
    }
    CloseHandle(snap);
    if (m.empty()) { why = selfMatched ? "is the host's own executable (rejected)" : "is not running"; return 0; }
    std::vector<DWORD> roots;
    for (const M& a : m) {
        bool parentMatches = false;
        for (const M& b : m) if (b.pid == a.parent) parentMatches = true;
        if (!parentMatches) roots.push_back(a.pid);
    }
    if (roots.size() == 1) { why = "single root of the process tree"; return roots[0]; }
    std::vector<DWORD> sess;
    ListAudioSessionPids(e, sess);
    for (DWORD r : roots) for (DWORD sp : sess) if (r == sp) { why = "root with an active audio session"; return r; }
    for (const M& a : m) for (DWORD sp : sess) if (a.pid == sp) { why = "process with an active audio session"; return a.pid; }
    if (!roots.empty()) { why = "first root (several, none with an audio session)"; return roots[0]; }
    why = "first match";
    return m[0].pid;
}

// ---------------------------------------------------------------- process loopback activation
// Process loopback needs ActivateAudioInterfaceAsync("VAD\\Process_Loopback"), which completes on an MTA thread.
// It is run on a dedicated thread so the host thread (an STA) never blocks inside COM, and so that a slow
// activation cannot touch freed memory:
//  - ActCtx is shared by the host thread and the activation thread with a reference count of 2; whoever releases
//    last frees it, and neither side writes the other's memory after releasing its reference.
//  - The thread holds its own reference to this DLL and exits with FreeLibraryAndExitThread, so the DLL cannot be
//    unloaded while the thread still runs.
//  - pid/mode are written by H before the thread is created (read-only afterwards).
// Waits: 3 s inside the thread for the activation, 8 s on the host thread for the thread.
struct ActCtx { LONG refs = 2; HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr); IAudioClient* client = nullptr; HRESULT hr = E_FAIL;
                DWORD pid = 0; WB_PROCESS_LOOPBACK_MODE mode = WB_LB_EXCLUDE_TREE; };
static void ActCtxRelease(ActCtx* c) {
    if (InterlockedDecrement(&c->refs) == 0) { SafeRelease(c->client); CloseHandle(c->done); delete c; }
}

// Completion handler (agile: may be invoked on any MTA thread). Counts in g_objects while alive.
class ActHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient* client = nullptr;
    ActHandler() { ++g_objects; }
    virtual ~ActHandler() { SafeRelease(client); CloseHandle(done); --g_objects; }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;                               // 0.9.8
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler)) ||
            IsEqualIID(riid, kIID_IAgileObject)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        IUnknown* u = nullptr; HRESULT hr = E_FAIL;
        if (op && SUCCEEDED(op->GetActivateResult(&hr, &u)) && SUCCEEDED(hr) && u)   // 0.9.8: null op checked
            hr = u->QueryInterface(__uuidof(IAudioClient), (void**)&client);
        SafeRelease(u);
        result = hr; SetEvent(done); return S_OK;
    }
private:
    LONG refs = 1;
};

typedef HRESULT (WINAPI* PFN_ActivateAsync)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                            IActivateAudioInterfaceAsyncOperation**);

// Activation thread (MTA). Mmdevapi.dll is loaded at run time (ActivateAudioInterfaceAsync is not in older SDK libs).
static DWORD WINAPI ActivateProcLoopbackThread(void* p) {
    ActCtx* ctx = (ActCtx*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE m = LoadLibraryW(L"Mmdevapi.dll");
    PFN_ActivateAsync fn = m ? (PFN_ActivateAsync)(void*)GetProcAddress(m, "ActivateAudioInterfaceAsync") : nullptr;
    if (fn) {
        WB_AUDIOCLIENT_ACTIVATION_PARAMS ap = {};
        ap.ActivationType = WB_ACT_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = ctx->pid;
        ap.ProcessLoopbackParams.ProcessLoopbackMode = ctx->mode;
        PROPVARIANT pv; PropVariantInit(&pv);
        pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;
        ActHandler* h = new (std::nothrow) ActHandler();     // 0.9.8: no exception in a thread procedure
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        HRESULT hr = h && h->done ? fn(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, h, &op) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            if (WaitForSingleObject(h->done, 3000) == WAIT_OBJECT_0) { hr = h->result; ctx->client = h->client; h->client = nullptr; }
            else hr = E_ABORT;
        }
        ctx->hr = hr;
        SafeRelease(op);
        if (h) h->Release();
    }
    if (m) FreeLibrary(m);
    CoUninitialize();
    SetEvent(ctx->done);
    ActCtxRelease(ctx);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&ActivateProcLoopbackThread, &self);
    FreeLibraryAndExitThread(self, 0);   // releases the reference taken when the thread was created
    return 0;
}

// Host thread. pid + mode: EXCLUDE_TREE with the own PID (everything but the DAW) or INCLUDE_TREE with the PID of an
// application. Returns an uninitialized IAudioClient or nullptr (hr has the reason).
static IAudioClient* ActivateProcessLoopback(HRESULT& hr, DWORD pid, WB_PROCESS_LOOPBACK_MODE mode) {
    hr = E_FAIL;
    HMODULE self = nullptr;   // reference to this DLL for the thread (released by FreeLibraryAndExitThread)
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void*)&ActivateProcLoopbackThread, &self)) return nullptr;
    ActCtx* c = new (std::nothrow) ActCtx();             // 0.9.8: no exception with the module reference held
    if (!c || !c->done) { FreeLibrary(self); if (c) { if (c->done) CloseHandle(c->done); delete c; } hr = E_OUTOFMEMORY; return nullptr; }
    c->pid = pid; c->mode = mode;
    HANDLE t = CreateThread(nullptr, 0, ActivateProcLoopbackThread, c, 0, nullptr);
    if (!t) { FreeLibrary(self); ActCtxRelease(c); ActCtxRelease(c); return nullptr; }
    CloseHandle(t);
    IAudioClient* client = nullptr;
    if (WaitForSingleObject(c->done, 8000) == WAIT_OBJECT_0) { hr = c->hr; client = c->client; c->client = nullptr; }
    else hr = E_ABORT;
    ActCtxRelease(c);
    return client;
}

// ---------------------------------------------------------------- COM callbacks (notification threads)
STDMETHODIMP VolumeCallback::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;                                   // 0.9.8
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IAudioEndpointVolumeCallback))) {
        *ppv = static_cast<IAudioEndpointVolumeCallback*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
}
// Volume or mute changed (N thread): the driver recomputes its software gain under the shared volume lock.
// 0.9.8: under the callback's own shared lock (see detach in driver.h); nothing here allocates or throws.
STDMETHODIMP VolumeCallback::OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) {
    AcquireSRWLockShared(&lock);
    if (WasapiBridge* o = owner.load()) o->onVolumeNotify(index);
    ReleaseSRWLockShared(&lock);
    return S_OK;
}

STDMETHODIMP DeviceNotifier::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;                                   // 0.9.8
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
        *ppv = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
}
// 0.9.8: every notification goes through forward(): the owner is used under the shared lock (detach() waits for
// it), and the exception barrier turns a failed allocation of the queued event into a dropped event.
void DeviceNotifier::forward(const char* what, LPCWSTR id, EDataFlow flow, bool isDefault) {
    AcquireSRWLockShared(&lock);
    try {
        if (auto o = owner.load()) o->queueDeviceEvent(what, id, flow, isDefault);
    } catch (...) {
        NoteException("a device notification");
    }
    ReleaseSRWLockShared(&lock);
}
// MMDevice notifications (N thread): only queue the event; the decision is taken on the host thread.
STDMETHODIMP DeviceNotifier::OnDeviceStateChanged(LPCWSTR id, DWORD) { forward("state", id, eAll, false); return S_OK; }
// 0.8.0 (G4), N thread: only the device format property is of interest; it is queued like every other event.
STDMETHODIMP DeviceNotifier::OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) {
    static const GUID kFmt = {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}};   // PKEY_AudioEngine_DeviceFormat
    if (id && IsEqualGUID(key.fmtid, kFmt) && key.pid == 0) forward("format", id, eAll, false);
    return S_OK;
}
STDMETHODIMP DeviceNotifier::OnDeviceAdded(LPCWSTR id) { forward("added", id, eAll, false); return S_OK; }
STDMETHODIMP DeviceNotifier::OnDeviceRemoved(LPCWSTR id) { forward("removed", id, eAll, false); return S_OK; }
STDMETHODIMP DeviceNotifier::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) {
    if (role == eConsole) forward("default", id, flow, true);
    return S_OK;
}

// ---------------------------------------------------------------- message window (host thread)
// Message-only window created by init() on the host thread. It turns requests published by the audio and
// notification threads into calls on the host thread (see "asioMessage marshalling" at the top of this file).
// 0.9.8: exception barrier inside (the WM_NCDESTROY bookkeeping must run even if a handler threw) and the 10 s
// statistics timer (WM_TIMER WB_STATS_TIMER, created in init).
static LRESULT MsgWndDispatch(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK MsgWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCDESTROY) --g_objects;   // the window kept the DLL loaded (DllCanUnloadNow)
    try {
        return MsgWndDispatch(h, msg, wp, lp);
    } catch (...) {
        NoteException("the hidden message window");
        return msg == WM_NCDESTROY ? 0 : DefWindowProcW(h, msg, wp, lp);
    }
}
static LRESULT MsgWndDispatch(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == WB_STATS_TIMER) {
        auto* d = (WasapiBridge*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (d && !d->isDying()) d->onStatsTimer();
        return 0;
    }
    if (msg == WM_WB_MONDESTROY) {     // fix 1: may arrive from the destructor (refs == 0): no AddRef here
        if (auto* d = (WasapiBridge*)GetWindowLongPtrW(h, GWLP_USERDATA)) d->destroyMonitorNow();
        return 0;
    }
    if (msg == WM_WB_TRAYDESTROY) {    // 0.8.0 (G5): same rule as WM_WB_MONDESTROY (sent from the destructor too)
        if (auto* d = (WasapiBridge*)GetWindowLongPtrW(h, GWLP_USERDATA)) d->destroyTrayNow();
        return 0;
    }
    if (msg == WM_WB_RESET || msg == WM_WB_DEVEVENT || msg == WM_WB_OVERLOAD || msg == WM_WB_LATCHG || msg == WM_WB_MONCREATE ||
        msg == WM_WB_TRAYCREATE) {
        auto* d = (WasapiBridge*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (d && d->isDying()) return 0;   // fix 7: refs reached 0 and WM_WB_DELETE is pending: never resurrect
        if (d) {
            d->AddRef();               // the host may release the driver inside asioMessage
            d->onHostMessage(msg);
            d->Release();              // if that was the last reference the destructor runs here (legal)
        }
        return 0;
    }
    if (msg == WM_WB_DELETE) {         // the last Release happened on the audio thread: destroy on the host thread
        if (auto* d = (WasapiBridge*)GetWindowLongPtrW(h, GWLP_USERDATA)) d->destroyFromHost();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- instance registry (0.7.1, locked in 0.9.1)
// Process-global list of initialized driver objects, in creation order (src/registry.h). 0.9.1 (fix 3): guarded
// by an SRWLOCK, because a host may create or release driver objects on more than one thread, and the lookup
// takes its reference under that lock (tryAddRef), so the panel never receives an object whose last Release
// already ran. Used by the control panel to rebind to the instance the host created after a reset.
static reg::Registry<WasapiBridge> g_instances;

void RegisterInstance(WasapiBridge* d) { g_instances.add(d); }
void UnregisterInstance(WasapiBridge* d) { g_instances.remove(d); }
// Newest initialized instance other than `old` that is alive and not dying, already AddRef'ed, or nullptr.
WasapiBridge* FindNewerInstance(WasapiBridge* old) { return g_instances.findNewerAddRef(old); }

// fix 3: one more reference only while the count is above zero and the object is not dying (no resurrection).
bool WasapiBridge::tryAddRef() {
    if (dying.load()) return false;
    LONG r = refs;
    while (r > 0) {
        const LONG seen = InterlockedCompareExchange(&refs, r + 1, r);
        if (seen == r) return true;
        r = seen;
    }
    return false;
}

// ---------------------------------------------------------------- COM
// 0.9.8: also registers the TraceLogging provider (once per DLL load; never in DllMain). The constructor itself does
// not throw except through CrashInstall's allocations; the class factory contains that (dll.cpp).
WasapiBridge::WasapiBridge() {
    ++g_objects;
    InitializeCriticalSection(&evLock);
    for (int i = 0; i < kMaxCh; ++i) { monOut[i] = -1; monGainL[i] = 0.f; monGainR[i] = 0.f; peakIn[i] = 0.f; peakOut[i] = 0.f; }
    wbtrace::Register();
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwLifecycle, "DriverCreated", wbtrace::U64("Instance", (uint64_t)(uintptr_t)this),
             wbtrace::I32("LiveObjects", (int32_t)g_objects.load()));
    CrashInstall();                      // F8: one reference per driver object (released in the destructor)
}

// Host thread (or deferred to it through WM_WB_DELETE). Stops the audio thread, releases streams, unregisters the
// notification client and destroys the message window (or asks its thread to close it).
// 0.9.8: a destructor must never throw (it is implicitly noexcept: an exception would call std::terminate inside
// the host), so the teardown work runs inside a try block; the bookkeeping that keeps the process consistent
// (critical section, crash protection reference, object count) runs after it in every case.
WasapiBridge::~WasapiBridge() {
  try {
    UnregisterInstance(this);            // fix 3: first, so no lookup can reach an object being destroyed
    stop();
    disposeImpl();
    // F2 / fix 1: destroyed on msgThread, synchronously (normally already gone)
    if (monWnd) { if (msgWnd && GetCurrentThreadId() != msgThread) SendMessageW(msgWnd, WM_WB_MONDESTROY, 0, 0); else destroyMonitorNow(); }
    // 0.8.0 (G5): the tray icon is removed by the thread that added it (Shell_NotifyIcon NIM_DELETE on msgThread)
    if (trayWnd) { if (msgWnd && GetCurrentThreadId() != msgThread) SendMessageW(msgWnd, WM_WB_TRAYDESTROY, 0, 0); else destroyTrayNow(); }
    if (notifier) {
        notifier->detach();              // 0.9.8: waits for a notification in flight (see driver.h)
        if (enumr) enumr->UnregisterEndpointNotificationCallback(notifier);   // never from the notification thread
        notifier->Release(); notifier = nullptr;
    }
    if (msgWnd) {
        SetWindowLongPtrW(msgWnd, GWLP_USERDATA, 0);
        if (GetCurrentThreadId() == msgThread) DestroyWindow(msgWnd);
        else PostMessageW(msgWnd, WM_CLOSE, 0, 0);     // DestroyWindow only works on the thread that created it
        msgWnd = nullptr;
    }
    if (timerRaised) { timeEndPeriod(1); timerRaised = false; }
    SafeRelease(inDev);
    SafeRelease(outDev);
    for (int k = 0; k < kMaxExtra; ++k) { SafeRelease(outEx[k].dev); SafeRelease(inEx[k].dev); }
    SafeRelease(enumr);
    if (outEvent)  CloseHandle(outEvent);
    if (stopEvent) CloseHandle(stopEvent);
    Log("Driver released");
  } catch (...) {
    NoteException("the driver destructor");
    // what must not outlive the object even after a failure: callbacks into it and the window's pointer to it
    if (notifier) notifier->detach();
    if (volCb) volCb->detach();
    for (OutputSink& o : sinks) if (o.volCb) o.volCb->detach();
    if (msgWnd) SetWindowLongPtrW(msgWnd, GWLP_USERDATA, 0);
  }
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwLifecycle, "DriverDestroyed", wbtrace::U64("Instance", (uint64_t)(uintptr_t)this));
    DeleteCriticalSection(&evLock);
    CrashUninstall();                    // noexcept (0.9.8)
    --g_objects;
}

STDMETHODIMP WasapiBridge::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;          // 0.9.8: a host passing a null out pointer gets an error, not a crash
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_WasapiBridge)) {   // ASIO convention: riid = CLSID
        *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) WasapiBridge::AddRef() { return InterlockedIncrement(&refs); }
// Last reference on the audio thread (host released the driver inside a callback): the object cannot be destroyed
// under its own thread, so the thread is told to stop and destruction is deferred to the host thread.
STDMETHODIMP_(ULONG) WasapiBridge::Release() {
    LONG r = InterlockedDecrement(&refs);
    if (r) return r;
    if (onAudioThread()) {             // do not destroy the object under its own audio thread
        dying = true;                  // fix 7: MsgWndProc ignores every other message for this object now
        SetEvent(stopEvent);           // stop calling the host, which already released the driver
        if (msgWnd) PostMessageW(msgWnd, WM_WB_DELETE, 0, 0);
        return 0;                      // without a window: leaked on purpose
    }
    // 0.9.8: `dying` is set on this path too, so the hidden window (statistics timer, posted WM_WB_* messages) and
    // the instance registry ignore the object from here on, also when the host releases it on another thread.
    dying = true;
    try { teardown(); } catch (...) { NoteException("Release"); }   // 0.9.8: exception barrier
    return 0;
}

// stop() + delete, unless the audio thread is stuck inside a host callback: then the object is leaked on purpose
// (the thread still references it; freeing it would crash the host later).
void WasapiBridge::teardown() {
    stop();                            // may detect a stuck thread only now
    if (stuckAlive()) { Log("Driver leaked on purpose: audio thread stuck inside the host"); return; }
    delete this;
}

// True while a thread that timed out in stop() is still alive; closes it once it has finished.
bool WasapiBridge::stuckAlive() {
    if (!stuckThread) return false;
    if (WaitForSingleObject(stuckThread, 0) == WAIT_OBJECT_0) {   // the stuck thread has finished
        CloseHandle(stuckThread); stuckThread = nullptr; threadStuck = false;
        if (timerRaised) { timeEndPeriod(1); timerRaised = false; }
        return false;
    }
    return true;
}

void WasapiBridge::setError(const char* msg) {
    strncpy(errMsg, msg, sizeof(errMsg) - 1); errMsg[sizeof(errMsg) - 1] = 0;
    Log("ERROR: %s", msg);
    WB_TRACE(wbtrace::kLvlError, wbtrace::kKwError, "DriverError", wbtrace::Str("Message", errMsg), wbtrace::HR("HResult", (long)lastHr.load()));
}

// ---------------------------------------------------------------- devices (host thread)
// Resolves primary output/input and the additional devices and computes the ASIO channel layout:
//   outputs: [0, nOutPrim) primary, then each additional output in order;
//   inputs:  [0, nInDev) primary, then each additional input, then Loopback L/R at lbFirst.
bool WasapiBridge::resolveDevices() {
    SafeRelease(outDev); SafeRelease(inDev);
    Log("Devices:");
    outDev = FindDevice(enumr, eRender, cfg.outputDevice, outFallback);
    if (!outDev) { setError("No active output device"); return false; }
    outId = DeviceId(outDev); outName = DeviceName(outDev); outContainer = DeviceContainer(outDev);
    outDevCh = MixChannels(outDev, cfg.maxChannels);
    inId.clear(); inName.clear(); inFallback = false; inContainer = GUID{};
    if (_wcsicmp(cfg.inputDevice.c_str(), L"none") != 0) inDev = FindDevice(enumr, eCapture, cfg.inputDevice, inFallback);
    if (inDev) { inId = DeviceId(inDev); inName = DeviceName(inDev); inContainer = DeviceContainer(inDev); inDevCh = MixChannels(inDev, cfg.maxChannels); }
    // 0.8.0 (G1): Bluetooth output. Opening the microphone of the same Bluetooth headset switches it from A2DP
    // (stereo, good quality) to the hands-free profile (mono, telephone quality) for the whole system, so with
    // ProtectA2dp that input is not opened (same ContainerId = same physical device).
    outBt = IsBluetoothDevice(outDev);
    inBt = inDev && IsBluetoothDevice(inDev);
    a2dpBlocked = false;
    if (inDev && outBt && cfg.protectA2dp && SameContainer(inContainer, outContainer)) {
        Log("Input '%s' not opened: it is the microphone of the Bluetooth output (ProtectA2dp keeps the headset in A2DP)", Utf8(inName).c_str());
        SafeRelease(inDev); inId.clear(); inBt = false; a2dpBlocked = true;
    }
    {
        double mr = 0; uint32_t mk = 0; MixInfo(outDev, mr, mk);
        UINT32 per = 0;
        IAudioClient* c = nullptr; REFERENCE_TIME defP = 0, minP = 0;
        if (outBt && SUCCEEDED(outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) {
            if (SUCCEEDED(c->GetDevicePeriod(&defP, &minP)) && mr > 0) per = RefTimeFrames(mr, (double)defP);
            c->Release();
        }
        // Recommended minimum buffer for Bluetooth: 512 frames, and at least two device periods.
        recBuf = outBt ? (long)(per * 2 > 512 ? per * 2 : 512) : 0;
        if (recBuf > 4096) recBuf = 4096;
        if (outBt) Log("Output is Bluetooth: recommended buffer >= %ld; Windows does not report the real Bluetooth latency", recBuf);
    }
    nInDev = inDev ? inDevCh : 0;
    int lb = cfg.loopback != LoopbackMode::Off ? 2 : 0;
    if (nInDev + lb > kMaxCh) nInDev = kMaxCh - lb;
    nOutPrim = outDevCh;
    // 0.5.0: additional devices. Channels: primary first, then each additional device in order; inputs: Loopback last.
    // A missing additional device keeps 2 (silent) channels so the channel count does not depend on its presence.
    long usedOut = nOutPrim, usedIn = nInDev;
    resolveExtras(eRender, cfg.extraOutputs, outEx, nOutEx, outId, usedOut, kMaxCh);
    resolveExtras(eCapture, cfg.extraInputs, inEx, nInEx, inId, usedIn, kMaxCh - lb);
    // G1: the same rule for additional inputs (their channels stay present and silent, like a missing device)
    for (int k = 0; k < nInEx; ++k)
        if (inEx[k].dev && outBt && cfg.protectA2dp && SameContainer(inEx[k].container, outContainer)) {
            Log("Additional input %d not opened: it is the microphone of the Bluetooth output (ProtectA2dp)", k + 2);
            SafeRelease(inEx[k].dev); inEx[k].id.clear(); a2dpBlocked = true;
        }
    lbFirst = usedIn;
    nIn  = usedIn + lb;
    nOut = usedOut;
    Log("ASIO channels: %ld inputs (%ld device + %ld additional + %d loopback), %ld outputs (%ld primary + %ld additional)",
        nIn, nInDev, usedIn - nInDev, lb, nOut, nOutPrim, nOut - nOutPrim);
    // 0.9.8: device names are sanitized (user / computer names masked); Ids only as a short hash
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwDevice, "DevicesResolved",
             wbtrace::Str("Output", wbtrace::SafeText(outName).c_str()), wbtrace::Str("OutputIdHash", Utf8(ShortHash(outId)).c_str()),
             wbtrace::Bool("OutputFallback", outFallback), wbtrace::Bool("OutputBluetooth", outBt),
             wbtrace::Str("Input", wbtrace::SafeText(inName).c_str()), wbtrace::Bool("InputFallback", inFallback),
             wbtrace::Bool("A2dpProtected", a2dpBlocked), wbtrace::I32("AsioInputs", (int32_t)nIn), wbtrace::I32("AsioOutputs", (int32_t)nOut),
             wbtrace::I32("ExtraOutputs", nOutEx), wbtrace::I32("ExtraInputs", nInEx), wbtrace::I32("Loopback", (int32_t)cfg.loopback));
    buildChannelNames();
    devicesDirty = false;
    return true;
}

// 0.8.0 (G6), host thread: wide device name -> ANSI (CP_ACP, no best fit, '?' for unmappable characters),
// shortened on whole characters until it fits `room` bytes (never splits a double-byte character or a UTF-16
// surrogate pair). 0.9.1 (fix 9): WideCharToMultiByte returns 0 when the text does not fit the buffer; for a
// non-empty text that means "too long" (shorten again), not "fits as an empty string".
static std::string AnsiFit(const std::wstring& w, size_t room) {
    return chn::FitConverted(w, room, [](const wchar_t* t, int len, char* out, int cap) {
        return WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, t, len, out, cap, "?", nullptr);
    });
}
static bool AnsiLead(unsigned char c) { return IsDBCSLeadByteEx(CP_ACP, c) != 0; }   // fix 9 (chn::CutBytes)

// 0.8.0 (G6), host thread: builds chNameIn / chNameOut from the device names (rules in chnames.h).
void WasapiBridge::buildChannelNames() {
    std::vector<std::string> outs, ins;
    auto shortW = [](const std::wstring& n) {        // short form decided on the wide string (ASCII rules)
        std::string a(n.begin(), n.end());           // only for the generic-word test; real text converted below
        const std::string sh = chn::ShortName(a);
        const size_t pos = a.find(sh);
        return pos == std::string::npos ? n : n.substr(pos, sh.size());
    };
    auto addDev = [&](std::vector<std::string>& v, const std::wstring& name, int n, uint32_t mask, bool input, size_t first) {
        for (int i = 0; i < n; ++i) {
            std::string pos = input ? "" : chn::PositionFromMask(mask, n, i);
            if (pos.empty()) pos = std::string(input ? "In " : "Out ") + std::to_string(i + 1);
            const std::string dev = AnsiFit(shortW(name), 31 - pos.size() - 1);
            if (v.size() < first + (size_t)i + 1) v.resize(first + (size_t)i + 1);
            v[first + (size_t)i] = chn::Fit(dev, pos, 31, AnsiLead);
        }
    };
    double r = 0; uint32_t mk = 0;
    MixInfo(outDev, r, mk);
    addDev(outs, outName, (int)nOutPrim, mk, false, 0);
    for (int k = 0; k < nOutEx; ++k) {
        uint32_t m2 = 0; MixInfo(outEx[k].dev, r, m2);
        addDev(outs, outEx[k].dev ? outEx[k].name : outEx[k].want, outEx[k].ch, outEx[k].ch == outEx[k].mixCh ? m2 : 0, false, (size_t)outEx[k].asioFirst);
    }
    addDev(ins, inName, (int)nInDev, 0, true, 0);
    for (int k = 0; k < nInEx; ++k) addDev(ins, inEx[k].dev ? inEx[k].name : inEx[k].want, inEx[k].ch, 0, true, (size_t)inEx[k].asioFirst);
    if (lbFirst < nIn) { if (ins.size() < (size_t)nIn) ins.resize((size_t)nIn); ins[(size_t)lbFirst] = "Loopback L"; ins[(size_t)lbFirst + 1] = "Loopback R"; }
    if (outs.size() < (size_t)nOut) outs.resize((size_t)nOut);
    if (ins.size() < (size_t)nIn) ins.resize((size_t)nIn);
    for (long c = 0; c < nOut; ++c) if (outs[(size_t)c].empty()) outs[(size_t)c] = "Out " + std::to_string(c + 1);
    for (long c = 0; c < nIn; ++c) if (ins[(size_t)c].empty()) ins[(size_t)c] = "In " + std::to_string(c + 1);
    chn::MakeUnique(outs, 31, AnsiLead); chn::MakeUnique(ins, 31, AnsiLead);
    memset(chNameIn, 0, sizeof chNameIn); memset(chNameOut, 0, sizeof chNameOut);
    for (long c = 0; c < nOut && c < kMaxCh; ++c) snprintf(chNameOut[c], 32, "%s", outs[(size_t)c].c_str());
    for (long c = 0; c < nIn && c < kMaxCh; ++c) snprintf(chNameIn[c], 32, "%s", ins[(size_t)c].c_str());
}

// 0.9.5: the host rate of this process (ratepolicy.h), shared by every instance. Kept in a small named memory
// block per process ("Local\\...HostRate.<pid>", zero-filled when created) whose handle and view are never
// closed, so it also survives a DLL unload between a release and a re-creation of the driver; it disappears with
// the process. Host threads only. A value other than a standard rate is ignored (ratepolicy.h Valid).
static volatile double* ProcessHostRateSlot() {
    static volatile double* p = nullptr;
    if (p) return p;
    wchar_t name[96];
    swprintf(name, 96, L"Local\\WasapiDriverForAsio.HostRate.%lu", (unsigned long)GetCurrentProcessId());
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(double), name);
    if (!h) return nullptr;
    void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(double));
    if (!v) { CloseHandle(h); return nullptr; }
    p = (volatile double*)v;                 // the handle is kept open on purpose (process lifetime)
    return p;
}
static double ProcessHostRate() { volatile double* p = ProcessHostRateSlot(); return p ? *p : 0.0; }
static void SetProcessHostRate(double r) { if (volatile double* p = ProcessHostRateSlot()) *p = r; }

// 0.9.5, host thread: the ASIO rate at init and at every reload of the settings (createBuffers after a device
// change or a panel reset, leaving safe mode): host rate of this process > SampleRate=Auto (Windows rate) >
// LastHostRate (not in safe mode) > the panel's SampleRate (ratepolicy.h, tested in tests/ratepolicy_test.cpp).
void WasapiBridge::chooseRate(const char* when) {
    const rp::Decision d = rp::Choose(cfg.sampleRate, cfg.sampleRateAuto, ProcessHostRate(), ReadLastHostRate(), safe);
    rate = d.rate; rateSrc = d.src;
    hostSetRate = d.src == rp::Src::Host;    // Auto does not move a rate the host chose
    applyAutoRate();                         // Src::Auto: the Windows mix rate of the output
    Log("Sample rate %.0f Hz (source: %s; %s)", rate, rp::SrcName(rateSrc), when);
}

// 0.8.0 (G4), host thread: SampleRate=Auto in Shared mode -> the ASIO rate is the Windows mix rate of the output.
// Not applied once the host has set a rate itself (setSampleRate): the host's choice wins; only a real change of
// the Windows rate (onFormatChanged) moves it again.
void WasapiBridge::applyAutoRate() {
    if (!cfg.sampleRateAuto || cfg.exclusive || !outDev || hostSetRate) return;
    double mr = 0; uint32_t mk = 0; MixInfo(outDev, mr, mk);
    if (mr < 8000 || mr > 384000) return;
    autoRateSeen = mr;
    if (fabs(mr - rate) >= 0.5) { Log("SampleRate=Auto: following Windows, %.0f Hz", mr); rate = mr; }
}

// 0.8.0 (G4), host thread (device event "format"). The Windows format of the output changed. With
// SampleRate=Auto the host is only asked to reset (0.9.1, fix 6: no synchronous sampleRateDidChange + reset
// pair, which some hosts handle as two reloads); the new rate is kept in pendingAutoRate and becomes the ASIO rate
// in disposeBuffers, once the running stream is closed, so getSampleRate / getLatencies / ASIOTime stay consistent
// with the stream that still runs until the host performs the reset. Without the Auto setting the ASIO rate stays
// and the stream is converted by Windows (the invalidated stream causes the usual single reset).
// autoRateSeen is the last Windows rate followed, so the reaction happens once per real change even when the
// stream invalidation reset already reopened the streams at the new Windows rate. A host that then sets its own
// project rate (setSampleRate) wins: Windows converts, and no further reset is requested until Windows changes.
// Every PKEY_AudioEngine_DeviceFormat notification of other endpoints is ignored.
void WasapiBridge::onFormatChanged(const std::wstring& id) {
    if (id != outId || !outDev || cfg.exclusive) return;
    double mr = 0; uint32_t mk = 0; MixInfo(outDev, mr, mk);
    if (mr < 8000 || mr > 384000) return;
    if (fabs(mr - mixRateOpen) >= 0.5)
        Log("Windows output format changed: %.0f -> %.0f Hz (%s)", mixRateOpen, mr, cfg.sampleRateAuto ? "SampleRate=Auto" : "ASIO rate kept, Windows converts");
    if (!cfg.sampleRateAuto || fabs(mr - autoRateSeen) < 0.5) return;
    autoRateSeen = mr;
    if (fabs(mr - rate) < 0.5) return;                    // already running at the new rate
    Log("SampleRate=Auto: following Windows, %.0f -> %.0f Hz (applied at the reset)", rate, mr);
    SetProcessHostRate(0); hostSetRate = false;           // 0.9.5: this reset follows Windows (Auto's own behaviour)
    if (buffersCreated) pendingAutoRate = mr;              // the stream still runs at `rate`
    else rate = mr;                                       // nothing open: the host reads it after the reset
    devicesDirty = true;
    requestReset("Windows sample rate changed");          // may destroy the driver: last statement
}

// Host thread. Resolves the list of additional devices of one direction; `used` = ASIO channels already taken
// (updated). Devices already used (primary, earlier extras) are skipped; the 32-channel limit truncates or omits.
void WasapiBridge::resolveExtras(EDataFlow flow, const std::vector<std::wstring>& want, ExtraDev* ex, int& n,
                                 const std::wstring& primaryId, long& used, long limit) {
    const char* kind = flow == eRender ? "output" : "input";
    for (int k = 0; k < kMaxExtra; ++k) { SafeRelease(ex[k].dev); ex[k] = ExtraDev{}; }
    n = 0;
    for (size_t i = 0; i < want.size() && n < kMaxExtra; ++i) {
        ExtraDev x; x.want = want[i];
        std::vector<std::wstring> skip;            // primary and the extras chosen so far
        if (!primaryId.empty()) skip.push_back(primaryId);
        for (int k = 0; k < n; ++k) if (!ex[k].id.empty()) skip.push_back(ex[k].id);
        IMMDevice* d = FindDeviceSub(enumr, flow, want[i], skip);
        if (d) { x.id = DeviceId(d); x.name = DeviceName(d); x.container = DeviceContainer(d); x.mixCh = MixChannels(d, cfg.maxChannels); }
        else Log("  Additional %s '%s' not found: its channels stay present and silent", kind, Utf8(want[i]).c_str());
        x.ch = x.mixCh;
        if (used + x.ch > limit) {
            x.ch = (int)(limit - used);
            if (x.ch <= 0) { Log("  Additional %s '%s' omitted: the maximum of %ld channels would be exceeded", kind, Utf8(want[i]).c_str(), limit); SafeRelease(d); continue; }
            Log("  Additional %s '%s' truncated to %d channels (maximum %ld)", kind, Utf8(want[i]).c_str(), x.ch, limit);
        }
        x.dev = d; x.asioFirst = (int)used; used += x.ch;
        if (d) Log("  Additional %s %d: %s, %d ASIO channels from %d", kind, n + 2, Utf8(x.name).c_str(), x.ch, x.asioFirst + 1);
        ex[n++] = x;
    }
}

// N thread: bounded queue (64 events) + wake-up of the host thread.
void WasapiBridge::queueDeviceEvent(const char* what, LPCWSTR id, EDataFlow flow, bool isDefault) {
    if (!id) return;
    EnterCriticalSection(&evLock);
    if (evQueue.size() < 64) evQueue.push_back(DevEvent{std::wstring(id), flow, isDefault, what});
    LeaveCriticalSection(&evLock);
    if (msgWnd) PostMessageW(msgWnd, WM_WB_DEVEVENT, 0, 0);
}

// A or N thread: the first reason wins (CAS from RR_NONE); a single message is posted until H consumes it.
void WasapiBridge::postReset(ResetReason r) {
    int expected = RR_NONE;
    if (pendingReset.compare_exchange_strong(expected, (int)r) && msgWnd) PostMessageW(msgWnd, WM_WB_RESET, 0, 0);
}

// Host thread (MsgWndProc). Anything that may destroy the driver is the last thing done.
void WasapiBridge::onHostMessage(UINT msg) {
    if (msg == WM_WB_RESET) {
        int r = pendingReset.exchange(RR_NONE);
        if (r == RR_WASAPI) {
        Log("Audio thread ended with hr=0x%08lX", (unsigned long)lastHr.load());
        WB_TRACE(wbtrace::kLvlError, wbtrace::kKwError | wbtrace::kKwStream, "AudioThreadEnded", wbtrace::HR("HResult", (long)lastHr.load()));
    }
        const bool mon = monResetPending.exchange(false);
        if (r != RR_NONE) requestReset(ReasonStr(r));          // may destroy the driver: touch nothing afterwards
        else if (mon) requestReset("direct monitoring availability changed");   // 0.9.4 (kAsioCanInputMonitor)
    } else if (msg == WM_WB_OVERLOAD) {
        pendingOverload = false;
        ASIOCallbacks* c = cb;
        if (c && overloadSupported) c->asioMessage(kAsioOverload, 0, nullptr, nullptr);
    } else if (msg == WM_WB_MONCREATE) {
        createMonitorNow();
    } else if (msg == WM_WB_TRAYCREATE) {
        createTrayNow();
    } else if (msg == WM_WB_LATCHG) {
        // F4: offsets changed live. kAsioLatenciesChanged lets the host re-read getLatencies without a reset;
        // hosts that do not support it get a reset request instead (may destroy the driver: last statement).
        latChangePending = false;
        ASIOCallbacks* c = cb;
        if (c && latChangedSupported && c->asioMessage) { Log("kAsioLatenciesChanged (latency offsets)"); c->asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr); }
        else if (c) requestReset("latency offsets");
    } else if (msg == WM_WB_DEVEVENT) {
        std::vector<DevEvent> q;
        EnterCriticalSection(&evLock); q.swap(evQueue); LeaveCriticalSection(&evLock);
        for (auto& e : q) {
            if (resetRequested) break;
            handleDeviceEvent(e);
            if (resetRequested) return;                        // requestReset may have destroyed the driver
        }
    }
}

// Host thread. Device change policy (FollowDeviceChanges):
//  - the eConsole default changed and that side follows the default -> reset;
//  - a device in use (primary or additional) is no longer ACTIVE -> reset;
//  - the configured device (or a missing additional device) shows up while a fallback is in use -> reset.
void WasapiBridge::handleDeviceEvent(const DevEvent& e) {
    if (!strcmp(e.what, "format")) { onFormatChanged(e.id); return; }   // 0.8.0 (G4), independent of FollowDeviceChanges
    if (!cfg.followDevices) return;
    bool reset = false;
    if (e.isDefault) {
        if (e.flow == eRender && cfg.outputDevice.empty() && e.id != outId) reset = true;
        if (e.flow == eCapture && cfg.inputDevice.empty() && inDev && e.id != inId) reset = true;
    } else if (e.id == outId || (!inId.empty() && e.id == inId) || isExtraId(e.id)) {
        IMMDevice* d = nullptr; DWORD st = 0;
        if (SUCCEEDED(enumr->GetDevice(e.id.c_str(), &d))) { d->GetState(&st); d->Release(); }
        if (st != DEVICE_STATE_ACTIVE) reset = true;
    } else {
        // The configured device appears while the default is in use, or an additional device that was missing
        bool missingExtra = false;
        for (int k = 0; k < nOutEx; ++k) if (!outEx[k].dev) missingExtra = true;
        for (int k = 0; k < nInEx; ++k) if (!inEx[k].dev) missingExtra = true;
        IMMDevice* d = nullptr;
        if ((outFallback || inFallback || missingExtra) && SUCCEEDED(enumr->GetDevice(e.id.c_str(), &d))) {
            DWORD st = 0; d->GetState(&st);
            std::wstring nm = Lower(DeviceName(d));
            if (st == DEVICE_STATE_ACTIVE) {
                if (outFallback && nm.find(Lower(cfg.outputDevice)) != std::wstring::npos) reset = true;
                if (inFallback  && nm.find(Lower(cfg.inputDevice))  != std::wstring::npos) reset = true;
                for (int k = 0; k < nOutEx; ++k) if (!outEx[k].dev && nm.find(Lower(outEx[k].want)) != std::wstring::npos) reset = true;
                for (int k = 0; k < nInEx; ++k) if (!inEx[k].dev && nm.find(Lower(inEx[k].want)) != std::wstring::npos) reset = true;
            }
            d->Release();
        }
    }
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwDevice, "DeviceNotification", wbtrace::Str("What", e.what),
             wbtrace::Str("IdHash", Utf8(ShortHash(e.id)).c_str()), wbtrace::I32("Flow", (int32_t)e.flow),
             wbtrace::Bool("IsDefault", e.isDefault), wbtrace::Bool("Reset", reset));
    if (reset) {
        Log("Device change (%s): %s", e.what, Utf8(ShortHash(e.id)).c_str());
        requestReset(ReasonStr(RR_DEVICE));
    }
}

// Host thread only. May make the host destroy the driver synchronously. Sent once per start cycle; without host
// support (or without buffers) the change is applied at the next createBuffers through devicesDirty.
void WasapiBridge::requestReset(const char* why) {
    ASIOCallbacks* c = cb;
    if (!(c && resetSupported && c->asioMessage)) {
        devicesDirty = true;     // re-resolved at the next createBuffers
        Log("Reset pending (%s): the host has no buffers or does not support kAsioResetRequest", why);
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwReset, "ResetPending", wbtrace::Str("Reason", why));
        return;
    }
    if (resetRequested.exchange(true)) return;
    Log("kAsioResetRequest (%s)", why);
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwReset, "ResetRequest", wbtrace::Str("Reason", why));
    c->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);   // may destroy the driver: last statement
}

// Host thread, after the panel saved config.ini ("Apply"). 0.7.0 (F1):
//   1. LIVE settings are copied into cfg and published to the audio thread (publishLive): they take effect at
//      the next buffer without interrupting the audio. Latency offset changes also post WM_WB_LATCHG so that
//      kAsioLatenciesChanged is sent from the message window, outside the panel's handler.
//   2. If a RESET setting differs (NeedsReset), the new configuration is loaded on the next createBuffers
//      (devicesDirty) and a reset is requested through postReset(RR_PANEL): the request is delivered by the hidden
//      window, i.e. never from inside the panel's WM_COMMAND, because the host may stop, dispose or even destroy
//      the driver synchronously inside kAsioResetRequest.
//   In safe mode (F8), Apply means the user chose new settings: the crash marker is cleared and the saved
//   settings are used from the next reset on.
// Returns what happened, for the panel's status line.
ApplyResult WasapiBridge::applyPanelChanges(const Config& saved) {
    Log("Panel: configuration saved");
    // 0.9.5: the user changed Sample rate (value or Auto) in the panel: it replaces the remembered host rate
    if (saved.sampleRate != uiCfg.sampleRate || saved.sampleRateAuto != uiCfg.sampleRateAuto) {
        SetProcessHostRate(0); WriteLastHostRate(0); hostSetRate = false;
        Log("Panel: Sample rate changed by the user; the remembered host rate is cleared");
    }
    uiCfg = saved;                       // fix 2: the panel's base is the saved configuration, immediately
    // 0.8.0 (G3): the panel showed (and has now saved) the settings with the host profile applied, so config.ini
    // holds them from now on; later reloads of this session must not apply the profile on top again.
    if (!hostProfile.empty()) { Log("Host profile: settings saved from the panel, profile no longer re-applied this session"); hostProfile.clear(); }
    const bool wasSafe = safe;
    if (safe) { safe = false; CrashClearMarker(); Log("Safe mode left: the user's settings are used from now on"); }
    const bool reset = wasSafe || NeedsReset(cfg, saved);
    const bool offsetsChanged = saved.outLatOffsetMs != cfg.outLatOffsetMs || saved.inLatOffsetMs != cfg.inLatOffsetMs;
    const bool logChanged = saved.log != cfg.log;
    // LIVE part (always applied now; RESET keys are copied too but only take effect when the streams reopen).
    if (reset) {
        Config keep = cfg;               // the RESET keys stay as opened until the host reopens the driver
        cfg = saved;
        cfg.exclusive = keep.exclusive; cfg.lowLatencyShared = keep.lowLatencyShared; cfg.outputDevice = keep.outputDevice;
        cfg.inputDevice = keep.inputDevice; cfg.loopback = keep.loopback; cfg.loopbackApp = keep.loopbackApp;
        cfg.bufferSize = keep.bufferSize; cfg.sampleRate = keep.sampleRate; cfg.drift = keep.drift;
        cfg.maxChannels = keep.maxChannels; cfg.resampler = keep.resampler; cfg.extraOutputs = keep.extraOutputs;
        cfg.extraInputs = keep.extraInputs; cfg.alignDevices = keep.alignDevices;
        cfg.protectA2dp = keep.protectA2dp; cfg.sampleRateAuto = keep.sampleRateAuto;          // 0.8.0 RESET keys
        cfg.deviceChannelNames = keep.deviceChannelNames;
    } else {
        cfg = saved;
    }
    if (logChanged) LogInit(cfg.log);
    if (streamsOpen) buildRoute(); else publishLive();   // buildRoute publishes the live parameters
    updateMonitorWindow();
    updateTray();                        // 0.8.0 (G5): TrayIcon is LIVE
    if (offsetsChanged && cb && !reset && !latChangePending.exchange(true) && msgWnd) PostMessageW(msgWnd, WM_WB_LATCHG, 0, 0);
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwPanel, "PanelApply", wbtrace::Bool("Reset", reset), wbtrace::Bool("LeftSafeMode", wasSafe));
    if (!reset) { Log("Panel: applied live"); return ApplyResult::Live; }
    resetRequested = false;
    devicesDirty = true;                 // the next createBuffers reloads config.ini and re-resolves the devices
    if (cb && resetSupported) { postReset(RR_PANEL); return ApplyResult::ResetRequested; }
    if (!buffersCreated) {
        loadSaved(uiCfg);
        cfg = uiCfg;
        LogInit(cfg.log);
        bufSize = cfg.bufferSize;
        resolveDevices();
        chooseRate("panel changes");
        devicesDirty = false;
        return ApplyResult::Live;        // nothing was open: the new settings are already in use
    }
    Log("Panel: the host does not support kAsioResetRequest; the changes apply when the driver is reloaded");
    return ApplyResult::ResetOnReload;
}

// Host thread (0.8.0, G3): the saved configuration as this session uses it: config.ini, plus the profile mapped to
// this program while it is still in effect (see applyPanelChanges).
void WasapiBridge::loadSaved(Config& c) {
    LoadConfig(c);
    if (!hostProfile.empty() && !LoadProfile(hostProfile, c)) hostProfile.clear();   // deleted meanwhile
}

// Host thread (F8): the safe defaults used for one session after a crash. The user's settings stay in uiCfg
// and in config.ini, untouched.
void WasapiBridge::applySafeSettings() {
    cfg.exclusive = false; cfg.lowLatencyShared = false;
    cfg.outputDevice.clear(); cfg.inputDevice = L"none";
    cfg.extraOutputs.clear(); cfg.extraInputs.clear(); cfg.outputMap.clear();
    cfg.loopback = LoopbackMode::Off; cfg.loopbackApp.clear();
    cfg.dmEnabled = false; cfg.bufferSize = 512;
    cfg.resampler = ResamplerQ::Hermite;
}

// Host thread (F8, "Use my settings again"): clears the marker, reloads config.ini and asks the host to reopen
// the driver (deferred through the hidden window).
void WasapiBridge::leaveSafeMode() {
    if (!safe) return;
    safe = false;
    CrashClearMarker();
    Log("Safe mode left on user request");
    loadSaved(uiCfg);                    // fix 2: in every branch; cfg follows when the streams are (re)opened
    devicesDirty = true;
    resetRequested = false;
    if (cb && resetSupported) postReset(RR_PANEL);
    else if (!buffersCreated) { cfg = uiCfg; bufSize = cfg.bufferSize; resolveDevices(); chooseRate("safe mode left"); devicesDirty = false; }
}

// Host thread (F2). Creates or destroys the floating monitor window according to cfg.monitorWindow. The window
// exists only while buffers exist (it shows the running stream).
void WasapiBridge::updateMonitorWindow() {
    // Fix 1: the window lives on msgThread only. Creation is posted (it runs from the hidden window's message
    // handler); destruction is synchronous (SendMessage from another thread, direct call on msgThread).
    if (cfg.monitorWindow && buffersCreated && !monWnd) { if (msgWnd) PostMessageW(msgWnd, WM_WB_MONCREATE, 0, 0); }
    else if ((!cfg.monitorWindow || !buffersCreated) && monWnd) {
        if (msgWnd && GetCurrentThreadId() != msgThread) SendMessageW(msgWnd, WM_WB_MONDESTROY, 0, 0); else destroyMonitorNow();
    }
    if (monWnd) SetMonitorTopmost(monWnd, cfg.monitorTopmost);
}

// msgThread (fix 1): creates the window if it is still wanted (the request was posted).
void WasapiBridge::createMonitorNow() {
    if (cfg.monitorWindow && buffersCreated && !monWnd) { monWnd = CreateMonitorWindow(this); if (monWnd) SetMonitorTopmost(monWnd, cfg.monitorTopmost); }
}

// msgThread (fix 1): DestroyMonitorWindow detaches the window from the driver first (no callback can reach it).
void WasapiBridge::destroyMonitorNow() {
    if (!monWnd) return;
    HWND w = monWnd; monWnd = nullptr;
    DestroyMonitorWindow(w);
}

// ---------------------------------------------------------------- tray icon (0.8.0, G5)
// Same life cycle rules as the monitor window (fix 1): the hidden window that owns the notification-area icon is
// created and destroyed on msgThread only, exists only while buffers exist and TrayIcon=1, and is removed by
// disposeBuffers / the destructor (synchronously, through WM_WB_TRAYDESTROY when called from another thread), so
// the icon never outlives the driver object. The audio thread never touches it.
void WasapiBridge::updateTray() {
    if (cfg.trayIcon && buffersCreated && !trayWnd) { if (msgWnd) PostMessageW(msgWnd, WM_WB_TRAYCREATE, 0, 0); }
    else if ((!cfg.trayIcon || !buffersCreated) && trayWnd) {
        if (msgWnd && GetCurrentThreadId() != msgThread) SendMessageW(msgWnd, WM_WB_TRAYDESTROY, 0, 0); else destroyTrayNow();
    }
}

// msgThread: creates the icon if it is still wanted (the request was posted).
void WasapiBridge::createTrayNow() {
    if (cfg.trayIcon && buffersCreated && !trayWnd && !dying) trayWnd = CreateTrayWindow(this);
}

// msgThread: DestroyTrayWindow detaches the window from the driver, deletes the icon and destroys the window.
void WasapiBridge::destroyTrayNow() {
    if (!trayWnd) return;
    HWND w = trayWnd; trayWnd = nullptr;
    DestroyTrayWindow(w);
}

// msgThread (left click on the tray icon): shows or hides the monitor window and stores MonitorWindow only.
void WasapiBridge::trayToggleMonitor() {
    const bool on = monWnd == nullptr;
    uiCfg.monitorWindow = on; cfg.monitorWindow = on;
    SaveGeneralFlag(L"MonitorWindow", on);
    updateMonitorWindow();               // posts the creation, or destroys directly (we are on msgThread)
}

// Host thread (F2, "Settings" in the monitor window). Same as the host's controlPanel().
void WasapiBridge::openPanelFromMonitor() { controlPanel(); }

// Host thread (F2, "Close" in the monitor window): turns the option off and saves it.
void WasapiBridge::monitorClosedByUser() {
    uiCfg.monitorWindow = false; cfg.monitorWindow = false;
    SaveGeneralFlag(L"MonitorWindow", false);   // 0.8.0: only this key (never the safe or host-profile values)
    destroyMonitorNow();                 // called from the window itself, i.e. on msgThread
}

// Host thread (F2): remembers and saves the window placement (only those keys are written).
void WasapiBridge::setMonitorPlacement(int x, int y, int w, int h, bool topmost) {
    for (Config* c : {&cfg, &uiCfg}) { c->monitorX = x; c->monitorY = y; c->monitorW = w; c->monitorH = h; c->monitorTopmost = topmost; }
    SaveMonitorPlacement(x, y, w, h, topmost);
}

// Host thread (F1). Builds the LIVE parameter block from cfg (gain, dither, the routing table computed by
// buildRoute, panel direct monitoring) and publishes it with the seqlock; the audio thread picks it up at its
// next wake-up. Also used before start() to seed the audio thread's copy.
void WasapiBridge::publishLive() {
    LiveParams p;
    p.gain = (float)cfg.gain;
    p.dither = cfg.dither;
    p.routeDefault = routeDefault;
    for (int d = 0; d < kMaxCh; ++d) p.route[d] = (int8_t)routeSrc[d];
    // Direct monitoring from the panel: only device inputs (never loopback, feedback risk), only the main output.
    p.dmOn = cfg.dmEnabled && nInDev > 0 && srcIn.cap != nullptr && nOutPrim > 0;
    p.dmIn = cfg.dmInput < nInDev ? cfg.dmInput : 0;
    p.dmOut = cfg.dmOutput < nOutPrim ? cfg.dmOutput : 0;
    p.dmMono = p.dmIn + 1 >= nInDev;
    const float lvl = cfg.dmMute ? 0.f : (float)pow(10.0, cfg.dmLevelDb / 20.0);
    const float pan = (float)(cfg.dmPan / 100.0);
    if (p.dmMono) dmix::PanGains(lvl, (pan + 1.f) * 0.5f, p.dmG[0], p.dmG[1]);   // mono: constant-power pan
    else          dmix::BalanceGains(lvl, pan, p.dmG[0], p.dmG[1]);
    // 0.8.0 (G2): hearing-safety limiter. The optional gain cap limits the Gain key while the limiter is on.
    p.limOn = cfg.limiterOn;
    p.limCeil = (float)pow(10.0, cfg.limiterCeilDb / 20.0);
    if (cfg.limiterOn && cfg.limiterCapOn) {
        const float cap = (float)pow(10.0, cfg.limiterCapDb / 20.0);
        if (p.gain > cap) p.gain = cap;
    }
    liveH = p;
    pubLive.publish(p);
}

// ---------------------------------------------------------------- IASIO
// 0.9.8 (hardening): every IASIO method is an exception barrier (AsioCall: whatever escapes the body becomes
// ASE_HWMalfunction, or ASE_NoMemory for std::bad_alloc, logged and traced) and checks the call order and its
// arguments with asiocheck.h before touching anything (see ARCHITECTURE.txt section 58). Each host-provided value
// is read once into a local copy, so a host changing its memory concurrently cannot pass a check and then use
// another value (TOCTOU).
// Host thread. sysHandle is the host's main window (used only as a fallback owner for the panel; may be null).
ASIOBool WasapiBridge::init(void* sysHandle) {
    try {
        const ASIOBool r = initImpl(sysHandle);
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwAsio | wbtrace::kKwLifecycle, "AsioInit", wbtrace::Bool("Result", r == ASIOTrue),
                 wbtrace::Str("Host", SafeLogName(HostExeName()).c_str()), wbtrace::Bool("SafeMode", safe),
                 wbtrace::Bool("Exclusive", cfg.exclusive), wbtrace::I32("BufferSize", (int32_t)cfg.bufferSize),
                 wbtrace::F64("SampleRate", rate), wbtrace::Str("Error", r == ASIOTrue ? "" : errMsg));
        return r;
    } catch (...) {
        NoteException("init");
        setError("Internal error during init");
        return ASIOFalse;
    }
}

ASIOBool WasapiBridge::initImpl(void* sysHandle) {
    if (initialized) return ASIOTrue;
    if (onAudioThread()) return ASIOFalse;               // 0.9.8: never from inside a callback of this driver
    hostWnd = (HWND)sysHandle;
    if (hostWnd && !IsWindow(hostWnd)) hostWnd = nullptr; // 0.9.8: a stale or garbage handle is ignored
    LoadConfig(cfg);
    LogInit(cfg.log);
    // 0.8.0 (G3): automatic profile for this program, applied before any device is opened. Only the executable's
    // file name is logged (never its folder), with anything outside a small safe character set replaced.
    {
        const std::wstring exe = HostExeName(), prof = HostProfileFor(exe);
        if (!prof.empty()) {
            if (LoadProfile(prof, cfg)) { hostProfile = prof; Log("Host profile: %s -> %s", SafeLogName(exe).c_str(), SafeLogName(prof).c_str()); }
            else Log("Host profile: %s -> %s (profile not found, ignored)", SafeLogName(exe).c_str(), SafeLogName(prof).c_str());
        }
    }
    uiCfg = cfg;
    // F8: a crash marker from the previous session -> safe settings for this session only (config.ini untouched)
    std::wstring crashInfo;
    // Fix 4: safe mode needs a crash marker AND a session file left behind by a process that did not exit
    // cleanly. A marker alone (the host survived the fault and closed normally) is cleared.
    const bool unclean = CrashUncleanExitFound();
    if (CrashMarkerPresent(&crashInfo) && !unclean) {
        CrashClearMarker(); Log("Crash marker found but the last session ended cleanly: cleared (%s)", Utf8(crashInfo).c_str());
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwCrash, "CrashMarkerCleared", wbtrace::Str("Why", "previous session ended cleanly"));
    }
    CrashSessionBegin();
    if (unclean && CrashMarkerPresent(&crashInfo)) {
        safe = true; applySafeSettings();
        Log("SAFE MODE: the previous session crashed (%s). Using safe settings (Shared, default output, no input, "
            "no extra devices, no loopback, no direct monitoring, buffer 512) for this session.", Utf8(crashInfo).c_str());
        WB_TRACE(wbtrace::kLvlWarning, wbtrace::kKwCrash, "SafeMode", wbtrace::Str("Marker", wbtrace::SafeText(crashInfo).c_str()));
    }
    Log("%s %s init. Mode=%s LowLatencyShared=%d BufferSize=%ld SampleRate=%.0f Gain=%.3f Loopback=%d Follow=%d Drift=%d",
        WB_NAME, WB_VERSION_STR, cfg.exclusive ? "Exclusive" : "Shared", (int)cfg.lowLatencyShared,
        cfg.bufferSize, cfg.sampleRate, cfg.gain, (int)cfg.loopback, (int)cfg.followDevices, (int)cfg.drift);

    static bool cls = false;
    if (!cls) {
        // 0.9.8: the window procedure is the barrier itself (MsgWndProc catches everything)
        WNDCLASSW wc = {}; wc.lpfnWndProc = MsgWndProc; wc.hInstance = g_hInst; wc.lpszClassName = L"WasapiBridgeMsg";
        RegisterClassW(&wc); cls = true;
    }
    if (!msgWnd) {
        msgWnd = CreateWindowExW(0, L"WasapiBridgeMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInst, nullptr);
        if (msgWnd) {
            ++g_objects; msgThread = GetCurrentThreadId(); SetWindowLongPtrW(msgWnd, GWLP_USERDATA, (LONG_PTR)this);
            SetTimer(msgWnd, WB_STATS_TIMER, 10000, nullptr);   // 0.9.8: Stats trace event (does nothing untraced)
        }
        else Log("Warning: the message window could not be created; automatic resets are disabled");
    }

    // 0.9.8: a second init after a failed one reuses the enumerator (before, it was created again and the first
    // reference leaked).
    HRESULT hr = enumr ? S_OK : CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumr);
    if (FAILED(hr) || !enumr) { lastHr = (long)hr; setError("Could not create IMMDeviceEnumerator"); return ASIOFalse; }
    if (!resolveDevices()) return ASIOFalse;
    if (!notifier) {
        notifier = new DeviceNotifier(this);
        enumr->RegisterEndpointNotificationCallback(notifier);
    }

    chooseRate("init");                                // 0.9.5 precedence; 0.8.0 (G4) Auto inside
    bufSize = cfg.bufferSize;
    if (!outEvent)  outEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stopEvent) stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!outEvent || !stopEvent) { setError("CreateEvent failed"); return ASIOFalse; }   // 0.9.8: checked
    initialized = true;
    RegisterInstance(this);              // 0.7.1: the panel can find the instance that replaces this one
    return ASIOTrue;
}

// 0.9.8: ASIO gives these three no size argument; asio.h fixes the buffers at 32 (name) and 124 (error message)
// bytes. Both strings are bounded below those sizes (static_assert / errMsg[124]), and null pointers are ignored.
static_assert(sizeof(WB_NAME) <= 32, "driver name must fit the 32 bytes of getDriverName");
void WasapiBridge::getDriverName(char* name) {
    if (name) memcpy(name, WB_NAME, sizeof(WB_NAME));
}
long WasapiBridge::getDriverVersion() { return WB_VERSION_NUM; }
// Review R1 (independent review of 0.9.8): only the message and its terminator are copied (at most 124 bytes),
// as in 0.9.7, so a host buffer that is exactly as long as the message plus NUL is never written past.
void WasapiBridge::getErrorMessage(char* s) {
    if (!s) return;
    static_assert(sizeof(errMsg) <= 124, "error message must fit the 124 bytes of getErrorMessage");
    const size_t n = strnlen(errMsg, sizeof(errMsg) - 1);
    memcpy(s, errMsg, n);
    s[n] = 0;
}

ASIOError WasapiBridge::getChannels(long* numIn, long* numOut) {
    return AsioCall(!onAudioThread(), "getChannels", wbtrace::kLvlVerbose, [&]() -> ASIOError {
        if (!initialized) return ASE_NotPresent;
        if (!numIn || !numOut) return ASE_InvalidParameter;      // F8 hardening: every host pointer is checked
        *numIn = nIn; *numOut = nOut; return ASE_OK;
    });
}

// Output latency = ASIO buffer + WASAPI buffer + stream latency; input = ASIO buffer + input FIFO target.
// 0.7.0 (F4): plus the user's latency offsets (ms, converted at the current rate); never below the ASIO buffer.
ASIOError WasapiBridge::getLatencies(long* inLat, long* outLat) {
    return AsioCall(!onAudioThread(), "getLatencies", wbtrace::kLvlVerbose, [&]() -> ASIOError {
        if (!initialized) return ASE_NotPresent;
        if (!inLat || !outLat) return ASE_InvalidParameter;
        latenciesNow(*inLat, *outLat);
        return ASE_OK;
    });
}

// Host thread (getLatencies, getStatus). The reported latencies, computed into locals (no host memory involved).
void WasapiBridge::latenciesNow(long& inL, long& outL) {
    long minIn = bufSize;
    if (streamsOpen) {
        // Aligned: every output plays with the latency of the slowest one (alignOut, which includes the delay added
        // to the primary); inputs: srcIn.target already includes the alignment shift.
        // 0.9.4: primOutLat / targets are in ASIO frames; a converted output includes the converter's look-ahead and
        // the device part (frames kept queued + stream latency) converted to the ASIO rate (sched::OutLatency). The
        // in/out split is sched::ReportLatencies (shared with the offline model): an aligned converted input reports
        // its target (InLatency) without adding the block again, so in + out = the exact round trip.
        const bool alignedIn = srcIn.client && srcIn.alignedIn;
        const sched::Reported r = sched::ReportLatencies(bufSize, (long)(aligned ? alignOut : primOutLat),
                                                         (long)(srcIn.client ? srcIn.target : (aligned ? alignIn : 0)), alignedIn);
        outL = r.out; inL = r.in;
        minIn = alignedIn ? 0 : bufSize;
    } else {
        long p = (long)(rate * (cfg.exclusive ? 0.003 : 0.010));
        outL = bufSize + p; inL = bufSize + p;
    }
    outL += lround(cfg.outLatOffsetMs * rate / 1000.0);
    inL  += lround(cfg.inLatOffsetMs * rate / 1000.0);
    if (outL < bufSize) outL = bufSize;
    if (inL < minIn) inL = minIn;      // an aligned converted input may be shorter than one block
}

ASIOError WasapiBridge::getBufferSize(long* minSize, long* maxSize, long* prefSize, long* granularity) {
    return AsioCall(!onAudioThread(), "getBufferSize", wbtrace::kLvlVerbose, [&]() -> ASIOError {
        if (!minSize || !maxSize || !prefSize || !granularity) return ASE_InvalidParameter;
        *minSize = asiochk::kMinBuffer; *maxSize = asiochk::kMaxBuffer; *prefSize = cfg.bufferSize; *granularity = -1;
        return ASE_OK;
    });
}

// IASIO, host thread. Accepts any known rate: Shared converts in the Windows engine, Exclusive falls back
// to Shared if the device rejects the rate when the streams are opened. 0.9.8: NaN / infinite rates refused.
ASIOError WasapiBridge::canSampleRate(ASIOSampleRate r) {
    return AsioCall(!onAudioThread(), "canSampleRate", wbtrace::kLvlVerbose, [&]() -> ASIOError {
        if (!initialized) return ASE_NotPresent;
        return (ASIOError)asiochk::CheckCanSampleRate(r);
    });
}

ASIOError WasapiBridge::getSampleRate(ASIOSampleRate* r) {
    return AsioCall(!onAudioThread(), "getSampleRate", wbtrace::kLvlVerbose, [&]() -> ASIOError {
        if (!r) return ASE_InvalidParameter;
        *r = rate; return ASE_OK;
    });
}

// IASIO, host thread. Same rate = no-op. Otherwise stops if running, reopens the streams at the new rate
// (when buffers exist), sends kAsioLatenciesChanged if the host supports it, and restarts if it was running.
// 0.9.8: refused from the audio thread (it would stop and join itself) and for NaN / infinite / negative rates.
ASIOError WasapiBridge::setSampleRate(ASIOSampleRate r) {
    return AsioCall(!onAudioThread(), "setSampleRate", wbtrace::kLvlInfo, [&]() -> ASIOError {
        if (!initialized) return ASE_NotPresent;
        if (onAudioThread()) return ASE_InvalidMode;
        bool noChange = false;
        const asiochk::Err e = asiochk::CheckSetSampleRate(r, noChange);
        if (e != asiochk::kOk) return (ASIOError)e;
        if (noChange) return ASE_OK;
        hostSetRate = true; rateSrc = rp::Src::Host;
        // 0.9.5: remembered for driver resets, re-created instances in this process and new processes (LastHostRate)
        SetProcessHostRate(r);
        if (fabs(ReadLastHostRate() - r) >= 0.5) { WriteLastHostRate(r); Log("LastHostRate=%.0f", r); }
        pendingAutoRate = 0;                  // fix 6: the host's own choice wins over a pending Windows rate
        if (fabs(r - rate) < 0.5) return ASE_OK;
        Log("setSampleRate %.0f -> %.0f", rate, r);
        bool wasRunning = running;
        if (wasRunning) stop();
        rate = r;
        if (buffersCreated) {
            closeStreams();
            if (!openStreams()) return ASE_HWMalfunction;
            ASIOCallbacks* c = cb;
            if (c && latChangedSupported && c->asioMessage) c->asioMessage(kAsioLatenciesChanged, 0, nullptr, nullptr);
        }
        if (wasRunning) start();
        return ASE_OK;
    });
}

// IASIO, host thread. A single internal clock: the WASAPI output device paces everything.
ASIOError WasapiBridge::getClockSources(ASIOClockSource* clocks, long* numSources) {
    return AsioCall(!onAudioThread(), "getClockSources", wbtrace::kLvlVerbose, [&]() -> ASIOError {
        const long cap = numSources ? *numSources : 0;               // read once
        const asiochk::Err e = asiochk::CheckClockSources(clocks != nullptr, numSources != nullptr, cap);
        if (e != asiochk::kOk) return (ASIOError)e;
        ASIOClockSource c = {};
        c.index = 0; c.associatedChannel = -1; c.associatedGroup = -1; c.isCurrentSource = ASIOTrue;
        snprintf(c.name, sizeof c.name, "%s", "Internal (WASAPI)");
        clocks[0] = c;
        *numSources = 1; return ASE_OK;
    });
}

// Review R2: like 0.9.7 in every state (reference 0 = the single internal clock: ASE_OK, also before init).
ASIOError WasapiBridge::setClockSource(long ref) {
    return AsioCall(!onAudioThread(), "setClockSource", wbtrace::kLvlInfo, [&]() -> ASIOError {
        return ref == 0 ? ASE_OK : ASE_InvalidMode;
    });
}

// Audio thread. Publishes (samplePosition, systemTime) for getSamplePosition with a seqlock:
//   1. increment tsSeq (becomes odd = write in progress),
//   2. store both values (relaxed),
//   3. increment tsSeq again with release (even = stable).
// The writer never blocks or allocates, so it is real-time safe. Readers retry while the counter is odd or
// changed during their read (see getSamplePosition).
void WasapiBridge::publishTime(uint64_t pos, uint64_t ns) {
    tsSeq.fetch_add(1, std::memory_order_acq_rel);   // odd: write in progress
    tsPos.store(pos, std::memory_order_relaxed);
    tsNs.store(ns, std::memory_order_relaxed);
    tsSeq.fetch_add(1, std::memory_order_release);   // even: stable
}

// IASIO; any thread (hosts call it from their own threads and from bufferSwitch). Seqlock reader: load tsSeq
// (acquire); if odd, a write is in progress -> spin (YieldProcessor, then SwitchToThread after 100 tries);
// otherwise read both values, fence (acquire) and re-check tsSeq. A matching counter means the pair is
// consistent. After 1000 failed tries it returns the last consistent pair (lastP/lastT), so it never blocks.
// Returns ASE_SPNotAdvancing while not running, ASE_NotPresent before init (0.9.8).
// Real-time safe: no allocation, no lock, no trace; nothing in it can throw (no barrier needed on this hot path).
ASIOError WasapiBridge::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    if (!sPos || !tStamp) return ASE_InvalidParameter;
    if (!initialized) return ASE_NotPresent;
    uint64_t p = lastP.load(std::memory_order_relaxed), t = lastT.load(std::memory_order_relaxed);   // fallback
    for (int tries = 0; tries < 1000; ++tries) {
        uint32_t s1 = tsSeq.load(std::memory_order_acquire);
        if (s1 & 1) { if (tries > 100) SwitchToThread(); else YieldProcessor(); continue; }
        uint64_t p1 = tsPos.load(std::memory_order_relaxed), t1 = tsNs.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (tsSeq.load(std::memory_order_relaxed) == s1) {
            p = p1; t = t1;
            lastP.store(p1, std::memory_order_relaxed); lastT.store(t1, std::memory_order_relaxed);
            break;
        }
    }
    ToAsio64(p, sPos->hi, sPos->lo);
    ToAsio64(t, tStamp->hi, tStamp->lo);
    return running ? ASE_OK : ASE_SPNotAdvancing;
}

// IASIO, host thread. Channel names: "In L/R" / "Out L/R" for stereo, "In n" / "Out n" otherwise, prefixed
// with "<device>:" when extra devices exist; the last two inputs are "Loopback L/R" when loopback is on.
// 0.9.8: isInput and channel are read once (a host changing them concurrently cannot pass the range check with one
// value and index with another).
ASIOError WasapiBridge::getChannelInfo(ASIOChannelInfo* info) {
    return AsioCall(!onAudioThread(), "getChannelInfo", wbtrace::kLvlVerbose, [&]() -> ASIOError {
        if (!initialized) return ASE_NotPresent;
        if (!info) return ASE_InvalidParameter;
        const bool isIn = info->isInput != ASIOFalse;
        const long c = info->channel;
        if (asiochk::CheckChannelIndex(c, isIn, nIn, nOut) != asiochk::kOk) return ASE_InvalidParameter;
        char name[32];
        channelName(isIn, c, name);
        info->isActive     = isIn ? inActive[c] : outActive[c];
        info->channelGroup = 0;
        info->type         = ASIOSTFloat32LSB;
        static_assert(sizeof(info->name) == sizeof(name), "ASIOChannelInfo::name is 32 bytes");
        memcpy(info->name, name, sizeof name);
        return ASE_OK;
    });
}

// Host thread. Name of ASIO channel c (0 <= c < kMaxCh, checked by the caller) into out[32], NUL-terminated.
void WasapiBridge::channelName(bool isIn, long c, char (&out)[32]) {
    // 0.8.0 (G6): names built from the devices at resolve time (DeviceChannelNames=1, the default)
    if (cfg.deviceChannelNames) {
        const char* nm = isIn ? chNameIn[c] : chNameOut[c];
        if (nm[0]) { snprintf(out, sizeof out, "%s", nm); return; }
    }
    if (isIn && c >= lbFirst) { snprintf(out, sizeof out, "Loopback %s", c - lbFirst == 0 ? "L" : "R"); return; }
    // Device the channel belongs to: 1 = main, 2.. = extra devices (prefix only when extras exist)
    const ExtraDev* ex = isIn ? inEx : outEx;
    const int nEx = isIn ? nInEx : nOutEx;
    long first = 0, total = isIn ? nInDev : nOutPrim; int devNo = 1;
    for (int k = 0; k < nEx; ++k)
        if (c >= ex[k].asioFirst && c < ex[k].asioFirst + ex[k].ch) { first = ex[k].asioFirst; total = ex[k].ch; devNo = k + 2; }
    const long local = c - first;
    char base[24];
    if (total == 2) snprintf(base, sizeof base, "%s %s", isIn ? "In" : "Out", local == 0 ? "L" : "R");
    else            snprintf(base, sizeof base, "%s %ld", isIn ? "In" : "Out", local + 1);
    if (nEx > 0) snprintf(out, sizeof out, "%d:%s", devNo, base);
    else         snprintf(out, sizeof out, "%s", base);
}

// Host thread. Releases the ASIO double buffers (one block of floats). If the audio thread is stuck in the
// host, the memory is deliberately leaked because that thread may still write to it.
// 0.9.8: the leak holder is allocated with std::nothrow (this runs inside the destructor, where an exception would
// terminate the host); if even that fails, bufMem simply stays in this object, which is itself leaked while the
// thread is stuck (teardown), and createBuffers refuses to run until the thread has ended (stuckAlive).
void WasapiBridge::freeBuffers() {
    if (threadStuck) {
        if (auto* keep = new (std::nothrow) std::vector<float>()) keep->swap(bufMem);   // leaked on purpose
    }
    if (!threadStuck) { bufMem.clear(); bufMem.shrink_to_fit(); }
    memset(outBufs, 0, sizeof(outBufs)); memset(inBufs, 0, sizeof(inBufs));
    memset(outActive, 0, sizeof(outActive)); memset(inActive, 0, sizeof(inActive));
    buffersCreated = false;
}

// IASIO, host thread. Opens the WASAPI streams if needed (or reopens them after a device/config change),
// validates the requested channels, allocates the double buffers and stores the host callbacks. Also queries
// the host's asioMessage capabilities (time info, reset request, overload, latency changed).
// 0.9.8: every argument is validated BEFORE existing buffers are disposed (a bad second call leaves the running
// configuration alone only if it fails these checks; a valid one replaces it, as ASIO requires), the callbacks
// are copied into cbStore, and the channel list is copied and only the copy is used (asiocheck.h CopyInfos).
ASIOError WasapiBridge::createBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* cbs) {
    return AsioCall(!onAudioThread(), "createBuffers", wbtrace::kLvlInfo, [&]() -> ASIOError {
        if (!initialized) return ASE_NotPresent;
        if (onAudioThread()) return ASE_InvalidMode;
        // 1. arguments that do not depend on the device layout
        if (!cbs) return ASE_InvalidMode;
        ASIOCallbacks cbCopy;
        memcpy(&cbCopy, cbs, sizeof cbCopy);                 // read once: the copy is what the driver uses
        asiochk::Err e = asiochk::CheckCallbacks(true, cbCopy.bufferSwitch != nullptr, cbCopy.bufferSwitchTimeInfo != nullptr);
        if (e == asiochk::kOk) e = asiochk::CheckBufferSize(bufferSize);
        if (e == asiochk::kOk && numChannels < 1) e = asiochk::kInvalidMode;
        if (e == asiochk::kOk && !infos) e = asiochk::kInvalidParameter;
        if (e != asiochk::kOk) return (ASIOError)e;
        if (stuckAlive()) { setError("Previous audio thread is blocked in the host"); return ASE_HWMalfunction; }
        if (buffersCreated) disposeImpl();
        if (devicesDirty) {                                   // device/config change without a full host reset
            long oi = nIn, oo = nOut;
            loadSaved(uiCfg);                                   // fix 2: reload the saved configuration ...
            cfg = uiCfg;
            if (safe) applySafeSettings();                      // ... and the safe overrides survive a reload
            if (!resolveDevices()) return ASE_NotPresent;
            chooseRate("reload after a device or settings change");   // 0.9.5; G4 Auto needs the (new) output device
            if (oi != nIn || oo != nOut) Log("Warning: channel count changed (%ld/%ld -> %ld/%ld); the host must reload the driver", oi, oo, nIn, nOut);
        }
        // 2. the channel list against the current layout (F8 hardening, 0.9.8: copied, see asiocheck.h)
        asiochk::Chan req[2 * kMaxCh];
        e = asiochk::CopyInfos(infos, numChannels, nIn, nOut, req, 2 * kMaxCh);
        if (e != asiochk::kOk) return (ASIOError)e;
        bufSize = bufferSize;
        try {
            bufMem.assign((size_t)numChannels * 2 * (size_t)bufSize, 0.f);   // at most 64 * 2 * 4096 floats (2 MB)
        } catch (const std::bad_alloc&) {
            setError("Out of memory for the ASIO buffers");
            return ASE_NoMemory;
        }
        for (long i = 0; i < numChannels; ++i) {
            float* a = &bufMem[(size_t)i * 2 * bufSize];
            float* b = a + bufSize;
            infos[i].buffers[0] = a; infos[i].buffers[1] = b;
            const int ch = req[i].ch;                          // the validated copy, never the host's array
            if (req[i].input) { inBufs[ch][0] = a; inBufs[ch][1] = b; inActive[ch] = true; }
            else              { outBufs[ch][0] = a; outBufs[ch][1] = b; outActive[ch] = true; }
        }
        cbStore = cbCopy;
        cb = &cbStore;
        timeInfoMode = resetSupported = overloadSupported = latChangedSupported = false;
        if (cb->asioMessage) {
            timeInfoMode        = cb->asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1 &&
                                  cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
            resetSupported      = cb->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr) == 1;
            overloadSupported   = cb->asioMessage(kAsioSelectorSupported, kAsioOverload, nullptr, nullptr) == 1;
            latChangedSupported = cb->asioMessage(kAsioSelectorSupported, kAsioLatenciesChanged, nullptr, nullptr) == 1;
        }
        buffersCreated = true;
        Log("createBuffers: channels=%ld buffer=%ld timeInfo=%d reset=%d overload=%d latChanged=%d", numChannels, bufSize,
            (int)timeInfoMode, (int)resetSupported, (int)overloadSupported, (int)latChangedSupported);
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwAsio, "AsioBuffers", wbtrace::I32("Channels", (int32_t)numChannels),
                 wbtrace::I32("BufferSize", (int32_t)bufSize), wbtrace::Bool("TimeInfo", timeInfoMode), wbtrace::Bool("ResetRequest", resetSupported),
                 wbtrace::Bool("Overload", overloadSupported), wbtrace::Bool("LatenciesChanged", latChangedSupported));
        if (!openStreams()) { freeBuffers(); cb = nullptr; return ASE_HWMalfunction; }
        if (!cb->bufferSwitchTimeInfo) timeInfoMode = false;       // never call a null callback
        if (!cb->bufferSwitch) timeInfoMode = true;                // only the time-info callback was given
        updateMonitorWindow();                                      // F2: lazily created once a stream exists
        updateTray();                                               // G5: same rule for the tray icon
        return ASE_OK;
    });
}

// IASIO, host thread. Stops if running and frees the buffers. Refused from the audio thread itself.
// Review R2 (independent review of 0.9.8): lenient like 0.9.7 - without buffers, and before init, it is a no-op
// that returns ASE_OK (some hosts dispose defensively and treat any error as fatal); asiocheck.h Allowed says so.
ASIOError WasapiBridge::disposeBuffers() {
    return AsioCall(!onAudioThread(), "disposeBuffers", wbtrace::kLvlInfo, [&]() -> ASIOError {
        const asiochk::Err e = asiochk::Allowed(asiochk::Call::DisposeBuffers, asiochk::StateOf(initialized, buffersCreated, running));
        if (e != asiochk::kOk) return (ASIOError)e;
        return disposeImpl();
    });
}

// Host thread. The work of disposeBuffers, also used internally (destructor, createBuffers) in any state.
ASIOError WasapiBridge::disposeImpl() {
    if (onAudioThread()) return ASE_InvalidMode;     // never free from the audio thread itself
    if (running || thread) stop();
    if (monWnd) { if (msgWnd && GetCurrentThreadId() != msgThread) SendMessageW(msgWnd, WM_WB_MONDESTROY, 0, 0); else destroyMonitorNow(); }   // F2: never outlives the buffers
    if (trayWnd) { if (msgWnd && GetCurrentThreadId() != msgThread) SendMessageW(msgWnd, WM_WB_TRAYDESTROY, 0, 0); else destroyTrayNow(); }   // G5
    if (buffersCreated) ++streamGen;      // fix 9: open panels notice that the driver was reopened
    closeStreams();
    if (pendingAutoRate > 0) {                            // fix 6: the Windows rate change takes effect now
        rate = pendingAutoRate; pendingAutoRate = 0;
        Log("SampleRate=Auto: ASIO rate now %.0f Hz", rate);
    }
    if (buffersCreated) Log("disposeBuffers");
    // F8: a session that started and stopped cleanly in safe mode clears the crash marker, so the next load uses
    // the user's settings again (safe mode itself stays on for the rest of this session).
    if (safe && hadStart && !threadStuck && CrashMarkerPresent()) {
        CrashClearMarker(); Log("Clean session in safe mode: crash marker cleared");
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwCrash, "CrashMarkerCleared", wbtrace::Str("Why", "clean session in safe mode"));
    }
    freeBuffers();
    cb = nullptr;
    return ASE_OK;
}

// IASIO, host thread. Resets all per-run state (counters, FIFOs, dither seeds, alignment delay, timestamp
// anchor) BEFORE the audio thread is created, so the thread starts with a consistent state and needs no locks.
// Raises the timer resolution to 1 ms for timeGetTime, then creates the audio thread.
// 0.9.8: from the audio thread it only reports success while running (it would otherwise wait for itself); the
// wait for a thread that is still exiting is bounded (10 s, like stop) and a thread that does not end is handled
// as stuck (its resources are kept, see stuckAlive) instead of blocking the host forever.
ASIOError WasapiBridge::start() {
    return AsioCall(!onAudioThread(), "start", wbtrace::kLvlInfo, [&]() -> ASIOError {
        const asiochk::Err e = asiochk::Allowed(asiochk::Call::Start, asiochk::StateOf(initialized, buffersCreated, running));
        if (e != asiochk::kOk) return (ASIOError)e;
        if (!streamsOpen) return ASE_InvalidMode;
        if (onAudioThread()) return running ? ASE_OK : ASE_InvalidMode;
        if (stuckAlive()) return ASE_HWMalfunction;
        if (thread && WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) {    // thread ended because of an error
            CloseHandle(thread); thread = nullptr; running = false;
            if (timerRaised) { timeEndPeriod(1); timerRaised = false; }
        }
        if (running) return ASE_OK;
        if (thread) {                          // thread exiting after a stop request from the audio thread: wait for it
            SetEvent(stopEvent);
            if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) {
                Log("start: the previous audio thread did not end within 10 s (host callback or device call blocked)");
                threadStuck = true; stuckThread = thread; thread = nullptr;
                setError("Previous audio thread is blocked");
                return ASE_HWMalfunction;
            }
            CloseHandle(thread); thread = nullptr;
            if (timerRaised) { timeEndPeriod(1); timerRaised = false; }
        }
        return startThread();
    });
}

// Host thread (start). Per-run state reset and thread creation; see start().
ASIOError WasapiBridge::startThread() {
    outFifo.clear(); monFifo.clear();
    InputSource* srcs[2 + kMaxExtra] = {&srcIn, &srcLb, &srcEx[0], &srcEx[1], &srcEx[2]};
    for (int c = 0; c < kMaxCh; ++c) ditherState[c] = 0x9E3779B9u * (uint32_t)(c + 1);   // distinct, non-zero seeds
    for (OutputSink& o : sinks) { o.fifo.clear(); o.primed = false; o.pos = o.posBase; o.ratio = 1.0; o.ratioView = 1.0;
                                  o.fillEma = o.sp; o.integ = 0.0; o.written = 0; o.underflows = 0; o.overflows = 0;
                                  for (int c = 0; c < kMaxCh; ++c) o.ditherState[c] = 0x85EBCA6Bu * (uint32_t)(c + 7); }
    primDelay.clear();                    // alignment delay: starts silent (memory reserved when opening)
    for (UINT32 i = 0; i < primDelayFrames; ++i) { float* f = primDelay.pushSlot(); for (int c = 0; c < primDelay.ch; ++c) f[c] = 0.f; }
    for (InputSource* s : srcs) { s->fifo.clear(); s->primed = false; s->pos = s->posBase; s->ratio = 1.0; s->ratioView = 1.0;
                                  s->integ = 0.0; s->underruns = 0; s->drops = 0; s->glitches = 0; }
    // 0.7.0: the audio thread starts with the current live parameters and silent monitoring ramps.
    lv = liveH; lvSeq = pubLive.version();
    for (int c = 0; c < kMaxCh; ++c) { hostRampL[c].cur = 0.f; hostRampR[c].cur = 0.f; }
    dmRamp[0].cur = dmRamp[1].cur = 0.f;
    monK = dmix::RampCoef(outDevRate); monFeed = false; monUnder = 0;
    rateRun = rate;                       // 0.8.3: the audio thread only reads this copy (see driver.h)
    limRel = lim::ReleaseCoef(rate);      // G2: 150 ms release at the stream rate (extra outputs)
    limRelMain = lim::ReleaseCoef(outDevRate);   // 0.9.4: writeOut runs at the device rate when converting
    limMain = lim::Limiter{}; for (lim::Limiter& l : limSink) l = lim::Limiter{};
    limGr = 0.f;
    curIndex = 0;
    samplePos = 0; blockClock.reset();
    if (outConvOn) outConv.reset();       // 0.9.4: silent history, exact phase 0
    for (InputSource* s : srcs) { if (s->convOn) s->conv.reset(); s->qHead = 0; s->qMade = 0; s->qOff = 0; }
    publishTime(0, (uint64_t)timeGetTime() * 1000000ull);
    cntSwitches = 0; cntOutGlitch = 0; cntTimeouts = 0;
    for (int i = 0; i < kMaxCh; ++i) { peakIn[i] = 0.f; peakOut[i] = 0.f; }
    clipInMask = 0; clipOutMask = 0; loadNow = 0.f; loadMax = 0.f; resetReq = 0;
    loadEma = 0; loadMaxA = 0; loadSkip = 8;   // audio-thread state: written before the thread is created
    resetRequested = false; pendingReset = RR_NONE; pendingOverload = false;
    ResetEvent(stopEvent);
    ResetEvent(outEvent);                  // drop a stale signal from the previous cycle
    if (!timerRaised) { timeBeginPeriod(1); timerRaised = true; }   // 1 ms resolution for timeGetTime (ASIO timestamps)
    running = true;
    cbRun = cb;
    thread = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread) { running = false; timeEndPeriod(1); timerRaised = false; setError("CreateThread failed"); return ASE_HWMalfunction; }
    hadStart = true;
    Log("start");
    return ASE_OK;
}

// IASIO, host thread (or from inside a host callback on the audio thread: then it only signals stopEvent).
// Signals the thread and waits up to 10 s. A thread that does not finish (blocked inside the host callback) is
// recorded as stuck: its resources are kept alive and stuckAlive() closes the handle later. Logs run statistics.
ASIOError WasapiBridge::stop() {
    return AsioCall(!onAudioThread(), "stop", wbtrace::kLvlInfo, [&]() -> ASIOError { return stopImpl(); });
}

ASIOError WasapiBridge::stopImpl() {
    if (!thread) { running = false; return ASE_OK; }
    if (onAudioThread()) { SetEvent(stopEvent); return ASE_OK; }   // called from inside a host callback
    SetEvent(stopEvent);
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0) {
        Log("stop: the audio thread did not finish within 10 s (host callback blocked); its resources are left allocated");
        WB_TRACE(wbtrace::kLvlError, wbtrace::kKwError | wbtrace::kKwStream, "AudioThreadStuck", wbtrace::U32("WaitMs", 10000));
        threadStuck = true;
        stuckThread = thread;           // closed in stuckAlive() once the thread ends
    } else {
        CloseHandle(thread);
        if (timerRaised) { timeEndPeriod(1); timerRaised = false; }
    }
    thread = nullptr;
    running = false;
    Log("stop: bufferSwitch=%llu outGlitch=%u timeouts=%u maxLoad=%.0f%% | input underrun=%u drop=%u glitch=%u ppm=%.1f | loopback underrun=%u drop=%u ppm=%.1f",
        (unsigned long long)cntSwitches.load(), cntOutGlitch.load(), cntTimeouts.load(), (double)loadMax.load(),
        srcIn.underruns.load(), srcIn.drops.load(), srcIn.glitches.load(), (srcIn.ratioView.load() - 1.0) * 1e6,
        srcLb.underruns.load(), srcLb.drops.load(), (srcLb.ratioView.load() - 1.0) * 1e6);
    for (int k = 0; k < nOutEx; ++k)
        Log("  %s: underflow=%u overflow=%u ppm=%.1f", sinks[k].tag, sinks[k].underflows.load(), sinks[k].overflows.load(), (sinks[k].ratioView.load() - 1.0) * 1e6);
    for (int k = 0; k < nInEx; ++k)
        Log("  %s: underrun=%u drop=%u ppm=%.1f", srcEx[k].tag, srcEx[k].underruns.load(), srcEx[k].drops.load(), (srcEx[k].ratioView.load() - 1.0) * 1e6);
    onStatsTimer();                       // 0.9.8: final counters of the run (Stats event, only when traced)
    return ASE_OK;
}

// IASIO, host thread. Shows the modal settings panel (panel.cpp). AddRef/Release keeps the driver alive while
// the panel pumps messages, because the host could release it from a nested message.
// 0.7.1: ShowControlPanel holds its own reference and may move it to a newer driver instance (rebind after the
// host reloads the driver). It releases whatever instance it holds at the end, so this function must not touch
// `this` after the call (the object may already be gone). 0.9.8: AsioCall does not touch it either.
ASIOError WasapiBridge::controlPanel() {
    return AsioCall(!onAudioThread(), "controlPanel", wbtrace::kLvlInfo, [&]() -> ASIOError {
        if (onAudioThread()) return ASE_InvalidMode;         // 0.9.8: never a modal loop on the audio thread
        ShowControlPanel(this);
        return ASE_OK;
    });
}

// IASIO, host thread. Supports kAsioCanTimeInfo, kAsioCanReportOverload and kAsioCanInputMonitor /
// kAsioSetInputMonitor (direct monitoring: equal-power pan gains and target channel are stored for the audio
// thread, which reads them each block). Other selectors return ASE_InvalidParameter.
// 0.9.8: `opt` is checked for null by every selector that reads it, and copied once before use.
ASIOError WasapiBridge::future(long selector, void* opt) {
    return AsioCall(!onAudioThread(), "future", wbtrace::kLvlVerbose, [&]() -> ASIOError { return futureImpl(selector, opt); });
}

ASIOError WasapiBridge::futureImpl(long selector, void* opt) {
    switch (selector) {
    case kAsioCanTimeInfo:       return ASE_SUCCESS;
    case kAsioCanReportOverload: return ASE_SUCCESS;
    case kAsioCanInputMonitor:
        // 0.9.4: not present when the open streams cannot monitor (monPath 0: converted output with an input at
        // another rate or clock); before an open, the availability of the last open of this instance
        if (nInDev <= 0) return ASE_NotPresent;
        if (streamsOpen ? (srcIn.cap && monPath == 0) : monAvailLast == 0) return ASE_NotPresent;
        return ASE_SUCCESS;
    case kAsioSetInputMonitor: {
        // 0.7.0 (F5): the host's direct monitoring ("ASIO Direct Monitoring" in Cubase). Validated, then stored as
        // per-input targets that the audio thread ramps to (no clicks). Switching a monitor off keeps its
        // destination and ramps the gain to 0. input = -1 means every device input.
        if (!opt) return ASE_InvalidParameter;
        ASIOInputMonitor m;
        memcpy(&m, opt, sizeof m);                 // 0.9.8: one read of the host's structure
        if (nInDev == 0 || m.input >= nInDev || m.input < -1) return ASE_SUCCESS;   // loopback / no input: ignored without rejecting the selector
        const float g = dmix::AsioGainToLinear(m.gain);          // 0x20000000 = 0 dB, 0x7fffffff = +12 dB
        const float p = dmix::AsioPanTo01(m.pan);                 // 0 = left, 1 = right
        float gl = 0.f, gr = 0.f;
        dmix::PanGains(g, p, gl, gr);
        long first = m.input < 0 ? 0 : m.input, last = m.input < 0 ? nInDev - 1 : m.input;
        long outL = m.output < 0 ? 0 : m.output;
        if (outL >= nOutPrim) outL = 0;              // main output channels only (never reaches extra devices)
        for (long i = first; i <= last && i < kMaxCh; ++i) {
            monGainL[i] = m.state ? gl : 0.f; monGainR[i] = m.state ? gr : 0.f;
            monOut[i] = (int)outL;
        }
        bool any = false;
        for (long i = 0; i < nInDev && i < kMaxCh; ++i) if (monGainL[i].load() > 0.f || monGainR[i].load() > 0.f) any = true;
        monAny = any;
        Log("InputMonitor in=%ld out=%ld gain=%.3f pan=%.2f state=%d", m.input, m.output, g, p, (int)m.state);
        return ASE_SUCCESS;
    }
    case WB_FUTURE_ENGINE_INFO: {          // 0.9.4: private selector of the test tool (common.h); 0.9.8: with a header
        asiochk::EngineInfo* ei = (asiochk::EngineInfo*)opt;
        if (asiochk::CheckEngineInfo(ei) != asiochk::kOk) return ASE_InvalidParameter;
        snprintf(ei->text, sizeof ei->text, "%s", streamsOpen ? engineLine : "streams not open");
        return ASE_SUCCESS;
    }
    // kAsioCanInputGain / kAsioCanOutputGain / kAsioCanInputMeter / kAsioCanOutputMeter and the rest are not
    // implemented: ASE_InvalidParameter tells the host so (it must not rely on them).
    default: return ASE_InvalidParameter;
    }
}

ASIOError WasapiBridge::outputReady() { return ASE_NotPresent; }

// 0.9.8, host thread (WM_TIMER of msgWnd every 10 s, and once at stop): one "Stats" event with the counters of the
// running stream, only when a session listens for kKwStats. The audio thread never traces; it only keeps these
// counters (atomics), which are read here. Counters only: no audio data, no levels.
void WasapiBridge::onStatsTimer() {
    if (!wbtrace::Enabled(wbtrace::kLvlInfo, wbtrace::kKwStats) || !streamsOpen) return;
    uint32_t sinkUnder = 0, sinkOver = 0, exUnder = 0, exDrops = 0;
    for (const OutputSink& o : sinks) { sinkUnder += o.underflows.load(); sinkOver += o.overflows.load(); }
    for (const InputSource& x : srcEx) { exUnder += x.underruns.load(); exDrops += x.drops.load(); }
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwStats, "Stats", wbtrace::Bool("Running", running.load()),
             wbtrace::U64("BufferSwitches", cntSwitches.load(std::memory_order_relaxed)), wbtrace::U32("OutputGlitches", cntOutGlitch.load()),
             wbtrace::U32("WaitTimeouts", cntTimeouts.load()), wbtrace::U32("InputUnderruns", srcIn.underruns.load()),
             wbtrace::U32("InputDrops", srcIn.drops.load()), wbtrace::U32("InputGlitches", srcIn.glitches.load()),
             wbtrace::U32("LoopbackUnderruns", srcLb.underruns.load()), wbtrace::U32("ExtraInputUnderruns", exUnder),
             wbtrace::U32("ExtraInputDrops", exDrops), wbtrace::U32("ExtraOutputUnderflows", sinkUnder),
             wbtrace::U32("ExtraOutputOverflows", sinkOver), wbtrace::F64("DspLoadPct", (double)loadNow.load()),
             wbtrace::F64("DspLoadMaxPct", (double)loadMax.load()), wbtrace::F64("InputDriftPpm", (srcIn.ratioView.load() - 1.0) * 1e6),
             wbtrace::U32("MonitorUnderruns", monUnder.load()));
}

// ---------------------------------------------------------------- WASAPI: opening streams (host thread)
// event = true: event-driven render/capture (duration = periodicity). false: polled capture with a buffer
// of 4x the output period to tolerate jitter in the thread's wake-ups.
// Host thread. Tries to initialize dev in Exclusive mode: candidate formats float32/int32/int24/int16 at the
// current rate with wantCh channels; on AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED it retries with the aligned duration.
// out/fmt/frames/period receive the client, format, buffer size and period. Returns false if nothing worked
// (lastExclHr keeps the last HRESULT so the caller can detect privacy blocking).
bool WasapiBridge::initExclusive(IMMDevice* dev, IAudioClient** out, DevFmt& fmt, UINT32& frames, UINT32& period, bool event, int wantCh) {
    static const int cand[][3] = {{32, 32, 1}, {32, 32, 0}, {32, 24, 0}, {24, 24, 0}, {16, 16, 0}};
    int chans[3] = {wantCh, 2, 1}; int list[3]; int nch = 0;
    for (int c : chans) { bool dup = false; for (int j = 0; j < nch; ++j) if (list[j] == c) dup = true; if (!dup) list[nch++] = c; }
    IAudioClient* c = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) return false;
    REFERENCE_TIME defP = 0, minP = 0;
    c->GetDevicePeriod(&defP, &minP);
    REFERENCE_TIME want = (REFERENCE_TIME)(10000000.0 * bufSize / rate + 0.5);
    if (want < minP) want = minP;
    REFERENCE_TIME dur = want;
    if (!event) {
        REFERENCE_TIME o4 = (REFERENCE_TIME)(4.0 * 10000000.0 * outPeriod / rate);
        dur = want * 4 > o4 ? want * 4 : o4;
    }
    for (int ci = 0; ci < nch; ++ci) for (auto& k : cand) {
        int ch = list[ci];
        WAVEFORMATEXTENSIBLE f = MakeFmt(rate, ch, k[0], k[1], k[2] != 0);
        if (c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.Format, nullptr) != S_OK) continue;
        DWORD flags = event ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
        REFERENCE_TIME p = want, d = dur;
        HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, d, p, &f.Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 n = 0; c->GetBufferSize(&n);
            c->Release(); c = nullptr;
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) return false;
            REFERENCE_TIME nal = (REFERENCE_TIME)(10000000.0 * n / rate + 0.5);   // aligned duration
            if (event) p = d = nal;          // event mode: period = aligned buffer
            else d = nal;                    // polled: align the buffer (4x); periodicity stays at want
            Log("  Exclusive: period not aligned, retrying with %u frames", n);
            hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, d, p, &f.Format, nullptr);
        }
        if (SUCCEEDED(hr)) {
            c->GetBufferSize(&frames);
            period = RefTimeFrames(rate, (double)p);
            fmt = ParseFmt(&f.Format);
            *out = c;
            char b[32]; FmtStr(fmt, b, sizeof b);
            Log("  Exclusive OK: %s, buffer %u frames, period %u frames (%.2f ms), device min period %.2f ms",
                b, frames, period, 1000.0 * period / rate, minP / 10000.0);
            return true;
        }
        Log("  Exclusive Initialize failed hr=0x%08lX (%dch %d/%d %s)", (unsigned long)hr, ch, k[0], k[1], k[2] ? "float" : "int");
        WB_TRACE(wbtrace::kLvlVerbose, wbtrace::kKwStream, "ExclusiveFormatRefused", wbtrace::HR("HResult", (long)hr), wbtrace::I32("Channels", ch),
                 wbtrace::I32("ContainerBits", k[0]), wbtrace::I32("ValidBits", k[1]), wbtrace::Bool("Float", k[2] != 0), wbtrace::F64("Rate", rate));
        lastExclHr = hr;
        c->Release(); c = nullptr;
        if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) return false;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) return false;
    }
    SafeRelease(c);
    Log("  Exclusive: no format accepted at %.0f Hz", rate);
    return false;
}

// Host thread. Opens the main output: Exclusive (if Mode=Exclusive and accepted), else Shared. 0.9.4: with
// LowLatencyShared=1 the Shared stream always runs at the device's mix format and rate, so the low-latency
// engine (IAudioClient3) is used whatever the ASIO rate; the fallback chain is IAudioClient3 at the minimum
// period -> IAudioClient3 at the default period -> standard Shared at the mix rate -> standard Shared at the ASIO
// rate with AUTOCONVERTPCM (Windows converts; the only path before 0.9.4 when the rates differed). When the ASIO
// rate differs from the device rate, the driver converts with a fixed exact ratio (outConv, fixedratio.h).
// Sets up the event, render client, IAudioClock (Exclusive glitch detection) and the volume callback. Returns
// false (with setError) if the output cannot be opened; the driver cannot run without it.
bool WasapiBridge::openOutput() {
    HRESULT hr;
    outExclusive = outLowLatency = outConvOn = false;
    outStreamLat = 0;
    outDevRate = rate;
    engineKind = 0;
    if (cfg.exclusive) {
        Log("Output: trying Exclusive");
        UINT32 per = 0;
        if (initExclusive(outDev, &outClient, outFmt, outBufFrames, per, true, outDevCh)) { outExclusive = true; outPeriod = outBufFrames; engineKind = 3; }
        else Log("Output: Exclusive not available -> Shared");
    }
    if (!outExclusive) {
        auto activate = [&]() -> bool {
            SafeRelease(outClient);
            return SUCCEEDED(outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&outClient));
        };
        if (!activate()) { setError("Activate(IAudioClient) for output failed"); return false; }
        WAVEFORMATEX* mix = nullptr;
        outClient->GetMixFormat(&mix);
        mixRateOpen = mix ? (double)mix->nSamplesPerSec : 0;      // G4: compared with later format changes
        DWORD mask = 0xFFFFFFFF;
        if (mix && mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->nChannels == outDevCh) mask = ((WAVEFORMATEXTENSIBLE*)mix)->dwChannelMask;
        bool atMix = false;                                       // stream opened at the mix format / rate
        if (cfg.lowLatencyShared && mix) {
            const double mr = (double)mix->nSamplesPerSec;
            const bool needConv = fabs(mr - rate) >= 0.5;
            // Every ASIO output channel is converted: OutputMap may route a channel of an extra device to the main
            // output (review fix 2). Quality policy (fixedratio.h Choose): sinc whenever the CPU budget allows, in
            // both directions; above the budget Hermite (rate up) or Windows converts (rate down) (review fixes 4
            // and 7). 0.9.6 (KI-5): independent of ResamplerQuality (that key only selects the drift resampler).
            const int convCh = nOut > 0 ? (int)nOut : 1;
            fr::Quality q = fr::Quality::Sinc;
            if (needConv) {
                q = fr::Choose(rate, mr, convCh, convBudgetLeft);   // R2: the output comes first (full budget)
                char why[128]; fr::Describe(why, sizeof why, q, rate, mr, convCh, convBudgetLeft);
                Log("Output: rate conversion %.0f -> %.0f Hz, %d channels: %s", rate, mr, convCh, why);
            }
            const bool convOk = !needConv || q != fr::Quality::None;
            const UINT32 blk = (UINT32)ceil((double)bufSize * mr / rate);   // one ASIO block in device frames
            UINT32 defF = 0, fundF = 0, minF = 0, maxF = 0;
            bool hasC3 = false;
            if (convOk) {
                IAudioClient3* c3 = nullptr;
                if (SUCCEEDED(outClient->QueryInterface(__uuidof(IAudioClient3), (void**)&c3))) {
                    hasC3 = SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &defF, &fundF, &minF, &maxF)) && minF > 0;
                    c3->Release();
                }
            }
            // One IAudioClient3 attempt on a fresh client. The buffer must hold one period plus one ASIO block
            // (review fix 6), otherwise a long block could not be absorbed.
            auto tryC3 = [&](UINT32 per) -> bool {
                if (!activate()) return false;
                IAudioClient3* c3 = nullptr;
                if (FAILED(outClient->QueryInterface(__uuidof(IAudioClient3), (void**)&c3))) return false;
                hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, mix, nullptr);
                c3->Release();
                if (FAILED(hr)) { Log("Output: InitializeSharedAudioStream(%u) failed hr=0x%08lX", per, (unsigned long)hr); return false; }
                UINT32 bf = 0; outClient->GetBufferSize(&bf);
                if (per + blk > bf) { Log("Output: IAudioClient3 period %u: buffer %u frames cannot hold one period + one ASIO block (%u)", per, bf, blk); return false; }
                atMix = outLowLatency = true; outPeriod = per; engineKind = 1;
                Log("Output: low-latency Shared (IAudioClient3) period %u frames (def %u, fund %u, min %u, max %u), buffer %u",
                    per, defF, fundF, minF, maxF, bf);
                return true;
            };
            if (hasC3) {
                // 1) the minimum period, then the next multiples of the fundamental below the default (a larger
                // buffer for long ASIO blocks), 2) the default period
                const UINT32 step = fundF ? fundF : minF;
                int tries = 0;
                for (UINT32 per = minF; per < defF && per <= maxF && tries < 16 && !atMix; per += step, ++tries) tryC3(per);
                if (!atMix && defF) tryC3(defF);
            }
            if (convOk && !atMix && activate()) {                 // 3) standard Shared at the mix format, own conversion
                hr = outClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
                if (SUCCEEDED(hr)) {
                    REFERENCE_TIME defP = 0, minP = 0; outClient->GetDevicePeriod(&defP, &minP);
                    atMix = true; engineKind = 2;
                    outPeriod = RefTimeFrames(mr, (double)defP);
                    Log("Output: standard Shared at the mix rate %.0f Hz", mr);
                } else Log("Output: Initialize Shared at the mix format failed hr=0x%08lX", (unsigned long)hr);
            }
            if (atMix && !DevFmtUsable(ParseFmt(mix))) {         // 0.9.8: untrusted device format
                Log("Output: the device mix format is not usable (%u ch, block %u, %u bits, tag 0x%04X); Windows converts instead",
                    (unsigned)mix->nChannels, (unsigned)mix->nBlockAlign, (unsigned)mix->wBitsPerSample, (unsigned)mix->wFormatTag);
                atMix = outLowLatency = false; engineKind = 0;
            }
            if (atMix) {
                outFmt = ParseFmt(mix);
                outDevRate = mr;
                if (needConv) {
                    outClient->GetBufferSize(&outBufFrames);
                    outConvOn = outConv.configure(rate, mr, convCh, (size_t)bufSize + 64, outBufFrames + 64, q == fr::Quality::Sinc);
                    if (!outConvOn) { atMix = outLowLatency = false; engineKind = 0; outDevRate = rate; Log("Output: converter not available"); }
                    else {                                // 0.9.6 (R2): its cost is no longer available to the inputs
                        convBudgetLeft = std::max(0.0, convBudgetLeft - fr::Cost(q, rate, mr, convCh));   // Hermite may exceed it
                        Log("Output: converter CPU budget left for inputs %.0f M MAC/s", convBudgetLeft / 1e6);
                    }
                }
            }
            if (!atMix && !activate()) { CoTaskMemFree(mix); setError("Activate for output failed"); return false; }
        }
        if (mix) CoTaskMemFree(mix);
        if (!atMix) {                                             // 4) Windows converts (AUTOCONVERTPCM at the ASIO rate)
            WAVEFORMATEXTENSIBLE f = MakeFmt(rate, outDevCh, 32, 32, true, mask);
            hr = outClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                0, 0, &f.Format, nullptr);
            if (FAILED(hr)) {
                char m[96]; snprintf(m, sizeof m, "Initialize Shared for output failed hr=0x%08lX", (unsigned long)hr);
                lastHr = (long)hr;
                setError(m); return false;
            }
            outFmt = ParseFmt(&f.Format);
            REFERENCE_TIME defP = 0, minP = 0; outClient->GetDevicePeriod(&defP, &minP);
            outPeriod = RefTimeFrames(rate, (double)defP);
            Log("Output: standard Shared (AUTOCONVERTPCM) at %.0f Hz", rate);
        }
        outClient->GetBufferSize(&outBufFrames);
    }
    // 0.9.8 (untrusted device data): every FIFO and converter is sized from the device buffer; a device reporting 0
    // or an absurd size (more than 2^20 frames, about 22 s at 48 kHz) is refused instead of overflowing the size
    // arithmetic on 32-bit builds.
    if (outBufFrames == 0 || outBufFrames > (1u << 20) || outPeriod > outBufFrames * 4u) {
        char m[96]; snprintf(m, sizeof m, "Output device reported an invalid buffer (%u frames, period %u)", outBufFrames, outPeriod);
        setError(m); return false;
    }
    // 0.9.4: frames kept queued in a low-latency stream: two periods, or one period plus one ASIO block (device
    // frames) when the block is longer, so a long host callback never empties the device buffer; never more than
    // the buffer. Other streams keep the whole buffer filled, as before.
    outFill = outLowLatency ? sched::FillTarget(outPeriod, outBufFrames, bufSize, rate, outDevRate) : outBufFrames;
    outPeriodAsio = (UINT32)lround((double)outPeriod * rate / outDevRate);
    outFmt.clamp = outExclusive;      // the Shared engine clips; in Exclusive the driver does it
    // TPDF dither: only Exclusive with integer output of 16 or 24 valid bits (Shared: the engine is float)
    // 0.7.0: ditherActive = "the format allows TPDF dither"; the Dither key itself is a live parameter (lv.dither).
    ditherActive = outExclusive && !outFmt.isFloat && outFmt.validBits <= 24;
    if (outExclusive && !outFmt.isFloat) Log("Output: integer %d valid bits, TPDF dither=%d", outFmt.validBits, (int)(ditherActive && cfg.dither));
    REFERENCE_TIME lat = 0;
    if (SUCCEEDED(outClient->GetStreamLatency(&lat))) outStreamLat = RefTimeFrames(outDevRate, (double)lat);   // device frames
    if (FAILED(outClient->SetEventHandle(outEvent))) { setError("SetEventHandle failed"); return false; }
    if (FAILED(outClient->GetService(__uuidof(IAudioRenderClient), (void**)&render))) { setError("GetService render failed"); return false; }
    outClockFreq = 0;
    if (outExclusive) {               // Exclusive glitch detection: device position vs frames written
        if (SUCCEEDED(outClient->GetService(__uuidof(IAudioClock), (void**)&outClock)) &&
            (FAILED(outClock->GetFrequency(&outClockFreq)) || outClockFreq == 0)) SafeRelease(outClock);
        if (!outClock) outClockFreq = 0;
        Log("Output: IAudioClock %s (frequency %llu)", outClock ? "available" : "not available", (unsigned long long)outClockFreq);
    }
    char b[32]; FmtStr(outFmt, b, sizeof b);
    Log("Output open: %s, buffer %u frames (%.2f ms), period %u, kept queued %u, stream latency %u frames", b, outBufFrames,
        1000.0 * outBufFrames / outDevRate, outPeriod, outFill, outStreamLat);
    // 0.9.4: engine line (English: log, Copy status, test tool; the panel shows a translated version)
    {
        static const char* kinds[4] = {"Shared (Windows converts the rate)", "Low-latency", "Shared at the mix rate", "Exclusive"};
        char conv[96];
        if (outConvOn) snprintf(conv, sizeof conv, "internal rate conversion %.0f->%.0f Hz (%s, %d taps)", rate, outDevRate,
                                outConv.sinc() ? "sinc" : "Hermite", outConv.taps());
        else snprintf(conv, sizeof conv, engineKind == 0 && !outExclusive && fabs(mixRateOpen - rate) >= 0.5 ? "Windows converts %.0f->%.0f Hz" : "no conversion",
                      rate, mixRateOpen);
        snprintf(engineLine, sizeof engineLine, "%s engine period %u frames (%.2f ms) at %.0f Hz; %s", kinds[engineKind], outPeriod,
                 1000.0 * outPeriod / outDevRate, outDevRate, conv);
        Log("Engine: %s", engineLine);
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwStream, "OutputOpen", wbtrace::I32("Engine", engineKind),
                 wbtrace::Bool("Exclusive", outExclusive), wbtrace::Bool("LowLatency", outLowLatency), wbtrace::F64("AsioRate", rate),
                 wbtrace::F64("DeviceRate", outDevRate), wbtrace::F64("MixRate", mixRateOpen), wbtrace::U32("PeriodFrames", outPeriod),
                 wbtrace::U32("BufferFrames", outBufFrames), wbtrace::U32("QueuedFrames", outFill), wbtrace::U32("StreamLatencyFrames", outStreamLat),
                 wbtrace::Str("Format", b), wbtrace::Bool("Converting", outConvOn), wbtrace::Bool("Sinc", outConvOn && outConv.sinc()),
                 wbtrace::I32("BufferSize", (int32_t)bufSize), wbtrace::Str("EngineLine", engineLine));
    }

    softVolume = false;
    IAudioEndpointVolume* v = nullptr;
    if (SUCCEEDED(outDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&v))) {
        DWORD hw = 0; v->QueryHardwareSupport(&hw);
        softVolume = outExclusive && !(hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME);
        AcquireSRWLockExclusive(&volLock); epVol = v; ReleaseSRWLockExclusive(&volLock);
        volCb = new VolumeCallback(this);
        v->RegisterControlChangeNotify(volCb);
        onVolumeNotify();
        Log("Volume: hardware=%d -> driver software volume=%d", (int)((hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0), (int)softVolume);
    }
    return true;
}

// Host thread. Opens the main input and up to three extra inputs. Missing devices are logged and their ASIO
// channels stay silent; the driver keeps running.
void WasapiBridge::openDeviceInput() {
    srcIn.tag = "Input"; srcIn.asioFirst = 0; srcIn.asioCount = (int)nInDev;
    if (inDev && nInDev > 0) openInput(srcIn, inDev, inDevCh, inContainer, cfg.exclusive);
    static const char* tags[kMaxExtra] = {"Input 2", "Input 3", "Input 4"};
    for (int k = 0; k < kMaxExtra; ++k) {          // 0.5.0: extra inputs (Shared; drift per ContainerId)
        InputSource& x = srcEx[k];
        x.tag = tags[k];
        x.asioFirst = k < nInEx ? inEx[k].asioFirst : 0; x.asioCount = k < nInEx ? inEx[k].ch : 0;
        if (k < nInEx && inEx[k].dev) openInput(x, inEx[k].dev, inEx[k].mixCh, inEx[k].container, false);
        else if (k < nInEx) Log("%s: device not found, channels are silent", x.tag);
    }
}

// Host thread. Opens a device capture source (main or extra); s.tag/asioFirst/asioCount
// are already set. tryExclusive only for the main input with Mode=Exclusive.
void WasapiBridge::openInput(InputSource& s, IMMDevice* dev, int devCh, const GUID& container, bool tryExclusive) {
    HRESULT hr;
    s.exclusive = false;
    UINT32 capPeriod = 0;
    if (tryExclusive) {
        Log("%s: trying Exclusive", s.tag);
        lastExclHr = S_OK;
        if (initExclusive(dev, &s.client, s.fmt, s.bufFrames, capPeriod, false, devCh)) s.exclusive = true;
        else { noteCaptureHr(lastExclHr, s.tag); Log("%s: Exclusive not available -> Shared", s.tag); }
    }
    s.convOn = s.alignedIn = false; s.devRate = rate; s.inDelay = 0; s.qOff = 0;
    bool atMix = false;                       // 0.9.4: opened at the device's mix format and rate
    fr::Quality inQ = fr::Quality::Sinc;      // 0.9.4: converter quality chosen before opening (fixedratio.h Choose)
    // Windows converts (AUTOCONVERTPCM at the ASIO rate): the 0.9.3 path, also the fallback of a failed conversion
    auto openAuto = [&]() -> bool {
        WAVEFORMATEXTENSIBLE f = MakeFmt(rate, devCh, 32, 32, true);
        hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  200000, 0, &f.Format, nullptr);
        if (FAILED(hr)) { Log("%s: Initialize Shared failed hr=0x%08lX, continuing without this input", s.tag, (unsigned long)hr); noteCaptureHr(hr, s.tag); SafeRelease(s.client); return false; }
        s.fmt = ParseFmt(&f.Format);
        s.client->GetBufferSize(&s.bufFrames);
        REFERENCE_TIME defP = 0, minP = 0; s.client->GetDevicePeriod(&defP, &minP);
        capPeriod = RefTimeFrames(rate, (double)defP);
        s.devRate = rate;
        return true;
    };
    if (!s.exclusive) {
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.client);
        if (FAILED(hr)) { Log("%s: Activate failed hr=0x%08lX", s.tag, (unsigned long)hr); noteCaptureHr(hr, s.tag); return; }
        WAVEFORMATEX* mix = nullptr; s.client->GetMixFormat(&mix);
        const double mr = mix ? (double)mix->nSamplesPerSec : 0;
        // 0.9.4: with LowLatencyShared the input opens at its mix rate whatever the ASIO rate (Exclusive mode keeps
        // the 0.9.3 rule: only when the rates match); the driver converts with an exact ratio.
        const bool needConv = mix && fabs(mr - rate) >= 0.5;
        // Review fixes 4, 5, 7: the quality is chosen with the real channel count before opening (sinc whenever the
        // CPU budget allows; over the budget Hermite up / Windows converts down). 0.9.6 (KI-5): independent of
        // ResamplerQuality, like the output.
        if (needConv && cfg.lowLatencyShared && !cfg.exclusive) {
            const int convCh = s.asioCount > 0 ? s.asioCount : 1;
            inQ = fr::Choose(mr, rate, convCh, convBudgetLeft);     // R2: what the output and earlier inputs left
            char why[128]; fr::Describe(why, sizeof why, inQ, mr, rate, convCh, convBudgetLeft);
            Log("%s: rate conversion %.0f -> %.0f Hz, %d channels: %s", s.tag, mr, rate, convCh, why);
        }
        const bool mixOk = cfg.lowLatencyShared && mix && (!needConv || (!cfg.exclusive && inQ != fr::Quality::None));
        if (mixOk) {
            // output period in input-device frames (the capture period should not be much shorter: one packet per wake)
            const double outPerIn = (double)outPeriod * mr / outDevRate;
            auto reactivate = [&]() -> bool {
                SafeRelease(s.client);
                return SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.client));
            };
            IAudioClient3* c3 = nullptr;
            if (SUCCEEDED(s.client->QueryInterface(__uuidof(IAudioClient3), (void**)&c3))) {
                UINT32 defF = 0, fundF = 0, minF = 0, maxF = 0;
                if (SUCCEEDED(c3->GetSharedModeEnginePeriod(mix, &defF, &fundF, &minF, &maxF))) {
                    UINT32 per = minF;
                    while (per < maxF && per * 3.0 < outPerIn * 2.0) per += fundF ? fundF : minF;
                    if (SUCCEEDED(c3->InitializeSharedAudioStream(0, per, mix, nullptr))) {
                        UINT32 bf = 0; s.client->GetBufferSize(&bf);
                        if (bf >= outPerIn * 2.0) { atMix = true; capPeriod = per; s.bufFrames = bf;
                            Log("%s: low-latency Shared (IAudioClient3) period %u frames, buffer %u, min period %u", s.tag, per, bf, minF); }
                        else Log("%s: IAudioClient3 buffer %u < 2x output period, using standard Shared", s.tag, bf);
                    }
                }
                c3->Release();
            }
            if (!atMix && needConv) {                 // standard Shared at the mix format (own conversion)
                if (!reactivate()) { CoTaskMemFree(mix); return; }
                if (SUCCEEDED(s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix, nullptr))) {
                    atMix = true;
                    s.client->GetBufferSize(&s.bufFrames);
                    REFERENCE_TIME defP = 0, minP = 0; s.client->GetDevicePeriod(&defP, &minP);
                    capPeriod = RefTimeFrames(mr, (double)defP);
                    Log("%s: standard Shared at the mix rate %.0f Hz", s.tag, mr);
                }
            }
            if (atMix && !DevFmtUsable(ParseFmt(mix))) {         // 0.9.8: untrusted device format
                Log("%s: the device mix format is not usable; Windows converts instead", s.tag);
                atMix = false;
            }
            if (atMix) { s.fmt = ParseFmt(mix); s.devRate = mr; }
            else if (!reactivate()) { CoTaskMemFree(mix); return; }
        }
        if (mix) CoTaskMemFree(mix);
        if (!atMix && !openAuto()) return;
    }
    if (FAILED(s.client->GetService(__uuidof(IAudioCaptureClient), (void**)&s.cap))) { Log("%s: GetService failed", s.tag); SafeRelease(s.client); return; }
    s.period   = capPeriod;
    s.target   = (UINT32)lround(capPeriod * rate / s.devRate) + outPeriodAsio;   // ASIO frames
    // Auto: correct drift only when input and output are on different physical hardware (different or unknown ContainerId)
    // {00000000-0000-0000-FFFF-FFFFFFFFFFFF} = ContainerId shared by all built-in devices (does not imply the same clock)
    static const GUID zero = {};
    static const GUID builtin = {0x00000000, 0x0000, 0x0000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    bool known = !IsEqualGUID(outContainer, zero) && !IsEqualGUID(outContainer, builtin);
    bool sameBox = known && IsEqualGUID(outContainer, container);
    s.resample = cfg.drift == DriftMode::On || (cfg.drift == DriftMode::Auto && !sameBox);
    if (atMix && fabs(s.devRate - rate) >= 0.5) {
        // 0.9.4: device rate -> ASIO rate before the FIFO. On the output's clock the FIFO is aligned to the
        // callbacks with a fixed delay (exact round trip); on another clock the drift PI follows as before.
        const int ch = s.asioCount > 0 ? s.asioCount : 1;
        const size_t maxOut = (size_t)ceil((double)(s.bufFrames + 64) * rate / s.devRate) + 64;
        s.convOn = s.conv.configure(s.devRate, rate, ch, (size_t)s.bufFrames + 64, maxOut, inQ == fr::Quality::Sinc);
        if (s.convOn) convBudgetLeft = std::max(0.0, convBudgetLeft - fr::Cost(inQ, s.devRate, rate, ch));   // 0.9.6 (R2)
        if (!s.convOn) {                          // chosen before opening: not expected. Review fix 5: let Windows convert
            Log("%s: converter not available, reopening with Windows conversion", s.tag);
            SafeRelease(s.cap); SafeRelease(s.client);
            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.client)) || !openAuto() ||
                FAILED(s.client->GetService(__uuidof(IAudioCaptureClient), (void**)&s.cap))) {
                Log("%s: reopening failed, channels are silent", s.tag);
                SafeRelease(s.cap); SafeRelease(s.client); s.bufFrames = s.target = s.period = 0; return;
            }
            s.period = capPeriod;
            s.target = capPeriod + outPeriodAsio;
            char b[32]; FmtStr(s.fmt, b, sizeof b);
            Log("%s open (Shared, Windows converts): %s, buffer %u, period %u, FIFO target %u, drift correction=%d",
                s.tag, b, s.bufFrames, capPeriod, s.target, (int)s.resample);
            return;
        }
        const UINT32 lookA = (UINT32)ceil((s.conv.ahead() + 1.0) * rate / s.devRate);
        s.alignedIn = s.convOn && !s.resample && !outExclusive;
        if (s.alignedIn) {
            const int aheadOut = outConvOn ? outConv.ahead() : 0;
            s.inDelay = sched::InDelay(bufSize, aheadOut, rate, s.devRate, s.conv.ahead(), capPeriod, outDevRate, outPeriod);
            const sched::Resync rs = sched::InResync(bufSize, rate, s.devRate, capPeriod, outDevRate, outPeriod);
            s.exMid = rs.mid; s.exHi = rs.hi;
            s.target = (UINT32)sched::InLatency(s.inDelay, bufSize, aheadOut);
        } else s.target += lookA;
        Log("%s: internal rate conversion %.0f->%.0f Hz (%s, %d taps)%s, delivery delay %u frames", s.tag, s.devRate, rate,
            s.conv.sinc() ? "sinc" : "Hermite", s.conv.taps(), s.alignedIn ? ", aligned to the output clock" : ", drift-corrected", s.inDelay);
    }
    char b[32]; FmtStr(s.fmt, b, sizeof b);
    Log("%s open (%s): %s, buffer %u, period %u, FIFO target %u, same hardware as output=%d, drift correction=%d",
        s.tag, s.exclusive ? "Exclusive" : "Shared", b, s.bufFrames, capPeriod, s.target, (int)sameBox, (int)s.resample);
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwStream, "InputOpen", wbtrace::Str("Source", s.tag), wbtrace::Bool("Exclusive", s.exclusive),
             wbtrace::Str("Format", b), wbtrace::F64("DeviceRate", s.devRate), wbtrace::U32("BufferFrames", s.bufFrames),
             wbtrace::U32("PeriodFrames", capPeriod), wbtrace::U32("TargetFrames", s.target), wbtrace::Bool("SameHardware", sameBox),
             wbtrace::Bool("DriftCorrection", s.resample), wbtrace::Bool("Converting", s.convOn), wbtrace::Bool("AlignedToOutput", s.alignedIn));
}

// Host thread. Opens the Loopback source (last two ASIO inputs). All: classic loopback of the output endpoint.
// ExcludeDaw/App: process loopback activated through ActivateAudioInterfaceAsync (see ActivateProcessLoopback).
// Polled, so it never shares the output's event. Failures leave the Loopback channels silent.
void WasapiBridge::openLoopback() {
    InputSource& s = srcLb;
    s.tag = "Loopback"; s.asioFirst = (int)lbFirst; s.asioCount = 2;
    if (cfg.loopback == LoopbackMode::Off) return;
    HRESULT hr = E_FAIL;
    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const char* lbName = cfg.loopback == LoopbackMode::ExcludeDaw ? "excluding DAW" : cfg.loopback == LoopbackMode::App ? "application" : "all";
    if (cfg.loopback == LoopbackMode::ExcludeDaw || cfg.loopback == LoopbackMode::App) {
        // Process loopback (Windows 10 2004+). ExcludeDaw: everything except the DAW's process tree.
        // App: only the chosen application's process tree; the PID is resolved on every open (it changes
        // when the application restarts). If it is not running, the Loopback channels stay silent.
        DWORD pid = GetCurrentProcessId();
        WB_PROCESS_LOOPBACK_MODE mode = WB_LB_EXCLUDE_TREE;
        if (cfg.loopback == LoopbackMode::App) {
            const char* why = "";
            pid = FindProcessByExe(enumr, cfg.loopbackApp, why);
            mode = WB_LB_INCLUDE_TREE;
            if (!pid) { Log("Loopback (application): '%s' %s; Loopback channels are silent", Utf8(cfg.loopbackApp).c_str(), why); return; }
            Log("Loopback (application): %s -> PID %lu (%s)", Utf8(cfg.loopbackApp).c_str(), pid, why);
        }
        static const int cand[][3] = {{32, 32, 1}, {16, 16, 0}};
        flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;   // required by process loopback; the event is not used (polled)
        for (int i = 0; i < 2 && !s.client; ++i) {
            HRESULT ah;
            IAudioClient* c = ActivateProcessLoopback(ah, pid, mode);
            if (!c) { Log("Loopback (%s): activation failed hr=0x%08lX", lbName, (unsigned long)ah); noteCaptureHr(ah, "Loopback"); return; }
            WAVEFORMATEXTENSIBLE f = MakeFmt(rate, 2, cand[i][0], cand[i][1], cand[i][2] != 0);
            hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &f.Format, nullptr);
            if (SUCCEEDED(hr)) { s.client = c; s.fmt = ParseFmt(&f.Format); }
            else { Log("Loopback (%s): Initialize %s failed hr=0x%08lX", lbName, cand[i][2] ? "float" : "int16", (unsigned long)hr); noteCaptureHr(hr, "Loopback"); c->Release(); }
        }
        if (!s.client) return;
        static HANDLE dummy = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s.client->SetEventHandle(dummy);
        s.resample = cfg.drift != DriftMode::Off;
    } else {
        if (outExclusive) Log("Loopback (all): the output is Exclusive; loopback of the same device will be silent");
        if (FAILED(outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.client))) return;
        WAVEFORMATEXTENSIBLE f = MakeFmt(rate, 2, 32, 32, true);
        hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &f.Format, nullptr);
        if (FAILED(hr)) { Log("Loopback (all): Initialize failed hr=0x%08lX", (unsigned long)hr); noteCaptureHr(hr, "Loopback"); SafeRelease(s.client); return; }
        s.fmt = ParseFmt(&f.Format);
        s.resample = cfg.drift == DriftMode::On;    // same clock as the output
    }
    if (FAILED(s.client->GetService(__uuidof(IAudioCaptureClient), (void**)&s.cap))) { Log("Loopback: GetService failed"); SafeRelease(s.client); return; }
    s.client->GetBufferSize(&s.bufFrames);
    s.period = (UINT32)(rate * 0.010);
    s.target = s.period + outPeriodAsio;
    Log("Loopback open (%s): buffer %u frames, target %u, drift correction=%d", lbName, s.bufFrames, s.target, (int)s.resample);
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwStream, "LoopbackOpen", wbtrace::Str("Mode", lbName), wbtrace::U32("BufferFrames", s.bufFrames),
             wbtrace::U32("TargetFrames", s.target), wbtrace::Bool("DriftCorrection", s.resample));
}

// Host thread. E_ACCESSDENIED when opening a capture stream means Windows privacy settings (microphone) block
// desktop apps. The driver continues without that input and the panel shows a hint (StatusInfo::captureDenied).
// hr: the failing HRESULT; tag: the source name for the log.
void WasapiBridge::noteCaptureHr(HRESULT hr, const char* tag) {
    WB_TRACE(wbtrace::kLvlWarning, wbtrace::kKwError | wbtrace::kKwStream, "CaptureOpenFailed", wbtrace::Str("Source", tag),
             wbtrace::HR("HResult", (long)hr), wbtrace::Bool("PrivacyBlocked", hr == E_ACCESSDENIED));
    if (hr != E_ACCESSDENIED) return;
    if (!captureDenied)
        Log("%s: Windows blocked access to the input for privacy reasons (Settings > Privacy & security > Microphone > "
            "Let desktop apps access your microphone). Set Input = (None) if you do not need inputs.", tag);
    captureDenied = true;
}

// Host thread. Opens every stream (output, inputs, loopback, extra sinks), sets up resamplers and FIFO
// targets, computes alignment and routing, and preallocates every buffer the audio thread will use.
// After this returns, the audio thread needs no allocation. Returns false if the main output failed.
bool WasapiBridge::openStreams() {
    if (streamsOpen) return true;
    captureDenied = false;
    convBudgetLeft = fr::kMacBudget;          // 0.9.6 (R2): one CPU budget shared by all converters of this open
    if (!openOutput()) { closeStreams(); return false; }
    openDeviceInput();
    openLoopback();
    for (int k = 0; k < kMaxExtra; ++k) openSink(k);
    // 0.9.8 (untrusted device data): a capture source or extra output whose device reported a buffer of more than
    // 2^20 frames (or a period beyond it) is closed: its channels stay silent, like a missing device, and the FIFO
    // size arithmetic below can never overflow on a 32-bit build.
    {
        InputSource* all[2 + kMaxExtra] = {&srcIn, &srcLb, &srcEx[0], &srcEx[1], &srcEx[2]};
        for (InputSource* x : all)
            if (x->client && (x->bufFrames > (1u << 20) || x->period > (1u << 20) || x->target > (1u << 21))) {
                Log("%s: the device reported an invalid buffer (%u frames, period %u): closed, channels are silent", x->tag, x->bufFrames, x->period);
                closeSource(*x);
            }
        for (OutputSink& o : sinks)
            if (o.client && (o.bufFrames == 0 || o.bufFrames > (1u << 20) || o.period > (1u << 20))) {
                Log("%s: the device reported an invalid buffer (%u frames, period %u): closed, channels are silent", o.tag, o.bufFrames, o.period);
                closeSink(o);
            }
    }
    // Sinc resampling (0.4.0): the table is built once (it allocates: only here, on the host thread).
    // The kSincHalf-frame look-ahead is added to the FIFO target (and therefore to the input latency).
    InputSource* srcs[2 + kMaxExtra] = {&srcIn, &srcLb, &srcEx[0], &srcEx[1], &srcEx[2]};
    for (InputSource* x : srcs) {
        x->sinc = x->cap && x->resample && cfg.resampler == ResamplerQ::Sinc;
        x->posBase = x->sinc ? (double)(kSincHalf - 1) : 1.0;
        x->pos = x->posBase;
        if (x->sinc) {
            if (sincTab.empty()) BuildSincTable(sincTab);
            x->target += kSincHalf;
            Log("%s: sinc resampling %d taps x %d phases, FIFO target %u", x->tag, kSincTaps, kSincPhases, x->target);
        }
    }
    for (int k = 0; k < kMaxExtra; ++k) {
        OutputSink& o = sinks[k];
        o.sinc = o.client && cfg.resampler == ResamplerQ::Sinc;
        if (o.sinc && sincTab.empty()) BuildSincTable(sincTab);
        o.posBase = o.sinc ? (double)(kSincHalf - 1) : 1.0;
        o.pos = o.posBase;
        o.sp = SinkSetpoint(bufSize, outPeriodAsio, o.sinc ? kSincHalf : 0);
    }
    computeAlignment();                    // may raise the sinks' sp and the inputs' target
    // 0.9.4: source of direct monitoring. writeOut runs at the output device's rate: capture packets (1) when they
    // are at that rate and on its clock, the delivered blocks (2) when the output is not converted and the input
    // is drift-corrected or converted; otherwise (converted output, input at another rate or clock) none.
    if (!srcIn.cap) monPath = 0;
    else if (!outConvOn) monPath = (srcIn.resample || srcIn.convOn) ? 2 : 1;
    else monPath = (srcIn.convOn && !srcIn.resample && fabs(srcIn.devRate - outDevRate) < 0.5) ? 1 : 0;
    if (srcIn.cap) {
        monKeep = monPath == 2 ? (srcIn.alignedIn ? outPeriodAsio * 2 : srcIn.target) + (UINT32)bufSize
                : srcIn.convOn ? srcIn.period + outPeriod : srcIn.target;
        if (!monPath) Log("Direct monitoring: not available with this rate combination (the input is not at the output device's rate)");
        // review fix 3: the host learns kAsioCanInputMonitor again when the availability changed since the last
        // open (posted: the reset is requested from the message window, never from inside createBuffers)
        const int avail = monPath != 0 ? 1 : 0;
        if (monAvailLast >= 0 && monAvailLast != avail) {
            Log("Direct monitoring: availability changed (%d -> %d), asking the host to reset", monAvailLast, avail);
            monResetPending = true;
            if (msgWnd) PostMessageW(msgWnd, WM_WB_RESET, 0, 0);
        }
        monAvailLast = avail;
    }
    size_t cap = (size_t)bufSize * 6 + outBufFrames * 2 + 16384;
    outFifo.init(cap, (int)nOut);
    srcIn.fifo.init(cap + srcIn.bufFrames * 2 + srcIn.target * 2 + srcIn.inDelay, srcIn.asioCount > 0 ? srcIn.asioCount : 1);
    srcLb.fifo.init(cap + srcLb.bufFrames * 2, 2);
    for (int k = 0; k < kMaxExtra; ++k)
        srcEx[k].fifo.init(cap + srcEx[k].bufFrames * 2 + srcEx[k].target * 2 + srcEx[k].inDelay, srcEx[k].asioCount > 0 ? srcEx[k].asioCount : 1);
    for (int k = 0; k < kMaxExtra; ++k) {
        OutputSink& o = sinks[k];
        o.overLimit = o.sp * 2 + (double)bufSize * 4 + o.targetPad;
        o.fifo.init((size_t)(o.overLimit * 2) + (size_t)bufSize * 8 + 16384, o.asioCount > 0 ? o.asioCount : 1);
    }
    primDelay.init((size_t)primDelayFrames + outBufFrames + 16, outFmt.channels > 0 ? outFmt.channels : 1);
    monFifo.init(cap, nInDev > 0 ? (int)nInDev : 1);
    monScratch.assign((size_t)bufSize * kMaxCh, 0.f);   // F5: resampled block for the monitor (A uses it, H owns it)
    buildRoute();
    streamsOpen = true;
    return true;
}

// Host thread. Releases an input source's COM objects (leaked on purpose if the audio thread is stuck).
void WasapiBridge::closeSource(InputSource& s) {
    if (threadStuck) { s.cap = nullptr; s.client = nullptr; }   // leak: the stuck thread still holds references
    SafeRelease(s.cap); SafeRelease(s.client);
    s.bufFrames = s.target = s.period = 0; s.resample = false; s.exclusive = false; s.sinc = false; s.posBase = 1.0;
    s.convOn = s.alignedIn = false; s.devRate = 0; s.inDelay = 0;   // 0.9.4
}

// Host thread. Closes every stream. The volume callback is detached first (owner = nullptr under the exclusive
// lock), so in-flight notifications cannot touch released objects.
void WasapiBridge::closeStreams() {
    if (volCb) volCb->detach();                          // first cut off access (0.9.8: waits for a callback in flight)
    IAudioEndpointVolume* v = nullptr;
    AcquireSRWLockExclusive(&volLock); v = epVol; epVol = nullptr; ReleaseSRWLockExclusive(&volLock);
    if (v && volCb) v->UnregisterControlChangeNotify(volCb);
    SafeRelease(v);
    if (volCb) { volCb->Release(); volCb = nullptr; }
    if (threadStuck) { render = nullptr; outClient = nullptr; outClock = nullptr; }
    SafeRelease(outClock); outClockFreq = 0;
    SafeRelease(render);
    SafeRelease(outClient);
    closeSource(srcIn);
    closeSource(srcLb);
    for (int k = 0; k < kMaxExtra; ++k) { closeSource(srcEx[k]); closeSink(sinks[k]); }
    outBufFrames = outPeriod = outStreamLat = outFill = outPeriodAsio = 0;
    outConvOn = false; outDevRate = rate; engineKind = 0; engineLine[0] = 0; monPath = 0;   // 0.9.4
    aligned = false; primDelayFrames = 0;
    if (streamsOpen) { Log("Streams closed"); WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwStream, "StreamsClosed", wbtrace::U32("Generation", streamGen)); }
    streamsOpen = false;
}

// Volume callback thread (or the host thread when opening). Shared lock against closeStreams. index -1 = main
// output, 0.. = extra output sink. Recomputes whether software volume applies and stores it in an atomic.
void WasapiBridge::onVolumeNotify(int index) {
    AcquireSRWLockShared(&volLock);
    IAudioEndpointVolume* v = index < 0 ? epVol : (index < kMaxExtra ? sinks[index].epVol : nullptr);
    if (v) {
        float db = 0.f; BOOL mute = FALSE;
        v->GetMasterVolumeLevel(&db);
        v->GetMute(&mute);
        const float g = mute ? 0.f : powf(10.f, db / 20.f);
        if (index < 0) volGain = g; else sinks[index].volGain = g;
    }
    ReleaseSRWLockShared(&volLock);
}

// ---------------------------------------------------------------- extra outputs (0.5.0)
// Host thread. Opens extra output sink k. No event (serviced on every wake-up of the master). Exclusive only when
// Mode=Exclusive and the device accepts it. Failures leave the sink closed and its ASIO channels silent.
void WasapiBridge::openSink(int k) {
    static const char* tags[kMaxExtra] = {"Output 2", "Output 3", "Output 4"};
    OutputSink& s = sinks[k];
    s.tag = tags[k];
    s.asioFirst = k < nOutEx ? outEx[k].asioFirst : 0;
    s.asioCount = k < nOutEx ? outEx[k].ch : 0;
    s.exclusive = s.softVolume = false;
    if (k >= nOutEx) return;
    IMMDevice* dev = outEx[k].dev;
    if (!dev) { Log("%s: device not found, channels are silent", s.tag); return; }
    const int mixCh = outEx[k].mixCh;
    if (cfg.exclusive) {
        UINT32 per = 0;
        if (initExclusive(dev, &s.client, s.fmt, s.bufFrames, per, false, mixCh)) { s.exclusive = true; s.period = per; }
        else Log("%s: Exclusive not available -> Shared", s.tag);
    }
    if (!s.exclusive) {
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.client))) { Log("%s: Activate failed", s.tag); return; }
        WAVEFORMATEX* mix = nullptr; s.client->GetMixFormat(&mix);
        DWORD mask = 0xFFFFFFFF;
        if (mix && mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->nChannels == mixCh) mask = ((WAVEFORMATEXTENSIBLE*)mix)->dwChannelMask;
        if (mix) CoTaskMemFree(mix);
        REFERENCE_TIME defP = 0, minP = 0; s.client->GetDevicePeriod(&defP, &minP);
        s.period = RefTimeFrames(rate, (double)defP);
        double frames = 2.0 * outPeriodAsio + 4.0 * s.period;
        if (frames < rate * 0.04) frames = rate * 0.04;
        WAVEFORMATEXTENSIBLE f = MakeFmt(rate, mixCh, 32, 32, true, mask);
        HRESULT hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                          (REFERENCE_TIME)(frames / rate * 10000000.0 + 0.5), 0, &f.Format, nullptr);
        if (FAILED(hr)) { Log("%s: Initialize Shared failed hr=0x%08lX, channels are silent", s.tag, (unsigned long)hr); SafeRelease(s.client); return; }
        s.fmt = ParseFmt(&f.Format);
        s.client->GetBufferSize(&s.bufFrames);
    }
    s.fmt.clamp = s.exclusive;
    if (FAILED(s.client->GetService(__uuidof(IAudioRenderClient), (void**)&s.render))) { Log("%s: GetService render failed", s.tag); SafeRelease(s.client); return; }
    s.clockFreq = 0;
    if (SUCCEEDED(s.client->GetService(__uuidof(IAudioClock), (void**)&s.clock)) &&
        (FAILED(s.clock->GetFrequency(&s.clockFreq)) || s.clockFreq == 0)) SafeRelease(s.clock);
    s.targetPad = SinkTargetPad(outPeriodAsio, s.period, s.bufFrames);
    s.ditherActive = s.exclusive && !s.fmt.isFloat && s.fmt.validBits <= 24;   // gated by lv.dither (live)
    if (s.exclusive) {                     // software volume if the card has none (same as the main output)
        IAudioEndpointVolume* v = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&v))) {
            DWORD hw = 0; v->QueryHardwareSupport(&hw);
            s.softVolume = !(hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME);
            if (s.softVolume) {
                AcquireSRWLockExclusive(&volLock); s.epVol = v; ReleaseSRWLockExclusive(&volLock);
                s.volCb = new VolumeCallback(this, k);
                v->RegisterControlChangeNotify(s.volCb);
                onVolumeNotify(k);
            } else v->Release();
        }
    }
    char b[32]; FmtStr(s.fmt, b, sizeof b);
    Log("%s open (%s): %s, buffer %u, period %u, queued target %u, IAudioClock=%d, ASIO channels %d..%d",
        s.tag, s.exclusive ? "Exclusive" : "Shared", b, s.bufFrames, s.period, s.targetPad, (int)(s.clock != nullptr),
        s.asioFirst + 1, s.asioFirst + s.asioCount);
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwStream, "ExtraOutputOpen", wbtrace::Str("Sink", s.tag), wbtrace::Bool("Exclusive", s.exclusive),
             wbtrace::Str("Format", b), wbtrace::U32("BufferFrames", s.bufFrames), wbtrace::U32("PeriodFrames", s.period),
             wbtrace::U32("TargetPadFrames", s.targetPad), wbtrace::I32("FirstChannel", s.asioFirst), wbtrace::I32("Channels", s.asioCount));
}

// Host thread. Releases an extra output sink's COM objects (leaked on purpose if the audio thread is stuck).
void WasapiBridge::closeSink(OutputSink& s) {
    if (s.volCb) s.volCb->detach();                      // 0.9.8: waits for a callback in flight
    IAudioEndpointVolume* v = nullptr;
    AcquireSRWLockExclusive(&volLock); v = s.epVol; s.epVol = nullptr; ReleaseSRWLockExclusive(&volLock);
    if (v && s.volCb) v->UnregisterControlChangeNotify(s.volCb);
    SafeRelease(v);
    if (s.volCb) { s.volCb->Release(); s.volCb = nullptr; }
    if (threadStuck) { s.render = nullptr; s.client = nullptr; s.clock = nullptr; }   // leak when the thread is stuck
    SafeRelease(s.clock); SafeRelease(s.render); SafeRelease(s.client);
    s.clockFreq = 0; s.bufFrames = s.period = s.targetPad = 0; s.exclusive = s.softVolume = s.sinc = s.ditherActive = false;
    s.streamLat = s.estLat = 0;
}

// Host thread (openStreams): estimated latency of every path and, with AlignDevices and extras open,
// delays so that all of them match the slowest one. Outputs: main = WASAPI buffer + stream latency;
// sink = sp (includes the sinc look-ahead) + targetPad + one master period + stream latency.
// Inputs: target (includes sinc) + stream latency. Loopback does not take part.
// The main output is delayed with the preallocated primDelay line; sinks get a larger sp; inputs a larger target.
void WasapiBridge::computeAlignment() {
    aligned = false; primDelayFrames = 0;
    auto frames = [&](REFERENCE_TIME t) { return RefTimeFrames(rate, (double)t); };
    // 0.9.4: in ASIO frames; the device part of a converted output is converted (sched::OutLatency - bufSize)
    primOutLat = outConvOn ? (UINT32)(sched::OutLatency(bufSize, rate, outDevRate, outFill, outStreamLat, outConv.ahead()) - bufSize)
                           : (UINT32)lround((double)(outFill + outStreamLat) * rate / outDevRate);
    bool extras = false;
    UINT32 maxOut = primOutLat, maxIn = 0;
    for (OutputSink& o : sinks) {
        o.streamLat = o.estLat = 0;
        if (!o.client) continue;
        REFERENCE_TIME l = 0; o.client->GetStreamLatency(&l);
        o.streamLat = frames(l);
        o.estLat = (UINT32)o.sp + o.targetPad + outPeriodAsio + o.streamLat;
        if (o.estLat > maxOut) maxOut = o.estLat;
        extras = true;
    }
    InputSource* ins[1 + kMaxExtra] = {&srcIn, &srcEx[0], &srcEx[1], &srcEx[2]};
    for (int i = 0; i < 1 + kMaxExtra; ++i) {
        InputSource* x = ins[i];
        x->estLat = 0;
        if (!x->client) continue;
        REFERENCE_TIME l = 0; x->client->GetStreamLatency(&l);
        x->estLat = x->target + frames(l);
        // 0.9.4: an aligned converted input reports its target without the block (sched::ReportLatencies); compare
        // it on the same basis as the others (reported - block), clamped at 0
        if (x->alignedIn) x->estLat = x->estLat > (UINT32)bufSize ? x->estLat - (UINT32)bufSize : 0;
        if (x->estLat > maxIn) maxIn = x->estLat;
        if (i > 0) extras = true;
    }
    primInLat = srcIn.estLat;
    alignOut = maxOut; alignIn = maxIn;
    if (!cfg.alignDevices || !extras) return;
    aligned = true;
    // the delay line runs at the device rate (writeOut)
    primDelayFrames = (UINT32)lround((double)(maxOut - primOutLat) * outDevRate / rate);
    for (OutputSink& o : sinks) if (o.client && o.estLat < maxOut) o.sp += (double)(maxOut - o.estLat);
    for (InputSource* x : ins) if (x->client && x->estLat < maxIn) {
        const UINT32 add = maxIn - x->estLat;
        x->target += add;
        if (x->alignedIn) x->inDelay += add;      // 0.9.4: an aligned input is delayed through its fixed delay
    }
    Log("Alignment: outputs at %u frames (%.2f ms; main output delay %u), inputs at %u frames (%.2f ms)",
        maxOut, 1000.0 * maxOut / rate, primDelayFrames, maxIn, 1000.0 * maxIn / rate);
}

// Host thread: routing of the main output (OutputMap "asio:dev,..." 1-based). Empty = identity.
// Fills routeSrc[deviceChannel] = ASIO channel index (or -1 = silent); invalid pairs are logged and ignored.
void WasapiBridge::buildRoute() {
    const int dch = outFmt.channels < kMaxCh ? outFmt.channels : kMaxCh;
    for (int d = 0; d < kMaxCh; ++d) routeSrc[d] = d < nOutPrim && d < dch ? d : -1;
    routeDefault = true;
    if (cfg.outputMap.empty()) { publishLive(); return; }
    for (int d = 0; d < kMaxCh; ++d) routeSrc[d] = -1;
    routeDefault = false;
    const std::wstring& m = cfg.outputMap;
    // 0.9.8: parse.h (bounded number parser, fuzzed offline) instead of swscanf (undefined on numeric overflow)
    const int pairs = cfgp::ParseOutputMap(m, nOut, dch, routeSrc, [](const std::wstring& t) {
        Log("OutputMap: ignored entry '%s'", Utf8(t).c_str());
    });
    Log("Output routing: %d pairs (OutputMap=%s)", pairs, Utf8(m).c_str());
    publishLive();                         // F1: the routing table reaches the audio thread as a live parameter
}

// Host thread (panel). Fills a snapshot for the Status tab and the "Copy status" report: formats, latencies,
// counters (atomic loads) and one line per extra device. Never touches audio-thread-only state.
void WasapiBridge::getStatus(StatusInfo& s) {
    s.streamsOpen = streamsOpen; s.outExclusive = outExclusive; s.outLowLatency = outLowLatency;
    FmtStr(outFmt, s.outFmt, sizeof s.outFmt);
    if (srcIn.client) FmtStr(srcIn.fmt, s.inFmt, sizeof s.inFmt); else strcpy(s.inFmt, "-");
    s.rate = rate; s.bufSize = bufSize; s.outPeriod = outPeriod;
    s.engineKind = streamsOpen ? engineKind : -1; s.outDevRate = outDevRate; s.mixRate = mixRateOpen;
    s.outConv = streamsOpen && outConvOn;
    s.monAvail = streamsOpen && srcIn.cap ? (monPath != 0 ? 1 : 0) : -1; s.convSinc = outConvOn && outConv.sinc();
    latenciesNow(s.inLat, s.outLat);         // 0.9.8: internal form (no host pointer, no trace)
    s.nIn = nIn; s.nOut = nOut;
    s.outGlitches = cntOutGlitch; s.timeouts = cntTimeouts;
    s.inUnderruns = srcIn.underruns + srcLb.underruns; s.inDrops = srcIn.drops + srcLb.drops; s.inGlitches = srcIn.glitches + srcLb.glitches;
    s.inPpm = (srcIn.ratioView.load() - 1.0) * 1e6; s.lbPpm = (srcLb.ratioView.load() - 1.0) * 1e6;
    s.hasInput = srcIn.client != nullptr; s.hasLoopback = srcLb.client != nullptr; s.monitorActive = monAny;
    s.inResample = srcIn.resample;
    s.sinc = srcIn.sinc || srcLb.sinc; s.dither = ditherActive && cfg.dither && streamsOpen;
    s.loadNow = loadNow.load(std::memory_order_relaxed); s.loadMax = loadMax.load(std::memory_order_relaxed);
    s.outName = outName; s.inName = inName;
    s.outDevChannels = outFmt.channels > 0 && streamsOpen ? outFmt.channels : outDevCh;
    s.nInDev = nInDev; s.nOutPrim = nOutPrim; s.safeMode = safe;
    s.outLatOffsetMs = cfg.outLatOffsetMs; s.inLatOffsetMs = cfg.inLatOffsetMs;
    s.dmActive = liveH.dmOn && streamsOpen;
    s.monitorActive = s.monitorActive || s.dmActive;
    s.inDropouts = s.inUnderruns + s.inDrops;
    for (const InputSource& x : srcEx) s.inDropouts += x.underruns.load() + x.drops.load();
    s.extras.clear();
    s.captureDenied = captureDenied;
    s.outBluetooth = outBt; s.inBluetooth = inBt; s.a2dpBlocked = a2dpBlocked; s.recBuffer = recBuf;   // 0.8.0
    s.limiterOn = cfg.limiterOn; s.rateAuto = cfg.sampleRateAuto; s.hostProfile = hostProfile;
    // 0.9.0 (H4): these lines are shown in the panel, so they use the interface language (lang::T); the log and
    // the tools stay English. getStatus runs on the host thread only, like the string table.
    using namespace lang;
    wchar_t b[512];
    if (captureDenied) s.extras += T(S_DX_PRIV);
    if (streamsOpen && (nOutEx || nInEx)) {
        swprintf(b, 512, T(S_DX_ALIGN), aligned ? T(S_YES) : T(S_NO),
                 1000.0 * primOutLat / rate, 1000.0 * primInLat / rate,
                 aligned ? T(S_DX_ALIGN_ALL) : L"");
        s.extras += b;
        if (aligned) { swprintf(b, 512, T(S_DX_COMMON), 1000.0 * alignOut / rate, 1000.0 * alignIn / rate); s.extras += b; }
    }
    for (int k = 0; k < nOutEx; ++k) {
        const OutputSink& o = sinks[k];
        if (!o.client) swprintf(b, 512, T(S_DX_OUT_NOTOPEN), k + 2,
                                outEx[k].dev ? outEx[k].name.c_str() : outEx[k].want.c_str(), o.asioFirst + 1, o.asioFirst + o.asioCount);
        else swprintf(b, 512, T(S_DX_OUT), k + 2,
                      outEx[k].name.c_str(), o.exclusive ? T(S_EXCLUSIVE) : T(S_SHARED), 1000.0 * o.estLat / rate,
                      (o.ratioView.load() - 1.0) * 1e6, o.underflows.load(), o.overflows.load());
        s.extras += b;
    }
    for (int k = 0; k < nInEx; ++k) {
        const InputSource& x = srcEx[k];
        if (!x.client) swprintf(b, 512, T(S_DX_IN_NOTOPEN), k + 2, inEx[k].dev ? inEx[k].name.c_str() : inEx[k].want.c_str());
        else swprintf(b, 512, T(S_DX_IN), k + 2,
                      inEx[k].name.c_str(), 1000.0 * x.estLat / rate, (x.ratioView.load() - 1.0) * 1e6, x.resample ? T(S_CORRECTING) : T(S_NO_CORRECTION), x.underruns.load(), x.drops.load());
        s.extras += b;
    }
}

// Host thread. Peaks are drained with exchange(0): the audio thread accumulates the maximum until the next read.
// Lock-free: only atomic loads/exchanges, so the panel never blocks the audio thread.
void WasapiBridge::readMeters(MeterSnapshot& m, int consumer) {
    if (consumer < 0 || consumer >= WB_METER_CONSUMERS) consumer = WB_METERS_PANEL;
    m.nIn = nIn; m.nOut = nOut; m.running = running.load();
    for (int c = 0; c < kMaxCh; ++c) {
        // Drain the atomics once and merge into every consumer's accumulator (H only, no locks), then hand this
        // consumer its accumulated maximum and clear it.
        const float pi = peakIn[c].exchange(0.f, std::memory_order_relaxed);
        const float po = peakOut[c].exchange(0.f, std::memory_order_relaxed);
        for (int k = 0; k < WB_METER_CONSUMERS; ++k) {
            if (pi > meterAccIn[k][c]) meterAccIn[k][c] = pi;
            if (po > meterAccOut[k][c]) meterAccOut[k][c] = po;
        }
        m.in[c] = meterAccIn[consumer][c]; m.out[c] = meterAccOut[consumer][c];
        meterAccIn[consumer][c] = 0.f; meterAccOut[consumer][c] = 0.f;
        m.inAct[c] = inActive[c]; m.outAct[c] = outActive[c];
    }
    m.clipIn = clipInMask.load(std::memory_order_relaxed);
    m.clipOut = clipOutMask.load(std::memory_order_relaxed);
    // G2: the largest gain reduction since the last read, merged per consumer like the peaks (H only).
    m.limOn = liveH.limOn && streamsOpen;
    const float gr = limGr.exchange(0.f, std::memory_order_relaxed);
    for (int k = 0; k < WB_METER_CONSUMERS; ++k) if (gr > limGrAcc[k]) limGrAcc[k] = gr;
    m.limGrDb = limGrAcc[consumer]; limGrAcc[consumer] = 0.f;
}

// Host thread. While the audio thread runs, this only publishes the request (the thread consumes it when it wakes
// and is the only one touching its internal state). Without a thread, the atomics are cleared directly (start()
// resets the rest). what: bitmask of RC_* flags.
void WasapiBridge::resetCounters(uint32_t what) {
    if (thread && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT) { resetReq.fetch_or(what, std::memory_order_release); return; }
    if (what & WB_RESET_CLIPS) { clipInMask = 0; clipOutMask = 0; }
    if (what & WB_RESET_STATS) {
        loadMax = 0.f; cntOutGlitch = 0; cntTimeouts = 0;
        InputSource* srcs[2 + kMaxExtra] = {&srcIn, &srcLb, &srcEx[0], &srcEx[1], &srcEx[2]};
        for (InputSource* x : srcs) { x->underruns = 0; x->drops = 0; x->glitches = 0; }
        for (OutputSink& o : sinks) { o.underflows = 0; o.overflows = 0; }
    }
}

// Audio thread, at the start of every wake-up: consumes resetReq (exchange) and clears the requested counters.
void WasapiBridge::applyResetRequest() {
    if (!resetReq.load(std::memory_order_relaxed)) return;
    const uint32_t r = resetReq.exchange(0, std::memory_order_acquire);
    if (r & WB_RESET_CLIPS) { clipInMask.store(0, std::memory_order_relaxed); clipOutMask.store(0, std::memory_order_relaxed); }
    if (r & WB_RESET_STATS) {
        loadMaxA = 0; loadMax.store(0.f, std::memory_order_relaxed);
        cntOutGlitch = 0; cntTimeouts = 0;
        srcIn.underruns = 0; srcIn.drops = 0; srcIn.glitches = 0;
        srcLb.underruns = 0; srcLb.drops = 0; srcLb.glitches = 0;
        for (InputSource& x : srcEx) { x.underruns.store(0, std::memory_order_relaxed); x.drops.store(0, std::memory_order_relaxed); x.glitches.store(0, std::memory_order_relaxed); }
        for (OutputSink& o : sinks) { o.underflows.store(0, std::memory_order_relaxed); o.overflows.store(0, std::memory_order_relaxed); }
    }
}

// ---------------------------------------------------------------- audio thread
// Audio thread entry point.
// 0.9.8 (exception barrier): nothing on the real-time path allocates, so nothing there is expected to throw; the
// guard sits here, outside the loop, where it costs nothing while no exception is in flight. If an exception ever
// escapes (for example from a host callback built with a compatible C++ runtime), the thread ends cleanly: it is
// no longer "running", the host is asked for a reset, and audioTid is cleared. The COM references taken by run()
// are not released in that case (their owners are unknown at this point): a small leak instead of a crash.
DWORD WINAPI WasapiBridge::threadProc(void* p) {
    WasapiBridge* d = static_cast<WasapiBridge*>(p);
    try {
        d->run();
    } catch (...) {
        d->audioTid = 0;
        d->lastHr = (long)E_UNEXPECTED;
        d->running = false;
        d->postReset(RR_WASAPI);
    }
    return 0;
}

// Audio thread. Drains every available packet from a capture client into the source's FIFO (silent packets
// become zeros, discontinuities count as glitches, overfill drops the oldest data and counts a drop).
// isDevInput: also feeds the direct-monitoring FIFO. Returns true if the capture client failed (the caller
// posts one capture reset per cycle).
// Real-time safe: preallocated FIFOs only.
bool WasapiBridge::pullSource(InputSource& s, IAudioCaptureClient* cap, bool isDevInput) {
    UINT32 pkt = 0;
    HRESULT hr;
    bool mon = false;
    // F5: same-clock input (no drift correction) feeds the monitor FIFO straight from the capture packets (lowest
    // latency). A drift-corrected input feeds it after resampling instead (readSource), so that the monitor runs
    // on the output clock too. 0.9.4: monPath (set at open) also covers the converted streams.
    if (isDevInput) mon = monFeed && monPath == 1;
    const int dch = s.fmt.channels;
    const int nch = s.asioCount;
    // 0.9.4: a converted source decodes into the converter (device rate), which fills the FIFO at the ASIO rate.
    auto drain = [&]() {
        const size_t made = s.conv.producible();
        if (!made) return;
        float* cur = nullptr;
        s.conv.produce(made, [&](size_t, int c, float v) { if (c == 0) cur = s.fifo.pushSlot(); cur[c] = v; });
        s.qMade += made;
    };
    // 0.9.8: at most 256 packets per wake-up (a device that keeps reporting packets cannot hold the audio thread in
    // this loop; what is left is read at the next wake-up).
    for (int guard = 0; guard < 256 && SUCCEEDED(hr = cap->GetNextPacketSize(&pkt)) && pkt > 0; ++guard) {
        BYTE* d = nullptr; UINT32 n = 0; DWORD flags = 0;
        hr = cap->GetBuffer(&d, &n, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && s.primed) ++s.glitches;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || !d;
        for (UINT32 i = 0; i < n; ++i) {
            float* o = s.convOn ? s.conv.pushSlot() : s.fifo.pushSlot();
            const BYTE* fr = silent ? nullptr : d + (size_t)i * s.fmt.block;
            for (int c = 0; c < nch; ++c) {
                if (!fr) { o[c] = 0.f; continue; }
                int sc = c < dch ? c : (dch == 1 ? 0 : -1);
                o[c] = sc >= 0 ? GetSample(fr + sc * s.fmt.bytes, s.fmt) : 0.f;
            }
            if (s.convOn && nch == 0) o[0] = 0.f;
            if (mon) { float* m = monFifo.pushSlot(); for (int c = 0; c < nch; ++c) m[c] = o[c]; }
        }
        cap->ReleaseBuffer(n);
        if (s.convOn) drain();             // per packet: the converter's ring only holds about one buffer
    }
    const bool invalid = FAILED(hr);
    size_t fill = s.fifo.size();
    if (s.alignedIn) {
        // 0.9.4: frames older than the next block are dropped by readSource; this only guards the FIFO's capacity
        // (callbacks stopped for a long time). readSource re-aligns afterwards.
        const size_t maxF = (size_t)s.inDelay + (size_t)bufSize * 4 + (size_t)s.exHi * 2;
        if (fill > maxF) { const size_t dr = fill - maxF / 2; s.fifo.drop(dr); s.qHead += dr; ++s.drops; }
    } else if (!s.resample) {
        size_t maxF = (size_t)bufSize * 2 + (size_t)s.target * 3;
        if (fill > maxF) { s.fifo.drop(fill - ((size_t)bufSize + s.target)); ++s.drops; }
    } else if (fill > (size_t)bufSize * 4 + (size_t)s.target * 6) {
        s.fifo.drop(fill - ((size_t)bufSize + s.target)); s.pos = s.posBase; ++s.drops;
    }
    return invalid;
}

// Audio thread. Produces one ASIO block (bufSize frames) for a source into the host's input buffers [idx].
// Without drift correction it copies from the FIFO; with it, a variable-ratio resampler (Hermite or sinc) reads
// the FIFO and a PI controller (drift.h) adjusts the ratio so the FIFO fill stays at the set point
// target + bufSize/2. Underruns produce silence and re-prime the source.
void WasapiBridge::readSource(InputSource& s, int idx) {
    const int n = s.asioCount;
    // F5: a drift-corrected main input also feeds the direct-monitoring FIFO with exactly the block delivered to
    // the host (one block per bufferSwitch, i.e. in lockstep with outFifo). Silence is pushed on underruns so the
    // monitor keeps its timing.
    const bool feedMon = &s == &srcIn && monPath == 2 && monFeed && !monScratch.empty();
    auto pushMon = [&](bool silent) {
        for (long i = 0; i < bufSize; ++i) {
            float* m = monFifo.pushSlot();
            for (int c = 0; c < monFifo.ch; ++c) m[c] = (silent || c >= n) ? 0.f : monScratch[(size_t)i * kMaxCh + c];
        }
    };
    auto zero = [&]() {
        for (int c = 0; c < n; ++c) if (float* o = inBufs[s.asioFirst + c][idx]) memset(o, 0, sizeof(float) * bufSize);
        if (feedMon) pushMon(true);
    };
    if (!s.cap) { zero(); return; }
    if (s.alignedIn) {
        // 0.9.4: converted input on the output's clock: FIFO frame q (ASIO rate, counted from the start of the
        // capture) is delivered at samplePosition q + inDelay (sched.h), so the round trip is fixed and equals the
        // reported latencies. `excess` = frames ready beyond this block; outside [0, exHi] the alignment was lost
        // (output dropout, late capture start) and the stream re-aligns to exMid (counted as a drop / underrun).
        const int64_t q0s = (int64_t)samplePos + s.qOff - (int64_t)s.inDelay;
        const int64_t excess = (int64_t)s.qMade - (q0s + (int64_t)bufSize);
        if (q0s >= 0) s.primed = true;
        if (q0s >= 0 && (excess < 0 || excess > (int64_t)s.exHi)) {
            if (excess < 0) ++s.underruns; else ++s.drops;
            s.qOff += excess - (int64_t)s.exMid;
        }
        const int64_t q0 = (int64_t)samplePos + s.qOff - (int64_t)s.inDelay;
        float* scratch = feedMon ? monScratch.data() : nullptr;
        for (long i = 0; i < bufSize; ++i) {
            const int64_t q = q0 + i;
            const float* f = nullptr;
            if (q >= (int64_t)s.qHead && q < (int64_t)s.qMade) f = s.fifo.slot((size_t)(q - (int64_t)s.qHead));
            for (int c = 0; c < n; ++c) {
                const float v = f ? f[c] : 0.f;
                if (float* o = inBufs[s.asioFirst + c][idx]) o[i] = v;
                if (scratch && c < kMaxCh) scratch[(size_t)i * kMaxCh + c] = v;
            }
        }
        if (feedMon) pushMon(false);
        const int64_t end = q0 + bufSize;           // frames before the next block are no longer needed
        if (end > (int64_t)s.qHead) {
            size_t dr = (size_t)(end - (int64_t)s.qHead);
            if (dr > s.fifo.size()) dr = s.fifo.size();
            s.fifo.drop(dr); s.qHead += dr;
        }
        return;
    }
    const size_t fill = s.fifo.size();
    const double sp = (double)s.target + bufSize * 0.5;      // fill set point
    if (!s.primed) {
        if (fill >= (size_t)bufSize + s.target + 4) { s.primed = true; s.pos = s.posBase; s.fillEma = sp; }
        else { zero(); return; }
    }
    if (!s.resample) {
        if (fill < (size_t)bufSize) { ++s.underruns; s.primed = false; zero(); return; }
        for (long i = 0; i < bufSize; ++i) {
            const float* f = s.fifo.slot(i);
            for (int c = 0; c < n; ++c) if (float* o = inBufs[s.asioFirst + c][idx]) o[i] = f[c];
        }
        s.fifo.drop(bufSize);
        return;
    }
    // Variable-ratio resampling. Hermite: x[k-1..k+2], pos in [1, 2). Sinc: x[k-kSincHalf+1..k+kSincHalf],
    // pos in [kSincHalf-1, kSincHalf). need guarantees that the highest index read is < fill.
    const double need = s.pos + bufSize * s.ratio + (s.sinc ? kSincHalf + 1.0 : 3.0);
    if ((double)fill < need) { ++s.underruns; s.primed = false; zero(); return; }
    double p = s.pos;
    float* scratch = feedMon ? monScratch.data() : nullptr;
    ResampleFifo(s.fifo, n, p, s.ratio, s.sinc, sincTab.data(), bufSize,
                 [&](long i, int c, float v) {
                     if (float* o = inBufs[s.asioFirst + c][idx]) o[i] = v;
                     if (scratch && c < kMaxCh) scratch[(size_t)i * kMaxCh + c] = v;
                 });
    if (feedMon) pushMon(false);
    size_t consumed = (size_t)p - (size_t)s.posBase;
    s.fifo.drop(consumed);
    s.pos = p - (double)consumed;
    // Time-normalized PI control (dt = block duration); constants and math in drift.h (validated in 0.2.2).
    const double r = PiUpdate(s.fillEma, s.integ, (double)s.fifo.size() - s.pos, sp, bufSize / rateRun);
    s.ratio = r; s.ratioView.store(r, std::memory_order_relaxed);
}

// Audio thread, on every wake-up of the master: tops up the extra device's buffer to targetPad with
// the resampled FIFO. Returns false if the device failed (the caller requests a reset once per cycle).
// now100ns: QPC time in 100 ns units, used to interpolate the IAudioClock position. Real-time safe: no locks,
// no allocation (the FIFO is preallocated), no logging.
bool WasapiBridge::serviceSink(OutputSink& s, IAudioClient* c, IAudioRenderClient* rc, IAudioClock* clk, double now100ns) {
    UINT32 pad = 0;
    if (FAILED(c->GetCurrentPadding(&pad))) return false;
    if ((double)s.fifo.size() > s.overLimit) {           // the device consumes less than expected: drop
        s.fifo.drop(s.fifo.size() - (size_t)(s.sp + s.posBase));
        s.pos = s.posBase; s.overflows.fetch_add(1, std::memory_order_relaxed);
    }
    const UINT32 w = SinkFramesToWrite(pad, s.bufFrames, s.targetPad);
    if (!w) return true;
    BYTE* d = nullptr;
    HRESULT hr = rc->GetBuffer(w, &d);
    if (hr == AUDCLNT_E_BUFFER_TOO_LARGE) return true;
    if (FAILED(hr)) return false;
    const double extra = s.sinc ? kSincHalf + 1.0 : 3.0;
    bool silent = false;
    if (!s.primed) {                                      // priming: FIFO >= set point + what is needed
        const double need = s.posBase + w * 1.003 + extra;
        const size_t fill = s.fifo.size();
        if ((double)fill >= s.sp + need) {
            const size_t keep = (size_t)(s.sp + need);
            if (fill > keep) s.fifo.drop(fill - keep);
            s.primed = true; s.pos = s.posBase; s.fillEma = s.sp; s.integ = 0; s.ratio = 1.0;
        } else silent = true;
    }
    if (!silent && (double)s.fifo.size() < s.pos + w * s.ratio + extra) {
        s.underflows.fetch_add(1, std::memory_order_relaxed); s.primed = false; silent = true;
    }
    if (silent) { s.written += w; return SUCCEEDED(rc->ReleaseBuffer(w, AUDCLNT_BUFFERFLAGS_SILENT)); }
    memset(d, 0, (size_t)w * s.fmt.block);
    const float g = lv.gain * (s.softVolume ? s.volGain.load(std::memory_order_relaxed) : 1.f);
    const DevFmt& f = s.fmt;
    const int dch = f.channels;
    double p = s.pos;
    const bool dith = s.ditherActive && lv.dither;
    // G2: with the limiter on, the samples of one frame are collected first (ResampleFifo calls back frame by
    // frame, channels 0..n-1 in order), limited together (linked), then written.
    const bool lon = lv.limOn;
    const float lceil = lv.limCeil;
    const int nw = s.asioCount < dch ? s.asioCount : dch;          // channels actually written
    lim::Limiter& lm = limSink[&s - sinks];
    float fr[kMaxCh];
    auto put = [&](long i, int ch, float x) {
        BYTE* dst = d + (size_t)i * f.block + (size_t)ch * f.bytes;
        if (dith && x != 0.f && ch < kMaxCh) PutSampleDither(dst, f, x, TriDither(s.ditherState[ch]));   // exact 0 stays undithered
        else PutSample(dst, f, x);
    };
    ResampleFifo(s.fifo, s.asioCount, p, s.ratio, s.sinc, sincTab.data(), (long)w, [&](long i, int ch, float v) {
        if (ch >= dch) return;
        const float x = v * g;
        if (!lon || nw > kMaxCh) { put(i, ch, x); return; }
        fr[ch] = x;
        if (ch != nw - 1) return;
        lm.frame(fr, nw, lceil, limRel);
        for (int c = 0; c < nw; ++c) put(i, c, fr[c]);
    });
    if (lon && lm.maxReductionDb > 0.f) { AtomicMax(limGr, lm.maxReductionDb); lm.maxReductionDb = 0.f; }
    const size_t consumed = (size_t)p - (size_t)s.posBase;
    s.fifo.drop(consumed);
    s.pos = p - (double)consumed;
    s.written += w;
    // Total fill = FIFO + what is queued in the device. With IAudioClock the played position is interpolated
    // to the current instant (avoids beating between the engine period and the master wake-up); without a clock, FIFO only.
    double queued = (double)s.targetPad;
    if (clk) {
        UINT64 dp = 0, qp = 0;
        if (SUCCEEDED(clk->GetPosition(&dp, &qp))) {
            const double played = (double)dp * rateRun / (double)s.clockFreq + (now100ns - (double)qp) * 1e-7 * rateRun;
            queued = (double)s.written - played;
        }
    }
    const double r = PiUpdate(s.fillEma, s.integ, ((double)s.fifo.size() - s.pos) + (queued - (double)s.targetPad), s.sp, (double)w / rateRun);
    s.ratio = r; s.ratioView.store(r, std::memory_order_relaxed);
    return SUCCEEDED(rc->ReleaseBuffer(w, 0));
}

// Audio thread. One ASIO block: read inputs, update meters, publish the timestamp, call the host's
// bufferSwitch / bufferSwitchTimeInfo, measure DSP load, and push extra-output channels into their FIFOs.
// Real-time safe.
void WasapiBridge::bufferSwitch() {
    const int idx = curIndex;
    if (nInDev) readSource(srcIn, idx);
    for (int k = 0; k < nInEx; ++k) readSource(srcEx[k], idx);   // no device: zeros
    if (lbFirst < nIn) readSource(srcLb, idx);          // loopback channels exist (H state fixed at open)
    for (long c = 0; c < nIn; ++c) if (const float* b = inBufs[c][idx]) {     // input meters (what the host receives)
        const float m = PeakAbs(b, bufSize);
        if (m > 0.f) { AtomicMax(peakIn[c], m); if (m >= 1.f) clipInMask.fetch_or(1u << c, std::memory_order_relaxed); }
    }
    // Continuous timestamp: t0 + pos/rate, gently re-anchored to timeGetTime (1 ms with timeBeginPeriod) on the
    // first block of each wake-up (sched::BlockClock, shared with the offline timing model). 0.9.4: the device
    // buffer and period are in device frames (outDevRate).
    const uint64_t pos = samplePos;
    const double nsPerFrame = 1e9 / rateRun;
    double jump = 3.0 * outFill / outDevRate * 1e9; if (jump < 50e6) jump = 50e6;
    const double ts = blockClock.stamp(pos, wakeNs, firstOfWake, nsPerFrame, jump, outPeriod / outDevRate);
    firstOfWake = false;
    const uint64_t tsNow = ts > 0 ? (uint64_t)ts : 0;
    publishTime(pos, tsNow);
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    if (timeInfoMode) {
        ASIOTime t; memset(&t, 0, sizeof t);
        ToAsio64(pos, t.timeInfo.samplePosition.hi, t.timeInfo.samplePosition.lo);
        ToAsio64(tsNow, t.timeInfo.systemTime.hi, t.timeInfo.systemTime.lo);
        t.timeInfo.sampleRate = rateRun;    // 0.8.3: the rate of the running stream (never the H value mid-reset)
        t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        cbRun->bufferSwitchTimeInfo(&t, idx, ASIOTrue);
    } else {
        cbRun->bufferSwitch(idx, ASIOTrue);
    }
    QueryPerformanceCounter(&q1);
    // DSP load: host callback time / block duration. The first blocks after start are ignored
    // (hosts often initialize during them) so that the maximum is representative.
    const double load = (double)(q1.QuadPart - q0.QuadPart) * qpcToLoad;
    loadEma += loadAlpha * (load - loadEma);
    if (loadSkip) --loadSkip; else if (load > loadMaxA) { loadMaxA = load; loadMax.store((float)load, std::memory_order_relaxed); }
    loadNow.store((float)loadEma, std::memory_order_relaxed);
    for (long c = 0; c < nOut; ++c) if (const float* b = outBufs[c][idx]) {    // output meters (what the host sends)
        const float m = PeakAbs(b, bufSize);
        if (m > 0.f) { AtomicMax(peakOut[c], m); if (m >= 1.f) clipOutMask.fetch_or(1u << c, std::memory_order_relaxed); }
    }
    if (outConvOn) {                    // 0.9.4: ASIO-rate frames of every output channel into the converter
        const long nc = nOut > 0 ? nOut : 1;
        for (long i = 0; i < bufSize; ++i) {
            float* f = outConv.pushSlot();
            for (long c = 0; c < nc; ++c) f[c] = c < nOut && outBufs[c][idx] ? outBufs[c][idx][i] : 0.f;
        }
    } else for (long i = 0; i < bufSize; ++i) {
        float* f = outFifo.pushSlot();
        for (long c = 0; c < nOut; ++c) f[c] = outBufs[c][idx] ? outBufs[c][idx][i] : 0.f;
    }
    for (int k = 0; k < nOutEx; ++k) {                 // extra outputs: their ASIO channels into their FIFO
        OutputSink& o = sinks[k];
        if (!o.client) continue;
        for (long i = 0; i < bufSize; ++i) {
            float* f = o.fifo.pushSlot();
            for (int c = 0; c < o.asioCount; ++c) { const float* b = outBufs[o.asioFirst + c][idx]; f[c] = b ? b[i] : 0.f; }
        }
    }
    samplePos = pos + (uint64_t)bufSize;
    curIndex = 1 - idx;
    cntSwitches.store(cntSwitches.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);   // A is the only writer
}

// Audio thread. One frame of direct monitoring (F5): mixes the monitor input frame `m` (nInDev channels, or
// nullptr = silence because the monitor FIFO ran dry) into mt[] (main output ASIO channels), moving every gain
// one step along its ramp. Two independent sources are summed:
//   - the host's ASIO direct monitoring (kAsioSetInputMonitor): per input channel, a destination channel o and
//     constant-power pan gains to o and o+1 (targets in monGainL/R, written by H);
//   - the panel's direct monitoring (lv.dm*): one input pair (or a mono input panned) to one output pair.
// Ramps advance even while the input is silent, so a monitor switched off fades out instead of clicking.
// Real-time safe: atomics are read with relaxed loads, no allocation, bounded loops (<= 32 channels).
void WasapiBridge::mixMonitor(float* mt, const float* m, float k) {
    const long nI = nInDev < kMaxCh ? nInDev : kMaxCh;
    for (long ic = 0; ic < nI; ++ic) {
        const float gl = hostRampL[ic].step(monGainL[ic].load(std::memory_order_relaxed), k);
        const float gr = hostRampR[ic].step(monGainR[ic].load(std::memory_order_relaxed), k);
        if ((gl == 0.f && gr == 0.f) || !m) continue;
        const int o = monOut[ic].load(std::memory_order_relaxed);
        if (o < 0 || o >= nOutPrim) continue;
        mt[o] += m[ic] * gl;
        if (o + 1 < nOutPrim) mt[o + 1] += m[ic] * gr;
    }
    const float t0 = lv.dmOn ? lv.dmG[0] : 0.f, t1 = lv.dmOn ? lv.dmG[1] : 0.f;
    const float g0 = dmRamp[0].step(t0, k), g1 = dmRamp[1].step(t1, k);
    if (!m || (g0 == 0.f && g1 == 0.f)) return;
    const int ia = lv.dmIn, ib = lv.dmMono ? lv.dmIn : lv.dmIn + 1;
    const int oa = lv.dmOut, ob = lv.dmOut + 1;
    if (ia < nI && oa < nOutPrim) mt[oa] += m[ia] * g0;
    if (ib < nI && ob < nOutPrim) mt[ob] += m[ib] * g1;
}

// Audio thread. Writes `frames` frames of the main output into the WASAPI buffer dst: applies the live gain,
// routing (live table), the alignment delay line, direct monitoring (after the delay, so its latency never
// depends on AlignDevices), a soft clipper while monitoring, software volume, clipping (Exclusive) and TPDF dither
// (integer Exclusive formats, live Dither key). Meters of monitored output channels include the monitor signal
// (post-mix). Real-time safe.
void WasapiBridge::writeOut(BYTE* dst, UINT32 frames) {
    const float g = lv.gain * (softVolume ? volGain.load(std::memory_order_relaxed) : 1.f);
    const int dch = outFmt.channels;
    const bool mon = monFeed;
    const bool dith = ditherActive && lv.dither;
    const bool pd = primDelayFrames > 0;
    const bool rdef = lv.routeDefault;
    const int8_t* route = lv.route;
    const bool lon = lv.limOn;
    const float lceil = lv.limCeil;
    if (mon) {
        // Keep the monitor queue short: the raw path receives capture packets, the resampled path one host block
        // per bufferSwitch; anything beyond this bound is stale and dropped (latency stays bounded).
        const size_t keep = monKeep;      // 0.9.4: set at open (frames at the output device's rate)
        const size_t lim = keep * 2 + frames;
        if (monFifo.size() > lim) monFifo.drop(monFifo.size() - keep);
    }
    float tmp[kMaxCh], mt[kMaxCh], pk[kMaxCh];
    const long nMt = nOutPrim < kMaxCh ? nOutPrim : kMaxCh;
    if (mon) for (long c = 0; c < nMt; ++c) pk[c] = 0.f;
    for (UINT32 i = 0; i < frames; ++i) {
        const float* s = outFifo.slot(i);
        for (long c = 0; c < nOut; ++c) tmp[c] = s[c] * g;
        bool hasMon = false;
        if (mon) {
            for (long c = 0; c < nMt; ++c) mt[c] = 0.f;
            const float* m = nullptr;
            if (monFifo.size() > 0) m = monFifo.slot(0); else monUnder.fetch_add(1, std::memory_order_relaxed);
            mixMonitor(mt, m, monK);
            if (m) monFifo.drop(1);
            for (long c = 0; c < nMt; ++c) {
                mt[c] = dmix::FlushDenormal(mt[c]);
                const float a = fabsf(s[c] + mt[c]);            // post-mix level (pre-gain, like the other meters)
                if (a > pk[c]) pk[c] = a;
            }
            hasMon = true;
        }
        BYTE* f = dst + (size_t)i * outFmt.block;
        // Physical channels: routing (identity by default; mono with identity = (L+R)/2)
        float vv[kMaxCh];
        const int dn = dch < kMaxCh ? dch : kMaxCh;
        if (dch == 1) vv[0] = rdef ? (nOutPrim >= 2 ? 0.5f * (tmp[0] + tmp[1]) : tmp[0]) : (route[0] >= 0 ? tmp[route[0]] : 0.f);
        else for (int c = 0; c < dn; ++c) vv[c] = route[c] >= 0 ? tmp[route[c]] : 0.f;
        if (pd) {                          // alignment: fixed delay of primDelayFrames (preallocated FIFO)
            float* q = primDelay.pushSlot();
            for (int c = 0; c < dn && c < primDelay.ch; ++c) q[c] = vv[c];
            const float* r = primDelay.slot(0);
            for (int c = 0; c < dn && c < primDelay.ch; ++c) vv[c] = r[c];
            primDelay.drop(1);
        }
        if (hasMon) {                      // same routing as the program, no delay, then the soft clipper
            auto mv = [&](int src) { return src >= 0 && src < nOutPrim ? mt[src] : 0.f; };
            if (dch == 1) vv[0] += rdef ? (nOutPrim >= 2 ? 0.5f * (mt[0] + mt[1]) : mt[0]) : mv(route[0]);
            else for (int c = 0; c < dn; ++c) vv[c] += mv(route[c]);
            for (int c = 0; c < dn; ++c) vv[c] = dmix::SoftClip(vv[c]);
        }
        if (lon) limMain.frame(vv, dn, lceil, limRelMain);   // G2: linked limiter, last stage before conversion/dither
        if (dch == 1) {
            const float v = vv[0];
            if (dith && v != 0.f) PutSampleDither(f, outFmt, v, TriDither(ditherState[0])); else PutSample(f, outFmt, v);
            continue;
        }
        for (int c = 0; c < dch; ++c) {
            const float v = c < dn ? vv[c] : 0.f;
            // exact 0 is kept (digital silence)
            if (dith && v != 0.f && c < kMaxCh) PutSampleDither(f + c * outFmt.bytes, outFmt, v, TriDither(ditherState[c]));
            else PutSample(f + c * outFmt.bytes, outFmt, v);
        }
    }
    if (mon) for (long c = 0; c < nMt; ++c) if (pk[c] > 0.f) {
        AtomicMax(peakOut[c], pk[c]);
        if (pk[c] >= 1.f) clipOutMask.fetch_or(1u << c, std::memory_order_relaxed);
    }
    if (lon && limMain.maxReductionDb > 0.f) { AtomicMax(limGr, limMain.maxReductionDb); limMain.maxReductionDb = 0.f; }
    outFifo.drop(frames);
}

// Audio thread body. Rules: registers with MMCSS ("Pro Audio", critical priority); takes its own COM references
// so objects outlive a host release while stuck; never locks, allocates or logs inside the loop; communicates
// with the host thread only through atomics and postReset (PostMessage to the hidden window, never asioMessage
// directly). Loop: wait for the output event (or stopEvent), service capture/sinks, run as many ASIO blocks as
// the WASAPI buffer has room for, and detect glitches. On a WASAPI error it stops and requests a reset.
void WasapiBridge::run() {
    audioTid = GetCurrentThreadId();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIdx = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (av) AvSetMmThreadPriority(av, AVRT_PRIORITY_CRITICAL);
    // Own references: the objects stay alive even if the host releases them while the thread is stuck.
    IAudioClient* oc = outClient; oc->AddRef();
    IAudioRenderClient* rc = render; rc->AddRef();
    IAudioClient* ic = srcIn.client; if (ic) ic->AddRef();
    IAudioClient* lc = srcLb.client; if (lc) lc->AddRef();
    IAudioCaptureClient* icap = srcIn.cap; if (icap) icap->AddRef();
    IAudioCaptureClient* lcap = srcLb.cap; if (lcap) lcap->AddRef();
    IAudioClock* clk = outExclusive ? outClock : nullptr; if (clk) clk->AddRef();
    // 0.5.0: own references to extra inputs and outputs (same rule as for the main ones)
    IAudioClient* xc[kMaxExtra] = {}; IAudioCaptureClient* xcap[kMaxExtra] = {};
    IAudioClient* sc[kMaxExtra] = {}; IAudioRenderClient* sr[kMaxExtra] = {}; IAudioClock* sk[kMaxExtra] = {};
    bool sinkFail[kMaxExtra] = {};
    for (int k = 0; k < kMaxExtra; ++k) {
        if (srcEx[k].client && srcEx[k].cap) { xc[k] = srcEx[k].client; xc[k]->AddRef(); xcap[k] = srcEx[k].cap; xcap[k]->AddRef(); }
        if (sinks[k].client && sinks[k].render) {
            sc[k] = sinks[k].client; sc[k]->AddRef(); sr[k] = sinks[k].render; sr[k]->AddRef();
            sk[k] = sinks[k].clock; if (sk[k]) sk[k]->AddRef();
        }
    }
    bool sinkResetPosted = false;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    const double blockSec = bufSize / rateRun;
    qpcToLoad = 100.0 / ((double)qf.QuadPart * blockSec);        // QPC ticks -> % of the block
    loadAlpha = 1.0 - exp(-blockSec / 0.3);                     // load EMA, tau 300 ms
    // Exclusive: with IAudioClock, a glitch only when the device has already played everything written. Without a
    // clock, fallback on late wake-ups: interval > max(2 periods - 1 ms, period + 1 ms) (double buffering leaves one period of margin).
    const double clkToFrames = clk ? rateRun / (double)outClockFreq : 0.0;
    const double perSec = (double)outPeriod / rateRun;
    double lateSec = 2.0 * perSec - 0.001;
    if (lateSec < perSec + 0.001) lateSec = perSec + 0.001;
    const double lateTicks = lateSec * (double)qf.QuadPart;
    uint64_t written = 0;              // frames delivered to the device (includes the pre-fill)
    LARGE_INTEGER prevWake = {};
    uint32_t wakes = 0;
    bool capResetPosted = false;
    bool stopNow = false;
    const bool headroom = outFill >= outPeriod + outPeriod / 2;   // only then does padding = 0 mean a real glitch

    BYTE* p = nullptr;
    const UINT32 prefill = outExclusive ? outBufFrames : outFill;   // 0.9.4: Shared keeps outFill queued
    if (SUCCEEDED(rc->GetBuffer(prefill, &p)) && SUCCEEDED(rc->ReleaseBuffer(prefill, AUDCLNT_BUFFERFLAGS_SILENT))) written = prefill;
    if (ic) ic->Start();
    if (lc) lc->Start();
    for (int k = 0; k < kMaxExtra; ++k) {
        if (xc[k]) xc[k]->Start();
        if (sc[k]) {                       // pre-fill with silence up to the target and start
            OutputSink& o = sinks[k];
            if (o.targetPad && SUCCEEDED(sr[k]->GetBuffer(o.targetPad, &p)) && SUCCEEDED(sr[k]->ReleaseBuffer(o.targetPad, AUDCLNT_BUFFERFLAGS_SILENT)))
                o.written = o.targetPad;
            if (FAILED(sc[k]->Start())) sinkFail[k] = true;
        }
    }
    HRESULT hr = oc->Start();

    HANDLE h[2] = {stopEvent, outEvent};
    // 0.9.8 (review H-list, no unbounded wait on device IO): the render event is awaited 2 s at a time; after 5
    // consecutive timeouts (10 s without a single event: device stalled or gone without a notification) the thread
    // ends with HRESULT_FROM_WIN32(ERROR_TIMEOUT) and the host is asked for a reset, instead of waiting forever.
    // A failed wait (invalid handle) ends the thread the same way.
    // Review R5 (independent review of 0.9.8): before giving up, the device position is checked. At every timeout
    // the thread samples the IAudioClock position (Exclusive) or the stream padding (Shared); if it changed at any
    // timeout since the counter was last reset, the device is still moving (for example right after a resume from
    // Modern Standby, when the event can lag behind the stream) and the counter starts again instead of ending the
    // stream. Only a device whose position stays frozen for 10 s ends it. Both calls are non-blocking queries.
    int idleTimeouts = 0;
    bool movedWhileIdle = false;
    auto probePosition = [&](uint64_t& v) -> bool {
        if (clk) { UINT64 dp = 0; if (SUCCEEDED(clk->GetPosition(&dp, nullptr))) { v = dp; return true; } return false; }
        UINT32 pd = 0;
        if (SUCCEEDED(oc->GetCurrentPadding(&pd))) { v = pd; return true; }
        return false;
    };
    uint64_t lastProbe = 0;
    bool lastProbeOk = probePosition(lastProbe);
    while (SUCCEEDED(hr)) {
        DWORD w = WaitForMultipleObjects(2, h, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_TIMEOUT) {
            ++cntTimeouts;
            uint64_t v = 0;
            const bool ok = probePosition(v);
            if (ok && lastProbeOk && v != lastProbe) movedWhileIdle = true;
            lastProbe = v; lastProbeOk = ok;
            if (++idleTimeouts >= 5) {
                if (movedWhileIdle) { idleTimeouts = 0; movedWhileIdle = false; continue; }   // still moving: keep waiting
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); break;
            }
            continue;
        }
        if (w != WAIT_OBJECT_0 + 1) { hr = HRESULT_FROM_WIN32(GetLastError()); if (SUCCEEDED(hr)) hr = E_FAIL; break; }
        idleTimeouts = 0; movedWhileIdle = false;
        wakeNs = (double)timeGetTime() * 1e6; firstOfWake = true;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        applyResetRequest();
        // F1: newest live parameters (seqlock read, never blocks; a publication in progress is picked up next time).
        pubLive.tryRead(lv, lvSeq);
        // F5: collect monitoring input while the host or the panel monitors, and until every ramp reached 0.
        {
            bool tail = dmRamp[0].cur != 0.f || dmRamp[1].cur != 0.f;
            for (long c = 0; c < nInDev && c < kMaxCh && !tail; ++c) tail = hostRampL[c].cur != 0.f || hostRampR[c].cur != 0.f;
            const bool want = (monAny.load(std::memory_order_relaxed) || lv.dmOn) && srcIn.cap && monPath;
            if (want && !monFeed && !tail) monFifo.clear();    // do not play stale audio when monitoring starts
            monFeed = want || tail;
        }
        if (outExclusive) {
            // The first wake-ups are skipped (the first event arrives right after Start) as well as those before the first
            // callback. At most one glitch per wake-up.
            bool glitch = false;
            if (wakes >= 3 && cntSwitches.load(std::memory_order_relaxed) > 0) {
                if (clk) {
                    UINT64 dpos = 0;
                    if (SUCCEEDED(clk->GetPosition(&dpos, nullptr))) {
                        const uint64_t played = (uint64_t)((double)dpos * clkToFrames);
                        if (played >= written) { glitch = true; written = played + outBufFrames; }   // re-synchronize
                    }
                } else if ((double)(now.QuadPart - prevWake.QuadPart) > lateTicks) glitch = true;
            }
            if (glitch) {
                ++cntOutGlitch;
                if (overloadSupported && !pendingOverload.exchange(true) && msgWnd) PostMessageW(msgWnd, WM_WB_OVERLOAD, 0, 0);
            }
            prevWake = now;
            if (wakes < 1000) ++wakes;
        }
        bool inv = false;
        if (icap) inv |= pullSource(srcIn, icap, true);
        if (lcap) inv |= pullSource(srcLb, lcap, false);
        for (int k = 0; k < kMaxExtra; ++k) if (xcap[k]) inv |= pullSource(srcEx[k], xcap[k], false);
        if (inv && !capResetPosted) { capResetPosted = true; postReset(RR_CAPTURE); }   // only once per cycle
        // Extra outputs: fed with what was produced up to the previous wake-up (one wake-up of delay).
        // A failure stops only that sink and requests a reset once per cycle.
        const double now100ns = (double)now.QuadPart * 1e7 / (double)qf.QuadPart;
        for (int k = 0; k < kMaxExtra; ++k) if (sc[k] && !sinkFail[k] && !serviceSink(sinks[k], sc[k], sr[k], sk[k], now100ns)) {
            sinkFail[k] = true;
            if (!sinkResetPosted) { sinkResetPosted = true; postReset(RR_DEVICE); }
        }
        UINT32 frames = outBufFrames;
        if (!outExclusive) {
            UINT32 pad = 0;
            hr = oc->GetCurrentPadding(&pad);
            if (FAILED(hr)) break;
            if (pad == 0 && cntSwitches.load(std::memory_order_relaxed) > 0 && headroom) {
                ++cntOutGlitch;
                if (overloadSupported && !pendingOverload.exchange(true) && msgWnd) PostMessageW(msgWnd, WM_WB_OVERLOAD, 0, 0);
            }
            frames = outFill > pad ? outFill - pad : 0;
        }
        if (!frames) continue;
        if (outConvOn) {
            // 0.9.4: ASIO callbacks on demand: as many as the converter needs to make `frames` device frames (its
            // look-ahead included), then the device-rate frames go to outFifo for writeOut.
            while (outConv.missingFor(frames) > 0) {
                bufferSwitch();
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) { stopNow = true; break; }
            }
            if (stopNow) break;
            float* cur = nullptr;               // all nOut channels are converted (OutputMap may use any of them)
            outConv.produce(frames, [&](size_t, int c, float v) { if (c == 0) cur = outFifo.pushSlot(); cur[c] = v; });
        } else while (outFifo.size() < frames) {
            bufferSwitch();
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) { stopNow = true; break; }   // stop() during the callback
        }
        if (stopNow) break;
        hr = rc->GetBuffer(frames, &p);
        if (hr == AUDCLNT_E_BUFFER_TOO_LARGE && outExclusive) { hr = S_OK; continue; }   // early event: wait for the next one
        if (FAILED(hr)) break;
        writeOut(p, frames);
        hr = rc->ReleaseBuffer(frames, 0);
        if (SUCCEEDED(hr)) written += frames;
    }
    oc->Stop(); oc->Reset();
    if (ic) { ic->Stop(); ic->Reset(); }
    if (lc) { lc->Stop(); lc->Reset(); }
    for (int k = 0; k < kMaxExtra; ++k) {
        if (xc[k]) { xc[k]->Stop(); xc[k]->Reset(); xc[k]->Release(); }
        if (xcap[k]) xcap[k]->Release();
        if (sc[k]) { sc[k]->Stop(); sc[k]->Reset(); sc[k]->Release(); }
        if (sr[k]) sr[k]->Release();
        if (sk[k]) sk[k]->Release();
    }
    rc->Release(); oc->Release(); if (ic) ic->Release(); if (lc) lc->Release();
    if (icap) icap->Release();
    if (lcap) lcap->Release();
    if (clk) clk->Release();
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
    audioTid = 0;
    if (FAILED(hr)) {                 // WASAPI error: the driver stops reporting "running" and asks the host for a reset
        lastHr = (long)hr;
        running = false;
        postReset(RR_WASAPI);
    }
}
