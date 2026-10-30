/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_disc_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

NSString* DetailForDisc(const xe::ui::IOSDiscoveredGame::Disc& disc) {
  NSString* filename = ToNSString(disc.path.filename().string());
  NSString* source = nil;
  if (!disc.source_label.empty()) {
    source = ToNSString(disc.source_label);
  } else if (disc.has_imported_source && disc.has_external_source) {
    source = @"Imported + External";
  } else {
    source = disc.is_external || disc.has_external_source ? @"External" : @"Imported";
  }
  if (disc.media_id) {
    return [NSString stringWithFormat:@"%@ - %@ - Media %08X", source, filename, disc.media_id];
  }
  return [NSString stringWithFormat:@"%@ - %@", source, filename];
}

}  // namespace

@implementation XeniaGameDiscViewController {
  NSString* title_;
  std::vector<xe::ui::IOSDiscoveredGame::Disc> discs_;
  void (^selection_handler_)(NSString* path, NSString* label);
}

- (instancetype)initWithTitle:(NSString*)title
                        discs:
                            (const std::vector<xe::ui::IOSDiscoveredGame::Disc>&)discs
             selectionHandler:(void (^)(NSString* path, NSString* label))selectionHandler {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    title_ = [title copy];
    discs_ = discs;
    selection_handler_ = [selectionHandler copy];
    self.title = @"Launch Disc";
  }
  return self;
}

- (void)dealloc {
  [title_ release];
  [selection_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                     target:self
                                                     action:@selector(doneTapped:)] autorelease];
}

- (void)doneTapped:(id)__unused sender {
  if (self.navigationController.presentingViewController &&
      self.navigationController.viewControllers.firstObject == self) {
    [self.navigationController dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  [self.navigationController popViewControllerAnimated:YES];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
 numberOfRowsInSection:(NSInteger)__unused section {
  return discs_.empty() ? 1 : static_cast<NSInteger>(discs_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)__unused section {
  return title_.length ? title_ : @"Game";
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger)__unused section {
  if (discs_.empty()) {
    return @"No alternate discs were found for this title.";
  }
  return @"Choose the disc to boot. Saves, title updates, and installed content "
         @"remain shared by title ID.";
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kDiscCellIdentifier = @"XeniaGameDiscCell";
  UITableViewCell* cell =
      [tableView dequeueReusableCellWithIdentifier:kDiscCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kDiscCellIdentifier] autorelease];
  }

  if (discs_.empty()) {
    cell.textLabel.text = @"No discs available";
    cell.detailTextLabel.text = nil;
    cell.textLabel.textColor = [XeniaTheme textMuted];
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }

  const auto& disc = discs_[static_cast<size_t>(indexPath.row)];
  cell.textLabel.text = ToNSString(disc.label);
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = DetailForDisc(disc);
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.imageView.image = [UIImage systemImageNamed:@"opticaldisc"];
  cell.imageView.tintColor = self.view.tintColor;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.accessibilityLabel = cell.textLabel.text;
  cell.accessibilityValue = cell.detailTextLabel.text;
  cell.accessibilityHint = @"Launches this disc.";
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  return cell;
}

- (void)tableView:(UITableView*)tableView
    didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (discs_.empty() || static_cast<size_t>(indexPath.row) >= discs_.size()) {
    return;
  }
  const auto& disc = discs_[static_cast<size_t>(indexPath.row)];
  NSString* path = [ToNSString(disc.path.string()) copy];
  NSString* label = [ToNSString(disc.label) copy];
  void (^handler)(NSString*, NSString*) = [selection_handler_ copy];
  [self.navigationController dismissViewControllerAnimated:YES
                                                completion:^{
                                                  if (handler) {
                                                    handler(path, label);
                                                  }
                                                  [handler release];
                                                  [path release];
                                                  [label release];
                                                }];
}

@end
