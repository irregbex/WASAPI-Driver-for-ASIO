// WASAPI Driver for ASIO - minimal TraceLogging (self-describing ETW) event encoder, header-only and portable
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
//   Builds the two blobs of a TraceLogging event (event metadata and payload) in fixed-size buffers, with no heap,
//   no exceptions and no Windows dependency, so that src/trace.cpp can hand them to EventWriteTransfer and
//   tests/trace_test.cpp can check the exact bytes (and fuzz the encoder) on Linux.
//   Why not Microsoft's TraceLoggingDynamic.h (MIT, github.com/microsoft/tracelogging, etw/cpp/traceloggingdynamic)?
//   It was evaluated for 0.9.8: it does not compile with mingw-w64 (it relies on MSVC SAL annotations such as
//   _Ret_bytecount_ and _Post_invalid_ that mingw does not define) and its Event class grows std::vector buffers on
//   the heap, which the driver avoids outside the host thread. TraceLoggingProvider.h (Windows SDK) is not shipped
//   with mingw-w64 either. So this file implements the small subset the driver needs, following the documented
//   format (TraceLoggingProvider.h, "TraceLogging ETW event layout").
//
// Format (all integers little-endian)
//   Provider metadata ("provider traits"):  UINT16 TotalSize; char Name[] (UTF-8, NUL-terminated).
//   Event metadata:                         UINT16 TotalSize; UINT8 Tags (0 = none, one byte); char EventName[];
//                                           then per field: char FieldName[]; UINT8 InType [| 0x80 if an OutType
//                                           byte follows]; [UINT8 OutType].
//   Payload: the field values in field order (fixed-size values raw; strings UTF-8 / ANSI NUL-terminated).
//   The ETW event descriptor uses Channel 11 (WINEVENT_CHANNEL_TRACELOGGING), Id 0, Version 0, Opcode 0, Task 0;
//   the provider metadata and event metadata go in two EVENT_DATA_DESCRIPTORs with Reserved (Type) = 2 and 1.
//
// Provider GUID
//   TraceLogging and .NET EventSource derive a provider's GUID from its name: SHA-1 over the 16 bytes of the
//   namespace GUID 482C2DB2-C390-47C8-87F8-1A15BFC130FB (in that written byte order) followed by the provider name in
//   upper case as UTF-16BE; the first 16 bytes of the digest, with byte 7 set to (byte7 & 0x0F) | 0x50, read as a
//   GUID (Data1..Data3 little-endian). GuidFromName implements it (the test compares it with the constant in
//   trace.h and with published provider GUIDs).
//
// Threading
//   Pure functions over caller buffers; real-time safe in principle, but the driver never traces on the audio thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tlg {

// TlgIn / TlgOut values (TraceLoggingProvider.h). Only the ones the driver uses.
enum InType : uint8_t {
    kInAnsiString = 2,   // 8-bit NUL-terminated; with kOutUtf8 decoded as UTF-8
    kInInt32 = 7, kInUInt32 = 8, kInInt64 = 9, kInUInt64 = 10, kInDouble = 12, kInBool32 = 13,
};
enum OutType : uint8_t { kOutDefault = 0, kOutHResult = 15, kOutUtf8 = 35 };
constexpr uint8_t kHasOutType = 0x80;          // InType flag: an OutType byte follows
constexpr uint8_t kChannelTraceLogging = 11;   // WINEVENT_CHANNEL_TRACELOGGING

// Append-only byte buffer over caller storage. Every write is bounds-checked; once a write does not fit, `bad` is
// set and nothing more is written (the event is then not sent).
struct Buf {
    uint8_t* p; size_t cap; size_t n = 0; bool bad = false;
    Buf(uint8_t* mem, size_t size) : p(mem), cap(size) {}
    size_t room() const { return bad ? 0 : cap - n; }
    void put(const void* src, size_t len) {
        if (bad || len > cap - n) { bad = true; return; }
        if (len) memcpy(p + n, src, len);
        n += len;
    }
    void u8(uint8_t v) { put(&v, 1); }
    void u16(uint16_t v) { const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, 2); }
    void u32(uint32_t v) { uint8_t b[4]; for (int i = 0; i < 4; ++i) b[i] = (uint8_t)(v >> (8 * i)); put(b, 4); }
    void u64(uint64_t v) { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i)); put(b, 8); }
    void cstr(const char* s) { if (!s) s = ""; put(s, strlen(s) + 1); }
};

// Length of the longest prefix of s (at most maxLen bytes) that does not cut a UTF-8 sequence in the middle: when
// the cut falls inside a sequence, it moves back to that sequence's lead byte (continuation bytes are 10xxxxxx).
inline size_t Utf8Prefix(const char* s, size_t len, size_t maxLen) {
    if (len <= maxLen) return len;
    size_t k = maxLen;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) --k;
    return k;
}

// Provider metadata blob (traits) for `name`. Returns its size, or 0 if it does not fit `cap`.
inline size_t ProviderMeta(uint8_t* out, size_t cap, const char* name) {
    Buf b(out, cap);
    b.u16(0); b.cstr(name);
    if (b.bad || b.n > 0xFFFF) return 0;
    out[0] = (uint8_t)b.n; out[1] = (uint8_t)(b.n >> 8);
    return b.n;
}

// One event under construction: metadata and payload in two caller-sized arrays (template sizes, on the stack).
// Strings longer than kMaxString bytes, or longer than the room left, are truncated on a UTF-8 boundary; a field
// name or event name that does not fit makes the event invalid (ok() false) instead.
template <size_t MetaCap = 512, size_t DataCap = 1024>
class Event {
public:
    static constexpr size_t kMaxString = 512;
    explicit Event(const char* name) : meta_(metaMem_, MetaCap), data_(dataMem_, DataCap) {
        meta_.u16(0);            // TotalSize, patched by finish()
        meta_.u8(0);             // no event tags
        meta_.cstr(name);
    }
    void i32(const char* f, int32_t v)  { field(f, kInInt32); data_.u32((uint32_t)v); }
    void u32(const char* f, uint32_t v) { field(f, kInUInt32); data_.u32(v); }
    void i64(const char* f, int64_t v)  { field(f, kInInt64); data_.u64((uint64_t)v); }
    void u64(const char* f, uint64_t v) { field(f, kInUInt64); data_.u64(v); }
    void f64(const char* f, double v)   { uint64_t u; memcpy(&u, &v, 8); field(f, kInDouble); data_.u64(u); }
    void boolean(const char* f, bool v) { field(f, kInBool32); data_.u32(v ? 1u : 0u); }
    void hresult(const char* f, int32_t hr) { field(f, kInInt32, kOutHResult); data_.u32((uint32_t)hr); }
    // UTF-8 text (truncated, see above). nullptr is written as "".
    void str(const char* f, const char* s) {
        field(f, kInAnsiString, kOutUtf8);
        if (!s) s = "";
        const size_t len = strlen(s);
        const size_t room = data_.room();
        if (room == 0) { data_.bad = true; return; }
        size_t max = room - 1;                    // keep room for the NUL
        if (max > kMaxString) max = kMaxString;
        const size_t k = Utf8Prefix(s, len, max);
        data_.put(s, k); data_.u8(0);
    }
    // Patches TotalSize. False if anything overflowed (the event must then not be written).
    bool finish() {
        if (meta_.bad || data_.bad || meta_.n > 0xFFFF) return false;
        metaMem_[0] = (uint8_t)meta_.n; metaMem_[1] = (uint8_t)(meta_.n >> 8);
        return true;
    }
    const uint8_t* meta() const { return metaMem_; }
    size_t metaSize() const { return meta_.n; }
    const uint8_t* data() const { return dataMem_; }
    size_t dataSize() const { return data_.n; }

private:
    void field(const char* f, uint8_t in, uint8_t out = kOutDefault) {
        meta_.cstr(f);
        if (out != kOutDefault) { meta_.u8((uint8_t)(in | kHasOutType)); meta_.u8(out); }
        else meta_.u8(in);
    }
    uint8_t metaMem_[MetaCap];
    uint8_t dataMem_[DataCap];
    Buf meta_, data_;
};

// ---------------------------------------------------------------- provider GUID from its name (EventSource rule)
// Minimal SHA-1 (FIPS 180-4), used only for GuidFromName.
class Sha1 {
public:
    Sha1() { h_[0] = 0x67452301u; h_[1] = 0xEFCDAB89u; h_[2] = 0x98BADCFEu; h_[3] = 0x10325476u; h_[4] = 0xC3D2E1F0u; }
    void add(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) { buf_[fill_++] = p[i]; ++total_; if (fill_ == 64) { block(); fill_ = 0; } }
    }
    void digest(uint8_t out[20]) {
        const uint64_t bits = total_ * 8;
        const uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (fill_ != 56) add(&zero, 1);
        uint8_t len[8]; for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        add(len, 8);
        for (int i = 0; i < 5; ++i) for (int k = 0; k < 4; ++k) out[i * 4 + k] = (uint8_t)(h_[i] >> (24 - 8 * k));
    }
private:
    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    void block() {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)buf_[4 * i] << 24 | (uint32_t)buf_[4 * i + 1] << 16 | (uint32_t)buf_[4 * i + 2] << 8 | buf_[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
    }
    uint32_t h_[5];
    uint8_t buf_[64] = {};
    size_t fill_ = 0;
    uint64_t total_ = 0;
};

struct Guid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };

// EventSource / TraceLogging provider GUID of an ASCII name (upper-cased with the invariant ASCII rule).
inline Guid GuidFromName(const char* name) {
    static const uint8_t ns[16] = {0x48, 0x2C, 0x2D, 0xB2, 0xC3, 0x90, 0x47, 0xC8, 0x87, 0xF8, 0x1A, 0x15, 0xBF, 0xC1, 0x30, 0xFB};
    Sha1 s;
    s.add(ns, 16);
    for (const char* p = name; *p; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        const uint8_t be[2] = {0, (uint8_t)c};       // UTF-16BE of an ASCII character
        s.add(be, 2);
    }
    uint8_t h[20];
    s.digest(h);
    h[7] = (uint8_t)((h[7] & 0x0F) | 0x50);
    Guid g;
    g.d1 = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    g.d2 = (uint16_t)(h[4] | h[5] << 8);
    g.d3 = (uint16_t)(h[6] | h[7] << 8);
    for (int i = 0; i < 8; ++i) g.d4[i] = h[8 + i];
    return g;
}

}  // namespace tlg
