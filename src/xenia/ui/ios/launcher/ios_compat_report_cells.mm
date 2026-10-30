/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_compat_report_cells.h"

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@implementation XeniaCompatReportCells

+ (UITableViewCell*)gameCellWithTitle:(NSString*)title
                               titleID:(uint32_t)title_id
                                   row:(NSInteger)row {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.detailTextLabel.adjustsFontSizeToFitWidth = YES;
  cell.detailTextLabel.minimumScaleFactor = 0.8;
  if (row == 0) {
    cell.textLabel.text = @"Title";
    cell.detailTextLabel.text = title;
  } else {
    cell.textLabel.text = @"Title ID";
    cell.detailTextLabel.text = XEFormatTitleIDHexUpper(title_id);
  }
  return cell;
}

+ (UITableViewCell*)environmentCellForRow:(NSInteger)row {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.detailTextLabel.adjustsFontSizeToFitWidth = YES;
  cell.detailTextLabel.minimumScaleFactor = 0.8;

  NSDictionary* build_info = xe_current_compat_report_build_info();
  switch (row) {
    case 0:
      cell.textLabel.text = @"Device";
      cell.detailTextLabel.text = xe_device_display_name();
      break;
    case 1:
      cell.textLabel.text = @"OS Version";
      cell.detailTextLabel.text = [UIDevice currentDevice].systemVersion;
      break;
    case 2:
      cell.textLabel.text = @"Architecture";
      cell.detailTextLabel.text = @"arm64";
      break;
    case 3:
      cell.textLabel.text = @"GPU Backend";
      cell.detailTextLabel.text = @"msl";
      break;
    case 4:
      cell.textLabel.text = @"Build";
      cell.detailTextLabel.text = xe_user_facing_build_label(build_info);
      break;
    default:
      break;
  }
  return cell;
}

+ (UIButton*)optionButtonWithTitle:(NSString*)title
                             color:(UIColor*)color
                          selected:(BOOL)selected
                           enabled:(BOOL)enabled
                            target:(id)target
                            action:(SEL)action
                               tag:(NSInteger)tag {
  UIButton* button = [UIButton buttonWithType:UIButtonTypeSystem];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  [button setTitle:title forState:UIControlStateNormal];
  [button setTitleColor:color forState:UIControlStateNormal];
  xe_apply_button_title_font(button, UIFontTextStyleCaption1, 13.0, UIFontWeightSemibold);
  button.contentEdgeInsets = UIEdgeInsetsMake(6.0, 12.0, 6.0, 12.0);
  button.backgroundColor = [color colorWithAlphaComponent:selected ? 0.16 : 0.10];
  button.layer.cornerRadius = XeniaRadiusLg;
  button.layer.borderWidth = selected ? 1.0 : 0.0;
  button.layer.borderColor = [color colorWithAlphaComponent:0.45].CGColor;
  button.enabled = enabled;
  button.alpha = enabled ? 1.0 : 0.35;
  button.tag = tag;
  [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
  return button;
}

+ (UITableViewCell*)optionsCellForSection:(NSInteger)section
                           selectedStatus:(NSInteger)selected_status
                             selectedPerf:(NSInteger)selected_perf
                                   target:(id)target
                             statusAction:(SEL)status_action
                               perfAction:(SEL)perf_action {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.backgroundColor = [UIColor clearColor];
  cell.contentView.backgroundColor = [UIColor clearColor];

  NSArray<NSString*>* keys = section == 2 ? xe_compat_statuses() : xe_compat_perfs();
  NSArray<NSString*>* labels = section == 2 ? xe_compat_status_labels() : xe_compat_perf_labels();

  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [XeniaTheme bgSurface];
  card.layer.cornerRadius = XeniaRadiusXl;
  card.layer.borderWidth = 0.5;
  card.layer.borderColor = [XeniaTheme border].CGColor;
  [cell.contentView addSubview:card];
  [NSLayoutConstraint activateConstraints:@[
    [card.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:6.0],
    [card.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16.0],
    [card.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16.0],
    [card.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-6.0],
  ]];

  UIStackView* vertical_stack = [[[UIStackView alloc] init] autorelease];
  vertical_stack.translatesAutoresizingMaskIntoConstraints = NO;
  vertical_stack.axis = UILayoutConstraintAxisVertical;
  vertical_stack.spacing = 10.0;
  [card addSubview:vertical_stack];

  NSMutableArray<NSArray<NSNumber*>*>* rows = [NSMutableArray array];
  if (section == 2) {
    [rows addObject:@[ @0, @1, @2 ]];
    [rows addObject:@[ @3, @4 ]];
  } else {
    [rows addObject:@[ @0, @1 ]];
    [rows addObject:@[ @2, @3 ]];
  }

  for (NSArray<NSNumber*>* row_indexes in rows) {
    UIStackView* row = [[[UIStackView alloc] init] autorelease];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.spacing = 10.0;
    row.alignment = UIStackViewAlignmentLeading;
    row.distribution = UIStackViewDistributionFillProportionally;

    for (NSNumber* index_number in row_indexes) {
      NSInteger option_index = [index_number integerValue];
      NSString* key = keys[option_index];
      NSString* label = labels[option_index];
      UIColor* color = section == 2 ? xe_compat_status_color(key) : xe_compat_perf_color(key);
      BOOL selected =
          section == 2 ? (option_index == selected_status) : (option_index == selected_perf);
      BOOL enabled = YES;
      if (section == 3) {
        // "n/a" (index 3) is only valid when the status is "nothing" (index 4);
        // the real tiers are only valid for every other status. Keep the button
        // states in sync with the server's invariant in both directions.
        BOOL status_is_nothing = (selected_status == 4);
        enabled = (option_index == 3) ? status_is_nothing : !status_is_nothing;
      }

      UIButton* button =
          [self optionButtonWithTitle:label
                                color:color
                             selected:selected
                              enabled:enabled
                               target:target
                               action:(section == 2) ? status_action : perf_action
                                  tag:option_index];
      [row addArrangedSubview:button];
    }

    UIView* spacer = [[[UIView alloc] init] autorelease];
    spacer.translatesAutoresizingMaskIntoConstraints = NO;
    [spacer.widthAnchor constraintGreaterThanOrEqualToConstant:1.0].active = YES;
    [row addArrangedSubview:spacer];
    [vertical_stack addArrangedSubview:row];
  }

  [NSLayoutConstraint activateConstraints:@[
    [vertical_stack.topAnchor constraintEqualToAnchor:card.topAnchor constant:14.0],
    [vertical_stack.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [vertical_stack.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],
    [vertical_stack.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-14.0],
  ]];

  return cell;
}

+ (UITableViewCell*)screenshotCellWithImage:(UIImage*)image index:(NSInteger)index {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  cell.imageView.image = image;
  cell.imageView.contentMode = UIViewContentModeScaleAspectFill;
  cell.imageView.clipsToBounds = YES;
  cell.imageView.layer.cornerRadius = XeniaRadiusXs;
  cell.textLabel.text = [NSString stringWithFormat:@"Screenshot %ld", (long)(index + 1)];
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  return cell;
}

+ (UITableViewCell*)addScreenshotCell {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.textLabel.text = @"Add Screenshot";
  cell.textLabel.textColor = [XeniaTheme accent];
  cell.imageView.image = [UIImage systemImageNamed:@"plus.circle"];
  cell.imageView.tintColor = [XeniaTheme accent];
  return cell;
}

+ (UITableViewCell*)submitCellSubmitting:(BOOL)submitting {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.backgroundColor = [XeniaTheme accent];
  cell.clipsToBounds = YES;
  cell.layer.cornerRadius = XeniaRadiusMd;
  cell.textLabel.text = submitting ? @"Submitting..." : @"Submit Report";
  cell.textLabel.textColor = [XeniaTheme accentFg];
  cell.textLabel.textAlignment = NSTextAlignmentCenter;
  xe_apply_label_font(cell.textLabel, UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold);
  return cell;
}

@end
