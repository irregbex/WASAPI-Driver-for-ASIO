; WASAPI Driver for ASIO - installer strings, English (0.9.0, H4)
; Copyright (C) 2026 The WASAPI Driver for ASIO contributors
; SPDX-License-Identifier: GPL-3.0-or-later
; ASIO is a registered trademark of Steinberg Media Technologies GmbH.
; Included by WasapiDriver.nsi after the MUI languages. Every LangString must also exist in lang_es.nsh.
LangString L_NOTICES_TITLE ${LANG_ENGLISH} "Notices"
LangString L_NOTICES_SUB ${LANG_ENGLISH} "Trademarks, license summary and safety notes."
LangString L_NEED64 ${LANG_ENGLISH} "${NAME} requires 64-bit Windows (x64 or ARM64)."
LangString L_REMOVING_OLD ${LANG_ENGLISH} "Removing the previous version (${OLDNAME}) from"
LangString L_ARM64_Q ${LANG_ENGLISH} "Windows on ARM64 detected.$\r$\n$\r$\nYes = the DAW is native ARM64$\r$\nNo = the DAW is x64 (emulated)"
LangString L_REG_ARM64 ${LANG_ENGLISH} "Registered ARM64 (64-bit view)"
LangString L_REG_X64 ${LANG_ENGLISH} "Registered x64 (64-bit view)"
LangString L_REG_X86 ${LANG_ENGLISH} "Registered x86 (32-bit view)"
; 0.9.9: Windows on Arm with the Arm64X DLL in the package (one registration for ARM64 and emulated x64 hosts)
LangString L_REG_ARM64X ${LANG_ENGLISH} "Registered Arm64X (64-bit view: native ARM64 and emulated x64 hosts)"
LangString L_WELCOME_TEXT ${LANG_ENGLISH} "Setup installs ${NAME} ${VERSION}: an ASIO driver for every Windows audio device (WASAPI Shared and Exclusive).$\r$\n$\r$\nClose your DAW before you continue.$\r$\n$\r$\n${ATTRIB}"
LangString L_WELCOME_TITLE ${LANG_ENGLISH} "${NAME}® ${VERSION}"
; 0.9.6 (KI-2): files in use (WaitFileFree in WasapiDriver.nsi)
LangString L_INUSE_LOG ${LANG_ENGLISH} "In use:"
LangString L_INUSE_HEAD ${LANG_ENGLISH} "This file of ${NAME} is in use, so Setup cannot replace it:"
LangString L_UN_INUSE_HEAD ${LANG_ENGLISH} "This file of ${NAME} is in use, so it cannot be removed:"
LangString L_INUSE_CLOSE ${LANG_ENGLISH} "Close your audio programs (Cubase, DaVinci Resolve, Ableton Live, other DAWs and audio editors, and the driver's diagnostics or test tool), then click Retry.$\r$\nIf a program stays in Task Manager after you closed it, end it there (Task Manager > select it > End task)."
LangString L_INUSE_CANCEL ${LANG_ENGLISH} "Cancel stops Setup. Nothing has been changed yet."
LangString L_UN_INUSE_CANCEL ${LANG_ENGLISH} "Cancel stops the uninstaller. Nothing has been removed yet."
