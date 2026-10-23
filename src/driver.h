// WASAPI Driver for ASIO - IASIO implementation over WASAPI: types and the driver class
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
//   Declares the driver object (class WasapiBridge, the COM object the ASIO host talks to) and the helper types it
//   uses: device formats, the single-threaded FIFO, capture sources with drift correction, additional output sinks,
//   COM callback objects for Windows notifications, and the snapshots handed to the control panel.
//
// Threading model (see ARCHITECTURE.txt section 3; the rules below are invariants)
//   H = host thread: the thread that called IASIO::init() (the DAW's UI thread). All IASIO methods, the control
//       panel and the hidden message window run here. H is the ONLY thread that calls the host's asioMessage()
//       callback and the only one that modifies configuration, devices, streams and buffers.
//   A = audio thread: one per start(), MMCSS "Pro Audio" at critical priority, woken by the WASAPI render event of
//       the primary output. A never takes locks, never allocates, never logs and never calls asioMessage(); it
//       publishes to H through atomics plus PostMessage() to the hidden window.
//   N = Windows notification threads (MMDevice notifications, volume callbacks, process-loopback activation): they
//       only queue events (under evLock) and post a message to H. Exception: the volume callback reads
//       IAudioEndpointVolume under the shared side of volLock; closeStreams takes it exclusively before releasing.
//   Members below are annotated with the thread that owns them where it matters.
#pragma once
#include "common.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <atomic>
#include <vector>
#include <cstdint>
#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"
#include "live.h"
#include "dmix.h"
#include "limiter.h"
#include "fixedratio.h"
#include "sched.h"
#include "ratepolicy.h"

// {A63175BD-B872-4FA5-9595-464F0236E1D0}
extern const CLSID CLSID_WasapiBridge;
extern std::atomic<long> g_objects;     // live objects keeping the DLL loaded (DllCanUnloadNow)
extern HINSTANCE g_hInst;

constexpr int kMaxCh = 32;              // ASIO channels per direction

// Sample format of an opened WASAPI stream.
struct DevFmt {
    int  channels = 2;
    int  bytes    = 4;      // container bytes per sample
    int  block    = 8;      // bytes per frame
    bool isFloat  = true;
    bool clamp    = false;  // clip float to [-1, 1] (Exclusive output: no engine does it for us)
    int  validBits = 32;    // valid bits (24 in a 32-bit container, etc.)
};

// Interleaved N-channel ring buffer of float frames. Used by a single thread (the audio thread): no locks.
// init() allocates and must run on the host thread before the audio thread starts. pushSlot() overwrites the oldest
// frame when full (callers keep the fill bounded so this does not happen in normal operation).
struct MultiFifo {
    std::vector<float> d; size_t cap = 0, head = 0, count = 0; int ch = 2;
    // 0.9.8: frames and channels are clamped (1..2^22 frames, 1..1024 channels) so frames * ch can never overflow
    // size_t on a 32-bit build (sizes derive from device-reported buffer sizes); allocation failure throws
    // std::bad_alloc on the host thread, where the IASIO barrier turns it into ASE_NoMemory.
    void   init(size_t frames, int channels) {
        if (channels < 1) channels = 1;
        if (channels > 1024) channels = 1024;
        if (frames < 1) frames = 1;
        if (frames > ((size_t)1 << 22)) frames = (size_t)1 << 22;
        ch = channels; d.assign(frames * (size_t)ch, 0.f); cap = frames; head = count = 0;
    }
    void   clear() { head = count = 0; }
    size_t size() const { return count; }
    float* slot(size_t i) { return &d[((head + i) % cap) * ch]; }        // i-th frame from the oldest
    float* pushSlot() {                                                   // appends a frame, returns its storage
        if (count == cap) { head = (head + 1) % cap; --count; }
        float* p = &d[((head + count) % cap) * ch]; ++count; return p;
    }
    void drop(size_t n) { if (n > count) n = count; head = (head + n) % cap; count -= n; }   // discards the oldest n
};

// Capture source (device input, additional input or loopback) with drift correction (Hermite or sinc + PI).
// Streams are opened/closed by H; fifo/pos/ratio/fillEma/integ/primed are audio-thread state (reset by H in start()
// before the thread exists); counters and ratioView are atomics read by H.
struct InputSource {
    const char*          tag = "";
    IAudioClient*        client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    DevFmt               fmt;
    bool                 exclusive = false;
    int                  asioFirst = 0, asioCount = 0;   // ASIO input channels fed by this source
    UINT32               bufFrames = 0, target = 0, period = 0;   // WASAPI buffer, FIFO target fill, capture period
    MultiFifo            fifo;
    bool                 primed = false;
    bool                 resample = false;      // drift correction active
    bool                 sinc = false;          // polyphase sinc instead of Hermite (0.4.0)
    UINT32               estLat = 0;            // 0.5.0 (H): estimated latency before alignment (frames)
    double               posBase = 1.0;         // minimum read position after consuming: 1 (Hermite) or kSincHalf-1 (sinc)
    double               pos = 1.0, ratio = 1.0, fillEma = 0.0, integ = 0.0;
    std::atomic<double>  ratioView{1.0};
    std::atomic<uint32_t> underruns{0}, drops{0}, glitches{0};
    // 0.9.4: the device runs at its mix rate and the driver converts to the ASIO rate (fixedratio.h). conv: device
    // rate -> ASIO rate, before the FIFO. alignedIn: on the output's clock (no drift correction): FIFO frame q is
    // delivered at samplePosition q + inDelay (exact round trip, sched.h); qHead / qMade: absolute index of the
    // FIFO's oldest frame / frames produced since start (audio thread, reset by H in start()).
    fr::Converter        conv;
    bool                 convOn = false, alignedIn = false;
    double               devRate = 0;
    uint32_t             inDelay = 0;
    uint64_t             qHead = 0, qMade = 0;
    int64_t              qOff = 0;              // A: re-alignment after a dropout (0 normally)
    uint32_t             exMid = 0, exHi = 0;   // H: re-alignment thresholds (sched::InResync)
};

class WasapiBridge;

// IAudioEndpointVolumeCallback forwarding volume changes to the driver (runs on an N thread).
// `owner` is cleared by H (detach) before the endpoint is released so that in-flight callbacks do nothing.
// 0.9.8 (review H-list, use-after-free): a callback used to load `owner` and then call it with no lock, so H could
// clear `owner`, finish closeStreams and even delete the driver while a callback that had already loaded the
// pointer was about to use it. Now a callback holds `lock` shared for its whole use of the owner, and detach()
// takes it exclusively: once detach() returns, no callback is inside the driver and none can enter it.
// Lock order: VolumeCallback::lock -> WasapiBridge::volLock (never the reverse; detach is called without volLock).
class VolumeCallback : public IAudioEndpointVolumeCallback {
public:
    explicit VolumeCallback(WasapiBridge* o, int idx = -1) : owner(o), index(idx) {}
    void detach() { AcquireSRWLockExclusive(&lock); owner = nullptr; ReleaseSRWLockExclusive(&lock); }
    virtual ~VolumeCallback() = default;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override;
    std::atomic<WasapiBridge*> owner;
    const int index;                    // -1 = primary output; 0..kMaxExtra-1 = additional output
    SRWLOCK lock = SRWLOCK_INIT;        // 0.9.8: shared by callbacks, exclusive by detach()
private:
    LONG refs = 1;
};

// Configured additional device (resolved by H in resolveDevices).
struct ExtraDev {
    std::wstring want, id, name;
    IMMDevice*   dev = nullptr;          // nullptr = not found (its channels exist and stay silent)
    GUID         container = {};
    int          mixCh = 2;              // channels of the device mix format
    int          ch = 0;                 // ASIO channels assigned (may be truncated to stay within 32)
    int          asioFirst = 0;
};

// Additional output (0.5.0). The master clock is the primary output: the sink has no event and is serviced at every
// wake-up of the audio thread. Open/close: H. fifo/pos/ratio/fillEma/integ/primed/written/ditherState: audio-thread
// state (reset by H in start() before the thread is created). Counters, ratioView and volGain: atomics.
struct OutputSink {
    const char*           tag = "";
    IAudioClient*         client = nullptr;
    IAudioRenderClient*   render = nullptr;
    IAudioClock*          clock = nullptr;       // optional: interpolated device position (better fill estimate)
    UINT64                clockFreq = 0;
    IAudioEndpointVolume* epVol = nullptr;       // Exclusive without hardware volume only (software volume)
    VolumeCallback*       volCb = nullptr;
    DevFmt                fmt;
    bool                  exclusive = false, softVolume = false, sinc = false;
    UINT32                bufFrames = 0, period = 0, targetPad = 0;
    int                   asioFirst = 0, asioCount = 0;
    double                sp = 0, posBase = 1.0, overLimit = 0;   // FIFO set point, minimum position, overflow limit
    MultiFifo             fifo;
    bool                  primed = false;
    double                pos = 1.0, ratio = 1.0, fillEma = 0.0, integ = 0.0;
    uint64_t              written = 0;            // frames handed to the device since start (silence included)
    std::atomic<double>   ratioView{1.0};
    std::atomic<float>    volGain{1.f};
    std::atomic<uint32_t> underflows{0}, overflows{0};
    UINT32                streamLat = 0, estLat = 0;   // H: stream latency and estimated total latency (frames)
    bool                  ditherActive = false;        // H at open: Exclusive integer output with <= 24 valid bits
    uint32_t              ditherState[kMaxCh] = {};    // A (seeded by H in start())
};

// IMMNotificationClient: device added/removed/state/default changes (runs on an N thread; only queues events).
// 0.9.8: same detach rule as VolumeCallback (shared `lock` around every use of `owner`, exclusive in detach()), so
// the driver can be destroyed right after detach() even if Windows is delivering a notification at that moment.
// Lock order: DeviceNotifier::lock -> WasapiBridge::evLock.
class DeviceNotifier : public IMMNotificationClient {
public:
    explicit DeviceNotifier(WasapiBridge* o) : owner(o) {}
    void detach() { AcquireSRWLockExclusive(&lock); owner = nullptr; ReleaseSRWLockExclusive(&lock); }
    virtual ~DeviceNotifier() = default;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD state) override;
    STDMETHODIMP OnDeviceAdded(LPCWSTR id) override;
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override;
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override;
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override;   // 0.8.0 (G4): device format
    std::atomic<WasapiBridge*> owner;
    SRWLOCK lock = SRWLOCK_INIT;        // 0.9.8: shared by callbacks, exclusive by detach()
private:
    void forward(const char* what, LPCWSTR id, EDataFlow flow, bool isDefault);   // 0.9.8: locked, never throws
    LONG refs = 1;
};

// 0.7.0 (F1). LIVE settings published by H to the audio thread with SeqPublished (live.h). Plain data only; the
// audio thread keeps its own copy (WasapiBridge::lv) and refreshes it at the start of every wake-up.
struct LiveParams {
    float  gain = 1.f;               // Gain (linear)
    bool   dither = true;            // Dither key (TPDF is applied only where the format allows it)
    bool   routeDefault = true;      // OutputMap empty: identity routing (mono device = (L+R)/2)
    int8_t route[kMaxCh] = {};       // main device channel -> ASIO channel (-1 = silence)
    bool   dmOn = false;             // panel direct monitoring active (enabled, input present)
    bool   dmMono = false;           // the input "pair" is a single channel: pan it to the output pair
    int    dmIn = 0, dmOut = 0;      // first device input channel / first main output channel of the pairs
    float  dmG[2] = {0.f, 0.f};      // stereo: gain in0->out0, in1->out1; mono: gain in0->out0, in0->out1 (0 if muted)
    bool   limOn = false;            // 0.8.0 (G2): hearing-safety limiter
    float  limCeil = 0.891f;         // ceiling (linear)
};

// Status snapshot for the control panel (filled by getStatus on H).
struct StatusInfo {
    bool   streamsOpen;
    bool   outExclusive, outLowLatency;
    char   outFmt[32], inFmt[32];
    double rate;
    long   bufSize, inLat, outLat;
    UINT32 outPeriod;                 // device frames of the output stream
    // 0.9.4: engine of the main output: 0 Shared (Windows converts), 1 low-latency Shared (IAudioClient3),
    // 2 Shared at the mix rate, 3 Exclusive; device rate and internal conversion
    int    engineKind;
    double outDevRate, mixRate;
    int    monAvail;                  // 0.9.4: direct monitoring possible with the open streams (1/0; -1 unknown/no input)     // stream rate of the main output; Windows mix rate when opened
    bool   outConv, convSinc, convSoxr;     // 0.9.10: convSoxr = the soxr engine converts the main output
    long   nIn, nOut;
    uint32_t outGlitches, timeouts;
    uint32_t inUnderruns, inDrops, inGlitches;
    double inPpm, lbPpm;
    bool   hasInput, hasLoopback, monitorActive, inResample;
    float  loadNow, loadMax;          // DSP load of the host callback, % of the block duration
    bool   sinc, dither;              // sinc resampling active on some input; TPDF dither active on the output
    std::wstring outName, inName;
    std::wstring extras;              // 0.5.0: one line per additional device (drift and counters)
    long   outDevChannels;            // channels of the primary output device (for routing)
    bool   captureDenied;             // 0.5.1: Windows denied a capture stream for privacy (E_ACCESSDENIED)
    long   nInDev, nOutPrim;          // 0.7.0: device input channels / main output channels (direct monitoring pairs)
    bool   safeMode;                  // 0.7.0 (F8): started with safe settings after a crash
    double outLatOffsetMs, inLatOffsetMs;   // 0.7.0 (F4): offsets included in inLat / outLat
    bool   dmActive;                  // 0.7.0 (F5): panel direct monitoring is mixing
    uint32_t inDropouts;              // 0.7.0: input underruns + drops of every capture source (monitor window)
    // 0.8.0
    bool   outBluetooth, inBluetooth; // G1: endpoint enumerator is Bluetooth (BTHENUM / BTHLEDEVICE / BTHHFENUM)
    bool   a2dpBlocked;               // G1: the input was not opened to keep the Bluetooth output in A2DP
    long   recBuffer;                 // G1: recommended minimum ASIO buffer for the Bluetooth output (0 = none)
    bool   limiterOn;                 // G2
    bool   rateAuto;                  // G4: SampleRate=Auto
    std::wstring hostProfile;         // G3: profile applied for this program ("" = none)
};

// Meter snapshot for the panel (H).
struct MeterSnapshot {
    long     nIn = 0, nOut = 0;
    float    in[kMaxCh] = {}, out[kMaxCh] = {};      // linear peak since the previous read
    bool     inAct[kMaxCh] = {}, outAct[kMaxCh] = {};
    uint32_t clipIn = 0, clipOut = 0;                // bit c = channel c clipped (>= 0 dBFS), latched
    bool     running = false;
    bool     limOn = false; float limGrDb = 0.f;      // 0.8.0 (G2): limiter active, largest gain reduction since last read
};

// Counter reset requests (bits of WasapiBridge::resetReq)
enum { WB_RESET_CLIPS = 1, WB_RESET_STATS = 2, WB_RESET_ALL = 3 };

// Device event queued by an N thread and handled by H.
struct DevEvent { std::wstring id; EDataFlow flow; bool isDefault; const char* what; };

// Reset reasons published by threads other than H
enum ResetReason { RR_NONE = 0, RR_WASAPI, RR_CAPTURE, RR_DEVICE, RR_PANEL };

// 0.7.0 (F1): result of WasapiBridge::applyPanelChanges, shown by the panel after "Apply".
enum class ApplyResult { Live, ResetRequested, ResetOnReload };

// Meter consumers (0.7.0): the panel and the monitor window each get every peak (readMeters keeps one
// accumulator per consumer on H, so one window never steals the peaks of the other).
enum { WB_METERS_PANEL = 0, WB_METERS_MONITOR = 1, WB_METER_CONSUMERS = 2 };

// The ASIO driver object. IASIO methods are called by the host on H (the ASIO specification requires all calls
// except getSamplePosition-style queries from callbacks to come from the thread that loaded the driver).
class WasapiBridge : public IASIO {
public:
    WasapiBridge();
    virtual ~WasapiBridge();

    // IUnknown. QueryInterface accepts the CLSID as IID (ASIO convention). The last Release may happen on the audio
    // thread (a host releasing from inside bufferSwitch): destruction is then deferred to H through the message window.
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IASIO (all on H). See asio.h for the contract of each method.
    ASIOBool  init(void* sysHandle) override;        // loads config, creates the message window, resolves devices
    void      getDriverName(char* name) override;
    long      getDriverVersion() override;
    void      getErrorMessage(char* string) override;
    ASIOError start() override;                      // creates the audio thread
    ASIOError stop() override;                       // from A: only signals; from H: signals and waits (10 s)
    ASIOError getChannels(long* numIn, long* numOut) override;
    ASIOError getLatencies(long* inLat, long* outLat) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* prefSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate rate) override;
    ASIOError getSampleRate(ASIOSampleRate* rate) override;
    ASIOError setSampleRate(ASIOSampleRate rate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;   // lock-free (seqlock reader)
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize, ASIOCallbacks* cbs) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;               // modal control panel
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;
    void      onStatsTimer();                        // 0.9.8: H, every 10 s: "Stats" trace event (counters only)

    // Threads other than H: they only queue / publish (never touch H state).
    void queueDeviceEvent(const char* what, LPCWSTR id, EDataFlow flow, bool isDefault);   // N
    void postReset(ResetReason r);                   // A or N: publish a reset reason once, wake H
    void onVolumeNotify(int index = -1);             // N (volume callback) or H (at open)
    // H only
    void onHostMessage(UINT msg);                    // dispatch of the hidden window's private messages
    void requestReset(const char* why);              // kAsioResetRequest (may destroy the driver synchronously)
    // 0.7.0 (F1): applies a configuration saved by the panel. LIVE differences take effect at the next buffer;
    // RESET differences make the driver ask the host to reopen it (kAsioResetRequest, deferred through the hidden
    // window so it never runs inside the panel's message handler).
    ApplyResult applyPanelChanges(const Config& saved);
    bool safeMode() const { return safe; }           // F8: running with safe settings after a crash
    void leaveSafeMode();                            // F8: clear the crash marker and reopen with the user's settings
    void openPanelFromMonitor();                     // F2: "Settings" button of the monitor window
    void monitorClosedByUser();                      // F2: "Close" button (MonitorWindow=0, saved)
    void setMonitorPlacement(int x, int y, int w, int h, bool topmost);   // F2: saved when moved/resized/pinned
    bool hostHoldsRef() const { return refs > 1; }
    void destroyFromHost() { teardown(); }
    void teardown();                                 // stop + delete, or leak on purpose if the audio thread is stuck
    void getStatus(StatusInfo& s);
    void readMeters(MeterSnapshot& m, int consumer = WB_METERS_PANEL);   // peaks accumulated since that consumer's last read
    void resetCounters(uint32_t what);               // WB_RESET_*; delegated to A while it runs
    // Configuration shown and edited by the panel: the user's settings (in safe mode these differ from the safe
    // settings actually in use, which are never written to config.ini).
    // Fix 2: uiCfg is the saved configuration (what config.ini holds). It is the base for display and Apply/Save
    // in the panel and the monitor window; cfg is only what is currently open (safe values in safe mode, the old
    // RESET values until the host reopens the driver). Safe-mode values are never written to config.ini.
    const Config& config() const { return uiCfg; }
    bool isDying() const { return dying.load(); }    // fix 7: last reference gone, WM_WB_DELETE pending
    bool tryAddRef();                                 // 0.9.1 fix 3: AddRef only if alive (registry lookup)
    uint32_t generation() const { return streamGen; }   // fix 9: incremented by every disposeBuffers
    void createMonitorNow();                         // fix 1: msgThread only (WM_WB_MONCREATE)
    void destroyMonitorNow();                        // fix 1: msgThread only (WM_WB_MONDESTROY or direct)
    // 0.8.0 (G5): notification-area icon, same life cycle rules as the monitor window (msgThread only)
    void createTrayNow();
    void destroyTrayNow();
    void updateTray();
    void trayToggleMonitor();                        // left click on the tray icon
    bool monitorShown() const { return monWnd != nullptr; }
    // 0.9.0 (H3): the panel started an update check at `t` (seconds since 1970); kept in both configurations.
    void noteUpdateCheck(long long t) { uiCfg.updateLastCheck = t; cfg.updateLastCheck = t; }
    IMMDeviceEnumerator* enumerator() { return enumr; }
    HWND hostWindow() const { return hostWnd; }

private:
    // ---- devices (H)
    bool  resolveDevices();
    void  resolveExtras(EDataFlow flow, const std::vector<std::wstring>& want, ExtraDev* ex, int& n,
                        const std::wstring& primaryId, long& used, long limit);
    bool  isExtraId(const std::wstring& id) const {
        for (int k = 0; k < nOutEx; ++k) if (!outEx[k].id.empty() && outEx[k].id == id) return true;
        for (int k = 0; k < nInEx; ++k) if (!inEx[k].id.empty() && inEx[k].id == id) return true;
        return false;
    }
    void  handleDeviceEvent(const DevEvent& e);
    // ---- streams (H)
    bool  openStreams();
    void  closeStreams();
    bool  openOutput();
    void  openDeviceInput();
    void  openInput(InputSource& s, IMMDevice* dev, int devCh, const GUID& container, bool tryExclusive);
    void  openSink(int k);
    void  closeSink(OutputSink& s);
    bool  serviceSink(OutputSink& s, IAudioClient* c, IAudioRenderClient* rc, IAudioClock* clk, double now100ns);   // A
    void  buildRoute();
    void  openLoopback();
    bool  initExclusive(IMMDevice* dev, IAudioClient** client, DevFmt& fmt, UINT32& frames, UINT32& period, bool event, int wantCh);
    void  freeBuffers();
    ASIOError disposeImpl();                         // 0.9.8: disposeBuffers without the state check (internal callers)
    // 0.9.8: bodies of IASIO methods (the public methods add the exception barrier, the state checks and the trace)
    ASIOBool  initImpl(void* sysHandle);
    ASIOError startThread();                         // start(): per-run reset + CreateThread
    ASIOError stopImpl();
    ASIOError futureImpl(long selector, void* opt);
    void      latenciesNow(long& inL, long& outL);   // getLatencies without host pointers (also getStatus)
    void      channelName(bool isIn, long c, char (&out)[32]);   // getChannelInfo name, c already validated
    void  closeSource(InputSource& s);
    bool  onAudioThread() const { return GetCurrentThreadId() == audioTid.load(); }
    // ---- audio thread (A)
    static DWORD WINAPI threadProc(void* p);
    void  run();
    bool  pullSource(InputSource& s, IAudioCaptureClient* cap, bool isDevInput);   // true = device invalidated
    void  readSource(InputSource& s, int idx);
    void  bufferSwitch();
    void  writeOut(BYTE* dst, UINT32 frames);
    void  publishTime(uint64_t pos, uint64_t ns);    // seqlock writer
    void  applyResetRequest();                       // consumes resetReq
    void  setError(const char* msg);
    // ---- 0.7.0
    void  publishLive();                             // H: builds LiveParams from cfg + routing and publishes them
    void  mixMonitor(float* mt, const float* m, float k);   // A: one frame of direct monitoring (ramps + matrix)
    void  applySafeSettings();                       // H: overrides cfg with the safe defaults (F8)
    void  chooseRate(const char* when);              // H (0.9.5): rate precedence (ratepolicy.h), Auto, log
    void  updateMonitorWindow();                     // H: creates/destroys the floating window per cfg (F2)

    LONG   refs = 1;
    Config cfg;                                      // H: settings in use
    Config uiCfg;                                    // H: the saved (user) configuration (fix 2)
    std::atomic<bool> dying{false};                  // fix 7
    uint32_t streamGen = 0;                          // fix 9 (H)
    bool   safe = false;                             // H: safe mode for this session (F8)
    bool   hadStart = false;                         // H: start() succeeded at least once (clean-session check)
    HWND   monWnd = nullptr;                         // H: floating monitor window (F2), nullptr if not shown
    HWND   trayWnd = nullptr;                        // H: hidden window owning the tray icon (G5)
    // 0.8.0 (G1/G4/G6/G3), H state fixed at resolve/open time
    bool   outBt = false, inBt = false, a2dpBlocked = false;
    long   recBuf = 0;
    double mixRateOpen = 0;                          // Windows mix rate of the output when it was opened (G4)
    double autoRateSeen = 0;                         // G4: last Windows rate followed with SampleRate=Auto
    double pendingAutoRate = 0;                      // 0.9.1 fix 6: new Windows rate, becomes `rate` in disposeBuffers
    std::wstring hostProfile;                        // profile applied for this program at init (G3)
    char   chNameIn[kMaxCh][32] = {}, chNameOut[kMaxCh][32] = {};   // G6: names from the devices
    void   buildChannelNames();
    void   onFormatChanged(const std::wstring& id);  // G4
    void   applyAutoRate();                          // G4
    // G2 (A): limiter state for the main output and each extra output, release coefficient; GR published to H
    lim::Limiter limMain, limSink[kMaxExtra];
    float  limRel = 0.001f, limRelMain = 0.001f;   // 0.9.4: limRelMain at the main output device rate
    std::atomic<float> limGr{0.f};
    float  limGrAcc[WB_METER_CONSUMERS] = {};       // H: per-consumer maximum of limGr (like the meter peaks)
    void   loadSaved(Config& c);                     // H (G3): LoadConfig + the host profile still in effect
    // F1: live settings. H publishes (pubLive); A copies into lv at every wake-up (lvSeq = last version seen).
    SeqPublished<LiveParams> pubLive;
    LiveParams liveH;                                // H: last published block
    LiveParams lv;                                   // A (H writes it only in start(), before the thread exists)
    uint32_t   lvSeq = 0;                            // A
    // F5: direct-monitoring gain ramps (A). host* = ASIO kAsioSetInputMonitor per input; dm* = panel pair.
    dmix::Ramp hostRampL[kMaxCh], hostRampR[kMaxCh], dmRamp[2];
    bool       monFeed = false;                      // A: monitoring input is being collected this wake-up
    float      monK = 1.f;                           // A: ramp coefficient (5 ms), set in start()
    std::vector<float> monScratch;                   // H allocates (openStreams); A: resampled input block
    std::atomic<uint32_t> monUnder{0};               // A: monitor FIFO underruns (informational)
    std::atomic<bool> latChangePending{false};       // H -> H (deferred kAsioLatenciesChanged)
    // Meter accumulators per consumer (H only): drained atomics are max-merged into every consumer.
    float meterAccIn[WB_METER_CONSUMERS][kMaxCh] = {}, meterAccOut[WB_METER_CONSUMERS][kMaxCh] = {};
    HWND   hostWnd = nullptr, msgWnd = nullptr;
    char   errMsg[124] = "";
    std::atomic<bool> initialized{false};            // 0.9.8: atomic (getSamplePosition may run on any thread)

    IMMDeviceEnumerator* enumr  = nullptr;
    DeviceNotifier*      notifier = nullptr;
    CRITICAL_SECTION     evLock;                    // protects evQueue (N writes, H drains)
    std::vector<DevEvent> evQueue;
    IMMDevice*           outDev = nullptr;
    IMMDevice*           inDev  = nullptr;
    std::wstring         outId, inId, outName, inName;
    GUID                 outContainer = {}, inContainer = {};
    bool                 outFallback = false, inFallback = false;   // configured device missing: default used
    int                  outDevCh = 2, inDevCh = 2;

    IAudioClient*        outClient = nullptr;
    IAudioRenderClient*  render    = nullptr;
    IAudioClock*         outClock  = nullptr;   // Exclusive only: dropout detection from the device position
    UINT64               outClockFreq = 0;
    IAudioEndpointVolume* epVol    = nullptr;
    VolumeCallback*      volCb     = nullptr;
    SRWLOCK              volLock = SRWLOCK_INIT;
    InputSource          srcIn, srcLb;
    // 0.5.0: additional devices (configuration resolved on H; streams opened on H; audio state on A)
    ExtraDev             outEx[kMaxExtra], inEx[kMaxExtra];
    int                  nOutEx = 0, nInEx = 0;
    OutputSink           sinks[kMaxExtra];
    InputSource          srcEx[kMaxExtra];
    long                 nOutPrim = 2, lbFirst = 0;
    int                  routeSrc[kMaxCh] = {};        // primary device channel -> ASIO channel (-1 = silence)
    bool                 routeDefault = true;
    // Alignment (H at open; A only reads primDelayFrames and uses primDelay, which H allocates and A fills in start()).
    bool                 aligned = false;
    UINT32               primOutLat = 0, primInLat = 0, alignOut = 0, alignIn = 0, primDelayFrames = 0;
    MultiFifo            primDelay;                    // delay line for the physical channels of the primary output
    void                 computeAlignment();
    // 0.5.1 (H): a capture stream was denied by Windows privacy settings (microphone) at the last open
    bool                 captureDenied = false;
    HRESULT              lastExclHr = S_OK;            // last Initialize failure inside initExclusive
    void                 noteCaptureHr(HRESULT hr, const char* tag);

    HANDLE outEvent = nullptr, stopEvent = nullptr, thread = nullptr;
    std::atomic<DWORD> audioTid{0};
    bool   threadStuck = false;                      // stop() timed out: the thread is blocked inside the host
    HANDLE stuckThread = nullptr;
    bool   timerRaised = false;                      // timeBeginPeriod(1) active
    DWORD  msgThread = 0;
    ASIOCallbacks* cbRun = nullptr;     // copy of cb used by the audio thread
    double wakeNs = 0; bool firstOfWake = false;
    std::atomic<uint64_t> lastP{0}, lastT{0};   // last valid pair (seqlock reader; 0.9.8: atomic, readers on any thread)
    bool  stuckAlive();
    DevFmt outFmt;
    UINT32 outBufFrames = 0, outPeriod = 0, outStreamLat = 0;
    bool   outExclusive = false, outLowLatency = false, softVolume = false;
    bool   streamsOpen = false;

    double rate = 48000.0;                           // H: current ASIO rate (getSampleRate)
    // 0.8.3: copy of `rate` for the audio thread, written by H only in start() before the thread exists. H may
    // change `rate` while the stream still runs (setSampleRate; since 0.9.1 SampleRate=Auto waits in pendingAutoRate
    // until disposeBuffers); the running stream keeps reporting the rate it was started with, without a data race.
    double rateRun = 48000.0;
    bool   hostSetRate = false;
    rp::Src rateSrc = rp::Src::Panel;               // 0.9.5: where `rate` came from (log)
    long   bufSize = 256;
    long   nIn = 0, nOut = 2, nInDev = 0;
    ASIOCallbacks* cb = nullptr;                     // points to cbStore while buffers exist
    // 0.9.8 (review H-list, TOCTOU): a private copy of the host's ASIOCallbacks, taken once in createBuffers. Before,
    // the driver kept the host's pointer, so a host that freed or rewrote its structure later (e.g. nulled a member
    // after the null checks) could make the audio thread call through a stale or null pointer.
    ASIOCallbacks cbStore = {};
    bool   timeInfoMode = false, resetSupported = false, overloadSupported = false, latChangedSupported = false;
    bool   buffersCreated = false;
    float* outBufs[kMaxCh][2] = {};
    float* inBufs[kMaxCh][2]  = {};
    bool   outActive[kMaxCh] = {}, inActive[kMaxCh] = {};
    std::vector<float> bufMem;

    MultiFifo outFifo;                                // A: host output frames waiting for the device (device rate)
    // 0.9.4: Shared output at the device mix rate with internal fixed-ratio conversion (ASIO rate -> device rate).
    // H sets them at open; A uses outConv (bufferSwitch pushes ASIO frames, run() pulls device frames on demand).
    fr::Converter outConv;
    bool     outConvOn = false;
    // 0.9.6 (review R2): CPU budget (multiply-accumulates per second) still free for fixed-ratio converters in this
    // stream open. openStreams resets it to fr::kMacBudget; openOutput / openInput pass it to fr::Choose and
    // subtract fr::Cost of every converter they configure. H only.
    double   convBudgetLeft = fr::kMacBudget;
    double   outDevRate = 48000.0;                    // H: rate of the main output stream (= rate unless converting)
    UINT32   outFill = 0;                             // H: device frames kept queued (Shared) / buffer (Exclusive)
    UINT32   outPeriodAsio = 0;                       // H: outPeriod in ASIO frames (input targets, sinks, alignment)
    int      engineKind = 0;                          // H: 0 Shared (Windows converts), 1 low-latency, 2 Shared at mix rate, 3 Exclusive
    char     engineLine[200] = "";
    UINT32   monKeep = 0;                             // H: monitor frames kept queued (writeOut bound)
    int      monPath = 0;                             // H: direct monitoring source: 0 none, 1 capture packets, 2 delivered blocks                    // H: English description (log, Copy status, test tool)
    sched::BlockClock blockClock;                     // A: systemTime of the ASIO blocks
    MultiFifo monFifo;                                // A: input frames for direct monitoring
    std::atomic<int>   monOut[kMaxCh];
    std::atomic<float> monGainL[kMaxCh], monGainR[kMaxCh];
    std::atomic<bool>  monAny{false};

    int    curIndex = 0;                              // A: next ASIO double-buffer half
    std::atomic<bool>     running{false};
    std::atomic<bool>     resetRequested{false}, devicesDirty{false};
    std::atomic<int>      pendingReset{RR_NONE};
    std::atomic<bool>     monResetPending{false};    // 0.9.4: direct-monitoring availability changed at a reopen (H)
    int                   monAvailLast = -1;         // H: availability at the last open of this instance (-1 unknown)
    std::atomic<bool>     pendingOverload{false};
    std::atomic<long>     lastHr{0};
    std::atomic<float>    volGain{1.f};
    // Sample position / system time published with a seqlock (getSamplePosition on H, written by A)
    std::atomic<uint32_t> tsSeq{0};
    std::atomic<uint64_t> tsPos{0}, tsNs{0};
    uint64_t samplePos = 0;      // audio thread only
    // 0.9.4: the ASIO timestamp anchor moved to blockClock (sched::BlockClock)
    std::atomic<uint32_t> cntOutGlitch{0}, cntTimeouts{0};
    std::atomic<uint64_t> cntSwitches{0};            // 0.9.8: atomic (A stores, H reads for the Stats event)

    // Meters. Writer: audio thread (CAS maximum, relaxed). Reader: host thread (exchange(0)).
    std::atomic<float>    peakIn[kMaxCh], peakOut[kMaxCh];
    std::atomic<uint32_t> clipInMask{0}, clipOutMask{0};    // A: fetch_or; cleared through resetReq
    // DSP load. Writer: audio thread. Internal state (EMA, maximum, QPC factor) belongs to the audio thread only.
    std::atomic<float>    loadNow{0.f}, loadMax{0.f};
    double   qpcToLoad = 0, loadEma = 0, loadMaxA = 0, loadAlpha = 0;
    uint32_t loadSkip = 0;
    // Reset request (H sets bits, A consumes them with exchange). Without an audio thread H applies it directly.
    std::atomic<uint32_t> resetReq{0};
    // 0.4.0. Sinc table: built once by H (openStreams) and never freed before the destructor; A only reads it.
    std::vector<float> sincTab;
    // TPDF dither: ditherActive is set by H when the output opens; ditherState is audio-thread state (seeded by H
    // in start() before the thread is created).
    bool     ditherActive = false;
    uint32_t ditherState[kMaxCh] = {};
};

// driver.cpp (0.7.1; 0.9.1 fix 3: locked): registry of initialized driver objects, used by the panel to rebind after
// the host reloads the driver. FindNewerInstance returns the instance with a reference already added (the caller
// releases it), or nullptr.
void RegisterInstance(WasapiBridge* d);
void UnregisterInstance(WasapiBridge* d);
WasapiBridge* FindNewerInstance(WasapiBridge* old);
// panel.cpp: modal control panel (H). Holds its own reference to `drv` (or to the instance it rebinds to) and
// releases it before returning.
void ShowControlPanel(WasapiBridge* drv);
// panel.cpp (0.9.3): releases the cached application icons (DllMain, when the DLL is unloaded).
void FreeAppIcons();
// panel.cpp (F2): floating monitor window (H). Create returns nullptr on failure; Destroy accepts nullptr.
HWND CreateMonitorWindow(WasapiBridge* drv);
void DestroyMonitorWindow(HWND w);
void SetMonitorTopmost(HWND w, bool on);
// panel.cpp (0.8.0, G5): hidden window owning the notification-area icon (msgThread only). Create returns nullptr
// on failure; Destroy removes the icon (NIM_DELETE), destroys the window and accepts nullptr.
HWND CreateTrayWindow(WasapiBridge* drv);
// panel.cpp (0.9.0): selects the theme and interface language of `c` (host thread) unless the control panel is
// open (it may be previewing other choices).
void ApplyUiPrefs(const Config& c);
void DestroyTrayWindow(HWND w);
// driver.cpp (H): PIDs that own an audio session on any active render endpoint.
void ListAudioSessionPids(IMMDeviceEnumerator* e, std::vector<DWORD>& pids);
