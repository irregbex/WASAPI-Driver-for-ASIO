@echo off
rem WASAPI Driver for ASIO - stops the diagnostic trace started by "Start trace.bat" and saves it as an .etl file.
rem SPDX-License-Identifier: GPL-3.0-or-later
rem The file goes to the shared folder %PUBLIC%\Documents\WASAPI Driver for ASIO\Traces
rem (WasapiDriver-<date>-<time>.etl), which is the same for every account. Attach it to a GitHub issue (zip it first);
rem read the privacy notes in TRACING.txt before sharing it.
rem Elevation (0.9.8, review R4): "fltmc" succeeds only with administrator rights (it works on every Windows
rem edition, unlike "net session", which needs the Server service). The elevated copy is started with the marker
rem argument "elevated"; if it still has no administrator rights it stops instead of asking again (no loop).
rem Single quotes in paths are doubled for PowerShell's single-quoted strings.
setlocal
color E1
title WASAPI Driver for ASIO - Stop trace
rem 0.9.10: the trace is always saved in ONE fixed shared folder, %PUBLIC%\Documents\WASAPI Driver for ASIO\Traces
rem (normally C:\Users\Public\Documents\...). Earlier versions tried to find "the signed-in user's Desktop", but when
rem Windows elevates with a DIFFERENT administrator account (standard user + admin credentials at the UAC prompt) the
rem script runs as that account, and the detection proved unreliable on real systems: the file ended up in the
rem administrator's profile. %PUBLIC% is identical for every account, readable by all users, and the folder is opened
rem in Explorer at the end so nobody has to look for the file.
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
if exist "%WPR%" goto :name
echo ERROR: wpr.exe (Windows Performance Recorder) was not found.
pause
exit /b 1

:name
set "OUTDIR=%PUBLIC%\Documents\WASAPI Driver for ASIO\Traces"
if not defined PUBLIC set "OUTDIR=%SystemDrive%\Users\Public\Documents\WASAPI Driver for ASIO\Traces"
if not exist "%OUTDIR%" mkdir "%OUTDIR%" >nul 2>&1
if not exist "%OUTDIR%" set "OUTDIR=%USERPROFILE%"
set "TS="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "TS=%%t"
if not defined TS set "TS=trace"
set "OUT=%OUTDIR%\WasapiDriver-%TS%.etl"
echo Saving the trace (this can take a minute)...
"%WPR%" -stop "%OUT%"
rem wpr returns HRESULTs (negative on failure): exactly 0 = OK
set "RC=%errorlevel%"
if not "%RC%"=="0" goto :failed
echo.
echo Trace saved to this file (full path):
echo.
echo     "%OUT%"
echo.
echo The folder opens now in Explorer. Zip the file and attach it to your GitHub issue.
echo Read TRACING.txt (privacy) before sharing it.
start "" explorer.exe /select,"%OUT%"
pause
exit /b 0

:failed
echo.
echo ERROR: no trace was saved (code %RC%). Was "Start trace.bat" run first?
pause
exit /b 1
