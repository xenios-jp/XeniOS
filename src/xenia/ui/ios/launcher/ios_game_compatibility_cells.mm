/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_compatibility_cells.h"

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

constexpr NSInteger kXeniaDiscussionPreviewCount = 3;

UIView* XeniaCompatCardViewForCell(UITableViewCell* cell) {
  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [XeniaTheme bgSurface];
  card.layer.cornerRadius = XeniaRadiusXl;
  card.layer.borderWidth = 0.5;
  card.layer.borderColor = [XeniaTheme border].CGColor;
  [cell.contentView addSubview:card];
  [NSLayoutConstraint activateConstraints:@[
    [card.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:6],
    [card.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16],
    [card.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16],
    [card.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-6],
  ]];
  return card;
}

UIView* XeniaCompatBuildMetadataPillRowForEntry(NSDictionary* entry) {
  NSDictionary* build_info = xe_compat_build_info_from_entry(entry);
  if (!build_info) {
    return nil;
  }

  UIStackView* stack = [[[UIStackView alloc] init] autorelease];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  stack.axis = UILayoutConstraintAxisHorizontal;
  stack.spacing = 8.0;
  stack.alignment = UIStackViewAlignmentLeading;
  stack.distribution = UIStackViewDistributionFillProportionally;

  NSString* channel = xe_string_from_object(build_info[@"channel"]);
  if (channel.length > 0) {
    [stack addArrangedSubview:xe_make_tag_pill(xe_compat_channel_label(channel),
                                               xe_compat_channel_color(channel))];
  }

  NSString* build_label = xe_compat_build_label(build_info);
  if (build_label.length > 0) {
    [stack addArrangedSubview:xe_make_tag_pill(build_label, [XeniaTheme textSecondary])];
  }

  if (stack.arrangedSubviews.count == 0) {
    return nil;
  }

  UIView* row = [[[UIView alloc] init] autorelease];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  [row addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [stack.topAnchor constraintEqualToAnchor:row.topAnchor],
    [stack.leadingAnchor constraintEqualToAnchor:row.leadingAnchor],
    [stack.trailingAnchor constraintLessThanOrEqualToAnchor:row.trailingAnchor],
    [stack.bottomAnchor constraintEqualToAnchor:row.bottomAnchor],
  ]];
  return row;
}

UIView* XeniaCompatDetailsMetricTile(NSString* label, NSString* value, UIColor* value_color,
                                    BOOL value_is_pill) {
  UIView* tile = [[[UIView alloc] init] autorelease];
  tile.translatesAutoresizingMaskIntoConstraints = NO;
  tile.backgroundColor = [XeniaTheme bgPrimary];
  tile.layer.cornerRadius = XeniaRadiusMd;
  tile.layer.borderWidth = 0.5;
  tile.layer.borderColor = [XeniaTheme border].CGColor;

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.translatesAutoresizingMaskIntoConstraints = NO;
  title.text = label;
  title.font = [UIFont systemFontOfSize:11 weight:UIFontWeightSemibold];
  title.textColor = [XeniaTheme textMuted];
  [tile addSubview:title];

  UIView* value_view = nil;
  if (value_is_pill) {
    value_view = xe_make_tag_pill(value ?: @"Unknown", value_color ?: [XeniaTheme textMuted]);
  } else {
    UILabel* value_label = [[[UILabel alloc] init] autorelease];
    value_label.translatesAutoresizingMaskIntoConstraints = NO;
    value_label.text = value ?: @"Unknown";
    value_label.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    value_label.textColor = value_color ?: [XeniaTheme textPrimary];
    value_label.numberOfLines = 1;
    value_view = value_label;
  }
  [tile addSubview:value_view];

  [NSLayoutConstraint activateConstraints:@[
    [tile.heightAnchor constraintGreaterThanOrEqualToConstant:86.0],
    [title.topAnchor constraintEqualToAnchor:tile.topAnchor constant:12.0],
    [title.leadingAnchor constraintEqualToAnchor:tile.leadingAnchor constant:12.0],
    [title.trailingAnchor constraintLessThanOrEqualToAnchor:tile.trailingAnchor constant:-12.0],
    [value_view.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:10.0],
    [value_view.leadingAnchor constraintEqualToAnchor:tile.leadingAnchor constant:12.0],
    [value_view.trailingAnchor constraintLessThanOrEqualToAnchor:tile.trailingAnchor constant:-12.0],
    [value_view.bottomAnchor constraintLessThanOrEqualToAnchor:tile.bottomAnchor constant:-12.0],
  ]];

  return tile;
}

UIView* XeniaCompatDiscussionPreviewCardForReport(NSDictionary* report, NSInteger report_index,
                                                  NSSet<NSNumber*>* expanded_report_indexes,
                                                  NSString* issue_url, id target,
                                                  SEL view_issue_action,
                                                  SEL toggle_report_action) {
  NSString* author = [report[@"submittedBy"] isKindOfClass:[NSString class]]
                         ? report[@"submittedBy"]
                         : @"anonymous";
  NSString* notes = [report[@"notes"] isKindOfClass:[NSString class]] ? report[@"notes"] : @"";
  NSString* date_string = [report[@"date"] isKindOfClass:[NSString class]] ? report[@"date"] : @"";
  NSString* formatted_date =
      date_string.length >= 10 ? xe_format_iso_date(date_string) : date_string;

  if (![notes isKindOfClass:[NSString class]] || notes.length == 0) {
    notes = @"No details provided.";
  }

  NSMutableArray<NSString*>* info_parts = [NSMutableArray array];
  NSString* status = [report[@"status"] isKindOfClass:[NSString class]] ? report[@"status"] : nil;
  if (status.length > 0) {
    [info_parts addObject:xe_compat_status_label(status)];
  }
  NSString* report_device = [report[@"deviceMachine"] isKindOfClass:[NSString class]]
                                ? report[@"deviceMachine"]
                                : report[@"device"];
  if ([report_device isKindOfClass:[NSString class]] && report_device.length > 0) {
    [info_parts addObject:xe_device_display_name_for_machine(report_device)];
  }
  NSString* platform_display = xe_platform_display_text(report[@"platform"], report[@"osVersion"]);
  if (platform_display.length > 0) {
    [info_parts addObject:platform_display];
  }
  NSString* gpu_backend =
      [report[@"gpuBackend"] isKindOfClass:[NSString class]] ? report[@"gpuBackend"] : nil;
  if (gpu_backend.length > 0) {
    [info_parts addObject:[gpu_backend uppercaseString]];
  }
  NSString* info_text = [info_parts componentsJoinedByString:@" · "];

  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [XeniaTheme bgPrimary];
  card.layer.cornerRadius = XeniaRadiusMd;
  card.layer.borderWidth = 0.5;
  card.layer.borderColor = [XeniaTheme border].CGColor;

  UILabel* author_label = [[[UILabel alloc] init] autorelease];
  author_label.translatesAutoresizingMaskIntoConstraints = NO;
  author_label.text = author.length > 0 ? author : @"anonymous";
  author_label.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
  author_label.textColor = [XeniaTheme textPrimary];
  [card addSubview:author_label];

  UILabel* date_label = [[[UILabel alloc] init] autorelease];
  date_label.translatesAutoresizingMaskIntoConstraints = NO;
  date_label.text = formatted_date;
  date_label.font = [UIFont systemFontOfSize:13];
  date_label.textColor = [XeniaTheme textMuted];
  date_label.textAlignment = NSTextAlignmentRight;
  [date_label setContentHuggingPriority:UILayoutPriorityRequired
                                forAxis:UILayoutConstraintAxisHorizontal];
  [date_label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                              forAxis:UILayoutConstraintAxisHorizontal];
  [card addSubview:date_label];

  UIView* build_row = XeniaCompatBuildMetadataPillRowForEntry(report);
  if (build_row) {
    [card addSubview:build_row];
  }

  UILabel* info_label = [[[UILabel alloc] init] autorelease];
  info_label.translatesAutoresizingMaskIntoConstraints = NO;
  info_label.text = info_text;
  info_label.font = [UIFont systemFontOfSize:13];
  info_label.textColor = [XeniaTheme textMuted];
  info_label.numberOfLines = 2;
  info_label.hidden = info_label.text.length == 0;
  [card addSubview:info_label];

  UILabel* notes_label = [[[UILabel alloc] init] autorelease];
  notes_label.translatesAutoresizingMaskIntoConstraints = NO;
  notes_label.text = notes;
  notes_label.font = [UIFont systemFontOfSize:15];
  notes_label.textColor = [XeniaTheme textSecondary];
  BOOL report_expanded =
      [expanded_report_indexes containsObject:[NSNumber numberWithInteger:report_index]];
  notes_label.numberOfLines = report_expanded ? 0 : 3;
  notes_label.lineBreakMode =
      report_expanded ? NSLineBreakByWordWrapping : NSLineBreakByTruncatingTail;
  [card addSubview:notes_label];

  BOOL can_expand_notes = notes.length > 170 || [notes rangeOfString:@"\n"].location != NSNotFound;
  UIButton* expand_notes_button = [UIButton buttonWithType:UIButtonTypeSystem];
  expand_notes_button.translatesAutoresizingMaskIntoConstraints = NO;
  [expand_notes_button setTitle:(report_expanded ? @"Show less" : @"Show more")
                       forState:UIControlStateNormal];
  [expand_notes_button setTitleColor:[XeniaTheme accent] forState:UIControlStateNormal];
  expand_notes_button.titleLabel.font = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
  expand_notes_button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
  expand_notes_button.tag = report_index;
  expand_notes_button.hidden = !can_expand_notes;
  [expand_notes_button addTarget:target
                          action:toggle_report_action
                forControlEvents:UIControlEventTouchUpInside];
  [card addSubview:expand_notes_button];

  UIButton* open_comment_button = [UIButton buttonWithType:UIButtonTypeSystem];
  open_comment_button.translatesAutoresizingMaskIntoConstraints = NO;
  [open_comment_button setTitle:@"Open comment" forState:UIControlStateNormal];
  [open_comment_button setTitleColor:[XeniaTheme accent] forState:UIControlStateNormal];
  open_comment_button.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
  open_comment_button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
  open_comment_button.hidden = (issue_url == nil);
  [open_comment_button addTarget:target
                          action:view_issue_action
                forControlEvents:UIControlEventTouchUpInside];
  [card addSubview:open_comment_button];

  NSMutableArray<NSLayoutConstraint*>* constraints = [NSMutableArray arrayWithArray:@[
    [author_label.topAnchor constraintEqualToAnchor:card.topAnchor constant:12],
    [author_label.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
    [date_label.firstBaselineAnchor constraintEqualToAnchor:author_label.firstBaselineAnchor],
    [date_label.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-10],
    [date_label.leadingAnchor constraintGreaterThanOrEqualToAnchor:author_label.trailingAnchor
                                                          constant:8],
    [notes_label.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
    [notes_label.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-10],
    [expand_notes_button.topAnchor constraintEqualToAnchor:notes_label.bottomAnchor constant:4],
    [expand_notes_button.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
    [expand_notes_button.trailingAnchor constraintLessThanOrEqualToAnchor:card.trailingAnchor
                                                                 constant:-10],
    [info_label.topAnchor constraintEqualToAnchor:expand_notes_button.bottomAnchor constant:6],
    [info_label.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
    [info_label.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-10],
    [open_comment_button.topAnchor constraintEqualToAnchor:info_label.bottomAnchor constant:8],
    [open_comment_button.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
    [open_comment_button.trailingAnchor constraintLessThanOrEqualToAnchor:card.trailingAnchor
                                                                 constant:-10],
    [open_comment_button.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-10],
  ]];
  if (build_row) {
    [constraints addObjectsFromArray:@[
      [build_row.topAnchor constraintEqualToAnchor:author_label.bottomAnchor constant:8],
      [build_row.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:10],
      [build_row.trailingAnchor constraintLessThanOrEqualToAnchor:card.trailingAnchor constant:-10],
      [notes_label.topAnchor constraintEqualToAnchor:build_row.bottomAnchor constant:8],
    ]];
  } else {
    [constraints addObject:[notes_label.topAnchor constraintEqualToAnchor:author_label.bottomAnchor
                                                                 constant:6]];
  }
  [NSLayoutConstraint activateConstraints:constraints];
  if (expand_notes_button.hidden) {
    [expand_notes_button.heightAnchor constraintEqualToConstant:0].active = YES;
  }
  if (info_label.hidden) {
    [info_label.heightAnchor constraintEqualToConstant:0].active = YES;
  }
  if (open_comment_button.hidden) {
    [open_comment_button.heightAnchor constraintEqualToConstant:0].active = YES;
  }

  return card;
}

}  // namespace

@implementation XeniaGameCompatibilityCells

+ (UITableViewCell*)detailsCellWithCompatInfo:(NSDictionary*)compat_info
                                  latestReport:(NSDictionary*)latest_report {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.backgroundColor = [UIColor clearColor];
  cell.contentView.backgroundColor = [UIColor clearColor];

  NSDictionary* summary_source = xe_preferred_summary_from_compat_info(compat_info);
  BOOL using_release_summary =
      [xe_string_from_object(summary_source[@"channel"]) isEqualToString:@"release"];
  NSDictionary* details_source = summary_source ?: latest_report;

  NSString* status = xe_string_from_object(details_source[@"status"]);
  NSString* status_label = status.length > 0 ? xe_compat_status_label(status) : @"Unknown";
  UIColor* status_color =
      status.length > 0 ? xe_compat_status_color(status) : [XeniaTheme textMuted];

  NSString* report_device = [details_source[@"deviceMachine"] isKindOfClass:[NSString class]]
                                ? details_source[@"deviceMachine"]
                                : details_source[@"device"];
  NSString* device =
      report_device.length > 0 ? xe_device_display_name_for_machine(report_device) : @"Unknown";

  NSString* platform_display =
      xe_platform_display_text(details_source[@"platform"], details_source[@"osVersion"]);
  if (platform_display.length == 0) {
    platform_display = @"Unknown";
  }

  NSString* gpu = [details_source[@"gpuBackend"] isKindOfClass:[NSString class]]
                      ? details_source[@"gpuBackend"]
                      : nil;
  if (gpu.length == 0) {
    gpu = @"Unknown";
  } else {
    gpu = [gpu uppercaseString];
  }

  NSString* based_on_date =
      [details_source[@"date"] isKindOfClass:[NSString class]] ? details_source[@"date"] : nil;
  if (based_on_date.length == 0 && [compat_info[@"updatedAt"] isKindOfClass:[NSString class]]) {
    based_on_date = compat_info[@"updatedAt"];
  }
  NSString* footnote = @"Based on available compatibility data.";
  if (using_release_summary && [status isEqualToString:@"untested"]) {
    footnote =
        @"No official release reports yet. Preview or self-built reports may still appear below.";
  } else if (using_release_summary && based_on_date.length > 0) {
    footnote = [NSString stringWithFormat:@"Based on the latest official release summary from %@.",
                                          xe_format_iso_date(based_on_date)];
  } else if (using_release_summary) {
    footnote = @"Based on the current official release summary.";
  } else if (based_on_date.length > 0) {
    footnote = [NSString
        stringWithFormat:@"Based on the latest report from %@.", xe_format_iso_date(based_on_date)];
  }

  UIView* card = XeniaCompatCardViewForCell(cell);

  UILabel* heading = [[[UILabel alloc] init] autorelease];
  heading.translatesAutoresizingMaskIntoConstraints = NO;
  heading.text = @"Details";
  heading.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
  heading.textColor = [XeniaTheme textPrimary];
  [card addSubview:heading];

  UILabel* subheading = [[[UILabel alloc] init] autorelease];
  subheading.translatesAutoresizingMaskIntoConstraints = NO;
  subheading.text =
      using_release_summary ? @"RELEASE SUMMARY" : (summary_source ? @"CURRENT SUMMARY" : @"LATEST REPORT");
  subheading.font = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
  subheading.textColor = [XeniaTheme textMuted];
  [card addSubview:subheading];

  NSDictionary* build_entry = details_source;
  if (!xe_compat_build_info_from_entry(build_entry) && compat_info && build_entry != compat_info) {
    build_entry = compat_info;
  }
  UIView* build_row = XeniaCompatBuildMetadataPillRowForEntry(build_entry);
  if (build_row) {
    [card addSubview:build_row];
  }

  UIStackView* grid = [[[UIStackView alloc] init] autorelease];
  grid.translatesAutoresizingMaskIntoConstraints = NO;
  grid.axis = UILayoutConstraintAxisVertical;
  grid.spacing = 10.0;
  [card addSubview:grid];

  UIStackView* row_one = [[[UIStackView alloc] init] autorelease];
  row_one.axis = UILayoutConstraintAxisHorizontal;
  row_one.spacing = 10.0;
  row_one.distribution = UIStackViewDistributionFillEqually;
  [grid addArrangedSubview:row_one];

  UIStackView* row_two = [[[UIStackView alloc] init] autorelease];
  row_two.axis = UILayoutConstraintAxisHorizontal;
  row_two.spacing = 10.0;
  row_two.distribution = UIStackViewDistributionFillEqually;
  [grid addArrangedSubview:row_two];

  [row_one addArrangedSubview:XeniaCompatDetailsMetricTile(@"STATUS", status_label, status_color,
                                                          YES)];
  [row_one addArrangedSubview:XeniaCompatDetailsMetricTile(@"DEVICE", device,
                                                          [XeniaTheme textPrimary], NO)];
  [row_two addArrangedSubview:XeniaCompatDetailsMetricTile(@"PLATFORM", platform_display,
                                                          [XeniaTheme textPrimary], NO)];
  [row_two addArrangedSubview:XeniaCompatDetailsMetricTile(@"GPU", gpu, [XeniaTheme textPrimary],
                                                          NO)];

  UILabel* footer = [[[UILabel alloc] init] autorelease];
  footer.translatesAutoresizingMaskIntoConstraints = NO;
  footer.text = footnote;
  footer.font = [UIFont systemFontOfSize:12];
  footer.textColor = [XeniaTheme textSecondary];
  footer.numberOfLines = 0;
  [card addSubview:footer];

  NSMutableArray<NSLayoutConstraint*>* constraints = [NSMutableArray arrayWithArray:@[
    [heading.topAnchor constraintEqualToAnchor:card.topAnchor constant:16.0],
    [heading.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [heading.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],
    [subheading.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:12.0],
    [subheading.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [subheading.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],
    [grid.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [grid.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],
    [footer.topAnchor constraintEqualToAnchor:grid.bottomAnchor constant:12.0],
    [footer.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [footer.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],
    [footer.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-14.0],
  ]];
  if (build_row) {
    [constraints addObjectsFromArray:@[
      [build_row.topAnchor constraintEqualToAnchor:subheading.bottomAnchor constant:8.0],
      [build_row.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
      [build_row.trailingAnchor constraintLessThanOrEqualToAnchor:card.trailingAnchor
                                                         constant:-16.0],
      [grid.topAnchor constraintEqualToAnchor:build_row.bottomAnchor constant:10.0],
    ]];
  } else {
    [constraints addObject:[grid.topAnchor constraintEqualToAnchor:subheading.bottomAnchor
                                                          constant:10.0]];
  }
  [NSLayoutConstraint activateConstraints:constraints];

  return cell;
}

+ (UITableViewCell*)ctaCellWithTarget:(id)target submitAction:(SEL)submit_action {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.backgroundColor = [UIColor clearColor];
  cell.contentView.backgroundColor = [UIColor clearColor];

  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [[XeniaTheme accent] colorWithAlphaComponent:0.04];
  card.layer.cornerRadius = XeniaRadiusXl;
  card.layer.borderWidth = 1.0;
  card.layer.borderColor = [[XeniaTheme accent] colorWithAlphaComponent:0.20].CGColor;
  card.clipsToBounds = YES;
  [cell.contentView addSubview:card];

  UILabel* heading = [[[UILabel alloc] init] autorelease];
  heading.translatesAutoresizingMaskIntoConstraints = NO;
  heading.text = @"Tested this game?";
  heading.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
  heading.textColor = [XeniaTheme textPrimary];
  heading.textAlignment = NSTextAlignmentCenter;
  [card addSubview:heading];

  UILabel* subtext = [[[UILabel alloc] init] autorelease];
  subtext.translatesAutoresizingMaskIntoConstraints = NO;
  subtext.text = @"Help the community by sharing how well this title runs on your device.";
  subtext.font = [UIFont systemFontOfSize:14];
  subtext.textColor = [XeniaTheme textSecondary];
  subtext.numberOfLines = 0;
  subtext.textAlignment = NSTextAlignmentCenter;
  [card addSubview:subtext];

  UIButton* submit_button = [UIButton buttonWithType:UIButtonTypeSystem];
  submit_button.translatesAutoresizingMaskIntoConstraints = NO;
  [submit_button setTitle:@"Submit Report" forState:UIControlStateNormal];
  submit_button.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
  [submit_button setTitleColor:[XeniaTheme accentFg] forState:UIControlStateNormal];
  submit_button.backgroundColor = [XeniaTheme accent];
  submit_button.layer.cornerRadius = XeniaRadiusMd;
  submit_button.clipsToBounds = YES;
  [submit_button addTarget:target action:submit_action forControlEvents:UIControlEventTouchUpInside];
  [card addSubview:submit_button];

  [NSLayoutConstraint activateConstraints:@[
    [card.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:6.0],
    [card.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16.0],
    [card.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16.0],
    [card.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-6.0],
    [heading.topAnchor constraintEqualToAnchor:card.topAnchor constant:18.0],
    [heading.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [heading.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],
    [subtext.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:8.0],
    [subtext.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:24.0],
    [subtext.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-24.0],
    [submit_button.topAnchor constraintEqualToAnchor:subtext.bottomAnchor constant:16.0],
    [submit_button.centerXAnchor constraintEqualToAnchor:card.centerXAnchor],
    [submit_button.heightAnchor constraintEqualToConstant:44.0],
    [submit_button.widthAnchor constraintGreaterThanOrEqualToConstant:164.0],
    [submit_button.leadingAnchor constraintGreaterThanOrEqualToAnchor:card.leadingAnchor
                                                             constant:16.0],
    [submit_button.trailingAnchor constraintLessThanOrEqualToAnchor:card.trailingAnchor
                                                           constant:-16.0],
    [submit_button.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-18.0],
  ]];

  return cell;
}

+ (UITableViewCell*)discussionCellWithReports:(NSArray<NSDictionary*>*)reports
                        expandedReportIndexes:(NSSet<NSNumber*>*)expanded_report_indexes
                                      loading:(BOOL)loading
                                      showAll:(BOOL)show_all
                                     issueURL:(NSString*)issue_url
                                  issueNumber:(NSInteger)issue_number
                                       target:(id)target
                              viewIssueAction:(SEL)view_issue_action
                        toggleExpansionAction:(SEL)toggle_expansion_action
                           toggleReportAction:(SEL)toggle_report_action {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.backgroundColor = [UIColor clearColor];
  cell.contentView.backgroundColor = [UIColor clearColor];

  UIView* card = XeniaCompatCardViewForCell(cell);
  UIStackView* stack = [[[UIStackView alloc] init] autorelease];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  stack.axis = UILayoutConstraintAxisVertical;
  stack.spacing = 12.0;
  [card addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:16],
    [stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16],
    [stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16],
    [stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-16],
  ]];

  UIView* heading_row = [[[UIView alloc] init] autorelease];
  heading_row.translatesAutoresizingMaskIntoConstraints = NO;
  heading_row.backgroundColor = [XeniaTheme bgSurface];
  UILabel* heading_label = [[[UILabel alloc] init] autorelease];
  heading_label.translatesAutoresizingMaskIntoConstraints = NO;
  heading_label.text = @"Discussion";
  heading_label.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
  heading_label.textColor = [XeniaTheme textPrimary];
  [heading_row addSubview:heading_label];

  UIButton* heading_button = [UIButton buttonWithType:UIButtonTypeSystem];
  heading_button.translatesAutoresizingMaskIntoConstraints = NO;
  NSString* button_title = issue_number > 0
                               ? [NSString stringWithFormat:@"View Issue #%ld", (long)issue_number]
                               : @"View on GitHub";
  [heading_button setTitle:button_title forState:UIControlStateNormal];
  [heading_button setTitleColor:[XeniaTheme accent] forState:UIControlStateNormal];
  heading_button.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
  heading_button.hidden = (issue_url == nil);
  [heading_button addTarget:target
                     action:view_issue_action
           forControlEvents:UIControlEventTouchUpInside];
  [heading_row addSubview:heading_button];

  [NSLayoutConstraint activateConstraints:@[
    [heading_label.topAnchor constraintEqualToAnchor:heading_row.topAnchor],
    [heading_label.leadingAnchor constraintEqualToAnchor:heading_row.leadingAnchor],
    [heading_label.bottomAnchor constraintEqualToAnchor:heading_row.bottomAnchor],
    [heading_button.firstBaselineAnchor constraintEqualToAnchor:heading_label.firstBaselineAnchor],
    [heading_button.trailingAnchor constraintEqualToAnchor:heading_row.trailingAnchor],
    [heading_button.leadingAnchor constraintGreaterThanOrEqualToAnchor:heading_label.trailingAnchor
                                                              constant:8],
  ]];
  [stack addArrangedSubview:heading_row];

  if (loading) {
    UILabel* loading_label = [[[UILabel alloc] init] autorelease];
    loading_label.text = @"Loading discussion...";
    loading_label.font = [UIFont systemFontOfSize:15];
    loading_label.textColor = [XeniaTheme textMuted];
    loading_label.numberOfLines = 1;
    [stack addArrangedSubview:loading_label];
    return cell;
  }

  if (reports.count == 0) {
    UILabel* empty_label = [[[UILabel alloc] init] autorelease];
    empty_label.text = @"No reports yet. Be the first to submit one.";
    empty_label.font = [UIFont systemFontOfSize:15];
    empty_label.textColor = [XeniaTheme textMuted];
    empty_label.numberOfLines = 0;
    [stack addArrangedSubview:empty_label];
    return cell;
  }

  NSInteger report_count = (NSInteger)reports.count;
  NSInteger visible_count = show_all ? report_count : MIN(report_count, kXeniaDiscussionPreviewCount);
  for (NSInteger report_index = 0; report_index < visible_count; ++report_index) {
    NSDictionary* report = reports[report_index];
    if (![report isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    [stack addArrangedSubview:XeniaCompatDiscussionPreviewCardForReport(
                               report, report_index, expanded_report_indexes, issue_url, target,
                               view_issue_action, toggle_report_action)];
  }

  if (report_count > kXeniaDiscussionPreviewCount) {
    UILabel* summary_label = [[[UILabel alloc] init] autorelease];
    summary_label.text =
        show_all
            ? [NSString stringWithFormat:@"Showing all %ld reports.", (long)report_count]
            : [NSString stringWithFormat:@"Showing latest %ld of %ld reports.", (long)visible_count,
                                         (long)report_count];
    summary_label.font = [UIFont systemFontOfSize:12];
    summary_label.textColor = [XeniaTheme textMuted];
    summary_label.numberOfLines = 1;
    [stack addArrangedSubview:summary_label];

    UIButton* toggle_button = [UIButton buttonWithType:UIButtonTypeSystem];
    toggle_button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    NSString* toggle_title = show_all
                                 ? @"Show fewer reports"
                                 : [NSString stringWithFormat:@"Show all %ld reports",
                                                            (long)report_count];
    [toggle_button setTitle:toggle_title forState:UIControlStateNormal];
    [toggle_button setTitleColor:[XeniaTheme accent] forState:UIControlStateNormal];
    toggle_button.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    [toggle_button addTarget:target
                      action:toggle_expansion_action
            forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:toggle_button];
  }

  return cell;
}

@end
