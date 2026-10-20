// WASAPI Driver for ASIO - automatic ASIO host test (console)
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
//   WasapiDriverTest.exe loads the driver the way a DAW does (CoCreateInstance on an STA thread) and runs a battery
//   of tests: T1 driver info, T2 10 s playback with timing analysis, T3 20 start/stop cycles, T4 buffer sizes
//   64..1024, T5 sample rates, T9 extra devices (when configured), T10 host rate kept across a re-created
//   instance (0.9.5), T6 10 full load/unload cycles, T7 asioMessage summary, T11 bad host calls (0.9.8: invalid
//   arguments and calls in the wrong order must return errors and leave the driver working; no audio), and the
//   optional T8 round-trip latency test (--latency) with a physical cable from
//   output 1 to input 1. Each test is PASSED, FAILED or SKIPPED (not applicable on this machine; not a failure).
//   The report is written to WasapiDriver-test.txt (exe folder, then %APPDATA%\WASAPI Driver for ASIO, then
//   %TEMP%) and printed live to the console. Outputs send silence except during T8.
//   0.9.1 (B3): --soak [minutes] (default 60) runs a long playback instead of the battery: one line per minute
//   (callbacks, interval deviation and maximum, overloads, invalid ASIOTime, private bytes, handles, GDI/USER
//   objects), device events (reset requests of the driver, default device and device state changes, sleep and
//   resume, stalls) with the time the stream needed to recover, optional buffer size changes (--soak-cycle,
//   every 5 minutes), and a verdict: PASSED when the stream recovered after every event and memory and handles
//   show no growth trend. Report: WasapiDriver-soak.txt (same folders, sanitized like the test report, rewritten
//   every minute so a crash leaves the data up to that minute). Ctrl+C ends the run early with a verdict.
//
// Threading
//   The main thread ALWAYS pumps messages while waiting: the driver's hidden message window lives on the thread
//   that called init(), and kAsioResetRequest / kAsioOverload arrive through it (asioMessage marshalling).
//   The driver callbacks (bufferSwitch*, asioMessage from the audio thread) run on the driver's audio thread: they
//   take no locks and do not allocate; they use preallocated arrays and atomics. The main thread reads the
//   recorded data only after stop().
//   The console Ctrl+C handler (soak mode) runs on a thread the system creates; it only sets an atomic flag.
//   The endpoint notification client (soak mode) runs on an MMDevice thread; it only posts messages.
//
// Order of the battery (wmain) and dependencies
//   T1 must load and init the driver, otherwise nothing else runs except T7 (T1 returns true even when only its
//   "Information" checks failed, so T2.. still run). 0.9.8: T11 runs right after T1, so every later test also
//   proves that the driver still works after the bad calls. Then T2, T3, T4, T5, T9, T8 (--latency only) and T10 run on
//   the same instance. T10 is last because it releases and re-creates that instance. The instance is released,
//   then T6 creates its own instances (only when the first result, T1 "Information", passed) and T7 summarizes the
//   messages of the whole run. Exit code 1 if any test FAILED (warnings and SKIPPED do not count).
//
// Common pass criteria (see Analyze / CountOk)
//   - callback count within +-5% + 1 of what the elapsed time predicts (+-10% + 1 for buffers >= 512),
//   - buffer index alternates 0/1/0/1 (a repeated index means the host would process the same half twice),
//   - samplePosition advances by exactly bufferSize per callback (a jump means lost or repeated blocks, or a
//     position that does not follow the delivered audio),
//   - systemTime never goes backwards,
//   - every ASIOTime passed to bufferSwitchTimeInfo is valid (tichk.h): not null, kSamplePositionValid,
//     kSystemTimeValid, kSampleRateValid with the rate the running instance reports.
//   These are the properties real hosts depend on for sync (position and timestamp) and for not glitching.
//
// Pitfalls
//   - Uses the current settings (config.ini / panel): Shared vs Exclusive, devices, extra devices. A DAW that keeps
//     the device open in Exclusive mode makes T2..T10 fail; close it first (printed in the header).
//   - T5, T8 (when the offset is saved) and T10 write config.ini through the driver (setSampleRate stores
//     LastHostRate; T8 uses SaveConfig). T5 and T10 restore the starting rate at the end.
//   - Timing tests are sensitive to machine load; a busy machine can produce stall warnings or count failures.
#include "common.h"
#include "privacy.h"
#include "conicon.h"
#include "tichk.h"
#include "asiocheck.h"   // 0.9.8: EngineInfo request of the private future() selector
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>
#include <shlobj.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

static const CLSID kClsid = {0xa63175bd, 0xb872, 0x4fa5, {0x95, 0x95, 0x46, 0x4f, 0x02, 0x36, 0xe1, 0xd0}};
static const wchar_t* kClsidStr = L"{A63175BD-B872-4FA5-9595-464F0236E1D0}";
static const wchar_t* kToolVersion = L"1.0";

// ---------------------------------------------------------------- report
static std::wstring g_out;
static void Emit(const std::wstring& t);
// Appends one formatted line to the report and prints it to the console (live progress).
// Never call P/F/AddResult from a driver callback: they allocate and SanitizeText reads environment variables.
static void P(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list a; va_start(a, fmt); int n = vswprintf(buf, 2048, fmt, a); va_end(a);
    if (n < 0) buf[2047] = 0;
    std::wstring line = SanitizeText(std::wstring(buf)) + L"\r\n";   // 0.6.1: no user paths/names (main thread only)
    g_out += line;
    Emit(line);                          // live progress on the console
}

// State of each test: PASSED, FAILED or SKIPPED (not applicable on this machine; does not count as a failure)
// SKIPPED is used when a test does not apply (range outside the driver's, no extra devices, no physical input
// for T8, rate not accepted for T10); it is never used to hide an error.
enum { ST_PASS = 0, ST_FAIL = 1, ST_SKIP = 2 };
static const wchar_t* StName(int st) { return st == ST_PASS ? L"PASSED" : st == ST_FAIL ? L"FAILED" : L"SKIPPED"; }
struct Result { std::wstring id, name; int st; std::wstring detail; };
static std::vector<Result> g_results;
static int g_warnings = 0;                       // warnings (not failures)
// Records a test result (shown in the summary) and prints it.
static void AddResultS(const wchar_t* id, const wchar_t* name, int st, const std::wstring& detail) {
    g_results.push_back(Result{id, name, st, detail});
    P(L"  -> %ls %ls: %ls  %ls", id, name, StName(st), detail.c_str());
}
// Shorthand for the usual PASSED / FAILED outcome.
static void AddResult(const wchar_t* id, const wchar_t* name, bool pass, const std::wstring& detail) {
    AddResultS(id, name, pass ? ST_PASS : ST_FAIL, detail);
}
// printf-style formatting into a std::wstring.
static std::wstring F(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list a; va_start(a, fmt); int n = vswprintf(buf, 1024, fmt, a); va_end(a);
    if (n < 0) buf[1023] = 0;
    return buf;
}

// ---------------------------------------------------------------- state shared with the callbacks
// kMaxBi: buffer infos (at most 32 inputs + 32 outputs, the limit T1 enforces). kMaxRec: per-callback records
// (131072; at 48 kHz / 64 frames that is about 174 s, far more than the longest run of 10 s; beyond it records are
// dropped but g_cbCount keeps counting). kMaxMsg: asioMessage records over the whole run.
// Pitfall: in soak mode ResetSession is not called, so g_recs fills up once and then stops recording; soak mode
// does not use g_recs (it only uses the g_s* sums), so that is harmless.
constexpr int kMaxBi = 64;
constexpr long kMaxRec = 1 << 17;
constexpr int kMaxMsg = 4096;

// One callback: QPC time on arrival, buffer index, driver samplePosition and systemTime (valid if hasTime).
// Publication: the audio thread writes g_recs[n] and then stores g_nRec = n + 1 with release order; the main
// thread reads g_nRec with acquire after stop().
struct Rec { int64_t qpc; int32_t idx; uint64_t spos, stime; bool hasTime; };
static Rec g_recs[kMaxRec];                       // writer: audio thread; reader: main thread after stop()
static std::atomic<long> g_nRec{0};
static std::atomic<long> g_cbCount{0};

// One asioMessage call (time, selector, value). Same publication scheme as Rec; asioMessage can come from the
// audio thread or from the driver's message window on the main thread.
struct MsgRec { int64_t qpc; long sel, val; };
static MsgRec g_msgs[kMaxMsg];
static std::atomic<long> g_nMsg{0};
static std::atomic<long> g_selCount[64];         // per selector (1..63)
static std::atomic<long> g_rateChanges{0};
// 0.7.0 (F9): validation of the ASIOTime the driver passes to bufferSwitchTimeInfo (audio thread writes, main
// thread reads after stop()). calls = bufferSwitchTimeInfo calls, plain = bufferSwitch calls (no time info).
// 0.8.3: counted per test phase and per kind (src/tichk.h); the expected rate of a phase is what the running
// driver instance reports with getSampleRate() just before start(). g_phaseNames[i] names phase i.
static tichk::Checker g_ti;
static std::atomic<long> g_tiPlain{0};
static std::vector<std::wstring> g_phaseNames(1, L"(before the tests)");

// Session configuration (written by the main thread before start(); the audio thread only reads it)
// g_bi is filled by createBuffers (buffers[0] / buffers[1] of every channel); g_sampleBytes and g_types come from
// getChannelInfo (T1 or SoakOpen). g_cbDrv must be cleared before the instance it points to is released.
static ASIOBufferInfo g_bi[kMaxBi];
static int   g_nBi = 0;
static long  g_bufSize = 256;
static int   g_sampleBytes[kMaxBi];
static long  g_types[kMaxBi];
static IASIO* g_cbDrv = nullptr;                 // for bufferSwitch without timeInfo (getSamplePosition)
static double g_rate = 48000.0;
static int64_t g_qpcFreq = 1;

// Latency test (state owned by the audio thread while running; the main thread reads it at the end)
// Only g_latMode and the counters are atomic: the other fields are written by the main thread before start() and
// by the audio thread while running; start() / stop() order those accesses (the main thread reads after stop()).
static std::atomic<bool> g_latMode{false};
static int   g_latIn = -1, g_latOut = -1;        // indices into g_bi
static bool  g_latInit = false, g_latWaiting = false, g_thrSet = false;
static uint64_t g_latStart = 0, g_emitPos = 0, g_nextEmit = 0;
static float g_noise = 0.f, g_thr = 0.f;
static std::atomic<int> g_latEmitted{0}, g_latMisses{0}, g_nLat{0};
static long  g_latRt[64];

static int64_t Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static uint64_t U64(unsigned long hi, unsigned long lo) { return ((uint64_t)hi << 32) | lo; }

// Sample helpers for the ASIO sample types the driver can report (16/24/32-bit int, float32/64, little endian).
// Unknown types are treated as 4 bytes so the silence memset in RecordBlock still covers a 32-bit buffer.
static int SampleBytes(long t) {
    switch (t) {
    case ASIOSTInt16LSB: return 2; case ASIOSTInt24LSB: return 3; case ASIOSTInt32LSB: return 4;
    case ASIOSTFloat32LSB: return 4; case ASIOSTFloat64LSB: return 8; default: return 4;
    }
}
// Int24 is assembled into the top 3 bytes of an int32 so the sign is extended, then scaled to -1..1.
static float ReadSample(const void* buf, long t, long i) {
    const unsigned char* b = (const unsigned char*)buf;
    switch (t) {
    case ASIOSTInt16LSB: return ((const int16_t*)buf)[i] / 32768.f;
    case ASIOSTInt24LSB: { const unsigned char* p = b + i * 3; int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); return (float)(v / 2147483648.0); }
    case ASIOSTInt32LSB: return (float)(((const int32_t*)buf)[i] / 2147483648.0);
    case ASIOSTFloat32LSB: return ((const float*)buf)[i];
    case ASIOSTFloat64LSB: return (float)((const double*)buf)[i];
    default: return 0.f;
    }
}
static void WriteHalf(void* buf, long t) {         // impulse of amplitude 0.5 at sample 0
    unsigned char* b = (unsigned char*)buf;
    switch (t) {
    case ASIOSTInt16LSB: ((int16_t*)buf)[0] = 16384; break;
    case ASIOSTInt24LSB: b[0] = 0x00; b[1] = 0x00; b[2] = 0x40; break;
    case ASIOSTInt32LSB: ((int32_t*)buf)[0] = 0x40000000; break;
    case ASIOSTFloat32LSB: ((float*)buf)[0] = 0.5f; break;
    case ASIOSTFloat64LSB: ((double*)buf)[0] = 0.5; break;
    default: break;
    }
}

// Audio thread, T8 only. State machine: 1 s of silence to measure the noise floor, then one impulse per second
// on output 1 and detection of the first input sample above the threshold; stores the round-trip in frames.
// Details: pos is the driver's samplePosition of this block. The impulse is written at sample 0 of output 1 in
// the block at position pos (after RecordBlock's silence memset, which runs first). Detection compares input 1
// against a threshold of 10x the measured noise floor (minimum 0.01 = -40 dBFS); the round trip is
// (pos + i) - emitPos frames, i.e. what getLatencies(input) + getLatencies(output) should add up to under the
// ASIO latency definition (both measured relative to the buffer switch). An impulse with no detection within
// 0.5 s counts as a miss. At most 15 impulses are emitted and the run stops after 10 detections (T8's poll).
// Runs only when samplePosition is valid (RecordBlock checks hasTime).
static void LatencyStep(long idx, uint64_t pos) {
    if (g_latIn < 0 || g_latOut < 0) return;
    const void* in = g_bi[g_latIn].buffers[idx];
    void* out = g_bi[g_latOut].buffers[idx];
    if (!in || !out) return;
    const long t = g_types[g_latIn];
    if (!g_latInit) { g_latInit = true; g_latStart = pos; g_noise = 0.f; g_thrSet = false; g_latWaiting = false; g_nextEmit = 0; }
    if (pos < g_latStart + (uint64_t)g_rate) {     // 1 s of silence: noise floor
        for (long i = 0; i < g_bufSize; ++i) { float v = fabsf(ReadSample(in, t, i)); if (v > g_noise) g_noise = v; }
        return;
    }
    if (!g_thrSet) { g_thr = g_noise * 10.f; if (g_thr < 0.01f) g_thr = 0.01f; g_thrSet = true; g_nextEmit = pos; }
    if (g_latWaiting) {
        for (long i = 0; i < g_bufSize; ++i) {
            if (fabsf(ReadSample(in, t, i)) > g_thr) {
                int n = g_nLat.load(std::memory_order_relaxed);
                if (n < 64) { g_latRt[n] = (long)(pos + (uint64_t)i - g_emitPos); g_nLat.store(n + 1, std::memory_order_release); }
                g_latWaiting = false;
                break;
            }
        }
        if (g_latWaiting && pos + (uint64_t)g_bufSize - g_emitPos > (uint64_t)(g_rate * 0.5)) { g_latWaiting = false; ++g_latMisses; }
    }
    if (!g_latWaiting && pos >= g_nextEmit && g_latEmitted.load() < 15 && g_nLat.load() < 10) {
        WriteHalf(out, g_types[g_latOut]);
        g_emitPos = pos; g_latWaiting = true; g_nextEmit = pos + (uint64_t)g_rate; ++g_latEmitted;
    }
}

// Audio thread: bufferSwitchTimeInfo callback. Records QPC time, buffer index and the driver's sample position
// and system time in the preallocated g_recs array; writes silence (or the T8 impulse) to every output.
static void RecordBlock(ASIOTime* t, long idx);
// The return value is not used by the driver; returning nullptr is what most hosts do. The validity check runs
// first so even a block that RecordBlock cannot record (array full) is still validated.
static ASIOTime* CbTimeInfo(ASIOTime* t, long idx, ASIOBool) {
    // F9: check the fields a host relies on: kSamplePositionValid, kSystemTimeValid, kSampleRateValid + rate.
    g_ti.check(t == nullptr, t ? t->timeInfo.flags : 0ul, t ? t->timeInfo.sampleRate : 0.0);   // 0.8.3: per phase
    RecordBlock(t, idx);
    return nullptr;
}

// 0.9.1 (B3) soak statistics: accumulated by the audio thread, taken (exchange) by the main thread once a
// minute. g_sPrev is audio-thread state; g_soakPrevReset (set by the main thread before start()) restarts it.
// Sums are in microseconds: count, sum and sum of squares give mean and deviation per minute, g_sMaxUs the worst
// interval. g_soakPrevReset makes the first callback after a (re)start not count the gap across the restart.
static std::atomic<bool> g_soakOn{false}, g_soakPrevReset{true};
static std::atomic<long long> g_sCount{0}, g_sSumUs{0}, g_sSumSqUs{0}, g_sMaxUs{0};
static int64_t g_sPrev = 0;

// Audio thread: records one block (shared by both callbacks).
// Lock-free and allocation-free (it runs in the real-time callback): only atomics and preallocated arrays. The
// silence memset matters: the driver's output buffers are not guaranteed to be zero, and a test tool that played
// stale buffer content would be loud.
static void RecordBlock(ASIOTime* t, long idx) {
    const int64_t q = Qpc();
    if (g_soakOn.load(std::memory_order_relaxed)) {             // B3: no per-block record, only the sums
        if (g_soakPrevReset.exchange(false)) g_sPrev = 0;
        if (g_sPrev) {
            const long long us = (long long)((q - g_sPrev) * 1000000 / g_qpcFreq);
            g_sCount.fetch_add(1, std::memory_order_relaxed);
            g_sSumUs.fetch_add(us, std::memory_order_relaxed);
            g_sSumSqUs.fetch_add(us * us, std::memory_order_relaxed);
            long long m = g_sMaxUs.load(std::memory_order_relaxed);
            while (us > m && !g_sMaxUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
        }
        g_sPrev = q;
    }
    uint64_t sp = 0, st = 0; bool has = false;
    if (t && (t->timeInfo.flags & kSamplePositionValid)) {
        sp = U64(t->timeInfo.samplePosition.hi, t->timeInfo.samplePosition.lo);
        st = U64(t->timeInfo.systemTime.hi, t->timeInfo.systemTime.lo);
        has = true;
    }
    for (int i = 0; i < g_nBi; ++i)                // silence on every output
        if (!g_bi[i].isInput && g_bi[i].buffers[idx]) memset(g_bi[i].buffers[idx], 0, (size_t)g_sampleBytes[i] * g_bufSize);
    if (g_latMode.load(std::memory_order_relaxed) && has) LatencyStep(idx, sp);
    long n = g_nRec.load(std::memory_order_relaxed);
    if (n < kMaxRec) { g_recs[n] = Rec{q, (int32_t)idx, sp, st, has}; g_nRec.store(n + 1, std::memory_order_release); }
    g_cbCount.fetch_add(1, std::memory_order_release);
}

// Audio thread: plain bufferSwitch callback (used when the driver does not take time info); fetches the position
// with getSamplePosition and calls RecordBlock directly (0.9.6, D9: this comment used to say it forwards to
// CbTimeInfo; it never did). So plain blocks are recorded and counted (g_tiPlain), but their ASIOTime is not
// validated by g_ti.check. This host answers kAsioSupportsTimeInfo with 1, so a driver that still uses the
// plain bufferSwitch shows up as "bufferSwitch (no time info) N" in T2 and T7 and fails T2 (tiC must be > 0).
// The local ASIOTime has no kSampleRateValid flag; that is fine because it is not passed to g_ti.check.
static void CbSwitch(long idx, ASIOBool direct) {
    ASIOTime t; memset(&t, 0, sizeof t);
    ASIOSamples s = {}; ASIOTimeStamp ts = {};
    if (g_cbDrv && g_cbDrv->getSamplePosition(&s, &ts) == ASE_OK) {
        t.timeInfo.samplePosition = s; t.timeInfo.systemTime = ts;
        t.timeInfo.flags = kSamplePositionValid | kSystemTimeValid;
    }
    (void)direct;
    g_tiPlain.fetch_add(1, std::memory_order_relaxed);
    RecordBlock(&t, idx);
}

// sampleRateDidChange callback (driver-initiated rate change); only counted and shown in T7. The current driver
// does not call it (a Windows rate change is signalled with kAsioResetRequest instead, see driver.cpp), so a
// non-zero count would point to a behaviour change.
static void CbRate(ASIOSampleRate) { ++g_rateChanges; }

static_assert(tichk::kSystemTimeValid == kSystemTimeValid && tichk::kSamplePositionValid == kSamplePositionValid &&
              tichk::kSampleRateValid == kSampleRateValid, "tichk flags must match asio.h");

// 0.8.3: starts an ASIOTime phase (main thread, no stream running). The expected rate is set in RunFor from the
// running instance's getSampleRate().
// Phases let the report say in which test an invalid ASIOTime appeared. tichk keeps at most 32 phases (the last
// slot is reused when full); the battery uses about 14.
static int BeginPhase(const std::wstring& name) {
    const int p = g_ti.beginPhase(0.0);
    if ((int)g_phaseNames.size() <= p) g_phaseNames.resize((size_t)p + 1);
    g_phaseNames[(size_t)p] = name;
    return p;
}
// "ASIOTime:" line of one phase: calls and every kind of invalid field, with the first bad rate seen.
static std::wstring TiLine(int p) {
    using namespace tichk;
    std::wstring r = F(L"ASIOTime (%ls): timeInfo calls %ld; null %ld, no samplePosition %ld, no systemTime %ld, bad sampleRate %ld",
                       g_phaseNames[(size_t)p].c_str(), g_ti.calls(p), g_ti.count(p, kNull), g_ti.count(p, kNoPos),
                       g_ti.count(p, kNoTime), g_ti.count(p, kBadRate));
    for (long i = 0; i < g_ti.badStored(); ++i) {
        const Bad& b = g_ti.bad((int)i);
        if (b.phase == p && (b.kinds & (1u << kBadRate))) { r += F(L" (reported %.0f, expected %.0f)", b.rate, b.expected); break; }
    }
    return r;
}

// asioMessage callback (any driver thread). Counts every selector, records the message, and answers the
// capability queries the way a typical host does.
// What the answers mean for the driver: kAsioResetRequest supported (= 1) tells the driver it may ask the host to
// reload it after a device change; this battery only counts the requests (a request during T3 / T7 is a failure,
// because nothing in those tests should invalidate the stream), while soak mode reacts to them by reloading.
// kAsioBufferSizeChange and kAsioResyncRequest are answered 0 (not supported), as many hosts do, so the driver
// must not rely on them. Allocation-free: may run on the audio thread (kAsioOverload).
static long CbMessage(long sel, long val, void*, double*) {
    long n = g_nMsg.load(std::memory_order_relaxed);
    if (n < kMaxMsg) { g_msgs[n] = MsgRec{Qpc(), sel, val}; g_nMsg.store(n + 1, std::memory_order_release); }
    if (sel > 0 && sel < 64) ++g_selCount[sel];
    switch (sel) {
    case kAsioSelectorSupported:
        return (val == kAsioResetRequest || val == kAsioOverload || val == kAsioLatenciesChanged ||
                val == kAsioSupportsTimeInfo || val == kAsioEngineVersion) ? 1 : 0;
    case kAsioEngineVersion:     return 2;
    case kAsioResetRequest:      return 1;   // counted; this test does not reopen the driver
    case kAsioLatenciesChanged:  return 1;
    case kAsioSupportsTimeInfo:  return 1;
    case kAsioOverload:          return 1;
    default:                     return 0;
    }
}

// Field order of ASIOCallbacks: bufferSwitch, sampleRateDidChange, asioMessage, bufferSwitchTimeInfo.
static ASIOCallbacks g_callbacks = {CbSwitch, CbRate, CbMessage, CbTimeInfo};

// Display name of an asioMessage selector.
static const wchar_t* SelName(long s) {
    switch (s) {
    case kAsioSelectorSupported: return L"kAsioSelectorSupported"; case kAsioEngineVersion: return L"kAsioEngineVersion";
    case kAsioResetRequest: return L"kAsioResetRequest"; case kAsioBufferSizeChange: return L"kAsioBufferSizeChange";
    case kAsioResyncRequest: return L"kAsioResyncRequest"; case kAsioLatenciesChanged: return L"kAsioLatenciesChanged";
    case kAsioSupportsTimeInfo: return L"kAsioSupportsTimeInfo"; case kAsioSupportsTimeCode: return L"kAsioSupportsTimeCode";
    case kAsioOverload: return L"kAsioOverload"; default: return L"(other)";
    }
}

// ---------------------------------------------------------------- waiting while pumping messages
static int64_t g_t0 = 0;
static double Ms(int64_t q) { return 1000.0 * (double)(q - g_t0) / (double)g_qpcFreq; }

// Dispatches every pending message of the main thread (re-posting WM_QUIT).
// The driver posts its notifications (reset request, overload, latency change) to a hidden window on the thread
// that called init() and calls asioMessage from there, so this thread must dispatch messages while it waits;
// a host that blocked here (Sleep) would never receive them. Same for the driver panel if it is opened.
static void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); return; }
        TranslateMessage(&m); DispatchMessageW(&m);
    }
}

// Waits ms while pumping messages; optional poll() every ~100 ms. Returns false if there were no callbacks in 3 s
// (when expectCb is true).
// poll() returning true ends the wait early (true is returned and *earlyStop is set). The 3 s "no callbacks"
// check only compares g_cbCount with its value at the start of the wait; one callback is enough to pass it.
template <class Fn> static bool WaitPump(DWORD ms, bool expectCb, Fn poll, bool* earlyStop = nullptr) {
    const int64_t start = Qpc();
    const long cb0 = g_cbCount.load();
    int64_t nextPoll = start;
    for (;;) {
        const int64_t now = Qpc();
        const double el = 1000.0 * (now - start) / g_qpcFreq;
        if (el >= ms) return true;
        if (expectCb && el >= 3000 && g_cbCount.load() == cb0) return false;
        if (now >= nextPoll) { if (poll()) { if (earlyStop) *earlyStop = true; return true; } nextPoll = now + g_qpcFreq / 10; }
        DWORD left = (DWORD)(ms - el); if (left > 100) left = 100;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, left, QS_ALLINPUT);
        Pump();
    }
}
static void WaitPlain(DWORD ms) { WaitPump(ms, false, [] { return false; }); }

// ---------------------------------------------------------------- driver
static HWND g_hostWnd = nullptr;

// Creates the driver COM object (in-process). hr receives the HRESULT. Returns nullptr on failure.
// Must run on the STA main thread (CoInitializeEx APARTMENTTHREADED in wmain): the driver registers with
// ThreadingModel=Apartment, like the hosts it is written for, and all IASIO calls of this tool come from here.
static IASIO* LoadDriver(HRESULT& hr) {
    IASIO* d = nullptr;
    hr = CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, kClsid, (void**)&d);   // ASIO convention: IID = CLSID
    return SUCCEEDED(hr) ? d : nullptr;
}

// Channel data from T1 (and refreshed by T6 / SoakOpen). hasExtras is set when a channel name starts with "2:"
// (with extra devices configured the driver prefixes every channel name with its device number). inNames is
// used by T8 to skip a Loopback input.
struct DrvInfo { bool hasExtras = false; long nIn = 0, nOut = 0, minB = 0, maxB = 0, prefB = 0, gran = 0; long inTypes[32] = {}, outTypes[32] = {}; char inNames[32][32] = {}; };
static DrvInfo g_info;

// Clears the per-run recording state (main thread, while the driver is stopped).
// Must only be called while no stream runs: the audio thread writes these fields while running.
static void ResetSession() {
    g_nRec = 0; g_cbCount = 0;
    g_latMode = false; g_latInit = false; g_latWaiting = false; g_thrSet = false;
    g_latEmitted = 0; g_latMisses = 0; g_nLat = 0;
}

// createBuffers with every input and output channel
// Opens every channel (as DAWs do when all channels are enabled), which is the heaviest case for the driver.
// A driver that returns ASE_OK but leaves a buffer pointer null is treated as a failure (ASE_InvalidParameter)
// and its buffers are disposed again, because a host would crash writing to it.
static ASIOError CreateAll(IASIO* d, long bufSize) {
    g_nBi = 0;
    for (long i = 0; i < g_info.nIn && g_nBi < kMaxBi; ++i) { g_bi[g_nBi] = ASIOBufferInfo{ASIOTrue, i, {nullptr, nullptr}}; g_types[g_nBi] = g_info.inTypes[i]; ++g_nBi; }
    for (long i = 0; i < g_info.nOut && g_nBi < kMaxBi; ++i) { g_bi[g_nBi] = ASIOBufferInfo{ASIOFalse, i, {nullptr, nullptr}}; g_types[g_nBi] = g_info.outTypes[i]; ++g_nBi; }
    for (int i = 0; i < g_nBi; ++i) g_sampleBytes[i] = SampleBytes(g_types[i]);
    g_bufSize = bufSize;
    ASIOError e = d->createBuffers(g_bi, g_nBi, bufSize, &g_callbacks);
    if (e == ASE_OK) for (int i = 0; i < g_nBi; ++i) if (!g_bi[i].buffers[0] || !g_bi[i].buffers[1]) { d->disposeBuffers(); return ASE_InvalidParameter; }
    return e;
}

// Timing statistics of one run, computed from g_recs after stop().
// count = callbacks counted by the audio thread; spanS = time from the first to the last recorded callback;
// expected = number of callbacks that span predicts. badPos = samplePosition jumps; hadTime = every record had a
// valid samplePosition.
struct RunStats {
    long count = 0; double spanS = 0, expected = 0, meanMs = 0, sdMs = 0, maxMs = 0;
    bool idxAlt = true, posStep = true, timeMono = true, hadTime = true; long badPos = 0;
    long nStall = 0;                     // intervals longer than the stall threshold
};

// Stall threshold for an interval between callbacks: max(100 ms, 4 blocks)
// 100 ms is far above any normal interval; with large buffers (4 blocks > 100 ms) the block time sets the limit.
static double StallMs(double rate, long bufSize) { return std::max(100.0, 4000.0 * bufSize / rate); }

// Analyzes the recorded callbacks: count, interval mean/deviation/max, stalls, buffer index alternation, exact
// samplePosition steps and monotonic systemTime.
// expected = span * rate / bufSize + 1: n callbacks span n - 1 intervals. The interval statistics are computed
// over all recorded intervals; the deviation uses E[x^2] - E[x]^2 (clamped at 0 against rounding).
static RunStats Analyze(double rate, long bufSize) {
    RunStats s;
    const double stall = StallMs(rate, bufSize);
    long n = g_nRec.load(std::memory_order_acquire);
    s.count = g_cbCount.load();
    if (n < 2) return s;
    s.spanS = (double)(g_recs[n - 1].qpc - g_recs[0].qpc) / g_qpcFreq;
    s.expected = s.spanS * rate / bufSize + 1.0;
    double sum = 0, sum2 = 0;
    for (long i = 1; i < n; ++i) {
        const Rec& a = g_recs[i - 1]; const Rec& b = g_recs[i];
        double dt = 1000.0 * (double)(b.qpc - a.qpc) / g_qpcFreq;
        sum += dt; sum2 += dt * dt; if (dt > s.maxMs) s.maxMs = dt;
        if (dt > stall) ++s.nStall;
        if (b.idx != 1 - a.idx) s.idxAlt = false;
        if (!a.hasTime || !b.hasTime) s.hadTime = false;
        else {
            if (b.spos != a.spos + (uint64_t)bufSize) { s.posStep = false; ++s.badPos; }
            if (b.stime < a.stime) s.timeMono = false;
        }
    }
    s.meanMs = sum / (n - 1);
    double var = sum2 / (n - 1) - s.meanMs * s.meanMs;
    s.sdMs = var > 0 ? sqrt(var) : 0;
    return s;
}

// Tolerance for the callback count: +-10% + 1 with buffers >= 512 (bursts aligned to the WASAPI period), else +-5% + 1
// Why the wider tolerance: in Shared mode with large ASIO buffers the driver delivers blocks in bursts aligned to
// the WASAPI engine period, so the count over a short run drifts more around the ideal value.
static bool CountOk(const RunStats& s, long bufSize) {
    const double tol = bufSize >= 512 ? 0.10 : 0.05;
    return s.expected > 0 && fabs((double)s.count - s.expected) <= tol * s.expected + 1.0;
}

// start + wait + stop. Returns false if start failed or there were no callbacks (why receives the reason).
// pollPos: also call getSamplePosition every ~100 ms and count errors / backward steps.
// The expected ASIOTime rate of the current phase is taken from this instance's getSampleRate() just before
// start() (0.8.3: comparing with a global rate gave false "bad sampleRate" counts on fresh instances, see tichk.h).
// The 50 ms pump after stop() delivers messages the driver posted during the run, so the per-test counters
// (reset requests, overloads) include them. Pitfall: callers read g_cbCount after RunFor; ResetSession clears it
// at the start of every run.
static bool RunFor(IASIO* d, DWORD ms, std::wstring& why, bool pollPos = false, long* posErr = nullptr, long* posNonMono = nullptr) {
    ResetSession();
    g_cbDrv = d;
    { ASIOSampleRate r = 0; if (d->getSampleRate(&r) == ASE_OK) g_ti.setExpected(r); }   // 0.8.3: this instance's rate
    ASIOError e = d->start();
    if (e != ASE_OK) { why = F(L"start()=%ld", (long)e); return false; }
    uint64_t lastPos = 0; bool first = true;
    auto poll = [&]() {
        if (pollPos) {
            ASIOSamples sp; ASIOTimeStamp ts;
            ASIOError r = d->getSamplePosition(&sp, &ts);
            if (r != ASE_OK) { if (posErr) ++*posErr; }
            else {
                uint64_t p = U64(sp.hi, sp.lo);
                if (!first && p < lastPos && posNonMono) ++*posNonMono;
                lastPos = p; first = false;
            }
        }
        return false;
    };
    bool ok = WaitPump(ms, true, poll);
    d->stop();
    WaitPlain(50);                         // deliver pending driver messages
    if (!ok) { why = L"no callbacks within 3 s"; return false; }
    return true;
}

// One-line summary of RunStats.
static std::wstring StatStr(const RunStats& s) {
    return F(L"callbacks %ld (expected %.0f), interval mean %.2f ms, dev %.2f ms, max %.2f ms",
             s.count, s.expected, s.meanMs, s.sdMs, s.maxMs);
}

// ---------------------------------------------------------------- tests
// The instance shared by T1..T5, T9, T8 and T10 (T10 replaces it). T6 and soak mode use their own instances.
static IASIO* g_drv = nullptr;

// T1: load, init and every information call (channels, buffer sizes, rates, channel info, latencies).
// Pass criteria ("Information"): getChannels OK with at least 1 output and at most 32 inputs / outputs;
// getBufferSize OK with min <= preferred <= max; getSampleRate OK and > 0; getChannelInfo OK for every channel;
// getLatencies OK with input >= 0 and output > 0. canSampleRate results are only printed. Returns false only
// when the driver cannot be created or init() fails (then no other test can run on it).
// g_rate is set here from getSampleRate and is the rate Analyze uses until T5 changes it.
static bool T1() {
    P(L""); P(L"T1 - Driver load and information");
    HRESULT hr; g_drv = LoadDriver(hr);
    if (!g_drv) { AddResult(L"T1", L"Load", false, F(L"CoCreateInstance hr=0x%08lX (driver not registered?)", (unsigned long)hr)); return false; }
    if (!g_drv->init(g_hostWnd)) {
        char em[128] = ""; g_drv->getErrorMessage(em);
        AddResult(L"T1", L"init", false, F(L"init() failed: %hs", em));
        return false;
    }
    char name[64] = ""; g_drv->getDriverName(name);
    long ver = g_drv->getDriverVersion();
    P(L"  Driver: %hs, version %ld", name, ver);
    bool ok = true;
    ASIOError e = g_drv->getChannels(&g_info.nIn, &g_info.nOut);
    P(L"  getChannels=%ld: %ld inputs, %ld outputs", (long)e, g_info.nIn, g_info.nOut);
    if (e != ASE_OK || g_info.nOut < 1 || g_info.nIn > 32 || g_info.nOut > 32) ok = false;
    e = g_drv->getBufferSize(&g_info.minB, &g_info.maxB, &g_info.prefB, &g_info.gran);
    P(L"  getBufferSize=%ld: min %ld, max %ld, preferred %ld, granularity %ld", (long)e, g_info.minB, g_info.maxB, g_info.prefB, g_info.gran);
    if (e != ASE_OK || g_info.prefB < g_info.minB || g_info.prefB > g_info.maxB) ok = false;
    ASIOSampleRate r = 0; e = g_drv->getSampleRate(&r);
    P(L"  getSampleRate=%ld: %.0f Hz", (long)e, r);
    if (e != ASE_OK || r <= 0) ok = false;
    g_rate = r;
    const double rates[] = {44100, 48000, 96000};
    for (double x : rates) P(L"  canSampleRate(%.0f) = %ld", x, (long)g_drv->canSampleRate(x));
    for (int in = 1; in >= 0; --in) {
        long n = in ? g_info.nIn : g_info.nOut;
        for (long c = 0; c < n && c < 32; ++c) {
            ASIOChannelInfo ci = {}; ci.channel = c; ci.isInput = in ? ASIOTrue : ASIOFalse;
            e = g_drv->getChannelInfo(&ci);
            P(L"  %ls %2ld: %-24hs type %ld %ls", in ? L"Input " : L"Output", c, ci.name, (long)ci.type, e == ASE_OK ? L"" : L"(ERROR)");
            if (e != ASE_OK) ok = false;
            if (!strncmp(ci.name, "2:", 2)) g_info.hasExtras = true;
            if (in) { g_info.inTypes[c] = ci.type; memcpy(g_info.inNames[c], ci.name, 31); g_info.inNames[c][31] = 0; }
            else g_info.outTypes[c] = ci.type;
        }
    }
    long il = 0, ol = 0; e = g_drv->getLatencies(&il, &ol);
    P(L"  getLatencies=%ld: input %ld, output %ld frames", (long)e, il, ol);
    if (e != ASE_OK || il < 0 || ol <= 0) ok = false;
    AddResult(L"T1", L"Information", ok, F(L"%hs v%ld, %ld in / %ld out, %.0f Hz, buffer pref %ld", name, ver, g_info.nIn, g_info.nOut, r, g_info.prefB));
    return true;
}

// T11 (0.9.8): bad host calls. A host can be buggy or hostile; the driver must answer with an ASIO error and keep
// working, never crash. Every call below is invalid (null pointers, out-of-range channels, duplicate channels,
// NaN / infinite / negative rates, sizes outside the range, missing callbacks, calls in the wrong order, the
// private engine-info selector without its header, a second instance used before init). No audio is started:
// the only valid createBuffers here is disposed again at once. Pass: every call returns the expected code (or,
// for void calls, returns at all) and afterwards getChannels / getSampleRate / getBufferSize report exactly what
// T1 saw. The later tests (T2..T10) then show that the driver still plays normally.
static void T11() {
    P(L""); P(L"T11 - Bad host calls (invalid arguments and wrong order; no audio)");
    int checks = 0, bad = 0;
    std::wstring failed;
    auto expect = [&](const wchar_t* what, long got, long want) {
        ++checks;
        if (got != want) { ++bad; failed += F(L"%ls=%ld (want %ld); ", what, got, want); P(L"  FAILED %ls: %ld (expected %ld)", what, got, want); }
    };
    IASIO* d = g_drv;
    const double nan = std::nan(""), inf = HUGE_VAL;
    ASIOSampleRate r0 = 0; d->getSampleRate(&r0);
    // null pointers
    expect(L"getChannels(null)", d->getChannels(nullptr, nullptr), ASE_InvalidParameter);
    expect(L"getLatencies(null)", d->getLatencies(nullptr, nullptr), ASE_InvalidParameter);
    expect(L"getBufferSize(null)", d->getBufferSize(nullptr, nullptr, nullptr, nullptr), ASE_InvalidParameter);
    expect(L"getSampleRate(null)", d->getSampleRate(nullptr), ASE_InvalidParameter);
    expect(L"getChannelInfo(null)", d->getChannelInfo(nullptr), ASE_InvalidParameter);
    expect(L"getSamplePosition(null)", d->getSamplePosition(nullptr, nullptr), ASE_InvalidParameter);
    long n0 = 0;
    expect(L"getClockSources(null)", d->getClockSources(nullptr, &n0), ASE_InvalidParameter);
    ASIOClockSource cs[1] = {}; long n1 = 0;
    expect(L"getClockSources(count 0)", d->getClockSources(cs, &n1), ASE_InvalidParameter);
    d->getDriverName(nullptr);                     // void: must simply return
    d->getErrorMessage(nullptr);
    ++checks;
    // channel indices
    ASIOChannelInfo ci = {};
    ci.isInput = ASIOFalse; ci.channel = -1;            expect(L"getChannelInfo(out -1)", d->getChannelInfo(&ci), ASE_InvalidParameter);
    ci.channel = g_info.nOut;                           expect(L"getChannelInfo(out = count)", d->getChannelInfo(&ci), ASE_InvalidParameter);
    ci.channel = 0x7FFFFFFF;                            expect(L"getChannelInfo(out 2^31-1)", d->getChannelInfo(&ci), ASE_InvalidParameter);
    ci.isInput = ASIOTrue; ci.channel = g_info.nIn;     expect(L"getChannelInfo(in = count)", d->getChannelInfo(&ci), ASE_InvalidParameter);
    // sample rates
    expect(L"canSampleRate(NaN)", d->canSampleRate(nan), ASE_NoClock);
    expect(L"canSampleRate(+inf)", d->canSampleRate(inf), ASE_NoClock);
    expect(L"canSampleRate(-48000)", d->canSampleRate(-48000.0), ASE_NoClock);
    expect(L"canSampleRate(1e308)", d->canSampleRate(1e308), ASE_NoClock);
    expect(L"setSampleRate(NaN)", d->setSampleRate(nan), ASE_NoClock);
    expect(L"setSampleRate(-inf)", d->setSampleRate(-inf), ASE_NoClock);
    expect(L"setSampleRate(12345)", d->setSampleRate(12345.0), ASE_NoClock);
    expect(L"setClockSource(7)", d->setClockSource(7), ASE_InvalidMode);
    expect(L"setClockSource(0)", d->setClockSource(0), ASE_OK);
    // wrong order: nothing to start or dispose yet
    expect(L"start() before createBuffers", d->start(), ASE_InvalidMode);
    expect(L"disposeBuffers() without buffers (no-op)", d->disposeBuffers(), ASE_OK);
    expect(L"outputReady()", d->outputReady(), ASE_NotPresent);
    ASIOSamples sp = {}; ASIOTimeStamp ts = {};
    expect(L"getSamplePosition() while stopped", d->getSamplePosition(&sp, &ts), ASE_SPNotAdvancing);
    // createBuffers with bad arguments
    ASIOBufferInfo bi[66] = {};
    bi[0] = ASIOBufferInfo{ASIOFalse, 0, {nullptr, nullptr}};
    bi[1] = ASIOBufferInfo{ASIOFalse, 0, {nullptr, nullptr}};          // duplicate of bi[0]
    const long pb = g_info.prefB;
    ASIOCallbacks noSwitch = {nullptr, CbRate, CbMessage, nullptr};
    expect(L"createBuffers(null infos)", d->createBuffers(nullptr, 1, pb, &g_callbacks), ASE_InvalidParameter);
    expect(L"createBuffers(0 channels)", d->createBuffers(bi, 0, pb, &g_callbacks), ASE_InvalidMode);
    expect(L"createBuffers(-1 channels)", d->createBuffers(bi, -1, pb, &g_callbacks), ASE_InvalidMode);
    expect(L"createBuffers(too many channels)", d->createBuffers(bi, g_info.nIn + g_info.nOut + 1, pb, &g_callbacks), ASE_InvalidParameter);
    expect(L"createBuffers(duplicate channel)", d->createBuffers(bi, 2, pb, &g_callbacks), ASE_InvalidParameter);
    bi[1] = ASIOBufferInfo{ASIOFalse, g_info.nOut, {nullptr, nullptr}};
    expect(L"createBuffers(channel = count)", d->createBuffers(bi, 2, pb, &g_callbacks), ASE_InvalidParameter);
    bi[1] = ASIOBufferInfo{ASIOFalse, -5, {nullptr, nullptr}};
    expect(L"createBuffers(negative channel)", d->createBuffers(bi, 2, pb, &g_callbacks), ASE_InvalidParameter);
    expect(L"createBuffers(size 0)", d->createBuffers(bi, 1, 0, &g_callbacks), ASE_InvalidMode);
    expect(L"createBuffers(size min-1)", d->createBuffers(bi, 1, g_info.minB - 1, &g_callbacks), ASE_InvalidMode);
    expect(L"createBuffers(size max+1)", d->createBuffers(bi, 1, g_info.maxB + 1, &g_callbacks), ASE_InvalidMode);
    expect(L"createBuffers(size -256)", d->createBuffers(bi, 1, -256, &g_callbacks), ASE_InvalidMode);
    expect(L"createBuffers(null callbacks)", d->createBuffers(bi, 1, pb, nullptr), ASE_InvalidMode);
    expect(L"createBuffers(no switch callback)", d->createBuffers(bi, 1, pb, &noSwitch), ASE_InvalidParameter);
    // future() with bad arguments
    char tiny[8] = {};
    expect(L"future(SetInputMonitor, null)", d->future(kAsioSetInputMonitor, nullptr), ASE_InvalidParameter);
    expect(L"future(engine info, char[8])", d->future(WB_FUTURE_ENGINE_INFO, tiny), ASE_InvalidParameter);
    expect(L"future(engine info, null)", d->future(WB_FUTURE_ENGINE_INFO, nullptr), ASE_InvalidParameter);
    expect(L"future(unknown selector)", d->future(0x7FFFABCD, nullptr), ASE_InvalidParameter);
    expect(L"future(CanTimeInfo)", d->future(kAsioCanTimeInfo, nullptr), ASE_SUCCESS);
    // a valid createBuffers twice without disposeBuffers (the second replaces the first), then dispose twice
    bi[0] = ASIOBufferInfo{ASIOFalse, 0, {nullptr, nullptr}};
    expect(L"createBuffers(valid)", d->createBuffers(bi, 1, pb, &g_callbacks), ASE_OK);
    expect(L"createBuffers(valid, again)", d->createBuffers(bi, 1, pb, &g_callbacks), ASE_OK);
    ++checks; if (!bi[0].buffers[0] || !bi[0].buffers[1]) { ++bad; failed += L"buffers not set; "; }
    expect(L"stop() without start", d->stop(), ASE_OK);
    expect(L"disposeBuffers()", d->disposeBuffers(), ASE_OK);
    expect(L"disposeBuffers() again (no-op)", d->disposeBuffers(), ASE_OK);
    // a second instance used before init
    HRESULT hr = S_OK;
    if (IASIO* d2 = LoadDriver(hr)) {
        long a = 0, b = 0;
        expect(L"new instance: getChannels before init", d2->getChannels(&a, &b), ASE_NotPresent);
        expect(L"new instance: start before init", d2->start(), ASE_NotPresent);
        expect(L"new instance: createBuffers before init", d2->createBuffers(bi, 1, pb, &g_callbacks), ASE_NotPresent);
        expect(L"new instance: disposeBuffers before init (no-op)", d2->disposeBuffers(), ASE_OK);
        expect(L"new instance: setClockSource(0) before init", d2->setClockSource(0), ASE_OK);
        expect(L"new instance: QueryInterface(null)", (long)d2->QueryInterface(kClsid, nullptr), (long)E_POINTER);
        d2->Release();
    }
    // the driver still answers as in T1
    long nIn = -1, nOut = -1, mn = 0, mx = 0, pf = 0, gr = 0; ASIOSampleRate r1 = 0;
    expect(L"getChannels afterwards", d->getChannels(&nIn, &nOut), ASE_OK);
    expect(L"getBufferSize afterwards", d->getBufferSize(&mn, &mx, &pf, &gr), ASE_OK);
    expect(L"getSampleRate afterwards", d->getSampleRate(&r1), ASE_OK);
    ++checks;
    if (nIn != g_info.nIn || nOut != g_info.nOut || pf != g_info.prefB || std::fabs(r1 - r0) >= 0.5) { ++bad; failed += L"state changed by the bad calls; "; }
    AddResult(L"T11", L"Bad host calls", bad == 0,
              bad == 0 ? F(L"%d checks: every invalid call returned the expected error; driver unchanged", checks)
                       : F(L"%d of %d checks failed: ", bad, checks) + failed);
}

// T2: 10 s run with every channel at the preferred buffer size; timing, positions and getSamplePosition polling.
// The main stability test, at the configuration a DAW uses by default. Pass: every common criterion (see the top
// of the file) plus: getSamplePosition polled from the main thread every 100 ms never fails and never goes
// backwards (hosts call it from their own threads, e.g. for transport display), at most 3 stalls (1..3 stalls are
// a warning), and at least one bufferSwitchTimeInfo call with no invalid ASIOTime. kAsioOverload is reported but
// does not fail the test (the driver reports it for glitches it detects, which can be caused by the machine).
// The engine line comes from future(WB_FUTURE_ENGINE_INFO) (common.h), a private selector of this driver that
// returns its WASAPI period and rate conversion; it is read while the buffers still exist.
static void T2() {
    P(L""); P(L"T2 - 10 s playback (all channels, preferred buffer)");
    long bs = g_info.prefB;
    ASIOError e = CreateAll(g_drv, bs);
    if (e != ASE_OK) { AddResult(L"T2", L"10 s", false, F(L"createBuffers=%ld", (long)e)); return; }
    long il = 0, ol = 0; g_drv->getLatencies(&il, &ol);
    P(L"  Latencies with buffers: input %ld, output %ld frames", il, ol);
    long ov0 = g_selCount[kAsioOverload].load();
    long posErr = 0, posNonMono = 0; std::wstring why;
    const int ph = BeginPhase(L"T2");
    bool ran = RunFor(g_drv, 10000, why, true, &posErr, &posNonMono);
    long ov = g_selCount[kAsioOverload].load() - ov0;
    // 0.9.4: engine period and rate conversion in use (private selector of this driver; other drivers reject it)
    // 0.9.8: the request carries a header (asiocheck.h EngineInfo). A 0.9.7 or older driver treats opt as a bare
    // char[256] and writes the engine line from its start, over the header, so text[] holds the line without its
    // first 8 characters (harmless: EngineInfo is larger than 256 bytes; use the matching test tool).
    asiochk::EngineInfo ei = {asiochk::kEngineInfoMagic, sizeof(asiochk::EngineInfo), ""};
    const bool hasEng = g_drv->future(WB_FUTURE_ENGINE_INFO, &ei) == ASE_SUCCESS && ei.text[0];
    const char* eng = ei.text;
    g_drv->disposeBuffers();
    if (hasEng) { ei.text[255] = 0; P(L"  Engine: %hs", eng); }
    if (!ran) { AddResult(L"T2", L"10 s", false, why); return; }
    RunStats s = Analyze(g_rate, bs);
    P(L"  %ls", StatStr(s).c_str());
    P(L"  indices alternate: %ls   samplePosition +%ld exact: %ls (%ld jumps)   systemTime monotonic: %ls",
      s.idxAlt ? L"yes" : L"NO", bs, s.posStep ? L"yes" : L"NO", s.badPos, s.timeMono ? L"yes" : L"NO");
    P(L"  getSamplePosition (every 100 ms): errors %ld, backward steps %ld   kAsioOverload: %ld", posErr, posNonMono, ov);
    const long tiC = g_ti.calls(ph), tiBad = g_ti.badCallbacks(ph);
    P(L"  %ls; bufferSwitch (no time info) %ld", TiLine(ph).c_str(), g_tiPlain.load());
    // Intervals > max(100 ms, 4 blocks): up to 3 = warning; more than 3 = failure
    const double stall = StallMs(g_rate, bs);
    if (s.nStall) P(L"  Intervals > %.0f ms: %ld (worst %.1f ms)", stall, s.nStall, s.maxMs);
    bool ok = CountOk(s, bs) && s.idxAlt && s.posStep && s.timeMono && s.hadTime && posErr == 0 && posNonMono == 0 && s.nStall <= 3 &&
              tiC > 0 && tiBad == 0;     // F9: this host supports time info, so the driver must use it with valid fields
    std::wstring d = StatStr(s) + F(L", overload %ld", ov);
    if (hasEng) { d += L"; engine: "; for (const char* c = eng; *c; ++c) d += (wchar_t)(unsigned char)*c; }
    if (tiBad) d += F(L"; ASIOTime invalid %ld", tiBad);
    if (ov) d += L" (NOTE: the driver detected glitches)";
    if (s.nStall > 3) d += F(L"; %ld intervals > %.0f ms (worst %.1f ms)", s.nStall, stall, s.maxMs);
    else if (s.nStall) {
        ++g_warnings;
        d += F(L"; WARNING: %ld interval(s) > %.0f ms (worst %.1f ms)", s.nStall, stall, s.maxMs);
    }
    AddResult(L"T2", L"10 s", ok, d);
}

// T3: 20 cycles of start (1 s) / stop on the same buffers; no reset request may be posted.
// Reproduces hosts that start and stop transport often without recreating buffers. Pass: all 20 cycles produce
// callbacks, the driver posts no kAsioResetRequest (a stop/start must not invalidate the stream) and no invalid
// ASIOTime. minCb shows the weakest cycle (a cycle that starts slowly has few callbacks in 1 s).
static void T3() {
    P(L""); P(L"T3 - 20 start (1 s) / stop cycles");
    ASIOError e = CreateAll(g_drv, g_info.prefB);
    if (e != ASE_OK) { AddResult(L"T3", L"start/stop", false, F(L"createBuffers=%ld", (long)e)); return; }
    long rr0 = g_selCount[kAsioResetRequest].load();
    int okCycles = 0; long minCb = 1 << 30; std::wstring why;
    const int ph = BeginPhase(L"T3");
    for (int i = 0; i < 20; ++i) {
        bool ran = RunFor(g_drv, 1000, why);
        long c = g_cbCount.load();
        if (ran && c > 0) ++okCycles; else P(L"  cycle %d: FAILED (%ls)", i + 1, why.c_str());
        if (c < minCb) minCb = c;
    }
    long rr = g_selCount[kAsioResetRequest].load() - rr0;
    g_drv->disposeBuffers();
    P(L"  %ls", TiLine(ph).c_str());
    AddResult(L"T3", L"start/stop", okCycles == 20 && rr == 0 && g_ti.badCallbacks(ph) == 0,
              F(L"%d/20 cycles with callbacks (min %ld per cycle), reset requests %ld, ASIOTime invalid %ld", okCycles, minCb, rr, g_ti.badCallbacks(ph)));
}

// T4: buffer sizes 64..1024 for 2 s each (sizes outside the driver's range are skipped).
// Each size: createBuffers, 2 s run, disposeBuffers (the sequence a host runs when the user changes the buffer
// size). Pass per size: run OK, callback count within tolerance, index alternation, exact samplePosition steps,
// no invalid ASIOTime. Result: SKIPPED if no size is in range, PASSED if all tested sizes pass, else FAILED.
static void T4() {
    P(L""); P(L"T4 - Buffer sizes 64..1024 (2 s each)");
    const long sizes[] = {64, 128, 256, 512, 1024};
    int pass = 0, total = 0; std::wstring summary;
    for (long bs : sizes) {
        if (bs < g_info.minB || bs > g_info.maxB) { P(L"  %ld: outside the driver range, SKIPPED", bs); summary += F(L"%ld:SKIPPED ", bs); continue; }
        ++total;
        ASIOError e = CreateAll(g_drv, bs);
        if (e != ASE_OK) { P(L"  %ld: createBuffers=%ld FAILED", bs, (long)e); summary += F(L"%ld:FAILED ", bs); continue; }
        std::wstring why;
        const int ph = BeginPhase(F(L"T4 %ld", bs));
        bool ran = RunFor(g_drv, 2000, why);
        g_drv->disposeBuffers();
        RunStats s = Analyze(g_rate, bs);
        bool ok = ran && CountOk(s, bs) && s.idxAlt && s.posStep && g_ti.badCallbacks(ph) == 0;
        P(L"  %4ld: %ls  %ls%ls", bs, ok ? L"PASSED" : L"FAILED", ran ? StatStr(s).c_str() : why.c_str(), s.posStep ? L"" : L" (irregular samplePosition)");
        P(L"        %ls", TiLine(ph).c_str());
        if (ok) ++pass;
        summary += F(L"%ld:%ls ", bs, ok ? L"ok" : L"FAILED");
    }
    AddResultS(L"T4", L"Buffers", total == 0 ? ST_SKIP : pass == total ? ST_PASS : ST_FAIL, F(L"%d/%d  ", pass, total) + summary);
}

// T5: sample rates 44100 / 48000 / 96000 for 3 s each (rates the driver rejects are skipped).
// Per rate: setSampleRate must succeed and getSampleRate must then report it (+-0.5 Hz); then a 3 s run at the
// preferred buffer must pass the count / samplePosition / ASIOTime checks, and the rate the instance reports
// just before start() must be the one set (rateOk). Rates canSampleRate rejects are SKIPPED. Side effect: each
// setSampleRate stores the rate as LastHostRate in config.ini (ratepolicy.h); the restore at the end stores the
// starting rate again.
static void T5() {
    P(L""); P(L"T5 - Sample rates 44100 / 48000 / 96000 (3 s each)");
    const double rate0 = g_rate;           // 0.9.4: restored at the end
    const double rates[] = {44100, 48000, 96000};
    int pass = 0, total = 0; std::wstring summary;
    for (double r : rates) {
        if (g_drv->canSampleRate(r) != ASE_OK) { P(L"  %.0f: canSampleRate rejected, SKIPPED", r); summary += F(L"%.0f:SKIPPED ", r); continue; }
        ++total;
        ASIOError e = g_drv->setSampleRate(r);
        ASIOSampleRate got = 0; g_drv->getSampleRate(&got);
        if (e != ASE_OK || fabs(got - r) > 0.5) { P(L"  %.0f: setSampleRate=%ld, getSampleRate=%.0f FAILED", r, (long)e, got); summary += F(L"%.0f:FAILED ", r); continue; }
        g_rate = r;
        e = CreateAll(g_drv, g_info.prefB);
        if (e != ASE_OK) { P(L"  %.0f: createBuffers=%ld FAILED", r, (long)e); summary += F(L"%.0f:FAILED ", r); continue; }
        std::wstring why;
        const int ph = BeginPhase(F(L"T5 %.0f", r));
        bool ran = RunFor(g_drv, 3000, why);
        g_drv->disposeBuffers();
        RunStats s = Analyze(r, g_info.prefB);
        // 0.8.3: the instance's own rate (expected of this phase) must also be the rate the tester set
        const bool rateOk = fabs(g_ti.expected(ph) - r) <= 0.5;
        bool ok = ran && CountOk(s, g_info.prefB) && s.posStep && g_ti.badCallbacks(ph) == 0 && rateOk;
        P(L"  %6.0f: %ls  %ls", r, ok ? L"PASSED" : L"FAILED", ran ? StatStr(s).c_str() : why.c_str());
        P(L"          %ls%ls", TiLine(ph).c_str(), rateOk ? L"" : F(L" (getSampleRate %.0f before start)", g_ti.expected(ph)).c_str());
        if (ok) ++pass;
        summary += F(L"%.0f:%ls ", r, ok ? L"ok" : L"FAILED");
    }
    // 0.9.4: back to the rate the driver started with (T8 then measures at that rate, e.g. 44.1 kHz with internal
    // conversion); 48000 if it cannot be restored
    if (g_drv->canSampleRate(rate0) == ASE_OK && g_drv->setSampleRate(rate0) == ASE_OK) g_rate = rate0;
    else if (g_drv->canSampleRate(48000) == ASE_OK) { g_drv->setSampleRate(48000); g_rate = 48000; }
    AddResultS(L"T5", L"Sample rates", total == 0 ? ST_SKIP : pass == total ? ST_PASS : ST_FAIL, F(L"%d/%d  ", pass, total) + summary);
}

// T9 (0.5.0): with extra devices configured (channel names "2:..."), 10 s with every channel, checking the
// timing like T2. The per-device counters (drift, underflow/overflow) are internal to the driver:
// they are shown in the panel and in log.txt (stop line).
// Pass: same criteria as T2 except the getSamplePosition polling and the time-info presence check. SKIPPED when
// no channel name starts with "2:".
static void T9() {
    P(L""); P(L"T9 - Extra devices: 10 s with all channels");
    if (!g_info.hasExtras) { AddResultS(L"T9", L"Extra devices", ST_SKIP, L"no extra devices configured"); return; }
    ASIOError e = CreateAll(g_drv, g_info.prefB);
    if (e != ASE_OK) { AddResult(L"T9", L"Extra devices", false, F(L"createBuffers=%ld", (long)e)); return; }
    std::wstring why;
    const int ph = BeginPhase(L"T9");
    bool ran = RunFor(g_drv, 10000, why);
    g_drv->disposeBuffers();
    RunStats s = Analyze(g_rate, g_info.prefB);
    bool ok = ran && CountOk(s, g_info.prefB) && s.idxAlt && s.posStep && s.nStall <= 3 && g_ti.badCallbacks(ph) == 0;
    P(L"  %ls", ran ? StatStr(s).c_str() : why.c_str());
    P(L"  %ls", TiLine(ph).c_str());
    P(L"  Per-device counters: see the panel (Status) or log.txt (\"stop:\" line).");
    AddResult(L"T9", L"Extra devices", ok, ran ? StatStr(s) : why);
}

// Asks a yes/no question on the console (main thread). Returns false when stdin is not a console.
// Unattended runs (redirected stdin) therefore never save a latency offset.
static bool AskYesNo(const std::wstring& q) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(in, &mode)) return false;
    Emit(q);
    wchar_t b[16] = {}; DWORD r = 0;
    if (!ReadConsoleW(in, b, 15, &r, nullptr)) return false;
    return r > 0 && (b[0] == L'y' || b[0] == L'Y');
}

// T8 (optional, --latency): round-trip latency with a cable from output 1 to input 1, compared with the latency
// the driver reports (getLatencies). Plays a loud click every second, so the user is warned first.
// Pass: invalid ASIOTime fails; fewer than 3 detected impulses fails (no cable, input muted or too low); otherwise
// the result is the stability of the measurement: max - min <= max(2, preferred buffer / 8) frames. The
// difference measured - reported is printed and may be saved as an offset, but it does not decide the result.
// SKIPPED when there is no input or input 1 is the driver's Loopback channel (a cable cannot be measured there).
// 0.9.6 (D12, verified): the driver names its loopback inputs exactly "Loopback L" / "Loopback R" and never adds
// the "<n>:" device prefix to them (getChannelInfo and buildChannelNames return those names before any prefix or
// device name is applied), so the old concern that a "1:" prefix could hide them was not real. The check is now an
// exact comparison with "Loopback L" instead of the "Loopback" prefix, so a recording device whose own name
// starts with "Loopback" (e.g. "Loopback Cable In 1") is no longer mistaken for the driver's loopback.
// The Enter prompt only waits when stdin is a console.
static void T8() {
    P(L""); P(L"T8 - Round-trip latency (cable output 1 -> input 1)");
    if (g_info.nIn < 1 || !strcmp(g_info.inNames[0], "Loopback L")) {
        AddResultS(L"T8", L"Latency", ST_SKIP, L"no device input (channel 1 is Loopback or missing)");
        return;
    }
    Emit(L"\r\n  WARNING: a loud click will be played every second on output 1.\r\n"
         L"  Connect a cable from output 1 to input 1, TURN DOWN speakers and headphones,\r\n"
         L"  and press Enter to continue...");
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE); DWORD mode = 0;
    if (GetConsoleMode(in, &mode)) { wchar_t b[8]; DWORD r = 0; ReadConsoleW(in, b, 8, &r, nullptr); }
    ASIOError e = CreateAll(g_drv, g_info.prefB);
    if (e != ASE_OK) { AddResult(L"T8", L"Latency", false, F(L"createBuffers=%ld", (long)e)); return; }
    long il = 0, ol = 0; g_drv->getLatencies(&il, &ol);
    g_latIn = g_latOut = -1;
    for (int i = 0; i < g_nBi; ++i) {
        if (g_bi[i].isInput && g_bi[i].channelNum == 0) g_latIn = i;
        if (!g_bi[i].isInput && g_bi[i].channelNum == 0) g_latOut = i;
    }
    ResetSession();
    g_latMode = true;
    g_cbDrv = g_drv;
    const int ph = BeginPhase(L"T8");
    { ASIOSampleRate r = 0; if (g_drv->getSampleRate(&r) == ASE_OK) g_ti.setExpected(r); }
    e = g_drv->start();
    bool ok = e == ASE_OK;
    if (ok) {
        bool done = false;
        ok = WaitPump(20000, true, [] { return g_nLat.load() >= 10 || g_latEmitted.load() >= 15; }, &done);
        WaitPlain(600);                    // last impulse in flight
        g_drv->stop();
    }
    g_latMode = false;
    g_drv->disposeBuffers();
    int n = g_nLat.load(std::memory_order_acquire);
    P(L"  Noise floor %.5f, threshold %.5f, impulses %d, detected %d, missed %d", g_noise, g_thr, g_latEmitted.load(), n, g_latMisses.load());
    P(L"  %ls", TiLine(ph).c_str());
    if (g_ti.badCallbacks(ph)) { AddResult(L"T8", L"Latency", false, F(L"ASIOTime invalid %ld", g_ti.badCallbacks(ph))); return; }
    if (!ok || n < 3) { AddResult(L"T8", L"Latency", false, F(L"only %d impulses detected (cable? input volume?)", n)); return; }
    std::vector<long> v(g_latRt, g_latRt + n);
    std::sort(v.begin(), v.end());
    long med = v[v.size() / 2], mn = v.front(), mx = v.back();
    long rep = il + ol;
    P(L"  Measured round trip: median %ld (%.2f ms), min %ld, max %ld frames", med, 1000.0 * med / g_rate, mn, mx);
    P(L"  Reported by the driver: input %ld + output %ld = %ld frames (%.2f ms)", il, ol, rep, 1000.0 * rep / g_rate);
    P(L"  Difference (measured - reported): %+ld frames (%+.2f ms)", med - rep, 1000.0 * (med - rep) / g_rate);
    bool stable = mx - mn <= std::max(2L, g_info.prefB / 8);
    AddResult(L"T8", L"Latency", stable, F(L"median %ld frames (%.2f ms), reported %ld, difference %+ld; spread %ld frames%ls",
              med, 1000.0 * med / g_rate, rep, med - rep, mx - mn, stable ? L"" : L" (unstable)"));
    // 0.7.0 (F4): offer to store the difference as a latency offset. Fix 10: the offsets in use are taken out of
    // the reported values first (reported - lround(offset * rate / 1000)), giving the driver's own estimate; the new
    // total offset is then measured - estimate. That is only valid if getLatencies did not clamp (it never reports
    // less than the buffer size): when a side equals the buffer and its offset is negative, the save is skipped.
    // Split: the whole correction goes into the output offset (a round trip cannot tell which side is late; the
    // DAW only needs the sum). Written with the driver's own config writer (LoadConfig / SaveConfig: English
    // comments, temporary file + atomic replace).
    Config c;
    LoadConfig(c);
    const long offIn = lround(c.inLatOffsetMs * g_rate / 1000.0), offOut = lround(c.outLatOffsetMs * g_rate / 1000.0);
    const bool clamped = (il <= g_info.prefB && offIn < 0) || (ol <= g_info.prefB && offOut < 0);
    const long base = (il - offIn) + (ol - offOut);                 // driver estimate without any offset
    const double totalMs = 1000.0 * (med - base) / g_rate;          // offset (in + out) that matches the cable
    const double newOut = totalMs - c.inLatOffsetMs;
    const double diffMs = newOut - c.outLatOffsetMs;
    if (stable && clamped) {
        P(L"  Offset not saved: a reported latency was clamped to the buffer size by a negative offset, so the driver's "
          L"own estimate cannot be recovered. Set the offsets to 0 in the panel and run the test again.");
    } else if (stable && fabs(diffMs) >= 0.05 && AskYesNo(F(L"\r\n  Save %+.2f ms as a latency offset in config.ini? (y/n) ", diffMs))) {
        const double old = c.outLatOffsetMs;
        double v = newOut;
        if (v < -50.0) v = -50.0;
        if (v > 500.0) v = 500.0;
        c.outLatOffsetMs = v;
        SaveConfig(c);
        P(L"  Saved: OutputLatencyOffsetMs %.2f -> %.2f ms (InputLatencyOffsetMs unchanged %.2f ms)%ls. Restart the DAW's audio engine or apply in the panel.",
          old, v, c.inLatOffsetMs, v != newOut ? L" - limited to -50..+500 ms" : L"");
    } else if (stable) {
        P(L"  Offset not saved.");
    }
}

// T10 (0.9.5): the rate set with setSampleRate survives the way hosts react to a driver reset request: Release the
// instance, let COM unload the DLL if it can, CoCreateInstance + init again (what Cubase did when a Bluetooth
// device disappeared). The new instance must report the host's rate, also after createBuffers / disposeBuffers,
// whatever SampleRate says in config.ini. The start rate is set again at the end (this stores it as LastHostRate).
// Why: when a Bluetooth device disappeared, Cubase reacted to kAsioResetRequest by releasing and re-creating the
// driver; the new instance read SampleRate from config.ini and came up at a different rate than the project, so
// the host's rate was lost. Since 0.9.5 the driver keeps the host rate in a per-process named memory block that
// survives the DLL unload (driver.cpp ProcessHostRateSlot) and in config.ini as LastHostRate (ratepolicy.h).
// Steps: pick a target different from the start rate (48000 if the start is 44100, else 44100); setSampleRate;
// Release (g_cbDrv cleared first so no callback pointer dangles); CoFreeUnusedLibrariesEx so the DLL can really
// unload; CoCreateInstance + init; read the rate, then createBuffers / disposeBuffers and read it again (a reload
// of the streams on the new instance must not fall back to config.ini either).
// Pass ("Rate kept"): setSampleRate OK, and the old instance after the set (r1), the new instance (r2) and the
// new instance after createBuffers (r3) all report the target (+-0.5 Hz). SKIPPED if the target rate is not
// accepted. If createBuffers fails, r3 stays r2. Re-creation failure is FAILED and leaves g_drv null.
static void T10() {
    P(L""); P(L"T10 - Host sample rate kept across a re-created driver instance");
    ASIOSampleRate r0 = 0; g_drv->getSampleRate(&r0);
    const double target = fabs(r0 - 44100) < 0.5 ? 48000 : 44100;
    if (g_drv->canSampleRate(target) != ASE_OK) { P(L"  %.0f not accepted, SKIPPED", target); AddResultS(L"T10", L"Rate kept", ST_SKIP, L"rate not accepted"); return; }
    ASIOError e = g_drv->setSampleRate(target);
    ASIOSampleRate r1 = 0; g_drv->getSampleRate(&r1);
    P(L"  start %.0f Hz; setSampleRate(%.0f)=%ld -> getSampleRate %.0f", r0, target, (long)e, r1);
    g_cbDrv = nullptr; g_drv->Release(); g_drv = nullptr;
    WaitPlain(100);
    CoFreeUnusedLibrariesEx(0, 0);                    // the DLL may unload here: the rate must still survive
    WaitPlain(50);
    HRESULT hr; g_drv = LoadDriver(hr);
    if (!g_drv || !g_drv->init(g_hostWnd)) {
        AddResult(L"T10", L"Rate kept", false, F(L"re-creating the driver failed hr=0x%08lX", (unsigned long)hr));
        if (g_drv) { g_drv->Release(); g_drv = nullptr; }
        return;
    }
    ASIOSampleRate r2 = 0; g_drv->getSampleRate(&r2);
    double r3 = r2;
    if (CreateAll(g_drv, g_info.prefB) == ASE_OK) {   // a reopen of the streams on the new instance
        ASIOSampleRate x = 0; g_drv->getSampleRate(&x); r3 = x;
        g_drv->disposeBuffers();
    }
    P(L"  new instance: getSampleRate %.0f Hz, after createBuffers %.0f Hz (expected %.0f)", r2, r3, target);
    const bool ok = e == ASE_OK && fabs(r1 - target) < 0.5 && fabs(r2 - target) < 0.5 && fabs(r3 - target) < 0.5;
    if (fabs(r0 - target) >= 0.5) { g_drv->setSampleRate(r0); g_rate = r0; }
    AddResult(L"T10", L"Rate kept", ok, F(L"set %.0f, re-created instance reports %.0f (after createBuffers %.0f)", target, r2, r3));
}

// T6: 10 full cycles of CoCreateInstance / init / createBuffers / start / stop / dispose / Release; the final
// reference count must be 0 and the DLL should unload (CoFreeUnusedLibraries).
// Reproduces hosts that load and unload the driver repeatedly (driver selection dialogs, project switches,
// reset handling). Pass: all 10 cycles succeed (create, init, createBuffers, 0.5 s with callbacks, dispose) with
// Release() returning 0 (no leaked COM references), and no invalid ASIOTime. "DLL unloaded" is printed only:
// COM may keep a DLL loaded for a while.
// 0.9.6 (D11): the DLL path is read from the registry view that matches this tool's own bitness (64-bit view for
// the x64 build, 32-bit view for an x86 build). CoCreateInstance of an in-process server always loads the DLL
// registered in the caller's own view, so that is exactly the DLL that was loaded and should now be unloaded.
// Before, the 64-bit view was always used, which was only right for a 64-bit tool.
static void T6() {
    P(L""); P(L"T6 - 10 full driver load / unload cycles");
    int okCycles = 0;
    // 0.8.3: each fresh instance starts at its own rate (0.9.5: the remembered host rate, see T10, else config.ini);
    // RunFor takes the expected rate from the instance itself.
    const int ph = BeginPhase(L"T6");
    double instRate = 0;
    for (int i = 0; i < 10; ++i) {
        HRESULT hr; IASIO* d = LoadDriver(hr);
        std::wstring why;
        bool ok = false;
        if (!d) why = F(L"CoCreateInstance hr=0x%08lX", (unsigned long)hr);
        else if (!d->init(g_hostWnd)) why = L"init failed";
        else {
            long ni = 0, no = 0; d->getChannels(&ni, &no);
            if (ni != g_info.nIn || no != g_info.nOut) { g_info.nIn = ni; g_info.nOut = no; }
            ASIOSampleRate ir = 0; if (d->getSampleRate(&ir) == ASE_OK) instRate = ir;
            ASIOError e = CreateAll(d, g_info.prefB);
            if (e != ASE_OK) why = F(L"createBuffers=%ld", (long)e);
            else {
                ok = RunFor(d, 500, why);
                d->disposeBuffers();
            }
        }
        ULONG left = 0;
        if (d) { g_cbDrv = nullptr; left = d->Release(); }
        if (d && left != 0) { ok = false; why += F(L" Release returned %lu", left); }
        WaitPlain(100);
        CoFreeUnusedLibrariesEx(0, 0);
        WaitPlain(50);
        if (ok) ++okCycles; else P(L"  cycle %d: FAILED (%ls)", i + 1, why.c_str());
    }
    std::wstring path; HKEY k;
    std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr + L"\\InprocServer32";
    const REGSAM view = sizeof(void*) == 8 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;   // 0.9.6 (D11): our own bitness
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ | view, &k) == ERROR_SUCCESS) {
        wchar_t b[MAX_PATH]; DWORD sz = sizeof(b) - sizeof(wchar_t), t = 0;
        if (RegQueryValueExW(k, nullptr, nullptr, &t, (BYTE*)b, &sz) == ERROR_SUCCESS) { b[sz / sizeof(wchar_t)] = 0; path = b; }
        RegCloseKey(k);
    }
    const wchar_t* unloaded = path.empty() ? L"?" : (GetModuleHandleW(path.c_str()) ? L"no (still loaded)" : L"yes");
    P(L"  Fresh instances run at %.0f Hz", instRate);
    P(L"  %ls", TiLine(ph).c_str());
    AddResult(L"T6", L"Load/unload", okCycles == 10 && g_ti.badCallbacks(ph) == 0,
              F(L"%d/10 cycles OK; DLL unloaded at the end: %ls; ASIOTime invalid %ld", okCycles, unloaded, g_ti.badCallbacks(ph)));
}

// T7: every asioMessage received during the whole run; passes when no reset request was posted.
// Pass: no kAsioResetRequest during the whole battery (nothing in it changes devices, so a request means the
// driver invalidated its own stream) and no invalid ASIOTime in any phase. Selector-supported queries are only
// counted in the summary, not listed one by one.
static void T7() {
    P(L""); P(L"T7 - asioMessage calls received during the whole run");
    long n = g_nMsg.load(std::memory_order_acquire);
    for (long i = 0; i < n; ++i) {
        const MsgRec& m = g_msgs[i];
        if (m.sel == kAsioSelectorSupported) continue;          // support queries: only in the summary
        P(L"  %10.1f ms  %ls (%ld) value %ld", Ms(m.qpc), SelName(m.sel), m.sel, m.val);
    }
    for (long s = 1; s < 64; ++s) if (long c = g_selCount[s].load()) P(L"  %-24ls %ld", SelName(s), c);
    if (long c = g_rateChanges.load()) P(L"  sampleRateDidChange      %ld", c);
    using namespace tichk;
    const long bad = g_ti.totalAll();
    P(L"  ASIOTime over the whole run: timeInfo calls %ld, plain calls %ld, invalid fields %ld (null %ld, no samplePosition %ld, "
      L"no systemTime %ld, bad sampleRate %ld)", g_ti.totalCalls(), g_tiPlain.load(), bad, g_ti.total(kNull), g_ti.total(kNoPos),
      g_ti.total(kNoTime), g_ti.total(kBadRate));
    for (int ph = 1; ph < g_ti.phases(); ++ph) if (g_ti.badCallbacks(ph)) P(L"    %ls", TiLine(ph).c_str());
    if (g_ti.badSeen()) {
        P(L"  First %ld invalid callbacks (of %ld):", g_ti.badStored(), g_ti.badSeen());
        for (long i = 0; i < g_ti.badStored(); ++i) {
            const Bad& b = g_ti.bad((int)i);
            P(L"    %-8ls callback %6ld  flags 0x%02lX  sampleRate %.0f (expected %.0f)%ls%ls%ls%ls", g_phaseNames[(size_t)b.phase].c_str(),
              b.callback, b.flags, b.rate, b.expected, (b.kinds & (1u << kNull)) ? L" null" : L"",
              (b.kinds & (1u << kNoPos)) ? L" no-position" : L"", (b.kinds & (1u << kNoTime)) ? L" no-time" : L"",
              (b.kinds & (1u << kBadRate)) ? L" bad-rate" : L"");
        }
    }
    long rr = g_selCount[kAsioResetRequest].load();
    AddResult(L"T7", L"Messages", rr == 0 && bad == 0, F(L"reset requests %ld, overload %ld, latencies changed %ld, messages %ld, ASIOTime invalid %ld",
              rr, g_selCount[kAsioOverload].load(), g_selCount[kAsioLatenciesChanged].load(), n, bad));
}

// ---------------------------------------------------------------- console / file
// Console output (WriteConsoleW or UTF-8 when redirected).
static void Emit(const std::wstring& t) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, w = 0;
    if (GetConsoleMode(h, &mode)) {
        for (size_t i = 0; i < t.size(); i += 8192)
            WriteConsoleW(h, t.data() + i, (DWORD)(t.size() - i < 8192 ? t.size() - i : 8192), &w, nullptr);
    } else {
        int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), &u[0], n, nullptr, nullptr);
        WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    }
}

// Writes the report as UTF-8 with BOM. Returns false on failure.
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

// Folder of this executable.
static std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

// 0.9.1 (B3): WasapiDriver-soak.txt in the first writable folder of: exe folder, %APPDATA%\WASAPI Driver for
// ASIO, %TEMP% (decided once, on the first call).
// The probe writes the current report into the chosen file (WriteReport overwrites it each minute anyway).
// %APPDATA%\WASAPI Driver for ASIO is created if missing. An empty result means no folder was writable.
static std::wstring SoakReportPath() {
    static std::wstring chosen; static bool done = false;
    if (done) return chosen;
    done = true;
    wchar_t ad[MAX_PATH] = L"", tmp[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, ad);
    std::wstring dirs[3] = {ExeDir(), ad[0] ? std::wstring(ad) + L"\\" WB_NAME_W : L"", L""};
    if (GetTempPathW(MAX_PATH, tmp)) { dirs[2] = tmp; while (!dirs[2].empty() && (dirs[2].back() == L'\\' || dirs[2].back() == L'/')) dirs[2].pop_back(); }
    for (const std::wstring& d : dirs) {
        if (d.empty()) continue;
        if (d == dirs[1]) CreateDirectoryW(d.c_str(), nullptr);
        const std::wstring p = d + L"\\" WB_FILEBASE_W L"-soak.txt";
        if (WriteReport(p)) { chosen = p; break; }
    }
    return chosen;
}

// Windows version string from RtlGetVersion.
static std::wstring OsVersion() {
    typedef LONG (WINAPI* PFN_RtlGetVersion)(OSVERSIONINFOEXW*);
    OSVERSIONINFOEXW v = {}; v.dwOSVersionInfoSize = sizeof(v);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    PFN_RtlGetVersion fn = nt ? (PFN_RtlGetVersion)(void*)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (fn && fn(&v) == 0) return F(L"Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return L"Windows (unknown version)";
}

static bool SoakHostMsg(HWND h, UINT m, WPARAM w, LPARAM l);   // B3 (below)
// Window procedure of the hidden host window (its HWND is the one passed to init()). WM_POWERBROADCAST must
// return TRUE when handled; everything else not handled by soak mode goes to DefWindowProc.
static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (SoakHostMsg(h, m, w, l)) return m == WM_POWERBROADCAST ? TRUE : 0;
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- soak mode (0.9.1, B3)
// Everything runs on the main thread from a 250 ms timer of the hidden host window, so it keeps working while a
// modal window of the driver (control panel opened from the monitor window or the tray icon) runs its own
// message loop. The driver is reloaded the way a DAW does it (stop, disposeBuffers, Release, CoCreateInstance,
// init, createBuffers, start) after every reset request and after a stall.
// Recovery model: one "open" event at a time (g_s.open = index in ev, -1 = none). An event is recovered when at
// least 10 callbacks arrived after the later of (event start, last successful open); "late" when that took more
// than 120 s. Verdict PASSED needs every event recovered and none late, at least one full minute, and no growth
// trend of private memory (< 64 KB/min and < 8 MB total), handles (< 1/min and < 50) and GDI / USER objects
// (< 1/min and < 50), measured from minute 3 when the run has at least 8 minutes. busy guards SoakTick against
// re-entry: SoakReload pumps messages, which could deliver another WM_TIMER.
enum { WM_SOAK_DEV = WM_APP + 21 };     // posted by the endpoint notification client: wParam = kind
enum { kDevDefault = 1, kDevState, kDevAdded, kDevRemoved };
struct SoakEvent { std::wstring what; double tMs = 0, recoveredMs = -1; bool late = false; };
struct SoakState {
    bool on = false, cycle = false, done = false, busy = false;
    int minutes = 60;
    IASIO* d = nullptr; bool running = false;
    long buf = 256; double rate = 48000.0;
    int64_t tStart = 0, nextMinute = 0, nextRetry = 0, nextCycle = 0, lastCbMove = 0;
    long cbLast = 0, cbAtOpen = 0, cbAtEvent = 0;
    int minute = 0, open = -1, phase = 0;
    long resetSeen = 0, overSeen = 0, tiSeen = 0;
    long reloads = 0, openFailures = 0, cycleIdx = 0;
    long long cbTotal = 0;
    std::vector<SoakEvent> ev;
    std::vector<double> mem, hnd, gdi, usr;   // one sample per minute
    std::vector<long> sizes;                  // --soak-cycle buffer sizes
    long maxGdi = 0, maxUsr = 0;
    bool panelSeen = false, passed = false;
    int64_t lastDevChange = 0;
};
static SoakState g_s;
static std::atomic<bool> g_soakStop{false};
static std::wstring SoakReportPath();
static bool WriteReport(const std::wstring& path);

static double SoakMs() { return 1000.0 * (double)(Qpc() - g_s.tStart) / (double)g_qpcFreq; }
static std::wstring SoakClock(double ms) {
    const long s = (long)(ms / 1000.0);
    return F(L"%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
}

// Endpoint notifications (MMDevice notification thread): only posts to the host window.
// Registered with IMMDeviceEnumerator in RunSoak; refcount starts at 1 (owned by RunSoak, released at the end).
// Only eConsole default changes are posted (one per flow), so the multimedia / communications roles do not
// produce duplicate lines. These notifications are logged only; the recovery is driven by the driver's own
// kAsioResetRequest, as in a real host.
class SoakDevClient final : public IMMNotificationClient {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IMMNotificationClient))) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs; }
    STDMETHODIMP_(ULONG) Release() override { const long r = --refs; if (!r) delete this; return (ULONG)r; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { PostMessageW(g_hostWnd, WM_SOAK_DEV, kDevState, 0); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { PostMessageW(g_hostWnd, WM_SOAK_DEV, kDevAdded, 0); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { PostMessageW(g_hostWnd, WM_SOAK_DEV, kDevRemoved, 0); return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow f, ERole r, LPCWSTR) override {
        if (r == eConsole) PostMessageW(g_hostWnd, WM_SOAK_DEV, kDevDefault, f == eRender ? 0 : 1);
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
private:
    std::atomic<long> refs{1};
};

// Starts an event that needs a recovery (only one is open at a time; later triggers are noted on it).
static void SoakEventStart(const std::wstring& what) {
    const double t = SoakMs();
    if (g_s.open >= 0) { P(L"  [%ls] %ls (during the open event)", SoakClock(t).c_str(), what.c_str()); return; }
    SoakEvent e; e.what = what; e.tMs = t;
    g_s.ev.push_back(e);
    g_s.open = (int)g_s.ev.size() - 1;
    g_s.cbAtEvent = g_cbCount.load();
    P(L"  [%ls] EVENT: %ls", SoakClock(t).c_str(), what.c_str());
}

// Stop, dispose and release the current instance (if any), in host order; g_cbDrv is cleared before Release.
static void SoakClose() {
    if (!g_s.d) return;
    if (g_s.running) g_s.d->stop();
    g_s.running = false;
    g_s.d->disposeBuffers();
    g_cbDrv = nullptr;
    g_s.d->Release();
    g_s.d = nullptr;
}
// Loads, initializes and starts the driver with every channel. False (and the reason logged) on failure.
// Re-reads channels, buffer range and channel types on every open (the device set can change between reloads).
// An invalid requested size falls back to the preferred one. The expected ASIOTime rate becomes this instance's
// rate. Counts failures in g_s.openFailures; the caller schedules a retry.
static bool SoakOpen(long buf) {
    HRESULT hr; IASIO* d = LoadDriver(hr);
    std::wstring why;
    if (!d) why = F(L"CoCreateInstance hr=0x%08lX", (unsigned long)hr);
    else if (!d->init(g_hostWnd)) { char em[128] = ""; d->getErrorMessage(em); why = F(L"init failed: %hs", em); }
    else {
        d->getChannels(&g_info.nIn, &g_info.nOut);
        if (g_info.nIn > 32) g_info.nIn = 32;
        if (g_info.nOut > 32) g_info.nOut = 32;
        d->getBufferSize(&g_info.minB, &g_info.maxB, &g_info.prefB, &g_info.gran);
        for (int in = 1; in >= 0; --in)
            for (long c = 0; c < (in ? g_info.nIn : g_info.nOut); ++c) {
                ASIOChannelInfo ci = {}; ci.channel = c; ci.isInput = in ? ASIOTrue : ASIOFalse;
                d->getChannelInfo(&ci);
                (in ? g_info.inTypes : g_info.outTypes)[c] = ci.type;
            }
        if (buf <= 0 || buf < g_info.minB || buf > g_info.maxB) buf = g_info.prefB;
        ASIOSampleRate r = 0; d->getSampleRate(&r);
        g_s.rate = r > 0 ? r : 48000.0; g_rate = g_s.rate;
        g_ti.setExpected(g_s.rate);
        const ASIOError e = CreateAll(d, buf);
        if (e != ASE_OK) why = F(L"createBuffers(%ld)=%ld", buf, (long)e);
        else {
            g_cbDrv = d;
            g_soakPrevReset = true;
            const ASIOError s = d->start();
            if (s != ASE_OK) { why = F(L"start()=%ld", (long)s); d->disposeBuffers(); g_cbDrv = nullptr; }
            else { g_s.d = d; g_s.running = true; g_s.buf = buf; g_s.cbAtOpen = g_cbCount.load(); g_s.lastCbMove = Qpc(); return true; }
        }
    }
    if (d) d->Release();
    ++g_s.openFailures;
    P(L"  [%ls] driver could not be started: %ls", SoakClock(SoakMs()).c_str(), why.c_str());
    return false;
}
// The DAW reaction to a reset request: close everything, let COM unload the DLL, open again with the same buffer
// size. A failed open is retried by SoakTick every 3 s.
static void SoakReload(const wchar_t* why) {
    P(L"  [%ls] reloading the driver (%ls)", SoakClock(SoakMs()).c_str(), why);
    ++g_s.reloads;
    SoakClose();
    WaitPlain(0);
    CoFreeUnusedLibrariesEx(0, 0);
    if (!SoakOpen(g_s.buf)) g_s.nextRetry = Qpc() + 3 * g_qpcFreq;
}

// Least-squares slope per minute of samples[from..] (0 when there are fewer than 3 points).
static double Slope(const std::vector<double>& v, size_t from) {
    const size_t n = v.size() > from ? v.size() - from : 0;
    if (n < 3) return 0.0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) { const double x = (double)i, y = v[from + i]; sx += x; sy += y; sxx += x * x; sxy += x * y; }
    const double den = n * sxx - sx * sx;
    return den != 0.0 ? (n * sxy - sx * sy) / den : 0.0;
}

// Once a minute: takes (and zeroes) the audio-thread sums, samples process resources and appends one line. The
// counters are process-wide, so they include the driver DLL's allocations, handles and windows (panel, tray).
// The report file is rewritten every minute so a crash keeps the data up to the last full minute.
static void SoakMinute() {
    ++g_s.minute;
    const long long n = g_sCount.exchange(0), sum = g_sSumUs.exchange(0), sq = g_sSumSqUs.exchange(0), mx = g_sMaxUs.exchange(0);
    g_s.cbTotal += n;
    const double mean = n ? (double)sum / (double)n / 1000.0 : 0.0;
    const double var = n ? (double)sq / (double)n / 1e6 - mean * mean : 0.0;
    const long over = g_selCount[kAsioOverload].load(), ti = g_ti.totalAll();
    PROCESS_MEMORY_COUNTERS_EX pm = {}; pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
    const long gdi = (long)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), usr = (long)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (gdi > g_s.maxGdi) g_s.maxGdi = gdi;
    if (usr > g_s.maxUsr) g_s.maxUsr = usr;
    const bool panel = FindWindowW(L"WasapiBridgePanel", nullptr) != nullptr;
    if (panel) g_s.panelSeen = true;
    g_s.mem.push_back((double)pm.PrivateUsage); g_s.hnd.push_back((double)handles);
    g_s.gdi.push_back((double)gdi); g_s.usr.push_back((double)usr);
    const double expect = 60.0 * g_s.rate / (double)g_s.buf;
    P(L"  min %3d: callbacks %lld (expected %.0f), interval mean %.2f ms, dev %.2f ms, max %.2f ms, overloads +%ld, ASIOTime invalid +%ld, "
      L"private %.1f MB, handles %lu, GDI %ld, USER %ld%ls%ls",
      g_s.minute, n, expect, mean, var > 0 ? sqrt(var) : 0.0, (double)mx / 1000.0, over - g_s.overSeen, ti - g_s.tiSeen,
      (double)pm.PrivateUsage / 1048576.0, (unsigned long)handles, gdi, usr, panel ? L" (panel open)" : L"",
      g_s.open >= 0 ? L" (recovering)" : L"");
    g_s.overSeen = over; g_s.tiSeen = ti;
    const std::wstring path = SoakReportPath();
    if (!path.empty()) WriteReport(path);
}

// Closes the driver, prints every event with its recovery time and the leak trends, and sets the verdict.
static void SoakFinish(bool early) {
    SoakClose();
    g_soakOn = false;
    P(L""); P(L"== Soak summary ==");
    P(L"  Duration: %d minutes%ls, %lld callbacks, %ld driver reloads, %ld failed starts", g_s.minute, early ? L" (stopped early)" : L"",
      g_s.cbTotal, g_s.reloads, g_s.openFailures);
    P(L"  Overloads %ld, ASIOTime invalid %ld, peak GDI %ld, peak USER %ld%ls", g_selCount[kAsioOverload].load(), g_ti.totalAll(),
      g_s.maxGdi, g_s.maxUsr, g_s.panelSeen ? L" (panel was open at times)" : L"");
    bool ok = true;
    int recovered = 0;
    for (const SoakEvent& e : g_s.ev) {
        if (e.recoveredMs < 0) { ok = false; P(L"  Event at %ls: %ls - NOT RECOVERED", SoakClock(e.tMs).c_str(), e.what.c_str()); }
        else {
            ++recovered;
            if (e.late) ok = false;
            P(L"  Event at %ls: %ls - recovered in %.1f s%ls", SoakClock(e.tMs).c_str(), e.what.c_str(), (e.recoveredMs - e.tMs) / 1000.0,
              e.late ? L" (over the 120 s limit)" : L"");
        }
    }
    if (g_s.ev.empty()) P(L"  No device events (unplug/replug the device, change the default device or sleep/resume to test recovery).");
    // Leak trend: from minute 3 on (start-up allocations excluded) when the run is long enough.
    const size_t from = g_s.mem.size() >= 8 ? 2 : 0;
    const double ms = Slope(g_s.mem, from), hs = Slope(g_s.hnd, from), gs = Slope(g_s.gdi, from), us = Slope(g_s.usr, from);
    const double mg = g_s.mem.size() > from ? g_s.mem.back() - g_s.mem[from] : 0, hg = g_s.hnd.size() > from ? g_s.hnd.back() - g_s.hnd[from] : 0;
    const double gg = g_s.gdi.size() > from ? g_s.gdi.back() - g_s.gdi[from] : 0, ug = g_s.usr.size() > from ? g_s.usr.back() - g_s.usr[from] : 0;
    const bool memOk = ms < 64.0 * 1024 && mg < 8.0 * 1048576, hndOk = hs < 1.0 && hg < 50, guiOk = gs < 1.0 && us < 1.0 && gg < 50 && ug < 50;
    P(L"  Memory: %+.1f KB/min (growth %+.2f MB, limit 64 KB/min and 8 MB) %ls", ms / 1024.0, mg / 1048576.0, memOk ? L"OK" : L"LEAK TREND");
    P(L"  Handles: %+.2f/min (growth %+.0f, limit 1/min and 50) %ls", hs, hg, hndOk ? L"OK" : L"LEAK TREND");
    P(L"  GDI/USER: %+.2f / %+.2f per min (growth %+.0f / %+.0f, limit 1/min and 50) %ls", gs, us, gg, ug, guiOk ? L"OK" : L"LEAK TREND");
    if (g_s.mem.size() < 8) P(L"  Note: runs shorter than 8 minutes give a weak leak trend; 60 minutes is the reference.");
    ok = ok && memOk && hndOk && guiOk && g_s.minute > 0;
    if (g_selCount[kAsioOverload].load() || g_ti.totalAll()) P(L"  Warning: overloads or invalid ASIOTime were seen (see the per-minute lines).");
    P(L"RESULT: %ls", ok ? L"PASSED" : L"FAILED");
    g_s.passed = ok;
    g_s.done = true;
}

// Runs every 250 ms on the main thread (WM_TIMER 1 of the host window). Order of checks as numbered below.
static void SoakTick() {
    if (g_s.busy || g_s.done) return;
    g_s.busy = true;
    const int64_t now = Qpc();
    const long cb = g_cbCount.load();
    if (cb != g_s.cbLast) { g_s.cbLast = cb; g_s.lastCbMove = now; }
    // 1. reset requests of the driver (device unplugged, default device changed, stream invalidated, ...)
    const long rr = g_selCount[kAsioResetRequest].load();
    if (rr != g_s.resetSeen) {
        g_s.resetSeen = rr;
        SoakEventStart(L"kAsioResetRequest from the driver");
        SoakReload(L"reset request");
    }
    // 2. stall: no callback for 2 s while running; reload after 5 s
    const double still = 1000.0 * (double)(now - g_s.lastCbMove) / (double)g_qpcFreq;
    if (g_s.running && still > 2000 && g_s.open < 0) SoakEventStart(L"stall: no callbacks for 2 s");
    if (g_s.running && still > 5000 && g_s.open >= 0 && g_s.d) SoakReload(L"stall");
    // 3. driver not running (start failed): retry every 3 s
    if (!g_s.d && now >= g_s.nextRetry) { if (!SoakOpen(g_s.buf)) g_s.nextRetry = now + 3 * g_qpcFreq; }
    // 4. recovery of the open event: callbacks flowing again (10 blocks) after the event / the last start
    if (g_s.open >= 0) {
        SoakEvent& e = g_s.ev[(size_t)g_s.open];
        const long base = g_s.cbAtOpen > g_s.cbAtEvent ? g_s.cbAtOpen : g_s.cbAtEvent;
        if (g_s.running && cb - base >= 10) {
            e.recoveredMs = SoakMs();
            e.late = e.recoveredMs - e.tMs > 120000.0;
            P(L"  [%ls] recovered after %.1f s", SoakClock(e.recoveredMs).c_str(), (e.recoveredMs - e.tMs) / 1000.0);
            g_s.open = -1;
        }
    }
    // 5. --soak-cycle: planned buffer size change every 5 minutes (not an event; a failure becomes one)
    if (g_s.cycle && g_s.open < 0 && g_s.d && now >= g_s.nextCycle && !g_s.sizes.empty()) {
        g_s.nextCycle = now + 300 * g_qpcFreq;
        g_s.cycleIdx = (g_s.cycleIdx + 1) % (long)g_s.sizes.size();
        const long nb = g_s.sizes[(size_t)g_s.cycleIdx];
        P(L"  [%ls] buffer size %ld -> %ld", SoakClock(SoakMs()).c_str(), g_s.buf, nb);
        g_s.d->stop(); g_s.running = false; g_s.d->disposeBuffers();
        g_soakPrevReset = true;
        if (CreateAll(g_s.d, nb) == ASE_OK && g_s.d->start() == ASE_OK) { g_s.running = true; g_s.buf = nb; g_s.cbAtOpen = g_cbCount.load(); g_s.lastCbMove = Qpc(); }
        else { SoakEventStart(F(L"buffer size change to %ld failed", nb)); g_s.buf = nb; SoakReload(L"buffer change failed"); }
    }
    // 6. once a minute: statistics; 7. the end
    if (now >= g_s.nextMinute) { g_s.nextMinute += 60 * g_qpcFreq; SoakMinute(); }
    if (g_soakStop.load()) SoakFinish(true);
    else if (g_s.minute >= g_s.minutes) SoakFinish(false);
    g_s.busy = false;
}

// Returns true when the message was handled for soak mode (only while g_s.on). WM_DEVICECHANGE is logged but
// not consumed, so DefWindowProc still sees it.
static bool SoakHostMsg(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_s.on) return false;
    switch (m) {
    case WM_TIMER: if (w == 1) { SoakTick(); return true; } break;
    case WM_POWERBROADCAST:
        if (w == PBT_APMSUSPEND) { P(L"  [%ls] system going to sleep", SoakClock(SoakMs()).c_str()); return true; }
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
            if (g_s.open < 0) SoakEventStart(L"resume from sleep");
            g_s.lastCbMove = Qpc();                    // the stall timer starts again after the resume
            return true;
        }
        break;
    case WM_DEVICECHANGE:                              // at most one line per 5 s (Bluetooth can be chatty)
        if (Qpc() - g_s.lastDevChange > 5 * g_qpcFreq) {
            g_s.lastDevChange = Qpc();
            P(L"  [%ls] WM_DEVICECHANGE 0x%04lX (devices changed)", SoakClock(SoakMs()).c_str(), (unsigned long)w);
        }
        return false;
    case WM_SOAK_DEV: {
        static const wchar_t* what[] = {L"", L"default device changed", L"device state changed (unplug/replug, enable/disable)",
                                        L"device added", L"device removed"};
        P(L"  [%ls] %ls%ls", SoakClock(SoakMs()).c_str(), what[w <= 4 ? w : 0], w == kDevDefault ? (l ? L" (input)" : L" (output)") : L"");
        return true;
    }
    }
    (void)h;
    return false;
}

static BOOL WINAPI SoakCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) { g_soakStop = true; return TRUE; }
    return FALSE;
}

// Runs the soak mode. Exit code: 0 = PASSED, 1 = FAILED.
// --soak-cycle sizes: the opened size, x2, /2 and x4, restricted to the driver range and to 32..4096. The message
// loop runs until SoakFinish sets done (or WM_QUIT).
static int RunSoak(int minutes, bool cycle) {
    g_s.on = true; g_s.minutes = minutes; g_s.cycle = cycle;
    P(L"Soak test: %d minutes%ls. Leave it running; unplug/replug the device, change the default device or let the computer",
      minutes, cycle ? L", buffer size change every 5 minutes" : L"");
    P(L"sleep and wake it to test recovery. Ctrl+C ends the run early (the verdict is still written).");
    SetConsoleCtrlHandler(SoakCtrl, TRUE);
    IMMDeviceEnumerator* en = nullptr;
    SoakDevClient* dc = new SoakDevClient;
    static const CLSID kEnum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
    if (SUCCEEDED(CoCreateInstance(kEnum, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) && en)
        en->RegisterEndpointNotificationCallback(dc);
    g_s.phase = BeginPhase(L"soak");
    g_soakOn = true;
    g_s.tStart = Qpc();
    g_s.nextMinute = g_s.tStart + 60 * g_qpcFreq;
    g_s.nextCycle = g_s.tStart + 300 * g_qpcFreq;
    g_s.lastCbMove = g_s.tStart;
    if (!SoakOpen(0)) g_s.nextRetry = Qpc() + 3 * g_qpcFreq;
    else {
        P(L"  Started: %.0f Hz, buffer %ld, %ld inputs, %ld outputs", g_s.rate, g_s.buf, g_info.nIn, g_info.nOut);
        if (cycle) {
            const long cand[] = {g_s.buf, g_s.buf * 2, g_s.buf / 2, g_s.buf * 4};
            for (long c : cand) if (c >= g_info.minB && c <= g_info.maxB && c >= 32 && c <= 4096) g_s.sizes.push_back(c);
        }
    }
    SetTimer(g_hostWnd, 1, 250, nullptr);
    MSG m;
    while (!g_s.done && GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (!g_s.done) SoakFinish(true);                   // WM_QUIT
    KillTimer(g_hostWnd, 1);
    if (en) { en->UnregisterEndpointNotificationCallback(dc); en->Release(); }
    dc->Release();
    SetConsoleCtrlHandler(SoakCtrl, FALSE);
    return g_s.passed ? 0 : 1;
}

// Runs every test in order and writes the report. Exit code: 0 = no failures, 1 = at least one failure.
// Arguments (case-insensitive): --latency adds T8; --soak [minutes] (1..1440, default 60) runs soak mode instead
// of the battery; --soak-cycle implies --soak and adds the buffer size changes. The window class name keeps the
// old project name (WasapiBridgeTestHost); it is internal and not visible to the user.
int wmain(int argc, wchar_t** argv) {
    ConsoleIcon conIcon;                 // 0.9.3: application icon on the console window while running
    bool latency = false, soak = false, soakCycle = false;
    int soakMin = 60;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--latency")) latency = true;
        else if (!_wcsicmp(argv[i], L"--soak-cycle")) { soak = true; soakCycle = true; }
        else if (!_wcsicmp(argv[i], L"--soak")) {
            soak = true;
            if (i + 1 < argc && iswdigit(argv[i + 1][0])) { soakMin = _wtoi(argv[++i]); if (soakMin < 1) soakMin = 1; if (soakMin > 24 * 60) soakMin = 24 * 60; }
        }
    }
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf); g_qpcFreq = qf.QuadPart;
    g_t0 = Qpc();
    for (auto& c : g_selCount) c = 0;
    SYSTEMTIME t; GetSystemTime(&t);
    P(L"%ls - Automatic ASIO host test %ls (expected driver %ls)", WB_NAME_W, kToolVersion, WB_VERSION_W);
    P(L"Date (UTC): %04u-%02u-%02u %02u:%02u   %ls", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, OsVersion().c_str());
    P(L"ASIO is a registered trademark of Steinberg Media Technologies GmbH.");
    P(L"Mode: %ls. Uses the current driver settings (panel / config.ini).", soak ? L"soak" : latency ? L"with latency test (T8)" : L"normal");
    P(L"Close the DAW before running this (in Exclusive mode the device would be busy). Outputs send silence.");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc = {}; wc.lpfnWndProc = HostProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"WasapiBridgeTestHost";
    RegisterClassW(&wc);
    g_hostWnd = CreateWindowExW(0, wc.lpszClassName, WB_NAME_W L" test", WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    if (soak) {                                        // 0.9.1 (B3)
        const int rc = RunSoak(soakMin, soakCycle);
        if (g_hostWnd) DestroyWindow(g_hostWnd);
        if (SUCCEEDED(hr)) CoUninitialize();
        const std::wstring path = SoakReportPath();
        if (!path.empty() && WriteReport(path)) Emit(L"\r\nReport saved to:\r\n  " + SanitizeText(path) + L"\r\n");
        else Emit(L"\r\nCould not save the report; copy the text from this window.\r\n");
        DWORD mode = 0;
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        if (GetConsoleMode(in, &mode)) { Emit(L"\r\nPress Enter to exit..."); wchar_t b[8]; DWORD r = 0; ReadConsoleW(in, b, 8, &r, nullptr); }
        return rc;
    }

    if (T1()) {
        T11();                                         // 0.9.8: bad host calls first; every later test proves the driver survived
        T2(); T3(); T4(); T5(); T9();
        if (latency) T8();
        T10();                                         // 0.9.5: last, it replaces the instance
    }
    if (g_drv) { g_cbDrv = nullptr; g_drv->Release(); g_drv = nullptr; }
    WaitPlain(100);
    if (!g_results.empty() && g_results[0].st == ST_PASS) T6();
    T7();

    int fails = 0;
    P(L""); P(L"== Summary ==");
    for (const Result& r : g_results) {
        P(L"  %-3ls %-16ls %-7ls %ls", r.id.c_str(), r.name.c_str(), StName(r.st), r.detail.c_str());
        if (r.st == ST_FAIL) ++fails;
    }
    if (fails) P(L"RESULT: FAILURES = %d", fails);
    else if (g_warnings) P(L"RESULT: PASSED, %d warnings", g_warnings);
    else P(L"RESULT: PASSED");

    if (g_hostWnd) DestroyWindow(g_hostWnd);
    if (SUCCEEDED(hr)) CoUninitialize();

    const wchar_t* name = L"\\" WB_FILEBASE_W L"-test.txt";
    wchar_t ad[MAX_PATH] = L"", tmp[MAX_PATH] = L"";
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, ad);
    std::wstring dirs[3] = {ExeDir(), ad[0] ? std::wstring(ad) + L"\\" WB_NAME_W : L"", L""};
    if (GetTempPathW(MAX_PATH, tmp)) { dirs[2] = tmp; while (!dirs[2].empty() && (dirs[2].back() == L'\\' || dirs[2].back() == L'/')) dirs[2].pop_back(); }
    std::wstring done;
    for (const std::wstring& d : dirs) {
        if (d.empty()) continue;
        if (d == dirs[1]) CreateDirectoryW(d.c_str(), nullptr);
        if (WriteReport(d + name)) { done = d + name; break; }
    }
    if (!done.empty()) Emit(L"\r\nReport saved to:\r\n  " + SanitizeText(done) + L"\r\n");
    else Emit(L"\r\nCould not save the report; copy the text from this window.\r\n");

    DWORD mode = 0;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(in, &mode)) {
        Emit(L"\r\nPress Enter to exit...");
        wchar_t b[8]; DWORD r = 0;
        ReadConsoleW(in, b, 8, &r, nullptr);
    }
    return fails ? 1 : 0;
}
