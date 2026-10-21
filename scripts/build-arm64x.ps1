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
# compiler error. Errors are published as GitHub annotations (first 10) and in the job summary (up to 200 lines),
# both visible on the public run page without signing in.
# Output: WasapiDriverARM64X.dll in the repository root. Exit code 0 only if the DLL was built and dumpbin reports
# an ARM64X image.

$ErrorActionPreference = 'Continue'
$root = (Get-Location).Path
New-Item -ItemType Directory -Force -Path obj\a64, obj\ec | Out-Null
$log = New-Object System.Collections.Generic.List[string]
$failed = $false

function Invoke-Step([string]$exe, [string[]]$argv) {
    $out = & $exe @argv 2>&1 | ForEach-Object { "$_" }
    foreach ($l in $out) { $log.Add($l); Write-Host $l }
    if ($LASTEXITCODE -ne 0) { $script:failed = $true; return $false }
    return $true
}

$cf = @('/nologo', '/c', '/O2', '/EHsc', '/std:c++17', '/W3', '/MT', '/utf-8', '/permissive-', '/Zc:__cplusplus',
        '/DUNICODE', '/D_UNICODE', '/D_WIN32_WINNT=0x0A00', '/D_CRT_SECURE_NO_WARNINGS', '/DNOMINMAX',
        '/Ithird_party\asiosdk', '/Isrc')
$src = @('dll', 'driver', 'config', 'panel', 'crash', 'update', 'trace')

Invoke-Step 'rc' @('/nologo', '/i', 'src', '/fo', 'obj\res.res', 'src\WasapiDriver.rc') | Out-Null
foreach ($s in $src) {
    Invoke-Step 'cl' ($cf + @("/Foobj\a64\$s.obj", "src\$s.cpp")) | Out-Null
    Invoke-Step 'cl' ($cf + @('/arm64EC', "/Foobj\ec\$s.obj", "src\$s.cpp")) | Out-Null
}

$machine = ''
if (-not $failed) {
    $libs = @('ole32.lib', 'uuid.lib', 'avrt.lib', 'winmm.lib', 'shell32.lib', 'user32.lib', 'advapi32.lib', 'gdi32.lib')
    $objs = @(Get-ChildItem obj\a64\*.obj, obj\ec\*.obj | ForEach-Object { $_.FullName })
    $ok = Invoke-Step 'link' (@('/nologo', '/dll', '/machine:arm64x', '/defArm64Native:src\WasapiBridge.def',
                                '/def:src\WasapiBridge.def', '/out:WasapiDriverARM64X.dll') + $objs + @('obj\res.res') + $libs)
    if ($ok) {
        $machine = (& dumpbin /headers WasapiDriverARM64X.dll | Select-String -Pattern 'machine' | Select-Object -First 3) -join ' | '
        Write-Host "dumpbin: $machine"
        if ($machine -notmatch 'ARM64X') { $failed = $true; $log.Add("error: dumpbin does not report an ARM64X image: $machine") }
        & dumpbin /exports WasapiDriverARM64X.dll | Select-String 'Dll' | ForEach-Object { Write-Host "$_" }
    }
}

# Report: error lines -> annotations (first 10, with file and line) and job summary (up to 200 lines).
$errs = @($log | Where-Object { $_ -match '(fatal error|error) [A-Z]+\d+|^error:|LNK\d+' } | Select-Object -Unique)
$n = 0
foreach ($e in $errs) {
    if ($n -ge 10) { break }
    $msg = $e -replace '%', '%25' -replace "`r", '' -replace "`n", ' '
    if ($e -match '^(.*?)\((\d+)(?:,\d+)?\)\s*:\s*(.*)$') {
        $f = $Matches[1].Replace($root + '\', '').Replace('\', '/')
        Write-Host "::error file=$f,line=$($Matches[2])::$($Matches[3] -replace '%','%25')"
    } else { Write-Host "::error::$msg" }
    $n++
}
if ($env:GITHUB_STEP_SUMMARY) {
    $sum = @('## Arm64X build (MSVC)', '', "Result: $(if ($failed) { 'FAILED' } else { 'OK' })", "dumpbin: $machine", '',
             "Compiler/linker errors: $($errs.Count)", '', '```')
    $sum += ($errs | Select-Object -First 200 | ForEach-Object { $_.Replace($root + '\', '') })
    $sum += '```'
    $sum | Add-Content -Path $env:GITHUB_STEP_SUMMARY -Encoding utf8
}
if ($failed) { exit 1 } else { exit 0 }
