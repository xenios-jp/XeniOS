/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_display_settings_view_controller.h"

#import "xenia/ui/ios/game/ios_shader_compile_indicator.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {
// Plain-language relabel of the three scaling modes (see proposal §5). The third
// column is the underlying XeniaIOSWindowScalingMode.
struct ScreenModeOption {
  XeniaIOSWindowScalingMode mode;
};
const ScreenModeOption kScreenModes[] = {
    {XeniaIOSWindowScalingModeFit},
    {XeniaIOSWindowScalingModeZoom},
    {XeniaIOSWindowScalingModeStretch},
};
// Looked up on use so the strings follow the current locale and are never held
// unretained.
NSString* ScreenModeTitle(XeniaIOSWindowScalingMode mode) {
  switch (mode) {
    case XeniaIOSWindowScalingModeFit:
      return NSLocalizedString(@"Fit", @"Screen mode option: keep the aspect ratio");
    case XeniaIOSWindowScalingModeZoom:
      return NSLocalizedString(@"Fill", @"Screen mode option: crop to fill the screen");
    default:
      return NSLocalizedString(@"Stretch", @"Screen mode option: ignore the aspect ratio");
  }
}
NSString* ScreenModeSubtitle(XeniaIOSWindowScalingMode mode) {
  switch (mode) {
    case XeniaIOSWindowScalingModeFit:
      return NSLocalizedString(@"Preserve aspect ratio", @"Subtitle of the Fit screen mode");
    case XeniaIOSWindowScalingModeZoom:
      return NSLocalizedString(@"Crop edges to fill the screen",
                               @"Subtitle of the Fill screen mode");
    default:
      return NSLocalizedString(@"Ignore aspect ratio", @"Subtitle of the Stretch screen mode");
  }
}
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
  self.title = NSLocalizedString(@"Display", @"Title of the display settings screen");
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
  return 3;  // Letterbox, Uncapped, Shader Compile Status.
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  return section == kSectionScreenMode
             ? NSLocalizedString(@"Screen Mode", @"Section header in display settings")
             : NSLocalizedString(@"Options", @"Section header in display settings");
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
  return section == kSectionOptions
             ? NSLocalizedString(@"Shader Compile Status shows when the game is building shaders, "
                                 @"which causes stutter and missing effects until it finishes.",
                                 @"Footer under the display options")
             : nil;
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
    cell.textLabel.text = ScreenModeTitle(option.mode);
    cell.textLabel.textColor = [XeniaTheme textPrimary];
    cell.detailTextLabel.text = ScreenModeSubtitle(option.mode);
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    const BOOL selected = host_ && [host_ currentWindowScalingMode] == option.mode;
    cell.accessoryType =
        selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.tintColor = [XeniaTheme accent];
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = ScreenModeTitle(option.mode);
    cell.accessibilityValue =
        selected ? NSLocalizedString(@"Selected", @"VoiceOver value for the selected screen mode")
                 : nil;
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
    cell.textLabel.text = NSLocalizedString(@"Letterbox", @"Display settings toggle");
    cell.textLabel.enabled = letterbox_available;
    UISwitch* letterbox_switch =
        [self switchWithOn:(letterbox_available && [host_ isPresentLetterboxEnabled])
                    action:@selector(letterboxChanged:)];
    letterbox_switch.enabled = letterbox_available;
    cell.accessoryView = letterbox_switch;
  } else if (indexPath.row == 1) {
    cell.textLabel.text =
        NSLocalizedString(@"Uncapped Emulated Display", @"Display settings toggle");
    cell.textLabel.enabled = YES;
    cell.accessoryView = [self switchWithOn:(host_ && [host_ isGuestDisplayUncapped])
                                     action:@selector(uncappedChanged:)];
  } else {
    cell.textLabel.text = NSLocalizedString(@"Shader Compile Status", @"Display settings toggle");
    cell.textLabel.enabled = YES;
    cell.accessoryView = [self switchWithOn:xe_ios_shader_compile_status_enabled()
                                     action:@selector(shaderCompileStatusChanged:)];
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

- (void)shaderCompileStatusChanged:(UISwitch*)sender {
  xe_ios_set_shader_compile_status_enabled(sender.on);
}

@end
