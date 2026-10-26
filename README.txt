WASAPI Driver for ASIO(R) 0.9.14 (public beta) - Generic ASIO driver on top of WASAPI, Windows 10/11 (x64, ARM64, x86)
======================================================================================================================

A free, open-source (GPL-3.0-or-later) ASIO driver for Windows 10/11 that runs on top of the audio
devices Windows already has (WASAPI, low-latency IAudioClient3 engine). Use Cubase, Nuendo, Ableton Live,
DaVinci Resolve and other ASIO hosts with built-in sound, USB headsets, HDMI monitors or Bluetooth
earbuds, without an audio interface. Status: public beta (pre-release builds are not code-signed yet).

WHERE TO GET IT (0.9.14, public beta)
-------------------------------------
Releases page: https://github.com/irregbex/WASAPI-Driver-for-ASIO/releases
  The current public beta is version 0.9.14. Open its release (or a newer
  release) and download, under "Assets":
    WASAPI-Driver-for-ASIO-<version>-Setup.exe   the installer (recommended)
    WASAPI-Driver-for-ASIO-<version>-bin.zip     the portable files (DLLs, tools, install.bat, documents)
  0.9.9: both contain WasapiDriverARM64X.dll for Windows on Arm (one driver for native ARM64 and emulated
  x64 DAWs, section 3; beta: confirmed with native ARM64 Cubase, emulated x64 not yet reported, KI-9).
    SHA256SUMS.txt                               checksums (PowerShell: Get-FileHash <file> -Algorithm SHA256)
  "Source code (zip)" / "WASAPI-Driver-for-ASIO-<version>-src.zip" contain the complete source tree.
  Versions marked "Pre-release" are betas. Download only from this page; copies elsewhere are not checked.
Source code and problem reports: https://github.com/irregbex/WASAPI-Driver-for-ASIO
  (Issues > New issue; security problems privately as described in SECURITY.txt).

WHY USE IT - COMPARED WITH STEINBERG'S GENERIC DRIVERS
------------------------------------------------------
Steinberg products install a generic ASIO driver for computers without an audio interface: the current
"Steinberg built-in ASIO Driver" or, on older installations, the "Generic Low Latency ASIO Driver", which
Steinberg no longer develops (Steinberg Help Center, "Steinberg built-in ASIO Driver: information &
download").

Steinberg built-in ASIO Driver - release history (Steinberg release notes):
  1.0.7   July 2023          first release (with WaveLab Cast 2)
  1.0.8   24 October 2024    Windows 11 on Arm (preview)
  1.0.9   18 December 2024   unified installer; LATEST version as of October 2026 (file version 1.0.9.26)

Same driver, two names: the built-in driver's control panel still shows the OLD name in some languages, so it
is easy to mistake it for the discontinued Generic driver. Check the version in the title (1.0.9.x = built-in).
Panel titles found in version 1.0.9.26:
  English    "Steinberg built-in ASIO Driver - Version"                 correct name
  German     "Steinberg eingebauter ASIO-Treiber - Version"             correct name
  Spanish    "Controlador Generico ASIO de Baja Latencia - Version"     OLD name
  French     "Pilote ASIO generique a faible Latence - Version"         OLD name
  Italian    "Driver ASIO generico a bassa latenza - versione"          OLD name
  Japanese   "Common low latency ASIO driver" (in Japanese)             OLD name
  (Spanish/French accents omitted in this ASCII file.)

                       Generic Low Latency    Steinberg built-in 1.0.9   WASAPI Driver for ASIO
                       (discontinued)         (18 December 2024)
  Other apps while     Exclusive in our       Shared in our tests:       Shared by default; Exclusive
  the DAW is open      tests: other apps      other apps kept playing    optional
                       went silent (KI-1)
  Sample-rate          -                      Automatic (soxr)           Automatic: 128-tap sinc (low
  conversion                                                             latency); soxr HQ selectable (0.9.10)
  Follows the Windows  -                      No: a device must be       Yes: "(Windows default)"
  default output                              picked in its port list    follows Windows (e.g.
                                              (Bluetooth only while      headphones on/off, in the
                                              already connected)         case and back)
  Bluetooth            No special handling    Works when picked          Detection, buffer advice,
                                              directly; after the        latency offsets; output
                                              headphones disconnect      moves to the next device
                                              the selection is lost      and comes back by itself
                                              and must be set again      on reconnect
  Several devices      Port list              Port list                  Up to 3 extra devices, drift
                                                                         correction, routing, app loopback
  Windows on Arm       -                      Windows 11 on Arm          ARM64 and Arm64X (one DLL for
                                              (preview, per Steinberg)   ARM64 and emulated x64 hosts)
  32-bit hosts         -                      Not available              x86 DLL included
  Diagnostics          -                      -                          Log, Diagnostics, host test,
                                                                         TraceLogging + .wprp
  Source code          Closed                 Closed                     Open (GPL-3.0)

Steinberg's drivers have years of use on many machines; this driver is a public beta. The built-in driver
column is based on version 1.0.9.26 (x64), Steinberg's published information and our own tests; "-" means we
found no such feature. Both drivers are products of Steinberg Media Technologies GmbH; ASIO is a registered
trademark of Steinberg Media Technologies GmbH. This project is not affiliated with Steinberg.

Code signing policy: CODE-SIGNING-POLICY.txt (signing via SignPath Foundation applied for; current beta
builds are not signed yet). Privacy: PRIVACY.txt (no telemetry; optional update check, off by default).

New here? Read QUICK-START.txt first (one page: install, select the driver in your DAW, recommended
settings, uninstall), then FAQ.txt for the common questions. Both are in Docs\ of the release folder and
in the installation folder.

RELEASE FOLDER LAYOUT
---------------------
  WASAPI-Driver-for-ASIO-0.9.14-Setup.exe Installer (recommended). Run it, then follow Docs\QUICK-START.txt.
  README.txt          This file (complete manual).
  Docs\               QUICK-START.txt (one page), FAQ.txt, COMPATIBILITY.txt (tested hosts and devices),
                      VERSION.txt (changelog), TEST-GUIDE.txt (test plan), ROADMAP.txt, TRACING.txt
                      (diagnostic tracing).
  Legal\              LICENSE.txt, COPYRIGHT.txt, THIRD-PARTY-NOTICES.txt, TRADEMARKS.txt, DISCLAIMER.txt,
                      PRIVACY.txt, SECURITY.txt, CONTRIBUTING.txt.
  Portable-Install\   The three driver DLLs with install.bat / uninstall.bat (no Setup; section 3, option B);
                      0.9.9: plus WasapiDriverARM64X.dll in packages built by the CI (Windows on Arm).
  Tools\              WasapiDriverDiag.exe (diagnostics) and WasapiDriverTest.exe (automatic, latency and
                      soak test); 0.9.8: WasapiDriver.wprp, "Start trace.bat", "Stop trace.bat" and
                      TRACING.txt (diagnostic tracing, section 42).
  Source-Code\        The complete source tree and the developer documents (BUILDING, ARCHITECTURE,
                      CODE-REVIEW, RELEASING, SIGNING).
The Setup installs the program, the tools and all documents into one folder:
%ProgramFiles%\WASAPI Driver for ASIO (Start menu > WASAPI Driver for ASIO).

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

Formerly named "WasapiBridge" (up to 0.5.1). See section 3 to upgrade.

Contents
  1. What it is
  2. Requirements
  3. Installing and uninstalling
  4. First use (Cubase, Ableton Live, REAPER)
  5. Control panel
  6. Level meters
  7. DSP load and counters
  8. Diagnostics tool (WasapiDriverDiag.exe)
  9. Automatic test (WasapiDriverTest.exe)
  10. Shared and Exclusive modes
  11. Loopback inputs (recording Windows audio)
  12. Clock drift correction
  13. Direct monitoring
  14. Following device changes
  15. Extra devices (beta)
  16. Output routing
  17. Latency: what each number means (and latency offsets)
  18. Troubleshooting
  19. Settings and log files
  20. Known limitations
  21. License and trademarks
  22. Files in this folder
  23. History
  24. Floating monitor window
  25. Crash protection and safe mode
  26. Bluetooth headphones and speakers
  27. Hearing-safety limiter
  28. Automatic profile per program
  29. Following the Windows sample rate
  30. Notification-area (tray) icon
  31. Channel names
  32. Panel visuals (animated background)
  33. Themes
  34. Interface languages
  35. Checking for updates (optional, off by default)
  36. Stable and beta features
  37. Accessibility and keyboard
  38. Soak test (long-run stability)
  39. Help tips
  40. Application icon
  41. Low-latency engine at any sample rate
  42. Diagnostic tracing (ETW)

----------------------------------------------------------------------
1. WHAT IT IS
----------------------------------------------------------------------
WASAPI Driver for ASIO is a generic ASIO driver: it lets you use any Windows
sound device (built-in, USB, HDMI, Bluetooth in Shared mode) from
ASIO-compatible programs (Cubase, Nuendo, Ableton Live, REAPER, Studio One,
FL Studio, Bitwig, etc.), with low latency and without silencing the rest of
Windows. The control panel shows the Steinberg ASIO Compatible logo.

It is not a kernel driver: it is a DLL that the DAW loads and that talks to
Windows through WASAPI, the native Windows audio interface. It needs no
Microsoft signature and no reboot.

Features:
- Shared mode (Windows keeps playing) and Exclusive mode (lowest latency).
- Every channel of the device (up to 32 inputs and 32 outputs).
- 2 "Loopback" inputs that record Windows audio (YouTube, Spotify,
  Discord), optionally excluding the DAW itself (no feedback), or a single
  application.
- Automatic drift correction when the microphone and the output are
  different devices (no clicks).
- ASIO direct monitoring (Cubase "ASIO Direct Monitoring").
- Automatic reopening when devices are plugged in or removed.
- Dark tabbed control panel: live status tiles (latencies, DSP load,
  glitches), per-channel level meters with clip indicators, extra devices,
  full status and "About".
- Profiles for quick switching between engine settings.
- Diagnostics tool (WasapiDriverDiag.exe) that writes a text report to
  attach to a bug report.
- Automatic test (WasapiDriverTest.exe) that behaves like a DAW and checks
  the driver, with an optional real round-trip latency measurement using a
  cable.
- Windows volume works in Exclusive mode too.
- 0.7.0: most settings change while the audio runs (no restart); a floating
  monitor window (meters, load, dropouts, latencies); latency offsets for
  sample-accurate recording; direct monitoring inside the driver; crash
  reports and an automatic safe mode after a crash.
- 0.8.0: Bluetooth mode (detection, recommended buffer, protection of the
  stereo A2DP sound), an optional hearing-safety limiter, an automatic
  profile per program, "Follow Windows" sample rate, a tray icon, and ASIO
  channel names taken from the devices.
- 0.8.1: optional animated panel background in demoscene style (plasma,
  starfield, copper bars, tunnel, audio-reactive) and a scroller on the
  About tab.
- 0.9.0: six color themes (including retro looks), four more background
  effects, an optional update check (off by default) and the interface in
  English, Spanish, Portuguese (Brazil), French and German.
- 0.9.4 / 0.9.10: the low-latency engine at any project rate, with an
  internal conversion: a 128-tap sinc by default (low latency) or, when
  chosen, the SoX Resampler library (soxr HQ, best quality, ~8-10 ms more
  latency at 44.1-192 kHz, more at 16-32 kHz) (section 41).

----------------------------------------------------------------------
2. REQUIREMENTS
----------------------------------------------------------------------
- Windows 10 version 2004 (build 19041) or later, or Windows 11.
- A 64-bit DAW (x64 or ARM64). 32-bit (x86) DAWs work as well.
- For Exclusive mode: in mmsys.cpl > device > Properties > Advanced, enable
  "Allow applications to take exclusive control of this device" (output
  and, if used, input).

----------------------------------------------------------------------
3. INSTALLING AND UNINSTALLING
----------------------------------------------------------------------
Option A - Installer (recommended)
  0. Download WASAPI-Driver-for-ASIO-0.9.14-Setup.exe from the 0.9.14 release
     (https://github.com/irregbex/WASAPI-Driver-for-ASIO/releases, "Assets");
     optionally compare its SHA-256 with SHA256SUMS.txt. For a newer version,
     use that version's release assets.
  1. Close the DAW.
  2. Double-click WASAPI-Driver-for-ASIO-0.9.14-Setup.exe.
  3. If SmartScreen appears: "More info" > "Run anyway" (the installer is
     not digitally signed).
  4. UAC: Yes. Welcome > Next. Notices > Next. License: I Agree. Folder: keep
     the suggested one > Install.
  5. Windows on ARM64 (0.9.9): no question. Setup installs the Arm64X DLL
     (WasapiDriverARM64X.dll) and registers it once; native ARM64 DAWs and
     x64 DAWs running under emulation both use it (each loads its own half
     of the file). Only a package built without the Arm64X DLL (the release
     notes say so) asks as in 0.9.8: Yes = native ARM64 DAW, No = x64 DAW
     (emulated); only that kind of 64-bit DAW then sees the driver.
  It installs into %ProgramFiles%\WASAPI Driver for ASIO and registers:
    64-bit view: WasapiDriver64.dll on x64 Windows; on ARM64
                 WasapiDriverARM64X.dll (0.9.9), or WasapiDriverARM64.dll /
                 WasapiDriver64.dll after the question above
    32-bit view: WasapiDriver32.dll
  On ARM64 the plain ARM64 and x64 DLLs are installed too but not
  registered. If the Arm64X DLL does not work for you (please report it,
  KNOWN-ISSUES.txt KI-9): uninstall, then use option B without
  WasapiDriverARM64X.dll (delete or rename it first): install.bat then
  asks for the ARM64 or the x64 DLL.
  Start menu > "WASAPI Driver for ASIO": Diagnostics, Automatic test,
  Latency test (cable), Soak test (60 min), Quick start.
  Uninstall: Settings > Apps > WASAPI Driver for ASIO > Uninstall.
  Files in use (0.9.6): if a program still has the driver loaded, Setup
  and the uninstaller say which file is in use and ask you to close your
  audio programs (Cubase, DaVinci Resolve, Ableton Live...); then Retry.
  A program that stays in Task Manager after closing must be ended there.
  Cancel changes nothing.

Option B - Portable (no installer)
  1. Put the Portable-Install folder in its final location (the registry
     stores the path; install.bat registers the DLLs of its own folder).
  2. Double-click install.bat (asks for UAC). On Windows on ARM64 it
     registers WasapiDriverARM64X.dll when that file is in its folder
     (0.9.9, no question); otherwise it asks: 1 = native ARM64 DAW, 2 = x64
     DAW. WasapiDriver32.dll is registered for 32-bit DAWs in every case.
  3. To uninstall: uninstall.bat.
  Use A or B, not both at the same time (the last one run sets the path).

Upgrading from WasapiBridge (0.5.1 or earlier): install on top, without
uninstalling first. The installer (and install.bat) removes the
HKLM\SOFTWARE\ASIO\WasapiBridge registration (64-bit and 32-bit), and the
installer also removes the old uninstall entry, the files of the old
installation folder and its shortcuts, then registers the new name (same
CLSID). In the DAW the driver is now called "WASAPI Driver for ASIO": select
it once in the audio settings. Your settings are copied automatically from
%APPDATA%\WasapiBridge to %APPDATA%\WASAPI Driver for ASIO the first time
the DAW loads the driver (the old folder is not deleted), and your profiles
are kept. If the DAW lists both names, run Diagnostics: it reports any
leftovers.

Registry keys (both methods):
  HKLM\SOFTWARE\ASIO\WASAPI Driver for ASIO      CLSID, Description
  HKLM\SOFTWARE\Classes\CLSID\{A63175BD-B872-4FA5-9595-464F0236E1D0}
      InprocServer32 = DLL path, ThreadingModel = Apartment
  (plus the same keys under WOW6432Node for 32-bit)

----------------------------------------------------------------------
4. FIRST USE
----------------------------------------------------------------------
Cubase / Nuendo
  1. Studio > Studio Setup > Audio System > ASIO Driver: WASAPI Driver for
     ASIO.
  2. In the list on the left, WASAPI Driver for ASIO > Control Panel: choose
     output, input, mode and buffer > "Apply".
  3. Project > Project Setup: Sample Rate (48 kHz recommended).
  4. Studio > Audio Connections (F4): check the output buses (Out L/R or
     Out 1..N) and create input buses (In L/R, Loopback L/R).
  In Cubase the buffer size is set by the driver (panel), not by Cubase.

Ableton Live
  Preferences > Audio > Driver Type: ASIO > Audio Device: WASAPI Driver for
  ASIO. Buffer Size is chosen in Live. "Hardware Setup" opens the panel.

REAPER
  Preferences > Audio > Device > Audio system: ASIO > Driver: WASAPI Driver
  for ASIO. "ASIO Configuration..." opens the panel.

----------------------------------------------------------------------
5. CONTROL PANEL
----------------------------------------------------------------------
Opened from the DAW (Cubase: Studio Setup > WASAPI Driver for ASIO >
Control Panel; Live: Hardware Setup; REAPER: ASIO Configuration). A dark
window with a dark title bar and its own taskbar and Alt+Tab entry. At the
top: name, version and the ASIO Compatible logo. Below it, five tabs; at the
bottom, always visible, "Apply" (green) and "Close".
"Apply" saves the settings and the panel stays open (0.7.0):
- Settings WITHOUT a dot are applied live, at the next audio buffer, without
  interrupting the audio: gain, dither, output routing, latency offsets,
  direct monitoring, the monitor window, the log, following device changes.
  The bottom bar says "Applied live."
- Settings marked with a dot after their name need the driver to be reopened: output
  and input device, mode, buffer, sample rate, low latency, drift
  correction, resampler, loopback, extra devices and alignment. The driver
  asks the DAW to restart it (kAsioResetRequest) and the bottom bar says
  "The DAW will restart the driver." The audio stops for a moment while the
  DAW reopens it. If the DAW does not support that request, restart its
  audio engine yourself.
"Close" closes the panel; changes not applied are discarded.
The panel can also be opened from the floating monitor window while the
audio runs (if it is already open, it is brought to the front).
The panel stays visible while you work in the DAW (0.7.1): while the DAW or
one of the driver's windows is in front, the panel and its dialogs stay above
the DAW's windows (including Cubase's Studio Setup); when you switch to
another program it no longer stays on top. The panel is an independent window: it
does not block the DAW, and it stays open when the window that opened it
(e.g. Cubase's Studio Setup) is closed or minimized. It always has a taskbar
button. When the DAW restarts the driver after "Apply", the panel shows
"Restarting driver..." and then continues with the new driver, keeping
everything on screen; if the DAW does not load the driver again within 5 s it
says "The DAW released the driver. Settings were saved." (Apply then only
saves). If the screen is too small for the panel at the current scaling, the
panel is drawn slightly smaller so that "Apply" and "Close" are always
visible. Note: Cubase may stop the audio by itself while its own Studio
Setup dialog is open; that is Cubase behavior, not the driver's.
Keyboard: Tab moves between controls, Space toggles the focused option,
Ctrl+Tab / Ctrl+Shift+Tab switch to the next / previous tab.
The panel follows the Windows display scaling and adapts when it is moved to
a monitor with a different scaling.

Main tab
  Profile         Profile list; "Save..." and "Delete" (see Profiles).
  Floating monitor window   Shows or hides the monitor window (section 24).
  Tray icon       Shows or hides the notification-area icon (section 30).
  Output device   "(Windows default)" follows the device marked as default
                  in Windows.
  Input device    First entry "(None - no inputs)" = no inputs at all; then
                  "(Windows default)" and the devices. When Windows blocks
                  recording for desktop apps and an input is selected (here
                  or as an extra input; 0.9.6), a warning appears under it
                  with a "Set to None" button, which also clears the extra
                  inputs (or allow microphone access: section 18). With a Bluetooth output the same place shows
                  the recommended buffer with a "Use recommended" button, or
                  why the headset microphone was not opened (section 26).
  Mode            Shared | Exclusive (section 10).
  Buffer          32..4096 samples. Smaller = lower latency and more load.
                  128 or 256 is a good starting point.
  Live            Four tiles refreshed every 0.5 s:
                    Output latency and input latency (ms, what is reported
                    to the DAW), DSP load (current with a bar and maximum;
                    amber above 80 %, red above 100 %) and output glitches
                    (red if there are any). Badges in the card header:
                    BLUETOOTH, LIMITER, RATE: WINDOWS, PROGRAM PROFILE.
  Channel levels  Per-channel meters (section 6), inputs on the left and
                  outputs on the right with a single dB scale.
  Bottom bar      Summary: active mode, sample rate, channels; warns if
                  Windows privacy settings block the input.

Advanced tab
  Low latency in Shared     Minimum engine period (IAudioClient3).
  Follow device changes     Reopens by itself when devices are plugged in
                            or removed (section 14).
  Write log.txt             Diagnostic log.
  TPDF dither               Exclusive with 16/24-bit integer output.
  Protect A2DP              Never opens the microphone of the Bluetooth
                            headset used as output (section 26). On by
                            default.
  Drift correction          Auto | Always | Never (section 12).
  Resampler quality         Hermite (light) | Sinc (high quality), for
                            drift correction. Since 0.9.6 the internal
                            rate conversion (section 41) always uses sinc
                            when the CPU budget allows.
  Rate conversion           0.9.10: engine of the internal rate conversion
                            (section 41): Auto (Sinc, low latency; the
                            default, as in 0.9.9) | soxr (best quality,
                            +8-10 ms) | Sinc (low latency) | Hermite
                            (light; only when the rate goes up).
                            FixedRatioEngine key.
  Initial sample rate       Rate used when opening while no DAW rate is
                            known; the DAW changes it later and the driver
                            remembers the DAW's rate (0.9.5, section 29).
                            Changing it here replaces the remembered rate.
                            "Follow Windows (Shared)" = SampleRate=Auto
                            (section 29).
  Gain (dB)                 Extra output gain, -60 to +24 dB.
  Limiter, Ceiling, Gain cap  Hearing-safety limiter (section 27).
  Latency offsets (ms)      Output / Input, -50 to +500 ms, added to the
                            latency reported to the DAW (section 17).
  Loopback                  Off | Everything except this DAW | All audio of
                            the output device | One application only (with
                            the "Application" list and "Refresh";
                            section 11).
  Profile for this program  Profile applied automatically when this program
                            loads the driver; list of all programs with a
                            profile and "Remove" (section 28).

Extra devices & monitoring tab
  Direct monitoring (section 13): Enabled, input channels (pair), output
  channels (pair), Mute,
  Level (dB), Pan. When Windows blocks the microphone for desktop apps, the
  card says so (with the place to change it).
  Extra outputs and inputs (up to 3 each), "Align devices", "Output
  routing..." (sections 15 and 16) and a list with the status of each extra
  device (drift, counters, latency).

Status tab
  Full status text (mode, formats, latencies, counters, drift, load, extra
  devices) and the buttons "Copy status" (to the clipboard, for a report),
  "Open log", "Settings folder" and "Reset counters".

About tab
  Name, version, ASIO Compatible logo, trademark notice ("ASIO is a
  registered trademark of Steinberg Media Technologies GmbH.") and the
  GPLv3 license, and a text scroller strip (section 32).

Interface tab (0.9.0)
  Appearance      Theme (section 33) and Language (section 34); both are
                  previewed at once and saved with Apply.
  Visuals         Effect, Intensity, Speed, "In monitor window" (section 32).
  Updates         "Check for updates now", "Check automatically once a week"
                  (off by default), Download and Install (section 35).

Profiles
  A profile stores the ENGINE settings: mode, low latency, buffer, sample
  rate, gain, drift correction, resampler quality, dither and loopback
  mode. It does NOT change the selected devices.
  "Save..." asks for a name (1 to 40 characters, no [ ] = ;) and has the
  option "Include devices" (off for a new profile; when you overwrite an
  existing profile it starts with that profile's setting): when it is on,
  the profile also stores output, input, loopback application, extra
  devices, routing and alignment. Overwriting a profile that has devices
  with "Include devices" off asks for confirmation, because the devices
  would be removed from it.
  Selecting a profile loads its values into the panel; they are applied
  with "Apply". "Delete" removes the selected one.
  Factory profiles (engine only, created once):
    Studio     Exclusive, buffer 128, low latency
    Laptop     Shared, buffer 256
    Streaming  Shared, buffer 512, Loopback "Everything except this DAW"
  When upgrading from 0.4/0.5, the three old factory profiles are renamed
  to Studio, Laptop and Streaming and lose the empty device keys that used
  to switch back to the default devices, but only if they were not changed
  to point to devices. Your own profiles keep their
  names and contents.

----------------------------------------------------------------------
6. LEVEL METERS
----------------------------------------------------------------------
The "Channel levels" card (Main tab) shows one vertical bar per ASIO
channel using the full available height: inputs on the left (what the
driver delivers to the DAW, including Loopback L/R), outputs on the right
(what the DAW sends, before gain and Windows volume). Up to 32 inputs and
32 outputs: with many channels the bars get narrower and the channel
numbers are shown at intervals; below each group, its name and channel
count.
- A single dBFS scale on the left (0, -6, -12, -24, -48; floor -60), with
  guide lines; labels that do not fit are left out (they never overlap).
- Gradient color: green up to -18 dB, green to yellow up to -6 dB, yellow
  to red up to 0 dB.
- White line: peak held for 1.5 s.
- Red indicator above the bar: clip (a sample reached 0 dBFS or more). It
  stays on until you click the meters or press "Reset counters".
- Channels the DAW did not enable are drawn in dark gray, without level.
- On short screens the panel shrinks (minimum 700 logical pixels high; on small screens the whole panel is scaled down instead) and
  the meter card gets smaller.
- With the limiter on (section 27), "Limiter -x.x dB" at the top shows the
  gain reduction (amber while it is limiting).
- Updated every 50 ms, only while the panel is open. The audio thread only
  computes a maximum per block, with no locks and no memory allocation.
- A clip on an output means the DAW sends more than 0 dBFS: in Exclusive
  mode it is clipped (distortion); lower the DAW master.

----------------------------------------------------------------------
7. DSP LOAD AND COUNTERS
----------------------------------------------------------------------
"DSP load: now NN% / max NN%" in the status = time the DAW takes to
process each ASIO block (its bufferSwitch callback) divided by the block
duration (buffer / sample rate).
- now: moving average (about 0.3 s).
- max: the worst block since the last "Reset counters" or since start (the
  first 8 blocks are ignored, because DAWs often initialize during them).
- Close to 100% there will be dropouts: raise the buffer or reduce plugins.
  It is the load of the DAW's audio thread, not the total CPU load.
Output glitches in Exclusive mode (since 0.3.0): a glitch is counted when
the device position (IAudioClock) catches up with what the driver wrote (it
ran out of audio). If the card does not provide IAudioClock, it is counted
when the driver wakes up almost two periods after the previous wake-up.
As in Shared mode, the DAW is notified with kAsioOverload (Cubase lights its
overload indicator).
"Reset counters" clears: maximum load, output glitches, timeouts, input
underruns/drops/discontinuities and clip indicators. It does not reopen the
driver or interrupt the audio.

----------------------------------------------------------------------
8. DIAGNOSTICS TOOL
----------------------------------------------------------------------
WasapiDriverDiag.exe (Start menu > WASAPI Driver for ASIO > WASAPI Driver
for ASIO Diagnostics, or double-click it in the installation folder). A
console window; it changes nothing on the computer and uses no network. It
writes WasapiDriver-diagnostics.txt next to the program; if that folder is
not writable (Program Files without administrator rights), it saves it in
%APPDATA%\WASAPI Driver for ASIO (or in %TEMP%) and shows the path at the
end. Contents:
- Windows version, architecture (x64/ARM64), CPU and memory.
- Driver registration in the 64-bit and 32-bit views (ASIO key, CLSID, DLL
  path and whether the file exists) and the other installed ASIO drivers.
- Leftovers of the previous name (WasapiBridge).
- Each active output and input device: name, Id, ContainerId, mix format,
  engine periods (GetDevicePeriod and IAudioClient3), Exclusive formats
  accepted at 44.1/48/96 kHz in float32/int32/int24/int24-in-32/int16
  stereo, and hardware volume support.
- Default devices (console, multimedia, communications).
- MMCSS: service and "Pro Audio" task.
- config.ini and the last 200 lines of log.txt.
Privacy: user folders are printed as %APPDATA%, %LOCALAPPDATA%, %TEMP% or
%USERPROFILE%, the user name and computer name are masked, and endpoint Ids
and ContainerIds are shown as short hashes (WasapiDriverDiag.exe --full-ids
prints them in full, for local troubleshooting only). Review the text before
sending it: it still includes device names. See PRIVACY.txt.

----------------------------------------------------------------------
9. AUTOMATIC TEST
----------------------------------------------------------------------
WasapiDriverTest.exe (Start menu > WASAPI Driver for ASIO > "WASAPI Driver
for ASIO Automatic test"). It loads the driver the way a DAW does, with the
current panel settings, and runs (about 2 minutes; the outputs send
silence):
  T1  Information: name, version, channels, buffers, sample rates, channel
      names and types, latencies.
  T11 (0.9.8) Bad host calls: invalid arguments (null pointers, channels out
      of range or twice, NaN / negative sample rates, buffer sizes out of
      range, missing callbacks) and calls in the wrong order; each must
      return the expected ASIO error and the driver must keep working (no
      sound; runs right after T1, so every later test proves it).
  T2  10 s of audio with every channel: number of callbacks (+-5%; +-10%
      with buffers >= 512), intervals (mean/deviation/maximum; an interval
      > max(100 ms, 4 blocks) is a WARNING, more than 3 = FAILED),
      alternating 0/1 indices, samplePosition advancing by exactly one
      buffer, monotonic systemTime and getSamplePosition, kAsioOverload
      notifications.
  T3  20 start/stop cycles of 1 s (no reset requests).
  T4  Buffers 64, 128, 256, 512 and 1024 (2 s each).
  T5  44100, 48000 and 96000 Hz (3 s each); returns to 48000 at the end.
  T9  Extra devices (only when configured): 10 s with every channel.
  T6  10 full driver load/unload cycles.
  T7  Summary of driver messages (kAsioResetRequest must be 0).
Result per test: PASSED, FAILED or SKIPPED (not applicable on this computer,
e.g. a sample rate the driver rejects, or T8 without a physical input; does
not count as a failure). Final line: "RESULT: PASSED" (", N warnings" if
there were any) or "RESULT: FAILURES = N". Report in WasapiDriver-test.txt
(next to the program; if that folder is not writable, in
%APPDATA%\WASAPI Driver for ASIO or %TEMP%).
Close the DAW before running it.

"WASAPI Driver for ASIO Soak test (60 min)" (WasapiDriverTest.exe --soak 60)
runs a long playback instead of the tests; see section 38.

"WASAPI Driver for ASIO Latency test (cable)" (WasapiDriverTest.exe
--latency) adds T8: it needs a cable from output 1 to input 1 of the sound
card. TURN THE VOLUME DOWN: it plays a loud click every second. It measures
10 impulses (median, minimum, maximum in samples and ms) and compares them
with the reported latency (input + output); the difference is what would
have to be compensated in the DAW. If input 1 is Loopback or there is no
input, T8 is SKIPPED.

----------------------------------------------------------------------
10. SHARED AND EXCLUSIVE MODES
----------------------------------------------------------------------
                      Shared                          Exclusive
Windows audio         Keeps playing                   Muted on that device
Several DAWs at once  Yes                             No
Windows volume        Applied by Windows              Hardware if available;
                                                      otherwise by the driver
Sample rate           Any (the driver converts)       Only the device's rates
Output latency        ~3-20 ms + ASIO buffer          ~1-3 ms + ASIO buffer
Windows effects       Applied                         Bypassed (pure signal)
TPDF dither           No (Windows mixes in float)     If the card uses 16 or
                                                      24 bits
If it fails           -                               Falls back to Shared

Dither (0.4.0): in Exclusive mode, if the card only accepts 16-bit or
24-bit integers, the driver adds triangular noise (TPDF) of +-1 least
significant bit before rounding. This avoids quantization distortion in
quiet passages and reverb tails. Digital silence (exact 0) is kept without
noise. With float or 32 valid bits it is not applied.

Recommendation: start in Shared mode with "Low latency" enabled. Switch to
Exclusive only if you need the lowest latency for live playing.

Since 0.9.4 "Low latency" (LowLatencyShared=1) works at any DAW rate: the
Shared stream always runs at the Windows rate with the shortest engine
period the device offers (IAudioClient3), and when the DAW rate differs the
driver converts internally with an exact fixed ratio (section 41). Status
shows the engine line, e.g. "Engine: low latency (IAudioClient3), period
144 frames (3.00 ms) at 48000 Hz; internal rate conversion 44100->48000 Hz
(sinc)" (0.9.10: "(soxr HQ)" when Rate conversion = soxr; log.txt also
shows the converter's look-ahead).

----------------------------------------------------------------------
11. LOOPBACK INPUTS
----------------------------------------------------------------------
They add "Loopback L" and "Loopback R" to the DAW's input list.
- Everything except this DAW: captures the audio of every program except
  the DAW. It can be monitored without echo. Ideal for recording YouTube,
  Spotify or a call.
- All audio of the output device: includes the DAW's output; do NOT monitor
  that track (echo/feedback). With the output in Exclusive mode it is
  silent.
- One application only (0.4.0): captures only one program (and the
  processes that program starts), e.g. Spotify or the browser. It is chosen
  by executable name. The program must be running when the DAW opens the
  driver: otherwise Loopback L/R stay silent (log.txt says so). If the
  program is closed and opened again, its process id changes: the driver
  looks it up again on every open, so "Apply" (or restarting the
  DAW's audio engine) is enough. Multi-process programs (browsers, Discord)
  are handled: the main process (the root of the process tree, which
  includes its child processes) is chosen or, if there are several, the one
  with active audio. The DAW's own executable is rejected (use "Everything
  except this DAW" for that).
In Cubase: F4 > Inputs > Add Bus (Stereo) > Loopback L / Loopback R; an
audio track with that bus as input > record.

----------------------------------------------------------------------
12. DRIFT CORRECTION
----------------------------------------------------------------------
Two different devices (e.g. a USB microphone and HDMI speakers) have clocks
that differ by a few tens of ppm. Without correction, every now and then a
block is missing or left over (a click). The driver resamples the input
with a variable ratio that follows that difference (PI controller).
- Auto: on when input and output are not the same physical hardware, and
  always for Loopback "Everything except this DAW" and "One application
  only". Recommended.
- Always / Never: force it.
Resampler quality (0.4.0):
- Hermite: 4-point interpolation, very light. It slightly attenuates and
  distorts high frequencies while correcting (error -27 dB at 10 kHz,
  -15 dB at 15 kHz relative to the signal).
- Sinc: 32-tap polyphase filter, flat response up to ~20 kHz (error -85 to
  -90 dB up to 15 kHz). More CPU (several times Hermite; little in
  absolute terms with 2 channels) and 16 samples more input latency
  (included in the reported latency).
It only affects inputs that are being corrected; without correction there
is no resampling.
The panel shows the corrected drift in ppm. Typical values: 0 to +-100.
Maximum range: +-3000 ppm.

----------------------------------------------------------------------
13. DIRECT MONITORING
----------------------------------------------------------------------
The driver can mix an input straight into the main output, without going
through the DAW. Two ways to use it (both can be active; they add up):
- From the DAW: with "ASIO Direct Monitoring" enabled (Cubase: Studio Setup
  > VST Audio System), the DAW turns it on per track and controls level and
  pan (kAsioSetInputMonitor).
- From the panel (0.7.0): Extra devices & monitoring tab > Direct monitoring: Enabled, the
  input pair (or a mono input, which is panned), the output pair, Level
  (-60 to +6 dB), Pan (-100 left .. 0 .. +100 right) and Mute. Applied live.
How it works and what latency to expect (honestly):
- The input is mixed just before the conversion to the output format, after
  the alignment delay, on the output clock. It does not wait for the DAW,
  so it keeps working when the DAW is heavily loaded.
- It is NOT zero latency: you hear the input after the input device buffer
  plus the output device buffer (typically 5-20 ms in Shared mode, a few ms
  in Exclusive mode). It does not depend on the ASIO buffer size when input
  and output are the same device. When they are different devices (drift
  correction active), the monitored signal goes through the drift-corrected
  input block and gets about one ASIO buffer more.
- Level, pan and on/off changes are smoothed (5 ms ramps): no clicks. While
  monitoring, the output goes through a soft limiter just below full scale
  so program + monitor never wraps or clips harshly. The output meters show
  the mixed signal.
- Loopback inputs cannot be monitored (feedback risk: the loopback captures
  the output itself). Monitoring is switched off automatically when there is
  no device input. Only the main output is reached (not extra devices).
- Use headphones: monitoring a microphone through speakers in the same room
  can feed back.

----------------------------------------------------------------------
14. FOLLOWING DEVICE CHANGES
----------------------------------------------------------------------
With "Follow device changes" enabled:
- If the output/input is "(Windows default)" and the default device is
  changed in Windows, the driver reopens on the new one.
- If the device in use is unplugged, it reopens on the default device.
- If the device selected in the panel is plugged in again, it reopens on
  it.
The DAW performs the reopening (kAsioResetRequest): audio stops for 1-2 s.

----------------------------------------------------------------------
15. EXTRA DEVICES (BETA)
----------------------------------------------------------------------
Lets you use several sound devices at once as a single ASIO device (for
example USB speakers + Bluetooth headphones, or two USB microphones).
Panel > Extra devices & monitoring tab > "Extra devices (beta)": select up to 3 outputs and 3
inputs (a click selects/deselects; the main device is not listed) > Apply
and restart. They are stored in config.ini as ExtraOutputs=name1|name2 and
ExtraInputs=... (a substring of the name, like OutputDevice) and in profiles
saved with "Include devices".
ASIO channels:
  Outputs:  first those of the main output, then those of each extra device
            in order. Names carry the device number: "1:Out L", "1:Out R",
            "2:Out L", "2:Out R", "3:Out 1"...
  Inputs:   main input, then each extra input, and Loopback L/R at the end.
  Without extra devices the names do not change ("Out L", "In 1"...).
  At most 32 per direction: an extra device that does not fit is truncated
  or left out (the log says so).
Clock: the main output is the master clock (it sets the DAW's pace). Each
extra device has its own clock; the driver corrects the difference with the
same resampling as the inputs (Hermite or Sinc according to "Resampler
quality", PI controller):
- Extra inputs: drift correction like the main input (Auto = on unless
  they are the same physical hardware as the output).
- Extra outputs: always corrected. They are opened in Shared mode (in
  Exclusive only if Mode=Exclusive and the card accepts it; otherwise
  Shared). Different sample rates are handled by Windows in Shared mode.
The panel shows one line per extra device: drift in ppm and underflow /
overflow (outputs) or underruns / drops (inputs). Normal values: stable
drift from 0 to +-300 ppm, counters at 0 (there may be 1 at start). "Reset
counters" clears them too.
If an extra device is unplugged with "Follow device changes" enabled, the
DAW reopens the driver; if it is not connected when the driver opens, its
channels exist but are silent, and the driver reopens by itself when it is
plugged in.
Alignment ("Align devices", on by default; AlignDevices=1): the driver
estimates the latency of each device and delays the faster ones so that all
outputs play at the same time and all inputs record in sync. Cost:
everything gets the latency of the slowest device (the DAW receives that
latency and compensates the recording). The status shows each device's own
latency and the common one. When off, each device keeps its own latency and
the DAW receives the main device's latency. For live playing with the
lowest latency, turn alignment off and use the main device.
ASIO direct monitoring only reaches the channels of the main output (a
destination on an extra device is moved to channel 1) and is not delayed by
the alignment: it always has the lowest possible latency.

----------------------------------------------------------------------
16. OUTPUT ROUTING
----------------------------------------------------------------------
Extra devices & monitoring tab > "Output routing...": for each physical channel of the main
output, choose which ASIO output channel feeds it, or "Silence". "Identity"
restores the normal mapping (channel N <- ASIO channel N). It is applied
with "Apply" and stored in config.ini (OutputMap=asio:dev,...,
1-based; e.g. "1:1,2:2,1:3,2:4" duplicates L/R on channels 3 and 4) and in
profiles saved with "Include devices". Device channels without an
assignment are silent. It only affects the main output (extra devices
receive their own channels).

----------------------------------------------------------------------
17. LATENCY
----------------------------------------------------------------------
Reported output latency = ASIO buffer + frames kept queued in the WASAPI
buffer (the whole buffer, or two engine periods with "Low latency") +
Windows stream latency; with internal rate conversion (0.9.4) also the
converter's delay, everything converted to frames at the ASIO rate. Input
latency = ASIO buffer + input FIFO target (capture period + output
period); a converted input on the output's clock reports its fixed
delivery delay, so that the round trip equals the reported total. The DAW uses these numbers to align what is recorded.
Latency offsets (0.7.0, Advanced tab, -50 to +500 ms) are added to these
numbers. Use them when the real latency is larger than what Windows
reports, typically with Bluetooth headphones or speakers (Windows does not
report their delay) or some USB devices. Changing an offset is applied live:
the driver tells the DAW (kAsioLatenciesChanged) or, if the DAW does not
support that, asks it to restart the driver.
Measuring it: connect a cable from output 1 to input 1, run "WASAPI Driver
for ASIO Latency test (cable)" (WasapiDriverTest.exe --latency). At the end
it prints the difference between the measured round trip and the reported
latency and asks "Save ... as a latency offset? (y/n)". Answering y adds the
difference to OutputLatencyOffsetMs (the whole correction goes into the
output offset: a round trip cannot tell which side is late, and the DAW
only needs the sum to place recordings correctly). For Bluetooth output,
which cannot be measured with a cable, measure by ear or with the DAW's
own tools and enter the value by hand.

----------------------------------------------------------------------
18. TROUBLESHOOTING
----------------------------------------------------------------------
Symptom                                What to do
The driver does not appear in the DAW  Reinstall; check that the DAW is
                                       64-bit or 32-bit (both supported).
No sound                               Panel > Status: check "Output". In
                                       Windows the device must be enabled
                                       and not muted.
Exclusive does not engage (status      Enable "Allow applications to take
says Shared)                           exclusive control" (section 2); close
                                       other apps using that device
                                       exclusively.
Dropouts or crackles                   Raise the buffer (256 or 512). Check
                                       "Output glitches" and "DSP load" in
                                       the panel (max load near 100% = the
                                       DAW cannot keep up; raise the
                                       buffer). Try Exclusive. Use
                                       LatencyMon to find system drivers
                                       that block the CPU.
Periodic clicks in recordings          Drift correction on Auto or Always.
Loopback is silent                     Output in Exclusive with loopback
                                       "All audio": use "Everything except
                                       this DAW" or Shared.
Windows volume changes nothing         Normal in Exclusive with hardware
                                       volume: the card controls it.
The DAW hangs when changing something  Send log.txt and log-previous.txt.
Status: "Input blocked by Windows      Windows denies the microphone:
privacy settings"                      Settings > Privacy & security >
                                       Microphone > enable "Let desktop apps
                                       access your microphone". If you do
                                       not use inputs: Input device =
                                       (None - no inputs).
The panel went behind and is hidden    Click its taskbar button or use
                                       Alt+Tab.
Any problem you want to report         Run WASAPI Driver for ASIO
                                       Diagnostics and attach
                                       WasapiDriver-diagnostics.txt.
Red clip on an output                  The DAW sends > 0 dBFS: lower the
                                       master.

----------------------------------------------------------------------
19. SETTINGS AND LOG
----------------------------------------------------------------------
%APPDATA%\WASAPI Driver for ASIO\config.ini        settings (UTF-16LE)
%APPDATA%\WASAPI Driver for ASIO\log.txt           current session
%APPDATA%\WASAPI Driver for ASIO\log-previous.txt  previous session
config.ini keys: Mode, LowLatencyShared, OutputDevice, InputDevice,
Loopback (Off|ExcludeDaw|All|App), LoopbackApp (e.g. spotify.exe),
BufferSize, SampleRate, LastHostRate (0.9.5: written by the driver, the rate
the DAW last set), Gain (linear), FollowDeviceChanges, DriftCorrection
(Auto|On|Off), ResamplerQuality (Hermite|Sinc), FixedRatioEngine (0.9.10:
Auto|Soxr|Sinc|Hermite, section 41), Dither (0|1), MaxChannels
(1..32), Log, ActiveProfile, ExtraOutputs, ExtraInputs (names separated by
|, up to 3), OutputMap (asio:dev,...), AlignDevices (0|1),
OutputLatencyOffsetMs / InputLatencyOffsetMs (-50..500), DirectMonitor
(0|1), DirectMonitorInput / DirectMonitorOutput (first channel, 1-based),
DirectMonitorLevelDb (-60..6), DirectMonitorPan (-100..100),
DirectMonitorMute (0|1), MonitorWindow (0|1), MonitorTopmost (0|1),
MonitorX / MonitorY / MonitorW / MonitorH. Latency offsets and direct
monitoring are stored in profiles only with "Include devices"; the monitor
window keys never. Profiles:
sections [Profile:name] with the engine keys (and the device keys when saved
with "Include devices"). Profile sections written by 0.4.0 to 0.5.1 are
converted automatically.
%LOCALAPPDATA%\WASAPI Driver for ASIO\crash\     crash reports (section 25)
Diagnostic traces (.etl, 0.9.8) are written only when you record one, where
"Stop trace.bat" saves it (C:\Users\Public\Documents\WASAPI Driver for ASIO\Traces); see section 42 and TRACING.txt.

----------------------------------------------------------------------
20. KNOWN LIMITATIONS 0.9.10
----------------------------------------------------------------------
- Extra devices (beta): up to 3 extra outputs and 3 extra inputs, 32
  channels per direction in total. Alignment is based on estimated
  latencies (buffers and the stream latency Windows reports): the typical
  error is 1-2 ms; Bluetooth devices add delay that Windows does not report
  well. Without alignment, extra outputs play 15-40 ms after the main one.
  Direct monitoring only reaches the main output.
- An extra device missing at open time keeps 2 silent channels; if it has a
  different channel count when it appears, the DAW must reload the driver.
- Glitches in Exclusive mode: depends on the sound card driver reporting the
  position correctly (IAudioClock); without IAudioClock a less exact timing
  criterion is used.
- Windows on ARM64 (0.9.9): the Arm64X DLL that serves native ARM64 and
  emulated x64 DAWs with one registration is built and checked by the CI
  and confirmed by a user with native ARM64 Cubase (Snapdragon X Plus);
  emulated x64 DAWs not reported yet (KNOWN-ISSUES.txt KI-9; ARCHITECTURE.txt section 59). Packages without it (local builds, or
  a release whose Arm64X build failed) register one 64-bit architecture at
  a time, as before.
- Bluetooth: Windows does not report the real delay of Bluetooth devices;
  use the output latency offset (section 26).
- Rate conversion with soxr (0.9.10, only when "soxr" is selected) adds
  about 8-10 ms of latency per converted direction at 44.1-192 kHz (more at
  16-32 kHz) compared with the default sinc; it is reported to the DAW
  (section 41, KNOWN-ISSUES.txt KI-10).
- If the host calls init() on a thread without a message loop (rare),
  automatic resets are not delivered.
- No Authenticode signature unless a release was built with a certificate
  (SIGNING.txt); unsigned files make SmartScreen warn.
- Features from 0.2.x to 0.9.14 have not been fully tested on real hardware
  yet (see TEST-GUIDE.txt and COMPATIBILITY.txt). Beta features are listed
  in section 36.

----------------------------------------------------------------------
21. LICENSE AND TRADEMARKS
----------------------------------------------------------------------
ASIO is a registered trademark of Steinberg Media Technologies GmbH.

GPL-3.0-or-later. Uses the headers of the Steinberg ASIO SDK 2.3.4 under its
GPLv3 license option. "ASIO" is only used to indicate compatibility ("for
ASIO"), and the ASIO Compatible logo (exactly as provided by Steinberg) is
shown in the control panel, according to the Steinberg ASIO Usage
Guidelines. This project is not affiliated with or endorsed by Steinberg.

Legal documents (plain text, in the installation folder and with the source
code): LICENSE.txt (GNU GPL v3), COPYRIGHT.txt, THIRD-PARTY-NOTICES.txt,
TRADEMARKS.txt, DISCLAIMER.txt (no warranty, hearing safety), PRIVACY.txt
(no network, no telemetry, local files), SECURITY.txt, CONTRIBUTING.txt.
They are provided for information only and are not legal advice.

----------------------------------------------------------------------
22. FILES
----------------------------------------------------------------------
Release folder: see "RELEASE FOLDER LAYOUT" at the top of this file.
Installation folder (%ProgramFiles%\WASAPI Driver for ASIO):
WasapiDriver64.dll            x64 driver
WasapiDriverARM64.dll         ARM64 driver
WasapiDriverARM64X.dll        0.9.9, Windows on Arm only: Arm64X driver (native ARM64 and
                              emulated x64 DAWs; section 3)
WasapiDriver32.dll            x86 driver (32-bit DAWs)
WasapiDriverDiag.exe          Diagnostics tool (section 8)
WasapiDriverTest.exe          Automatic, latency and soak test (sections 9, 38)
README.txt                    This file
QUICK-START.txt               One-page start guide
FAQ.txt                       Frequent questions
COMPATIBILITY.txt             Hosts and devices tested, and how to report
TEST-GUIDE.txt                Test plan for real hardware
LICENSE.txt                   GNU GPL v3 (full text)
COPYRIGHT.txt                 Copyright notice and how to cite
THIRD-PARTY-NOTICES.txt       ASIO SDK license option and runtime libraries
TRADEMARKS.txt                ASIO, logo usage and other trademarks
DISCLAIMER.txt                No warranty; hearing and speaker safety
PRIVACY.txt                   No telemetry; optional update check; local files
SECURITY.txt                  Reporting vulnerabilities
CONTRIBUTING.txt              License of contributions, DCO, code rules
Tracing\                      0.9.8: WasapiDriver.wprp, Start trace.bat, Stop trace.bat,
                              TRACING.txt (section 42)
NOTICES.txt                   Notices page of the installer
Uninstall.exe                 Uninstaller
In the release folder only: VERSION.txt (build information and full
changelog), ROADMAP.txt (next steps), install.bat / uninstall.bat
(portable installation) and the source code with the technical documents:
  BUILDING.txt                Source index and build instructions
  ARCHITECTURE.txt            Complete internal design
  CODE-REVIEW.txt             Defects found and fixed in each review
  RELEASING.txt, SIGNING.txt  Release procedure and code-signing options
  installer/NOTICES.txt       Notices page of the installer

----------------------------------------------------------------------
23. HISTORY
----------------------------------------------------------------------
See VERSION.txt for the detailed changelog.
0.9.6  Release candidate: the microphone warning only appears when an input
       is selected; the internal rate conversion uses sinc whenever the CPU
       allows (also 44.1 -> 48 kHz); Setup and the uninstaller detect a
       driver still in use and ask to close the audio programs; minor
       code-review fixes (D1-D12).
0.9.7  Public beta: first version prepared for publication on GitHub
       (Releases page, issue forms, winget templates); documents and test
       data cleaned of personal hardware details; no change to the audio
       engine or the settings. The update check treats an unfilled
       repository owner as "not configured".
0.9.8  Hardening: invalid or malicious calls from a host and invalid data
       from devices or config.ini return errors instead of crashing the
       host; no unbounded waits; fuzz and validation tests gate every
       release; diagnostic tracing (ETW) with a Windows Performance
       Recorder profile. No new user features; valid calls and the sound
       are unchanged.
0.9.10 soxr: the internal rate conversion can use the SoX Resampler
       library (HQ) when "soxr" is selected (Rate conversion /
       FixedRatioEngine; the default Auto stays the low-latency sinc);
       MSVC warning C4244 fixed; CI actions on Node 24; getDriverVersion
       9100.
0.9.9  Arm64X: one driver DLL for native ARM64 and emulated x64 DAWs on
       Windows on Arm, built with MSVC in CI and registered once by the
       Setup (no question; beta, KNOWN-ISSUES.txt KI-9); two MSVC
       portability fixes; "Stop trace.bat" saves to the signed-in user's
       Desktop; code signing policy. No engine change.
0.9.5  Release candidate: the sample rate set by the DAW survives every
       restart caused by the driver (device and Bluetooth changes), a
       re-created driver instance and the next session.
0.9.4  Release candidate: the low-latency engine (IAudioClient3) at any
       DAW rate, with internal fixed-ratio conversion when the DAW rate
       differs from Windows (suggested by Microsoft's Windows music team).
0.9.3  Release candidate: own application icon (panel, dialogs, monitor
       window, tools, Setup, Start menu, Settings > Apps); the tray icon is
       the application icon with a status dot (red held 10 s after a
       dropout) and its tooltip ends with the state.
0.9.2  Release candidate: help tips for every control of the panel, its
       dialogs and the monitor window (hover or F1), also read by screen
       readers; Interface > "Show help tips".
0.9.1  Release candidate: 12 review fixes (updater hardening, driver
       lifetime and registry locking, SampleRate=Auto with a single reset,
       channel names in double-byte code pages, release pipeline), screen
       reader and keyboard access, Windows high contrast, ASIO Compatible
       logo in the installer, soak test, quick start, FAQ and
       compatibility table. Feature freeze until 1.0.
0.9.0  Themes (Dark, Retro Blue (16-bit), Retro 8-bit, Green Terminal,
       Neon, Light), four more background effects, an optional update
       check, and the interface in five languages.
0.8.3  Automatic test: ASIOTime checked per test with the rate of the
       running driver instance (fixes a false T7 count); the audio thread
       keeps its own copy of the sample rate.
0.8.2  Panel visuals about 3 times faster, smooth (~60 fps, time-based) with
       a Speed slider; frame rate and cost shown on the Status tab.
0.8.1  Optional animated panel background (plasma, starfield, copper bars,
       tunnel, audio-reactive mix) and an About-tab scroller.
0.8.0  Bluetooth mode (badge, recommended buffer, A2DP protection, latency
       offset hint), hearing-safety limiter, automatic profile per program,
       "Follow Windows" sample rate, tray icon, channel names from the
       devices; release, winget and code-signing preparation.
0.7.1  The panel no longer disappears behind the DAW (topmost while the DAW
       is in front, detaches from a hidden owner, taskbar button); it keeps
       working when the DAW restarts the driver (rebind); clearer device
       labels, "(None - no inputs)" first, privacy warning with "Set to
       None"; the bottom bar is never cut off.
0.7.0  Settings applied live without restarting the audio (reset only for
       devices, mode, buffer, rate and the like); floating monitor window;
       latency offsets with a measure-and-save option in the latency test;
       direct monitoring from the panel and improved ASIO direct monitoring
       (click-free ramps, drift-corrected path, soft limiter); crash
       reports (minidump) and automatic safe mode after a crash;
       microphone privacy verdict in the diagnostics and the panel.
0.6.1  config.ini comments regenerated in English (settings and profiles
       kept); user paths, user name, computer name and device Ids masked in
       reports, log and "Copy status"; legal documents (copyright,
       third-party notices, trademarks, disclaimer, privacy, security,
       contributing); no build paths embedded in the binaries.
0.6.0  New name "WASAPI Driver for ASIO" (formerly WasapiBridge) with
       automatic migration (registry, files, settings, profiles), ASIO
       Compatible logo and trademark notice according to the Steinberg ASIO
       Usage Guidelines. New dark tabbed panel with status tiles and
       full-height meters on a single scale. Profiles no longer change the
       devices (unless "Include devices"). Everything in English.
0.5.1  Real-use fix: the panel could end up behind Studio Setup (Cubase)
       when switching applications; its owner is now the window that opened
       it and it has a taskbar button. "Copy status" button. Clear message
       when Windows privacy settings block the microphone.
0.5.0  (beta) Extra devices: up to 3 extra outputs and 3 extra inputs with
       their own drift correction (the main output is the clock); latency
       alignment between devices (AlignDevices); output routing (OutputMap)
       with a dialog in the panel; dither on Exclusive integer extra
       outputs too; test T9.
0.4.0  Profiles (factory profiles, save and delete), single-application
       loopback, TPDF dither on 16/24-bit Exclusive integer outputs,
       optional sinc resampling for drift correction, automatic builds on
       GitHub Actions.
0.3.1  Automatic ASIO host test WasapiDriverTest.exe (T1-T7) and real
       round-trip latency measurement with a cable, with Start menu
       shortcuts. The driver itself did not change (version only).
0.3.0  Per-channel level meters (peak, 1.5 s hold, latched clip), DSP load
       meter (now/max), "Reset counters" button, output glitch detection in
       Exclusive mode (IAudioClock + late wake-up) with kAsioOverload,
       diagnostics tool with a Start menu shortcut.
0.2.2  Stability review: 34 defects fixed (see CODE-REVIEW.txt): resets and
       device events processed on the host thread, audio thread with its own
       references, process loopback without late writes, continuous
       timestamps, buffer-independent drift control, clipping in Exclusive
       mode, glitch detection without false positives, robust panel.
0.2.1  .exe installer (NSIS), x86 build, 32-bit registration.
0.2.0  Panel, loopback, multichannel, direct monitoring, drift correction,
       device following, ARM64.
0.1.0  First version: Shared/Exclusive, 2 inputs / 2 outputs.

----------------------------------------------------------------------
24. FLOATING MONITOR WINDOW
----------------------------------------------------------------------
Main tab > "Floating monitor window" > Apply (applied live). A small dark
window appears (without taking the focus from the DAW) as soon as the DAW
opens the driver, and shows:
- input and output meters with peak hold and clip (same as the panel);
- DSP load (now and maximum), output glitches and input dropouts (shown in
  amber when there are any), the reported input and output latencies in ms;
- one line with the mode, sample rate, buffer and devices.
Buttons: Settings (opens the control panel, also while the audio runs),
Reset counters, Pin (always on top) and Close (turns the option off). It can
be resized; position, size and Pin are remembered (MonitorX/Y/W/H,
MonitorTopmost) and a position that is no longer on any monitor is moved to
the top-right corner of the main screen. It has no taskbar button and does
not belong to the DAW window, so it never blocks the DAW. It follows the
Windows display scaling per monitor. It closes when the DAW closes the
driver.

----------------------------------------------------------------------
25. CRASH PROTECTION AND SAFE MODE
----------------------------------------------------------------------
If the driver itself crashes (a fault inside the driver DLL), it writes a
crash report before Windows or the DAW handles the crash:
%LOCALAPPDATA%\WASAPI Driver for ASIO\crash\crash-<date-time>.dmp (a
minidump) and crash-marker.txt. The driver never hides the crash; the DAW
still closes or recovers as it would without this feature. At most 5 dumps
are kept.
The next time the driver loads, it starts in safe mode for that session
only: Shared, Windows default output, no input, no extra devices, no
loopback, no direct monitoring, buffer 512. Your config.ini is not changed.
The panel and the monitor window show "Recovered from a crash in the
previous session - safe settings used." with a "Use my settings again"
button. Your settings come back when you press it, when you Apply in the
panel, or automatically at the next load after a session that started and
stopped cleanly in safe mode.
To report a crash: run the Diagnostics tool (its "Crash reports" section
lists the dumps) and attach the newest .dmp file. A minidump contains part
of the DAW's process memory; see PRIVACY.txt before sending it.


----------------------------------------------------------------------
26. BLUETOOTH HEADPHONES AND SPEAKERS
----------------------------------------------------------------------
The driver recognizes Bluetooth devices (classic Bluetooth and LE Audio).
With a Bluetooth output:
- the Live card and the monitor window show a BLUETOOTH badge;
- the Main tab recommends a buffer (512, or two device periods if that is
  more). "Use recommended" selects it and applies at once (the DAW restarts
  the driver);
- Windows does not report the real Bluetooth delay (often 100-250 ms), so
  the latency shown to the DAW is too small. Recorded tracks then land late.
  Fix: Advanced > Latency offsets > Output, about +150 ms, then fine-tune
  (or measure with the automatic test, section 9). The driver never sets
  this value by itself;
- Protect A2DP (Advanced, on by default): opening the microphone of the same
  headset makes Windows switch it from stereo music quality (A2DP) to
  telephone quality (hands-free) for every program. With this option the
  driver does not open that microphone (also as an extra input); the Main
  tab says so. Choose another input, or turn the option off if you really
  want the headset microphone.
Use Shared mode with Bluetooth; if a Bluetooth device refuses Exclusive
mode, the driver falls back to Shared by itself.

----------------------------------------------------------------------
27. HEARING-SAFETY LIMITER
----------------------------------------------------------------------
Advanced > Quality > "Limiter" (off by default, applied live). A peak
limiter at the very end of the output (all outputs, extra outputs too):
- Ceiling: -0.1 to -20 dBFS (default -1). No sample ever goes above it.
- Gain cap (optional): the Gain setting cannot go above this value (dB)
  while the limiter is on.
- It reacts instantly (no added latency, no lookahead); levels more than
  3 dB below the ceiling pass unchanged; louder peaks are softly bent
  towards the ceiling and the gain recovers smoothly (150 ms time constant).
  All channels of a device get the same gain (the stereo image is kept).
- The meters show "Limiter -x.x dB" (the gain reduction).
It protects against sudden loud peaks from the DAW; it is not a substitute
for a sensible volume (see DISCLAIMER.txt). In Shared mode Windows applies
its own volume after the driver.

----------------------------------------------------------------------
28. AUTOMATIC PROFILE PER PROGRAM
----------------------------------------------------------------------
Advanced > "Profile for this program": the profile chosen here is applied
every time this program (identified by its file name, e.g. the DAW's .exe)
loads the driver, before any device is opened. Press Apply to store it. The
"All programs" list shows every program with a profile; "Remove" deletes the
selected entry at once. Stored in config.ini, section [HostProfiles], as
program.exe=profile name (the program name is not case-sensitive).
While a program profile is in use the panel shows its settings; pressing
Apply then saves the settings shown as your normal settings, and the
profile is not applied again until the program loads the driver next time.
A profile that no longer exists is ignored (the log says so).

----------------------------------------------------------------------
29. FOLLOWING THE WINDOWS SAMPLE RATE
----------------------------------------------------------------------
Advanced > Initial sample rate > "Follow Windows (Shared)" (SampleRate=Auto
in config.ini). In Shared mode the driver starts at the rate set in Windows
(Sound settings > device > Format). If you change that rate while the DAW
runs, the driver asks the DAW to restart it (one kAsioResetRequest); until
the DAW does, the driver keeps reporting the rate of the stream that is
still running, and after the restart it reports the new Windows rate. If the DAW insists on its own project rate, the DAW wins and Windows
converts. With a fixed rate (any other choice) a change in Windows never
restarts anything: the driver keeps its rate and Windows converts.
0.9.4: with "Low latency" on, the driver converts instead of Windows (exact
fixed ratio, section 41), so a fixed DAW rate different from Windows keeps
the low-latency engine; "Follow Windows" is no longer needed for low
latency, only to avoid any conversion.
Exclusive mode always uses the DAW's rate.

Which rate the driver reports (0.9.5). The rate the DAW sets is kept when
the driver restarts itself (a device appears or disappears, Bluetooth
headphones switch, sleep and resume, a panel change that does not touch
the rate), when the DAW re-creates the driver after such a restart, and in
the next session (config.ini LastHostRate). Order: the DAW's rate in this
program > "Follow Windows" (the Windows rate) > the DAW's last rate
(LastHostRate) > the panel's Initial sample rate. Choosing a rate in the
panel replaces the remembered one. After a crash (safe mode) the panel's
rate is used until the DAW sets one. The log says where the rate came from
("Sample rate 44100 Hz (source: host)", "last host", "panel" or "auto").
Before 0.9.5 a restart caused by the driver could come back at the panel's
rate, and the DAW then reported "The ASIO sample rate has changed".

----------------------------------------------------------------------
30. NOTIFICATION-AREA (TRAY) ICON
----------------------------------------------------------------------
While a DAW uses the driver, the driver's icon appears in the notification
area (Main tab > "Tray icon", on by default; TrayIcon=0 turns it off). Since
0.9.3 it is the application icon (section 40) with a status dot in its
bottom-right corner (dark outline, readable on light and dark taskbars):
  green = running fine; amber = DSP load above 80 %; red = dropouts in the
  last 10 seconds (it stays red at least 10 s after the last one, so short
  stutters are seen); gray = the driver is loaded but no stream runs.
Tooltip: version, mode, sample rate, buffer, DSP load and dropouts, then the
state: OK, High load, Dropouts or Stopped.
Left click: show or hide the floating monitor window.
Right click: Open settings, Show monitor window, Reset counters, Copy status
(to the clipboard, privacy-filtered like the panel's), About.
The icon disappears when the DAW closes the driver. If Windows hides new
icons, drag it from the overflow area to the taskbar once.

----------------------------------------------------------------------
31. CHANNEL NAMES
----------------------------------------------------------------------
The DAW can show channel names built from the devices (DeviceChannelNames=1,
the default for a new installation): the short device name and the speaker position or a number,
for example "AX-22 In 1", "Kestrel E10 L", "Loopback L". Names are at most
31 characters and always unique. Characters that do not exist in the
Windows ANSI code page appear as "?".
Note: DAWs remember connections by channel name. So an update keeps the old
names: when an existing config.ini has no DeviceChannelNames key, the driver
writes DeviceChannelNames=0 at its first load ("In 1", "Out L", "2:Out L"
...). A new config.ini gets DeviceChannelNames=1. To switch, change the key in
config.ini and restart the DAW's audio engine (you may have to reassign the
inputs and outputs once).

----------------------------------------------------------------------
32. PANEL VISUALS (ANIMATED BACKGROUND)
----------------------------------------------------------------------
Interface tab > "Visuals" (0.9.0; the About tab in 0.8.x): Off, Plasma
(default), Starfield, Copper bars, Tunnel, Audio-reactive mix, and since
0.9.0 Metaballs, Fire, Vector (a rotating wireframe polyhedron) and Twister, with an Intensity slider (0-100, default 35) and a
Speed slider (25 % to 400 %, middle = 100 %, the default). The effect is
shown immediately as a preview; Apply saves it (Visuals, VisualsIntensity,
VisualsSpeed, VisualsMonitor in config.ini; applied live, never part of a
profile). It is drawn behind the panel: in the header, the margins and the
gaps between the cards; the cards themselves stay opaque, so all text keeps
its contrast. The About tab also shows a sine-wave text scroller over a
starfield. "Audio-reactive mix" moves faster and brighter with the level of
the meters (no effect on the audio). "Also in the monitor window" shows the
same effect very faintly behind the monitor window's meters ("In monitor
window").
The effects are drawn by the driver itself at low resolution (chunky
pixels) and are original: no images, music or fonts from other programs.
They are kept cheap because they run in the DAW's user-interface thread:
about 60 frames per second; if drawing the background takes more than 6 ms
per frame on average (measured over 2 s) it switches to about 30 frames per
second, and back when it is fast again. The movement is based on the real
time, so it never slows down when frames are skipped. The Status tab shows
"Visuals: <effect>, <fps> fps, <ms> ms/frame, throttled: yes/no". They stop
while the panel is minimized, show a still picture when Windows "Show animations" is
off (Settings > Accessibility > Visual effects), and are off in a Remote
Desktop session unless you chose an effect yourself. Choose Off for a plain
panel.

----------------------------------------------------------------------
33. THEMES
----------------------------------------------------------------------
Interface tab > Theme: Dark (default), Retro Blue (16-bit), Retro 8-bit,
Green Terminal, Neon, Light. A theme changes every color of the panel, its
dialogs, the monitor window, the meters and the background effects; the
retro themes also draw the panel title with the driver's own dot font. The
palettes are original (inspired by the look of 1980s and 1990s computers and
terminals; no logos, icons, fonts or graphics of any product). Every theme
keeps text contrast at 4.5:1 or more (checked by an automatic test). The
notification-area icon keeps its colors (green, amber, red, gray) in every
theme. Saved as Theme=Dark|RetroBlue|Retro8|Terminal|Neon|Light.

----------------------------------------------------------------------
34. INTERFACE LANGUAGES
----------------------------------------------------------------------
Interface tab > Language: Automatic (Windows) (default), English, Español,
Português (Brasil), Français, Deutsch. Automatic uses the language of the
Windows interface and English for any other language. It applies to the
panel, its dialogs, the monitor window, the tray icon and the status texts.
These stay in English on purpose, so that help and bug reports work for
everyone: "Copy status", log.txt, the Diagnostics tool, the Automatic test
and the documentation. "ASIO", "WASAPI", the product name and the trademark
sentence are never translated. The installer shows English or Spanish pages
following the Windows language. Saved as Language=Auto|en|es|pt|fr|de.

----------------------------------------------------------------------
35. CHECKING FOR UPDATES (OPTIONAL, OFF BY DEFAULT)
----------------------------------------------------------------------
The driver never contacts the network by itself. Interface tab > Updates:
- "Check for updates now": one HTTPS request to the GitHub Releases API of
  the project (https://api.github.com/repos/irregbex/
  WASAPI-Driver-for-ASIO/releases/latest). A build made from the prepared
  source tree before the project's GitHub account was filled in (an owner
  name containing the word "PLACEHOLDER") says "Update source not
  configured yet" and sends nothing.
- "Check automatically once a week" (off by default): the same request, at
  most once a week, only when you open the control panel.
- Only the program name and version are sent (User-Agent
  "WASAPI-Driver-for-ASIO/<version>"); no identifier, no settings, no
  usage data. Windows' normal HTTPS certificate checks apply.
- If a newer version exists, "Download" fetches its Setup and
  SHA256SUMS.txt into %LOCALAPPDATA%\WASAPI Driver for ASIO\updates. The
  Setup is kept only if its SHA-256 matches; if it carries a digital
  signature, the signature must be valid. Otherwise it is deleted.
- "Install (run Setup)" asks you to close the DAW and starts the normal
  Setup only when you confirm. Nothing is ever installed silently. The
  file is checked again (SHA-256 and signature) through a handle that no
  other program can write to until the Setup has started, so the file
  that runs is the file that was checked.
- Redirects must end on GitHub's own servers (api.github.com for the
  check; github.com, objects.githubusercontent.com or
  release-assets.githubusercontent.com for downloads); anything else is
  refused. A check gives up after 2 minutes and a download after 10
  minutes; closing the panel cancels a running check or download.
Downloaded files stay in that folder until you delete them.
The update check is beta until the project has a public release.

----------------------------------------------------------------------
36. STABLE AND BETA FEATURES
----------------------------------------------------------------------
0.9.1 to 0.9.6 are release candidates and 0.9.7 is the first public beta;
0.9.8 hardens it (robustness, tests, diagnostic tracing), 0.9.9 adds the
Arm64X DLL for Windows on Arm, 0.9.10 the soxr rate conversion, 0.9.11
to 0.9.14 the input-path fixes from the recording tests (beta):
no new features until 1.0, only fixes
(0.9.2 adds help tips and 0.9.3 the application icon, accepted as exceptions:
interface only, no audio change; 0.9.4 extends the low-latency engine to
every sample rate, see section 41).
Stable (tested on real hardware, see COMPATIBILITY.txt):
- one output device and one input device, Shared and Exclusive mode
  (recording with a USB interface, Steinberg UR22mkII, tested in both
  modes at 256 and 512: 0.9.12 / 0.9.13 Shared clean at 256 (KI-11
  fixed; at 512 Windows' own capture engine flags intermittent
  discontinuities with the interface's driver, see KI-11); Exclusive 256
  fixed in 0.9.14 by the capture gate on the output clock (0.9.13 gated
  on the capture stream's clock, which the tested driver reports wrongly),
  to be confirmed by TEST-GUIDE block AB; see KNOWN-ISSUES KI-11 / KI-13);
- buffer sizes, fixed sample rates, drift correction, resampler, gain,
  limiter, latency offsets, loopback, direct monitoring, profiles;
- control panel, monitor window, tray icon, themes, languages, visuals;
- crash protection and safe mode, diagnostics and automatic test.
Beta (works in the tests, little real-world use yet):
- extra devices, alignment and output routing (section 15);
- "Follow Windows" sample rate, SampleRate=Auto (section 29);
- the internal rate conversion of the low-latency engine (0.9.4, section
  41) until it has been tested on real hardware, and its optional soxr
  engine (0.9.10, FixedRatioEngine=Soxr);
- the Arm64X DLL for Windows on Arm (0.9.9: in the Setup, registered
  once for ARM64 and x64 DAWs; native ARM64 half confirmed by a user, KI-9);
- the update check, until a real public release exists (section 35).
The About tab of the panel shows the same summary.

----------------------------------------------------------------------
37. ACCESSIBILITY AND KEYBOARD
----------------------------------------------------------------------
Screen readers (Narrator, NVDA, JAWS): every control has a name; the
toggles are announced as check boxes with their state ("checked" / "not
checked"), the Intensity and Speed sliders as sliders with their value,
the buttons by their text and the lists and boxes by the label in front of
them. The status texts are read-only edit boxes reachable with Tab.
Keyboard:
- Tab / Shift+Tab: next / previous control (every control is reachable).
- Ctrl+Tab / Ctrl+Shift+Tab: next / previous tab.
- Alt+underlined letter: that tab, Apply or Close (the letters are chosen
  per language and shown when you press Alt).
- Space or Enter: press the focused button or switch the focused toggle.
- Arrow keys, Home, End, Page Up, Page Down: move the focused slider.
- F1 or Shift+F1: help tip of the focused control (section 39); Esc hides
  it.
- Esc: Close (unapplied changes are discarded).
The focused control always shows a ring in a contrasting color, in every
theme.
High contrast: when Windows high contrast is on (Settings > Accessibility
> Contrast themes), the panel and the monitor window use the Windows
colors only, with no background effects, gradients or dark styles; the
theme you chose returns when high contrast is turned off. Turning it on
or off while the panel is open takes effect at once.

----------------------------------------------------------------------
38. SOAK TEST (LONG-RUN STABILITY)
----------------------------------------------------------------------
Start menu > "WASAPI Driver for ASIO Soak test (60 min)", or
WasapiDriverTest.exe --soak <minutes> [--soak-cycle] (default 60 minutes).
Close the DAW first. It plays silence on every channel with the current
settings and writes one line per minute: callbacks (and the expected
number), interval mean, deviation and maximum, overloads, invalid ASIOTime
fields, private memory, handles and GDI/USER objects (marked when the
control panel is open). --soak-cycle also changes the buffer size every 5
minutes. While it runs you can unplug and plug the device again, change
the Windows default device, or put the computer to sleep and wake it: each
event is logged with the time the stream needed to recover (the test
reloads the driver like a DAW when the driver asks for a reset, or when no
callback arrives for 5 s). Ctrl+C stops early and still writes the
verdict.
RESULT: PASSED when the stream recovered after every event (within 120 s)
and memory, handles and GDI/USER objects show no growth trend (from minute
3: memory under 64 KB per minute and 8 MB in total, handles and objects
under 1 per minute and 50 in total). Report: WasapiDriver-soak.txt (next to
the program, else %APPDATA%\WASAPI Driver for ASIO or %TEMP%), rewritten
every minute, with user and computer names masked. Send it with your
COMPATIBILITY.txt line.

----------------------------------------------------------------------
39. HELP TIPS
----------------------------------------------------------------------
Rest the mouse on any control of the panel, its dialogs or the monitor
window for about half a second: a small tip explains what it does, often
with a recommendation (for example Buffer: "256 is a good start; Bluetooth:
512 or more"), and for settings whether it "Applies live" or the "DAW
restarts the driver". The status tiles, the meters and the monitor
window's text lines explain what the numbers mean and what is good or bad.
The tip disappears when the mouse leaves the control, on a click, a key or
the mouse wheel, and after 15 seconds.
Keyboard: F1 (or Shift+F1) shows the tip of the focused control, Esc hides
it; this works even when the tips are turned off. Screen readers read the
same text as the control's description.
Interface tab > "Show help tips" (on by default, applied live; Apply saves
it as HelpTips=0|1 in config.ini). The tips use the colors of the theme
(the Windows colors in high contrast) and the interface language.

----------------------------------------------------------------------
40. APPLICATION ICON
----------------------------------------------------------------------
Since 0.9.3 the driver has its own icon: a dark rounded tile with two
nodes (Windows audio on one side, your DAW on the other) joined by a bright
arch, a "bridge" in the teal and cyan of the Dark theme. It is original
artwork of the project (no logo of any other product), drawn by
scripts/make_icon.py; src/res/app.ico holds 16, 20, 24, 32, 40, 48, 64, 96,
128 and 256 pixel images (the small ones tuned by hand) and
src/res/icon-256.png is a large copy for web pages. It is shown by the
control panel and its dialogs (title bar, taskbar, Alt+Tab), the monitor
window, message boxes (taskbar and Alt+Tab), the About tab (under the ASIO
Compatible logo, which keeps its own space), WasapiDriverDiag.exe and
WasapiDriverTest.exe (file and console window), the Setup and the
uninstaller, the Start menu shortcuts and Settings > Apps. The tray icon is
the same icon with a status dot (section 30). It is licensed like the rest
of the project (GPL-3.0-or-later).

----------------------------------------------------------------------
41. LOW-LATENCY ENGINE AT ANY SAMPLE RATE
----------------------------------------------------------------------
Suggested by Microsoft's Windows music team (0.9.4). Windows' low-latency
Shared engine (IAudioClient3) only runs at the device's own rate (the
"Default Format" in Sound settings). Up to 0.9.3 the driver used it only
when the DAW rate was the same; a DAW at 44.1 kHz on Windows at 48 kHz got
the standard 10 ms engine plus Windows' converter, whose delay is not
reported.
Now, with "Low latency" on (Shared mode):
- the output always opens at the Windows rate with the shortest engine
  period the device offers (fallbacks: the default period, then the
  standard Shared stream at the Windows rate, then the 0.9.3 behavior);
- if the DAW rate differs, the driver converts with an exact fixed ratio
  (44.1 -> 48 kHz = 147/160): no drift, no control loop, the device is the
  clock. Quality: windowed sinc, about 110-130 dB signal-to-noise on a
  1 kHz tone, flat to 20 kHz, more than 100 dB alias rejection when the
  rate goes down. Since 0.9.6 sinc is used in both directions whenever the
  CPU allows, whatever ResamplerQuality says (that setting only affects
  drift correction; 0.9.4 and 0.9.5 used Hermite, about 89 dB, going up
  unless ResamplerQuality=Sinc; sinc adds about 1.45 ms of reported output
  latency at 44.1 kHz). If the conversions would cost too much CPU (many
  channels at high rates, over 150 million multiply-adds per second for
  all converted streams together since 0.9.6) the driver uses Hermite going up and lets Windows convert going
  down; the log says which;
- inputs open at their own Windows rate as well and are converted by the
  driver; the round trip equals the reported latencies;
- direct monitoring needs the input at the output device's rate (same
  device); otherwise the driver tells the DAW it is not available and the
  panel's direct-monitoring card says so;
- the DAW still gets ASIO callbacks of exactly its buffer size; they are
  produced when the device needs data, so they are spread evenly over
  time with a jitter of up to one engine period.
Expected output latency, DAW at 44.1 kHz, ASIO buffer 128, Windows at
48 kHz: with a 144-frame engine period (3 ms) about 12 ms; with a device
that only offers 480 frames (10 ms) about 29 ms, the same as before but
without Windows' unreported converter delay. (With the default sinc;
soxr adds about 8-10 ms, see below.)
0.9.10: the conversion engine is selectable (Advanced tab > Rate conversion,
FixedRatioEngine=Auto|Soxr|Sinc|Hermite). The default, "Auto (Sinc, low
latency)", is exactly the 0.9.9 behaviour (sinc whenever the CPU budget
allows); "Sinc (low latency)" is the same. "soxr (best quality, +8-10 ms)"
uses the SoX Resampler library (recipe HQ: 20-bit, linear phase; Steinberg's
built-in ASIO driver installs a soxr.dll as well) when the CPU budget allows: about
132 dB signal-to-noise on a 1 kHz tone (sinc about 115 dB), distortion and
aliasing 10-35 dB lower than sinc, flat to 20 kHz (0.004 dB), at about half
of sinc's CPU. It works on FFT blocks, so it needs a longer look-ahead: about
9.7 ms instead of 1.45 ms at 44.1 -> 48 kHz, i.e. about 8 ms more output
latency (at buffer 128 with a 3 ms engine period: 20 ms instead of 12 ms) and
the same again on a converted input; about 8-10 ms of look-ahead for every
pair between 44.1 and 192 kHz, more at 16-32 kHz (48 -> 16 kHz 15.6 ms,
16 -> 44.1 kHz 26.8 ms, 44.1 -> 22.05 kHz 19.2 ms). The DAW is told the exact latency, so
recordings stay in time. If soxr would exceed the CPU budget, the driver
uses the default rule instead and the log says so. "Hermite (light)" is the
cheapest and is only used when the rate goes up. The log and the Status tab
name the engine ("soxr HQ", "sinc", "Hermite"). KNOWN-ISSUES.txt KI-10,
ARCHITECTURE.txt section 60.
Check the Status tab (engine
line) and WasapiDriverDiag.exe ("Low-latency period below the default"):
many built-in and USB devices offer 128-144 frames with the standard
Windows driver; some vendor drivers only 480.
Not changed: Exclusive mode, Loopback (Windows converts), extra outputs
(standard Shared stream, drift correction).

----------------------------------------------------------------------
42. DIAGNOSTIC TRACING (ETW)
----------------------------------------------------------------------
0.9.8. When a problem is hard to describe (dropouts, resets, a driver that
stops responding), a trace shows exactly what the driver did and when. The
driver writes events to Windows event tracing (provider
"WASAPIDriverForASIO"); nothing is recorded unless you start a trace
yourself, and an idle provider costs nothing measurable.
  1. Start menu > WASAPI Driver for ASIO > "... Start trace" (or Tools\Start
     trace.bat in the release folder). It asks for administrator rights.
  2. Reproduce the problem.
  3. "... Stop trace": WasapiDriver-<date>-<time>.etl is saved on your
     Desktop. Zip it and attach it to your GitHub issue.
The trace contains the driver's settings, device names (user and computer
names masked), formats, latencies, error codes and counters, plus what the
Windows audio service reports; never audio. Details, the manual wpr.exe
commands and the privacy notes: TRACING.txt.
