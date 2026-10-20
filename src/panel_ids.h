// WASAPI Driver for ASIO - control ids of the control panel, its dialogs and the monitor window
// Copyright (C) 2026 The WASAPI Driver for ASIO contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
// Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License (LICENSE.txt) for more details.
//
// ASIO is a registered trademark of Steinberg Media Technologies GmbH.
//
// Purpose (0.9.2)
//   Moved out of panel.cpp so that the help-tip table (tips.h) and tests/strings_test.cpp can check that every
//   control has a tip. Portable (no Windows types). Timer ids share the enum (small values, never control ids).
#pragma once

namespace wbid {
enum {
    ID_OUT = 100, ID_IN, ID_LB, ID_MODE, ID_BUF, ID_RATE, ID_LL, ID_FOLLOW, ID_DRIFT, ID_GAIN, ID_LOG,
    ID_RESAMP, ID_DITHER, ID_PROFILE, ID_PSAVE, ID_PDEL, ID_APP, ID_APPLBL, ID_APPREF,
    ID_STATUS = 140, ID_METER, ID_EXSTATUS, ID_OPENLOG = 150, ID_OPENDIR, ID_RESETCNT, ID_NAMEEDIT = 160, ID_NAMEDEV,
    ID_EXOUT = 170, ID_EXIN, ID_ROUTE, ID_ROUTEID, ID_ALIGN, ID_COPY, ID_ROUTE0 = 200, ID_TIMER = 1, ID_MTIMER = 2,
    // 0.7.0
    ID_PRIVLBL = 290, ID_SETNONE, ID_TOPTIMER = 4,
    ID_MONWIN = 300, ID_OFSOUT, ID_OFSIN, ID_DMON, ID_DMIN, ID_DMOUT, ID_DMLVL, ID_DMPAN, ID_DMMUTE, ID_DMNOTE, ID_SAFEBTN,
    ID_MW_SET = 400, ID_MW_RESET, ID_MW_PIN, ID_MW_CLOSE, ID_MW_SAFE, ID_MW_TIMER = 3,
    // 0.8.0
    ID_A2DP = 320, ID_LIMON, ID_LIMCEIL, ID_LIMCAP, ID_LIMCAPDB, ID_TRAY, ID_BTLBL, ID_BTREC, ID_HPROF, ID_HPLIST,
    ID_HPDEL, ID_OFSHINT, ID_TRAY_TIMER = 5,
    IDM_TR_SETTINGS = 500, IDM_TR_MONITOR, IDM_TR_RESET, IDM_TR_COPY, IDM_TR_ABOUT,
    // 0.8.1: panel visuals
    ID_VIS = 340, ID_VISINT, ID_VISMON, ID_VISSPD, ID_VTIMER = 6,
    // 0.9.0: Interface tab
    ID_THEME = 360, ID_LANG, ID_APPNOTE, ID_UPDCHECK, ID_UPDWEEKLY, ID_UPDSTATUS, ID_UPDDL, ID_UPDRUN, ID_UPDNOTE, ID_UPDLAST,
    // 0.9.2: help tips. ID_HELPTIPS is a control; the ID_TIP_* values are keys of painted items (tiles, tabs, the
    // monitor window's text lines) and of dialog buttons whose meaning differs from the panel's IDOK / IDCANCEL.
    ID_HELPTIPS = 370, ID_TIPTIMER = 7,
    ID_TIP_TILE0 = 380, ID_TIP_TAB0 = 390, ID_TIP_MWSUM = 410, ID_TIP_NAMESAVE = 420, ID_TIP_DLGCANCEL, ID_TIP_ROUTEOK,
    ID_DLG_OK = 1, ID_DLG_CANCEL = 2      // = IDOK / IDCANCEL of windows.h (static_assert in panel.cpp)
};

}  // namespace wbid
