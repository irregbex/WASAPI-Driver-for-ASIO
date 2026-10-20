@echo off
rem WASAPI Driver for ASIO - starts a diagnostic trace of the driver (Windows Performance Recorder, built into Windows).
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Records only the driver's own events and the Windows audio service (WasapiDriver.wprp, next to this file); no
rem audio, no file contents. Reproduce the problem, then run "Stop trace.bat". See TRACING.txt.
rem Needs administrator rights (Windows requires them for tracing). Paths are only used inside quotes; delayed
rem expansion is switched off explicitly because the profile argument contains "!".
rem Elevation (0.9.8, review R4): "fltmc" succeeds only with administrator rights (it works on every Windows
rem edition, unlike "net session", which needs the Server service). The elevated copy is started with the marker
rem argument "elevated"; if it still has no administrator rights it stops instead of asking again (no loop).
rem Single quotes in paths are doubled for PowerShell's single-quoted strings.
setlocal DisableDelayedExpansion
color B1
title WASAPI Driver for ASIO 0.9.8 - Start trace
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
set "WPR=%SystemRoot%\System32\wpr.exe"
if not exist "%WPR%" set "WPR=%SystemRoot%\Sysnative\wpr.exe"
if exist "%WPR%" goto :profile
echo ERROR: wpr.exe (Windows Performance Recorder) was not found. It is part of Windows 10 and 11.
pause
exit /b 1

:profile
set "PROFILE=%~dp0WasapiDriver.wprp"
if exist "%PROFILE%" goto :start
echo ERROR: WasapiDriver.wprp was not found next to this file.
pause
exit /b 1

:start
"%WPR%" -start "%PROFILE%!WasapiDriver"
rem wpr returns HRESULTs, which are negative numbers on failure ("if errorlevel 1" would miss them): exactly 0 = OK
set "RC=%errorlevel%"
if not "%RC%"=="0" goto :failed
echo.
echo Tracing started (circular memory buffers: the most recent minutes are kept).
echo Now reproduce the problem in your DAW, then run "Stop trace.bat" to save the trace.
echo ASIO is a registered trademark of Steinberg Media Technologies GmbH.
pause
exit /b 0

:failed
echo.
echo ERROR: the trace could not be started (code %RC%). If a trace is already running,
echo run "Stop trace.bat" first, or cancel it with: wpr -cancel
pause
exit /b 1
