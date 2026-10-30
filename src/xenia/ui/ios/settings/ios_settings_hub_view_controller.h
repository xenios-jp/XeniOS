/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SETTINGS_HUB_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_SETTINGS_HUB_VIEW_CONTROLLER_H_

#include "xenia/ui/ios/app/windowed_app_context_ios.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

typedef void (^XeniaSettingsStatusHandler)(NSString* status_message);

typedef NS_ENUM(NSInteger, XeniaSettingsInitialSection) {
  XeniaSettingsInitialSectionMain,
  XeniaSettingsInitialSectionProfile,
};

typedef NS_ENUM(NSInteger, XeniaSettingsHubAction) {
  XeniaSettingsHubActionNone,
  XeniaSettingsHubActionOpenTouchLayoutLibrary,
  XeniaSettingsHubActionChooseGameTouchLayout,
  XeniaSettingsHubActionEditTouchControls,
  XeniaSettingsHubActionImportTouchLayout,
  XeniaSettingsHubActionExportTouchLayout,
  XeniaSettingsHubActionResetTouchLayout,
  XeniaSettingsHubActionImportGame,
  XeniaSettingsHubActionRefreshLibrary,
  XeniaSettingsHubActionClearCurrentGameShaderCache,
  XeniaSettingsHubActionClearAllShaderCaches,
};

typedef void (^XeniaSettingsHubActionHandler)(XeniaSettingsHubAction action);

@class XeniaSettingsHubViewController;

@protocol XeniaSettingsHubViewControllerDelegate <NSObject>
- (void)settingsHubViewController:(XeniaSettingsHubViewController*)hub
              didSelectController:(UIViewController*)controller;
@end

// Settings category list used as the single-column iPhone settings root and as
// the primary column of the iPad settings workspace. It creates detail
// controllers through typed app-context callbacks, so the root app controller
// only needs to present the hub/workspace.
@interface XeniaSettingsHubViewController : XESheetTableViewController
@property(nonatomic, assign) id<XeniaSettingsHubViewControllerDelegate> selectionDelegate;
@property(nonatomic, copy) void (^dismissalHandler)(void);
@property(nonatomic, copy) XeniaSettingsHubActionHandler actionHandler;
@property(nonatomic, assign) BOOL showsCloseButton;

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status;
- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status
                    initialSection:(XeniaSettingsInitialSection)initial_section;

// Returns an autoreleased detail controller for the default workspace
// selection.
- (UIViewController*)makeInitialDetailController;
@end

// iPad-oriented settings container. The hub lives in the primary column and
// selected details appear in the secondary column. Compact presentations can
// still use XeniaSettingsHubViewController directly for a native push stack.
@interface XeniaSettingsWorkspaceViewController : UISplitViewController
@property(nonatomic, copy) void (^dismissalHandler)(void);
@property(nonatomic, copy) XeniaSettingsHubActionHandler actionHandler;

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status;
- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(XeniaSettingsStatusHandler)on_status
                    initialSection:(XeniaSettingsInitialSection)initial_section;
@end

#endif  // XENIA_UI_IOS_SETTINGS_HUB_VIEW_CONTROLLER_H_
