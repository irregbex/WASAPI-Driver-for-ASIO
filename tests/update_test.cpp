// WASAPI Driver for ASIO - offline test of the update check helpers (src/update.h)
// (Linux: g++ -O2 -std=c++17 tests/update_test.cpp -o ut && ./ut)
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
// Purpose (0.9.0, H3)
//   Without network: semantic version parsing / comparison, the JSON reader on a realistic GitHub release
//   answer (fields at the right level only, escapes, nested objects, unknown fields), robustness on malformed or
//   hostile input (truncated documents, deep nesting, wrong types, huge input, random bytes: never crashes and
//   never accepts garbage), Setup asset selection, SHA256SUMS.txt lookup, URL and repository rules.
//   0.9.1: SemVer pre-release precedence (fix 8) and the final host allow-list (fix 5).
//   0.9.10: the first two-digit patch number (0.9.10 > 0.9.9), and src/common.h's WB_VERSION_NUM against the
//   new getDriverVersion scheme (major * 100000 + minor * 1000 + patch * 10; read from the source root).
//   Prints one line per check and "RESULT: PASSED" or "RESULT: FAILED".
#include "../src/update.h"
#include <cstdio>
#include <cstdlib>     // atol (0.9.10)

using namespace upd;
static int g_fail = 0;
static void Check(bool ok, const char* what) { printf("  %-66s %s\n", what, ok ? "OK" : "FAILED"); if (!ok) ++g_fail; }
static int Cmp(const char* a, const char* b) { Version x, y; if (!ParseVersion(a, x) || !ParseVersion(b, y)) return 99; return Compare(x, y); }

int main() {
    printf("Update helpers offline test\n");
    Version v;
    Check(ParseVersion("v0.9.0", v) && v.major == 0 && v.minor == 9 && v.patch == 0 && !v.pre, "parse v0.9.0");
    Check(ParseVersion("1.2", v) && v.patch == 0, "parse 1.2 (patch 0)");
    Check(ParseVersion("1.2.3-beta.2", v) && v.pre && v.preTag == "beta.2", "parse 1.2.3-beta.2");
    Check(!ParseVersion("", v) && !ParseVersion("v", v) && !ParseVersion("1.x", v) && !ParseVersion("1.2.3 ", v) &&
          !ParseVersion("99999999.0", v) && !ParseVersion("1.2.3-", v), "reject malformed versions");
    Check(Cmp("v0.9.1", "0.9.0") > 0 && Cmp("0.10.0", "0.9.9") > 0 && Cmp("1.0.0", "0.99.99") > 0, "newer versions compare greater");
    Check(Cmp("0.9.0", "v0.9.0") == 0 && Cmp("0.9.0-rc.1", "0.9.0") < 0 && Cmp("0.9.0-rc.2", "0.9.0-rc.1") > 0, "equal and pre-release ordering");
    // 0.9.10: the first two-digit patch number compares numerically, not as text ("0.9.10" > "0.9.9")
    Check(Cmp("0.9.10", "0.9.9") > 0 && Cmp("v0.9.10", "0.9.10") == 0 && Cmp("0.9.10", "0.9.11") < 0 &&
          Cmp("0.10.0", "0.9.10") > 0 && Cmp("0.9.10-rc.1", "0.9.9") > 0 && Cmp("0.9.10-rc.1", "0.9.10") < 0,
          "two-digit patch: 0.9.10 is newer than 0.9.9");
    {   // 0.9.10: src/common.h WB_VERSION_STR / WB_VERSION_NUM (read as text, like build.yml does; common.h needs
        // <windows.h>). WB_VERSION_NUM = major * 100000 + minor * 1000 + patch * 10 and larger than 990 (0.9.9).
        std::string text;
        for (const char* path : {"src/common.h", "../src/common.h"}) {
            if (FILE* f = fopen(path, "rb")) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n); fclose(f); break; }
        }
        auto define = [&](const char* name) {
            const std::string key = std::string("#define ") + name + " ";
            const size_t p = text.find(key);
            if (p == std::string::npos) return std::string();
            size_t a = p + key.size();
            while (a < text.size() && text[a] == ' ') ++a;
            size_t b = a;
            while (b < text.size() && text[b] != '\r' && text[b] != '\n' && text[b] != ' ') ++b;
            return text.substr(a, b - a);
        };
        std::string vs = define("WB_VERSION_STR");
        if (vs.size() >= 2 && vs.front() == '"' && vs.back() == '"') vs = vs.substr(1, vs.size() - 2);
        const long num = atol(define("WB_VERSION_NUM").c_str());
        Version cur;
        const bool parsed = ParseVersion(vs, cur);
        printf("  common.h: WB_VERSION_STR \"%s\", WB_VERSION_NUM %ld\n", vs.c_str(), num);
        Check(parsed && num == cur.major * 100000L + cur.minor * 1000L + cur.patch * 10L && num > 990 && Cmp(vs.c_str(), "0.9.9") >= 0,
              "WB_VERSION_NUM = major*100000 + minor*1000 + patch*10, above 990 (0.9.9)");
    }

    const std::string json = R"({
      "url": "https://api.github.com/repos/o/r/releases/1", "id": 1, "tag_name": "v0.9.1",
      "name": "WASAPI Driver for ASIO 0.9.1 \"beta\"", "draft": false, "prerelease": true,
      "author": {"login": "someone", "name": "not the release name", "tag_name": "nested-not-top"},
      "assets": [
        {"name": "SHA256SUMS.txt", "size": 300, "browser_download_url": "https://github.com/o/r/releases/download/v0.9.1/SHA256SUMS.txt",
         "uploader": {"name": "x"}},
        {"name": "WASAPI-Driver-for-ASIO-0.9.1-Setup.exe", "size": 950000, "label": null,
         "browser_download_url": "https://github.com/o/r/releases/download/v0.9.1/WASAPI-Driver-for-ASIO-0.9.1-Setup.exe"},
        {"name": "WASAPI-Driver-for-ASIO-0.9.1-src.zip", "size": 1.5e6, "browser_download_url": "https://github.com/o/r/x.zip"}
      ],
      "body": "line 1\nline 2 \u00e9 \\ \/ \t", "reactions": [1, 2, [3, {"a": [true, false, null]}]]
    })";
    Release r;
    Check(ParseRelease(json, r), "parse a realistic release answer");
    Release rn;
    Check(ParseRelease("{\"tag_name\": \"v2.0.0\", \"name\": null, \"assets\": []}", rn) && rn.name.empty(), "release name may be null");
    Check(r.tag == "v0.9.1" && r.name == "WASAPI Driver for ASIO 0.9.1 \"beta\"" && r.prerelease && !r.draft, "top-level fields only (nested name/tag ignored)");
    Check(r.assets.size() == 3 && r.assets[1].size == 950000 && r.assets[0].url.find("SHA256SUMS.txt") != std::string::npos, "assets: names, sizes, URLs");
    Check(SetupAssetName(r) == "WASAPI-Driver-for-ASIO-0.9.1-Setup.exe", "Setup asset selected");
    Check(AllowedDownloadUrl(r.assets[1].url) && !AllowedDownloadUrl("http://github.com/o/r/x.exe") &&
          !AllowedDownloadUrl("https://example.com/x.exe") && !AllowedDownloadUrl("https://github.com.evil.example/x"),
          "download URLs: https://github.com/ only");
    Check(!AllowedDownloadUrl("https://github.com/o/r/a b.exe") && !AllowedDownloadUrl("https://github.com/o/r/\"x"), "download URLs: no spaces or quotes");

    const char* bad[] = {"", "{", "[]", "{\"tag_name\": 5}", "{\"tag_name\": \"v1\",}", "{\"tag_name\": \"v1\"} x", "{\"a\":\"\\q\"}",
                         "{\"tag_name\": \"v1\", \"assets\": [{\"name\": 7}]}", "{\"tag_name\": \"\\u12G4\"}", "nul", "{\"a\": tru}",
                         "{\"tag_name\": \"v1\"\n\"name\": \"x\"}", "\"just a string\"", "{\"tag_name\": \"v1\", \"draft\": \"no\"}",
                         "{\"tag_name\": \"v1\", \"assets\": {\"name\": \"x\"}}", "{\"tag_name\": \"v1\", \"assets\": [{\"size\": \"9\"}]}"};
    bool rejected = true;
    for (const char* b : bad) { Release x; if (ParseRelease(b, x)) { rejected = false; printf("    accepted: %s\n", b); } }
    Check(rejected, "malformed documents are rejected");
    std::string deep(5000, '['); deep += std::string(5000, ']');
    Release x;
    Check(!ParseRelease(deep, x), "deep nesting rejected (depth limit, no stack overflow)");
    std::string big = "{\"tag_name\": \"v1\", \"body\": \"" + std::string(2u << 20, 'a') + "\"}";
    Check(!ParseRelease(big, x), "answers over 1 MB rejected");
    uint32_t s = 7; long accepted = 0;
    for (int i = 0; i < 20000; ++i) {                    // random bytes and random mutations of the real answer
        std::string m = json;
        for (int k = 0; k < 4; ++k) { s = s * 1664525u + 1013904223u; m[(s >> 8) % m.size()] = (char)(s >> 24); }
        Release y; if (ParseRelease(m, y) && y.tag.empty()) ++accepted;
        std::string rnd(64, ' ');
        for (char& c : rnd) { s = s * 1664525u + 1013904223u; c = (char)(s >> 24); }
        ParseRelease(rnd, y);
    }
    Check(accepted == 0, "20000 mutated / random inputs: no crash, never an empty tag");

    const std::string sums = "0000000000000000000000000000000000000000000000000000000000000000  other.zip\r\n"
                             "ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789 *WASAPI-Driver-for-ASIO-0.9.1-Setup.exe\n";
    std::string h;
    Check(FindSha256(sums, "WASAPI-Driver-for-ASIO-0.9.1-Setup.exe", h) && h == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
          "SHA256SUMS.txt lookup (binary marker, CRLF, case)");
    Check(!FindSha256(sums, "missing.exe", h) && !FindSha256("xyz  WASAPI-Driver-for-ASIO-0.9.1-Setup.exe\n", "WASAPI-Driver-for-ASIO-0.9.1-Setup.exe", h),
          "missing file or bad hash rejected");
    Check(ValidRepo(L"someone/WASAPI-Driver-for-ASIO") && !ValidRepo(L"noslash") && !ValidRepo(L"a/b/c") && !ValidRepo(L"a/b?x") &&
          !ValidRepo(L"/b") && !ValidRepo(L"a/"), "repository names");
    Check(IsPlaceholderRepo(L"OWNER/WASAPI-Driver-for-ASIO") && IsPlaceholderRepo(L"") && !IsPlaceholderRepo(L"someone/repo"),
          "placeholder repository detected");
    // 0.9.7: the prepared source tree uses an owner token containing "PLACEHOLDER" until the GitHub account is known.
    // The token is assembled from two literals here so that the owner-substitution script, which replaces the
    // whole token everywhere, leaves this test unchanged.
    const std::wstring tokenOwner = std::wstring(L"GHUSER_") + L"PLACEHOLDER";
    Check(IsPlaceholderRepo(tokenOwner + L"/WASAPI-Driver-for-ASIO") && IsPlaceholderRepo(L"some_placeholder/repo") &&
          IsPlaceholderRepo(L"owner/WASAPI-Driver-for-ASIO"), "owner token / any-case PLACEHOLDER or OWNER = not configured");
    Check(!IsPlaceholderRepo(L"someone/placeholder-tools") && !IsPlaceholderRepo(L"owners/repo"),
          "only the owner part is examined, and only whole OWNER");
    // 0.9.1 (fix 8): SemVer 2.0.0 pre-release precedence, identifier by identifier
    printf("Pre-release precedence (0.9.1)\n");
    {
        const char* order[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                               "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};          // the example list of the specification
        bool ok = true;
        for (int i = 0; i < 8; ++i) for (int k = 0; k < 8; ++k) {
            const int c = Cmp(order[i], order[k]);
            if (c == 99 || (i < k && c >= 0) || (i > k && c <= 0) || (i == k && c != 0)) { ok = false; printf("    %s vs %s -> %d\n", order[i], order[k], c); }
        }
        Check(ok, "SemVer example order (alpha < alpha.1 < ... < rc.1 < release)");
    }
    Check(Cmp("0.9.1-rc.10", "0.9.1-rc.9") > 0 && Cmp("0.9.1-rc.2", "0.9.1-rc.10") < 0, "numeric identifiers compare as numbers (rc.10 > rc.9)");
    Check(Cmp("1.0.0-1", "1.0.0-a") < 0 && Cmp("1.0.0-a", "1.0.0-1") > 0, "numeric identifier lower than alphanumeric");
    Check(Cmp("1.0.0-rc.01", "1.0.0-rc.1") == 0 && Cmp("1.0.0-rc.99999999999999999999", "1.0.0-rc.9") > 0, "long and zero-padded numbers, no overflow");
    Check(Cmp("1.0.0-rc", "1.0.0-rc.1") < 0 && Cmp("1.0.0-rc.1.1", "1.0.0-rc.1") > 0, "shorter identifier set is lower");
    Check(Cmp("1.0.0-RC.1", "1.0.0-rc.1") < 0, "alphanumeric identifiers compare in ASCII order");
    Check(!ParseVersion("1.0.0-rc..1", v) && !ParseVersion("1.0.0-.rc", v) && !ParseVersion("1.0.0-rc.", v), "empty pre-release identifiers rejected");
    Check(ComparePre("", "") == 0 && ComparePre("a", "") > 0 && ComparePre("", "a") < 0, "ComparePre edge cases");

    // 0.9.1 (fix 5): final host allow-list after redirects
    printf("Final host allow-list (0.9.1)\n");
    Check(AllowedFinalHost(false, L"api.github.com") && AllowedFinalHost(false, L"API.GitHub.com"), "check: api.github.com (any case)");
    Check(!AllowedFinalHost(false, L"github.com") && !AllowedFinalHost(false, L"objects.githubusercontent.com") &&
          !AllowedFinalHost(false, L"api.github.com.evil.example") && !AllowedFinalHost(false, L""), "check: nothing else");
    Check(AllowedFinalHost(true, L"github.com") && AllowedFinalHost(true, L"objects.githubusercontent.com") &&
          AllowedFinalHost(true, L"release-assets.githubusercontent.com"), "download: github.com and the release file servers");
    Check(!AllowedFinalHost(true, L"api.github.com") && !AllowedFinalHost(true, L"evil-github.com") &&
          !AllowedFinalHost(true, L"github.com.") && !AllowedFinalHost(true, L"x.objects.githubusercontent.com") &&
          !AllowedFinalHost(true, L"raw.githubusercontent.com") && !AllowedFinalHost(true, L"example.com"), "download: no suffix tricks, no other hosts");

    printf("RESULT: %s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
