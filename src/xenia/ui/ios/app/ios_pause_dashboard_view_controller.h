/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_PAUSE_DASHBOARD_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_APP_IOS_PAUSE_DASHBOARD_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Logical destinations the dashboard can ask its host to open. They map onto the
// existing in-game flows (touch editor, display options, quick/graphics
// settings, achievements sheet, live log) rather than owning that UI.
typedef NS_ENUM(NSInteger, XeniaPauseDashboardSection) {
  XeniaPauseDashboardSectionControls = 0,
  XeniaPauseDashboardSectionDisplay,
  XeniaPauseDashboardSectionGraphics,
  XeniaPauseDashboardSectionAchievements,
  XeniaPauseDashboardSectionDiagnostics,
};

@class XeniaPauseDashboardViewController;

@protocol XeniaPauseDashboardViewControllerDelegate <NSObject>
- (void)pauseDashboardDidSelectResume:(XeniaPauseDashboardViewController*)dashboard;
- (void)pauseDashboardDidSelectExit:(XeniaPauseDashboardViewController*)dashboard;
- (void)pauseDashboard:(XeniaPauseDashboardViewController*)dashboard
      didSelectSection:(XeniaPauseDashboardSection)section;
@end

// Unified in-game pause "home": a themed inset-grouped table that prioritizes
// Resume, surfaces the most-changed quick settings as value rows, summarizes the
// game, and treats Exit as a separate destructive action. Display strings are
// pushed in via properties and all actions are routed to the delegate, so the
// controller has no dependency on the app context and can be hosted directly or
// inside the iPad split-view workspace. Not yet wired into the overlay flow.
@interface XeniaPauseDashboardViewController : XESheetTableViewController

@property(nonatomic, assign) id<XeniaPauseDashboardViewControllerDelegate> delegate;

// Display strings, set by the host before presentation and on refresh. Nil
// values render as an empty detail (no crash).
@property(nonatomic, copy) NSString* gameTitle;
@property(nonatomic, copy) NSString* achievementsSummary;
@property(nonatomic, copy) NSString* profileName;
@property(nonatomic, copy) NSString* displayModeValue;
@property(nonatomic, copy) NSString* touchControlsValue;
@property(nonatomic, copy) NSString* performanceOverlayValue;

// Re-applies the title and reloads rows from the current property values.
- (void)reloadDashboard;

@end

#endif  // XENIA_UI_IOS_APP_IOS_PAUSE_DASHBOARD_VIEW_CONTROLLER_H_
