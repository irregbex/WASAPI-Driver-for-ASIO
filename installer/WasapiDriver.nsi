; WASAPI Driver for ASIO - NSIS installer (built on Linux with makensis -DVERSION=x.y.z)
; Copyright (C) 2026 The WASAPI Driver for ASIO contributors
; SPDX-License-Identifier: GPL-3.0-or-later
;
; This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
; Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
; option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY
; WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
; GNU General Public License (LICENSE.txt) for more details.
;
; ASIO is a registered trademark of Steinberg Media Technologies GmbH.
;
; Installs the three driver DLLs, the diagnostics and test tools and the documentation into
; "Program Files\WASAPI Driver for ASIO", registers the COM class and the ASIO key in both registry views
; (64-bit view: x64 or ARM64 DLL, chosen on ARM64; 32-bit view: x86 DLL for 32-bit hosts), creates Start menu
; shortcuts and an uninstaller. Upgrades from the old name (WasapiBridge, up to 0.5.x) are cleaned up first.
; 0.9.6 (KI-2): before anything is changed, Setup and the uninstaller check that the installed DLLs and tools are
; not in use (WaitFilesFree below) and ask the user to close the audio programs, with Retry / Cancel.
; 0.9.8: also installs the diagnostic tracing kit into "$INSTDIR\Tracing" (WasapiDriver.wprp, Start trace.bat,
; Stop trace.bat, TRACING.txt; from ..\trace\ and ..\TRACING.txt) with Start menu shortcuts. The .bat files
; ask for administrator rights themselves (Windows Performance Recorder needs them).
Unicode true
!include "x64.nsh"
!include "LogicLib.nsh"
!include "MUI2.nsh"
!define NAME "WASAPI Driver for ASIO"
!define FILEBASE "WasapiDriver"
!define OLDNAME "WasapiBridge"
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!define CLSID "{A63175BD-B872-4FA5-9595-464F0236E1D0}"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\${NAME}"
!define OLDUNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\${OLDNAME}"
!define ATTRIB "ASIO is a registered trademark of Steinberg Media Technologies GmbH."

Name "${NAME} ${VERSION}"
Caption "${NAME}(R) ${VERSION} - Setup"
UninstallCaption "${NAME} ${VERSION} - Uninstall"
OutFile "..\WASAPI-Driver-for-ASIO-${VERSION}-Setup.exe"
RequestExecutionLevel admin
; 0.8.0 (G8): optional code signing. The release workflow passes -DSIGN_CMD="<command>"; makensis then runs it on
; the finished installer (!finalize) and on the uninstaller before it is embedded (!uninstfinalize, NSIS 3.08+),
; so both are signed. %1 is replaced by the file path. Without SIGN_CMD (local builds) nothing is signed. The
; command itself does nothing when no certificate is configured (scripts/sign.sh, see SIGNING.txt).
!ifdef SIGN_CMD
  !finalize '${SIGN_CMD} "%1"' = 0
  !uninstfinalize '${SIGN_CMD} "%1"' = 0
!endif
InstallDir "$PROGRAMFILES64\${NAME}"
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
BrandingText "${ATTRIB}"
CompletedText "Done. ${ATTRIB}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "FileDescription" "${NAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later. ${ATTRIB}"

; 0.9.0 (H4): Modern UI 2 pages in English and Spanish. The language follows the Windows UI language (English for
; any other language). The legal texts (NOTICES.txt, LICENSE.txt) stay in English; the page header says so in
; Spanish. The driver name, "ASIO" and the trademark sentence are never translated. Strings: lang_en.nsh,
; lang_es.nsh.
!define MUI_ABORTWARNING
; 0.9.3: the application icon (src/res/app.ico, copied next to this script) for the Setup, the uninstaller, the
; Settings > Apps entry (DisplayIcon) and the Quick start shortcut.
!define MUI_ICON "app.ico"
!define MUI_UNICON "app.ico"
; 0.9.1 (B2): the ASIO Compatible logo (white artwork of src/res, scaled with its proportions kept, on a dark
; background so it is legible; no other change) in the page header and on the welcome page, next to the product
; name, with the attribution sentence (TRADEMARKS.txt). Images: header.bmp 150x57, welcome.bmp 164x314.
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_BITMAP_NOSTRETCH
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP_NOSTRETCH
!define MUI_WELCOMEPAGE_TITLE "$(L_WELCOME_TITLE)"
!define MUI_WELCOMEPAGE_TEXT "$(L_WELCOME_TEXT)"
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_HEADER_TEXT "$(L_NOTICES_TITLE)"
!define MUI_PAGE_HEADER_SUBTEXT "$(L_NOTICES_SUB)"
!insertmacro MUI_PAGE_LICENSE "NOTICES.txt"
!insertmacro MUI_PAGE_LICENSE "..\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Spanish"
!include "lang_en.nsh"
!include "lang_es.nsh"

; COM class + ASIO key for one DLL in the current registry view (SetRegView before use).
!macro RegCom DLLPATH
  WriteRegStr HKLM "SOFTWARE\Classes\CLSID\${CLSID}" "" "${NAME}"
  WriteRegStr HKLM "SOFTWARE\Classes\CLSID\${CLSID}\InprocServer32" "" "${DLLPATH}"
  WriteRegStr HKLM "SOFTWARE\Classes\CLSID\${CLSID}\InprocServer32" "ThreadingModel" "Apartment"
  WriteRegStr HKLM "SOFTWARE\ASIO\${NAME}" "CLSID" "${CLSID}"
  WriteRegStr HKLM "SOFTWARE\ASIO\${NAME}" "Description" "${NAME}"
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(L_NEED64)" /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

; 0.9.6 (KI-2): "files in use" check, for Setup and (with the un. prefix) for the uninstaller.
; Why: a DLL that a host has loaded (Cubase, DaVinci Resolve, Ableton Live... or a host process that hung after
; quitting, see KNOWN-ISSUES KI-2) is mapped as an image; Windows then refuses to overwrite or delete it. Before
; 0.9.6 the File / Delete commands failed and Setup ended with an error, or left the old DLL in place (the user
; then kept testing the old version). Now the check runs first and nothing is changed until every file is free.
; How (stock NSIS only, no plug-in): FileOpen in mode "a" opens the file with GENERIC_READ | GENERIC_WRITE and
; OPEN_ALWAYS, without truncating and without writing anything. Windows refuses write access to a file that is
; mapped as an image (loaded DLL, running EXE), so the open fails with the error flag set exactly when Setup could
; not replace the file. Renaming is not a usable test: a loaded DLL can be renamed. FileExists comes first so the
; "a" mode never creates a missing file. The handle is closed at once. A read-only attribute is cleared first
; (SetFileAttributes NORMAL, review R5), so only a real lock produces the message.
; WaitFileFree: argument on the stack (full path). While the file is in use it shows a Retry / Cancel message;
; Retry checks again, Cancel aborts Setup (or the uninstaller) before anything was changed. In silent mode (/S)
; the default answer is Cancel, so a silent update never half-replaces the files. Keeps $R0 / $R1.
; WaitFilesFree: the check for every binary that Setup replaces and the uninstaller deletes.
!macro WAIT_FILES_FREE UN
Function ${UN}WaitFileFree
  Exch $R0
  Push $R1
  wff_retry:
  ${If} ${FileExists} "$R0"
    ; Review R5: a read-only attribute would make FileOpen fail forever (a false "in use"); File and Delete would
    ; also fail on it. Clearing the attributes is harmless: Setup replaces (or the uninstaller deletes) the file.
    SetFileAttributes "$R0" NORMAL
    ClearErrors
    FileOpen $R1 "$R0" a
    ${If} ${Errors}
      DetailPrint "$(L_INUSE_LOG) $R0"
      !if "${UN}" == ""
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(L_INUSE_HEAD)$\r$\n$\r$\n$R0$\r$\n$\r$\n$(L_INUSE_CLOSE)$\r$\n$\r$\n$(L_INUSE_CANCEL)" /SD IDCANCEL IDRETRY wff_retry
      !else
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(L_UN_INUSE_HEAD)$\r$\n$\r$\n$R0$\r$\n$\r$\n$(L_INUSE_CLOSE)$\r$\n$\r$\n$(L_UN_INUSE_CANCEL)" /SD IDCANCEL IDRETRY wff_retry
      !endif
      Pop $R1
      Pop $R0
      Abort
    ${EndIf}
    FileClose $R1
  ${EndIf}
  Pop $R1
  Pop $R0
FunctionEnd

Function ${UN}WaitFilesFree
  Push "$INSTDIR\${FILEBASE}64.dll"
  Call ${UN}WaitFileFree
  Push "$INSTDIR\${FILEBASE}32.dll"
  Call ${UN}WaitFileFree
  Push "$INSTDIR\${FILEBASE}ARM64.dll"
  Call ${UN}WaitFileFree
  Push "$INSTDIR\${FILEBASE}Diag.exe"
  Call ${UN}WaitFileFree
  Push "$INSTDIR\${FILEBASE}Test.exe"
  Call ${UN}WaitFileFree
FunctionEnd
!macroend
!insertmacro WAIT_FILES_FREE ""
!insertmacro WAIT_FILES_FREE "un."

; Removes the previous installation under the old name (WasapiBridge, up to 0.5.x): ASIO registration, files,
; Start menu shortcuts and the uninstall entry. The CLSID is the same and is registered again with the new path.
; The old folder is taken from InstallLocation of the old uninstall entry (64-bit view, then 32-bit view), with
; $PROGRAMFILES64\WasapiBridge as fallback. Only the files that version installed are deleted, and the folder is
; removed only if it is then empty (RMDir without /r), so a wrong InstallLocation cannot delete anything else.
; The old uninstall entry is deleted (in both views) only after the folder has been handled.
; The file names below are those of the released 0.5.x packages and must stay as they are.
Function RemoveOldInstall
  SetRegView 64
  DeleteRegKey HKLM "SOFTWARE\ASIO\${OLDNAME}"
  SetRegView 32
  DeleteRegKey HKLM "SOFTWARE\ASIO\${OLDNAME}"
  SetRegView 64
  ReadRegStr $0 HKLM "${OLDUNINST}" "InstallLocation"
  ${If} $0 == ""
    SetRegView 32
    ReadRegStr $0 HKLM "${OLDUNINST}" "InstallLocation"
    SetRegView 64
  ${EndIf}
  ${If} $0 == ""
    StrCpy $0 "$PROGRAMFILES64\${OLDNAME}"
  ${EndIf}
  ${If} ${FileExists} "$0\*.*"
    DetailPrint "$(L_REMOVING_OLD) $0"
    Delete "$0\WasapiBridge64.dll"
    Delete "$0\WasapiBridgeARM64.dll"
    Delete "$0\WasapiBridge32.dll"
    Delete "$0\WasapiBridgeDiag.exe"
    Delete "$0\WasapiBridgeTest.exe"
    Delete "$0\WasapiBridge-*.txt"
    Delete "$0\README.txt"
    Delete "$0\GUIA-DE-PRUEBAS.txt"   ; legacy Spanish file name (0.5.x test guide), cleanup only
    Delete "$0\LICENSE.txt"
    Delete "$0\Desinstalar.exe"       ; legacy Spanish file name (0.5.x uninstaller), cleanup only
    RMDir "$0"
  ${EndIf}
  SetRegView 64
  DeleteRegKey HKLM "${OLDUNINST}"
  SetRegView 32
  DeleteRegKey HKLM "${OLDUNINST}"
  SetRegView 64
  SetShellVarContext all
  Delete "$SMPROGRAMS\${OLDNAME}\*.lnk"
  RMDir "$SMPROGRAMS\${OLDNAME}"
FunctionEnd

Section "Driver"
  Call WaitFilesFree                  ; 0.9.6 (KI-2): first, so Cancel leaves the old installation untouched
  Call RemoveOldInstall
  SetOutPath "$INSTDIR"
  File "..\${FILEBASE}64.dll"
  File "..\${FILEBASE}ARM64.dll"
  File "..\${FILEBASE}32.dll"
  File "..\${FILEBASE}Diag.exe"
  File "..\${FILEBASE}Test.exe"
  File "..\README.txt"
  File "..\QUICK-START.txt"
  File "..\FAQ.txt"
  File "..\COMPATIBILITY.txt"
  File "..\TEST-GUIDE.txt"
  File "..\COPYRIGHT.txt"
  File "..\THIRD-PARTY-NOTICES.txt"
  File "..\TRADEMARKS.txt"
  File "..\DISCLAIMER.txt"
  File "..\PRIVACY.txt"
  File "..\SECURITY.txt"
  File "..\CONTRIBUTING.txt"
  File "..\LICENSE.txt"
  File "NOTICES.txt"
  File "app.ico"
  ; 0.9.8: tracing kit (Windows Performance Recorder profile and helpers, see TRACING.txt)
  SetOutPath "$INSTDIR\Tracing"
  File "..\trace\WasapiDriver.wprp"
  File "..\trace\Start trace.bat"
  File "..\trace\Stop trace.bat"
  File "..\TRACING.txt"
  SetOutPath "$INSTDIR"
  ; 64-bit view: x64 or ARM64 (one architecture per machine)
  SetRegView 64
  ${If} ${IsNativeARM64}
    MessageBox MB_YESNO|MB_ICONQUESTION "$(L_ARM64_Q)" /SD IDNO IDNO x64dll
      !insertmacro RegCom "$INSTDIR\${FILEBASE}ARM64.dll"
      DetailPrint "$(L_REG_ARM64)"
      Goto reg32
    x64dll:
  ${EndIf}
  !insertmacro RegCom "$INSTDIR\${FILEBASE}64.dll"
  DetailPrint "$(L_REG_X64)"
  reg32:
  ; 32-bit view (WOW6432Node): 32-bit DAWs
  SetRegView 32
  !insertmacro RegCom "$INSTDIR\${FILEBASE}32.dll"
  DetailPrint "$(L_REG_X86)"
  SetRegView 64
  ; Start menu shortcuts (all users). Shortcuts left by earlier builds in this folder are replaced.
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\${NAME}"
  Delete "$SMPROGRAMS\${NAME}\*.lnk"
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Diagnostics.lnk" "$INSTDIR\${FILEBASE}Diag.exe" "" "$INSTDIR\${FILEBASE}Diag.exe" 0
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Automatic test.lnk" "$INSTDIR\${FILEBASE}Test.exe" "" "$INSTDIR\${FILEBASE}Test.exe" 0
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Latency test (cable).lnk" "$INSTDIR\${FILEBASE}Test.exe" "--latency" "$INSTDIR\${FILEBASE}Test.exe" 0
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Soak test (60 min).lnk" "$INSTDIR\${FILEBASE}Test.exe" "--soak 60" "$INSTDIR\${FILEBASE}Test.exe" 0
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Quick start.lnk" "$INSTDIR\QUICK-START.txt" "" "$INSTDIR\app.ico" 0
  ; 0.9.8: tracing (the .bat files elevate themselves; the guide explains what is recorded)
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Start trace.lnk" "$INSTDIR\Tracing\Start trace.bat" "" "$INSTDIR\app.ico" 0
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Stop trace.lnk" "$INSTDIR\Tracing\Stop trace.bat" "" "$INSTDIR\app.ico" 0
  CreateShortcut "$SMPROGRAMS\${NAME}\${NAME} Tracing guide.lnk" "$INSTDIR\Tracing\TRACING.txt" "" "$INSTDIR\app.ico" 0
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINST}" "DisplayName" "${NAME} ${VERSION}"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\app.ico"
  WriteRegStr HKLM "${UNINST}" "Publisher" "${NAME} contributors"
  WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
  DetailPrint "${ATTRIB}"
SectionEnd

; Removes only this driver: its CLSID and ASIO keys (new and old name) in both views, its files, shortcuts and the
; uninstall entry. The user's settings in %APPDATA%\WASAPI Driver for ASIO are kept.
Section "Uninstall"
  Call un.WaitFilesFree               ; 0.9.6 (KI-2): nothing is removed while a host still uses the driver
  SetRegView 64
  DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\${CLSID}"
  DeleteRegKey HKLM "SOFTWARE\ASIO\${NAME}"
  DeleteRegKey HKLM "SOFTWARE\ASIO\${OLDNAME}"
  DeleteRegKey HKLM "${UNINST}"
  SetRegView 32
  DeleteRegKey HKLM "SOFTWARE\Classes\CLSID\${CLSID}"
  DeleteRegKey HKLM "SOFTWARE\ASIO\${NAME}"
  DeleteRegKey HKLM "SOFTWARE\ASIO\${OLDNAME}"
  SetRegView 64
  Delete "$INSTDIR\${FILEBASE}64.dll"
  Delete "$INSTDIR\${FILEBASE}ARM64.dll"
  Delete "$INSTDIR\${FILEBASE}32.dll"
  Delete "$INSTDIR\${FILEBASE}Diag.exe"
  Delete "$INSTDIR\${FILEBASE}Test.exe"
  Delete "$INSTDIR\${FILEBASE}-*.txt"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\QUICK-START.txt"
  Delete "$INSTDIR\FAQ.txt"
  Delete "$INSTDIR\COMPATIBILITY.txt"
  Delete "$INSTDIR\TEST-GUIDE.txt"
  Delete "$INSTDIR\COPYRIGHT.txt"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  Delete "$INSTDIR\TRADEMARKS.txt"
  Delete "$INSTDIR\DISCLAIMER.txt"
  Delete "$INSTDIR\PRIVACY.txt"
  Delete "$INSTDIR\SECURITY.txt"
  Delete "$INSTDIR\CONTRIBUTING.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\NOTICES.txt"
  Delete "$INSTDIR\app.ico"
  Delete "$INSTDIR\Tracing\WasapiDriver.wprp"
  Delete "$INSTDIR\Tracing\Start trace.bat"
  Delete "$INSTDIR\Tracing\Stop trace.bat"
  Delete "$INSTDIR\Tracing\TRACING.txt"
  RMDir "$INSTDIR\Tracing"
  Delete "$INSTDIR\Uninstall.exe"
  SetShellVarContext all
  Delete "$SMPROGRAMS\${NAME}\*.lnk"
  RMDir "$SMPROGRAMS\${NAME}"
  RMDir "$INSTDIR"
SectionEnd
