/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_debug_settings_view_controller.h"

#include "xenia/ui/ios/settings/ios_config_models.h"

// Search lives in XeniaConfigViewController (it is shared with Game Settings and All Settings);
// this class only fixes the catalog and the live-override defaults of the developer pages.
@implementation XeniaIOSDebugSettingsViewController

- (instancetype)init {
  return [self initWithCatalogKind:IOSConfigCatalogKind::kDebugSettings liveOverride:YES];
}

- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                       liveOverride:(BOOL)liveOverride {
  return [self initWithCatalogKind:catalogKind
                      liveOverride:liveOverride
                       gameTitleID:0
                         gameTitle:nil];
}

- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                       liveOverride:(BOOL)liveOverride
                        gameTitleID:(uint32_t)gameTitleID
                          gameTitle:(NSString*)gameTitle {
  self = [super initWithCatalogKind:catalogKind
                              style:UITableViewStyleInsetGrouped
                        gameTitleID:gameTitleID
                          gameTitle:gameTitle];
  if (self) {
    self.liveOverride = liveOverride;
    self.showsRootDismissButton = NO;
  }
  return self;
}

@end
