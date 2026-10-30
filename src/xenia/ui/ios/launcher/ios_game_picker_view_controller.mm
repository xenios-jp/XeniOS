/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_picker_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaIOSGamePickerItem

+ (instancetype)itemWithTitle:(NSString*)title
                     subtitle:(NSString*)subtitle
                      titleID:(uint32_t)titleID
                    gameIndex:(NSUInteger)gameIndex {
  XeniaIOSGamePickerItem* item = [[[XeniaIOSGamePickerItem alloc] init] autorelease];
  item.title = title;
  item.subtitle = subtitle;
  item.titleID = titleID;
  item.gameIndex = gameIndex;
  return item;
}

- (void)dealloc {
  [_title release];
  [_subtitle release];
  [super dealloc];
}

@end

@implementation XeniaIOSGamePickerViewController {
  NSString* title_;
  NSString* prompt_;
  NSArray<XeniaIOSGamePickerItem*>* items_;
  void (^selection_handler_)(NSUInteger gameIndex);
}

@synthesize selectionHandler = selection_handler_;

- (instancetype)initWithTitle:(NSString*)title
                       prompt:(NSString*)prompt
                        items:(NSArray<XeniaIOSGamePickerItem*>*)items {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    title_ = [title copy];
    prompt_ = [prompt copy];
    items_ = [items copy];
  }
  return self;
}

- (void)dealloc {
  [title_ release];
  [prompt_ release];
  [items_ release];
  [selection_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = title_;
  self.navigationItem.prompt = prompt_;
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 72.0;
  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                     target:self
                                                     action:@selector(cancelTapped:)] autorelease];
}

- (void)cancelTapped:(id)sender {
  (void)sender;
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? static_cast<NSInteger>(items_.count) : 0;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaIOSGamePickerCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor secondarySystemGroupedBackgroundColor];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    cell.textLabel.numberOfLines = 2;
    cell.detailTextLabel.numberOfLines = 2;
  }

  XeniaIOSGamePickerItem* item = [items_ objectAtIndex:indexPath.row];
  cell.textLabel.text = item.title;
  cell.detailTextLabel.text = item.subtitle;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.imageView.image = [UIImage systemImageNamed:@"gamecontroller.fill"];
  cell.imageView.tintColor = [XeniaTheme accent];
  XEApplyAccessibility(cell, item.title, item.subtitle, @"Opens this game's touch layout editor.",
                       UIAccessibilityTraitButton);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (!selection_handler_) {
    return;
  }
  XeniaIOSGamePickerItem* item = [items_ objectAtIndex:indexPath.row];
  selection_handler_(item.gameIndex);
}

@end
