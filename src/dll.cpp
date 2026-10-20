// WASAPI Driver for ASIO - DLL entry point, COM class factory and self-registration
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
//   An ASIO driver is an in-process COM server: the host (DAW) reads HKLM\SOFTWARE\ASIO\<name>\CLSID and calls
//   CoCreateInstance with that CLSID as both class id and interface id. This file provides DllMain,
//   DllGetClassObject (a static class factory), DllCanUnloadNow and DllRegisterServer/DllUnregisterServer.
//
// Threading
//   All functions are called by COM/the loader on the host's threads; none touch audio state. DllCanUnloadNow is
//   based on g_objects (live driver objects, hidden windows, helper objects) and g_locks (IClassFactory::LockServer).
//
// 0.9.8 (hardening): every exported function and every factory method is an exception barrier (nothing may cross
// the COM / loader boundary as a C++ exception), validates its out pointers, and unbalanced LockServer(FALSE)
// calls can no longer drive the lock count below zero (which would have kept DllCanUnloadNow at S_FALSE forever,
// or let the DLL unload under a later LockServer(TRUE) pair).
//
// Registry layout written by DllRegisterServer (all under HKLM, in the registry view of the calling process: a
// 64-bit regsvr32 writes the 64-bit view, a 32-bit regsvr32 is redirected to WOW6432Node automatically):
//   SOFTWARE\Classes\CLSID\{A631...}                    (default) = driver name
//   SOFTWARE\Classes\CLSID\{A631...}\InprocServer32     (default) = full path of this DLL, ThreadingModel=Apartment
//   SOFTWARE\ASIO\<WB_NAME_W>                           CLSID = {A631...}, Description = driver name
// The host enumerates SOFTWARE\ASIO, shows Description and uses CLSID; COM then finds the DLL via InprocServer32.
#include "driver.h"
#include "guard.h"      // 0.9.8: exception barrier
#include "trace.h"      // 0.9.8: TraceLogging provider (registered lazily, unregistered on unload)
#include <cwchar>
#include <new>        // std::bad_alloc, std::nothrow (0.9.8)
#include <olectl.h>

// Module handle of this DLL, set in DllMain. Used for resources (icons, dialogs, strings) and GetModuleFileNameW.
HINSTANCE g_hInst = nullptr;
// Outstanding IClassFactory::LockServer(TRUE) calls. Atomic because COM may call LockServer from any thread.
static std::atomic<long> g_locks{0};
// Must match CLSID_WasapiBridge (defined elsewhere as a binary GUID); this is only its string form for the registry.
static const wchar_t* kClsidStr = L"{A63175BD-B872-4FA5-9595-464F0236E1D0}";

// Class factory with static storage duration: reference counting is a no-op.
// COM requires DllGetClassObject to return an IClassFactory. One global instance serves every request; since it
// is never deleted, AddRef/Release return constant non-zero values (callers must not interpret them anyway).
// Neither this factory nor its CreateInstance is counted in g_objects: the DLL lifetime is governed by the driver
// objects themselves (the WasapiBridge constructor increments g_objects) and by LockServer.
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;                                       // 0.9.8
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }    // static object
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    // Creates a new driver instance. The ASIO convention passes the CLSID as riid (see WasapiBridge::QueryInterface).
    // Aggregation is not supported (no ASIO host uses it). new(std::nothrow) avoids an exception crossing the COM
    // boundary. The object starts with one reference; QueryInterface adds one for the caller and the Release drops
    // the creation reference, so on failure (unknown riid) the object is destroyed and *ppv is null.
    // 0.9.8: new(std::nothrow) only covers the allocation; the constructor (crash protection setup) can still throw,
    // so the whole body is an exception barrier. *ppv is cleared first, as COM requires on failure.
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try {
            wbtrace::Register();
            WasapiBridge* d = new (std::nothrow) WasapiBridge();
            if (!d) return E_OUTOFMEMORY;
            HRESULT hr = d->QueryInterface(riid, ppv);
            d->Release();
            return hr;
        } catch (const std::bad_alloc&) {
            NoteException("IClassFactory::CreateInstance");
            return E_OUTOFMEMORY;
        } catch (...) {
            NoteException("IClassFactory::CreateInstance");
            return E_FAIL;
        }
    }
    // Only the counter changes; DllCanUnloadNow reads it. 0.9.8: an unbalanced unlock never takes it below zero.
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) { ++g_locks; return S_OK; }
        long cur = g_locks.load();
        while (cur > 0 && !g_locks.compare_exchange_weak(cur, cur - 1)) {}
        return S_OK;
    }
};
static Factory g_factory;

// Loader callback; runs under the loader lock, so it does only trivial work:
//   - PROCESS_ATTACH: remember the module handle. DisableThreadLibraryCalls suppresses THREAD_ATTACH/DETACH
//     notifications, which this DLL does not need (avoids a loader callback per host thread creation).
//   - PROCESS_DETACH: `reserved` is null for a dynamic unload (FreeLibrary / CoFreeUnusedLibraries) and non-null
//     at process termination. Icons are destroyed only on a dynamic unload: at process exit other threads have
//     already been terminated and the OS reclaims GDI/USER objects anyway, so touching them there is pointless
//     and potentially unsafe.
extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); }
    // 0.9.3: FreeLibrary (not process exit): no window uses the icons any more (g_objects was 0)
    // 0.9.8: the ETW provider is unregistered too, so ETW never calls the enable callback in unmapped code. Writing
    // the last event and EventUnregister are allowed under the loader lock (no DLL is loaded by them).
    if (reason == DLL_PROCESS_DETACH && !reserved) {
        WB_TRACE(wbtrace::kLvlInfo, wbtrace::kKwLifecycle, "DriverUnloaded", wbtrace::Str("Version", WB_VERSION_STR));
        wbtrace::Unregister();
        FreeAppIcons();
    }
    return TRUE;
}

// Called by CoCreateInstance/CoGetClassObject (host thread, via ole32) after it found this DLL through
// InprocServer32. Only CLSID_WasapiBridge is served; *ppv is always written, as COM requires.
// 0.9.8: also the first point where the TraceLogging provider can be registered (outside DllMain).
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(clsid, CLSID_WasapiBridge)) return CLASS_E_CLASSNOTAVAILABLE;
    wbtrace::Register();
    WB_TRACE(wbtrace::kLvlVerbose, wbtrace::kKwLifecycle, "DriverLoaded", wbtrace::Str("Version", WB_VERSION_STR),
#if defined(__aarch64__) || defined(_M_ARM64)
             wbtrace::Str("Architecture", "ARM64")
#elif defined(_WIN64)
             wbtrace::Str("Architecture", "x64")
#else
             wbtrace::Str("Architecture", "x86")
#endif
             );
    return g_factory.QueryInterface(riid, ppv);
}

// The DLL may be unloaded only when no object, hidden window or helper thread context is alive.
// Polled by CoFreeUnusedLibraries (host's choice of thread). Returning S_OK allows ole32 to FreeLibrary the DLL,
// so every object whose code may still run later (driver objects, hidden message windows, accessibility
// wrappers, see a11y.h) must hold a g_objects count.
STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

// Creates key `sub` under `root` and sets string value `name` (nullptr = default value).
// KEY_WRITE only; the key is created if missing and opened otherwise. The string length passed to
// RegSetValueExW includes the terminating null, as REG_SZ requires. Returns the Win32 error code (not an HRESULT).
static LONG SetStr(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY k; LONG r = RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

// Deletes only HKLM\SOFTWARE\ASIO\WasapiBridge (the pre-0.6.0 name) in both the 64-bit and the 32-bit registry view.
// Cleans up registrations of the pre-0.6.0 name, which would otherwise appear as a second driver in hosts.
// The explicit KEY_WOW64_64KEY / KEY_WOW64_32KEY flags open both views regardless of the bitness of the calling
// process. Errors are ignored (usually: key not present). Requires write access to HKLM (elevated process).
static void DeleteOldAsioKey() {
    const REGSAM views[2] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    for (REGSAM v : views) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WRITE | DELETE | v, &k) == ERROR_SUCCESS) {
            RegDeleteTreeW(k, WB_OLDNAME_W);
            RegCloseKey(k);
        }
    }
}

// regsvr32 entry point: registers this DLL (its own path) as the COM server and the ASIO driver entry.
// Run elevated (regsvr32 from the installer): writing HKLM fails with ERROR_ACCESS_DENIED otherwise, which is
// reported as SELFREG_E_CLASS. The path written to InprocServer32 is whatever path this DLL was loaded from, so
// the DLL must be registered from its final install location. ThreadingModel=Apartment: ASIO drivers are created and called on the host's thread; COM
// then never marshals calls to a different apartment for STA callers. Partial writes are not rolled back.
// 0.9.8: exception barrier (std::wstring) and a path that does not fit MAX_PATH is refused instead of being
// registered truncated (a truncated InprocServer32 path would make every host fail to load the driver).
static HRESULT RegisterImpl();
STDAPI DllRegisterServer() {
    try { return RegisterImpl(); }
    catch (...) { NoteException("DllRegisterServer"); return SELFREG_E_CLASS; }
}
static HRESULT RegisterImpl() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_hInst, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return SELFREG_E_CLASS;
    std::wstring clsKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr;
    std::wstring asioKey = L"SOFTWARE\\ASIO\\" WB_NAME_W;
    DeleteOldAsioKey();                    // 0.6.0: key of the previous name (same CLSID), both views
    if (SetStr(HKEY_LOCAL_MACHINE, clsKey, nullptr, WB_NAME_W) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (SetStr(HKEY_LOCAL_MACHINE, clsKey + L"\\InprocServer32", nullptr, path) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (SetStr(HKEY_LOCAL_MACHINE, clsKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (SetStr(HKEY_LOCAL_MACHINE, asioKey, L"CLSID", kClsidStr) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    if (SetStr(HKEY_LOCAL_MACHINE, asioKey, L"Description", WB_NAME_W) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    return S_OK;
}

// regsvr32 /u entry point: removes the COM registration, the ASIO entry and the old-name ASIO entry.
// Best effort: return values of RegDeleteTreeW are ignored and S_OK is always returned, so uninstall never fails
// because a key is already missing. Like registration, it acts on the view of the calling process (plus both
// views for the old-name key).
STDAPI DllUnregisterServer() {
    try {                                  // 0.9.8: exception barrier
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr).c_str());
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\" WB_NAME_W);
        DeleteOldAsioKey();
    } catch (...) {
        NoteException("DllUnregisterServer");
    }
    return S_OK;
}
