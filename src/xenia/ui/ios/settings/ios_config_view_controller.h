/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CONFIG_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_CONFIG_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

#include "xenia/ui/ios/settings/ios_config_models.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

// Settings sheet shown from the launcher and from the in-game overlay. Each
// row maps to either a Xenia cvar or an iOS NSUserDefaults key; rows are
// constructed by ios_config_builder, and Save persists every dirty row back
// through ApplyIOSConfigSections.
@interface XeniaConfigViewController : XESheetTableViewController
@property(nonatomic, copy) void (^dismissalHandler)(void);
@property(nonatomic, assign) BOOL showsRootDismissButton;
@property(nonatomic, readonly) IOSConfigCatalogKind catalogKind;
@property(nonatomic, readonly) uint32_t gameTitleID;
@property(nonatomic, copy, readonly) NSString* gameTitle;
@property(nonatomic, assign) BOOL liveOverride;

- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind style:(UITableViewStyle)style;
- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                              style:(UITableViewStyle)style
                        gameTitleID:(uint32_t)gameTitleID
                          gameTitle:(NSString*)gameTitle;

// Replaces the section list and reloads the table. Intended for subclasses
// that need dynamic filtering (e.g. search controllers).
- (void)replaceSections:(std::vector<IOSConfigSection>)newSections;

// Subclass hooks for dynamic catalogs and filtered save surfaces.
- (std::vector<IOSConfigSection>)buildSections;
- (std::vector<IOSConfigSection>)sectionsForSaving;
@end

#endif  // XENIA_UI_IOS_CONFIG_VIEW_CONTROLLER_H_
