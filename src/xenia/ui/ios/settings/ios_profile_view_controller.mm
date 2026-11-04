/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_profile_view_controller.h"

#include <vector>

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@implementation XeniaProfileViewController {
  xe::ui::IOSWindowedAppContext* app_context_;
  IOSProfileStatusHandler on_status_;
  BOOL shows_dismiss_button_;
  BOOL profile_reload_scheduled_;
  std::vector<xe::ui::IOSProfileSummary> profiles_;
}

@synthesize showsDismissButton = shows_dismiss_button_;

- (instancetype)initWithAppContext:(xe::ui::IOSWindowedAppContext*)app_context
                          onStatus:(IOSProfileStatusHandler)on_status {
  self = [super initWithStyle:UITableViewStyleInsetGrouped];
  if (self) {
    app_context_ = app_context;
    on_status_ = [on_status copy];
    shows_dismiss_button_ = YES;
    self.title = NSLocalizedString(@"Profiles", @"profile view controller: title");
  }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  if (shows_dismiss_button_) {
    self.navigationItem.leftBarButtonItem = [[[UIBarButtonItem alloc]
        initWithTitle:NSLocalizedString(@"Close", @"profile view controller: button")
                style:UIBarButtonItemStylePlain
               target:self
               action:@selector(doneTapped:)] autorelease];
  }
}

- (void)scheduleProfileServicesReload {
  if (profile_reload_scheduled_ || !app_context_ || app_context_->ProfileServicesReady()) {
    return;
  }

  profile_reload_scheduled_ = YES;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), dispatch_get_main_queue(), ^{
    self->profile_reload_scheduled_ = NO;
    if (!self->app_context_ || !self.view.window) {
      return;
    }
    [self reloadProfiles];
  });
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  if (app_context_ && !app_context_->ProfileServicesReady()) {
    app_context_->PrepareProfileServices();
  }
  [self reloadProfiles];
}

- (void)doneTapped:(id)sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)reloadProfiles {
  profiles_.clear();
  if (app_context_) {
    if (!app_context_->ProfileServicesReady()) {
      app_context_->PrepareProfileServices();
    }
    profiles_ = app_context_->ListProfiles();
  }
  [self.tableView reloadData];
  if (app_context_ && !app_context_->ProfileServicesReady()) {
    [self scheduleProfileServicesReload];
  }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

// The profiles, then a last row that adds one.
- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger __unused)section {
  return static_cast<NSInteger>(profiles_.size()) + 1;
}

- (BOOL)isAddRow:(NSIndexPath*)indexPath {
  return indexPath.row == static_cast<NSInteger>(profiles_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger __unused)section {
  if (profiles_.empty() && app_context_ && !app_context_->ProfileServicesReady()) {
    return NSLocalizedString(@"Loading profiles...",
                             @"profile view controller: user-visible text");
  }
  return NSLocalizedString(@"Games save progress and achievements to the signed-in profile.",
                           @"profile view controller: footer");
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaProfileCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
  }

  cell.textLabel.numberOfLines = 1;
  cell.detailTextLabel.numberOfLines = 1;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.accessoryView = nil;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;

  if ([self isAddRow:indexPath]) {
    cell.textLabel.text = NSLocalizedString(@"Add Profile…", @"profile view controller: button");
    cell.textLabel.textColor = [XeniaTheme accent];
    cell.detailTextLabel.text = nil;
    cell.imageView.image = XESettingsBadgeImage(@"plus", [XeniaTheme accent]);
    XEApplyAccessibility(cell, cell.textLabel.text, nil, nil, UIAccessibilityTraitButton);
    return cell;
  }

  const auto& profile = profiles_[indexPath.row];
  NSString* gamertag = ToNSString(profile.gamertag);
  cell.textLabel.text = gamertag;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  if (profile.signed_in) {
    cell.detailTextLabel.text =
        profile.signed_in_slot
            ? [NSString stringWithFormat:NSLocalizedString(@"Signed In as Player %u",
                                                           @"profile view controller: detail text"),
                                         profile.signed_in_slot + 1]
            : NSLocalizedString(@"Signed In", @"profile view controller: detail text");
    cell.accessoryType = UITableViewCellAccessoryCheckmark;
  } else {
    cell.detailTextLabel.text =
        NSLocalizedString(@"Tap to sign in", @"profile view controller: detail text");
  }
  cell.imageView.image = XESettingsBadgeImage(
      @"person.fill", profile.signed_in ? [XeniaTheme accent] : [UIColor systemGrayColor]);
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  XEApplyAccessibility(cell, gamertag, cell.detailTextLabel.text, nil,
                       profile.signed_in ? (UIAccessibilityTraitButton | UIAccessibilityTraitSelected)
                                         : UIAccessibilityTraitButton);
  return cell;
}

- (void)presentCreateProfileAlert {
  if (!app_context_) {
    return;
  }
  if (!app_context_->ProfileServicesReady()) {
    app_context_->PrepareProfileServices();
    if (on_status_) {
      on_status_(NSLocalizedString(@"Initializing profile services...",
                                   @"profile view controller: user-visible text"));
    }
    [self scheduleProfileServicesReload];
    return;
  }

  __block UIAlertController* create_alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Add Profile",
                                                 @"profile view controller: title")
                       message:NSLocalizedString(@"Choose a gamertag of up to 15 characters.",
                                                 @"profile view controller: message")
                preferredStyle:UIAlertControllerStyleAlert];
  [create_alert addTextFieldWithConfigurationHandler:^(UITextField* text_field) {
    text_field.placeholder =
        NSLocalizedString(@"Gamertag", @"profile view controller: placeholder");
    text_field.autocapitalizationType = UITextAutocapitalizationTypeWords;
    text_field.autocorrectionType = UITextAutocorrectionTypeNo;
    text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
  }];
  [create_alert
      addAction:[UIAlertAction
                    actionWithTitle:NSLocalizedString(@"Cancel", @"profile view controller: title")
                              style:UIAlertActionStyleCancel
                            handler:nil]];
  [create_alert
      addAction:[UIAlertAction
                    actionWithTitle:NSLocalizedString(@"Create", @"profile view controller: title")
                              style:UIAlertActionStyleDefault
                            handler:^(__unused UIAlertAction* action) {
                              UITextField* text_field = create_alert.textFields.firstObject;
                              NSString* raw_text = text_field.text ?: @"";
                              NSString* trimmed =
                                  [raw_text stringByTrimmingCharactersInSet:
                                                [NSCharacterSet whitespaceAndNewlineCharacterSet]];
                              if (trimmed.length == 0 || !app_context_) {
                                return;
                              }
                              NSString* gamertag = [[trimmed copy] autorelease];
                              create_alert = nil;
                              uint64_t xuid =
                                  app_context_->CreateProfile(std::string([gamertag UTF8String]));
                              if (!xuid || !app_context_->SignInProfile(xuid)) {
                                if (on_status_) {
                                  on_status_(NSLocalizedString(
                                      @"Failed to create profile.",
                                      @"profile view controller: user-visible text"));
                                }
                                return;
                              }
                              if (on_status_) {
                                on_status_([NSString
                                    stringWithFormat:
                                        NSLocalizedString(
                                            @"Signed in as %@.",
                                            @"profile view controller: user-visible text"),
                                        gamertag]);
                              }
                              [self reloadProfiles];
                            }]];
  create_alert.preferredAction = create_alert.actions.lastObject;
  [self presentViewController:create_alert animated:YES completion:nil];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (!app_context_) {
    return;
  }

  if ([self isAddRow:indexPath]) {
    [self presentCreateProfileAlert];
    return;
  }

  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(profiles_.size())) {
    return;
  }

  const auto& profile = profiles_[indexPath.row];
  if (app_context_->SignInProfile(profile.xuid)) {
    if (on_status_) {
      on_status_([NSString
          stringWithFormat:NSLocalizedString(@"Signed in as %@.",
                                             @"profile view controller: user-visible text"),
                           ToNSString(profile.gamertag)]);
    }
    [self reloadProfiles];
  } else if (on_status_) {
    on_status_(NSLocalizedString(@"Failed to sign in profile.",
                                 @"profile view controller: user-visible text"));
  }
}

@end
