// WASAPI Driver for ASIO - help tip table (0.9.2)
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
// Purpose
//   One entry per tip key: the control id (or an ID_TIP_* key of a painted item or dialog button, panel_ids.h),
//   the string id of its text (strings.h, all five languages) and its tag: kLive ("Applies live") or kReset
//   ("DAW restarts the driver") for settings, following NeedsReset (config.cpp) and the RESET marks of the
//   labels; kNone for actions and read-only items. Information labels with an id reuse the tip of the control
//   they describe. kRequired lists every control and item the panel, its dialogs and the monitor window
//   create; tests/strings_test.cpp fails if one of them has no tip or a tip is longer than 220 characters.
//   Portable: no Windows types; used by panel.cpp (host thread) and the offline test.
//
// Lookup flow (panel.cpp)
//   On hover, the panel knows the window context (Ctx) and the control id: KeyFor(ctx, id) maps it to a tip key,
//   Find(key) returns the Def; the text is T(str) plus a localized "(Applies live)" / "(DAW restarts the
//   driver)" suffix by tag, and the tag also picks the accent color of the tip.
//
// Maintenance pitfalls
//   - Adding a control: add it to kDefs AND kRequired, and add the S_TIP_* string in all five languages; the test
//     catches a missing entry, but not a wrong tag. Keep the tag in sync with NeedsReset in config.cpp.
//   - Find() is a linear scan (~100 entries); fine for hover events, not for anything per-frame.
#pragma once
#include "panel_ids.h"
#include "strings.h"

namespace tips {

// Suffix shown after the tip text: none, "Applies live" or "DAW restarts the driver".
enum Tag { kNone = 0, kLive, kReset };
// key: control id or ID_TIP_* pseudo-id; str: string id (lang::S_TIP_*); tag: Tag.
struct Def { int key; int str; int tag; };

using namespace wbid;
using namespace lang;

inline const Def kDefs[] = {
    // tabs (painted)
    {ID_TIP_TAB0 + 0, S_TIP_TAB_MAIN, kNone}, {ID_TIP_TAB0 + 1, S_TIP_TAB_ADV, kNone}, {ID_TIP_TAB0 + 2, S_TIP_TAB_DEV, kNone},
    {ID_TIP_TAB0 + 3, S_TIP_TAB_STATE, kNone}, {ID_TIP_TAB0 + 4, S_TIP_TAB_UI, kNone}, {ID_TIP_TAB0 + 5, S_TIP_TAB_ABOUT, kNone},
    // Main
    {ID_PROFILE, S_TIP_PROFILE, kNone}, {ID_PSAVE, S_TIP_PSAVE, kNone}, {ID_PDEL, S_TIP_PDEL, kNone},
    {ID_OUT, S_TIP_OUT, kReset}, {ID_IN, S_TIP_IN, kReset}, {ID_MODE, S_TIP_MODE, kReset}, {ID_BUF, S_TIP_BUF, kReset},
    {ID_PRIVLBL, S_TIP_IN, kReset}, {ID_SETNONE, S_TIP_SETNONE, kNone},
    {ID_BTLBL, S_TIP_BTREC, kNone}, {ID_BTREC, S_TIP_BTREC, kNone},
    {ID_MONWIN, S_TIP_MONWIN, kLive}, {ID_TRAY, S_TIP_TRAY, kLive}, {ID_METER, S_TIP_METER, kNone},
    {ID_TIP_TILE0 + 0, S_TIP_TILE_OUTLAT, kNone}, {ID_TIP_TILE0 + 1, S_TIP_TILE_INLAT, kNone},
    {ID_TIP_TILE0 + 2, S_TIP_TILE_DSP, kNone}, {ID_TIP_TILE0 + 3, S_TIP_TILE_GLITCH, kNone},
    // Advanced
    {ID_LL, S_TIP_LL, kReset}, {ID_FOLLOW, S_TIP_FOLLOW, kLive}, {ID_LOG, S_TIP_LOG, kLive}, {ID_DITHER, S_TIP_DITHER, kLive},
    {ID_A2DP, S_TIP_A2DP, kReset}, {ID_DRIFT, S_TIP_DRIFT, kReset}, {ID_RESAMP, S_TIP_RESAMP, kReset}, {ID_RATE, S_TIP_RATE, kReset},
    {ID_GAIN, S_TIP_GAIN, kLive}, {ID_LIMON, S_TIP_LIMON, kLive}, {ID_LIMCEIL, S_TIP_LIMCEIL, kLive},
    {ID_LIMCAP, S_TIP_LIMCAP, kLive}, {ID_LIMCAPDB, S_TIP_LIMCAPDB, kLive},
    {ID_LB, S_TIP_LB, kReset}, {ID_APPLBL, S_TIP_APP, kReset}, {ID_APP, S_TIP_APP, kReset}, {ID_APPREF, S_TIP_APPREF, kNone},
    {ID_OFSOUT, S_TIP_OFSOUT, kLive}, {ID_OFSIN, S_TIP_OFSIN, kLive}, {ID_OFSHINT, S_TIP_OFSOUT, kLive},
    {ID_HPROF, S_TIP_HPROF, kNone}, {ID_HPLIST, S_TIP_HPLIST, kNone}, {ID_HPDEL, S_TIP_HPDEL, kNone},
    // Extra devices & monitoring
    {ID_EXOUT, S_TIP_EXOUT, kReset}, {ID_EXIN, S_TIP_EXIN, kReset}, {ID_ALIGN, S_TIP_ALIGN, kReset}, {ID_ROUTE, S_TIP_ROUTE, kLive},
    {ID_DMON, S_TIP_DMON, kLive}, {ID_DMIN, S_TIP_DMIN, kLive}, {ID_DMOUT, S_TIP_DMOUT, kLive}, {ID_DMMUTE, S_TIP_DMMUTE, kLive},
    {ID_DMLVL, S_TIP_DMLVL, kLive}, {ID_DMPAN, S_TIP_DMPAN, kLive}, {ID_DMNOTE, S_TIP_DMON, kLive},
    {ID_EXSTATUS, S_TIP_EXSTATUS, kNone},
    // Status
    {ID_STATUS, S_TIP_STATUS, kNone}, {ID_COPY, S_TIP_COPY, kNone}, {ID_OPENLOG, S_TIP_OPENLOG, kNone},
    {ID_OPENDIR, S_TIP_OPENDIR, kNone}, {ID_RESETCNT, S_TIP_RESETCNT, kNone},
    // Interface
    {ID_THEME, S_TIP_THEME, kLive}, {ID_LANG, S_TIP_LANG, kLive}, {ID_APPNOTE, S_TIP_THEME, kLive}, {ID_HELPTIPS, S_TIP_HELPTIPS, kLive},
    {ID_VIS, S_TIP_VIS, kLive}, {ID_VISINT, S_TIP_VISINT, kLive}, {ID_VISMON, S_TIP_VISMON, kLive}, {ID_VISSPD, S_TIP_VISSPD, kLive},
    {ID_UPDCHECK, S_TIP_UPDCHECK, kNone}, {ID_UPDWEEKLY, S_TIP_UPDWEEKLY, kLive}, {ID_UPDSTATUS, S_TIP_UPDCHECK, kNone},
    {ID_UPDDL, S_TIP_UPDDL, kNone}, {ID_UPDRUN, S_TIP_UPDRUN, kNone}, {ID_UPDNOTE, S_TIP_UPDCHECK, kNone},
    {ID_UPDLAST, S_TIP_UPDCHECK, kNone},
    // bottom bar
    {ID_DLG_OK, S_TIP_APPLY, kNone}, {ID_DLG_CANCEL, S_TIP_CLOSE, kNone}, {ID_SAFEBTN, S_TIP_SAFE, kNone},
    // dialogs (profile name, output routing)
    {ID_NAMEEDIT, S_TIP_NAMEEDIT, kNone}, {ID_NAMEDEV, S_TIP_NAMEDEV, kNone}, {ID_TIP_NAMESAVE, S_TIP_NAMESAVE, kNone},
    {ID_TIP_DLGCANCEL, S_TIP_DLGCANCEL, kNone}, {ID_ROUTE0, S_TIP_ROUTECH, kNone}, {ID_ROUTEID, S_TIP_ROUTEID, kNone},
    {ID_TIP_ROUTEOK, S_TIP_ROUTEOK, kNone},
    // monitor window (Reset counters and Use my settings map to the panel's tips, see KeyFor)
    {ID_TIP_MWSUM, S_TIP_MW_SUMMARY, kNone}, {ID_MW_SET, S_TIP_MW_SET, kNone}, {ID_MW_PIN, S_TIP_MW_PIN, kLive},
    {ID_MW_CLOSE, S_TIP_MW_CLOSE, kNone},
};

// Contexts: which window the control belongs to (the same id means different things in the dialogs).
enum Ctx { kPanel = 0, kMonitor, kNameDlg, kRouteDlg };

// Tip key of control `id` in context `ctx`.
// The generic OK / Cancel ids mean different things per dialog, the 32 routing combo boxes share one tip
// (ID_ROUTE0), and the monitor window's Reset / Safe buttons reuse the panel's tips. Anything else maps to itself.
inline int KeyFor(int ctx, int id) {
    if (ctx == kNameDlg) { if (id == ID_DLG_OK) return ID_TIP_NAMESAVE; if (id == ID_DLG_CANCEL) return ID_TIP_DLGCANCEL; }
    if (ctx == kRouteDlg) {
        if (id == ID_DLG_OK) return ID_TIP_ROUTEOK;
        if (id == ID_DLG_CANCEL) return ID_TIP_DLGCANCEL;
        if (id >= ID_ROUTE0 && id < ID_ROUTE0 + 32) return ID_ROUTE0;
    }
    if (ctx == kMonitor) { if (id == ID_MW_RESET) return ID_RESETCNT; if (id == ID_MW_SAFE) return ID_SAFEBTN; }
    return id;
}
// Def for a tip key, or nullptr (the panel then shows no tip).
inline const Def* Find(int key) {
    for (const Def& d : kDefs) if (d.key == key) return &d;
    return nullptr;
}

// Every control and painted item that must have a tip, as (context, id) pairs.
struct Item { int ctx; int id; };
inline const Item kRequired[] = {
    {kPanel, ID_TIP_TAB0}, {kPanel, ID_TIP_TAB0 + 1}, {kPanel, ID_TIP_TAB0 + 2}, {kPanel, ID_TIP_TAB0 + 3},
    {kPanel, ID_TIP_TAB0 + 4}, {kPanel, ID_TIP_TAB0 + 5},
    {kPanel, ID_PROFILE}, {kPanel, ID_PSAVE}, {kPanel, ID_PDEL}, {kPanel, ID_OUT}, {kPanel, ID_IN}, {kPanel, ID_MODE},
    {kPanel, ID_BUF}, {kPanel, ID_PRIVLBL}, {kPanel, ID_SETNONE}, {kPanel, ID_BTLBL}, {kPanel, ID_BTREC},
    {kPanel, ID_MONWIN}, {kPanel, ID_TRAY}, {kPanel, ID_METER},
    {kPanel, ID_TIP_TILE0}, {kPanel, ID_TIP_TILE0 + 1}, {kPanel, ID_TIP_TILE0 + 2}, {kPanel, ID_TIP_TILE0 + 3},
    {kPanel, ID_LL}, {kPanel, ID_FOLLOW}, {kPanel, ID_LOG}, {kPanel, ID_DITHER}, {kPanel, ID_A2DP}, {kPanel, ID_DRIFT},
    {kPanel, ID_RESAMP}, {kPanel, ID_RATE}, {kPanel, ID_GAIN}, {kPanel, ID_LIMON}, {kPanel, ID_LIMCEIL},
    {kPanel, ID_LIMCAP}, {kPanel, ID_LIMCAPDB}, {kPanel, ID_LB}, {kPanel, ID_APPLBL}, {kPanel, ID_APP},
    {kPanel, ID_APPREF}, {kPanel, ID_OFSOUT}, {kPanel, ID_OFSIN}, {kPanel, ID_OFSHINT}, {kPanel, ID_HPROF},
    {kPanel, ID_HPLIST}, {kPanel, ID_HPDEL},
    {kPanel, ID_EXOUT}, {kPanel, ID_EXIN}, {kPanel, ID_ALIGN}, {kPanel, ID_ROUTE}, {kPanel, ID_DMON}, {kPanel, ID_DMIN},
    {kPanel, ID_DMOUT}, {kPanel, ID_DMMUTE}, {kPanel, ID_DMLVL}, {kPanel, ID_DMPAN}, {kPanel, ID_DMNOTE},
    {kPanel, ID_EXSTATUS},
    {kPanel, ID_STATUS}, {kPanel, ID_COPY}, {kPanel, ID_OPENLOG}, {kPanel, ID_OPENDIR}, {kPanel, ID_RESETCNT},
    {kPanel, ID_THEME}, {kPanel, ID_LANG}, {kPanel, ID_APPNOTE}, {kPanel, ID_HELPTIPS}, {kPanel, ID_VIS},
    {kPanel, ID_VISINT}, {kPanel, ID_VISMON}, {kPanel, ID_VISSPD}, {kPanel, ID_UPDCHECK}, {kPanel, ID_UPDWEEKLY},
    {kPanel, ID_UPDSTATUS}, {kPanel, ID_UPDDL}, {kPanel, ID_UPDRUN}, {kPanel, ID_UPDNOTE}, {kPanel, ID_UPDLAST},
    {kPanel, ID_DLG_OK}, {kPanel, ID_DLG_CANCEL}, {kPanel, ID_SAFEBTN},
    {kNameDlg, ID_NAMEEDIT}, {kNameDlg, ID_NAMEDEV}, {kNameDlg, ID_DLG_OK}, {kNameDlg, ID_DLG_CANCEL},
    {kRouteDlg, ID_ROUTE0}, {kRouteDlg, ID_ROUTE0 + 31}, {kRouteDlg, ID_ROUTEID}, {kRouteDlg, ID_DLG_OK},
    {kRouteDlg, ID_DLG_CANCEL},
    {kMonitor, ID_TIP_MWSUM}, {kMonitor, ID_METER}, {kMonitor, ID_MW_SET}, {kMonitor, ID_MW_RESET}, {kMonitor, ID_MW_PIN},
    {kMonitor, ID_MW_CLOSE}, {kMonitor, ID_MW_SAFE},
};

}  // namespace tips
