// WASAPI Driver for ASIO - registry of live driver instances (0.9.1, fix 3)
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
//   Process-global list of initialized driver objects, in creation order. The control panel uses it to rebind to
//   the instance the host created after a reset. Portable template so tests/registry_test.cpp can hammer it with
//   threads on Linux.
//
// Rules
//   - every access holds the lock (SRWLOCK on Windows, std::mutex elsewhere): a host may create or release driver
//     objects on more than one thread;
//   - findNewerAddRef returns an object that is ALREADY referenced: the reference is taken under the lock with
//     T::tryAddRef(), which succeeds only while the object's count is above zero and it is not dying, so an
//     object whose last Release already ran (its destructor may be running) is never handed out;
//   - T's destructor calls remove() first; until then the object memory is valid, so tryAddRef on it is safe.
//   Never used by the audio thread.
//
// Usage in driver.cpp
//   One global reg::Registry<WasapiBridge> (g_instances). An instance is added when it initializes and removed in
//   its destructor; FindNewerInstance(old) wraps findNewerAddRef for the panel. The caller of findNewerAddRef owns
//   the returned reference and must Release() it.
//
// Pitfall
//   Do not call into T (other than tryAddRef) while holding the lock: T's destructor takes the same lock in
//   remove(), so a Release() under the lock could deadlock.
#pragma once
#include <cstddef>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <mutex>
#endif

namespace reg {

// Minimal exclusive lock with the same interface on both platforms (non-recursive; no RAII guard on purpose,
// every critical section below is a few lines with a single exit).
class Lock {
public:
#ifdef _WIN32
    void lock() { AcquireSRWLockExclusive(&l_); }
    void unlock() { ReleaseSRWLockExclusive(&l_); }
private:
    SRWLOCK l_ = SRWLOCK_INIT;
#else
    void lock() { m_.lock(); }
    void unlock() { m_.unlock(); }
private:
    std::mutex m_;
#endif
};

// T must provide bool tryAddRef() (see Rules). Pointers are not owned: the registry holds no reference itself.
template <class T> class Registry {
public:
    // Appends d (newest last). Duplicates are not checked; each instance adds itself once.
    void add(T* d) { lock_.lock(); v_.push_back(d); lock_.unlock(); }
    void remove(T* d) {
        lock_.lock();
        for (size_t i = 0; i < v_.size(); ++i) if (v_[i] == d) { v_.erase(v_.begin() + (long)i); break; }
        lock_.unlock();
    }
    // Newest instance other than `old` that could be referenced, with one reference added for the caller.
    // Scans from the end (newest first); a dying instance is skipped and the next older one is tried.
    // Returns nullptr if none.
    T* findNewerAddRef(const T* old) {
        T* r = nullptr;
        lock_.lock();
        for (size_t i = v_.size(); i-- > 0;)
            if (v_[i] != old && v_[i]->tryAddRef()) { r = v_[i]; break; }
        lock_.unlock();
        return r;
    }
    // Number of registered instances (a snapshot; may change right after the lock is released).
    size_t size() { lock_.lock(); const size_t n = v_.size(); lock_.unlock(); return n; }
private:
    Lock lock_;
    std::vector<T*> v_;
};

}  // namespace reg
