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

#include "xenia/ui/ios/app/windowed_app_context_ios.h"
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
// The game is an original Xbox game, which starts through the Xbox emulator: its settings offer
// the build to use. Set before the page is shown.
@property(nonatomic, assign) BOOL originalXbox;
@property(nonatomic, assign) BOOL liveOverride;
// A game is running, so saved changes take effect only after a relaunch. Pushed pages inherit it.
@property(nonatomic, assign) BOOL gameRunning;
// Lets live overrides and resets run with the game paused (ApplyLiveSettings). Not retained.
// Nil when no game can be running; pushed controllers inherit it.
@property(nonatomic, assign) xe::ui::IOSWindowedAppContext* appContext;
// Row to scroll to and highlight when the page first appears (a search result): the setting's
// key, or the title of a row that has no key (an action). Set before the page is shown.
@property(nonatomic, copy) NSString* focusKey;

- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind style:(UITableViewStyle)style;
- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                              style:(UITableViewStyle)style
                        gameTitleID:(uint32_t)gameTitleID
                          gameTitle:(NSString*)gameTitle;

// Replaces the section list and reloads the table. Intended for subclasses
// that need dynamic filtering. Pages that search (Game Settings, All Settings, the developer
// pages) filter themselves; Save always writes the unfiltered list.
- (void)replaceSections:(std::vector<IOSConfigSection>)newSections;

// Subclass hooks for dynamic catalogs and filtered save surfaces.
- (std::vector<IOSConfigSection>)buildSections;
- (std::vector<IOSConfigSection>)sectionsForSaving;
@end

// A settings-style trailing value for a table cell's accessoryView: grey text, truncated at
// |max_width|, optionally followed by a disclosure chevron. Autoreleased; the cell stays the
// VoiceOver element.
UIView* XeniaConfigMakeValueAccessoryView(NSString* value, CGFloat max_width, BOOL with_chevron);

#endif  // XENIA_UI_IOS_CONFIG_VIEW_CONTROLLER_H_
