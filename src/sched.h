// WASAPI Driver for ASIO - timing model of the audio thread (0.9.4, header-only, portable)
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
//   The pieces of the audio thread's timing that the driver and tests/fixedratio_test.cpp share:
//     BlockClock    the systemTime of each ASIO block (linear in samplePosition, re-anchored to the device
//                   wake-ups with a 10 s time constant);
//     OutLatency,   the latencies reported by getLatencies for a Shared stream whose ASIO rate differs from the
//     InDelay,      device rate (internal fixed-ratio conversion, fixedratio.h), and the fixed delay with which a
//     InLatency     converted input on the output's clock is delivered (exact round trip);
//     Sim           a model of run() for one output device and one input on the same clock (device wake-ups,
//                   on-demand ASIO callbacks, both converters, the input FIFO), used offline to check the
//                   accounting, the callback timing and that the measured latencies equal the reported ones.
//   Driver: BlockClock and the latency functions only (audio thread / host thread); no allocation in BlockClock.
//   Sim allocates (std::vector, converters) and exists only for the offline tests; the driver never uses it.
//
// Units (pitfall: three clocks are involved)
//   "ASIO frames" are at the host's rate (asioRate), "device frames" at the device mix rate (devRate / inRate /
//   outRate). Converting a count between them multiplies by asioRate / devRate; the functions round up (ceil)
//   where a shortfall would cause an underrun and round to nearest (lround) where the value is only reported.
//   Times are ns in BlockClock and seconds elsewhere.
//
// Why on-demand callbacks matter for the latency math
//   With conversion, the output thread wakes at device periods and runs as many ASIO blocks as the output
//   converter needs (fixedratio.h missingFor). So ASIO blocks are not evenly spaced in time; the reported
//   latencies are derived from the stream structure (block + look-ahead + device queue) instead of a measured
//   time, and Sim verifies that the derived round trip equals the measured one.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "fixedratio.h"

namespace sched {

// systemTime (ns) of the block starting at samplePosition `pos`. wakeNs: time of the current device wake-up;
// firstOfWake: first block computed in this wake-up (only it corrects the anchor). nsPerFrame at the ASIO rate.
// jumpNs: deviation beyond which the anchor jumps instead of sliding (the driver uses 3 x the device queue,
// at least 50 ms). wakeDtSec: nominal time between wake-ups (device period / device rate), the dt of the filter.
// Model: ts = t0 + pos * nsPerFrame, i.e. timestamps advance exactly with samplePosition (what hosts expect for
// ASIOTime.systemTime); t0 tracks the wake-up clock with a first-order low-pass (alpha = 1 - exp(-dt / 10 s)),
// which absorbs scheduling jitter and the ppm difference between the sound card and the system clock.
// The first stamp after reset() anchors t0 directly. State is two plain fields: audio thread only.
struct BlockClock {
    double t0 = 0.0;
    bool set = false;
    void reset() { set = false; t0 = 0.0; }
    double stamp(uint64_t pos, double wakeNs, bool firstOfWake, double nsPerFrame, double jumpNs, double wakeDtSec) {
        if (!set) { t0 = wakeNs - (double)pos * nsPerFrame; set = true; }
        double ts = t0 + (double)pos * nsPerFrame;
        if (firstOfWake) {
            const double drift = wakeNs - ts;
            if (std::fabs(drift) > jumpNs) { t0 += drift; ts = wakeNs; }            // real jump: re-anchor
            else t0 += drift * (1.0 - std::exp(-wakeDtSec / 10.0));                  // slow correction (tau 10 s)
        }
        return ts;
    }
};

// Output latency in ASIO frames of a converted Shared stream: the ASIO block, the converter's look-ahead
// (ASIO frames) and the device buffer plus stream latency (device frames, converted).
// devBuffer is the number of device frames the driver keeps queued (outFill, see FillTarget), not necessarily the
// whole WASAPI buffer. Host thread (getLatencies / stream setup).
inline long OutLatency(long bufSize, double asioRate, double devRate, uint32_t devBuffer, uint32_t streamLat, int ahead) {
    return bufSize + ahead + (long)std::lround(((double)devBuffer + (double)streamLat) * asioRate / devRate);
}
// Converted input on the output's clock: input sample q (ASIO rate, counted from the start of the capture) is
// delivered at host samplePosition q + InDelay. The delay is fixed at start, so the round trip is exact:
// the block and the output converter's look-ahead (the newest block ends there), the input converter's look-ahead
// plus one frame, one capture period (the newest packet may still be missing) and one device period of margin
// for wake-up jitter.
// In ASIO frames: bufSize + aheadOut + ceil((aheadIn + 1 + capPeriod) * asioRate / inRate)
//                 + ceil(outPeriod * asioRate / outRate). Computed once at stream setup (host thread).
inline uint32_t InDelay(long bufSize, int aheadOut, double asioRate, double inRate, int aheadIn, uint32_t capPeriod,
                        double outRate, uint32_t outPeriod) {
    return (uint32_t)(bufSize + aheadOut + (long)std::ceil(((double)aheadIn + 1.0 + (double)capPeriod) * asioRate / inRate) +
                      (long)std::ceil((double)outPeriod * asioRate / outRate));
}
// Input latency reported for such an input: the round trip (InDelay + device part of the output) minus the
// output latency, i.e. InDelay - bufSize - aheadOut (the remaining part is in OutLatency).
inline long InLatency(uint32_t inDelay, long bufSize, int aheadOut) { return (long)inDelay - bufSize - aheadOut; }
// What getLatencies reports before the user offsets; the driver and the model call this one function.
// outDevice: the output part after the ASIO block (OutLatency - bufSize, or the common aligned value); inTarget:
// the input FIFO target; alignedIn: a converted input on the output's clock, whose target (InLatency) already is
// the whole input side of the round trip, so the block is not added a second time. In ASIO frames.
struct Reported { long in, out; };
inline Reported ReportLatencies(long bufSize, long outDevice, long inTarget, bool alignedIn) {
    Reported r;
    r.out = bufSize + outDevice;
    r.in = alignedIn ? inTarget : bufSize + inTarget;
    return r;
}
// Input frames ready beyond the end of the block being delivered ("excess") stay between about 2 and 3 output
// periods in steady state (the model measures it, the test checks it). Outside [0, hi] the stream lost its
// alignment (output dropout, late capture start): the driver re-aligns to `mid`. In ASIO frames.
struct Resync { uint32_t mid, hi; };
inline Resync InResync(long bufSize, double asioRate, double inRate, uint32_t capPeriod, double outRate, uint32_t outPeriod) {
    const double pa = (double)outPeriod * asioRate / outRate, ca = (double)capPeriod * asioRate / inRate;
    Resync r;
    r.mid = (uint32_t)std::lround(1.5 * pa + ca);
    r.hi  = (uint32_t)std::lround(3.0 * pa + 2.0 * ca + (double)bufSize + 64.0);
    return r;
}
// Device frames kept queued by a low-latency output: two periods, or one period plus one ASIO block (device
// frames) when the block is longer; never more than the buffer.
inline uint32_t FillTarget(uint32_t period, uint32_t bufferFrames, long bufSize, double asioRate, double devRate) {
    const uint32_t blk = (uint32_t)std::ceil((double)bufSize * devRate / asioRate);
    const uint32_t want = period * 2 > period + blk ? period * 2 : period + blk;
    return want < bufferFrames ? want : bufferFrames;
}

// --- Offline model (tests only) ---
// Sim parameters: rates in Hz, period / devBuffer / streamLat / inPeriod in device frames, bufSize in ASIO frames.
struct Config {
    double asioRate = 44100, devRate = 48000, inRate = 48000;
    uint32_t period = 480, devBuffer = 960, streamLat = 0, inPeriod = 480;   // devBuffer: frames kept queued
    long bufSize = 256;
    bool sinc = true;
};
// Sim outcome. underruns: device queue ran empty; overruns: a converter ring held more than its capacity;
// inMissing: an input frame was due before it had been produced; accountingOk: frame bookkeeping is exact;
// maxJitterMs: spread of (device time - ASIO time) at callbacks; *Trip: round trip in ASIO frames (reported by
// getLatencies, expected from the structure, measured from the impulse; -1 = not measured).
struct Result {
    uint64_t callbacks = 0, underruns = 0, overruns = 0, inMissing = 0;
    bool accountingOk = true;
    size_t heldMin = (size_t)-1, heldMax = 0;
    double maxJitterMs = 0;
    double reportedTrip = 0, expectedTrip = 0, measuredTrip = -1;
    long long exMin = (long long)1 << 60, exMax = -((long long)1 << 60);   // input frames ready beyond each block
};

// Model of run() for one output device and one converted input on the same clock: device wake-ups every
// `period` frames; the input packet is converted into the ASIO-rate FIFO; the output buffer is refilled to
// devBuffer with on-demand ASIO callbacks; each callback delivers input FIFO frames [pos - InDelay, ...). Times in
// device frames. With `signal`, real samples flow (output impulse -> device -> loop-back cable -> input) and the
// round trip is measured from the received pulse; without it only positions are simulated (fast, 10 minutes).
class Sim {
public:
    bool init(const Config& c, bool signal) {
        c_ = c; signal_ = signal;
        const int ch = 1;
        if (!out_.configure(c.asioRate, c.devRate, ch, (size_t)c.bufSize * 2, c.devBuffer, c.sinc)) return false;
        if (!in_.configure(c.inRate, c.asioRate, ch, c.inPeriod * 2, (size_t)c.inPeriod * 4, c.sinc)) return false;
        delay_ = InDelay(c.bufSize, out_.ahead(), c.asioRate, c.inRate, in_.ahead(), c.inPeriod, c.devRate, c.period);
        return true;
    }
    Result run(double seconds) {
        Result r;
        const double Ra = c_.asioRate, Rd = c_.devRate;
        uint64_t writtenDev = c_.devBuffer;                // pre-filled with silence, playing from time 0
        uint64_t pos = 0;                                  // ASIO samplePosition
        uint64_t inMade = 0, inTaken = 0;                  // ASIO-rate input frames produced / delivered or dropped
        const uint64_t wakes = (uint64_t)(seconds * Rd / c_.period);
        double minDev = 1e30, maxDev = -1e30;
        const uint64_t settle = (uint64_t)(2.0 * Rd / c_.period);
        size_t startHeld = 0;
        // signal mode: device output samples (index = device frame incl. the pre-fill) and the input stream
        std::vector<float> devOut, inFifo, hostIn;
        const uint64_t impulseAt = (uint64_t)(0.5 * Ra) / (uint64_t)c_.bufSize * (uint64_t)c_.bufSize + 7;
        if (signal_) devOut.assign(c_.devBuffer, 0.f);
        uint64_t devIn = 0;                                // device input frames captured (loop-back of devOut)
        for (uint64_t n = 1; n <= wakes; ++n) {
            const uint64_t t = n * (uint64_t)c_.period;   // device frames elapsed
            // input: the packet(s) captured up to t (whole capture periods)
            const uint64_t capTo = t / c_.inPeriod * c_.inPeriod;
            while (devIn < capTo) {
                if (signal_) {                             // the cable: input frame i = output frame i - streamLat
                    const long long src = (long long)devIn - (long long)c_.streamLat;
                    *in_.pushSlot() = src >= 0 && (size_t)src < devOut.size() ? devOut[(size_t)src] : 0.f;
                } else in_.skipPush(1);
                ++devIn;
            }
            const size_t made = in_.producible();
            if (signal_) in_.produce(made, [&](size_t, int, float v) { inFifo.push_back(v); });
            else in_.advance(made);
            inMade += made;
            if (in_.held() > in_.capacity()) ++r.overruns;
            // output: refill the device buffer with on-demand callbacks
            const uint64_t pad = writtenDev > t ? writtenDev - t : 0;
            if (pad == 0 && n > 2) ++r.underruns;
            const size_t frames = (size_t)(c_.devBuffer - pad);
            while (out_.missingFor(frames) > 0) {
                // input block: FIFO frames [pos - delay, pos - delay + bufSize)
                if (n > settle) {
                    const long long ex = (long long)inMade - ((long long)pos + c_.bufSize - (long long)delay_);
                    if (ex < r.exMin) r.exMin = ex;
                    if (ex > r.exMax) r.exMax = ex;
                }
                for (long i = 0; i < c_.bufSize; ++i) {
                    const long long q = (long long)pos + i - (long long)delay_;
                    float v = 0.f;
                    if (q >= 0) {
                        if ((uint64_t)q < inMade) { if (signal_) v = inFifo[(size_t)q]; ++inTaken; }
                        else ++r.inMissing;
                    }
                    if (signal_) hostIn.push_back(v);
                }
                if (n > settle) {
                    const double dev = (double)t / Rd - (double)pos / Ra;
                    if (dev < minDev) minDev = dev;
                    if (dev > maxDev) maxDev = dev;
                }
                for (long i = 0; i < c_.bufSize; ++i) {
                    if (signal_) *out_.pushSlot() = (pos + (uint64_t)i == impulseAt) ? 1.f : 0.f;
                    else out_.skipPush(1);
                }
                pos += (uint64_t)c_.bufSize;
                ++r.callbacks;
                if (out_.held() > out_.capacity()) ++r.overruns;
            }
            if (signal_) out_.produce(frames, [&](size_t, int, float v) { devOut.push_back(v); });
            else out_.advance(frames);
            writtenDev += frames;
            const size_t h = out_.held();
            if (n == settle) startHeld = h;
            if (n > settle) { if (h < r.heldMin) r.heldMin = h; if (h > r.heldMax) r.heldMax = h; }
        }
        // accounting: ASIO frames produced == device frames consumed * Ra / Rd, exactly, up to what is held
        const double heldNow = (double)pos - (double)(writtenDev - c_.devBuffer) * Ra / Rd;
        r.accountingOk = heldNow >= 0 && heldNow <= (double)(c_.bufSize + out_.taps() + 2) &&
                         r.heldMax <= startHeld + (size_t)c_.bufSize + 2 &&
                         inMade >= inTaken && inMade - inTaken <= delay_ + (uint64_t)c_.bufSize + (uint64_t)std::ceil(c_.inPeriod * Ra / c_.inRate) + 2;
        r.maxJitterMs = (maxDev - minDev) * 1e3;
        const Reported rep = ReportLatencies(c_.bufSize, OutLatency(c_.bufSize, Ra, Rd, c_.devBuffer, c_.streamLat, out_.ahead()) - c_.bufSize,
                                             InLatency(delay_, c_.bufSize, out_.ahead()), true);
        r.reportedTrip = (double)(rep.in + rep.out);       // exactly what the driver's getLatencies reports
        r.expectedTrip = ((double)c_.devBuffer + c_.streamLat) * Ra / Rd + delay_;
        if (signal_) {                                     // the received pulse: peak with parabolic interpolation
            size_t best = 0; float bv = 0.f;
            for (size_t i = 0; i < hostIn.size(); ++i) if (std::fabs(hostIn[i]) > bv) { bv = std::fabs(hostIn[i]); best = i; }
            double frac = 0;
            if (best > 0 && best + 1 < hostIn.size()) {
                const double a = hostIn[best - 1], b = hostIn[best], c = hostIn[best + 1], den = a - 2 * b + c;
                if (std::fabs(den) > 1e-12) frac = 0.5 * (a - c) / den;
            }
            if (bv > 0.3f) r.measuredTrip = (double)best + frac - (double)impulseAt;
        }
        return r;
    }
    uint32_t delay() const { return delay_; }
private:
    Config c_;
    bool signal_ = false;
    fr::Converter out_, in_;
    uint32_t delay_ = 0;
};

}  // namespace sched
