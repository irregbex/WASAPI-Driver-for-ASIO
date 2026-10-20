// WASAPI Driver for ASIO - accessibility helpers for the owner-drawn controls (0.9.1, B1)
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
//   The panel draws its toggles (owner-drawn BUTTON) and its sliders (own window class) itself, so the system
//   would describe a toggle as a push button without state and a slider as an anonymous pane. Native check boxes
//   were considered: their look can only be customized with NM_CUSTOMDRAW, which needs comctl32 version 6, and a
//   driver DLL cannot rely on the host process having it (the panel must look the same in every host). Instead:
//     - WM_GETOBJECT(OBJID_CLIENT) of a toggle or slider returns a small IAccessible that wraps the system's
//       standard object (CreateStdAccessibleObject) and corrects only what is wrong: role check box with the
//       checked state and "Check"/"Uncheck" as default action (toggles); role slider with the current value as
//       text (sliders). Name, location, focus, keyboard shortcut and everything else come from the standard
//       object (the name is the window text). Screen readers that use UI Automation see these windows through
//       the MSAA proxy, because the window answers WM_GETOBJECT itself: ControlType CheckBox with the Toggle
//       state, ControlType Slider with a value.
//     - AccName gives a native control without a usable label (an edit after a toggle, a read-only status edit)
//       an accessible name through Direct Annotation (IAccPropServices::SetHwndPropStr, PROPID_ACC_NAME);
//       AccClear removes it before the window is destroyed. 0.9.2: AccDescription sets the help tip as the
//       accessible description the same way (PROPID_ACC_DESCRIPTION); the wrapper of toggles and sliders asks
//       the parent for it (kMsgDescription).
//     - AccNotify* raise the WinEvents a screen reader listens to (state or value changed).
//   oleacc.dll and oleaut32.dll are loaded from System32 at run time (no new import of the driver DLL; nothing is
//   loaded until a screen reader or another accessibility client asks).
//
// Threading and lifetime
//   Host thread (the windows' thread): WM_GETOBJECT, AccName, AccClear, AccNotify*. The wrapper object may be
//   called by COM on another thread when the client lives in another apartment; it then only reads window
//   properties (GetWindowLongPtr, GetWindowText) and sends one message to the slider's parent. Each live wrapper
//   counts in g_objects, so DllCanUnloadNow keeps the DLL loaded while a client still holds one. Never used by
//   the audio thread.
//
// Contract with panel.cpp
//   - A toggle keeps its on/off state in GWLP_USERDATA (0/1); a slider keeps its value 0..100 there. The wrapper
//     reads those directly, so they must stay the single source of truth for the drawn state.
//   - The control's window procedure forwards WM_GETOBJECT to OnGetObject and calls NotifyState / NotifyValue
//     after every change (also changes made by code, not only by the user).
//   - Parents answer kMsgSliderText / kMsgDescription (return 1 when they filled the buffer).
//
// Pitfalls
//   - Load() caches its result in function statics without a lock: call it first from the host thread (the
//     first WM_GETOBJECT arrives there), so a COM call on another thread only reads the filled struct.
//   - The GUIDs are spelled out locally so the DLL does not need oleacc.lib / uuid.lib imports for them.
//   - The wrapper holds an HWND, not a reference to the window: if a client keeps the object after the window is
//     destroyed, GetWindowLongPtr / SendMessage fail harmlessly (0 / no reply) and the standard object reports
//     the window as gone.
#pragma once
#include <windows.h>
#include <oleacc.h>
#include <atomic>
#include <cwchar>

extern std::atomic<long> g_objects;

namespace a11y {

// Message sent to a slider's parent to get its value as text: wParam = slider HWND, lParam = wchar_t[64].
// Return 1 if the buffer was filled; otherwise the value 0..100 is used.
enum { kMsgSliderText = WM_APP + 70 };
// 0.9.2: message sent to a toggle's or slider's parent to get its accessible description (the help tip):
// wParam = control HWND, lParam = wchar_t[kDescLen]. Return 1 if the buffer was filled.
enum { kMsgDescription = WM_APP + 71, kDescLen = 512 };
// Which correction the wrapper applies (passed by the window procedure to OnGetObject).
enum Kind { kToggle = 1, kSlider = 2 };

// Interface / class / property GUIDs (values from oleacc.h / the MSAA documentation).
static const IID kIID_IAccessible = {0x618736e0, 0x3c3d, 0x11cf, {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const CLSID kCLSID_AccPropServices = {0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
static const IID kIID_IAccPropServices = {0x6e26e776, 0x04f0, 0x495d, {0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69}};
static const GUID kPROPID_ACC_NAME = {0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};
static const GUID kPROPID_ACC_DESCRIPTION = {0x4d48dfe4, 0xbd3f, 0x491f, {0xa6, 0x48, 0x49, 0x2d, 0x6f, 0x20, 0xc5, 0x88}};

// Run-time imports: CreateStdAccessibleObject and LresultFromObject (oleacc.dll), SysAllocString (oleaut32.dll).
// ok is true only when all three were found; otherwise WM_GETOBJECT falls back to the system's default answer.
struct Api {
    HRESULT (WINAPI* CreateStd)(HWND, LONG, REFIID, void**) = nullptr;
    LRESULT (WINAPI* FromObject)(REFIID, WPARAM, LPUNKNOWN) = nullptr;
    BSTR (WINAPI* AllocString)(const OLECHAR*) = nullptr;
    bool ok = false;
};
inline const Api& Load() {
    static Api a;
    static bool tried = false;
    if (!tried) {
        tried = true;                                   // host thread; the modules stay loaded (system DLLs)
        HMODULE acc = LoadLibraryExW(L"oleacc.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE aut = LoadLibraryExW(L"oleaut32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (acc && aut) {
            a.CreateStd = (decltype(a.CreateStd))(void*)GetProcAddress(acc, "CreateStdAccessibleObject");
            a.FromObject = (decltype(a.FromObject))(void*)GetProcAddress(acc, "LresultFromObject");
            a.AllocString = (decltype(a.AllocString))(void*)GetProcAddress(aut, "SysAllocString");
            a.ok = a.CreateStd && a.FromObject && a.AllocString;
        }
    }
    return a;
}

// Delegating IAccessible: every method forwards to the standard object (base_) except, for CHILDID_SELF only,
// get_accRole, get_accState (checked flag; pressed/default removed), get_accValue (sliders),
// get_accDefaultAction and accDoDefaultAction (toggles) and get_accDescription (help tip, falls back to base_).
// Reference counting is atomic because COM may call from a client's thread. Constructed with refs_ = 1.
class Wrap final : public IAccessible {
public:
    Wrap(IAccessible* base, HWND h, int kind) : base_(base), h_(h), kind_(kind) { ++g_objects; }
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch) || IsEqualIID(riid, kIID_IAccessible)) {
            *ppv = static_cast<IAccessible*>(this); AddRef(); return S_OK;
        }
        if (IsEqualIID(riid, IID_IOleWindow)) return base_->QueryInterface(riid, ppv);   // WindowFromAccessibleObject
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs_; }
    STDMETHODIMP_(ULONG) Release() override { const long r = --refs_; if (!r) delete this; return (ULONG)r; }
    // IDispatch: the standard object's
    STDMETHODIMP GetTypeInfoCount(UINT* n) override { return base_->GetTypeInfoCount(n); }
    STDMETHODIMP GetTypeInfo(UINT i, LCID l, ITypeInfo** t) override { return base_->GetTypeInfo(i, l, t); }
    STDMETHODIMP GetIDsOfNames(REFIID r, LPOLESTR* n, UINT c, LCID l, DISPID* d) override { return base_->GetIDsOfNames(r, n, c, l, d); }
    STDMETHODIMP Invoke(DISPID d, REFIID r, LCID l, WORD f, DISPPARAMS* p, VARIANT* v, EXCEPINFO* e, UINT* a) override { return base_->Invoke(d, r, l, f, p, v, e, a); }
    // IAccessible: corrected for the window itself, the standard answer for everything else
    STDMETHODIMP get_accParent(IDispatch** p) override { return base_->get_accParent(p); }
    STDMETHODIMP get_accChildCount(long* n) override { return base_->get_accChildCount(n); }
    STDMETHODIMP get_accChild(VARIANT c, IDispatch** d) override { return base_->get_accChild(c, d); }
    STDMETHODIMP get_accName(VARIANT c, BSTR* s) override { return base_->get_accName(c, s); }
    STDMETHODIMP get_accValue(VARIANT c, BSTR* s) override {
        if (!Self(c) || kind_ != kSlider) return base_->get_accValue(c, s);
        if (!s) return E_POINTER;
        wchar_t b[64] = L"";
        if (SendMessageW(GetParent(h_), kMsgSliderText, (WPARAM)h_, (LPARAM)b) != 1)
            swprintf(b, 64, L"%d", (int)GetWindowLongPtrW(h_, GWLP_USERDATA));
        *s = Load().AllocString(b);
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP get_accDescription(VARIANT c, BSTR* s) override {   // 0.9.2: the help tip
        if (!Self(c)) return base_->get_accDescription(c, s);
        if (!s) return E_POINTER;
        wchar_t b[kDescLen] = L"";
        if (SendMessageW(GetParent(h_), kMsgDescription, (WPARAM)h_, (LPARAM)b) != 1 || !b[0]) return base_->get_accDescription(c, s);
        *s = Load().AllocString(b);
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP get_accRole(VARIANT c, VARIANT* r) override {
        if (!Self(c)) return base_->get_accRole(c, r);
        if (!r) return E_POINTER;
        VariantInitLocal(r); r->vt = VT_I4; r->lVal = kind_ == kToggle ? ROLE_SYSTEM_CHECKBUTTON : ROLE_SYSTEM_SLIDER;
        return S_OK;
    }
    STDMETHODIMP get_accState(VARIANT c, VARIANT* r) override {
        const HRESULT hr = base_->get_accState(c, r);
        if (Self(c) && SUCCEEDED(hr) && r && r->vt == VT_I4) {
            r->lVal &= ~(STATE_SYSTEM_PRESSED | STATE_SYSTEM_DEFAULT);
            if (kind_ == kToggle && GetWindowLongPtrW(h_, GWLP_USERDATA) != 0) r->lVal |= STATE_SYSTEM_CHECKED;
        }
        return hr;
    }
    STDMETHODIMP get_accHelp(VARIANT c, BSTR* s) override { return base_->get_accHelp(c, s); }
    STDMETHODIMP get_accHelpTopic(BSTR* f, VARIANT c, long* t) override { return base_->get_accHelpTopic(f, c, t); }
    STDMETHODIMP get_accKeyboardShortcut(VARIANT c, BSTR* s) override { return base_->get_accKeyboardShortcut(c, s); }
    STDMETHODIMP get_accFocus(VARIANT* r) override { return base_->get_accFocus(r); }
    STDMETHODIMP get_accSelection(VARIANT* r) override { return base_->get_accSelection(r); }
    STDMETHODIMP get_accDefaultAction(VARIANT c, BSTR* s) override {
        if (!Self(c) || kind_ != kToggle) return base_->get_accDefaultAction(c, s);
        if (!s) return E_POINTER;
        *s = Load().AllocString(GetWindowLongPtrW(h_, GWLP_USERDATA) ? L"Uncheck" : L"Check");
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP accSelect(long f, VARIANT c) override { return base_->accSelect(f, c); }
    STDMETHODIMP accLocation(long* x, long* y, long* w, long* h, VARIANT c) override { return base_->accLocation(x, y, w, h, c); }
    STDMETHODIMP accNavigate(long d, VARIANT s, VARIANT* e) override { return base_->accNavigate(d, s, e); }
    STDMETHODIMP accHitTest(long x, long y, VARIANT* r) override { return base_->accHitTest(x, y, r); }
    STDMETHODIMP accDoDefaultAction(VARIANT c) override {
        if (!Self(c) || kind_ != kToggle) return base_->accDoDefaultAction(c);
        if (!IsWindowEnabled(h_)) return E_FAIL;
        PostMessageW(h_, BM_CLICK, 0, 0);               // the same path as a click: BN_CLICKED to the parent
        return S_OK;
    }
    STDMETHODIMP put_accName(VARIANT c, BSTR s) override { return base_->put_accName(c, s); }
    STDMETHODIMP put_accValue(VARIANT c, BSTR s) override { return base_->put_accValue(c, s); }

private:
    ~Wrap() { base_->Release(); --g_objects; }
    static bool Self(const VARIANT& c) { return c.vt == VT_I4 && c.lVal == CHILDID_SELF; }
    static void VariantInitLocal(VARIANT* v) { memset(v, 0, sizeof *v); }
    std::atomic<long> refs_{1};
    IAccessible* base_;
    HWND h_;
    int kind_;
};

// WM_GETOBJECT of a toggle or slider window. Returns true and sets `out` when it answered.
// Only OBJID_CLIENT is handled (other ids, e.g. UIA's UiaRootObjectId, fall through to DefWindowProc).
// A new wrapper is created per request; LresultFromObject marshals it and keeps the reference the client gets.
inline bool OnGetObject(HWND h, WPARAM wp, LPARAM lp, int kind, LRESULT& out) {
    if ((LONG)lp != OBJID_CLIENT) return false;
    const Api& a = Load();
    if (!a.ok) return false;
    IAccessible* base = nullptr;
    if (FAILED(a.CreateStd(h, OBJID_CLIENT, kIID_IAccessible, (void**)&base)) || !base) return false;
    Wrap* w = new Wrap(base, h, kind);                 // takes the reference on `base`
    out = a.FromObject(kIID_IAccessible, wp, static_cast<IAccessible*>(w));
    w->Release();                                       // LresultFromObject holds its own reference
    return true;
}

// Direct Annotation of the accessible name of a native control (COM must be initialized on this thread).
inline void AccName(HWND h, const wchar_t* name) {
    if (!h || !name) return;
    IAccPropServices* s = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, kIID_IAccPropServices, (void**)&s)) && s) {
        s->SetHwndPropStr(h, OBJID_CLIENT, CHILDID_SELF, kPROPID_ACC_NAME, name);
        s->Release();
    }
}
// 0.9.2: the help tip as the accessible description (Narrator reads it after the name and the state).
inline void AccDescription(HWND h, const wchar_t* text) {
    if (!h || !text) return;
    IAccPropServices* s = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, kIID_IAccPropServices, (void**)&s)) && s) {
        s->SetHwndPropStr(h, OBJID_CLIENT, CHILDID_SELF, kPROPID_ACC_DESCRIPTION, text);
        s->Release();
    }
}
// Removes both annotations (name and description); call before the window is destroyed, since Direct Annotation
// is keyed by HWND and a reused handle could otherwise inherit stale text.
inline void AccClear(HWND h) {
    if (!h) return;
    IAccPropServices* s = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, kIID_IAccPropServices, (void**)&s)) && s) {
        MSAAPROPID ids[2] = {kPROPID_ACC_NAME, kPROPID_ACC_DESCRIPTION};
        s->ClearHwndProps(h, OBJID_CLIENT, CHILDID_SELF, ids, 2);
        s->Release();
    }
}
// WinEvents for screen readers: a toggle's checked state changed / a slider's value changed.
inline void NotifyState(HWND h) { if (h) NotifyWinEvent(EVENT_OBJECT_STATECHANGE, h, OBJID_CLIENT, CHILDID_SELF); }
inline void NotifyValue(HWND h) { if (h) NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, h, OBJID_CLIENT, CHILDID_SELF); }

}  // namespace a11y
