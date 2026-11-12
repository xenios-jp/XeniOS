/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_choice_list_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaChoiceListViewController {
  std::vector<IOSConfigChoice> choices_;
  int64_t selected_value_;
  NSString* subtitle_;
  IOSChoiceSelectionHandler on_selection_;
}

- (instancetype)initWithTitle:(NSString*)title
                     subtitle:(NSString*)subtitle
                      choices:(const std::vector<IOSConfigChoice>&)choices
                selectedValue:(int64_t)selectedValue
                  onSelection:(IOSChoiceSelectionHandler)onSelection {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    self.title = title;
    subtitle_ = [subtitle copy];
    choices_ = choices;
    selected_value_ = selectedValue;
    on_selection_ = [onSelection copy];
  }
  return self;
}

- (void)dealloc {
  [subtitle_ release];
  [on_selection_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.separatorInset = UIEdgeInsetsMake(0, 16, 0, 16);

  if (subtitle_.length > 0) {
    UILabel* header_label = [[UILabel alloc] initWithFrame:CGRectZero];
    header_label.text = subtitle_;
    header_label.textColor = [XeniaTheme textSecondary];
    xe_apply_label_font(header_label, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
    header_label.numberOfLines = 0;
    header_label.textAlignment = NSTextAlignmentLeft;
    header_label.translatesAutoresizingMaskIntoConstraints = NO;
    header_label.isAccessibilityElement = YES;
    header_label.accessibilityTraits = UIAccessibilityTraitHeader;

    UIView* header = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 1, 56)];
    [header addSubview:header_label];
    [NSLayoutConstraint activateConstraints:@[
      [header_label.leadingAnchor constraintEqualToAnchor:header.leadingAnchor constant:18],
      [header_label.trailingAnchor constraintEqualToAnchor:header.trailingAnchor constant:-18],
      [header_label.topAnchor constraintEqualToAnchor:header.topAnchor constant:8],
      [header_label.bottomAnchor constraintEqualToAnchor:header.bottomAnchor constant:-8],
    ]];
    self.tableView.tableHeaderView = header;
  }
}

- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger)__unused section {
  return static_cast<NSInteger>(choices_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger)__unused section {
  for (const IOSConfigChoice& choice : choices_) {
    if (choice.is_custom) {
      return NSLocalizedString(
          @"The current value is not one of the listed choices. It was probably set by editing "
          @"the config file by hand. Choose another entry to replace it.",
          @"choice list view controller: footer for a value that is not in the list");
    }
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kChoiceCellIdentifier = @"XeniaChoiceCell";
  static NSString* const kChoiceSubtitleCellIdentifier = @"XeniaChoiceSubtitleCell";
  const BOOL has_subtitle =
      indexPath.row >= 0 && indexPath.row < static_cast<NSInteger>(choices_.size()) &&
      !choices_[indexPath.row].subtitle.empty();
  NSString* identifier = has_subtitle ? kChoiceSubtitleCellIdentifier : kChoiceCellIdentifier;
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:identifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:has_subtitle ? UITableViewCellStyleSubtitle
                                                                : UITableViewCellStyleDefault
                                   reuseIdentifier:identifier] autorelease];
  }
  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(choices_.size())) {
    cell.textLabel.text = @"";
    cell.accessoryType = UITableViewCellAccessoryNone;
    return cell;
  }
  const IOSConfigChoice& choice = choices_[indexPath.row];
  cell.textLabel.text = ToNSString(choice.title);
  if (has_subtitle) {
    cell.detailTextLabel.text = ToNSString(choice.subtitle);
    cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  }
  cell.accessoryType = (choice.value == selected_value_) ? UITableViewCellAccessoryCheckmark
                                                         : UITableViewCellAccessoryNone;
  cell.isAccessibilityElement = YES;
  cell.accessibilityLabel = has_subtitle ? [NSString stringWithFormat:@"%@, %@", cell.textLabel.text,
                                                                       cell.detailTextLabel.text]
                                         : cell.textLabel.text;
  cell.accessibilityValue =
      (choice.value == selected_value_)
          ? NSLocalizedString(@"Selected", @"choice list view controller: accessibility value")
          : NSLocalizedString(@"Not selected", @"choice list view controller: accessibility value");
  // A custom value only marks what is stored now; it can't be picked.
  cell.selectionStyle =
      choice.is_custom ? UITableViewCellSelectionStyleNone : UITableViewCellSelectionStyleDefault;
  cell.accessibilityTraits =
      (choice.is_custom ? UIAccessibilityTraitStaticText : UIAccessibilityTraitButton) |
      ((choice.value == selected_value_) ? UIAccessibilityTraitSelected : 0);
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(choices_.size())) {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    return;
  }
  const IOSConfigChoice& choice = choices_[indexPath.row];
  if (choice.is_custom) {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    return;
  }
  selected_value_ = choice.value;
  if (on_selection_) {
    on_selection_(selected_value_);
  }
  [self.navigationController popViewControllerAnimated:YES];
}

@end
