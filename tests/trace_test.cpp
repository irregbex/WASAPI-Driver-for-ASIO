// WASAPI Driver for ASIO - offline test of the TraceLogging encoder and the provider identity (src/tlg.h, trace.h)
// (g++ -O1 -g -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all tests/trace_test.cpp
//  -o trace_test && ./trace_test)
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
//   1. SHA-1 against the FIPS 180 test vectors.
//   2. The name-hash GUID rule against provider GUIDs published by Microsoft (Windows MIDI Services providers, as
//      listed in its CollectMidiLogs providers.wprp), so the algorithm is the real EventSource / TraceLogging one.
//   3. The GUID written in trace.h equals the hash of the provider name written in trace.h (a rename of the
//      provider without a new GUID, or a typo in the GUID, fails here).
//   4. The exact bytes of a provider-traits blob and of an event (metadata and payload), decoded back field by
//      field with an independent reader of the documented layout.
//   5. Limits: long strings truncated on a UTF-8 character boundary, oversized names make the event invalid
//      (never written), nothing is written past the buffers (sanitizers).
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED"; exit code 0 = passed.
#include "../src/tlg.h"
#include "../src/trace.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %-70s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }

static std::string Hex(const uint8_t* d, size_t n) { std::string s; char b[3]; for (size_t i = 0; i < n; ++i) { snprintf(b, 3, "%02x", d[i]); s += b; } return s; }
static std::string Sha1Hex(const std::string& m) { tlg::Sha1 s; s.add((const uint8_t*)m.data(), m.size()); uint8_t d[20]; s.digest(d); return Hex(d, 20); }
static std::string GuidStr(const tlg::Guid& g) {
    char b[40];
    snprintf(b, sizeof b, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.d1, g.d2, g.d3, g.d4[0], g.d4[1], g.d4[2], g.d4[3],
             g.d4[4], g.d4[5], g.d4[6], g.d4[7]);
    return b;
}

// Independent reader of the event metadata layout (TraceLoggingProvider.h): returns false on any inconsistency.
struct FieldDesc { std::string name; uint8_t in, out; };
static bool Decode(const uint8_t* m, size_t n, std::string& event, std::vector<FieldDesc>& fields) {
    if (n < 4 || (size_t)(m[0] | m[1] << 8) != n) return false;
    size_t i = 2;
    while (i < n && (m[i] & 0x80)) ++i;          // tags (continuation bit)
    if (i >= n) return false;
    ++i;
    const void* z = memchr(m + i, 0, n - i);
    if (!z) return false;
    event.assign((const char*)m + i); i = (const uint8_t*)z - m + 1;
    while (i < n) {
        const void* e = memchr(m + i, 0, n - i);
        if (!e) return false;
        FieldDesc f; f.name.assign((const char*)m + i); i = (const uint8_t*)e - m + 1;
        if (i >= n) return false;
        f.in = m[i++]; f.out = 0;
        if (f.in & 0x80) { if (i >= n) return false; f.out = m[i++]; f.in &= 0x7F; }
        fields.push_back(f);
    }
    return true;
}

int main() {
    printf("TraceLogging encoder offline test\n");
    Check(Sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 \"abc\"");
    Check(Sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 empty message");
    Check(Sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA-1 two blocks");
    Check(Sha1Hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "SHA-1 one million 'a'");
    // Published GUIDs of name-hashed TraceLogging providers (Windows MIDI Services, providers.wprp)
    Check(GuidStr(tlg::GuidFromName("Microsoft.Windows.Midi2.MidiSrv")) == "f42d2441-aac3-5216-0150-3c0f50006b64", "name hash: Microsoft.Windows.Midi2.MidiSrv");
    Check(GuidStr(tlg::GuidFromName("Microsoft.Midi.Console")) == "ce8536f1-a223-5028-7c7f-c64452906687", "name hash: Microsoft.Midi.Console");
    Check(GuidStr(tlg::GuidFromName("microsoft.midi.console")) == "ce8536f1-a223-5028-7c7f-c64452906687", "name hash is case-insensitive");
    // This project's provider: the constant in trace.h must be the hash of the name in trace.h
    struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } k = WB_TRACE_PROVIDER_GUID_INIT;
    const tlg::Guid g = tlg::GuidFromName(WB_TRACE_PROVIDER_NAME);
    Check(k.d1 == g.d1 && k.d2 == g.d2 && k.d3 == g.d3 && !memcmp(k.d4, g.d4, 8), "trace.h GUID = name hash of " WB_TRACE_PROVIDER_NAME);
    printf("      provider %s {%s}\n", WB_TRACE_PROVIDER_NAME, GuidStr(g).c_str());
    Check(GuidStr(g) == "52b9f111-957d-5973-b76d-97bdf8f813df", "provider GUID value");
    Check(strncmp(WB_TRACE_PROVIDER_NAME, "Microsoft", 9) != 0, "provider name is not a Microsoft name");

    // Provider traits
    uint8_t pm[64];
    const size_t pn = tlg::ProviderMeta(pm, sizeof pm, "Abc");
    Check(pn == 6 && pm[0] == 6 && pm[1] == 0 && !memcmp(pm + 2, "Abc", 4), "provider metadata: size + name + NUL");
    Check(tlg::ProviderMeta(pm, 4, "Abcdef") == 0, "provider metadata that does not fit is refused");

    // One event, every field type
    tlg::Event<512, 1024> e("Ev");
    e.i32("a", -2); e.u32("b", 3); e.i64("c", -4); e.u64("d", 5); e.f64("f", 0.5); e.boolean("g", true); e.hresult("h", (int32_t)0x80070005);
    e.str("s", "xy");
    Check(e.finish(), "event fits");
    std::string ev; std::vector<FieldDesc> fd;
    Check(Decode(e.meta(), e.metaSize(), ev, fd) && ev == "Ev" && fd.size() == 8, "metadata decodes: name and 8 fields");
    const uint8_t expIn[8] = {tlg::kInInt32, tlg::kInUInt32, tlg::kInInt64, tlg::kInUInt64, tlg::kInDouble, tlg::kInBool32, tlg::kInInt32, tlg::kInAnsiString};
    const uint8_t expOut[8] = {0, 0, 0, 0, 0, 0, tlg::kOutHResult, tlg::kOutUtf8};
    bool types = fd.size() == 8;
    for (size_t i = 0; types && i < 8; ++i) types = fd[i].in == expIn[i] && fd[i].out == expOut[i] && fd[i].name.size() == 1;
    Check(types, "field types (HRESULT and UTF-8 out types)");
    const uint8_t expData[] = {0xFE, 0xFF, 0xFF, 0xFF, 3, 0, 0, 0, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 5, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0xE0, 0x3F, 1, 0, 0, 0, 0x05, 0x00, 0x07, 0x80, 'x', 'y', 0};
    Check(e.dataSize() == sizeof expData && !memcmp(e.data(), expData, sizeof expData), "payload bytes (little-endian values, NUL-terminated text)");

    // Truncation on a UTF-8 boundary: 600 two-byte characters, limit 512 bytes -> 512 bytes, no half character
    std::string longText;
    for (int i = 0; i < 600; ++i) longText += "\xC3\xA9";
    tlg::Event<512, 1024> t("T");
    t.str("x", longText.c_str());
    Check(t.finish() && t.dataSize() == 513 && t.data()[511] == 0xA9 && t.data()[512] == 0, "long text cut at 512 bytes on a character boundary");
    std::string odd = "a" + longText;              // the 512-byte cut would split a character: one byte less
    tlg::Event<512, 1024> t2("T");
    t2.str("x", odd.c_str());
    Check(t2.finish() && t2.dataSize() == 512 && t2.data()[510] == 0xA9, "a cut inside a character moves back to its lead byte");
    // Overflow: a field name larger than the metadata buffer makes the event invalid (it is never written)
    std::string huge(600, 'n');
    tlg::Event<512, 1024> o("O");
    o.u32(huge.c_str(), 1);
    Check(!o.finish(), "oversized metadata: event refused");
    tlg::Event<64, 16> small("S");
    small.str("s", "0123456789abcdefghij");
    Check(small.finish() && small.dataSize() == 16 && small.data()[15] == 0, "text truncated to the room left in a small payload buffer");
    small.u64("v", 1);
    Check(!small.finish(), "a value that no longer fits makes the event invalid");
    // 0.9.12: the "Stats" event of driver.cpp (onStatsTimer) must fit the 512-byte metadata buffer of Write(); its
    // field names are repeated here (keep in sync when a counter is added: 27 fields since 0.9.13, about 480 bytes).
    {
        static const char* statsFields[] = {"Running", "BufferSwitches", "OutputGlitches", "WaitTimeouts", "InputUnderruns", "InputDrops",
            "InputGlitches", "LoopbackUnderruns", "ExtraInputUnderruns", "ExtraInputDrops", "ExtraOutputUnderflows", "ExtraOutputOverflows",
            "DspLoadPct", "DspLoadMaxPct", "InputDriftPpm", "MonitorUnderruns", "CapturePulls", "CaptureEmptyPulls", "CaptureMultiPacketPulls",
            "CaptureMaxPacket", "CaptureMaxPerWake", "CaptureMaxFifoFill", "CaptureFifoFillNow", "WakeGapMaxUs", "CaptureReadGapMaxUs",
            "CaptureGated", "CaptureClockFallback"};   // 0.9.13
        tlg::Event<512, 1024> st("Stats");
        st.boolean(statsFields[0], true); st.u64(statsFields[1], 1);
        for (size_t i = 2; i < sizeof statsFields / sizeof statsFields[0]; ++i) {
            if (i == 12 || i == 13 || i == 14) st.f64(statsFields[i], 1.0);
            else if (i == 26) st.boolean(statsFields[i], true);
            else st.u32(statsFields[i], 1);
        }
        Check(st.finish() && st.metaSize() <= 512, "the Stats event (27 fields) fits the 512-byte metadata buffer");
    }
    // The level / keyword constants used by the .wprp and TRACING.txt
    Check(wbtrace::kLvlVerbose == 5 && wbtrace::kLvlInfo == 4 && wbtrace::kKwStats == 0x40 && wbtrace::kKwUpdate == 0x200, "levels and keywords");
    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
