/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_APP_IOS_PAUSE_WORKSPACE_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_APP_IOS_PAUSE_WORKSPACE_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/app/ios_pause_dashboard_view_controller.h"

@class XeniaPauseWorkspaceViewController;

@protocol XeniaPauseWorkspaceViewControllerDelegate <NSObject>
- (void)pauseWorkspaceDidSelectResume:(XeniaPauseWorkspaceViewController*)workspace;
- (void)pauseWorkspaceDidSelectExit:(XeniaPauseWorkspaceViewController*)workspace;
// Vends the detail controller for a section. The host owns how each section is
// built (achievements sheet, quick/graphics settings, live log, …). Return nil
// to ignore the selection.
- (UIViewController*)pauseWorkspace:(XeniaPauseWorkspaceViewController*)workspace
                  contentForSection:(XeniaPauseDashboardSection)section;
@end

// iPad / regular-width host for the pause dashboard: a double-column split view
// with the dashboard as the sidebar and the selected section in the detail
// column. On compact width UIKit collapses it to a single navigation stack
// rooted at the dashboard, so iPhone (incl. landscape) stays a stack rather than
// a cramped sidebar — the same controllers serve both layouts. Section content
// is vended by the delegate so the workspace stays decoupled from the app
// context. Not yet wired into the overlay flow.
@interface XeniaPauseWorkspaceViewController : UISplitViewController

- (instancetype)initWithDashboard:(XeniaPauseDashboardViewController*)dashboard;

@property(nonatomic, assign) id<XeniaPauseWorkspaceViewControllerDelegate> workspaceDelegate;
@property(nonatomic, readonly) XeniaPauseDashboardViewController* dashboard;

@end

#endif  // XENIA_UI_IOS_APP_IOS_PAUSE_WORKSPACE_VIEW_CONTROLLER_H_
