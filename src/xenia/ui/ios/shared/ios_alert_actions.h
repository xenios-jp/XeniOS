/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_ALERT_ACTIONS_H_
#define XENIA_UI_IOS_SHARED_IOS_ALERT_ACTIONS_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

// Lets a controller answer an alert the way a tap does, running the handler of
// the action it picks rather than dismissing the alert bare, which strands
// whoever waits on that handler. UIAlertAction keeps its handler private, so
// every action records its own when it is created.

// What B runs, for an alert without a cancel-style action.
void XeniaAlertSetCancelHandler(UIAlertController* alert, void (^handler)(void));
// Marks an alert a guest thread waits on, so leaving the game cancels it.
void XeniaAlertMarkGuestPrompt(UIAlertController* alert);

// Dismisses the alert, then runs the action's handler, as a tap does.
void XeniaAlertPerformAction(UIAlertController* alert, UIAlertAction* action);
// A: the preferred action, else the first enabled one that does not cancel.
void XeniaAlertAccept(UIAlertController* alert);
// B: the cancel handler, else the cancel-style action, else a bare dismiss.
void XeniaAlertCancel(UIAlertController* alert);
// Moves the preferred action by delta through the enabled actions, wrapping.
void XeniaAlertMoveSelection(UIAlertController* alert, NSInteger delta);

// Cancels every guest prompt presented above root. Main thread only.
void XeniaCancelGuestAlerts(UIViewController* root);

// Keeps the field's text to at most max_length UTF-16 units, the guest buffer
// it goes into, rather than cutting it off silently on the way in.
void XeniaLimitTextFieldLength(UITextField* field, NSUInteger max_length);

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_SHARED_IOS_ALERT_ACTIONS_H_
