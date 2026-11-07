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

#import "xenia/ui/ios/settings/ios_live_account_view_controllers.h"
#import "xenia/ui/ios/shared/ios_live_account.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

// New Profile, as drawn: ✕ dismisses, ✓ creates. One field for the gamertag.
@interface XeniaNewProfileViewController : XESheetTableViewController <UITextFieldDelegate>
- (instancetype)initWithCreate:(void (^)(NSString* gamertag))create;
@end

@implementation XeniaNewProfileViewController {
  void (^create_)(NSString* gamertag);
  UIButton* create_button_;  // Owned by the commit helper.
  UITextField* field_;
}

- (instancetype)initWithCreate:(void (^)(NSString* gamertag))create {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.glassStyle = YES;
    create_ = [create copy];
    self.title = NSLocalizedString(@"New Profile", @"new profile sheet: title");
  }
  return self;
}

- (void)dealloc {
  [create_ release];
  [field_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor clearColor];
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  XESetSheetClose(self, self, @selector(closeTapped:));
  create_button_ = XESetFormCommit(
      self, NSLocalizedString(@"Create Profile", @"profile view controller: button"), self,
      @selector(doneTapped:));
  field_ = [[UITextField alloc] init];
  field_.placeholder = NSLocalizedString(@"Gamertag", @"profile view controller: placeholder");
  field_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  field_.adjustsFontForContentSizeCategory = YES;
  field_.textColor = [XeniaTheme textPrimary];
  field_.autocapitalizationType = UITextAutocapitalizationTypeWords;
  field_.autocorrectionType = UITextAutocorrectionTypeNo;
  field_.clearButtonMode = UITextFieldViewModeWhileEditing;
  field_.returnKeyType = UIReturnKeyDone;
  field_.delegate = self;
  [field_ addTarget:self
                action:@selector(textChanged:)
      forControlEvents:UIControlEventEditingChanged];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [field_ becomeFirstResponder];
}

- (NSString*)gamertag {
  return [field_.text
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

- (void)textChanged:(UITextField*)__unused sender {
  create_button_.enabled = [self gamertag].length > 0;
}

- (BOOL)textField:(UITextField*)textField
    shouldChangeCharactersInRange:(NSRange)range
                replacementString:(NSString*)string {
  // A gamertag is at most 15 characters.
  return [textField.text stringByReplacingCharactersInRange:range withString:string].length <= 15;
}

- (BOOL)textFieldShouldReturn:(UITextField*)__unused textField {
  [self doneTapped:nil];
  return NO;
}

- (void)closeTapped:(id)__unused sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)doneTapped:(id)__unused sender {
  NSString* gamertag = [[[self gamertag] copy] autorelease];
  if (!gamertag.length) {
    return;
  }
  void (^create)(NSString*) = [[create_ copy] autorelease];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             create(gamertag);
                           }];
}

- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger __unused)section {
  return 1;
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForFooterInSection:(NSInteger __unused)section {
  return NSLocalizedString(@"Up to 15 characters. Games show it to other players.",
                           @"new profile sheet: footer");
}

- (UITableViewCell*)tableView:(UITableView* __unused)tableView
        cellForRowAtIndexPath:(NSIndexPath* __unused)indexPath {
  UITableViewCell* cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                                  reuseIdentifier:nil] autorelease];
  cell.selectionStyle = UITableViewCellSelectionStyleNone;
  field_.translatesAutoresizingMaskIntoConstraints = NO;
  [cell.contentView addSubview:field_];
  [NSLayoutConstraint activateConstraints:@[
    [field_.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
    [field_.trailingAnchor
        constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
    [field_.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor],
    [field_.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor],
    [field_.heightAnchor constraintGreaterThanOrEqualToConstant:52.0],
  ]];
  return cell;
}

@end

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
    self.glassStyle = YES;
    app_context_ = app_context;
    on_status_ = [on_status copy];
    shows_dismiss_button_ = YES;
    self.title = NSLocalizedString(@"Profiles", @"profile view controller: title");
  }
  return self;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor clearColor];
  if (shows_dismiss_button_) {
    XESetSheetClose(self, self, @selector(doneTapped:));
  }
  // + adds a profile, just before ✕.
  UIBarButtonItem* add =
      [[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"plus"]
                                        style:UIBarButtonItemStylePlain
                                       target:self
                                       action:@selector(addTapped:)] autorelease];
  add.accessibilityLabel = NSLocalizedString(@"Add Profile", @"profile view controller: button");
  XESetSheetActions(self, @[ add ]);
}

- (void)addTapped:(id __unused)sender {
  [self presentCreateProfileAlert];
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
  [self setNeedsUpdateContentUnavailableConfiguration];
  if (app_context_ && !app_context_->ProfileServicesReady()) {
    [self scheduleProfileServicesReload];
  }
}

- (BOOL)showsEmptyState {
  return profiles_.empty() && (!app_context_ || app_context_->ProfileServicesReady());
}

// Without a profile the sheet is one centred message and the button that adds one.
- (void)updateContentUnavailableConfigurationUsingState:(UIContentUnavailableConfigurationState*)
                                                            __unused state {
  if (![self showsEmptyState]) {
    self.contentUnavailableConfiguration = nil;
    return;
  }
  UIContentUnavailableConfiguration* config = [UIContentUnavailableConfiguration emptyConfiguration];
  config.image = [UIImage systemImageNamed:@"person.crop.circle"];
  config.text = NSLocalizedString(@"No Profiles", @"profile view controller: empty state title");
  config.secondaryText =
      NSLocalizedString(@"Games save progress and achievements to the signed-in profile.",
                        @"profile view controller: footer");
  UIButtonConfiguration* button = [UIButtonConfiguration filledButtonConfiguration];
  button.title = NSLocalizedString(@"Add Profile", @"profile view controller: empty state button");
  button.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  button.baseBackgroundColor = [UIColor whiteColor];
  button.baseForegroundColor = [UIColor blackColor];
  button.contentInsets = NSDirectionalEdgeInsetsMake(10.0, 22.0, 10.0, 22.0);
  button.titleTextAttributesTransformer =
      ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        return out;
      };
  config.button = button;
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  config.buttonProperties.primaryAction =
      [UIAction actionWithHandler:^(__unused UIAction* action) {
        [unsafe_self presentCreateProfileAlert];
      }];
  self.contentUnavailableConfiguration = config;
}

// The signed-in profile's name, or nil.
- (NSString*)activeGamertag {
  for (const auto& profile : profiles_) {
    if (profile.signed_in) {
      return ToNSString(profile.gamertag);
    }
  }
  return nil;
}

// A second section, Online, for the signed-in profile: Online Account, and Move Profile once
// the account is online.
- (BOOL)showsAccountSection {
  return xe_ios_live_account_ui_enabled() && ![self showsEmptyState] && [self activeGamertag] != nil;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return [self showsAccountSection] ? 2 : 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger)section {
  if ([self showsEmptyState]) {
    return 0;
  }
  if (section == 1) {
    return xe_ios_live_account_service().summary.online ? 2 : 1;
  }
  return static_cast<NSInteger>(profiles_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)section {
  if ([self showsEmptyState]) {
    return nil;
  }
  if (section == 1) {
    return NSLocalizedString(@"Online", @"profile view controller: section");
  }
  return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad
             ? NSLocalizedString(@"On this iPad", @"profile view controller: section")
             : NSLocalizedString(@"On this iPhone", @"profile view controller: section");
}

// The footer closes the card, under its last group.
- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
  if ([self showsEmptyState] || section != [self numberOfSectionsInTableView:tableView] - 1) {
    return nil;
  }
  if (profiles_.empty() && app_context_ && !app_context_->ProfileServicesReady()) {
    return NSLocalizedString(@"Loading profiles...",
                             @"profile view controller: user-visible text");
  }
  return NSLocalizedString(@"Games save progress and achievements to the signed-in profile.",
                           @"profile view controller: footer");
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section == 1 && indexPath.row == 1) {
    UITableViewCell* move = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                                    reuseIdentifier:nil] autorelease];
    move.textLabel.text = NSLocalizedString(@"Move Profile", @"profile view controller: row");
    move.textLabel.font = xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
    move.textLabel.textColor = [XeniaTheme textPrimary];
    move.detailTextLabel.text = NSLocalizedString(@"To another device, as a file",
                                                  @"profile view controller: row detail");
    move.detailTextLabel.font =
        xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular);
    move.detailTextLabel.textColor = [XeniaTheme textSecondary];
    move.imageView.image = XERowSymbolImage(@"square.and.arrow.up", nil);
    move.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    XEApplyAccessibility(move, move.textLabel.text, move.detailTextLabel.text, nil,
                         UIAccessibilityTraitButton);
    return move;
  }
  if (indexPath.section == 1) {
    UITableViewCell* account = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                                       reuseIdentifier:nil] autorelease];
    XeniaLiveAccountSummary* summary = xe_ios_live_account_service().summary;
    account.textLabel.text = NSLocalizedString(@"Online Account", @"profile view controller: row");
    account.textLabel.font = xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
    account.textLabel.textColor = [XeniaTheme textPrimary];
    account.detailTextLabel.text =
        summary.online ? summary.gamertag
                       : NSLocalizedString(@"Play online with friends", @"profile view controller: row detail");
    account.detailTextLabel.font = xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular);
    account.detailTextLabel.textColor = [XeniaTheme textSecondary];
    account.imageView.image = XERowSymbolImage(@"globe", nil);
    account.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    XEApplyAccessibility(account, account.textLabel.text, account.detailTextLabel.text, nil,
                         UIAccessibilityTraitButton);
    return account;
  }
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
  cell.imageView.image = xe_profile_avatar_image(gamertag, profile.signed_in, 36.0);
  cell.textLabel.font = xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightSemibold);
  cell.detailTextLabel.font =
      xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular);
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

  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  XeniaNewProfileViewController* new_profile = [[[XeniaNewProfileViewController alloc]
      initWithCreate:^(NSString* gamertag) {
        [unsafe_self createProfileWithGamertag:gamertag];
      }] autorelease];
  UINavigationController* navigation =
      [[[UINavigationController alloc] initWithRootViewController:new_profile] autorelease];
  navigation.modalPresentationStyle = UIModalPresentationFormSheet;
  // Tall enough for the bar, the field, its footer and Create Profile under them; the keyboard
  // lifts it.
  navigation.sheetPresentationController.detents = @[ [UISheetPresentationControllerDetent
      customDetentWithIdentifier:@"xe.new-profile"
                        resolver:^CGFloat(
                            id<UISheetPresentationControllerDetentResolutionContext> __unused
                                context) {
                          return 276.0;
                        }] ];
  [self presentViewController:navigation animated:YES completion:nil];
}

- (void)createProfileWithGamertag:(NSString*)gamertag {
  if (!app_context_) {
    return;
  }
  uint64_t xuid = app_context_->CreateProfile(std::string([gamertag UTF8String]));
  if (!xuid || !app_context_->SignInProfile(xuid)) {
    if (on_status_) {
      on_status_(NSLocalizedString(@"Failed to create profile.",
                                   @"profile view controller: user-visible text"));
    }
    return;
  }
  if (on_status_) {
    on_status_([NSString
        stringWithFormat:NSLocalizedString(@"Signed in as %@.",
                                           @"profile view controller: user-visible text"),
                         gamertag]);
  }
  [self reloadProfiles];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (!app_context_) {
    return;
  }

  if (indexPath.section == 1 && indexPath.row == 1) {
    xe_ios_present_move_profile(self);
    return;
  }
  if (indexPath.section == 1) {
    NSString* gamertag = [self activeGamertag];
    if (gamertag) {
      [self.navigationController
          pushViewController:xe_ios_new_account_view_controller(gamertag)
                    animated:YES];
    }
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
