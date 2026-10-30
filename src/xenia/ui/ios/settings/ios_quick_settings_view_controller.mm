/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_quick_settings_view_controller.h"

#include "xenia/ui/ios/settings/ios_config_models.h"
#import "xenia/ui/ios/settings/ios_debug_settings_view_controller.h"
#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaIOSQuickSettingsViewController

- (instancetype)init {
  return [self initWithGameTitleID:0 gameTitle:nil];
}

- (instancetype)initWithGameTitleID:(uint32_t)gameTitleID gameTitle:(NSString*)gameTitle {
  self = [super initWithCatalogKind:IOSConfigCatalogKind::kGraphicsCompat
                              style:UITableViewStyleInsetGrouped
                        gameTitleID:gameTitleID
                          gameTitle:gameTitle];
  if (self) {
    self.liveOverride = YES;
    self.showsRootDismissButton = YES;
  }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];

  UIView* footer = [[[UIView alloc] initWithFrame:CGRectMake(0, 0, 1, 56)] autorelease];
  footer.backgroundColor = [UIColor clearColor];

  UIButton* debugButton = [UIButton buttonWithType:UIButtonTypeSystem];
  debugButton.translatesAutoresizingMaskIntoConstraints = NO;
  [debugButton setTitle:@"Show Advanced Debug" forState:UIControlStateNormal];
  debugButton.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  [debugButton addTarget:self
                  action:@selector(showDebugSettings)
        forControlEvents:UIControlEventTouchUpInside];
  [footer addSubview:debugButton];

  [NSLayoutConstraint activateConstraints:@[
    [debugButton.topAnchor constraintEqualToAnchor:footer.topAnchor constant:8.0],
    [debugButton.centerXAnchor constraintEqualToAnchor:footer.centerXAnchor],
    [debugButton.bottomAnchor constraintEqualToAnchor:footer.bottomAnchor constant:-8.0],
  ]];

  self.tableView.tableFooterView = footer;
}

- (void)showDebugSettings {
  XeniaIOSDebugSettingsViewController* debugVC = [[XeniaIOSDebugSettingsViewController alloc]
      initWithCatalogKind:IOSConfigCatalogKind::kDebugSettings
             liveOverride:YES
              gameTitleID:self.gameTitleID
                gameTitle:self.gameTitle];
  [self.navigationController pushViewController:debugVC animated:YES];
  [debugVC release];
}

@end
