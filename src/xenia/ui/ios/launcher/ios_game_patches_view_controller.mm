/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_patches_view_controller.h"

#include <string>
#include <vector>

#import "xenia/ui/ios/app/windowed_app_context_ios.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

NSString* PatchDetailText(const xe::ui::IOSPatchEntrySummary& patch) {
  NSMutableArray<NSString*>* parts = [NSMutableArray arrayWithCapacity:2];
  if (!patch.description.empty()) {
    [parts addObject:ToNSString(patch.description)];
  }
  if (!patch.author.empty()) {
    [parts addObject:[NSString
                         stringWithFormat:NSLocalizedString(
                                              @"by %@",
                                              @"game patches view controller: user-visible text"),
                                          ToNSString(patch.author)]];
  }
  return [parts componentsJoinedByString:@"\n"];
}

}  // namespace

@implementation XeniaGamePatchesViewController {
  uint32_t title_id_;
  NSString* game_title_;
  xe::ui::IOSWindowedAppContext* app_context_;
  std::vector<xe::ui::IOSPatchFileSummary> patch_files_;
  xe::ui::IOSPatchDiscoverySummary discovery_summary_;
}

- (instancetype)initWithTitleID:(uint32_t)titleID
                          title:(NSString*)title
                     appContext:(xe::ui::IOSWindowedAppContext*)appContext {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    title_id_ = titleID;
    game_title_ = [title copy];
    app_context_ = appContext;
    self.title = NSLocalizedString(@"Patches", @"game patches view controller: title");
  }
  return self;
}

- (void)dealloc {
  [game_title_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 76.0;
  self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
      initWithTitle:NSLocalizedString(@"Close", @"game patches view controller: title")
              style:UIBarButtonItemStylePlain
             target:self
             action:@selector(doneTapped:)] autorelease];
  [self reloadPatches];
}

- (void)doneTapped:(id)__unused sender {
  if (self.navigationController.presentingViewController &&
      self.navigationController.viewControllers.firstObject == self) {
    [self.navigationController dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  [self.navigationController popViewControllerAnimated:YES];
}

- (void)reloadPatches {
  patch_files_ = app_context_ ? app_context_->ListPatchFiles(title_id_)
                              : std::vector<xe::ui::IOSPatchFileSummary>();
  discovery_summary_ = app_context_ ? app_context_->GetPatchDiscoverySummary(title_id_)
                                    : xe::ui::IOSPatchDiscoverySummary();
  [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return patch_files_.empty() ? 1 : static_cast<NSInteger>(patch_files_.size());
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  if (patch_files_.empty()) {
    return 1;
  }
  const auto& file = patch_files_[static_cast<size_t>(section)];
  return file.patches.empty() ? 1 : static_cast<NSInteger>(file.patches.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  if (patch_files_.empty()) {
    return game_title_.length ? game_title_
                              : NSLocalizedString(@"Game", @"game patches view controller: title");
  }
  const auto& file = patch_files_[static_cast<size_t>(section)];
  if (!file.title_name.empty()) {
    return ToNSString(file.title_name);
  }
  if (!file.display_name.empty()) {
    return ToNSString(file.display_name);
  }
  return ToNSString(file.filename);
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (patch_files_.empty()) {
    if (!discovery_summary_.directory_exists) {
      return NSLocalizedString(@"The patches folder was not found.",
                               @"game patches view controller: user-visible text");
    }
    if (discovery_summary_.candidate_files == 0 && discovery_summary_.bundled_files == 0) {
      return NSLocalizedString(@"The patches folder did not contain any .patch.toml files.",
                               @"game patches view controller: user-visible text");
    }
    return NSLocalizedString(@"Patch files were found, but none matched this title ID.",
                             @"game patches view controller: user-visible text");
  }
  if (section == static_cast<NSInteger>(patch_files_.size()) - 1) {
    return NSLocalizedString(@"Patch changes are saved immediately and take effect the next time "
                             @"the game launches.",
                             @"game patches view controller: user-visible text");
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kPatchCellIdentifier = @"XeniaGamePatchCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kPatchCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kPatchCellIdentifier] autorelease];
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
  }

  if (patch_files_.empty()) {
    cell.textLabel.text =
        NSLocalizedString(@"No patches available", @"game patches view controller: label text");
    if (!discovery_summary_.directory_exists) {
      cell.detailTextLabel.text = [NSString
          stringWithFormat:NSLocalizedString(@"Create a patches folder here:\n%@",
                                             @"game patches view controller: detail text"),
                           ToNSString(discovery_summary_.directory_path)];
    } else if (discovery_summary_.candidate_files == 0 && discovery_summary_.bundled_files == 0) {
      cell.detailTextLabel.text = [NSString
          stringWithFormat:NSLocalizedString(@"Scanned %zu files in:\n%@\n\nAdd xenia-canary "
                                             @".patch.toml files for this title.",
                                             @"game patches view controller: detail text"),
                           discovery_summary_.scanned_files,
                           ToNSString(discovery_summary_.directory_path)];
    } else {
      cell.detailTextLabel.text = [NSString
          stringWithFormat:NSLocalizedString(@"Title ID %08X did not match any discovered patch "
                                             @"file.\n\nFolder: %@\nPatch-looking files: %zu\n"
                                             @"Parse failures: %zu\nOther title IDs: %zu",
                                             @"game patches view controller: detail text"),
                           title_id_, ToNSString(discovery_summary_.directory_path),
                           discovery_summary_.candidate_files, discovery_summary_.parse_failures,
                           discovery_summary_.title_mismatches];
    }
    cell.textLabel.textColor = [XeniaTheme textMuted];
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    cell.accessoryView = nil;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }

  const auto& file = patch_files_[static_cast<size_t>(indexPath.section)];
  if (file.patches.empty()) {
    cell.textLabel.text =
        NSLocalizedString(@"No patch entries", @"game patches view controller: label text");
    cell.detailTextLabel.text = NSLocalizedString(@"This patch file did not contain any toggles.",
                                                  @"game patches view controller: detail text");
    cell.textLabel.textColor = [XeniaTheme textMuted];
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    cell.accessoryView = nil;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }

  const auto& patch = file.patches[static_cast<size_t>(indexPath.row)];
  cell.textLabel.text =
      patch.name.empty()
          ? [NSString
                stringWithFormat:NSLocalizedString(@"Patch %zu",
                                                   @"game patches view controller: label text"),
                                 patch.patch_index + 1]
          : ToNSString(patch.name);
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.text = PatchDetailText(patch);
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;

  UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
  toggle.on = patch.is_enabled;
  [toggle addTarget:self
                action:@selector(patchSwitchChanged:)
      forControlEvents:UIControlEventValueChanged];
  cell.accessoryView = toggle;
  cell.accessibilityLabel = cell.textLabel.text;
  cell.accessibilityValue =
      toggle.on
          ? NSLocalizedString(@"Enabled", @"game patches view controller: accessibility value")
          : NSLocalizedString(@"Disabled", @"game patches view controller: accessibility value");
  cell.accessibilityHint = NSLocalizedString(@"Toggles this patch for the next launch.",
                                             @"game patches view controller: accessibility hint");
  return cell;
}

- (void)patchSwitchChanged:(UISwitch*)sender {
  // The switch belongs to whichever row it currently sits in.
  CGPoint point = [sender convertPoint:CGPointMake(CGRectGetMidX(sender.bounds),
                                                   CGRectGetMidY(sender.bounds))
                                toView:self.tableView];
  NSIndexPath* index_path = [self.tableView indexPathForRowAtPoint:point];
  if (!index_path) {
    return;
  }
  NSInteger section = index_path.section;
  NSInteger row = index_path.row;
  if (section < 0 || row < 0 || section >= static_cast<NSInteger>(patch_files_.size())) {
    return;
  }
  auto& file = patch_files_[static_cast<size_t>(section)];
  if (row >= static_cast<NSInteger>(file.patches.size())) {
    return;
  }
  const auto& patch = file.patches[static_cast<size_t>(row)];
  std::string status;
  bool success =
      app_context_ && app_context_->SetPatchEnabled(title_id_, file.filename, patch.patch_index,
                                                    sender.on, &status);
  if (!success) {
    sender.on = !sender.on;
    XEPresentOKAlert(
        self,
        NSLocalizedString(@"Patch Not Saved", @"game patches view controller: user-visible text"),
        status.empty() ? NSLocalizedString(@"Failed to update patch state.",
                                           @"game patches view controller: user-visible text")
                       : ToNSString(status));
    return;
  }
  [self reloadPatches];
}

@end
