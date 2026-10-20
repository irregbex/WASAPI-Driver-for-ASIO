// WASAPI Driver for ASIO - drift controller shared by inputs and additional outputs, sink write planning
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
//   Header-only helpers used by driver.cpp (real-time audio thread: no allocation, no locks, no system calls) and
//   by tests/sink_test.cpp (offline simulation of the same code).
//
// Drift problem
//   Two audio devices run on different crystals; their sample rates differ by tens to hundreds of ppm. A FIFO
//   between them slowly fills or drains. The driver keeps the FIFO fill at a set point by resampling with a
//   variable ratio r (input frames consumed per output frame) computed by the PI controller below.
//
// Who uses what
//   Inputs (additional capture devices feeding ASIO inputs): FIFO filled by the capture device, read once per
//   ASIO block by the resampler; PiUpdate is called with dt = bufSize / rate after each block.
//   Sinks (additional render devices fed from ASIO outputs): FIFO filled once per ASIO block by bufferSwitch, read
//   at each wake-up of the master device by serviceSink; SinkTargetPad / SinkFramesToWrite decide how much to
//   write into the device, SinkSetpoint is the FIFO set point, and PiUpdate runs after each written chunk with
//   dt = written frames / rate. For sinks the measured "fill" also includes the frames queued in the device
//   minus targetPad (interpolated with IAudioClock when available), so the loop sees the whole path.
//
// Why PI and why these constants
//   P alone would leave a steady-state error proportional to the ppm offset (the FIFO would settle away from the
//   set point). The I term absorbs the constant crystal offset so the error returns to 0. The fill estimate is
//   low-passed first because a FIFO filled in bursts (host blocks, device periods) has a saw-tooth fill; feeding
//   that ripple straight into r would frequency-modulate the audio. The output clamp (+-3000 ppm) bounds the pitch
//   deviation and keeps the sinc resampler inside its no-aliasing assumption (sinc.h).
#pragma once
#include <cmath>
#include <cstdint>

// Time-normalized PI controller (constants validated in 0.2.2; see ARCHITECTURE.txt section 9).
//   fill    current fill (frames ahead of the read position)
//   sp      set point (frames)
//   dt      seconds since the previous update (one ASIO block for inputs, one written chunk for sinks)
//   fillEma, integ   controller state, owned by the caller (audio thread)
// Math:
//   fillEma += (1 - exp(-dt / 1.0667 s)) * (fill - fillEma)      first-order low-pass, tau ~ 1.07 s
//   e        = (fillEma - sp) / sp                                normalized error
//   integ   += 9.375e-6 * e * dt, clamped to +-0.002              integral term, +-2000 ppm
//   r        = 1 + 0.0005 * e + integ, clamped to [0.997, 1.003] proportional + integral, +-3000 ppm
// Because every term is scaled by dt, the loop behaves the same for any ASIO buffer size.
// Returns the resampling ratio. fill > sp -> r > 1 -> the FIFO is consumed faster. Real-time safe.
// Units: fill and sp in frames (sp must be > 0, it is a divisor), dt in seconds, return value dimensionless.
// State: both are reset (fillEma = sp, integ = 0) when streams start. On re-priming after an underrun, sinks
// reset both (and ratio = 1); inputs reset only fillEma and keep integ, i.e. the learned ppm offset survives.
static inline double PiUpdate(double& fillEma, double& integ, double fill, double sp, double dt) {
    const double alpha = 1.0 - exp(-dt / 1.0667);
    fillEma += alpha * (fill - fillEma);
    const double err = (fillEma - sp) / sp;
    integ += 9.375e-6 * err * dt;
    if (integ > 0.002) integ = 0.002;
    if (integ < -0.002) integ = -0.002;
    double r = 1.0 + 0.0005 * err + integ;
    if (r > 1.003) r = 1.003;
    if (r < 0.997) r = 0.997;
    return r;
}

// Sink (additional output, no event): at every wake-up of the master clock the device's WASAPI buffer is topped
// up to targetPad queued frames without exceeding the free space. Returns the number of frames to write now.
//   padding    frames currently queued in the device (IAudioClient::GetCurrentPadding)
//   bufFrames  the device's WASAPI buffer size in frames (IAudioClient::GetBufferSize)
//   targetPad  desired queue depth in frames (SinkTargetPad)
// Returns 0 when the queue is already at/above target or the buffer is full (no GetBuffer call is then made).
static inline uint32_t SinkFramesToWrite(uint32_t padding, uint32_t bufFrames, uint32_t targetPad) {
    if (padding >= targetPad || padding >= bufFrames) return 0;
    uint32_t w = targetPad - padding, space = bufFrames - padding;
    return w < space ? w : space;
}

// Target of frames queued in the device: one master period (the longest the next wake-up may take) plus two
// device periods (the granularity at which the audio engine consumes). Capped at the buffer size.
// All values in frames of the stream rate. Computed once when the sink is opened (host thread).
static inline uint32_t SinkTargetPad(uint32_t masterPeriod, uint32_t devPeriod, uint32_t bufFrames) {
    uint32_t t = masterPeriod + 2 * devPeriod;
    return t < bufFrames ? t : bufFrames;
}

// Set point of the sink FIFO (ASIO frames ahead of the read position): the host delivers bufSize blocks in bursts,
// so the FIFO keeps one block plus one master period (plus the sinc look-ahead when the sinc resampler is used).
// bufSize: ASIO buffer size (frames); masterPeriod: master device period (frames); sincLookahead: kSincHalf or 0.
// This set point is part of the extra output's latency relative to the main output.
static inline double SinkSetpoint(long bufSize, uint32_t masterPeriod, int sincLookahead) {
    return (double)bufSize + (double)masterPeriod + (double)sincLookahead;
}
