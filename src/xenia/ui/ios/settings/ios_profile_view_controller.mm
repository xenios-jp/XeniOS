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
    self.title = @"Profiles";
  }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.separatorInset = UIEdgeInsetsMake(0, 16, 0, 16);
  if (shows_dismiss_button_) {
    self.navigationItem.leftBarButtonItem =
        [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                       target:self
                                                       action:@selector(doneTapped:)]
            autorelease];
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
  return 2;
}

- (NSInteger)tableView:(UITableView* __unused)tableView numberOfRowsInSection:(NSInteger)section {
  if (section == 0) {
    return 1;
  }
  return static_cast<NSInteger>(profiles_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForHeaderInSection:(NSInteger)section {
  if (section == 0) {
    return @"Actions";
  }
  return @"Local Profiles";
}

- (NSString*)tableView:(UITableView* __unused)tableView titleForFooterInSection:(NSInteger)section {
  if (section == 0) {
    return @"Create and sign in profiles used by Xbox Live emulation.";
  }
  if (profiles_.empty()) {
    if (app_context_ && !app_context_->ProfileServicesReady()) {
      return @"Loading profiles...";
    }
    return @"No profiles created yet.";
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaProfileCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                  reuseIdentifier:kCellIdentifier];
  }

  cell.textLabel.numberOfLines = 1;
  cell.detailTextLabel.numberOfLines = 1;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.accessoryView = nil;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;

  if (indexPath.section == 0) {
    cell.textLabel.text = @"Create Profile";
    cell.textLabel.textColor = [XeniaTheme accent];
    cell.detailTextLabel.text = @"Create a new profile and sign in.";
    cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
  }

  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(profiles_.size())) {
    cell.textLabel.text = @"";
    cell.detailTextLabel.text = @"";
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }

  const auto& profile = profiles_[indexPath.row];
  NSString* gamertag = ToNSString(profile.gamertag);
  cell.textLabel.text = gamertag;
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  if (profile.signed_in) {
    cell.detailTextLabel.text =
        [NSString stringWithFormat:@"Signed in on slot %u", profile.signed_in_slot];
    cell.accessoryType = UITableViewCellAccessoryCheckmark;
  } else {
    cell.detailTextLabel.text = @"Not signed in";
  }
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  return cell;
}

- (void)presentCreateProfileAlert {
  if (!app_context_) {
    return;
  }
  if (!app_context_->ProfileServicesReady()) {
    app_context_->PrepareProfileServices();
    if (on_status_) {
      on_status_(@"Initializing profile services...");
    }
    [self scheduleProfileServicesReload];
    return;
  }

  __block UIAlertController* create_alert =
      [UIAlertController alertControllerWithTitle:@"Create Profile"
                                          message:@"Enter a gamertag (1-15 characters)."
                                   preferredStyle:UIAlertControllerStyleAlert];
  [create_alert addTextFieldWithConfigurationHandler:^(UITextField* text_field) {
    text_field.placeholder = @"Gamertag";
    text_field.autocapitalizationType = UITextAutocapitalizationTypeWords;
    text_field.autocorrectionType = UITextAutocorrectionTypeNo;
    text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
  }];
  [create_alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                   style:UIAlertActionStyleCancel
                                                 handler:nil]];
  [create_alert
      addAction:[UIAlertAction
                    actionWithTitle:@"Create"
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
                                  on_status_(@"Failed to create profile.");
                                }
                                return;
                              }
                              if (on_status_) {
                                on_status_(
                                    [NSString stringWithFormat:@"Signed in as %@.", gamertag]);
                              }
                              [self reloadProfiles];
                            }]];
  [self presentViewController:create_alert animated:YES completion:nil];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (!app_context_) {
    return;
  }

  if (indexPath.section == 0) {
    [self presentCreateProfileAlert];
    return;
  }

  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(profiles_.size())) {
    return;
  }

  const auto& profile = profiles_[indexPath.row];
  if (app_context_->SignInProfile(profile.xuid)) {
    if (on_status_) {
      on_status_([NSString stringWithFormat:@"Signed in as %@.", ToNSString(profile.gamertag)]);
    }
    [self reloadProfiles];
  } else if (on_status_) {
    on_status_(@"Failed to sign in profile.");
  }
}

@end
