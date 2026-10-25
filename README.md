<p align="center">
  <img src="assets/banner.png" alt="WASAPI Driver for ASIO - free open-source ASIO driver for Windows 10/11" width="100%">
</p>

<p align="center">
  <a href="https://github.com/irregbex/WASAPI-Driver-for-ASIO/releases"><img alt="Latest release" src="https://img.shields.io/github/v/release/irregbex/WASAPI-Driver-for-ASIO?include_prereleases&label=release&color=3b82f6"></a>
  <a href="https://github.com/irregbex/WASAPI-Driver-for-ASIO/actions/workflows/build.yml"><img alt="Build" src="https://github.com/irregbex/WASAPI-Driver-for-ASIO/actions/workflows/build.yml/badge.svg"></a>
  <a href="LICENSE.txt"><img alt="License: GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0-16a34a"></a>
  <img alt="Windows 10/11" src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4">
  <img alt="x64 | x86 | Arm64X" src="https://img.shields.io/badge/arch-x64%20%7C%20x86%20%7C%20Arm64X-6b7280">
  <a href="https://github.com/irregbex/WASAPI-Driver-for-ASIO/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/irregbex/WASAPI-Driver-for-ASIO/total?color=8b5cf6"></a>
</p>

<p align="center">
  <img src="src/res/icon-256.png" alt="" width="72"><br>
  <b>Use Cubase, Ableton Live, DaVinci Resolve and other ASIO hosts with the sound devices Windows already has.</b><br>
  Built-in sound, USB headsets, HDMI monitors, Bluetooth headphones - no audio interface needed.
</p>

<p align="center">
  <a href="https://github.com/irregbex/WASAPI-Driver-for-ASIO/releases"><b>Download</b></a> ·
  <a href="QUICK-START.txt">Quick start</a> ·
  <a href="FAQ.txt">FAQ</a> ·
  <a href="README.txt">Full manual</a> ·
  <a href="https://github.com/irregbex/WASAPI-Driver-for-ASIO/issues">Report a problem</a>
</p>

---

> [!NOTE]
> **Public beta (0.9.x).** The installer is not code-signed yet (signing through SignPath Foundation is in progress),
> so Windows SmartScreen may warn: choose **More info > Run anyway**. Download only from the
> [Releases page](https://github.com/irregbex/WASAPI-Driver-for-ASIO/releases) and compare the SHA-256 with `SHA256SUMS.txt`.

## Why this driver

Most DAWs talk to audio hardware through **ASIO**. Without an audio interface, Windows users usually rely on a generic
driver. Some of them take the sound device **exclusively**: while the DAW is open, YouTube, calls, system sounds and
screen recordings go silent.

**WASAPI Driver for ASIO** runs in **Shared mode by default**: your DAW and the rest of Windows play through the same
headphones or speakers at the same time. When you need the lowest latency, switch to **Exclusive** mode in one click.

It can also **follow the Windows default output**: switch from speakers to headphones in Windows and the DAW follows.
Put your Bluetooth headphones in their case and the sound moves to the next device; take them out and it comes back by
itself - no need to open any settings again.

## Features

| Feature | Details |
|---|---|
| **Modern Windows audio** | WASAPI with the low-latency `IAudioClient3` engine at the device's native rate (typically 10 ms periods) |
| **Shared or Exclusive** | Shared by default (everything keeps playing); Exclusive for minimum latency |
| **Any project rate** | Internal conversion when the project rate differs from the device (e.g. 44.1 kHz project on a 48 kHz device): low-latency 128-tap sinc by default, or **soxr** (SoX Resampler library, HQ) for the best quality (adds ~8-10 ms at 44.1-192 kHz, more at 16-32 kHz) |
| **Bluetooth aware** | Detects Bluetooth outputs, recommends a buffer, latency offsets; keeps playing when headphones go into their case and come back |
| **Several devices** | Main device plus up to 3 extra inputs/outputs with clock-drift correction, output routing and loopback of a single application |
| **Monitoring and safety** | Direct monitoring, hearing-safety limiter, dither, live level meters and DSP load |
| **Control panel** | Profiles (also per program), floating monitor window, tray icon, 6 themes, 5 languages (EN/ES/PT/FR/DE), keyboard and screen-reader support |
| **Windows on Arm** | One **Arm64X** DLL serves native ARM64 hosts and emulated x64 hosts |
| **Troubleshooting** | Log file, Diagnostics tool, automatic ASIO host test, TraceLogging provider with a ready-made `.wprp` profile |
| **Privacy** | No telemetry. The optional update check is off by default ([PRIVACY.txt](PRIVACY.txt)) |

## Compared with Steinberg's generic drivers

Cubase, Nuendo and other Steinberg products install a generic driver for computers without an audio interface:
the current **Steinberg built-in ASIO Driver** or, on older installations, the **Generic Low Latency ASIO Driver**,
which Steinberg no longer develops.

**Steinberg built-in ASIO Driver - release history** (Steinberg release notes):

| Version | Date | Change |
|---|---|---|
| 1.0.7 | July 2023 | First release (with WaveLab Cast 2) |
| 1.0.8 | 24 October 2024 | Windows 11 on Arm (preview) |
| **1.0.9** | **18 December 2024** | Unified installer. **Latest version** as of October 2026 (file version 1.0.9.26) |

> [!IMPORTANT]
> **Same driver, two names.** The built-in driver's control panel still shows the **old name** in some languages,
> so it is easy to mistake it for the discontinued Generic driver. Check the version in the title: **1.0.9.x = built-in**.
>
> | Language | Title of the built-in driver's panel (1.0.9.26) | Name |
> |---|---|---|
> | English | Steinberg built-in ASIO Driver - Version 1.0.9.26 | correct |
> | German | Steinberg eingebauter ASIO-Treiber - Version 1.0.9.26 | correct |
> | Spanish | Controlador Genérico ASIO de Baja Latencia - Version 1.0.9.26 | **old name** |
> | French | Pilote ASIO générique à faible Latence - Version 1.0.9.26 | **old name** |
> | Italian | Driver ASIO generico a bassa latenza - versione 1.0.9.26 | **old name** |
> | Japanese | 共通低レイテンシー ASIO ドライバ：Vers. 1.0.9.26 | **old name** |

| Feature | Generic Low Latency ASIO Driver (discontinued) | Steinberg built-in ASIO Driver 1.0.9 (18 Dec 2024) | **WASAPI Driver for ASIO** |
|---|---|---|---|
| Other apps while the DAW is open | Exclusive in our tests: other apps went silent | Shared in our tests: other apps kept playing | **Shared by default**, Exclusive optional |
| Sample-rate conversion | - | Automatic (soxr library) | Automatic (built-in 128-tap sinc, low latency; **soxr** library HQ selectable) |
| Follows the Windows default output | - | No: a device must be picked in its port list (Bluetooth only while already connected) | **Yes**: "(Windows default)" follows Windows, e.g. headphones on/off |
| Bluetooth | No special handling | Works when picked directly; after the headphones disconnect the selection is lost and must be set again in the panel | Detection, buffer advice, latency offsets; output moves to the next device and comes back by itself on reconnect |
| Several devices | Port list | Port list | Up to 3 extra devices with drift correction, routing, app loopback |
| Windows on Arm | - | Windows 11 on Arm (preview, per Steinberg) | ARM64 and **Arm64X** (one DLL for ARM64 and emulated x64 hosts) |
| 32-bit hosts | - | Not available | x86 DLL included |
| Diagnostics | - | - | Log, diagnostics tool, host test, TraceLogging |
| Source code | Closed | Closed | **Open (GPL-3.0)** |

Steinberg's drivers have years of use on many machines; this driver is a public beta. The built-in driver column is based on
version 1.0.9.26 (x64), Steinberg's published information and our own tests; "-" means we found no such feature.
The Generic Low Latency ASIO Driver and the Steinberg built-in ASIO Driver are products of Steinberg Media Technologies GmbH.
WASAPI Driver for ASIO is an independent open-source project, not affiliated with or endorsed by Steinberg.

## Install

1. Close your DAW.
2. Download **`WASAPI-Driver-for-ASIO-<version>-Setup.exe`** from [Releases](https://github.com/irregbex/WASAPI-Driver-for-ASIO/releases) and run it.
3. In your DAW, choose **WASAPI Driver for ASIO** as the ASIO driver
   (Cubase: *Studio > Studio Setup > Audio System*).
4. Open the driver's **Control Panel** to pick the output device, mode and buffer.

No reboot needed. A portable `bin.zip` with `install.bat` is also available. Full steps: [QUICK-START.txt](QUICK-START.txt).

## Tested with

| Host | Status |
|---|---|
| Cubase 15 (x64) - built-in, USB, HDMI and Bluetooth outputs | Tested: hours of use, Bluetooth on/off, sleep/resume |
| Cubase 15 (x64) - Steinberg UR22mkII (USB interface, inputs) | Tested: recording, Shared and Exclusive, 256 and 512 (0.9.11 fixes KI-11 / KI-13 found there) |
| DaVinci Resolve Studio | Tested (0.9.4). Current Resolve versions also offer I/O Engine = System Audio (no ASIO needed); this driver is optional there |
| Ableton Live, REAPER, FL Studio, Studio One, Bitwig | Not tested yet - reports welcome |
| Cubase for Arm on a Snapdragon X Plus laptop (Arm64X, native ARM64 half) | Tested by a user: Shared mode, output, 12-18 ms with buffers 32-256 |
| x64 DAWs emulated on Windows on Arm (ARM64EC half) | Not tested yet - testers welcome |

See [COMPATIBILITY.txt](COMPATIBILITY.txt) and [KNOWN-ISSUES.txt](KNOWN-ISSUES.txt).

## Quality

- **Exception barriers** on every entry point: an internal error cannot crash your DAW.
- **Validation** of every call and argument from the host; bounded waits on device I/O.
- **18 offline test suites**, including fuzz tests under AddressSanitizer / UndefinedBehaviorSanitizer; a release is only built when all of them pass.
- Built in public by [GitHub Actions](https://github.com/irregbex/WASAPI-Driver-for-ASIO/actions) from this repository.
- Every review finding is documented in [CODE-REVIEW.txt](CODE-REVIEW.txt).

## Reporting a problem

Open an [issue](https://github.com/irregbex/WASAPI-Driver-for-ASIO/issues) and attach the **Diagnostics** report and, if possible,
a trace made with `Start trace.bat` / `Stop trace.bat` ([TRACING.txt](TRACING.txt)). Security problems: report them privately
(*Security > Report a vulnerability*, see [SECURITY.txt](SECURITY.txt)).

## Documentation

| Document | Contents |
|---|---|
| [QUICK-START.txt](QUICK-START.txt) | One page: install, select the driver, recommended settings |
| [README.txt](README.txt) | Complete manual |
| [FAQ.txt](FAQ.txt) | Common questions |
| [VERSION.txt](VERSION.txt) | Changelog |
| [BUILDING.txt](BUILDING.txt) / [ARCHITECTURE.txt](ARCHITECTURE.txt) | Building from source and how it works |
| [CODE-SIGNING-POLICY.txt](CODE-SIGNING-POLICY.txt) | Code signing policy and team roles |
| [CONTRIBUTING.txt](CONTRIBUTING.txt) | How to contribute |

## License

GPL-3.0-or-later - see [LICENSE.txt](LICENSE.txt), [COPYRIGHT.txt](COPYRIGHT.txt) and [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

ASIO is a registered trademark of Steinberg Media Technologies GmbH ([TRADEMARKS.txt](TRADEMARKS.txt)).
Provided without any warranty ([DISCLAIMER.txt](DISCLAIMER.txt)); an audio driver can produce very loud sound - start at a low volume.
