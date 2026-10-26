@echo off
rem WASAPI Driver for ASIO - portable install (registers the DLLs in this folder; no installer needed).
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Elevation (0.9.8, review R4): "fltmc" succeeds only with administrator rights (it works on every Windows
rem edition, unlike "net session", which needs the Server service). The elevated copy is started with the marker
rem argument "elevated"; if it still has no administrator rights it stops instead of asking again (no loop).
rem Single quotes in paths are doubled for PowerShell's single-quoted strings.
rem Paths are only ever echoed outside ( ) blocks or inside quotes, so folders such as
rem "C:\Program Files (x86)\..." or names containing ")" cannot break the script.
rem 0.9.9 (Arm64X): on Windows on ARM64, if WasapiDriverARM64X.dll is in this folder it is registered ONCE in the
rem 64-bit view, without a question: native ARM64 and emulated x64 processes read the same 64-bit registry view,
rem and the loader maps the ARM64 half of that DLL into ARM64 DAWs and its ARM64EC half into emulated x64 DAWs.
rem Without that file (or after renaming it) the 0.9.8 question (native ARM64 or x64 DAW) is asked. 32-bit DAWs
rem keep WasapiDriver32.dll in the 32-bit view (WOW6432Node), as before.
setlocal
color 4A
title WASAPI Driver for ASIO 0.9.15 - Install
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
set "CLSID={A63175BD-B872-4FA5-9595-464F0236E1D0}"
set "HERE=%~dp0"
set "DLL=%HERE%WasapiDriver64.dll"
set "DLL32=%HERE%WasapiDriver32.dll"
set "ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "ARCH=%PROCESSOR_ARCHITEW6432%"
if /i not "%ARCH%"=="ARM64" goto :check
echo Windows ARM64 detected.
if not exist "%HERE%WasapiDriverARM64X.dll" goto :askarm
set "DLL=%HERE%WasapiDriverARM64X.dll"
echo Arm64X DLL found: one registration for native ARM64 and emulated x64 DAWs.
goto :check

:askarm
echo   1 = native ARM64 DAW
echo   2 = x64 DAW (emulated)
choice /c 12 /n /m "DAW architecture [1/2]: "
if errorlevel 2 goto :check
set "DLL=%HERE%WasapiDriverARM64.dll"

:check
if exist "%DLL%" goto :register
echo ERROR: file not found:
echo "%DLL%"
pause
exit /b 1

:register
set "R=%SystemRoot%\System32\reg.exe"
set "KEY=HKLM\SOFTWARE\ASIO\WASAPI Driver for ASIO"
rem Remove the registration of the old name (WasapiBridge, up to 0.5.x); the CLSID is the same
"%R%" delete "HKLM\SOFTWARE\ASIO\WasapiBridge" /f /reg:64 >nul 2>&1
"%R%" delete "HKLM\SOFTWARE\ASIO\WasapiBridge" /f /reg:32 >nul 2>&1
"%R%" add "HKLM\SOFTWARE\Classes\CLSID\%CLSID%" /ve /d "WASAPI Driver for ASIO" /f /reg:64 >nul || goto :err
"%R%" add "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" /ve /d "%DLL%" /f /reg:64 >nul || goto :err
"%R%" add "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" /v ThreadingModel /d Apartment /f /reg:64 >nul || goto :err
"%R%" add "%KEY%" /v CLSID /d "%CLSID%" /f /reg:64 >nul || goto :err
"%R%" add "%KEY%" /v Description /d "WASAPI Driver for ASIO" /f /reg:64 >nul || goto :err
if not exist "%DLL32%" goto :done
"%R%" add "HKLM\SOFTWARE\Classes\CLSID\%CLSID%" /ve /d "WASAPI Driver for ASIO" /f /reg:32 >nul
"%R%" add "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" /ve /d "%DLL32%" /f /reg:32 >nul
"%R%" add "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" /v ThreadingModel /d Apartment /f /reg:32 >nul
"%R%" add "%KEY%" /v CLSID /d "%CLSID%" /f /reg:32 >nul
"%R%" add "%KEY%" /v Description /d "WASAPI Driver for ASIO" /f /reg:32 >nul
echo Also registered for 32-bit DAWs:
echo "%DLL32%"

:done
echo.
echo Installed:
echo "%DLL%"
echo ASIO is a registered trademark of Steinberg Media Technologies GmbH.
echo Do not move this folder; if you move it, run install.bat again.
echo.
"%R%" query "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" /reg:64
pause
exit /b 0

:err
echo ERROR: could not write to the registry (code %errorlevel%)
pause
exit /b 1
