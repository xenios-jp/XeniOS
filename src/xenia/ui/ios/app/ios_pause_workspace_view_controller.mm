/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_pause_workspace_view_controller.h"

#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/shared/ios_theme.h"

@interface XeniaPauseWorkspaceViewController () <XeniaPauseDashboardViewControllerDelegate,
                                                 UISplitViewControllerDelegate>
@end

@implementation XeniaPauseWorkspaceViewController {
  XeniaPauseDashboardViewController* dashboard_;
  XeniaLandscapeNavigationController* primary_nav_;
  XeniaLandscapeNavigationController* secondary_nav_;
  id<XeniaPauseWorkspaceViewControllerDelegate> workspace_delegate_;  // assign
}

@synthesize workspaceDelegate = workspace_delegate_;

- (instancetype)initWithDashboard:(XeniaPauseDashboardViewController*)dashboard {
  self = [super initWithStyle:UISplitViewControllerStyleDoubleColumn];
  if (self) {
    dashboard_ = [dashboard retain];
    dashboard_.delegate = self;

    primary_nav_ =
        [[XeniaLandscapeNavigationController alloc] initWithRootViewController:dashboard_];
    secondary_nav_ = [[XeniaLandscapeNavigationController alloc]
        initWithRootViewController:[self makePlaceholderController]];

    self.delegate = self;
    self.preferredDisplayMode = UISplitViewControllerDisplayModeOneBesideSecondary;
    self.preferredSplitBehavior = UISplitViewControllerSplitBehaviorTile;
    self.preferredPrimaryColumnWidthFraction = 0.36;
    self.minimumPrimaryColumnWidth = 240.0;
    self.maximumPrimaryColumnWidth = 360.0;
    [self setViewController:primary_nav_ forColumn:UISplitViewControllerColumnPrimary];
    [self setViewController:secondary_nav_ forColumn:UISplitViewControllerColumnSecondary];
  }
  return self;
}

- (void)dealloc {
  [dashboard_ release];
  [primary_nav_ release];
  [secondary_nav_ release];
  [super dealloc];
}

- (XeniaPauseDashboardViewController*)dashboard {
  return dashboard_;
}

- (UIViewController*)makePlaceholderController {
  UIViewController* placeholder = [[[UIViewController alloc] init] autorelease];
  placeholder.view.backgroundColor = [UIColor systemBackgroundColor];
  UILabel* hint = [[[UILabel alloc] init] autorelease];
  hint.translatesAutoresizingMaskIntoConstraints = NO;
  hint.text = @"Select an option";
  hint.textColor = [XeniaTheme textMuted];
  hint.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  [placeholder.view addSubview:hint];
  [NSLayoutConstraint activateConstraints:@[
    [hint.centerXAnchor constraintEqualToAnchor:placeholder.view.centerXAnchor],
    [hint.centerYAnchor constraintEqualToAnchor:placeholder.view.centerYAnchor],
  ]];
  return placeholder;
}

#pragma mark - XeniaPauseDashboardViewControllerDelegate

- (void)pauseDashboardDidSelectResume:(XeniaPauseDashboardViewController*)dashboard {
  [workspace_delegate_ pauseWorkspaceDidSelectResume:self];
}

- (void)pauseDashboardDidSelectExit:(XeniaPauseDashboardViewController*)dashboard {
  [workspace_delegate_ pauseWorkspaceDidSelectExit:self];
}

- (void)pauseDashboard:(XeniaPauseDashboardViewController*)dashboard
      didSelectSection:(XeniaPauseDashboardSection)section {
  UIViewController* content = [workspace_delegate_ pauseWorkspace:self contentForSection:section];
  if (!content) {
    return;
  }
  if (self.isCollapsed) {
    [primary_nav_ pushViewController:content animated:YES];
  } else {
    XeniaLandscapeNavigationController* nav = [[[XeniaLandscapeNavigationController alloc]
        initWithRootViewController:content] autorelease];
    [self setViewController:nav forColumn:UISplitViewControllerColumnSecondary];
    [self showColumn:UISplitViewControllerColumnSecondary];
  }
}

#pragma mark - UISplitViewControllerDelegate

- (UISplitViewControllerColumn)splitViewController:(UISplitViewController*)svc
        topColumnForCollapsingToProposedTopColumn:(UISplitViewControllerColumn)proposedTopColumn {
  // Collapse to the dashboard (sidebar) rather than the detail column so the
  // compact presentation is a single nav stack rooted at the dashboard.
  return UISplitViewControllerColumnPrimary;
}

@end
