/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_compat_report_view_controller.h"

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_compat_report_cells.h"
#import "xenia/ui/ios/launcher/ios_compat_report_submission.h"
#import "xenia/ui/ios/shared/ios_theme.h"

@implementation XeniaCompatReportViewController {
  uint32_t title_id_;
  NSString* game_title_;
  NSInteger selected_status_;
  NSInteger selected_perf_;
  UITextView* notes_text_view_;
  UILabel* notes_placeholder_label_;
  UIBarButtonItem* keyboard_done_button_;
  NSMutableArray<UIImage*>* screenshots_;
  BOOL submitting_;
}

- (instancetype)initWithTitleID:(uint32_t)title_id title:(NSString*)title {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    title_id_ = title_id;
    game_title_ = [title copy];
    selected_status_ = -1;
    selected_perf_ = -1;
    screenshots_ = [[NSMutableArray alloc] init];
    submitting_ = NO;
    self.title = @"Submit Report";
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [game_title_ release];
  [notes_text_view_ release];
  [notes_placeholder_label_ release];
  [keyboard_done_button_ release];
  [screenshots_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [XeniaTheme bgPrimary];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 64.0;
  self.tableView.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
  keyboard_done_button_ =
      [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                    target:self
                                                    action:@selector(dismissKeyboard)];
  keyboard_done_button_.tintColor = [XeniaTheme accent];
  if (@available(iOS 15.0, *)) {
    self.tableView.sectionHeaderTopPadding = 0;
  }
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(keyboardWillChangeFrame:)
                                               name:UIKeyboardWillChangeFrameNotification
                                             object:nil];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(keyboardWillHide:)
                                               name:UIKeyboardWillHideNotification
                                             object:nil];
}

- (void)scrollNotesEditorIntoViewAnimated:(BOOL)animated {
  NSIndexPath* notes_path = [NSIndexPath indexPathForRow:0 inSection:4];
  if ([self.tableView numberOfSections] <= notes_path.section ||
      [self.tableView numberOfRowsInSection:notes_path.section] <= notes_path.row) {
    return;
  }
  [self.tableView scrollToRowAtIndexPath:notes_path
                        atScrollPosition:UITableViewScrollPositionTop
                                animated:animated];
  if (!notes_text_view_) {
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    CGRect target = [self->notes_text_view_ convertRect:self->notes_text_view_.bounds
                                                 toView:self.tableView];
    target = CGRectInset(target, 0.0, -12.0);
    [self.tableView scrollRectToVisible:target animated:animated];
  });
}

- (void)keyboardWillChangeFrame:(NSNotification*)notification {
  NSDictionary* user_info = notification.userInfo;
  CGRect keyboard_end = [user_info[UIKeyboardFrameEndUserInfoKey] CGRectValue];
  CGRect keyboard_in_view = [self.view convertRect:keyboard_end fromView:nil];
  CGFloat overlap = MAX(0.0, CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(keyboard_in_view));
  CGFloat safe_bottom = self.view.safeAreaInsets.bottom;
  CGFloat bottom_inset = MAX(0.0, overlap - safe_bottom);

  NSTimeInterval duration = [user_info[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
  UIViewAnimationOptions options =
      (UIViewAnimationOptions)([user_info[UIKeyboardAnimationCurveUserInfoKey] integerValue] << 16);

  [UIView animateWithDuration:duration
                        delay:0.0
                      options:options
                   animations:^{
                     UIEdgeInsets content_inset = self.tableView.contentInset;
                     content_inset.bottom = bottom_inset + 16.0;
                     self.tableView.contentInset = content_inset;
                     self.tableView.scrollIndicatorInsets = content_inset;
                   }
                   completion:nil];

  if ([notes_text_view_ isFirstResponder]) {
    [self scrollNotesEditorIntoViewAnimated:YES];
  }
}

- (void)keyboardWillHide:(NSNotification*)notification {
  NSDictionary* user_info = notification.userInfo;
  NSTimeInterval duration = [user_info[UIKeyboardAnimationDurationUserInfoKey] doubleValue];
  UIViewAnimationOptions options =
      (UIViewAnimationOptions)([user_info[UIKeyboardAnimationCurveUserInfoKey] integerValue] << 16);
  [UIView animateWithDuration:duration
                        delay:0.0
                      options:options
                   animations:^{
                     UIEdgeInsets content_inset = self.tableView.contentInset;
                     content_inset.bottom = 0.0;
                     self.tableView.contentInset = content_inset;
                     self.tableView.scrollIndicatorInsets = content_inset;
                   }
                   completion:nil];
}

- (void)textViewDidBeginEditing:(UITextView*)textView {
  if (textView != notes_text_view_) {
    return;
  }
  self.navigationItem.rightBarButtonItem = keyboard_done_button_;
  [self scrollNotesEditorIntoViewAnimated:YES];
}

- (void)textViewDidChange:(UITextView*)textView {
  if (textView == notes_text_view_) {
    notes_placeholder_label_.hidden = (textView.text.length > 0);
  }
}

- (void)textViewDidEndEditing:(UITextView*)textView {
  if (textView == notes_text_view_) {
    self.navigationItem.rightBarButtonItem = nil;
  }
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    // Option-button + card borders are CGColor (frozen at assignment). Cells
    // are rebuilt on reload, which re-resolves XeniaTheme via the new trait
    // collection.
    [self.tableView reloadData];
  }
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  if (!notes_text_view_ || !notes_placeholder_label_) {
    return;
  }
  UIEdgeInsets insets = notes_text_view_.textContainerInset;
  CGFloat line_padding = notes_text_view_.textContainer.lineFragmentPadding;
  CGFloat available_width =
      CGRectGetWidth(notes_text_view_.bounds) - insets.left - insets.right - (line_padding * 2.0);
  if (available_width > 0.0) {
    notes_placeholder_label_.preferredMaxLayoutWidth = floor(available_width);
  }
}

- (void)reportStatusButtonTapped:(UIButton*)sender {
  selected_status_ = sender.tag;
  // The server only accepts the "n/a" performance tier when the status is
  // "nothing". Force it for "nothing", and clear any stale "n/a" selection when
  // switching to another status so the user is required to pick a real tier.
  if (selected_status_ == 4) {
    selected_perf_ = 3;
  } else if (selected_perf_ == 3) {
    selected_perf_ = -1;
  }
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(2, 2)]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (void)reportPerfButtonTapped:(UIButton*)sender {
  if (selected_status_ == 4 && sender.tag != 3) {
    return;
  }
  // "n/a" performance is only valid for the "nothing" status.
  if (selected_status_ != 4 && sender.tag == 3) {
    return;
  }
  selected_perf_ = sender.tag;
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:3]
                withRowAnimation:UITableViewRowAnimationNone];
}

- (void)dismissKeyboard {
  [notes_text_view_ resignFirstResponder];
}

- (void)showAlertWithTitle:(NSString*)title message:(NSString*)message {
  XEPresentOKAlert(self, title, message);
}

- (void)finishSuccessfulSubmissionWithIssueURL:(NSString*)issue_url
                                    compatInfo:(NSDictionary*)compat_info
                            discussionSnapshot:(NSDictionary*)discussion_snapshot {
  NSMutableDictionary* compat_user_info = [NSMutableDictionary dictionaryWithObject:@(title_id_)
                                                                             forKey:@"titleId"];
  if (compat_info) {
    compat_user_info[@"compatInfo"] = compat_info;
  }
  [[NSNotificationCenter defaultCenter] postNotificationName:kXeniaCompatDataDidUpdateNotification
                                                      object:nil
                                                    userInfo:compat_user_info];

  NSMutableDictionary* discussion_user_info = [NSMutableDictionary dictionaryWithObject:@(title_id_)
                                                                                 forKey:@"titleId"];
  if (discussion_snapshot) {
    discussion_user_info[@"discussion"] = discussion_snapshot;
  }
  [[NSNotificationCenter defaultCenter] postNotificationName:kXeniaDiscussionDidUpdateNotification
                                                      object:nil
                                                    userInfo:discussion_user_info];

  NSString* message = @"Your compatibility report has been submitted.";
  if ([issue_url isKindOfClass:[NSString class]] && issue_url.length > 0) {
    message = [message stringByAppendingFormat:@"\n\nGitHub issue: %@", issue_url];
  }

  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:@"Report Submitted"
                                          message:message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:@"OK"
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 if (self.navigationController) {
                                   [self.navigationController popViewControllerAnimated:YES];
                                 } else {
                                   [self dismissViewControllerAnimated:YES completion:nil];
                                 }
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

- (NSString*)trimmedNotes {
  return [notes_text_view_.text
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

- (void)submitReport {
  if (submitting_) {
    return;
  }
  if (selected_status_ < 0) {
    [self showAlertWithTitle:@"Missing Status" message:@"Please select a compatibility status."];
    return;
  }
  if (selected_perf_ < 0) {
    [self showAlertWithTitle:@"Missing Performance" message:@"Please select a performance tier."];
    return;
  }

  NSString* notes = [self trimmedNotes];
  if (notes.length == 0) {
    [self showAlertWithTitle:@"Missing Notes"
                     message:@"Please add a short note about your experience."];
    return;
  }

  submitting_ = YES;
  [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:6]
                withRowAnimation:UITableViewRowAnimationNone];

  NSString* status = xe_compat_statuses()[selected_status_];
  NSString* perf = xe_compat_perfs()[selected_perf_];
  [XeniaCompatReportSubmission
      submitReportForTitleID:title_id_
                       title:game_title_
                      status:status
                        perf:perf
                       notes:notes
                 screenshots:screenshots_
                  completion:^(NSString* issue_url, NSDictionary* compat_info,
                               NSDictionary* discussion_snapshot, NSString* error_title,
                               NSString* error_message) {
                    self->submitting_ = NO;
                    [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:6]
                                  withRowAnimation:UITableViewRowAnimationNone];

                    if (error_title.length > 0) {
                      [self showAlertWithTitle:error_title message:error_message ?: @""];
                      return;
                    }

                    [self finishSuccessfulSubmissionWithIssueURL:issue_url
                                                      compatInfo:compat_info
                                              discussionSnapshot:discussion_snapshot];
                  }];
}

- (void)addScreenshotTapped {
  if (screenshots_.count >= 5) {
    [self showAlertWithTitle:@"Limit Reached" message:@"You can attach up to 5 screenshots."];
    return;
  }

  PHPickerConfiguration* configuration = [[PHPickerConfiguration alloc] init];
  configuration.selectionLimit = static_cast<NSInteger>(5 - screenshots_.count);
  configuration.filter = [PHPickerFilter imagesFilter];

  PHPickerViewController* picker =
      [[PHPickerViewController alloc] initWithConfiguration:configuration];
  picker.delegate = self;
  [self presentViewController:picker animated:YES completion:nil];
  [picker release];
  [configuration release];
}

#pragma mark - PHPickerViewControllerDelegate

- (void)picker:(PHPickerViewController*)picker didFinishPicking:(NSArray<PHPickerResult*>*)results {
  [picker dismissViewControllerAnimated:YES completion:nil];

  NSUInteger available_slots = screenshots_.count >= 5 ? 0 : 5 - screenshots_.count;
  [XeniaCompatReportSubmission
      loadSanitizedScreenshotsFromPickerResults:results
                                 availableSlots:available_slots
                                     completion:^(NSArray<UIImage*>* images) {
                                       BOOL added = NO;
                                       for (UIImage* image in images) {
                                         if (self->screenshots_.count >= 5) {
                                           break;
                                         }
                                         [self->screenshots_ addObject:image];
                                         added = YES;
                                       }
                                       if (added) {
                                         [self.tableView
                                             reloadSections:[NSIndexSet indexSetWithIndex:5]
                                           withRowAnimation:UITableViewRowAnimationAutomatic];
                                       }
                                     }];
}

#pragma mark - UITableViewDataSource

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 7;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  switch (section) {
    case 0:
      return 2;
    case 1:
      return 5;
    case 2:
      return 1;
    case 3:
      return 1;
    case 4:
      return 1;
    case 5:
      return static_cast<NSInteger>(screenshots_.count) + 1;
    case 6:
      return 1;
    default:
      return 0;
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  switch (section) {
    case 0:
      return @"Game";
    case 1:
      return @"Environment";
    case 2:
      return @"Compatibility Status";
    case 3:
      return @"Performance";
    case 4:
      return @"Notes";
    case 5:
      return @"Screenshots";
    default:
      return nil;
  }
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section != 1) {
    return nil;
  }
  NSDictionary* build_info = xe_current_compat_report_build_info();
  NSString* build_label = xe_user_facing_build_label(build_info);
  return build_label.length > 0
             ? [NSString stringWithFormat:@"Reports are tagged as %@.", build_label]
             : nil;
}

- (CGFloat)tableView:(UITableView* __unused)tableView
    heightForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 4) {
    return 128.0;
  }
  if (indexPath.section == 6) {
    return 52.0;
  }
  return UITableViewAutomaticDimension;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 0 || indexPath.section == 1) {
    if (indexPath.section == 0) {
      return [XeniaCompatReportCells gameCellWithTitle:game_title_
                                               titleID:title_id_
                                                   row:indexPath.row];
    }
    return [XeniaCompatReportCells environmentCellForRow:indexPath.row];
  }

  if (indexPath.section == 2 || indexPath.section == 3) {
    return [XeniaCompatReportCells optionsCellForSection:indexPath.section
                                          selectedStatus:selected_status_
                                            selectedPerf:selected_perf_
                                                  target:self
                                            statusAction:@selector(reportStatusButtonTapped:)
                                              perfAction:@selector(reportPerfButtonTapped:)];
  }

  if (indexPath.section == 4) {
    UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                    reuseIdentifier:nil] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    if (!notes_text_view_) {
      notes_text_view_ = [[UITextView alloc] init];
      notes_text_view_.delegate = self;
      notes_text_view_.backgroundColor = [UIColor clearColor];
      notes_text_view_.textColor = [XeniaTheme textPrimary];
      notes_text_view_.textContainerInset = UIEdgeInsetsMake(8, 4, 8, 4);
      xe_apply_text_view_font(notes_text_view_, UIFontTextStyleBody, 15.0, UIFontWeightRegular, NO);

      notes_placeholder_label_ = [[UILabel alloc] init];
      notes_placeholder_label_.translatesAutoresizingMaskIntoConstraints = NO;
      notes_placeholder_label_.text = @"Describe your experience (e.g. crashes, graphical "
                                      @"glitches, audio issues, performance drops)...";
      notes_placeholder_label_.textColor = [XeniaTheme textMuted];
      notes_placeholder_label_.numberOfLines = 0;
      notes_placeholder_label_.lineBreakMode = NSLineBreakByWordWrapping;
      notes_placeholder_label_.userInteractionEnabled = NO;
      xe_apply_label_font(notes_placeholder_label_, UIFontTextStyleBody, 15.0, UIFontWeightRegular);
    }

    if (notes_text_view_.superview != cell.contentView) {
      notes_text_view_.translatesAutoresizingMaskIntoConstraints = NO;
      [cell.contentView addSubview:notes_text_view_];
      [NSLayoutConstraint activateConstraints:@[
        [notes_text_view_.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:8],
        [notes_text_view_.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor
                                                      constant:-8],
        [notes_text_view_.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor
                                                       constant:8],
        [notes_text_view_.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor
                                                        constant:-8],
      ]];
    }
    if (notes_placeholder_label_.superview != cell.contentView) {
      UIEdgeInsets insets = notes_text_view_.textContainerInset;
      CGFloat line_padding = notes_text_view_.textContainer.lineFragmentPadding;
      [cell.contentView addSubview:notes_placeholder_label_];
      [NSLayoutConstraint activateConstraints:@[
        [notes_placeholder_label_.topAnchor constraintEqualToAnchor:notes_text_view_.topAnchor
                                                           constant:insets.top],
        [notes_placeholder_label_.leadingAnchor
            constraintEqualToAnchor:notes_text_view_.leadingAnchor
                           constant:insets.left + line_padding],
        [notes_placeholder_label_.trailingAnchor
            constraintEqualToAnchor:notes_text_view_.trailingAnchor
                           constant:-(insets.right + line_padding)],
      ]];
    }
    notes_placeholder_label_.hidden = (notes_text_view_.text.length > 0);
    return cell;
  }

  if (indexPath.section == 5) {
    if (indexPath.row < static_cast<NSInteger>(screenshots_.count)) {
      return [XeniaCompatReportCells screenshotCellWithImage:screenshots_[indexPath.row]
                                                       index:indexPath.row];
    }

    return [XeniaCompatReportCells addScreenshotCell];
  }

  return [XeniaCompatReportCells submitCellSubmitting:submitting_];
}

#pragma mark - UITableViewDelegate

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];

  if (indexPath.section == 4) {
    [notes_text_view_ becomeFirstResponder];
    return;
  }

  if (indexPath.section == 5) {
    if (indexPath.row >= static_cast<NSInteger>(screenshots_.count)) {
      [self addScreenshotTapped];
    }
    return;
  }

  if (indexPath.section == 6) {
    [self submitReport];
  }
}

- (BOOL)tableView:(UITableView* __unused)tableView canEditRowAtIndexPath:(NSIndexPath*)indexPath {
  return (indexPath.section == 5 && indexPath.row < static_cast<NSInteger>(screenshots_.count));
}

- (void)tableView:(UITableView*)tableView
    commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
     forRowAtIndexPath:(NSIndexPath*)indexPath {
  if (editingStyle != UITableViewCellEditingStyleDelete) {
    return;
  }
  if (indexPath.section != 5 || indexPath.row >= static_cast<NSInteger>(screenshots_.count)) {
    return;
  }
  [screenshots_ removeObjectAtIndex:indexPath.row];
  [tableView reloadSections:[NSIndexSet indexSetWithIndex:5]
           withRowAnimation:UITableViewRowAnimationAutomatic];
}

@end
