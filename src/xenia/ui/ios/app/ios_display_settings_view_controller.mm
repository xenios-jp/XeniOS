/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_display_settings_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

namespace {
// Plain-language relabel of the three scaling modes (see proposal §5). The third
// column is the underlying XeniaIOSWindowScalingMode.
struct ScreenModeOption {
  XeniaIOSWindowScalingMode mode;
  __unsafe_unretained NSString* title;
  __unsafe_unretained NSString* subtitle;
};
const ScreenModeOption kScreenModes[] = {
    {XeniaIOSWindowScalingModeFit, @"Fit", @"Preserve aspect ratio"},
    {XeniaIOSWindowScalingModeZoom, @"Fill", @"Crop edges to fill the screen"},
    {XeniaIOSWindowScalingModeStretch, @"Stretch", @"Ignore aspect ratio"},
};
constexpr NSInteger kSectionScreenMode = 0;
constexpr NSInteger kSectionOptions = 1;
}  // namespace

@implementation XeniaDisplaySettingsViewController {
  id<XeniaDisplaySettingsHost> host_;  // assign
}

- (instancetype)initWithHost:(id<XeniaDisplaySettingsHost>)host {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    host_ = host;
  }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"Display";
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 56.0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return 2;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  if (section == kSectionScreenMode) {
    return (NSInteger)(sizeof(kScreenModes) / sizeof(kScreenModes[0]));
  }
  return 2;  // Letterbox, Uncapped.
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  return section == kSectionScreenMode ? @"Screen Mode" : @"Options";
}

- (UISwitch*)switchWithOn:(BOOL)on action:(SEL)action {
  UISwitch* control = [[[UISwitch alloc] init] autorelease];
  control.on = on;
  control.onTintColor = [XeniaTheme accent];
  [control addTarget:self action:action forControlEvents:UIControlEventValueChanged];
  return control;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == kSectionScreenMode) {
    static NSString* const kModeCell = @"XeniaDisplayModeCell";
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kModeCell];
    if (!cell) {
      cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                     reuseIdentifier:kModeCell] autorelease];
    }
    const ScreenModeOption& option = kScreenModes[(size_t)indexPath.row];
    cell.textLabel.text = option.title;
    cell.textLabel.textColor = [XeniaTheme textPrimary];
    cell.detailTextLabel.text = option.subtitle;
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    const BOOL selected = host_ && [host_ currentWindowScalingMode] == option.mode;
    cell.accessoryType =
        selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.tintColor = [XeniaTheme accent];
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = option.title;
    cell.accessibilityValue = selected ? @"Selected" : nil;
    cell.accessibilityTraits = UIAccessibilityTraitButton;
    return cell;
  }

  static NSString* const kToggleCell = @"XeniaDisplayToggleCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kToggleCell];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kToggleCell] autorelease];
  }
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  if (indexPath.row == 0) {
    const BOOL letterbox_available =
        host_ && [host_ currentWindowScalingMode] != XeniaIOSWindowScalingModeStretch;
    cell.textLabel.text = @"Letterbox";
    cell.textLabel.enabled = letterbox_available;
    UISwitch* letterbox_switch =
        [self switchWithOn:(letterbox_available && [host_ isPresentLetterboxEnabled])
                    action:@selector(letterboxChanged:)];
    letterbox_switch.enabled = letterbox_available;
    cell.accessoryView = letterbox_switch;
  } else {
    cell.textLabel.text = @"Uncapped Emulated Display";
    cell.textLabel.enabled = YES;
    cell.accessoryView = [self switchWithOn:(host_ && [host_ isGuestDisplayUncapped])
                                     action:@selector(uncappedChanged:)];
  }
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section != kSectionScreenMode) {
    return;
  }
  [host_ setCurrentWindowScalingMode:kScreenModes[(size_t)indexPath.row].mode];
  [tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, 2)]
           withRowAnimation:UITableViewRowAnimationAutomatic];
}

- (void)letterboxChanged:(UISwitch*)sender {
  [host_ setPresentLetterboxEnabled:sender.on];
  sender.on = host_ && [host_ isPresentLetterboxEnabled];
}

- (void)uncappedChanged:(UISwitch*)sender {
  [host_ setGuestDisplayUncapped:sender.on];
}

@end
