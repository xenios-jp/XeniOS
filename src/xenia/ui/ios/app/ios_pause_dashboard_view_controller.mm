/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_pause_dashboard_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

typedef NS_ENUM(NSInteger, XeniaPauseDashboardRowId) {
  XeniaPauseDashboardRowResume = 0,
  XeniaPauseDashboardRowDisplayMode,
  XeniaPauseDashboardRowTouchControls,
  XeniaPauseDashboardRowPerformanceOverlay,
  XeniaPauseDashboardRowAchievements,
  XeniaPauseDashboardRowProfile,
  XeniaPauseDashboardRowGraphics,
  XeniaPauseDashboardRowDiagnostics,
  XeniaPauseDashboardRowExit,
};

@implementation XeniaPauseDashboardViewController {
  NSArray<NSDictionary*>* sections_;
  NSString* game_title_;
  NSString* achievements_summary_;
  NSString* profile_name_;
  NSString* display_mode_value_;
  NSString* touch_controls_value_;
  NSString* performance_overlay_value_;
  id<XeniaPauseDashboardViewControllerDelegate> delegate_;  // assign (no retain)
}

@synthesize delegate = delegate_;
@synthesize gameTitle = game_title_;
@synthesize achievementsSummary = achievements_summary_;
@synthesize profileName = profile_name_;
@synthesize displayModeValue = display_mode_value_;
@synthesize touchControlsValue = touch_controls_value_;
@synthesize performanceOverlayValue = performance_overlay_value_;

- (instancetype)init {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    [self buildSections];
  }
  return self;
}

- (void)dealloc {
  [sections_ release];
  [game_title_ release];
  [achievements_summary_ release];
  [profile_name_ release];
  [display_mode_value_ release];
  [touch_controls_value_ release];
  [performance_overlay_value_ release];
  [super dealloc];
}

- (void)buildSections {
  // Static layout. Quick rows are shortcuts into the matching destination, so
  // there is one canonical entry point per area (Display via Display Mode,
  // Controls via Touch Controls, Achievements in Game, Graphics/Diagnostics in
  // More) — no duplicated category like the old grid had.
  NSArray<NSDictionary*>* sections = @[
    @{@"rows" : @[ @(XeniaPauseDashboardRowResume) ]},
    @{
      @"header" : @"Quick Settings",
      @"rows" : @[
        @(XeniaPauseDashboardRowDisplayMode),
        @(XeniaPauseDashboardRowTouchControls),
        @(XeniaPauseDashboardRowPerformanceOverlay),
      ]
    },
    @{
      @"header" : @"Game",
      @"rows" : @[ @(XeniaPauseDashboardRowAchievements), @(XeniaPauseDashboardRowProfile) ]
    },
    @{
      @"header" : @"More",
      @"rows" : @[ @(XeniaPauseDashboardRowGraphics), @(XeniaPauseDashboardRowDiagnostics) ]
    },
    @{@"rows" : @[ @(XeniaPauseDashboardRowExit) ]},
  ];
  [sections_ release];
  sections_ = [sections retain];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = game_title_.length ? game_title_ : @"Game Menu";
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 52.0;
  if (@available(iOS 15.0, *)) {
    self.tableView.sectionHeaderTopPadding = 0;
  }
}

- (void)reloadDashboard {
  self.title = game_title_.length ? game_title_ : @"Game Menu";
  [self.tableView reloadData];
}

- (XeniaPauseDashboardRowId)rowIdForIndexPath:(NSIndexPath*)indexPath {
  NSArray<NSNumber*>* rows = sections_[(NSUInteger)indexPath.section][@"rows"];
  return (XeniaPauseDashboardRowId)rows[(NSUInteger)indexPath.row].integerValue;
}

#pragma mark - Cell configuration

- (void)configureValueCell:(UITableViewCell*)cell
                     title:(NSString*)title
                    symbol:(NSString*)symbol
                     value:(NSString*)value
                disclosure:(BOOL)disclosure
                selectable:(BOOL)selectable {
  cell.textLabel.text = title;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  cell.detailTextLabel.text = value;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  cell.imageView.image = symbol.length ? [UIImage systemImageNamed:symbol] : nil;
  cell.imageView.tintColor = [XeniaTheme accent];
  cell.accessoryType =
      disclosure ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
  cell.selectionStyle =
      selectable ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
  cell.isAccessibilityElement = YES;
  cell.accessibilityLabel = title;
  cell.accessibilityValue = value;
  cell.accessibilityTraits =
      selectable ? UIAccessibilityTraitButton : UIAccessibilityTraitStaticText;
}

- (void)configureActionCell:(UITableViewCell*)cell
                      title:(NSString*)title
                     symbol:(NSString*)symbol
                 accentFill:(BOOL)accentFill
                destructive:(BOOL)destructive {
  cell.textLabel.text = title;
  cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  cell.imageView.image = symbol.length ? [UIImage systemImageNamed:symbol] : nil;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;

  UIBackgroundConfiguration* background =
      [UIBackgroundConfiguration listGroupedCellConfiguration];
  if (accentFill) {
    background.backgroundColor = [XeniaTheme accent];
    cell.textLabel.textColor = [XeniaTheme accentFg];
    cell.imageView.tintColor = [XeniaTheme accentFg];
  } else {
    UIColor* tint = destructive ? [XeniaTheme statusError] : [XeniaTheme textPrimary];
    cell.textLabel.textColor = tint;
    cell.imageView.tintColor = tint;
  }
  cell.backgroundConfiguration = background;

  cell.isAccessibilityElement = YES;
  cell.accessibilityLabel = title;
  cell.accessibilityTraits = UIAccessibilityTraitButton;
}

#pragma mark - UITableViewDataSource

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return (NSInteger)sections_.count;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  NSArray* rows = sections_[(NSUInteger)section][@"rows"];
  return (NSInteger)rows.count;
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  return sections_[(NSUInteger)section][@"header"];
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  const XeniaPauseDashboardRowId row = [self rowIdForIndexPath:indexPath];
  const BOOL is_action =
      (row == XeniaPauseDashboardRowResume || row == XeniaPauseDashboardRowExit);
  NSString* const reuse_id = is_action ? @"XeniaPauseActionCell" : @"XeniaPauseValueCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:reuse_id];
  if (!cell) {
    UITableViewCellStyle style =
        is_action ? UITableViewCellStyleDefault : UITableViewCellStyleValue1;
    cell = [[[UITableViewCell alloc] initWithStyle:style
                                   reuseIdentifier:reuse_id] autorelease];
  }

  switch (row) {
    case XeniaPauseDashboardRowResume:
      [self configureActionCell:cell
                          title:@"Resume Game"
                         symbol:@"play.fill"
                     accentFill:YES
                    destructive:NO];
      break;
    case XeniaPauseDashboardRowExit:
      [self configureActionCell:cell
                          title:@"Exit to Library"
                         symbol:@"rectangle.portrait.and.arrow.right"
                     accentFill:NO
                    destructive:YES];
      break;
    case XeniaPauseDashboardRowDisplayMode:
      [self configureValueCell:cell
                         title:@"Display Mode"
                        symbol:@"rectangle.expand.vertical"
                         value:display_mode_value_
                    disclosure:YES
                    selectable:YES];
      break;
    case XeniaPauseDashboardRowTouchControls:
      [self configureValueCell:cell
                         title:@"Touch Controls"
                        symbol:@"hand.tap"
                         value:touch_controls_value_
                    disclosure:YES
                    selectable:YES];
      break;
    case XeniaPauseDashboardRowPerformanceOverlay:
      [self configureValueCell:cell
                         title:@"Performance Overlay"
                        symbol:@"speedometer"
                         value:performance_overlay_value_
                    disclosure:YES
                    selectable:YES];
      break;
    case XeniaPauseDashboardRowAchievements:
      [self configureValueCell:cell
                         title:@"Achievements"
                        symbol:@"trophy"
                         value:achievements_summary_
                    disclosure:YES
                    selectable:YES];
      break;
    case XeniaPauseDashboardRowProfile:
      [self configureValueCell:cell
                         title:@"Profile"
                        symbol:@"person.crop.circle"
                         value:profile_name_
                    disclosure:NO
                    selectable:NO];
      break;
    case XeniaPauseDashboardRowGraphics:
      [self configureValueCell:cell
                         title:@"Graphics"
                        symbol:@"slider.horizontal.3"
                         value:nil
                    disclosure:YES
                    selectable:YES];
      break;
    case XeniaPauseDashboardRowDiagnostics:
      [self configureValueCell:cell
                         title:@"Diagnostics"
                        symbol:@"waveform.path.ecg"
                         value:nil
                    disclosure:YES
                    selectable:YES];
      break;
  }
  return cell;
}

#pragma mark - UITableViewDelegate

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  switch ([self rowIdForIndexPath:indexPath]) {
    case XeniaPauseDashboardRowResume:
      [delegate_ pauseDashboardDidSelectResume:self];
      break;
    case XeniaPauseDashboardRowExit:
      [delegate_ pauseDashboardDidSelectExit:self];
      break;
    case XeniaPauseDashboardRowDisplayMode:
      [delegate_ pauseDashboard:self didSelectSection:XeniaPauseDashboardSectionDisplay];
      break;
    case XeniaPauseDashboardRowTouchControls:
      [delegate_ pauseDashboard:self didSelectSection:XeniaPauseDashboardSectionControls];
      break;
    case XeniaPauseDashboardRowPerformanceOverlay:
      [delegate_ pauseDashboard:self didSelectSection:XeniaPauseDashboardSectionDiagnostics];
      break;
    case XeniaPauseDashboardRowAchievements:
      [delegate_ pauseDashboard:self didSelectSection:XeniaPauseDashboardSectionAchievements];
      break;
    case XeniaPauseDashboardRowGraphics:
      [delegate_ pauseDashboard:self didSelectSection:XeniaPauseDashboardSectionGraphics];
      break;
    case XeniaPauseDashboardRowDiagnostics:
      [delegate_ pauseDashboard:self didSelectSection:XeniaPauseDashboardSectionDiagnostics];
      break;
    case XeniaPauseDashboardRowProfile:
      break;
  }
}

@end
