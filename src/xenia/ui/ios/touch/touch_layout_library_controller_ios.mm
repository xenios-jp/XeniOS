/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_layout_library_controller_ios.h"

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

@interface XeniaTouchLayoutLibraryTableController ()
- (XeniaTouchLayoutLibraryItem*)layoutItemAtIndexPath:(NSIndexPath*)indexPath;
@end

@implementation XeniaTouchLayoutLibraryTableController {
  NSArray<XeniaTouchLayoutLibraryItem*>* items_;
  NSString* current_layout_local_id_;
  XeniaTouchLayoutLibraryFilter filter_;
}

@synthesize loadHandler = loadHandler_;
@synthesize renameLayoutHandler = renameLayoutHandler_;
@synthesize deleteLayoutHandler = deleteLayoutHandler_;
@synthesize exportLayoutHandler = exportLayoutHandler_;
@synthesize setTitleDefaultHandler = setTitleDefaultHandler_;
@synthesize setGlobalDefaultHandler = setGlobalDefaultHandler_;
@synthesize favoriteLayoutHandler = favoriteLayoutHandler_;

- (void)dealloc {
  [favoriteLayoutHandler_ release];
  [setGlobalDefaultHandler_ release];
  [setTitleDefaultHandler_ release];
  [exportLayoutHandler_ release];
  [deleteLayoutHandler_ release];
  [renameLayoutHandler_ release];
  [loadHandler_ release];
  [current_layout_local_id_ release];
  [items_ release];
  [super dealloc];
}

- (void)setItems:(NSArray<XeniaTouchLayoutLibraryItem*>*)items
    currentLayoutLocalID:(NSString*)currentLayoutLocalID {
  [items_ release];
  items_ = [items copy];
  [current_layout_local_id_ release];
  current_layout_local_id_ = [currentLayoutLocalID copy];
}

- (void)setFilter:(XeniaTouchLayoutLibraryFilter)filter {
  filter_ = filter;
}

- (BOOL)itemMatchesCurrentFilter:(XeniaTouchLayoutLibraryItem*)item {
  switch (filter_) {
    case XeniaTouchLayoutLibraryFilterOfficial:
      return item.official;
    case XeniaTouchLayoutLibraryFilterSaved:
      return !item.official;
    case XeniaTouchLayoutLibraryFilterFavorites:
      return item.isFavorite || item.isDefaultForCurrentTitle || item.isDefaultForAllGames;
  }
  return YES;
}

- (XeniaTouchLayoutLibraryItem*)currentLayoutItem {
  if (!current_layout_local_id_.length) {
    return nil;
  }
  for (XeniaTouchLayoutLibraryItem* item in items_) {
    if ([item.localID isEqualToString:current_layout_local_id_]) {
      return item;
    }
  }
  return nil;
}

- (NSArray<XeniaTouchLayoutLibraryItem*>*)filteredLayoutItems {
  NSMutableArray<XeniaTouchLayoutLibraryItem*>* filtered = [NSMutableArray array];
  for (XeniaTouchLayoutLibraryItem* item in items_) {
    if (current_layout_local_id_.length &&
        [item.localID isEqualToString:current_layout_local_id_]) {
      continue;
    }
    if ([self itemMatchesCurrentFilter:item]) {
      [filtered addObject:item];
    }
  }
  return filtered;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  if (section != 0) {
    return 0;
  }
  return static_cast<NSInteger>(([self currentLayoutItem] ? 1 : 0) +
                                [self filteredLayoutItems].count);
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  (void)section;
  return nil;
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  (void)section;
  return nil;
}

- (CGFloat)tableView:(UITableView* __unused)tableView
    heightForHeaderInSection:(NSInteger)__unused section {
  return 0.01f;
}

- (CGFloat)tableView:(UITableView* __unused)tableView
    heightForFooterInSection:(NSInteger)__unused section {
  return 0.01f;
}

- (UIView*)tableView:(UITableView*)tableView viewForHeaderInSection:(NSInteger)section {
  (void)tableView;
  (void)section;
  return nil;
}

- (void)configureCell:(XeniaTouchLayoutLibraryRowCell*)cell
              forItem:(XeniaTouchLayoutLibraryItem*)item {
  cell.overlayTitleLabel.text = item.displayName ?: item.localID;
  NSMutableArray<NSString*>* subtitle_parts = [NSMutableArray array];
  [subtitle_parts addObject:(item.official ? @"Official preset" : @"Saved local copy")];
  if (item.author.length > 0) {
    [subtitle_parts addObject:item.author];
  }
  if (item.isDefaultForCurrentTitle) {
    [subtitle_parts addObject:@"Default for this game"];
  }
  if (item.isDefaultForAllGames) {
    [subtitle_parts addObject:@"Default for all games"];
  }
  if (item.isFavorite) {
    [subtitle_parts addObject:@"Favorite"];
  }
  if (current_layout_local_id_.length &&
      [item.localID isEqualToString:current_layout_local_id_]) {
    [subtitle_parts addObject:@"Active"];
  }
  cell.overlaySubtitleLabel.text = [subtitle_parts componentsJoinedByString:@" • "];
  [cell setShowsCheckmark:[item.localID isEqualToString:current_layout_local_id_]];
  [cell setThumbnailImage:item.thumbnail];
  [cell setShowsDefaultBadge:item.isDefaultForCurrentTitle ||
                             item.isDefaultForAllGames || item.isFavorite];
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kTouchLayoutLibraryCellIdentifier = @"XeniaTouchLayoutLibraryCell";
  XeniaTouchLayoutLibraryRowCell* cell =
      [tableView dequeueReusableCellWithIdentifier:kTouchLayoutLibraryCellIdentifier];
  if (!cell) {
    cell = [[[XeniaTouchLayoutLibraryRowCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                                  reuseIdentifier:kTouchLayoutLibraryCellIdentifier]
        autorelease];
  }

  cell.overlayTitleLabel.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.96];
  cell.overlaySubtitleLabel.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.64];
  cell.overlayTitleLabel.text = @"";
  cell.overlaySubtitleLabel.text = @"";
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  [cell setShowsDisclosure:NO];
  [cell setShowsCheckmark:NO];
  [cell setThumbnailImage:nil];
  [cell setShowsDefaultBadge:NO];

  XeniaTouchLayoutLibraryItem* item = [self layoutItemAtIndexPath:indexPath];
  if (item) {
    [self configureCell:cell forItem:item];
  }

  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];

  XeniaTouchLayoutLibraryItem* item = [self layoutItemAtIndexPath:indexPath];
  if (!item || !loadHandler_) {
    return;
  }
  [current_layout_local_id_ release];
  current_layout_local_id_ = [item.localID copy];
  loadHandler_(item.localID);
  [tableView reloadData];
}

- (XeniaTouchLayoutLibraryItem*)layoutItemAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.row < 0) {
    return nil;
  }
  if (indexPath.section != 0) {
    return nil;
  }
  NSInteger filtered_row = indexPath.row;
  XeniaTouchLayoutLibraryItem* active_item = [self currentLayoutItem];
  if (active_item) {
    if (filtered_row == 0) {
      return active_item;
    }
    --filtered_row;
  }
  NSArray<XeniaTouchLayoutLibraryItem*>* filtered = [self filteredLayoutItems];
  if (filtered_row >= static_cast<NSInteger>(filtered.count)) {
    return nil;
  }
  return [filtered objectAtIndex:filtered_row];
}

- (UISwipeActionsConfiguration*)tableView:(UITableView*)__unused tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaTouchLayoutLibraryItem* item = [self layoutItemAtIndexPath:indexPath];
  if (!item) {
    return nil;
  }

  NSString* local_id = [[item.localID copy] autorelease];
  const BOOL official = item.official;
  XeniaTouchLayoutLibraryTableController* controller = self;
  NSMutableArray<UIContextualAction*>* actions = [NSMutableArray array];
  UIContextualAction* export_action = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleNormal
                           title:@"Export"
                         handler:^(__unused UIContextualAction* action,
                                   __unused UIView* source_view,
                                   void (^completion_handler)(BOOL)) {
                           if (controller->exportLayoutHandler_) {
                             controller->exportLayoutHandler_(local_id);
                           }
                           completion_handler(YES);
                         }];
  export_action.backgroundColor = [XeniaTheme touchTintSky];
  [actions addObject:export_action];

  if (!official) {
    UIContextualAction* rename_action = [UIContextualAction
        contextualActionWithStyle:UIContextualActionStyleNormal
                             title:@"Rename"
                           handler:^(__unused UIContextualAction* action,
                                     __unused UIView* source_view,
                                     void (^completion_handler)(BOOL)) {
                             if (controller->renameLayoutHandler_) {
                               controller->renameLayoutHandler_(local_id);
                             }
                             completion_handler(YES);
                           }];
    rename_action.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:0.32];
    [actions addObject:rename_action];

    UIContextualAction* delete_action = [UIContextualAction
        contextualActionWithStyle:UIContextualActionStyleDestructive
                             title:@"Delete"
                           handler:^(__unused UIContextualAction* action,
                                     __unused UIView* source_view,
                                     void (^completion_handler)(BOOL)) {
                             if (controller->deleteLayoutHandler_) {
                               controller->deleteLayoutHandler_(local_id);
                             }
                             completion_handler(YES);
                           }];
    delete_action.backgroundColor = [XeniaTheme statusError];
    [actions addObject:delete_action];
  }

  UISwipeActionsConfiguration* configuration =
      [UISwipeActionsConfiguration configurationWithActions:actions];
  configuration.performsFirstActionWithFullSwipe = NO;
  return configuration;
}

- (UISwipeActionsConfiguration*)tableView:(UITableView*)__unused tableView
    leadingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaTouchLayoutLibraryItem* item = [self layoutItemAtIndexPath:indexPath];
  if (!item) {
    return nil;
  }

  NSString* local_id = [[item.localID copy] autorelease];
  const BOOL favorite = item.isFavorite;
  XeniaTouchLayoutLibraryTableController* controller = self;
  UIContextualAction* favorite_action = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleNormal
                           title:(favorite ? @"Unfavorite" : @"Favorite")
                         handler:^(__unused UIContextualAction* action,
                                   __unused UIView* source_view,
                                   void (^completion_handler)(BOOL)) {
                           if (controller->favoriteLayoutHandler_) {
                             controller->favoriteLayoutHandler_(local_id, !favorite);
                           }
                           completion_handler(YES);
                         }];
  favorite_action.backgroundColor =
      favorite ? [[UIColor whiteColor] colorWithAlphaComponent:0.32] : [XeniaTheme touchTintAmber];

  UIContextualAction* default_action = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleNormal
                           title:@"All Games"
                         handler:^(__unused UIContextualAction* action,
                                   __unused UIView* source_view,
                                   void (^completion_handler)(BOOL)) {
                           if (controller->setGlobalDefaultHandler_) {
                             controller->setGlobalDefaultHandler_(local_id);
                           }
                           completion_handler(YES);
                         }];
  default_action.backgroundColor = [XeniaTheme touchTintMint];

  UISwipeActionsConfiguration* configuration =
      [UISwipeActionsConfiguration configurationWithActions:@[ favorite_action, default_action ]];
  configuration.performsFirstActionWithFullSwipe = NO;
  return configuration;
}

- (UIContextMenuConfiguration*)tableView:(UITableView*)__unused tableView
    contextMenuConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath
                                        point:(CGPoint)__unused point {
  if (@available(iOS 13.0, *)) {
    XeniaTouchLayoutLibraryItem* item = [self layoutItemAtIndexPath:indexPath];
    if (!item) {
      return nil;
    }

    NSString* local_id = [[item.localID copy] autorelease];
    const BOOL official = item.official;
    const BOOL favorite = item.isFavorite;
    XeniaTouchLayoutLibraryTableController* controller = self;
    return [UIContextMenuConfiguration
        configurationWithIdentifier:nil
                     previewProvider:nil
                      actionProvider:^UIMenu*(
                          __unused NSArray<UIMenuElement*>* suggested_actions) {
                        NSMutableArray<UIMenuElement*>* actions = [NSMutableArray array];
                        [actions addObject:[UIAction
                                               actionWithTitle:@"Use for This Game"
                                                         image:[UIImage systemImageNamed:
                                                                           @"checkmark.circle"]
                                                    identifier:nil
                                                       handler:^(__unused UIAction* action) {
                                                         if (controller->setTitleDefaultHandler_) {
                                                           controller->setTitleDefaultHandler_(
                                                               local_id);
                                                         }
                                                       }]];
                        [actions addObject:[UIAction
                                               actionWithTitle:@"Use for All Games"
                                                         image:[UIImage systemImageNamed:
                                                                           @"star.circle"]
                                                    identifier:nil
                                                       handler:^(__unused UIAction* action) {
                                                         if (controller->setGlobalDefaultHandler_) {
                                                           controller->setGlobalDefaultHandler_(
                                                               local_id);
                                                         }
                                                       }]];
                        [actions addObject:[UIAction
                                               actionWithTitle:(favorite ? @"Unfavorite"
                                                                         : @"Favorite")
                                                         image:[UIImage systemImageNamed:@"star"]
                                                    identifier:nil
                                                       handler:^(__unused UIAction* action) {
                                                         if (controller->favoriteLayoutHandler_) {
                                                           controller->favoriteLayoutHandler_(
                                                               local_id, !favorite);
                                                         }
                                                       }]];
                        [actions addObject:[UIAction
                                               actionWithTitle:@"Export"
                                                         image:[UIImage systemImageNamed:
                                                                           @"square.and.arrow.up"]
                                                    identifier:nil
                                                       handler:^(__unused UIAction* action) {
                                                         if (controller->exportLayoutHandler_) {
                                                           controller->exportLayoutHandler_(
                                                               local_id);
                                                         }
                                                       }]];
                        if (!official) {
                          [actions addObject:[UIAction
                                                 actionWithTitle:@"Rename"
                                                           image:[UIImage systemImageNamed:@"pencil"]
                                                      identifier:nil
                                                         handler:^(__unused UIAction* action) {
                                                           if (controller->renameLayoutHandler_) {
                                                             controller->renameLayoutHandler_(
                                                                 local_id);
                                                           }
                                                         }]];
                          UIAction* delete_action = [UIAction
                              actionWithTitle:@"Delete"
                                        image:[UIImage systemImageNamed:@"trash"]
                                   identifier:nil
                                      handler:^(__unused UIAction* action) {
                                        if (controller->deleteLayoutHandler_) {
                                          controller->deleteLayoutHandler_(local_id);
                                        }
                                      }];
                          delete_action.attributes = UIMenuElementAttributesDestructive;
                          [actions addObject:delete_action];
                        }
                        return [UIMenu menuWithTitle:@"" children:actions];
                      }];
  }
  return nil;
}

@end
