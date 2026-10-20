// WASAPI Driver for ASIO - opt-in update check: Windows worker thread (0.9.0, H3; hardened in 0.9.1)
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
//   The network part of the update check (rules in update.h). One worker thread per job:
//     check:    GET https://api.github.com/repos/<owner>/<repo>/releases/latest -> Release
//     download: GET SHA256SUMS.txt and the Setup of that release into %LOCALAPPDATA%\WASAPI Driver for ASIO\updates,
//               SHA-256 of the Setup must match, an Authenticode signature (if any) must verify.
//   0.9.1: the final host of every request (after redirects) must be on the allow-list (fix 5); the .part file
//   stays open without sharing from the first byte to the signature check and is renamed through that same
//   handle (fix 1); LaunchVerified hashes, verifies and starts the Setup from one read-only handle that denies
//   writers until the process exists (fix 1); a job can be cancelled and has a total deadline (fix 4).
//   winhttp.dll, bcrypt.dll and wintrust.dll are loaded from System32 at run time (LoadLibraryExW with
//   LOAD_LIBRARY_SEARCH_SYSTEM32) only when a job starts: the DAW never loads network code because of the driver
//   unless the user asks for a check.
//
// Threading and lifetime
//   StartCheck / StartDownload run on the host thread (the control panel). The worker never touches the driver,
//   the panel or the configuration: it writes only into its Job and posts one message to the panel's window when
//   finished (PostMessage to a destroyed window simply fails). The Job is reference counted (host + worker); the
//   last Unref frees it. The reference on this DLL is taken by Start() on the host thread BEFORE CreateThread
//   (fix 2: no window in which the DLL could be unloaded before the worker runs its first instruction), handed to
//   the worker, and released by the worker with FreeLibraryAndExitThread; Start() releases it itself if
//   CreateThread fails. g_objects counts each running job (DllCanUnloadNow).
//   Cancel (host thread) sets a flag and closes the request handle the worker is blocked on; the handle is
//   published through an atomic exchanged by whoever closes it, so it is closed exactly once. Cancel takes its
//   own reference on winhttp.dll first, so the worker cannot unload it in between. The audio thread is never
//   involved.
//
// Reading guide
//   Job/Done/ResultOf/Unref/Cancel  shared state between the host thread and one worker, and its lifetime.
//   WinHttp/Sha256/VerifySignature  thin run-time bindings to System32 DLLs (no import-table dependency).
//   Get                             one HTTPS GET with allow-list, cancellation, deadline and size limit.
//   DoCheck/DoDownload/Worker/Start the two job kinds and the thread that runs them.
//   LaunchVerified                  host-thread hash + signature check and start of the downloaded Setup.
// Error reporting: every failure ends up as Result::error (+ optional Result::detail with a Win32/WinHTTP code
// or the HTTP status); nothing throws and nothing is logged from the worker.
#include "common.h"
#include "update.h"
#include "driver.h"                  // g_objects
#include "trace.h"                   // 0.9.8: update-check result event (no URL, no path)
#include <winhttp.h>
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <shellapi.h>
#include <new>                       // std::nothrow (HoldAndClose, 0.9.6)

namespace upd {

// One update job. Ownership: created by StartCheck/StartDownload with two references (host + worker). Fields
// written by the host before CreateThread (op, notify, msg, repo, folder, rel, self, deadline) are read-only
// afterwards; `res` is written only by the worker and published to the host by `done` (release/acquire).
// `cancel` and `active` are the only fields both threads touch concurrently, hence atomic.
struct Job {
    LONG refs = 2;                   // host + worker
    Op op = kOpCheck;
    HWND notify = nullptr; UINT msg = 0;
    std::wstring repo, folder;
    upd::Release rel;                // download: the release found by the check
    Result res;
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};             // fix 4: set by Cancel (host thread)
    std::atomic<void*> active{nullptr};          // fix 4: request handle the worker waits on; closed once by the exchanger
    HMODULE self = nullptr;                      // fix 2: DLL reference taken by Start, released by the worker
    ULONGLONG deadline = 0;                      // fix 4: GetTickCount64 limit of the whole job
};

// Host-side accessors. ResultOf must only be called once Done() returned true (the acquire in Done pairs with the
// release store in Worker, making every write to `res` visible). Unref deletes the job on the last reference,
// which may happen on either thread (host Unref after the worker finished, or worker Unref after the host left).
bool Done(const Job* j) { return j && j->done.load(std::memory_order_acquire); }
const Result& ResultOf(const Job* j) { return j->res; }
void Unref(Job* j) { if (j && InterlockedDecrement(&j->refs) == 0) delete j; }

// Host thread, any time while the host still holds its reference. Two-step cancellation:
//   1. set `cancel`: the worker's Stop() checks see it between requests and between read chunks;
//   2. abort the WinHTTP call the worker is blocked in, by closing its request handle.
// `active` is non-null only while the worker has winhttp.dll loaded (set after WinHttp::load, cleared before the
// WinHttp destructor frees it). GetModuleHandleExW without UNCHANGED_REFCOUNT pins winhttp.dll for the duration
// of the close; if the module is not loaded at all, there is no handle to close and step 1 is enough.
// Whoever wins exchange(nullptr) on `active` closes the handle; the other side skips it (no double close).
// Pitfall: after the close, the worker may still call WinHTTP with the (now invalid) handle value; those calls
// fail and Get maps the failure to kErrCancelled because `cancel` is already set.
void Cancel(Job* j) {
    if (!j || Done(j)) return;
    j->cancel.store(true);
    HMODULE wh = nullptr;            // winhttp.dll is loaded while `active` can be set; keep it loaded during the close
    if (!GetModuleHandleExW(0, L"winhttp.dll", &wh) || !wh) return;
    if (void* h = j->active.exchange(nullptr)) {
        auto close = (decltype(&WinHttpCloseHandle))(void*)GetProcAddress(wh, "WinHttpCloseHandle");
        if (close) close(h);         // the worker's blocked call fails with ERROR_WINHTTP_OPERATION_CANCELLED
    }
    FreeLibrary(wh);
}

// ---------------------------------------------------------------- system DLLs (run-time loading)
// LoadLibraryExW with LOAD_LIBRARY_SEARCH_SYSTEM32: only %windir%\System32 is searched, never the host's folder
// or the current directory (DLL planting). Every successful call must be balanced by FreeLibrary (the RAII
// structs below do that).
static HMODULE Sys(const wchar_t* name) { return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); }

// Function table for winhttp.dll. decltype(&WinHttpX) uses the SDK prototypes for type safety while the DLL
// itself is not linked. load() resolves all entries or reports failure (-> kErrUnavailable); the destructor
// frees the module in both cases. Lives on the worker's stack for the duration of one job.
struct WinHttp {
    HMODULE m = nullptr;
    decltype(&WinHttpOpen) Open = nullptr;
    decltype(&WinHttpConnect) Connect = nullptr;
    decltype(&WinHttpOpenRequest) OpenRequest = nullptr;
    decltype(&WinHttpSendRequest) SendRequest = nullptr;
    decltype(&WinHttpReceiveResponse) ReceiveResponse = nullptr;
    decltype(&WinHttpQueryHeaders) QueryHeaders = nullptr;
    decltype(&WinHttpQueryOption) QueryOption = nullptr;
    decltype(&WinHttpReadData) ReadData = nullptr;
    decltype(&WinHttpCloseHandle) CloseHandle = nullptr;
    decltype(&WinHttpSetTimeouts) SetTimeouts = nullptr;
    decltype(&WinHttpSetOption) SetOption = nullptr;
    decltype(&WinHttpCrackUrl) CrackUrl = nullptr;
    bool load() {
        m = Sys(L"winhttp.dll");
        if (!m) return false;
        // The lambda casts the FARPROC through void* to the exact pointer type of the member it is assigning to.
        auto g = [&](auto& f, const char* n) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>((void*)GetProcAddress(m, n)); return f != nullptr; };
        return g(Open, "WinHttpOpen") && g(Connect, "WinHttpConnect") && g(OpenRequest, "WinHttpOpenRequest") &&
               g(SendRequest, "WinHttpSendRequest") && g(ReceiveResponse, "WinHttpReceiveResponse") &&
               g(QueryHeaders, "WinHttpQueryHeaders") && g(QueryOption, "WinHttpQueryOption") && g(ReadData, "WinHttpReadData") &&
               g(CloseHandle, "WinHttpCloseHandle") && g(SetTimeouts, "WinHttpSetTimeouts") && g(SetOption, "WinHttpSetOption") &&
               g(CrackUrl, "WinHttpCrackUrl");
    }
    ~WinHttp() { if (m) FreeLibrary(m); }
};

// SHA-256 with BCrypt. feed() may be called many times; finish() returns 64 lowercase hex characters.
// Owns one BCrypt SHA-256 algorithm provider and one hash object; single use (BCryptFinishHash finalizes the hash
// object, so finish() must be called at most once). Any NTSTATUS other than 0 (STATUS_SUCCESS) clears `ok`,
// after which feed() is a no-op and finish() fails; callers check `ok` before and finish()'s result after.
// BCryptCreateHash with a null object buffer lets BCrypt allocate the hash object itself (Windows 7+).
struct Sha256 {
    HMODULE m = nullptr;
    decltype(&BCryptOpenAlgorithmProvider) Open = nullptr;
    decltype(&BCryptCreateHash) Create = nullptr;
    decltype(&BCryptHashData) Hash = nullptr;
    decltype(&BCryptFinishHash) Finish = nullptr;
    decltype(&BCryptDestroyHash) Destroy = nullptr;
    decltype(&BCryptCloseAlgorithmProvider) Close = nullptr;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    Sha256() {
        m = Sys(L"bcrypt.dll");
        if (!m) return;
        Open = (decltype(Open))(void*)GetProcAddress(m, "BCryptOpenAlgorithmProvider");
        Create = (decltype(Create))(void*)GetProcAddress(m, "BCryptCreateHash");
        Hash = (decltype(Hash))(void*)GetProcAddress(m, "BCryptHashData");
        Finish = (decltype(Finish))(void*)GetProcAddress(m, "BCryptFinishHash");
        Destroy = (decltype(Destroy))(void*)GetProcAddress(m, "BCryptDestroyHash");
        Close = (decltype(Close))(void*)GetProcAddress(m, "BCryptCloseAlgorithmProvider");
        ok = Open && Create && Hash && Finish && Destroy && Close &&
             Open(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && Create(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    }
    void feed(const void* p, size_t n) { if (ok && n) ok = Hash(h, (PUCHAR)p, (ULONG)n, 0) == 0; }
    bool finish(std::string& hex) {
        unsigned char d[32];
        if (!ok || Finish(h, d, 32, 0) != 0) return false;
        static const char* x = "0123456789abcdef";
        hex.clear();
        for (unsigned char c : d) { hex += x[c >> 4]; hex += x[c & 15]; }
        return true;
    }
    ~Sha256() { if (h) Destroy(h); if (alg) Close(alg, 0); if (m) FreeLibrary(m); }
};

// Moves the file pointer to offset 0; the hash and the signature check both read from the start of a handle
// that was just written to or is shared between the two checks.
static bool Rewind(HANDLE f) { LARGE_INTEGER z = {}; return SetFilePointerEx(f, z, nullptr, FILE_BEGIN) != 0; }

// SHA-256 of an open file, read from its start through this handle (fix 1).
// Used by LaunchVerified (host thread). 64 KiB chunks; a ReadFile error ends the loop like EOF, so a short read
// produces a wrong hash and therefore a mismatch (fail-safe), never a false match.
static bool Sha256Handle(HANDLE f, std::string& hex) {
    if (!Rewind(f)) return false;
    Sha256 s;
    std::vector<unsigned char> buf(65536);
    DWORD got = 0;
    while (s.ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) s.feed(buf.data(), got);
    return s.ok && s.finish(hex);
}

// Authenticode through an open handle (WTD_CHOICE_FILE with hFile: the bytes checked are the bytes of this open
// file, not of whatever the path names later). 1 = signed and valid, 0 = no signature, -1 = present, not valid.
// Run on the worker (download) and on the host thread (launch). Details:
//   - WTD_UI_NONE and hwnd INVALID_HANDLE_VALUE: never show trust UI.
//   - WTD_REVOKE_NONE: no revocation check, so no CRL/OCSP network traffic beyond the user's explicit request.
//   - WTD_STATEACTION_VERIFY must be paired with WTD_STATEACTION_CLOSE to release the state WinVerifyTrust keeps.
//   - "no signature" covers TRUST_E_NOSIGNATURE and the two codes returned for files whose type has no SIP
//     (subject form / provider unknown); every other failure (bad hash, untrusted root, expired, ...) is -1.
// Pitfall: if wintrust.dll or WinVerifyTrust cannot be loaded the result is 0 (treated as unsigned), so the
// SHA-256 match against SHA256SUMS.txt remains the mandatory check; only mustBeSigned in LaunchVerified can
// reject an unsigned Setup.
static int VerifySignature(const std::wstring& path, HANDLE f) {
    if (!Rewind(f)) return -1;
    HMODULE m = Sys(L"wintrust.dll");
    if (!m) return 0;
    auto fn = (decltype(&WinVerifyTrust))(void*)GetProcAddress(m, "WinVerifyTrust");
    int r = 0;
    if (fn) {
        WINTRUST_FILE_INFO fi = {}; fi.cbStruct = sizeof fi; fi.pcwszFilePath = path.c_str(); fi.hFile = f;
        WINTRUST_DATA wd = {}; wd.cbStruct = sizeof wd; wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
        wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG st = fn((HWND)INVALID_HANDLE_VALUE, &action, &wd);
        wd.dwStateAction = WTD_STATEACTION_CLOSE; fn((HWND)INVALID_HANDLE_VALUE, &action, &wd);
        if (st == 0) r = 1;
        else if (st == (LONG)TRUST_E_NOSIGNATURE || st == (LONG)TRUST_E_SUBJECT_FORM_UNKNOWN || st == (LONG)TRUST_E_PROVIDER_UNKNOWN) r = 0;
        else r = -1;
    }
    FreeLibrary(m);
    return r;
}

// Marks an open file (opened with DELETE access) for deletion; it disappears when the handle is closed.
// Sets the delete disposition; errors are ignored (worst case a stale .part or Setup stays in the updates
// folder and is overwritten by the next download via CREATE_ALWAYS / ReplaceIfExists).
static void DeleteOnClose(HANDLE f) {
    FILE_DISPOSITION_INFO d = {}; d.DeleteFile = TRUE;
    SetFileInformationByHandle(f, FileDispositionInfo, &d, sizeof d);
}
// Renames an open file (opened with DELETE access) through its handle, replacing an existing target (fix 1).
// FILE_RENAME_INFO is variable length: FileName is a flexible array, so the buffer is sized for the header plus
// the name plus one terminator (zero-filled). `target` is a full path, so RootDirectory is null. Because the
// rename goes through the open handle, no other process can swap the file between download and verification.
static bool RenameByHandle(HANDLE f, const std::wstring& target) {
    const size_t bytes = target.size() * sizeof(wchar_t);
    std::vector<unsigned char> buf(sizeof(FILE_RENAME_INFO) + bytes + sizeof(wchar_t), 0);
    FILE_RENAME_INFO* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    ri->ReplaceIfExists = TRUE;
    ri->RootDirectory = nullptr;
    ri->FileNameLength = (DWORD)bytes;
    memcpy(ri->FileName, target.c_str(), bytes);
    return SetFileInformationByHandle(f, FileRenameInfo, ri, (DWORD)buf.size()) != 0;
}

// ---------------------------------------------------------------- HTTPS GET
// Checked before every network step and every chunk. Cancel wins over timeout. The deadline is absolute
// (GetTickCount64, no wrap) and covers the whole job, not just one request.
static bool Stop(Job* j, Error& err) {
    if (j->cancel.load()) { err = kErrCancelled; return true; }
    if (GetTickCount64() > j->deadline) { err = kErrTimeout; return true; }
    return false;
}
// Extracts the host of a (final, post-redirect) URL; false unless the scheme is https. Hosts longer than 255
// characters make WinHttpCrackUrl fail (buffer too small), which callers treat as "not allowed".
static bool HostOf(WinHttp& w, const wchar_t* url, std::wstring& host) {
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof uc;
    wchar_t h[256] = L"";
    uc.lpszHostName = h; uc.dwHostNameLength = 255;
    uc.dwUrlPathLength = (DWORD)-1;                      // not needed; let WinHTTP point into `url`
    if (!w.CrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    host = h;
    return true;
}
// Worker thread. Exactly one of `body` / `file` is used by the callers: check and SHA256SUMS.txt read into memory
// (`body`), the Setup streams into `file` with `sha` hashing the same bytes as they arrive. `max` bounds the total
// bytes actually received (independent of Content-Length). `api` adds the GitHub Accept header.
// Security sequence: https-only parse -> allow-listed initial host -> send -> allow-listed final host after
// redirects (WINHTTP_OPTION_URL) -> status 200 -> bounded read. Handles: session `s`, connection `c`, request
// `r`; `r` is published in j->active so Cancel can abort a blocking call; all three are closed on every path.
// Return value: true only for a complete 200 body within limits; otherwise `err` (and possibly `detail`) is set.
// Reads `url` (https only) into `body` (max bytes) or streams it into `file` (hashing it). Fills err/detail.
static bool Get(Job* j, WinHttp& w, const std::wstring& url, bool api, size_t max, std::string* body, HANDLE file, Sha256* sha,
                Error& err, unsigned long& detail) {
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof uc;
    wchar_t host[256] = L"", path[2048] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 255; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!w.CrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) { err = kErrParse; return false; }
    if (!AllowedFinalHost(j->op == kOpDownload, host)) { err = kErrHost; return false; }
    if (Stop(j, err)) return false;
    const std::wstring ua = std::wstring(L"WASAPI-Driver-for-ASIO/") + WB_VERSION_W;
    // A WinHTTP session per request: simple, and no state (cookies, connections) is reused between requests.
    // Automatic proxy (Windows 8.1+) falls back to the system default proxy setting.
    HINTERNET s = w.Open(ua.c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = w.Open(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = kErrNet; detail = GetLastError(); return false; }
    // Timeouts in ms: resolve 10 s, connect 10 s, send 15 s, receive 30 s (per operation; the job deadline is the
    // overall bound). Redirects are followed automatically, but never from https to http.
    w.SetTimeouts(s, 10000, 10000, 15000, 30000);
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    w.SetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);
    bool ok = false;
    HINTERNET c = w.Connect(s, host, uc.nPort, 0);
    HINTERNET r = c ? w.OpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r) j->active.store(r);                          // from here Cancel may close `r` (fix 4)
    const wchar_t* hdr = api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!r) { err = kErrNet; detail = GetLastError(); }
    else if (Stop(j, err)) {}
    else if (!w.SendRequest(r, hdr, api ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !w.ReceiveResponse(r, nullptr)) {
        if (!Stop(j, err)) { err = kErrNet; detail = GetLastError(); }
    } else {
        // fix 5: where did the redirects end? Only allow-listed hosts, https only.
        wchar_t fin[2048] = L""; DWORD flen = sizeof fin;
        std::wstring fhost;
        DWORD code = 0, sz = sizeof code;
        if (!w.QueryOption(r, WINHTTP_OPTION_URL, fin, &flen) || !HostOf(w, fin, fhost) || !AllowedFinalHost(j->op == kOpDownload, fhost))
            err = kErrHost;
        else if (w.QueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX), code != 200) {
            // QueryHeaders runs via the comma operator; if it fails, `code` stays 0 -> kErrHttp, detail 0.
            err = kErrHttp; detail = code;
        } else {
            std::vector<char> buf(65536);
            size_t total = 0; ok = true;
            while (true) {
                if (Stop(j, err)) { ok = false; break; }
                DWORD got = 0;
                if (!w.ReadData(r, buf.data(), (DWORD)buf.size(), &got)) { if (!Stop(j, err)) { err = kErrNet; detail = GetLastError(); } ok = false; break; }
                if (!got) break;
                total += got;
                if (total > max) { err = kErrSize; ok = false; break; }
                if (body) body->append(buf.data(), got);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD wr = 0;
                    if (!WriteFile(file, buf.data(), got, &wr, nullptr) || wr != got) { err = kErrFile; ok = false; break; }
                }
                if (sha) sha->feed(buf.data(), got);
            }
        }
    }
    // Release the request handle only if Cancel has not already taken (and closed) it.
    if (r && j->active.exchange(nullptr) == r) w.CloseHandle(r);   // else Cancel already closed it
    if (c) w.CloseHandle(c);
    w.CloseHandle(s);
    return ok;
}

// ---------------------------------------------------------------- worker
// Worker. API answer limited to 1 MiB; a well-formed answer without tag_name is reported as kErrParse.
// The release is not filtered here (draft/prerelease/version comparison is up to the caller on the host thread).
static void DoCheck(Job* j, WinHttp& w) {
    std::string body;
    const std::wstring url = L"https://api.github.com/repos/" + j->repo + L"/releases/latest";
    if (!Get(j, w, url, true, 1u << 20, &body, INVALID_HANDLE_VALUE, nullptr, j->res.error, j->res.detail)) return;
    if (!ParseRelease(body, j->res.release)) j->res.error = kErrParse;
}

// Worker. Steps: pick the Setup asset and SHA256SUMS.txt of the same release, validate both URLs
// (https://github.com/ prefix), reject a declared size > 50 MiB up front, fetch the checksum list (64 KiB max),
// then stream the Setup into "<name>.part" while hashing it. On hash match: flush, rename to the final name
// through the same handle, verify the Authenticode signature through that handle, close. Any failure marks the
// file delete-on-close (the .part, or the renamed file if the failure comes after the rename) and closes it,
// so a rejected download never remains on disk under a name that LaunchVerified could be given.
// Note: the hash of the download is computed from the received bytes, not re-read from disk; LaunchVerified
// hashes the file again from disk before starting it.
static void DoDownload(Job* j, WinHttp& w) {
    const std::string setup = SetupAssetName(j->rel);
    const Asset* a = setup.empty() ? nullptr : FindAsset(j->rel, setup);
    const Asset* sums = FindAsset(j->rel, "SHA256SUMS.txt");
    if (!a || !sums || !AllowedDownloadUrl(a->url) || !AllowedDownloadUrl(sums->url)) { j->res.error = kErrNoAsset; return; }
    if (a->size > 50LL * 1024 * 1024) { j->res.error = kErrSize; return; }
    std::string sumText, expect;
    if (!Get(j, w, Widen(sums->url), false, 64 * 1024, &sumText, INVALID_HANDLE_VALUE, nullptr, j->res.error, j->res.detail)) return;
    if (!FindSha256(sumText, setup, expect)) { j->res.error = kErrNoAsset; return; }
    CreateDirectoryW(j->folder.c_str(), nullptr);
    const std::wstring final = j->folder + L"\\" + Widen(setup), part = final + L".part";
    // share mode 0: no other opener (reader or writer) for the lifetime of `f`; DELETE access is needed for the
    // rename and the delete disposition via SetFileInformationByHandle.
    // fix 1: one handle, no sharing, from the first byte to the signature check; renamed through the handle.
    HANDLE f = CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { j->res.error = kErrFile; j->res.detail = GetLastError(); return; }
    auto fail = [&](Error e, unsigned long d) { j->res.error = e; if (d) j->res.detail = d; DeleteOnClose(f); CloseHandle(f); };
    Sha256 sha;
    const bool ok = sha.ok && Get(j, w, Widen(a->url), false, 50u * 1024 * 1024, nullptr, f, &sha, j->res.error, j->res.detail);
    std::string got;
    if (!ok || !sha.finish(got)) { fail(j->res.error ? j->res.error : kErrUnavailable, 0); return; }
    if (got != expect) { fail(kErrHash, 0); return; }
    if (!FlushFileBuffers(f) || !RenameByHandle(f, final)) { fail(kErrFile, GetLastError()); return; }
    const int sig = VerifySignature(final, f);
    if (sig < 0) { fail(kErrSignature, 0); return; }
    CloseHandle(f);
    j->res.signedValid = sig > 0;
    j->res.path = final;
    j->res.sha256 = got;
}

// Thread entry point. The WinHttp table is scoped so that winhttp.dll is freed before the job is published.
// Exit order matters:
//   1. done.store(release) publishes `res`; 2. post the completion message (a destroyed window just makes
//   PostMessageW fail); 3. drop the worker's job reference (the job may be freed here if the host already
//   unreferenced it, hence notify/msg were copied first); 4. --g_objects (DllCanUnloadNow may now say yes);
//   5. FreeLibraryAndExitThread drops the DLL reference taken by Start, so the code being executed stays mapped
//   until the thread is gone. The final `return 0` is never reached; it only satisfies the signature.
// 0.9.8 (exception barrier): the work allocates (response body, strings, file names); a std::bad_alloc must not
// escape a thread procedure (it would terminate the host), so it becomes kErrUnavailable and the normal completion
// path below (done flag, notification, reference drops) always runs.
static DWORD WINAPI Worker(void* p) {
    Job* j = (Job*)p;
    const HMODULE self = j->self;        // taken by Start before this thread existed (fix 2)
    try {
        WinHttp w;
        if (!w.load()) j->res.error = kErrUnavailable;
        else if (j->op == kOpCheck) DoCheck(j, w);
        else DoDownload(j, w);
        // Review R6: inside the try block (SafeAscii allocates)
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwUpdate, j->op == kOpCheck ? "UpdateCheck" : "UpdateDownload",
                 wbtrace::I32("Error", (int32_t)j->res.error), wbtrace::U32("Detail", (uint32_t)j->res.detail),
                 wbtrace::Str("Tag", j->res.op == kOpCheck ? SafeAscii(j->res.release.tag).c_str() : ""));
    } catch (...) {
        j->res.error = kErrUnavailable;
    }
    const HWND n = j->notify; const UINT m = j->msg;
    j->done.store(true, std::memory_order_release);
    PostMessageW(n, m, 0, 0);            // fails harmlessly if the panel is gone
    Unref(j);
    --g_objects;
    FreeLibraryAndExitThread(self, 0);
    return 0;
}

// Host thread. Sets the job deadline (120 s check, 600 s download), takes the extra DLL reference that the worker
// will own, counts the job in g_objects and starts the thread. On failure the job is deleted directly (its two
// references were never handed out) and nullptr is returned. The thread handle is closed immediately: nobody
// joins the worker; completion is signaled by `done` and the posted message.
static Job* Start(Job* j) {
    j->deadline = GetTickCount64() + (j->op == kOpCheck ? 120000ULL : 600000ULL);
    // fix 2: the DLL reference exists before the thread does; the worker releases it on exit.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void*)&Worker, &j->self) || !j->self) { delete j; return nullptr; }
    ++g_objects;
    HANDLE t = CreateThread(nullptr, 0, Worker, j, 0, nullptr);
    if (!t) { --g_objects; FreeLibrary(j->self); delete j; return nullptr; }
    CloseHandle(t);
    return j;
}

// Host thread (control panel). Refuses the placeholder/invalid repository so no request is ever made to a
// made-up URL. `notify` is an HWND passed as void* so update.h stays free of windows.h.
Job* StartCheck(void* notify, unsigned msg, const std::wstring& repo) {
    if (IsPlaceholderRepo(repo)) return nullptr;
    Job* j = new (std::nothrow) Job;     // 0.9.8: nullptr = "could not start" (the panel handles it), never a throw
    if (!j) return nullptr;
    j->op = kOpCheck; j->res.op = kOpCheck; j->notify = (HWND)notify; j->msg = msg; j->repo = repo;
    return Start(j);
}

// Host thread. `rel` is copied into the job (the worker must not reference host-owned data). The URLs inside it
// are validated by DoDownload, not here.
Job* StartDownload(void* notify, unsigned msg, const upd::Release& rel, const std::wstring& folder) {
    Job* j = new (std::nothrow) Job;     // 0.9.8: see StartCheck
    if (!j) return nullptr;
    j->op = kOpDownload; j->res.op = kOpDownload; j->notify = (HWND)notify; j->msg = msg; j->rel = rel; j->folder = folder;
    return Start(j);
}

// ---------------------------------------------------------------- launch (fix 1)
// Host thread, after the user confirmed. Re-hashes and re-verifies the file from disk (the file could have been
// replaced between download and launch, e.g. by another process running as the same user) and launches it while
// the read-only, read-share-only handle prevents modification. Return codes: kLaunchHash also covers "cannot
// open". SEE_MASK_NOASYNC makes ShellExecuteExW finish its work before returning (required when the calling
// thread might not pump messages long enough for asynchronous execution); SEE_MASK_NOCLOSEPROCESS returns the
// process handle used by the hold thread below. If the Setup requests elevation, the UAC prompt happens inside
// ShellExecuteExW.
// 0.9.6 (D7, then independent review R1): the panel thread no longer waits. Before, it called WaitForInputIdle
// (up to 3 s) itself. Closing our read handle right after ShellExecuteExW is NOT safe: the mapped image protects
// only the bytes of the running stub; an NSIS Setup reopens its own file ($EXEPATH) by NAME to read the data
// appended to it, and a mapped image can still be RENAMED, so another process of the same user could rename the
// verified file and drop a different one under its name before NSIS opens it. Our handle (FILE_SHARE_READ, no
// FILE_SHARE_DELETE) forbids renames and deletes while it is open. So the file handle and the process handle go
// to a small detached thread (HoldThread) that waits until the Setup is idle (its first window is up and it has
// read its data; at most 10 s) and then closes both. The thread holds its own reference to this DLL (taken with
// GetModuleHandleExW before it starts) and ends with FreeLibraryAndExitThread, so the DLL cannot be unloaded
// under it. If the thread cannot be created, both handles are closed at once (the 0.9.6 D7 behaviour, the
// remaining window is small).
namespace {
struct Hold { HANDLE file; HANDLE proc; HMODULE self; };   // owned by HoldThread
DWORD WINAPI HoldThread(void* arg) {
    Hold* h = (Hold*)arg;
    const HMODULE self = h->self;
    if (h->proc) { WaitForInputIdle(h->proc, 10000); CloseHandle(h->proc); }
    CloseHandle(h->file);
    delete h;
    FreeLibraryAndExitThread(self, 0);          // drops the reference taken by HoldAndClose; never returns
}
// Hands `file` and `proc` (may be null) to HoldThread; on any failure closes them immediately. Never blocks.
void HoldAndClose(HANDLE file, HANDLE proc) {
    Hold* h = new (std::nothrow) Hold{file, proc, nullptr};
    if (h && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void*)&HoldThread, &h->self) && h->self) {
        if (HANDLE t = CreateThread(nullptr, 64 * 1024, HoldThread, h, 0, nullptr)) { CloseHandle(t); return; }
        FreeLibrary(h->self);                   // the thread never started: release its reference here
    }
    delete h;
    if (proc) CloseHandle(proc);
    CloseHandle(file);
}
}  // namespace

Launch LaunchVerified(void* owner, const std::wstring& path, const std::string& sha256, bool mustBeSigned) {
    // Read access, read sharing only: no other process can write, rename or delete the file while `f` is open.
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return kLaunchHash;
    std::string h;
    if (!Sha256Handle(f, h) || h != sha256) { CloseHandle(f); return kLaunchHash; }
    const int sig = VerifySignature(path, f);
    if (sig < 0 || (mustBeSigned && sig == 0)) { CloseHandle(f); return kLaunchSignature; }
    SHELLEXECUTEINFOW se = {}; se.cbSize = sizeof se;
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC; se.hwnd = (HWND)owner; se.lpVerb = L"open";
    se.lpFile = path.c_str(); se.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&se) != 0;
    HoldAndClose(f, ok ? se.hProcess : nullptr);   // R1: keep the file locked until the Setup is idle, off-thread
    return ok ? kLaunchOk : kLaunchFailed;
}

}  // namespace upd
