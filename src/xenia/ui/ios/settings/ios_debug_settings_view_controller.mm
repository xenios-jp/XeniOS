/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_debug_settings_view_controller.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "xenia/ui/ios/settings/ios_config_catalog.h"
#include "xenia/ui/ios/settings/ios_config_models.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

std::string Lowercase(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

}  // namespace

@implementation XeniaIOSDebugSettingsViewController {
  std::vector<IOSConfigSection> full_sections_;
  UISearchController* search_controller_;
}

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

- (std::vector<IOSConfigSection>)buildSections {
  full_sections_ = [super buildSections];
  return full_sections_;
}

- (std::vector<IOSConfigSection>)sectionsForSaving {
  return full_sections_;
}

- (void)viewDidLoad {
  [super viewDidLoad];

  search_controller_ = [[UISearchController alloc] initWithSearchResultsController:nil];
  search_controller_.searchResultsUpdater = self;
  search_controller_.obscuresBackgroundDuringPresentation = NO;
  search_controller_.hidesNavigationBarDuringPresentation = YES;
  self.navigationItem.searchController = search_controller_;
  self.navigationItem.hidesSearchBarWhenScrolling = NO;
  self.definesPresentationContext = YES;
}

- (void)dealloc {
  [search_controller_ release];
  [super dealloc];
}

- (void)configItemDidChange:(const IOSConfigItem*)item {
  if (!item) {
    return;
  }
  for (auto& section : full_sections_) {
    for (auto& full_item : section.items) {
      if (full_item.key == item->key) {
        full_item = *item;
        return;
      }
    }
  }
}

#pragma mark - UISearchResultsUpdating

- (void)updateSearchResultsForSearchController:(UISearchController*)searchController {
  NSString* query = searchController.searchBar.text;
  if (query.length == 0) {
    [self replaceSections:full_sections_];
    return;
  }

  std::string lower = Lowercase([query UTF8String]);
  IOSConfigSection results;
  results.title = "Results";
  for (const auto& section : full_sections_) {
    for (const auto& item : section.items) {
      if (Lowercase(item.key).find(lower) != std::string::npos ||
          Lowercase(item.title).find(lower) != std::string::npos ||
          Lowercase(item.subtitle).find(lower) != std::string::npos) {
        results.items.push_back(item);
      }
    }
  }
  [self replaceSections:{results}];
}

@end
