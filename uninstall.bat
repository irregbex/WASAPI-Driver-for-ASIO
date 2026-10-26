@echo off
rem WASAPI Driver for ASIO - portable uninstall (removes only this driver's registry entries, both views).
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Elevation (0.9.8, review R4): "fltmc" succeeds only with administrator rights (it works on every Windows
rem edition, unlike "net session", which needs the Server service). The elevated copy is started with the marker
rem argument "elevated"; if it still has no administrator rights it stops instead of asking again (no loop).
rem Single quotes in paths are doubled for PowerShell's single-quoted strings.
rem 0.9.9: the keys are the same whichever DLL was registered (x64, ARM64, Arm64X, x86), so this also removes an
rem Arm64X registration.
setlocal
color 8B
title WASAPI Driver for ASIO 0.9.14 - Uninstall
fltmc >nul 2>&1
if not errorlevel 1 goto :admin
if /i "%~1"=="elevated" goto :noadmin
echo Requesting administrator rights...
set "SELF=%~f0"
powershell -NoProfile -Command "Start-Process -FilePath '%SELF:'=''%' -ArgumentList 'elevated' -Verb RunAs"
exit /b

:noadmin
echo ERROR: administrator rights are required and were not granted.
pause
exit /b 1

:admin
set "R=%SystemRoot%\System32\reg.exe"
"%R%" delete "HKLM\SOFTWARE\Classes\CLSID\{A63175BD-B872-4FA5-9595-464F0236E1D0}" /f /reg:64 >nul 2>&1
"%R%" delete "HKLM\SOFTWARE\ASIO\WASAPI Driver for ASIO" /f /reg:64 >nul 2>&1
"%R%" delete "HKLM\SOFTWARE\ASIO\WasapiBridge" /f /reg:64 >nul 2>&1
"%R%" delete "HKLM\SOFTWARE\Classes\CLSID\{A63175BD-B872-4FA5-9595-464F0236E1D0}" /f /reg:32 >nul 2>&1
"%R%" delete "HKLM\SOFTWARE\ASIO\WASAPI Driver for ASIO" /f /reg:32 >nul 2>&1
"%R%" delete "HKLM\SOFTWARE\ASIO\WasapiBridge" /f /reg:32 >nul 2>&1
echo Uninstalled. Your settings remain in %%APPDATA%%\WASAPI Driver for ASIO
pause
