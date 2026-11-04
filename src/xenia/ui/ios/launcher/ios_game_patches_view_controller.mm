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
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

// Patch files are written by hand: names arrive with trailing periods and lowercase starts.
NSString* PatchDisplayName(const std::string& name) {
  NSString* text = [ToNSString(name)
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  while ([text hasSuffix:@"."] && ![text hasSuffix:@"..."]) {
    text = [text substringToIndex:text.length - 1];
  }
  if (text.length) {
    text = [[[text substringToIndex:1] uppercaseString]
        stringByAppendingString:[text substringFromIndex:1]];
  }
  return text;
}

// The description in secondary text, the author quieter under it.
NSAttributedString* PatchDetailText(const xe::ui::IOSPatchEntrySummary& patch) {
  NSMutableAttributedString* text = [[[NSMutableAttributedString alloc] init] autorelease];
  if (!patch.description.empty()) {
    [text appendAttributedString:
              [[[NSAttributedString alloc]
                  initWithString:ToNSString(patch.description)
                      attributes:@{
                        NSFontAttributeName : xe_scaled_system_font(UIFontTextStyleSubheadline,
                                                                    14.0, UIFontWeightRegular),
                        NSForegroundColorAttributeName : [XeniaTheme textSecondary],
                      }] autorelease]];
  }
  if (!patch.author.empty()) {
    NSString* author = [NSString
        stringWithFormat:NSLocalizedString(@"by %@",
                                           @"game patches view controller: user-visible text"),
                         ToNSString(patch.author)];
    [text appendAttributedString:
              [[[NSAttributedString alloc]
                  initWithString:[NSString stringWithFormat:@"%@%@", text.length ? @"\n" : @"",
                                                            author]
                      attributes:@{
                        NSFontAttributeName : xe_scaled_system_font(UIFontTextStyleFootnote, 13.0,
                                                                    UIFontWeightRegular),
                        NSForegroundColorAttributeName : [XeniaTheme textMuted],
                      }] autorelease]];
  }
  return text;
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
      initWithBarButtonSystemItem:UIBarButtonSystemItemClose
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
  [self setNeedsUpdateContentUnavailableConfiguration];
}

- (void)updateContentUnavailableConfigurationUsingState:(UIContentUnavailableConfigurationState*)
                                                            __unused state {
  if (!patch_files_.empty()) {
    self.contentUnavailableConfiguration = nil;
    return;
  }
  UIContentUnavailableConfiguration* empty = [UIContentUnavailableConfiguration emptyConfiguration];
  empty.image = [UIImage systemImageNamed:@"puzzlepiece.extension"];
  empty.text = NSLocalizedString(@"No Patches", @"game patches view controller: empty title");
  NSString* folder = ToNSString(discovery_summary_.directory_path);
  if (!discovery_summary_.directory_exists ||
      (discovery_summary_.candidate_files == 0 && discovery_summary_.bundled_files == 0)) {
    empty.secondaryText = [NSString
        stringWithFormat:NSLocalizedString(@"Add .patch.toml files for this game to %@.",
                                           @"game patches view controller: empty detail; %@ is "
                                           @"the folder"),
                         folder];
  } else {
    empty.secondaryText = [NSString
        stringWithFormat:NSLocalizedString(@"None of the patch files is for title ID %08X.",
                                           @"game patches view controller: empty detail"),
                         title_id_];
  }
  self.contentUnavailableConfiguration = empty;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return static_cast<NSInteger>(patch_files_.size());
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  const auto& file = patch_files_[static_cast<size_t>(section)];
  return file.patches.empty() ? 1 : static_cast<NSInteger>(file.patches.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  // The sheet is already about one game; a header only tells several patch files apart.
  if (patch_files_.size() < 2) {
    return nil;
  }
  const auto& file = patch_files_[static_cast<size_t>(section)];
  if (!file.display_name.empty()) {
    return ToNSString(file.display_name);
  }
  return ToNSString(file.filename);
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
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
          : PatchDisplayName(patch.name);
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.detailTextLabel.attributedText = PatchDetailText(patch);
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
