// WASAPI Driver for ASIO - opt-in update check: portable parts and the Windows worker API (0.9.0, H3; hardened in 0.9.1)
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
//   Portable, tested offline (tests/update_test.cpp): semantic version parsing and comparison, a small JSON reader
//   for the GitHub "latest release" answer (tag_name, name, draft, prerelease, assets[].name, assets[].size,
//   assets[].browser_download_url), SHA256SUMS.txt lookup, repository and URL validation.
//   Windows only (update.cpp): the worker thread that performs the HTTPS requests (WinHTTP), the SHA-256
//   (BCrypt) and the Authenticode check (WinVerifyTrust), all loaded at run time from System32 so the driver DLL
//   imports nothing new and loads no network code until the user asks for a check.
//
// Privacy and safety rules (see PRIVACY.txt / SECURITY.txt)
//   - nothing happens unless the user clicks "Check for updates now" or turned on the weekly check (off by
//     default; then at most once a week, when the control panel is opened);
//   - only https:// URLs; the API URL is built from the configured "owner/repository"; the download URL must
//     start with https://github.com/; redirects may not go to plain HTTP and the final host must be on a short
//     allow-list (AllowedFinalHost); TLS certificate checks are WinHTTP's defaults (never disabled);
//   - requests carry only "User-Agent: WASAPI-Driver-for-ASIO/<version>" and the GitHub Accept header: no
//     identifier, no settings, no telemetry;
//   - size limits (API answer 1 MB, SHA256SUMS.txt 64 KB, Setup 50 MB), timeouts, total deadlines and a cancel;
//   - the Setup is accepted only when its SHA-256 matches SHA256SUMS.txt of the same release, and refused when it
//     carries an Authenticode signature that does not verify; it is never started silently, and the file that is
//     started is the file that was verified (same open handle, no writers allowed; see LaunchVerified).
//
// Threading
//   Portable functions: pure. Job: created on the host thread, shared with one worker thread through a
//   reference count; the worker writes the Result and sets `done` (release), the host reads it after the posted
//   message (acquire). The audio thread is never involved.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace upd {

// ---------------------------------------------------------------- versions
struct Version { int major = 0, minor = 0, patch = 0; bool pre = false; std::string preTag; };

// "v1.2.3", "1.2", "1.2.3-beta.2" (a leading v/V is accepted, missing parts are 0). False for anything else.
inline bool ParseVersion(const std::string& in, Version& v) {
    v = Version{};
    size_t i = 0;
    if (i < in.size() && (in[i] == 'v' || in[i] == 'V')) ++i;
    int part[3] = {0, 0, 0}, n = 0;
    while (n < 3) {
        if (i >= in.size() || in[i] < '0' || in[i] > '9') return false;
        long long x = 0;
        while (i < in.size() && in[i] >= '0' && in[i] <= '9') { x = x * 10 + (in[i] - '0'); if (x > 1000000) return false; ++i; }
        part[n++] = (int)x;
        if (i < in.size() && in[i] == '.' && n < 3) { ++i; continue; }
        break;
    }
    if (i < in.size()) {
        if (in[i] == '-') {
            v.pre = true; v.preTag = in.substr(i + 1);
            if (v.preTag.empty() || v.preTag.size() > 32) return false;
            for (char c : v.preTag) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
            if (v.preTag.front() == '.' || v.preTag.back() == '.' || v.preTag.find("..") != std::string::npos) return false;   // empty identifier
        } else if (in[i] != '+') return false;               // "+build" metadata is ignored
    }
    v.major = part[0]; v.minor = part[1]; v.patch = part[2];
    return true;
}
// Pre-release precedence per SemVer 2.0.0, section 11 (0.9.1, fix 8): the dot-separated identifiers are compared
// left to right; identifiers of digits only compare numerically, others as ASCII text; a numeric identifier is
// lower than an alphanumeric one; when all shared identifiers are equal, the shorter set is lower
// ("rc.2" < "rc.10", "alpha" < "alpha.1" < "alpha.beta" < "beta").
inline int ComparePre(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        size_t ie = a.find('.', i), je = b.find('.', j);
        if (ie == std::string::npos) ie = a.size();
        if (je == std::string::npos) je = b.size();
        const std::string x = a.substr(i, ie - i), y = b.substr(j, je - j);
        auto numeric = [](const std::string& t) { if (t.empty()) return false; for (char c : t) if (c < '0' || c > '9') return false; return true; };
        const bool nx = numeric(x), ny = numeric(y);
        if (nx && ny) {
            size_t zx = 0, zy = 0;                               // compare by value without overflow: strip leading zeros,
            while (zx + 1 < x.size() && x[zx] == '0') ++zx;     // then the longer digit string is the larger number
            while (zy + 1 < y.size() && y[zy] == '0') ++zy;
            const std::string vx = x.substr(zx), vy = y.substr(zy);
            if (vx.size() != vy.size()) return vx.size() < vy.size() ? -1 : 1;
            if (vx != vy) return vx < vy ? -1 : 1;
        } else if (nx != ny) return nx ? -1 : 1;
        else if (x != y) return x < y ? -1 : 1;
        i = ie + 1; j = je + 1;                                  // past the end once the last identifier is used
    }
    const bool ea = i >= a.size(), eb = j >= b.size();
    if (ea && eb) return 0;
    return ea ? -1 : 1;
}
// <0, 0, >0. A pre-release is lower than the same version without one; pre-releases compare with ComparePre.
inline int Compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre != b.pre) return a.pre ? -1 : 1;
    return a.pre ? ComparePre(a.preTag, b.preTag) : 0;
}

// ---------------------------------------------------------------- JSON (only what the release answer needs)
struct Asset { std::string name, url; long long size = -1; };
struct Release { std::string tag, name; bool draft = false, prerelease = false; std::vector<Asset> assets; };

// Minimal, bounded JSON reader: walks the whole document (objects, arrays, strings with escapes, numbers,
// true/false/null), depth limit 32, and stores the release fields when it meets them at the expected places.
// Returns false on malformed input (the caller then reports "unexpected answer"). Strings are kept as UTF-8;
// \uXXXX escapes outside ASCII are kept as '?', enough for tags, names and URLs.
class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}
    bool parseRelease(Release& r) {
        r = Release{};
        rel_ = &r; pos_ = 0;
        skipWs();
        if (!value(0, Ctx::Root)) return false;
        skipWs();
        return pos_ == s_.size();
    }
private:
    enum class Ctx { Root, Top, Assets, Asset, Other };
    const std::string& s_;
    size_t pos_ = 0;
    Release* rel_ = nullptr;

    void skipWs() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_; }
    bool str(std::string& out) {
        out.clear();
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { if (out.size() < 4096) out += c; continue; }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
            case '"': case '\\': case '/': out += e; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                if (pos_ + 4 > s_.size()) return false;
                unsigned v = 0;
                for (int k = 0; k < 4; ++k) {
                    const char h = s_[pos_++]; v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0'); else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10); else return false;
                }
                out += v < 0x80 ? (char)v : '?';
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool number(long long* out) {
        const size_t b = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        bool digits = false;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) { digits = true; ++pos_; }
        if (!digits) return false;
        if (out) {
            long long x = 0; bool ok = true;
            for (size_t i = b; i < pos_; ++i) { if (s_[i] < '0' || s_[i] > '9') { ok = false; break; } if (x < (1LL << 50)) x = x * 10 + (s_[i] - '0'); }
            *out = ok ? x : -1;
        }
        return true;
    }
    bool literal(const char* w) {
        const size_t n = strlen(w);
        if (s_.compare(pos_, n, w) != 0) return false;
        pos_ += n; return true;
    }
    // One value in context `ctx`; for objects the keys decide the context of the children. A field that is
    // stored must have the expected type (string, string or null, number, boolean); anything else is malformed.
    enum Need { kAny, kStr, kStrOrNull, kNum, kBool };
    bool value(int depth, Ctx ctx, std::string* sOut = nullptr, long long* nOut = nullptr, bool* bOut = nullptr, Need need = kAny) {
        if (depth > 32 || pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (need == kStr && c != '"') return false;
        if (need == kStrOrNull && c != '"' && c != 'n') return false;
        if (need == kNum && c != '-' && (c < '0' || c > '9')) return false;
        if (need == kBool && c != 't' && c != 'f') return false;
        if (c == '"') { std::string t; if (!str(t)) return false; if (sOut) *sOut = t; return true; }
        if (c == 't') { if (!literal("true")) return false; if (bOut) *bOut = true; return true; }
        if (c == 'f') { if (!literal("false")) return false; if (bOut) *bOut = false; return true; }
        if (c == 'n') return literal("null");
        if (c == '-' || (c >= '0' && c <= '9')) return number(nOut);
        if (c == '[') {
            ++pos_; skipWs();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            while (true) {
                skipWs();
                if (ctx == Ctx::Assets) {
                    if (pos_ < s_.size() && s_[pos_] == '{' && rel_->assets.size() < 256) {
                        rel_->assets.push_back(Asset{});
                        if (!value(depth + 1, Ctx::Asset)) return false;
                    } else if (!value(depth + 1, Ctx::Other)) return false;
                } else if (!value(depth + 1, Ctx::Other)) return false;
                skipWs();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '{') {
            ++pos_; skipWs();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            const Ctx inner = ctx == Ctx::Root ? Ctx::Top : ctx;
            while (true) {
                skipWs();
                std::string key;
                if (!str(key)) return false;
                skipWs();
                if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                ++pos_; skipWs();
                bool ok;
                if (inner == Ctx::Top && key == "tag_name") ok = value(depth + 1, Ctx::Other, &rel_->tag, nullptr, nullptr, kStr);
                else if (inner == Ctx::Top && key == "name") ok = value(depth + 1, Ctx::Other, &rel_->name, nullptr, nullptr, kStrOrNull);
                else if (inner == Ctx::Top && key == "draft") ok = value(depth + 1, Ctx::Other, nullptr, nullptr, &rel_->draft, kBool);
                else if (inner == Ctx::Top && key == "prerelease") ok = value(depth + 1, Ctx::Other, nullptr, nullptr, &rel_->prerelease, kBool);
                else if (inner == Ctx::Top && key == "assets") ok = (pos_ < s_.size() && s_[pos_] == '[') && value(depth + 1, Ctx::Assets);
                else if (inner == Ctx::Asset && key == "name") ok = value(depth + 1, Ctx::Other, &rel_->assets.back().name, nullptr, nullptr, kStr);
                else if (inner == Ctx::Asset && key == "browser_download_url") ok = value(depth + 1, Ctx::Other, &rel_->assets.back().url, nullptr, nullptr, kStr);
                else if (inner == Ctx::Asset && key == "size") ok = value(depth + 1, Ctx::Other, nullptr, &rel_->assets.back().size, nullptr, kNum);
                else ok = value(depth + 1, Ctx::Other);
                if (!ok) return false;
                skipWs();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
        }
        return false;
    }
};
inline bool ParseRelease(const std::string& json, Release& r) {
    if (json.size() > (1u << 20)) return false;
    JsonReader j(json);
    return j.parseRelease(r) && !r.tag.empty();
}

// ---------------------------------------------------------------- assets, checksums, URLs, repository
// Name of the Setup asset ("WASAPI-Driver-for-ASIO-<version>-Setup.exe"), or "" if the release has none.
inline std::string SetupAssetName(const Release& r) {
    const std::string pre = "WASAPI-Driver-for-ASIO-", suf = "-Setup.exe";
    for (const Asset& a : r.assets)
        if (a.name.size() > pre.size() + suf.size() && a.name.compare(0, pre.size(), pre) == 0 &&
            a.name.compare(a.name.size() - suf.size(), suf.size(), suf) == 0 && a.name.find_first_of("/\\:") == std::string::npos)
            return a.name;
    return "";
}
inline const Asset* FindAsset(const Release& r, const std::string& name) {
    for (const Asset& a : r.assets) if (a.name == name) return &a;
    return nullptr;
}
// Download URLs must be https://github.com/... (GitHub then redirects to its file servers over HTTPS).
inline bool AllowedDownloadUrl(const std::string& u) {
    const std::string p = "https://github.com/";
    if (u.compare(0, p.size(), p) != 0 || u.size() > 2048) return false;
    for (char c : u) if ((unsigned char)c <= ' ' || c == '"' || c == '\\' || (unsigned char)c >= 0x7F) return false;
    return true;
}
// Final host of a request after redirects (0.9.1, fix 5): the check may only end on api.github.com; downloads only
// on github.com or GitHub's release file servers. Compared case-insensitively, exact names (no suffix match).
inline bool AllowedFinalHost(bool download, const std::wstring& host) {
    std::wstring h;
    for (wchar_t c : host) h += (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
    if (!download) return h == L"api.github.com";
    return h == L"github.com" || h == L"objects.githubusercontent.com" || h == L"release-assets.githubusercontent.com";
}
// SHA-256 (64 lowercase hex characters) of `file` in a SHA256SUMS.txt ("<hex>  <name>" or "<hex> *<name>").
inline bool FindSha256(const std::string& sums, const std::string& file, std::string& hex) {
    size_t i = 0;
    while (i < sums.size()) {
        size_t e = sums.find('\n', i);
        if (e == std::string::npos) e = sums.size();
        std::string line = sums.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 66) continue;
        std::string h = line.substr(0, 64);
        bool ok = true;
        for (char& c : h) { if (c >= 'A' && c <= 'F') c = (char)(c + 32); if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) ok = false; }
        if (!ok || (line[64] != ' ' && line[64] != '\t')) continue;
        size_t n = 65;
        while (n < line.size() && (line[n] == ' ' || line[n] == '\t' || line[n] == '*')) ++n;
        if (line.substr(n) == file) { hex = h; return true; }
    }
    return false;
}
// "owner/repository" with GitHub's allowed characters.
inline bool ValidRepo(const std::wstring& r) {
    const size_t sl = r.find(L'/');
    if (sl == std::wstring::npos || sl == 0 || sl + 1 >= r.size() || r.size() > 140 || r.find(L'/', sl + 1) != std::wstring::npos) return false;
    for (wchar_t c : r) if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.' || c == L'/')) return false;
    return r.compare(0, 2, L"..") != 0;
}
// True while the update source is not a real repository: anything invalid, the pre-0.9.7 default "OWNER/...", or
// (0.9.7) an owner that still contains the word "PLACEHOLDER" in any letter case, which is how the prepared source
// tree names the not-yet-known GitHub account (common.h WB_UPDATE_REPO). Every caller (StartCheck, the panel's
// buttons and the weekly check) refuses such a source, so no request is ever made to api.github.com for it.
// Only the owner part (before the '/') is examined, so a repository name containing the word is still accepted.
inline bool IsPlaceholderRepo(const std::wstring& r) {
    if (!ValidRepo(r)) return true;
    std::wstring owner = r.substr(0, r.find(L'/'));
    for (auto& c : owner) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');   // ASCII upper case
    return owner == L"OWNER" || owner.find(L"PLACEHOLDER") != std::wstring::npos;
}

// Helpers for the UI (ASCII only for the log; release texts are shown as plain characters).
inline std::wstring Widen(const std::string& s) { std::wstring w; for (char c : s) w += (wchar_t)(unsigned char)c; return w; }
inline std::string SafeAscii(const std::string& s) { std::string r; for (char c : s) r += (c >= 32 && c < 127) ? c : '_'; return r.substr(0, 64); }

// ---------------------------------------------------------------- Windows worker (update.cpp)
enum Op { kOpCheck = 0, kOpDownload };
enum Error { kOk = 0, kErrUnavailable, kErrNet, kErrHttp, kErrParse, kErrNoAsset, kErrSize, kErrFile, kErrHash, kErrSignature,
             kErrHost, kErrCancelled, kErrTimeout };
struct Result {
    Op op = kOpCheck;
    Error error = kOk;
    unsigned long detail = 0;       // Win32 / WinHTTP error code or HTTP status
    Release release;                // check
    std::wstring path;              // download: verified Setup
    std::string sha256;             // download: its SHA-256
    bool signedValid = false;       // download: Authenticode signature present and valid
};
struct Job;                          // opaque (update.cpp)
#ifdef _WIN32
// Host thread. Start the worker; nullptr if it could not be started. The job posts `msg` to `notify` when done.
// Total deadlines (fix 4): 120 s for a check, 10 minutes for a download.
Job* StartCheck(void* notify, unsigned msg, const std::wstring& repo);
Job* StartDownload(void* notify, unsigned msg, const Release& rel, const std::wstring& folder);
bool Done(const Job* j);                       // acquire
const Result& ResultOf(const Job* j);          // only after Done()
void Cancel(Job* j);                           // host thread: asks the worker to stop and aborts its open request (fix 4)
void Unref(Job* j);                            // drops the caller's reference
// Host thread (fix 1). Opens the Setup with GENERIC_READ and FILE_SHARE_READ only (nobody can write, rename or
// delete it while the handle is open), hashes it from that handle, verifies its Authenticode signature from the
// same handle (refused if a signature is present and invalid, or if `mustBeSigned` and there is none), starts it
// with ShellExecuteExW and closes the handle only once the new process exists.
enum Launch { kLaunchOk = 0, kLaunchHash, kLaunchSignature, kLaunchFailed };
Launch LaunchVerified(void* owner, const std::wstring& path, const std::string& sha256, bool mustBeSigned);
#endif

}  // namespace upd
