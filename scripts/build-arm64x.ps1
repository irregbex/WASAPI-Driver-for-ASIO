# WASAPI Driver for ASIO - builds the Arm64X driver DLL with MSVC (GitHub Actions, windows-11-arm runner).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Arm64X (recommended by Microsoft for in-process audio components such as ASIO drivers on Windows on Arm): ONE DLL
# that holds native ARM64 code and ARM64EC code. A native ARM64 host (ARM64 Cubase) runs the ARM64 half; an x64 host
# running under emulation (Prism) runs the ARM64EC half, at native speed. The CLSID points to a single file, so no
# file-system redirection is involved. See:
# devblogs.microsoft.com/windows-music-dev/which-compilation-target-should-i-pick-for-my-arm64-music-apps-asio-components-and-plugins/
#
# Requires the MSVC environment for arm64 (ilammy/msvc-dev-cmd with arch: arm64): cl compiles native ARM64 by
# default and ARM64EC with /arm64EC; link /machine:arm64x merges both halves. The .def file is given twice
# (/defArm64Native for the ARM64 half, /def for the ARM64EC half) so both halves export the same four COM entry
# points.
#
# Every source file is compiled on its own and the script keeps going after an error, so ONE run reports every
# compiler error. 0.9.9: the messages are classified before they are published:
#   errors    "error Cxxxx" / "fatal error Cxxxx" (compiler), LNK1xxx / LNK2xxx (linker), RCxxxx errors, Dxxxx
#             command-line errors and the script's own "error:" lines -> ::error annotations (first 10)
#   warnings  "warning Cxxxx" (compiler), LNK4xxx (linker), RCxxxx / Dxxxx warnings -> ::warning annotations
#             (first 10). A warning never fails the build (before 0.9.9 every LNKnnnn line, warnings included,
#             was published as an error).
# Annotations carry the file and line when the message has them. Every classified line also goes to the job
# summary (up to 200 lines each). At the very end ONE ::notice annotation is ALWAYS written (also after a
# failure) with the result, the dumpbin machine line(s), the exported names, the imported DLLs and the SHA-256
# of the DLL, because the job summary is not visible on the public run page without signing in, while the
# annotations are.
#
# Expected linker warning, benign (documented in CODE-REVIEW.txt "0.9.9 MSVC / Arm64X"):
#   LNK4279 'SetFilePointerEx' mapped to different icall push thunks (libucrt.lib lseek.obj / _flsbuf.obj and
#   update.obj). An icall push thunk is the adapter that ARM64EC code uses to call through an import whose target
#   might be x64 code; the linker creates one per function signature. The prebuilt static UCRT objects (/MT) and
#   our update.obj each requested a thunk for the same import, and the linker reports that the two requests did
#   not resolve to the same thunk. SetFilePointerEx is a kernel32 export, which is native (ARM64EC-compatible)
#   code on Windows on Arm, so the import is called directly and neither thunk is used; and both thunks marshal
#   the same signature (HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD) anyway. The warning is still published
#   as a ::warning (never hidden), with a pointer to this note.
#
# 0.9.9: LNK4070 fixed. src/WasapiBridge.def says "LIBRARY WasapiDriver" (the mingw / zig builds name their output
# with -o, so the LIBRARY line is only the internal name there), while this build writes WasapiDriverARM64X.dll;
# link warned that the /OUT directive in the .EXP differs from the output name. The script now writes a temporary
# copy, obj\WasapiDriverARM64X.def, identical except for "LIBRARY WasapiDriverARM64X", and passes it to both
# /defArm64Native and /def.
#
# 0.9.10: the SoX Resampler library (third_party/soxr, LGPL-2.1-or-later, see its README-vendored.txt) is compiled
# as C (cl /TC) for both halves, like the C++ sources: obj\a64\soxr_<name>.obj (native ARM64) and
# obj\ec\soxr_<name>.obj (/arm64EC). Third-party code: /W0 for these files ONLY (the C++ sources keep /W3 and are
# expected to build without warnings). The configuration is third_party\soxr\soxr-config.h (plain C engine "cr32",
# no SIMD, no OpenMP); third_party\soxr\data-io-portable.c replaces src\data-io.c because soxr's rint.h would pick
# SSE2 intrinsics for ARM64EC (which defines _M_X64); third_party\soxr\fifo-headroom.c is the FIFO allocator that
# soxr-config.h names. /D_CRT_SECURE_NO_WARNINGS and /D_USE_MATH_DEFINES only
# silence CRT deprecation notes and provide M_PI where soxr expects it (math-wrap.h defines it anyway).
#
# Checks after the link (any failure makes the script fail):
#   dumpbin /headers    the machine line must say ARM64X
#   dumpbin /exports    exactly DllCanUnloadNow, DllGetClassObject, DllRegisterServer, DllUnregisterServer
#   dumpbin /dependents no Visual C++ runtime DLL (vcruntime*, msvcp*, msvcr*, concrt*, ucrtbase*): the build
#                       uses the static runtime (/MT), so a host never needs a Visual C++ redistributable. Other
#                       DLLs outside the list of expected Windows system DLLs are reported as warnings.
# Output: WasapiDriverARM64X.dll in the repository root. Exit code 0 only if the DLL was built and every check
# passed. When run in GitHub Actions the step outputs are also written: ok=true and sha256=<hex> (only on
# success); the packaging jobs use them to decide whether the Setup and the bin zip contain the Arm64X DLL.

$ErrorActionPreference = 'Continue'
$root = (Get-Location).Path
New-Item -ItemType Directory -Force -Path obj\a64, obj\ec | Out-Null
$log = New-Object System.Collections.Generic.List[string]
$failed = $false
$stepFails = New-Object System.Collections.Generic.List[string]   # failed tool runs (exit code), see below
$dll = 'WasapiDriverARM64X.dll'

function Invoke-Step([string]$exe, [string[]]$argv) {
    $out = & $exe @argv 2>&1 | ForEach-Object { "$_" }
    foreach ($l in $out) { $log.Add($l); Write-Host $l }
    if ($LASTEXITCODE -ne 0) {
        $script:failed = $true
        $script:stepFails.Add("error: $exe$(if ($exe -ne 'link') { ' ' + $argv[-1] }) exited with code $LASTEXITCODE")
        return $false
    }
    return $true
}

# GitHub workflow command data: %, CR and LF must be escaped; properties (file, title) also escape ':' and ','.
function Esc-Data([string]$s) { return $s.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A') }
function Esc-Prop([string]$s) { return (Esc-Data $s).Replace(':', '%3A').Replace(',', '%2C') }

$cf = @('/nologo', '/c', '/O2', '/EHsc', '/std:c++17', '/W3', '/MT', '/utf-8', '/permissive-', '/Zc:__cplusplus',
        '/DUNICODE', '/D_UNICODE', '/D_WIN32_WINNT=0x0A00', '/D_CRT_SECURE_NO_WARNINGS', '/DNOMINMAX',
        '/Ithird_party\asiosdk', '/Ithird_party\soxr\src', '/Isrc')
$src = @('dll', 'driver', 'config', 'panel', 'crash', 'update', 'trace')
# 0.9.10: soxr C sources (path without .c) and their compiler flags (see the header)
$cfC = @('/nologo', '/c', '/O2', '/TC', '/W0', '/MT', '/utf-8', '/DSOXR_LIB', '/DNDEBUG', '/D_CRT_SECURE_NO_WARNINGS',
         '/D_USE_MATH_DEFINES', '/Ithird_party\soxr')
$soxr = @('third_party\soxr\src\soxr', 'third_party\soxr\data-io-portable', 'third_party\soxr\fifo-headroom', 'third_party\soxr\src\dbesi0',
          'third_party\soxr\src\filter', 'third_party\soxr\src\fft4g32', 'third_party\soxr\src\fft4g64',
          'third_party\soxr\src\cr', 'third_party\soxr\src\cr32')

Invoke-Step 'rc' @('/nologo', '/i', 'src', '/fo', 'obj\res.res', 'src\WasapiDriver.rc') | Out-Null
foreach ($s in $src) {
    Invoke-Step 'cl' ($cf + @("/Foobj\a64\$s.obj", "src\$s.cpp")) | Out-Null
    Invoke-Step 'cl' ($cf + @('/arm64EC', "/Foobj\ec\$s.obj", "src\$s.cpp")) | Out-Null
}
foreach ($c in $soxr) {
    $n = 'soxr_' + [System.IO.Path]::GetFileName($c)
    Invoke-Step 'cl' ($cfC + @("/Foobj\a64\$n.obj", "$c.c")) | Out-Null
    Invoke-Step 'cl' ($cfC + @('/arm64EC', "/Foobj\ec\$n.obj", "$c.c")) | Out-Null
}

# Temporary .def with the real output name (LNK4070, see above). Only the LIBRARY line changes; the EXPORTS
# section is copied as it is, so the exports can never differ from the other builds.
$def = 'obj\WasapiDriverARM64X.def'
$defText = @(Get-Content -LiteralPath 'src\WasapiBridge.def' | ForEach-Object {
    if ($_ -match '^\s*LIBRARY\s') { 'LIBRARY WasapiDriverARM64X' } else { $_ } })
if (-not ($defText -match '^LIBRARY WasapiDriverARM64X$')) { $failed = $true; $log.Add('error: src\WasapiBridge.def has no LIBRARY line') }
Set-Content -LiteralPath $def -Value $defText -Encoding ascii

$machine = @(); $exports = @(); $imports = @(); $sha = ''
$expected = @('DllCanUnloadNow', 'DllGetClassObject', 'DllRegisterServer', 'DllUnregisterServer')
if (-not $failed) {
    $libs = @('ole32.lib', 'uuid.lib', 'avrt.lib', 'winmm.lib', 'shell32.lib', 'user32.lib', 'advapi32.lib', 'gdi32.lib')
    $objs = @(Get-ChildItem obj\a64\*.obj, obj\ec\*.obj | ForEach-Object { $_.FullName })
    $ok = Invoke-Step 'link' (@('/nologo', '/dll', '/machine:arm64x', "/defArm64Native:$def", "/def:$def",
                                "/out:$dll") + $objs + @('obj\res.res') + $libs)
    if ($ok) {
        # Machine: "AA64 machine (ARM64) (ARM64X)" for a correct image.
        $machine = @(& dumpbin /headers $dll | Where-Object { $_ -match '\bmachine\b' } | ForEach-Object { $_.Trim() } |
                     Select-Object -First 3)
        Write-Host "dumpbin machine: $($machine -join ' | ')"
        if (($machine -join ' ') -notmatch 'ARM64X') {
            $failed = $true; $log.Add("error: dumpbin does not report an ARM64X image: $($machine -join ' | ')")
        }
        # Exports: rows "ordinal hint RVA name" of dumpbin /exports.
        $exports = @(& dumpbin /exports $dll | ForEach-Object {
                         if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)') { $Matches[1] } } |
                     Sort-Object -Unique)
        Write-Host "dumpbin exports: $($exports -join ', ')"
        if (($exports -join ',') -ne ($expected -join ',')) {
            $failed = $true; $log.Add("error: exports are '$($exports -join ', ')', expected '$($expected -join ', ')'")
        }
        # Imported DLLs: the indented *.dll lines of dumpbin /dependents.
        $imports = @(& dumpbin /dependents $dll | ForEach-Object {
                         if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() } } | Sort-Object -Unique)
        Write-Host "dumpbin dependents: $($imports -join ', ')"
        $crt = @($imports | Where-Object { $_ -match '^(vcruntime|msvcp|msvcr|concrt|ucrtbase)' })
        if ($crt.Count -gt 0) {
            $failed = $true; $log.Add("error: Visual C++ runtime DLL imported (the build must be /MT): $($crt -join ', ')")
        }
        $system = '^(kernel32|user32|gdi32|advapi32|ole32|oleaut32|shell32|shlwapi|avrt|winmm|comctl32|version|' +
                  'bcrypt|winhttp|crypt32|wintrust|ntdll|propsys|uxtheme|dwmapi|shcore|windowscodecs|api-ms-win-.*|' +
                  'ext-ms-win-.*)\.dll$'
        foreach ($i in $imports) {
            if ($i -notmatch $system -and $crt -notcontains $i) { $log.Add("warning: unexpected imported DLL: $i") }
        }
        $sha = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-Host "SHA-256: $sha"
    }
}
if (-not (Test-Path -LiteralPath $dll)) { $failed = $true }

# Classification of every compiler / linker / script message (see the header). Identical lines (the same warning
# from the ARM64 and the ARM64EC compilation of one file) are reported once.
$reErr  = '\b(fatal error|error) (C|LNK1|LNK2|RC|D)\d+|^error:'
$reWarn = '\bwarning (C|LNK4|RC|D)\d+|^warning:'
$errs  = @($log | Where-Object { $_ -match $reErr } | Select-Object -Unique)
$warns = @($log | Where-Object { $_ -notmatch $reErr -and $_ -match $reWarn } | Select-Object -Unique)
# A tool that failed without printing a classified error line (or the DLL is missing) still gets an ::error.
if ($failed -and $errs.Count -eq 0) { $errs = @($stepFails) + $(if (Test-Path -LiteralPath $dll) { @() } else { @("error: $dll was not built") }) }

function Write-Annotation([string]$kind, [string]$line) {
    $line = $line -ireplace [regex]::Escape($root + '\'), ''
    if ($line -match 'LNK4279') { $line += ' (benign, see scripts/build-arm64x.ps1 and CODE-REVIEW.txt "0.9.9 MSVC / Arm64X")' }
    # "path(line): message" or "path(line,col): message" (cl, rc)
    if ($line -match '^(.*?)\((\d+)(?:,\d+)?\)\s*:\s*(.*)$') {
        $f = $Matches[1].Replace('\', '/')
        Write-Host "::$kind file=$(Esc-Prop $f),line=$($Matches[2])::$(Esc-Data $Matches[3])"
    } else {
        Write-Host "::$kind::$(Esc-Data $line)"
    }
}
foreach ($e in ($errs | Select-Object -First 10)) { Write-Annotation 'error' $e }
foreach ($w in ($warns | Select-Object -First 10)) { Write-Annotation 'warning' $w }

$result = if ($failed) { 'FAILED' } else { 'OK' }
$summaryLines = @(
    "Result: $result",
    "dumpbin machine: $(if ($machine.Count) { $machine -join ' | ' } else { '(no image)' })",
    "Exports ($($exports.Count)): $(if ($exports.Count) { $exports -join ', ' } else { '(none)' })",
    "Imported DLLs ($($imports.Count)): $(if ($imports.Count) { $imports -join ', ' } else { '(none)' })",
    "SHA-256 ${dll}: $(if ($sha) { $sha } else { '(no file)' })",
    "Errors: $($errs.Count), warnings: $($warns.Count) (LNK4279 is expected and benign)")

if ($env:GITHUB_STEP_SUMMARY) {
    $sum = @('## Arm64X build (MSVC)', '') + ($summaryLines | ForEach-Object { "- $_" }) + @('', '### Errors', '', '```')
    $sum += ($errs | Select-Object -First 200)
    $sum += @('```', '', '### Warnings', '', '```')
    $sum += ($warns | Select-Object -First 200)
    $sum += '```'
    $sum | ForEach-Object { $_ -ireplace [regex]::Escape($root + '\'), '' } | Add-Content -Path $env:GITHUB_STEP_SUMMARY -Encoding utf8
}
if ($env:GITHUB_OUTPUT -and -not $failed) {
    @('ok=true', "sha256=$sha") | Add-Content -Path $env:GITHUB_OUTPUT -Encoding utf8
}
# The final notice: always written, readable on the public run page (Annotations) without signing in.
Write-Host "::notice title=Arm64X build ${result}::$(Esc-Data ($summaryLines -join "`n"))"
if ($failed) { exit 1 } else { exit 0 }
