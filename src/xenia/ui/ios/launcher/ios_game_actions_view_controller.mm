/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_actions_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

struct XeniaIOSGameActionRow {
  XeniaIOSGameAction action;
  NSString* title;
  NSString* subtitle;
  NSString* symbol;
  BOOL enabled;
};

}  // namespace

@implementation XeniaIOSGameActionsViewController {
  NSString* game_title_;
  uint32_t title_id_;
  BOOL supports_compatibility_;
  BOOL supports_manage_content_;
  BOOL supports_disc_selection_;
  BOOL supports_patches_;
  BOOL supports_zar_conversion_;
  NSArray<NSValue*>* rows_;
  void (^action_handler_)(XeniaIOSGameAction action);
}

@synthesize actionHandler = action_handler_;

- (instancetype)initWithGameTitle:(NSString*)gameTitle
                          titleID:(uint32_t)titleID
            supportsCompatibility:(BOOL)supportsCompatibility
            supportsManageContent:(BOOL)supportsManageContent
            supportsDiscSelection:(BOOL)supportsDiscSelection
                  supportsPatches:(BOOL)supportsPatches
            supportsZarConversion:(BOOL)supportsZarConversion {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    game_title_ = [gameTitle copy];
    title_id_ = titleID;
    supports_compatibility_ = supportsCompatibility;
    supports_manage_content_ = supportsManageContent;
    supports_disc_selection_ = supportsDiscSelection;
    supports_patches_ = supportsPatches;
    supports_zar_conversion_ = supportsZarConversion;
  }
  return self;
}

- (void)dealloc {
  [game_title_ release];
  [rows_ release];
  [action_handler_ release];
  [super dealloc];
}

- (NSArray<NSValue*>*)buildRows {
  XeniaIOSGameActionRow rows[] = {
      {XeniaIOSGameActionPlay,
       NSLocalizedString(@"Play", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Launch the selected title.",
                         @"game actions view controller: user-visible text"),
       @"play.fill", YES},
      {XeniaIOSGameActionSystemLink,
       NSLocalizedString(@"System Link", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Play with others over the internet, in the public lobby or a "
                         @"private room.",
                         @"game actions view controller: user-visible text"),
       @"network", title_id_ != 0},
      {XeniaIOSGameActionGameSettings,
       NSLocalizedString(@"Game Settings", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Save display, compatibility, and cvar overrides for this title.",
                         @"game actions view controller: user-visible text"),
       @"slider.horizontal.3", title_id_ != 0},
      {XeniaIOSGameActionResetGameSettings,
       NSLocalizedString(@"Reset Game Settings",
                         @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Delete saved overrides for this title and return to defaults.",
                         @"game actions view controller: user-visible text"),
       @"arrow.counterclockwise", title_id_ != 0},
      {XeniaIOSGameActionTouchLayout,
       NSLocalizedString(@"Touch Layout", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Edit or assign the touch controls used by this title.",
                         @"game actions view controller: user-visible text"),
       @"hand.tap", title_id_ != 0},
      {XeniaIOSGameActionCompatibility,
       NSLocalizedString(@"Compatibility", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Open compatibility status, reports, and notes.",
                         @"game actions view controller: user-visible text"),
       @"checkmark.shield", supports_compatibility_},
      {XeniaIOSGameActionManageContent,
       NSLocalizedString(@"Manage Content", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Manage installed title updates and related content.",
                         @"game actions view controller: user-visible text"),
       @"square.stack.3d.up", supports_manage_content_},
      {XeniaIOSGameActionLaunchDisc,
       NSLocalizedString(@"Launch Disc", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Pick which disc to start for multi-disc games.",
                         @"game actions view controller: user-visible text"),
       @"opticaldisc", supports_disc_selection_},
      {XeniaIOSGameActionPatches,
       NSLocalizedString(@"Patches", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"View and manage title patches.",
                         @"game actions view controller: user-visible text"),
       @"puzzlepiece.extension", supports_patches_},
      {XeniaIOSGameActionConvertToZar,
       NSLocalizedString(@"Convert to ZAR", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Create a compressed .zar copy in the XeniOS library.",
                         @"game actions view controller: user-visible text"),
       @"opticaldisc", supports_zar_conversion_},
      {XeniaIOSGameActionCopyLaunchURL,
       NSLocalizedString(@"Copy Launch URL", @"game actions view controller: user-visible text"),
       NSLocalizedString(@"Copy the xenia:// URL for this title.",
                         @"game actions view controller: user-visible text"),
       @"link", title_id_ != 0},
  };

  NSMutableArray<NSValue*>* values = [NSMutableArray array];
  for (const XeniaIOSGameActionRow& row : rows) {
    [values addObject:[NSValue valueWithBytes:&row objCType:@encode(XeniaIOSGameActionRow)]];
  }
  return values;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Game Actions", @"game actions view controller: title");
  if (game_title_.length) {
    self.navigationItem.prompt = game_title_;
  }
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  rows_ = [[self buildRows] retain];
  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                               target:self
                                       action:@selector(cancelTapped:)] autorelease];
}

- (void)cancelTapped:(id)sender {
  (void)sender;
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (XeniaIOSGameActionRow)rowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaIOSGameActionRow row = {};
  if (indexPath.section != 0 || indexPath.row < 0 ||
      indexPath.row >= static_cast<NSInteger>(rows_.count)) {
    return row;
  }
  [rows_[indexPath.row] getValue:&row];
  return row;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? static_cast<NSInteger>(rows_.count) : 0;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaIOSGameActionCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor secondarySystemGroupedBackgroundColor];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    cell.textLabel.numberOfLines = 1;
    cell.detailTextLabel.numberOfLines = 2;
  }

  XeniaIOSGameActionRow row = [self rowAtIndexPath:indexPath];
  cell.textLabel.text = row.title;
  cell.detailTextLabel.text = row.subtitle;
  cell.imageView.image = [UIImage systemImageNamed:row.symbol];
  cell.userInteractionEnabled = row.enabled;
  cell.accessoryType =
      row.enabled ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
  cell.textLabel.textColor = row.enabled ? [XeniaTheme textPrimary] : [XeniaTheme textMuted];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.imageView.tintColor = row.enabled ? [XeniaTheme accent] : [XeniaTheme textMuted];
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  XeniaIOSGameActionRow row = [self rowAtIndexPath:indexPath];
  if (!row.enabled || !action_handler_) {
    return;
  }
  action_handler_(row.action);
}

@end
