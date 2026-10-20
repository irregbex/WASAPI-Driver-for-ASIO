// WASAPI Driver for ASIO - single-writer / lock-free-reader publication of live settings (header-only, portable)
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
//   Settings that may change while audio runs ("LIVE" settings, 0.7.0: gain, dither, routing, direct monitoring)
//   are bundled in one plain struct and published from the host thread to the audio thread with SeqPublished<T>.
//   This is a seqlock whose payload is stored as an array of std::atomic<uint64_t> words, so every access is a
//   well-defined atomic operation (no data race even in the formal C++ sense) and no thread ever blocks:
//     writer (one thread only, the host thread):  seq -> odd, store the words (relaxed), seq -> even (release)
//     reader (the audio thread):                  read seq (acquire); odd = write in progress -> keep the old copy;
//                                                 load the words; fence(acquire); seq unchanged -> copy is valid
//   The reader never waits: if a publication is in progress it simply keeps its previous copy and tries again at
//   the next buffer. A new value therefore reaches the audio thread within one or two buffers.
//
// Threading
//   publish(): a single writer thread (H). tryRead(): any number of readers; real-time safe (no locks, no
//   allocation, bounded work: sizeof(T)/8 relaxed loads).
//
// Usage in driver.cpp
//   The host thread fills a LiveParams from the config (applyPanelChanges / stream setup) and calls
//   pubLive.publish(p). The audio thread calls pubLive.tryRead(lv, lvSeq) once per wake-up and then works with
//   its private copy `lv` for the whole cycle, so every parameter of one block comes from the same publication.
//
// Why the fences
//   Writer: the release fence after the odd increment keeps the word stores from becoming visible before the
//   odd sequence number (otherwise a reader could see new words with the old even seq). The final release
//   increment publishes the words before the even value.
//   Reader: the acquire load of seq orders the word loads after it; the acquire fence before the second seq load
//   keeps the word loads from being reordered after it. Equal seq values before and after, both even, prove no
//   publish overlapped the copy. This is the standard "seqlock with atomic payload" pattern (see Boehm, "Can
//   seqlocks get along with programming language memory models?").
//
// Invariants / pitfalls
//   - Exactly one writer. Two concurrent publish() calls could interleave and leave seq even over mixed words.
//   - T must be trivially copyable (memcpy-able); pointers inside T would be copied, not their pointees.
//   - lastSeq must match the reader's current copy: the driver seeds lv with the host's copy and lvSeq with
//     version() when streams start, so only later publications are picked up.
//   - seq wraps after 2^31 publications; harmless in practice (it is compared only for equality).
//   - A reader that keeps losing the race just keeps the older settings; there is no starvation hazard for audio.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

template <class T>
class SeqPublished {
    static_assert(std::is_trivially_copyable<T>::value, "SeqPublished<T> needs a trivially copyable T");
    static constexpr size_t kWords = (sizeof(T) + 7) / 8;   // payload size rounded up to whole 64-bit words
    std::atomic<uint32_t> seq_{0};                           // even = stable, odd = publication in progress
    std::atomic<uint64_t> words_[kWords];                    // the payload, word by word (lock-free atomics)

public:
    SeqPublished() { for (auto& w : words_) w.store(0, std::memory_order_relaxed); }

    // Writer (single thread). Never blocks.
    void publish(const T& v) {
        uint64_t tmp[kWords] = {};
        std::memcpy(tmp, &v, sizeof(T));
        seq_.fetch_add(1, std::memory_order_acq_rel);            // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i) words_[i].store(tmp[i], std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_release);            // even: stable
    }

    // Reader. Copies the published value into `out` and returns true only if it is consistent AND newer than the
    // version in `lastSeq` (updated on success). Returns false (leaving `out` untouched) when nothing changed or a
    // publication is in progress. Real-time safe.
    bool tryRead(T& out, uint32_t& lastSeq) const {
        const uint32_t s1 = seq_.load(std::memory_order_acquire);
        if ((s1 & 1u) || s1 == lastSeq) return false;
        uint64_t tmp[kWords];
        for (size_t i = 0; i < kWords; ++i) tmp[i] = words_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        std::memcpy(&out, tmp, sizeof(T));
        lastSeq = s1;
        return true;
    }

    // Current sequence number (even when stable); does not read the payload. Used to seed the reader's lastSeq
    // when the reader's copy is initialized directly from the writer's value.
    uint32_t version() const { return seq_.load(std::memory_order_acquire); }
};
