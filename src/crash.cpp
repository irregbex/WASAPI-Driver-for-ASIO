// WASAPI Driver for ASIO - crash protection (see crash.h)
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
// Purpose: implementation of crash.h. Threading: see crash.h. Nothing here runs on the audio thread except the
// exception handler itself if the audio thread faults (it then only signals an event and waits).
//
// Overview of the data flow
//   CrashInstall (first driver object)  -> computes g_dir and the DLL address range, loads dbghelp, creates the
//                                          events and the dump thread, then registers the vectored handler.
//   Handler (faulting thread)           -> filters, fills g_mei/g_code/g_offset, signals g_req, waits on g_done.
//   DumpThread (dedicated thread)       -> writes the marker ("dump pending"), the .dmp, the marker again, g_done.
//   CrashUninstall (last driver object) -> removes the handler, signals g_quit, drops the thread handle.
// The handler waits at most 5 s for the dump thread (see Handler below; crash.h says the same since 0.9.6, D1).
#include "crash.h"
#include "common.h"
#include <dbghelp.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>     // snprintf (0.9.6, D3)
#include <cstdlib>    // strtoull (0.9.6, D3)
#include <cstring>    // strstr (0.9.6, D3)
#include <cwchar>
#include <new>        // std::nothrow (0.9.6, R3)
#include "trace.h"    // 0.9.8: crash-protection events (stack only, never from the vectored handler itself)

// Internal linkage for everything that only the handler/dump thread machinery needs.
namespace {

// Signature of dbghelp!MiniDumpWriteDump. Resolved with GetProcAddress at install time instead of linking
// against dbghelp.lib, so the DLL has no static import of dbghelp.dll (which the loader could resolve to a copy
// in the host's folder) and still loads if dbghelp is missing.
typedef BOOL (WINAPI* PFN_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                            PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Everything the handler and the dump thread touch is static and prepared at install time (no heap in the
// crash path; the dump thread uses only its own stack).
// Invariant: all globals below are written only by CrashInstall/CrashUninstall (under g_installLock) and, for
// g_mei/g_code/g_offset, by the single handler invocation that wins g_dumping. The handler itself reads them
// without taking the lock (it may run on any thread, including the audio thread, and must not block on an SRW
// lock that a crashed thread could own).
LONG                  g_installRefs = 0;           // H, guarded by g_installLock
SRWLOCK               g_installLock = SRWLOCK_INIT;  // fix 6: install/uninstall may race between driver objects
PVOID                 g_veh = nullptr;             // handler cookie
HANDLE                g_req = nullptr, g_done = nullptr, g_quit = nullptr, g_thread = nullptr;
PFN_MiniDumpWriteDump g_writeDump = nullptr;
uintptr_t             g_modBase = 0, g_modEnd = 0;  // address range of this DLL
volatile LONG         g_dumping = 0;               // first fatal exception wins; later ones are not dumped
MINIDUMP_EXCEPTION_INFORMATION g_mei = {};
DWORD                 g_code = 0;
uintptr_t             g_offset = 0;
wchar_t               g_dir[MAX_PATH] = L"";       // crash folder, computed at install

// Fatal exception codes. Breakpoints, C++ exceptions, debugger notifications and guard pages are not fatal.
// Only these codes trigger a dump. Anything else (e.g. 0xE06D7363 for a C++ throw, DBG_PRINTEXCEPTION_C from
// OutputDebugString, STATUS_GUARD_PAGE_VIOLATION during stack growth) is part of normal operation and must not
// produce a dump or a safe-mode marker. 0xC0000374 has no named constant in older mingw headers, hence the literal.
bool IsFatal(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: case 0xC0000374 /* STATUS_HEAP_CORRUPTION */:
        return true;
    default:
        return false;
    }
}

// Vectored handler (faulting thread). Only static data, one event, one bounded wait.
// Called by the OS for every exception in the process (first-chance), on the thread that raised it, before
// frame-based handlers. Because of that it must be cheap and filter early:
//   1. not fatal, or the faulting instruction is outside [g_modBase, g_modEnd) -> not ours, ignore. A fault in the
//      host or another plugin is not attributed to this driver (an AV in a system DLL called by us is also not
//      caught, by design: the address is the instruction address, not a caller address).
//   2. InterlockedExchange on g_dumping: only the first fatal exception is dumped; concurrent or later faults
//      (e.g. a second thread faulting while the dump is written) pass straight through.
//   3. publish the exception data in static g_mei (ClientPointers = FALSE: the pointers are valid in this
//      process, which is the process being dumped) and wake the dump thread.
// Pitfall: the handler sees first-chance exceptions, so a fault that this DLL itself would catch with a C++ catch
// would still be dumped; the code base does not rely on catching hardware faults (mingw has no SEH __try).
LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (!IsFatal(code) || a < g_modBase || a >= g_modEnd) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&g_dumping, 1)) return EXCEPTION_CONTINUE_SEARCH;
    g_mei.ThreadId = GetCurrentThreadId();
    g_mei.ExceptionPointers = ep;
    g_mei.ClientPointers = FALSE;
    g_code = code;
    g_offset = a - g_modBase;
    // The wait is on either g_done (dump finished) or the thread handle (dump thread already exited, e.g. after
    // g_quit during uninstall), so the faulting thread is never blocked forever. If the install was incomplete
    // (no thread/events) nothing is waited for and only the static fields above were written.
    // Fix 5: also wake up if the dump thread is gone; fix 4: at most 5 s on the faulting thread.
    if (g_req && g_done && g_thread) {
        SetEvent(g_req);
        HANDLE w[2] = {g_done, g_thread};
        WaitForMultipleObjects(2, w, FALSE, 5000);
    }
    return EXCEPTION_CONTINUE_SEARCH;              // never swallow the fault: the host decides
}

// Writes crash-marker.txt (dump thread, stack buffers only). `state`: "dump pending", "dump written" or
// "dump not written". Fix 3: called once before MiniDumpWriteDump (so a dump that hangs or crashes still leaves
// the marker and the next session starts safely) and once after it with the result.
// No heap allocation (the heap may be the thing that is corrupted, cf. STATUS_HEAP_CORRUPTION): fixed wchar_t
// and char buffers, raw CreateFileW/WriteFile. CREATE_ALWAYS overwrites a marker from an earlier crash, so the
// marker always describes the most recent crash. FlushFileBuffers makes the marker durable even if the process
// is terminated right after the handler returns. Errors are ignored: there is nothing useful to do about them.
static void WriteMarker(const SYSTEMTIME& t, const char* state) {
    wchar_t mpath[MAX_PATH + 32];
    swprintf(mpath, MAX_PATH + 32, L"%ls\\crash-marker.txt", g_dir);
    HANDLE m = CreateFileW(mpath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m == INVALID_HANDLE_VALUE) return;
    char line[256];
    const int n = snprintf(line, sizeof line,
        "%s %s crashed: exception 0x%08lX at module offset 0x%llX, %04u-%02u-%02u %02u:%02u:%02u, %s\r\n",
        WB_NAME, WB_VERSION_STR, (unsigned long)g_code, (unsigned long long)g_offset, t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, state);
    DWORD w = 0;
    if (n > 0) WriteFile(m, line, (DWORD)n, &w, nullptr);
    FlushFileBuffers(m);
    CloseHandle(m);
}

// Dump thread: writes the minidump and the marker, using only stack buffers. Also exits on g_quit.
// Created at install time with a 64 KiB stack and parked in a wait, so that at crash time no thread creation and
// no loader work is needed. `self` is an extra module reference (taken in CrashInstall) that keeps the code of this
// function mapped until FreeLibraryAndExitThread drops it; this is what makes it safe for the host to FreeLibrary
// the driver while this thread is still finishing.
// Two exits:
//   - g_req signaled (crash): dump, signal g_done, exit. The events are deliberately not closed (the process is
//     about to die, and the faulting thread may still be waiting on g_done).
//   - g_quit signaled (clean uninstall): close the three events, which only this thread still owns.
// MiniDumpWriteDump suspends the other threads of the process (including the faulting one, which is parked in
// WaitForMultipleObjects) while it walks them; writing from this separate thread avoids running dbghelp on a
// thread whose stack may be exhausted (stack overflow) or whose state is otherwise damaged.
// 0.9.6 (independent review R3): the thread receives its three events and its module reference in a DumpArgs
// block (heap, allocated by CrashInstall at install time, never in the crash path) instead of copying g_req /
// g_done / g_quit after it has started. Before, a quick CrashUninstall (globals nulled) or an Uninstall + Install
// cycle (globals replaced by the next cycle's events) between CreateThread and the first line of the thread could
// give it null handles, or handles of the next cycle that it would later close twice. Now the handles are fixed
// at CreateThread time; this thread is their only owner and closes them on the g_quit path.
struct DumpArgs { HANDLE req, done, quit; HMODULE self; };
DWORD WINAPI DumpThread(void* arg) {
    DumpArgs* a = (DumpArgs*)arg;
    HANDLE hs[2] = {a->req, a->quit};
    HANDLE done = a->done;
    const HMODULE self = a->self;
    delete a;                                     // install time: the heap is fine here (not the crash path)
    // 0.9.8 (wait review): INFINITE on purpose. This thread waits for a crash request or for the quit event of
    // CrashUninstall; it never waits for device IO or for another thread's progress, and the faulting thread's own
    // wait for it is bounded (5 s, Handler). The quit event is always signaled by the last CrashUninstall, so the
    // thread always ends before the DLL can be unloaded (it holds its own module reference).
    const DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0) {
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t path[MAX_PATH + 64];
        // The folder normally exists already (CrashInstall created it); recreated in case the user deleted it.
        CreateDirectoryW(g_dir, nullptr);
        WriteMarker(t, "dump pending");
        swprintf(path, MAX_PATH + 64, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u.dmp", g_dir, t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        BOOL ok = FALSE;
        if (f != INVALID_HANDLE_VALUE) {
            // MiniDumpNormal | MiniDumpWithThreadInfo: stacks of all threads plus thread times/priorities, small
            // enough to be attached to a bug report (no full memory).
            if (g_writeDump)
                ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                 (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo), &g_mei, nullptr, nullptr);
            CloseHandle(f);
        }
        WriteMarker(t, ok ? "dump written" : "dump not written");
        // 0.9.8: one ETW event (stack buffers only, like the rest of this path); written from this thread, never
        // from the faulting thread's handler. Code and module offset only (no memory contents, no paths).
        WB_TRACE(wbtrace::kLvlCritical, wbtrace::kKwCrash, "CrashDump", wbtrace::U32("ExceptionCode", (uint32_t)g_code),
                 wbtrace::U64("ModuleOffset", (uint64_t)g_offset), wbtrace::Bool("DumpWritten", ok != FALSE));
        SetEvent(done);
    } else {
        // Clean uninstall: the handler is already removed, nobody else uses these events any more.
        CloseHandle(hs[0]); CloseHandle(hs[1]); CloseHandle(done);
    }
    FreeLibraryAndExitThread(self, 0);
}

// Deletes the oldest dumps so that at most `keep` remain. Host thread.
// Retention policy: keeps the `keep` newest crash-*.dmp files by last-write time. Uses the heap (std::vector,
// std::wstring), which is fine because it runs at install time, never in the crash path. Delete failures (file
// open in a viewer) are ignored; the next install tries again.
void PruneDumps(size_t keep) {
    struct D { FILETIME t; std::wstring name; };
    std::vector<D> v;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(g_dir) + L"\\crash-*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back({fd.ftLastWriteTime, fd.cFileName}); } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (v.size() <= keep) return;
    std::sort(v.begin(), v.end(), [](const D& a, const D& b) { return CompareFileTime(&a.t, &b.t) < 0; });
    for (size_t i = 0; i + keep < v.size(); ++i) DeleteFileW((std::wstring(g_dir) + L"\\" + v[i].name).c_str());
}

}  // namespace

// Per-user location (LOCALAPPDATA, not roaming: dumps are machine specific and can be large). Returns an empty
// string if the shell folder cannot be resolved (e.g. service account without profile); callers treat that as
// "crash protection unavailable". SHGetFolderPathW (legacy CSIDL API) fills a caller buffer, so no CoTaskMemFree
// is needed. The folder itself is not created here.
std::wstring CrashDir() {
    wchar_t p[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p)) || !p[0]) return L"";
    return std::wstring(p) + L"\\" WB_NAME_W L"\\crash";
}

// Reference counted per process: every driver object calls CrashInstall/CrashUninstall in pairs, and only the
// first install does the real work. Pitfalls:
//   - g_installRefs is incremented before any check, so an early return (no LOCALAPPDATA, path too long, failed
//     GetModuleHandleExW/CreateEventW/CreateThread) still counts as one install; CrashUninstall then just
//     decrements and finds nothing (or only partially created objects) to release.
//   - If the events were created but the module reference or the thread could not be obtained, the events are
//     closed again and the globals nulled (0.9.6, D2: before, they leaked; CloseEvents below). No handler is
//     registered in that case, so nothing can wait on them.
//   - The vectored handler is registered last, so it can never observe a half-initialized state.
// The Unlock guard releases the SRW lock on every return path.
// 0.9.6 (D2): closes the events of a failed install (any of them may be null) and clears the globals. Only used
// before the dump thread exists; once it runs, the thread owns the events and closes them after g_quit.
static void CloseEvents() {
    HANDLE* ev[3] = {&g_req, &g_done, &g_quit};
    for (HANDLE* e : ev) if (*e) { CloseHandle(*e); *e = nullptr; }
}

// 0.9.8 (exception barrier): CrashInstall and CrashUninstall never throw. They run in the driver's constructor and
// destructor; an exception from the constructor would leave g_installRefs counted without the matching uninstall,
// and one from the destructor would terminate the host. The work is in the *Impl functions; the SRW lock is released
// by the Unlock guard on every path, including an exception.
static void CrashInstallImpl();
static void CrashUninstallImpl();
void CrashInstall() noexcept {
    try { CrashInstallImpl(); } catch (...) { /* crash protection unavailable for this object; the driver works */ }
}
void CrashUninstall() noexcept {
    try { CrashUninstallImpl(); } catch (...) { /* nothing left to do safely */ }
}
static void CrashInstallImpl() {
    AcquireSRWLockExclusive(&g_installLock);
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_installLock); } } unlock;
    if (g_installRefs++ > 0) return;
    const std::wstring dir = CrashDir();
    if (dir.empty() || dir.size() >= MAX_PATH) return;
    wcscpy(g_dir, dir.c_str());
    CreateDirectoryW(dir.substr(0, dir.rfind(L'\\')).c_str(), nullptr);
    CreateDirectoryW(g_dir, nullptr);
    PruneDumps(5);
    HMODULE self = nullptr;
    // First lookup (UNCHANGED_REFCOUNT): only to find our own base address from the address of a function in it;
    // it does not pin the DLL. The PE headers then give SizeOfImage, i.e. the [base, end) range used by the handler.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void*)&CrashInstall, &self) || !self) return;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)self;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)self + dos->e_lfanew);
    g_modBase = (uintptr_t)self;
    g_modEnd = g_modBase + nt->OptionalHeader.SizeOfImage;
    // dbghelp.dll from System32 only (never from the host's folder or the current directory), at runtime.
    if (HMODULE dh = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        g_writeDump = (PFN_MiniDumpWriteDump)(void*)GetProcAddress(dh, "MiniDumpWriteDump");
    g_req = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // Second lookup without UNCHANGED_REFCOUNT: increments the DLL load count. That reference belongs to the dump
    // thread and is released by FreeLibraryAndExitThread (or by FreeLibrary below if the thread cannot start).
    HMODULE ref = nullptr;            // reference held by the dump thread, released by FreeLibraryAndExitThread
    if (!g_req || !g_done || !g_quit ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void*)&CrashInstall, &ref)) {
        CloseEvents();                // 0.9.6 (D2): no dump thread will ever close them
        return;
    }
    // 0.9.6 (R3): the thread gets its own copies of the handles and the reference (see DumpThread).
    DumpArgs* args = new (std::nothrow) DumpArgs{g_req, g_done, g_quit, ref};
    g_thread = args ? CreateThread(nullptr, 64 * 1024, DumpThread, args, 0, nullptr) : nullptr;
    if (!g_thread) { delete args; FreeLibrary(ref); CloseEvents(); return; }   // 0.9.6 (D2): events closed too
    // Reset for a fresh install cycle; after a crash that the host survived, a later install may dump again.
    InterlockedExchange(&g_dumping, 0);
    g_veh = AddVectoredExceptionHandler(0, Handler);   // 0 = called after handlers registered first
    WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwCrash, "CrashProtectionInstalled", wbtrace::Bool("Handler", g_veh != nullptr),
             wbtrace::Bool("MiniDump", g_writeDump != nullptr));
}

static void CrashUninstallImpl() {
    // Inverse of CrashInstall. Order matters: the handler is removed first so no new crash can start waiting on the
    // events, then the dump thread is told to quit. The thread handle is only closed here, after the handler has been
    // removed, because the handler waits on it (fix 5). The globals are cleared without closing the events; the dump
    // thread closes them once it observes g_quit. Also ends the crash-session tracking for this process (the last
    // driver object is going away cleanly).
    AcquireSRWLockExclusive(&g_installLock);
    struct Unlock { ~Unlock() { ReleaseSRWLockExclusive(&g_installLock); } } unlock;
    if (g_installRefs <= 0 || --g_installRefs > 0) return;
    CrashSessionEnd();                                  // fix 4: the last driver object of this process exits cleanly
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    if (g_quit) SetEvent(g_quit);                       // the dump thread exits and releases its DLL reference
    // Fix 5: the thread handle stays open until here (the handler waits on it); closed only after the handler
    // has been removed.
    if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
    // The dump thread closes the three events itself once it has seen g_quit.
    g_req = g_done = g_quit = nullptr;
}

// Read-only check used by driver init to decide on safe mode. Reads at most 511 bytes; only the first line is
// returned (narrowed to wide by byte copy, which is correct because the marker is plain ASCII). FILE_SHARE_READ
// lets several hosts check at the same time. A marker of any age counts until CrashClearMarker removes it.
bool CrashMarkerPresent(std::wstring* info) {
    const std::wstring dir = CrashDir();
    if (dir.empty()) return false;
    HANDLE f = CreateFileW((dir + L"\\crash-marker.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char b[512] = {}; DWORD n = 0;
    ReadFile(f, b, sizeof b - 1, &n, nullptr);
    CloseHandle(f);
    if (info) {
        std::string s(b, n);
        size_t e = s.find_first_of("\r\n"); if (e != std::string::npos) s.resize(e);
        info->assign(s.begin(), s.end());
    }
    return true;
}

// Called once the safe-mode decision has been made / acknowledged; the .dmp files are kept for the user.
void CrashClearMarker() {
    const std::wstring dir = CrashDir();
    if (!dir.empty()) DeleteFileW((dir + L"\\crash-marker.txt").c_str());
}

// ---------------------------------------------------------------- session files (fix 4)
// session-<pid>.txt exists while a driver object of that process is alive. A crash leaves it behind; a clean exit
// deletes it. Safe mode needs both a crash marker and a stale session file (its process is no longer running), so
// a crash that the host survived and then exited cleanly does not force safe mode.
// Path of the session file for `pid`, or empty if CrashDir() is unavailable.
static std::wstring SessionPath(DWORD pid) {
    const std::wstring dir = CrashDir();
    if (dir.empty()) return L"";
    wchar_t n[48]; swprintf(n, 48, L"\\session-%lu.txt", (unsigned long)pid);
    return dir + n;
}

// 0.9.6 (D3): creation time of process `proc` as a 64-bit FILETIME value, or 0 if it cannot be read.
static unsigned long long ProcessCreationTime(HANDLE proc) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(proc, &c, &e, &k, &u)) return 0;
    return ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
}

// Called at driver init (host thread). Creates the parent and crash folders if needed (CreateDirectoryW does not
// create intermediate folders), then writes a tiny file whose existence is the signal.
// 0.9.6 (D3): the second line stores this process's creation time ("created=<FILETIME as decimal>"), so
// CrashUncleanExitFound can tell a reused pid (an unrelated process that got the same number) from the original
// process. Files written by 0.9.5 and older have only the first line; they are checked by pid alone.
// Several driver objects in the same process share one file (same pid); it is removed by CrashSessionEnd, which
// CrashUninstall calls only when the last object is released.
void CrashSessionBegin() {
    const std::wstring p = SessionPath(GetCurrentProcessId());
    if (p.empty()) return;
    const std::wstring dir = CrashDir();
    CreateDirectoryW(dir.substr(0, dir.rfind(L'\\')).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char txt[80];
    const int n = snprintf(txt, sizeof txt, "session-open\r\ncreated=%llu\r\n", ProcessCreationTime(GetCurrentProcess()));
    DWORD w = 0; if (n > 0) WriteFile(f, txt, (DWORD)n, &w, nullptr);
    CloseHandle(f);
}

// Clean-exit path: the file of this process disappears. Missing file is not an error.
void CrashSessionEnd() {
    const std::wstring p = SessionPath(GetCurrentProcessId());
    if (!p.empty()) DeleteFileW(p.c_str());
}

// Scans session-*.txt for files left by other processes. `fd.cFileName + 8` skips the "session-" prefix and
// wcstoul stops at ".txt". A file is stale if its process cannot be opened or has already exited
// (WaitForSingleObject with timeout 0 returns WAIT_TIMEOUT only for a running process). Stale files are deleted,
// so each unclean exit is reported once. The current process's own file is skipped.
// 0.9.6 (D3): two former pitfalls are handled now:
//   - OpenProcess denied (live process of another user / higher integrity level): the process exists. Review R4:
//     PROCESS_QUERY_LIMITED_INFORMATION alone is tried next (exit code + creation time); only if that is refused
//     too is it treated as ALIVE (before 0.9.6: dead, a false positive). ERROR_INVALID_PARAMETER ("no such
//     process") or a process that has exited counts as dead.
//   - Reused pid: when the file stores a creation time ("created=" line, see CrashSessionBegin) and the running
//     process with that pid has a different creation time, it is another process: the file is stale. If the time
//     cannot be read (old file, or GetProcessTimes fails), the pid check alone decides, as before.
// Safe mode additionally requires a crash marker, so any remaining error only affects that rare combination.
// Reads the "created=" value of a session file; 0 if absent (file of 0.9.5 or older) or unreadable.
static unsigned long long SessionCreationTime(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char b[128] = {}; DWORD n = 0;
    ReadFile(f, b, sizeof b - 1, &n, nullptr);
    CloseHandle(f);
    const char* c = strstr(b, "created=");
    return c ? strtoull(c + 8, nullptr, 10) : 0;
}

bool CrashUncleanExitFound() {
    const std::wstring dir = CrashDir();
    if (dir.empty()) return false;
    bool found = false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\session-*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const DWORD pid = (DWORD)wcstoul(fd.cFileName + 8, nullptr, 10);
        if (pid == GetCurrentProcessId()) continue;
        const std::wstring path = dir + L"\\" + fd.cFileName;
        bool alive = false;
        if (pid) {
            HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!proc) proc = OpenProcess(SYNCHRONIZE, FALSE, pid);    // some processes allow only one of the two
            if (proc) {
                alive = WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
                const unsigned long long want = SessionCreationTime(path);
                const unsigned long long have = alive && want ? ProcessCreationTime(proc) : 0;
                if (have && have != want) alive = false;               // same pid, other process: stale file
                CloseHandle(proc);
            } else if (GetLastError() == ERROR_ACCESS_DENIED) {
                // 0.9.6 (review R4): the process exists but refuses SYNCHRONIZE. Try query access alone, which is
                // granted more often (e.g. for elevated processes): GetExitCodeProcess tells whether it still runs
                // and the creation time detects a reused pid. Only if that is refused too is it treated as alive.
                if (HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                    DWORD code = 0;
                    alive = GetExitCodeProcess(q, &code) && code == STILL_ACTIVE;
                    const unsigned long long want = SessionCreationTime(path);
                    const unsigned long long have = alive && want ? ProcessCreationTime(q) : 0;
                    if (have && have != want) alive = false;           // same pid, other process: stale file
                    CloseHandle(q);
                } else {
                    alive = true;                                      // exists, nothing readable: assume alive
                }
            }                                                          // other errors: no such process (dead)
        }
        if (!alive) { found = true; DeleteFileW(path.c_str()); }   // stale: unclean exit
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

