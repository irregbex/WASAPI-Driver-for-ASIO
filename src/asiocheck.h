// WASAPI Driver for ASIO - validation of everything an ASIO host passes to the driver (pure, header-only)
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
//   The driver runs inside the host's process: a buggy or malicious host must get an error code back, never a
//   crash, an out-of-bounds write or a hang. Every IASIO argument check of driver.cpp is expressed here as a pure
//   function with no Windows or ASIO SDK dependency, so tests/asiocheck_test.cpp can drive it on Linux with
//   garbage (random channel lists, NaN / infinite / negative rates, wrong call order, null pointers) under
//   AddressSanitizer and UndefinedBehaviorSanitizer. driver.cpp maps the results 1:1 to ASIOError (the numeric
//   values below are those of asio.h, checked with static_assert there).
//
// Rules (see ARCHITECTURE.txt section 58 for the reasoning)
//   - State machine: Created -> init -> Initialized -> createBuffers -> Prepared -> start -> Running; stop goes back
//     to Prepared, disposeBuffers to Initialized. A call that needs a later state gets kNotPresent (before init) or
//     kInvalidMode (before createBuffers / start). Calls that are harmless in any state (stop, disposeBuffers,
//     setClockSource, getDriverName, ...) are always accepted; disposeBuffers and setClockSource(0) answer ASE_OK in
//     every state, as 0.9.7 did (independent review R2: hosts that dispose defensively must not see an error).
//   - createBuffers: callbacks must exist and provide bufferSwitch or bufferSwitchTimeInfo; numChannels in
//     1..nIn+nOut; each entry's channel index in range for its direction (isInput: any non-zero value is an input,
//     as ASIOBool is a long); no channel requested twice; bufferSize within [min, max]. Sizes that are not a power
//     of two are accepted on purpose: getBufferSize reports the preferred size from config.ini, which may be any
//     value from 32 to 4096, and the engine handles any size (see CODE-REVIEW.txt H-list, "granularity").
//     The checked entries are COPIED by the caller (CopyInfos) and only the copy is used afterwards, so a host that
//     changes the array while (or after) it is validated cannot slip an out-of-range index past the check (TOCTOU).
//   - Sample rates: NaN, infinite, negative and non-standard rates are refused (kNoClock); 0 means "no change" for
//     setSampleRate (ASIO convention), but is not a rate canSampleRate accepts.
//
// Threading
//   Pure functions over their arguments.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace asiochk {

// Values of asio.h (ASIOError).
enum Err : long {
    kOk = 0, kNotPresent = -1000, kHWMalfunction = -999, kInvalidParameter = -998, kInvalidMode = -997,
    kSPNotAdvancing = -996, kNoClock = -995, kNoMemory = -994
};

constexpr int  kMaxCh = 32;          // = kMaxCh in driver.h (ASIO channels per direction)
constexpr long kMinBuffer = 32;      // getBufferSize minimum / maximum
constexpr long kMaxBuffer = 4096;

// ---------------------------------------------------------------- state machine
enum class State { Created, Initialized, Prepared, Running };
inline State StateOf(bool initialized, bool buffersCreated, bool running) {
    if (!initialized) return State::Created;
    if (!buffersCreated) return State::Initialized;
    return running ? State::Running : State::Prepared;
}
enum class Call {
    GetChannels, GetLatencies, GetBufferSize, CanSampleRate, GetSampleRate, SetSampleRate, GetClockSources,
    SetClockSource, GetSamplePosition, GetChannelInfo, CreateBuffers, DisposeBuffers, Start, Stop, ControlPanel,
    Future, OutputReady
};
// Whether `c` may run in state `s`. kOk = proceed (the method may still refuse its arguments).
inline Err Allowed(Call c, State s) {
    switch (c) {
    case Call::Stop: case Call::ControlPanel: case Call::Future: case Call::GetBufferSize:
        return kOk;                                   // harmless in any state (getBufferSize reports the defaults)
    case Call::OutputReady:
        return kNotPresent;                           // not supported (the driver never asks for it)
    case Call::Start:
        if (s == State::Created) return kNotPresent;
        return s == State::Initialized ? kInvalidMode : kOk;   // Running: no-op
    case Call::DisposeBuffers:
        return kOk;                                   // review R2: lenient like 0.9.7 (no-op without buffers)
    case Call::GetSamplePosition:
        if (s == State::Created) return kNotPresent;
        return s == State::Running ? kOk : kSPNotAdvancing;
    default:
        return s == State::Created ? kNotPresent : kOk;
    }
}

// ---------------------------------------------------------------- sample rates
// The rates the driver advertises (44.1 k .. 192 k, +-0.5 Hz). NaN and infinities compare false and are refused.
inline bool StandardRate(double r) {
    static const double rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    if (!std::isfinite(r)) return false;
    for (double x : rates) if (std::fabs(x - r) < 0.5) return true;
    return false;
}
inline Err CheckCanSampleRate(double r) { return StandardRate(r) ? kOk : kNoClock; }
// setSampleRate: 0 = keep the current rate (kOk, nothing to do; `noChange` set).
inline Err CheckSetSampleRate(double r, bool& noChange) {
    noChange = r == 0.0;
    if (noChange) return kOk;
    return StandardRate(r) ? kOk : kNoClock;
}

// ---------------------------------------------------------------- buffers
inline Err CheckBufferSize(long size) { return size >= kMinBuffer && size <= kMaxBuffer ? kOk : kInvalidMode; }

// Callbacks: presence of each member (the caller copies the structure first and passes the copy's members).
inline Err CheckCallbacks(bool present, bool hasBufferSwitch, bool hasTimeInfo) {
    if (!present) return kInvalidMode;               // asio.h: no callbacks = the host cannot run
    return hasBufferSwitch || hasTimeInfo ? kOk : kInvalidParameter;
}

// A validated channel request (the copy the driver works with).
struct Chan { bool input; int ch; };

// Validates and copies `infos[0..num)` (any type with isInput and channelNum members, i.e. ASIOBufferInfo) into
// `out` (capacity 2 * kMaxCh). Each host field is read exactly once. nIn / nOut: channels the driver has.
template <class Info>
inline Err CopyInfos(const Info* infos, long num, long nIn, long nOut, Chan* out, size_t outCap) {
    if (num < 1) return kInvalidMode;
    if (!infos || !out) return kInvalidParameter;
    if (nIn < 0) nIn = 0;
    if (nOut < 0) nOut = 0;
    if (nIn > kMaxCh) nIn = kMaxCh;
    if (nOut > kMaxCh) nOut = kMaxCh;
    if (num > nIn + nOut || (size_t)num > outCap) return kInvalidParameter;
    bool seenIn[kMaxCh] = {}, seenOut[kMaxCh] = {};
    for (long i = 0; i < num; ++i) {
        const long isIn = (long)infos[i].isInput;     // one read of each field
        const long chl = (long)infos[i].channelNum;
        const bool input = isIn != 0;
        const long n = input ? nIn : nOut;
        if (chl < 0 || chl >= n) return kInvalidParameter;
        bool& seen = input ? seenIn[chl] : seenOut[chl];
        if (seen) return kInvalidParameter;           // the same channel twice
        seen = true;
        out[i].input = input; out[i].ch = (int)chl;
    }
    return kOk;
}

// getChannelInfo: channel index of the requested direction.
inline Err CheckChannelIndex(long channel, bool input, long nIn, long nOut) {
    const long n = input ? nIn : nOut;
    return channel >= 0 && channel < n && channel < kMaxCh ? kOk : kInvalidParameter;
}

// getClockSources: the array must hold at least one entry.
inline Err CheckClockSources(bool clocksPresent, bool countPresent, long capacity) {
    return clocksPresent && countPresent && capacity >= 1 ? kOk : kInvalidParameter;
}

// ---------------------------------------------------------------- private future() selector of the test tool
// 0.9.8: the engine-information request carries a header, so a host that happens to pass the selector value with
// another kind of `opt` can never make the driver write 256 bytes into a smaller buffer. Before 0.9.8 `opt` was a
// bare char[256].
constexpr long kEngineInfoSelector = 0x57424549L;     // 'WBEI' = WB_FUTURE_ENGINE_INFO (common.h)
constexpr uint32_t kEngineInfoMagic = 0x49454257u;    // "WBEI" in memory order
struct EngineInfo {
    uint32_t magic;       // kEngineInfoMagic
    uint32_t size;        // sizeof(EngineInfo)
    char     text[256];   // receives the engine line, NUL-terminated
};
inline Err CheckEngineInfo(const EngineInfo* e) {
    if (!e) return kInvalidParameter;
    return e->magic == kEngineInfoMagic && e->size == sizeof(EngineInfo) ? kOk : kInvalidParameter;
}

}  // namespace asiochk
