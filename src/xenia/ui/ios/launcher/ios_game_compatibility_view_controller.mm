/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_compatibility_view_controller.h"

#include <cmath>
#include <cstdint>

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_compat_report_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_compatibility_discussion_controller.h"
#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

// Reports show this many, newest first; the last row opens them all on GitHub.
constexpr NSUInteger kLatestReportCount = 5;

NSString* StringIn(NSDictionary* dict, NSString* key) {
  id value = dict[key];
  return [value isKindOfClass:[NSString class]] && [value length] ? value : nil;
}

NSString* ReportDate(NSDictionary* report) {
  NSString* date = StringIn(report, @"date");
  return date.length >= 10 ? xe_format_iso_date(date) : date;
}

NSString* ReportDevice(NSDictionary* report) {
  NSString* machine = StringIn(report, @"deviceMachine") ?: StringIn(report, @"device");
  return machine ? xe_device_display_name_for_machine(machine) : nil;
}

NSString* GPUName(NSString* backend) {
  NSString* key = backend.lowercaseString;
  if ([key isEqualToString:@"msl"] || [key isEqualToString:@"metal"]) {
    return @"Metal";
  }
  if ([key isEqualToString:@"vulkan"] || [key isEqualToString:@"spirv"]) {
    return @"Vulkan";
  }
  return backend.uppercaseString;
}

NSString* RunsText(NSString* perf) {
  if ([perf isEqualToString:@"great"]) {
    return NSLocalizedString(@"Runs great", @"compatibility: performance");
  }
  if ([perf isEqualToString:@"ok"]) {
    return NSLocalizedString(@"Runs okay", @"compatibility: performance");
  }
  if ([perf isEqualToString:@"poor"]) {
    return NSLocalizedString(@"Runs poorly", @"compatibility: performance");
  }
  return nil;
}

// A report's first line, as its row title.
NSString* ReportHeadline(NSDictionary* report) {
  NSString* notes = [StringIn(report, @"notes")
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (!notes.length) {
    return NSLocalizedString(@"No details given", @"compatibility: report without notes");
  }
  return [notes componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]]
      .firstObject;
}

// "Playable · iPhone 17 · iOS 26.2 · Jun 5".
NSString* ReportDetail(NSDictionary* report) {
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  NSString* status = StringIn(report, @"status");
  if (status) {
    [parts addObject:xe_compat_status_label(status)];
  }
  if (NSString* device = ReportDevice(report)) {
    [parts addObject:device];
  }
  NSString* platform = xe_platform_display_text(report[@"platform"], report[@"osVersion"]);
  if (platform.length) {
    [parts addObject:platform];
  }
  if (NSString* date = ReportDate(report)) {
    [parts addObject:date];
  }
  return [parts componentsJoinedByString:@" · "];
}

}  // namespace

// One report in full: what they wrote, then what they played it on.
@interface XeniaCompatReportPage : XESheetTableViewController
- (instancetype)initWithReport:(NSDictionary*)report issueURL:(NSString*)issue_url;
@end

@implementation XeniaCompatReportPage {
  NSDictionary* report_;
  NSString* issue_url_;
  NSArray<NSArray<NSString*>*>* facts_;
}

- (instancetype)initWithReport:(NSDictionary*)report issueURL:(NSString*)issue_url {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
    report_ = [report retain];
    issue_url_ = [issue_url copy];
    self.title = NSLocalizedString(@"Report", @"compatibility: one report's page title");
    NSMutableArray<NSArray<NSString*>*>* facts = [NSMutableArray array];
    void (^add)(NSString*, NSString*) = ^(NSString* name, NSString* value) {
      if (value.length) {
        [facts addObject:@[ name, value ]];
      }
    };
    NSString* status = StringIn(report, @"status");
    add(NSLocalizedString(@"Status", @"compatibility report"),
        status ? xe_compat_status_label(status) : nil);
    add(NSLocalizedString(@"Performance", @"compatibility report"),
        RunsText(StringIn(report, @"perf")));
    add(NSLocalizedString(@"Device", @"compatibility report"), ReportDevice(report));
    add(NSLocalizedString(@"System", @"compatibility report"),
        xe_platform_display_text(report[@"platform"], report[@"osVersion"]));
    NSString* gpu = StringIn(report, @"gpuBackend");
    add(NSLocalizedString(@"Graphics", @"compatibility report"), gpu ? GPUName(gpu) : nil);
    add(NSLocalizedString(@"From", @"compatibility report: who sent it"),
        StringIn(report, @"submittedBy"));
    add(NSLocalizedString(@"Date", @"compatibility report"), ReportDate(report));
    facts_ = [facts copy];
  }
  return self;
}

- (void)dealloc {
  [report_ release];
  [issue_url_ release];
  [facts_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 52.0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)table_view {
  return issue_url_.length ? 3 : 2;
}

- (NSInteger)tableView:(UITableView* __unused)table_view numberOfRowsInSection:(NSInteger)section {
  return section == 1 ? (NSInteger)facts_.count : 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)table_view
        cellForRowAtIndexPath:(NSIndexPath*)index_path {
  if (index_path.section == 0) {
    UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                    reuseIdentifier:nil] autorelease];
    NSString* notes = StringIn(report_, @"notes");
    cell.textLabel.text = notes ?: NSLocalizedString(@"No details given",
                                                     @"compatibility: report without notes");
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.textColor = notes ? [XeniaTheme textPrimary] : [XeniaTheme textSecondary];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }
  if (index_path.section == 1) {
    UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                                    reuseIdentifier:nil] autorelease];
    cell.textLabel.text = facts_[index_path.row][0];
    cell.detailTextLabel.text = facts_[index_path.row][1];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.textLabel.text = NSLocalizedString(@"Open on GitHub", @"compatibility report");
  cell.accessoryView = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"arrow.up.forward.square"]] autorelease];
  cell.accessoryView.tintColor = [XeniaTheme textSecondary];
  return cell;
}

- (void)tableView:(UITableView*)table_view didSelectRowAtIndexPath:(NSIndexPath*)index_path {
  [table_view deselectRowAtIndexPath:index_path animated:YES];
  if (index_path.section == 2) {
    NSURL* url = [NSURL URLWithString:issue_url_];
    if (url) {
      [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
    }
  }
}

@end

@interface XeniaGameCompatibilityViewController () <
    XeniaGameCompatibilityDiscussionControllerDelegate>
@end

// Compatibility (docs/ios_design_language.md, "Compatibility"): the verdict as the headline,
// one row per report, the last row opens them all on GitHub, and one white button.
@implementation XeniaGameCompatibilityViewController {
  uint32_t title_id_;
  NSString* game_title_;
  NSDictionary* compat_info_;
  XeniaGameCompatibilityDiscussionController* discussion_controller_;
  XEStatusHeaderView* header_;
}

- (instancetype)initWithTitleID:(uint32_t)title_id
                          title:(NSString*)title
                     compatData:(NSDictionary*)compat_data {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.glassStyle = YES;
    title_id_ = title_id;
    game_title_ = [title copy];
    compat_info_ = [compat_data retain];
    discussion_controller_ = [[XeniaGameCompatibilityDiscussionController alloc]
        initWithTitleID:title_id
             compatInfo:compat_data];
    self.title = NSLocalizedString(@"Compatibility", @"game compatibility view controller: title");
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  discussion_controller_.delegate = nil;
  [game_title_ release];
  [compat_info_ release];
  [discussion_controller_ release];
  [header_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 60.0;
  XESetSheetClose(self, self, @selector(doneTapped:));
  XESetFormCommit(self,
                  NSLocalizedString(@"Submit a Report", @"compatibility: the one white button"),
                  self, @selector(submitReportTapped:))
      .enabled = YES;

  header_ = [[XEStatusHeaderView alloc] init];
  [self refreshHeader];

  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onDiscussionDidUpdate:)
                                               name:kXeniaDiscussionDidUpdateNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(onCompatDataDidUpdate:)
                                               name:kXeniaCompatDataDidUpdateNotification
                                             object:nil];
  discussion_controller_.delegate = self;
  [discussion_controller_ loadFromCompatibilityData];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [header_ fitAsHeaderOfTable:self.tableView];
}

- (NSDictionary*)summary {
  return xe_preferred_summary_from_compat_info(compat_info_) ?: [discussion_controller_ latestReport];
}

// "● Playable" over "Runs okay · 12 reports · Updated Oct 8".
- (void)refreshHeader {
  NSDictionary* summary = [self summary];
  NSString* status = xe_string_from_object(summary[@"status"]);
  const BOOL tested = status.length && ![status isEqualToString:@"untested"];
  header_.titleLabel.text =
      tested ? xe_compat_status_label(status)
             : NSLocalizedString(@"Not Tested", @"compatibility: headline without reports");
  header_.dot.backgroundColor = tested ? xe_compat_status_color(status) : [XeniaTheme textMuted];
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  if (NSString* runs = RunsText(xe_string_from_object(summary[@"perf"]))) {
    [parts addObject:runs];
  }
  const NSUInteger count = discussion_controller_.reports.count;
  if (count) {
    [parts addObject:count == 1
                         ? NSLocalizedString(@"1 report", @"compatibility: header")
                         : [NSString localizedStringWithFormat:
                                         NSLocalizedString(@"%lu reports",
                                                           @"compatibility: header"),
                                         (unsigned long)count]];
  }
  NSString* updated = StringIn(compat_info_, @"updatedAt");
  if (updated) {
    [parts addObject:[NSString stringWithFormat:NSLocalizedString(@"Updated %@",
                                                                  @"compatibility: header; %@ "
                                                                  @"is a date"),
                                                xe_format_iso_date(updated)]];
  }
  header_.detailLabel.text = parts.count
                           ? [parts componentsJoinedByString:@" · "]
                           : NSLocalizedString(@"Nobody has reported on this game yet.",
                                               @"compatibility: header");
  [self.view setNeedsLayout];
}

- (void)doneTapped:(id)__unused sender {
  if (self.navigationController.presentingViewController &&
      self.navigationController.viewControllers.firstObject == self) {
    [self.navigationController dismissViewControllerAnimated:YES completion:nil];
    return;
  }
  [self.navigationController popViewControllerAnimated:YES];
}

- (void)submitReportTapped:(id)__unused sender {
  XeniaCompatReportViewController* report_controller =
      [[XeniaCompatReportViewController alloc] initWithTitleID:title_id_ title:game_title_];
  [self.navigationController pushViewController:report_controller animated:YES];
  [report_controller release];
}

- (void)onCompatDataDidUpdate:(NSNotification*)notification {
  NSNumber* updated_title_id = notification.userInfo[@"titleId"];
  if (![updated_title_id isKindOfClass:[NSNumber class]] ||
      [updated_title_id unsignedIntValue] != title_id_) {
    return;
  }
  NSDictionary* next_info = xe_dictionary_from_object(notification.userInfo[@"compatInfo"]);
  if (!next_info) {
    NSDictionary* cached_by_title_id = xe_load_cached_compat_data();
    next_info = xe_dictionary_from_object(cached_by_title_id[XEFormatTitleIDHexUpper(title_id_)]);
  }
  if (!next_info) {
    return;
  }
  [compat_info_ release];
  compat_info_ = [next_info retain];
  [discussion_controller_ setCompatInfo:next_info];
  [self refreshHeader];
  [self.tableView reloadData];
}

- (void)onDiscussionDidUpdate:(NSNotification*)notification {
  [discussion_controller_ handleDiscussionNotification:notification];
}

- (void)compatibilityDiscussionControllerDidUpdate:
    (XeniaGameCompatibilityDiscussionController*)__unused controller {
  [self refreshHeader];
  [self.tableView reloadData];
}

#pragma mark - Table

- (NSArray<NSDictionary*>*)latestReports {
  NSArray<NSDictionary*>* reports = discussion_controller_.reports;
  return reports.count > kLatestReportCount
             ? [reports subarrayWithRange:NSMakeRange(0, kLatestReportCount)]
             : reports;
}

- (BOOL)hasAllReportsRow {
  return discussion_controller_.issueURL.length > 0 && discussion_controller_.reports.count > 0;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)table_view {
  return 1;
}

- (NSString*)tableView:(UITableView* __unused)table_view
    titleForHeaderInSection:(NSInteger __unused)section {
  return NSLocalizedString(@"Latest reports", @"compatibility: section");
}

- (NSInteger)tableView:(UITableView* __unused)table_view
    numberOfRowsInSection:(NSInteger __unused)section {
  const NSUInteger reports = [self latestReports].count;
  return reports ? (NSInteger)reports + ([self hasAllReportsRow] ? 1 : 0) : 1;
}

- (UITableViewCell*)tableView:(UITableView* __unused)table_view
        cellForRowAtIndexPath:(NSIndexPath*)index_path {
  NSArray<NSDictionary*>* reports = [self latestReports];
  if (!reports.count) {
    // The one state row: loading, offline, or none yet.
    UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                                    reuseIdentifier:nil] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    if (discussion_controller_.loading) {
      cell.textLabel.text = NSLocalizedString(@"Loading reports…", @"compatibility: state");
      UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
          initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
      [spinner startAnimating];
      cell.accessoryView = spinner;
    } else if (discussion_controller_.onlineFeaturesOff) {
      cell.textLabel.text = NSLocalizedString(@"Reports need Online Features",
                                              @"compatibility: state");
      cell.detailTextLabel.text = NSLocalizedString(@"Turn them on in Settings to see what "
                                                    @"other players report.",
                                                    @"compatibility: state");
    } else {
      cell.textLabel.text = NSLocalizedString(@"No reports yet", @"compatibility: state");
      cell.detailTextLabel.text = NSLocalizedString(@"Played it? Yours would be the first.",
                                                    @"compatibility: state");
    }
    return cell;
  }
  if ((NSUInteger)index_path.row >= reports.count) {
    UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                    reuseIdentifier:nil] autorelease];
    const NSUInteger total = discussion_controller_.reports.count;
    cell.textLabel.text =
        total == 1 ? NSLocalizedString(@"All Reports", @"compatibility: last row")
                   : [NSString localizedStringWithFormat:NSLocalizedString(
                                                             @"All %lu Reports",
                                                             @"compatibility: last row, opens "
                                                             @"GitHub"),
                                                         (unsigned long)total];
    UIImageView* link = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"arrow.up.forward.square"]] autorelease];
    link.tintColor = [XeniaTheme textSecondary];
    cell.accessoryView = link;
    cell.accessibilityHint = NSLocalizedString(@"Opens GitHub.", @"compatibility: VoiceOver");
    return cell;
  }
  NSDictionary* report = reports[index_path.row];
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                                  reuseIdentifier:nil] autorelease];
  cell.textLabel.text = ReportHeadline(report);
  cell.textLabel.numberOfLines = 2;
  cell.detailTextLabel.text = ReportDetail(report);
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.detailTextLabel.numberOfLines = 0;
  cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  return cell;
}

- (void)tableView:(UITableView*)table_view didSelectRowAtIndexPath:(NSIndexPath*)index_path {
  [table_view deselectRowAtIndexPath:index_path animated:YES];
  NSArray<NSDictionary*>* reports = [self latestReports];
  if (!reports.count) {
    return;
  }
  if ((NSUInteger)index_path.row >= reports.count) {
    NSURL* url = [NSURL URLWithString:discussion_controller_.issueURL];
    if (url) {
      [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
    }
    return;
  }
  XeniaCompatReportPage* page =
      [[[XeniaCompatReportPage alloc] initWithReport:reports[index_path.row]
                                            issueURL:discussion_controller_.issueURL]
          autorelease];
  [self.navigationController pushViewController:page animated:YES];
}

@end
