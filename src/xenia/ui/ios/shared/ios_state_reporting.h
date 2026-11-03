/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_STATE_REPORTING_H_
#define XENIA_UI_IOS_SHARED_IOS_STATE_REPORTING_H_

#include <cstdint>

// iOS 27 state reporting, so MetricKit splits its daily metrics and labels its
// diagnostics by game: domain jp.xenios.title, state "game" with the title ID
// as stable metadata while one runs, no state otherwise. Does nothing before
// iOS 27.

#define XE_IOS_STATE_DOMAIN_TITLE "jp.xenios.title"

// 0: no game.
void xe_ios_state_reporting_note_title(uint32_t title_id);

#endif  // XENIA_UI_IOS_SHARED_IOS_STATE_REPORTING_H_
