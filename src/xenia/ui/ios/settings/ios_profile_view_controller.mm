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

#import "xenia/ui/ios/settings/ios_gamer_picture_view_controller.h"
#import "xenia/ui/ios/settings/ios_live_account_view_controllers.h"
#import "xenia/ui/ios/shared/ios_glass_card.h"
#import "xenia/ui/ios/shared/ios_live_account.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

// New Profile, as drawn: ✕ dismisses, Create Profile creates. The avatar it will have over one
// field for the gamertag.
constexpr CGFloat kAvatarDiameter = 72.0;
constexpr CGFloat kAvatarHeaderHeight = 4.0 + 72.0 + 20.0;

// The avatar before a gamertag: a circle the lettered avatar's size with a head and shoulders cut
// out. (person.crop.circle.fill draws its circle well inside its point size, so it was smaller.)
static UIImage* PlaceholderAvatarImage(CGFloat diameter, UIColor* color) {
  UIGraphicsImageRenderer* renderer = [[[UIGraphicsImageRenderer alloc]
      initWithSize:CGSizeMake(diameter, diameter)] autorelease];
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    const CGFloat d = diameter;
    UIBezierPath* circle = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(0, 0, d, d)];
    [color setFill];
    [circle fill];
    [circle addClip];
    CGContextSetBlendMode(context.CGContext, kCGBlendModeDestinationOut);
    [[UIColor blackColor] setFill];
    [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(d * 0.32, d * 0.18, d * 0.36, d * 0.36)]
        fill];
    [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(d * 0.15, d * 0.62, d * 0.70, d * 0.56)]
        fill];
  }];
}

@interface XeniaNewProfileViewController : XESheetTableViewController <UITextFieldDelegate>
- (instancetype)initWithCreate:(void (^)(NSString* gamertag))create;
@end

@implementation XeniaNewProfileViewController {
  void (^create_)(NSString* gamertag);
  UIButton* create_button_;  // Owned by the commit helper.
  UITextField* field_;
  UIImageView* avatar_;
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
  [avatar_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor clearColor];
  self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
  // Pushed in the Profiles card it goes back with ‹; presented on its own it closes with ✕.
  if (![self isPushed]) {
    XESetSheetClose(self, self, @selector(closeTapped:));
  }
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
  // The profile as it will look, over the field (the form mock): its initials grow in as the
  // name is typed. It fills the space under the title that was empty.
  self.tableView.sectionHeaderTopPadding = 0.0;
  // In the card the table's top is tucked under the bar for its captions; the avatar drops by the
  // same amount so it clears the title.
  const CGFloat avatar_top = XEIsInGlassCard(self) ? 4.0 + 20.0 : 4.0;
  UIView* header = [[[UIView alloc]
      initWithFrame:CGRectMake(0, 0, 320.0, kAvatarHeaderHeight - 4.0 + avatar_top)] autorelease];
  avatar_ = [[UIImageView alloc] init];
  avatar_.translatesAutoresizingMaskIntoConstraints = NO;
  avatar_.isAccessibilityElement = NO;
  [header addSubview:avatar_];
  [NSLayoutConstraint activateConstraints:@[
    [avatar_.centerXAnchor constraintEqualToAnchor:header.centerXAnchor],
    [avatar_.topAnchor constraintEqualToAnchor:header.topAnchor constant:avatar_top],
    [avatar_.widthAnchor constraintEqualToConstant:kAvatarDiameter],
    [avatar_.heightAnchor constraintEqualToConstant:kAvatarDiameter],
  ]];
  self.tableView.tableHeaderView = header;
  [self updateAvatar];
}

- (void)updateAvatar {
  NSString* gamertag = [self gamertag];
  if (gamertag.length) {
    avatar_.image = xe_profile_avatar_image(gamertag, YES, kAvatarDiameter);
    avatar_.tintColor = nil;
  } else {
    avatar_.image = PlaceholderAvatarImage(kAvatarDiameter, [XeniaTheme textMuted]);
    avatar_.tintColor = nil;
  }
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
  [self updateAvatar];
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

- (BOOL)isPushed {
  return self.navigationController.viewControllers.firstObject != self;
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
  if ([self isPushed]) {
    // Back to the list, which now shows the new profile signed in.
    [field_ resignFirstResponder];
    create(gamertag);
    [self.navigationController popViewControllerAnimated:YES];
    return;
  }
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
  NSSet<NSString*>* online_names_;  // Profiles here with an online account, by gamertag.
  NSSet<NSString*>* linked_names_;  // The same, with those whose online play is off.
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

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [online_names_ release];
  [linked_names_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor clearColor];
  if (shows_dismiss_button_) {
    XESetSheetClose(self, self, @selector(doneTapped:));
  }
  // + adds a profile, just before ✕: a new one, or (online) one signed in with a gamertag.
  UIBarButtonItem* add = nil;
  if (xe_ios_live_account_ui_enabled()) {
    __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
    UIMenu* menu = [UIMenu menuWithChildren:@[
      [UIAction actionWithTitle:NSLocalizedString(@"New Profile", @"profile view controller: menu")
                          image:[UIImage systemImageNamed:@"person.badge.plus"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self presentCreateProfileAlert];
                        }],
      [UIAction actionWithTitle:NSLocalizedString(@"I Have a Gamertag",
                                                  @"profile view controller: menu")
                          image:[UIImage systemImageNamed:@"qrcode.viewfinder"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          xe_ios_present_sign_in(unsafe_self, nil);
                        }],
    ]];
    add = [[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"plus"]
                                             menu:menu] autorelease];
  } else {
    add = [[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"plus"]
                                            style:UIBarButtonItemStylePlain
                                           target:self
                                           action:@selector(addTapped:)] autorelease];
  }
  add.accessibilityLabel = NSLocalizedString(@"Add Profile", @"profile view controller: button");
  XESetSheetActions(self, @[ add ]);
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(reloadProfiles)
                                               name:XeniaLiveAccountDidChangeNotification
                                             object:nil];
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
  [self loadOnlineNames];
  [self.tableView reloadData];
  [self setNeedsUpdateContentUnavailableConfiguration];
  if (app_context_ && !app_context_->ProfileServicesReady()) {
    [self scheduleProfileServicesReload];
  }
}

- (void)loadOnlineNames {
  if (!xe_ios_live_account_ui_enabled()) {
    return;
  }
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  [xe_ios_live_account_service()
      localProfilesWithCompletion:^(NSArray<XeniaLiveLocalProfile*>* profiles) {
        NSMutableSet* names = [NSMutableSet set];
        NSMutableSet* linked = [NSMutableSet set];
        for (XeniaLiveLocalProfile* profile in profiles) {
          if (profile.online) {
            [names addObject:profile.gamertag];
          }
          if (profile.linked) {
            [linked addObject:profile.gamertag];
          }
        }
        [unsafe_self->linked_names_ release];
        unsafe_self->linked_names_ = [linked copy];
        if ([names isEqualToSet:unsafe_self->online_names_ ?: [NSSet set]]) {
          return;
        }
        [unsafe_self->online_names_ release];
        unsafe_self->online_names_ = [names copy];
        [unsafe_self.tableView reloadData];
      }];
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

- (NSInteger)numberOfSectionsInTableView:(UITableView* __unused)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView* __unused)tableView
    numberOfRowsInSection:(NSInteger)section {
  if ([self showsEmptyState]) {
    return 0;
  }
  return static_cast<NSInteger>(profiles_.size());
}

- (NSString*)tableView:(UITableView* __unused)tableView
    titleForHeaderInSection:(NSInteger)section {
  if ([self showsEmptyState]) {
    return nil;
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
  const BOOL live = xe_ios_live_account_ui_enabled();
  const BOOL online = live && (profile.signed_in ? xe_ios_live_account_service().summary.online
                                                 : [online_names_ containsObject:gamertag]);
  if (profile.signed_in) {
    NSString* signed_in =
        profile.signed_in_slot
            ? [NSString stringWithFormat:NSLocalizedString(@"Signed In as Player %u",
                                                           @"profile view controller: detail text"),
                                         profile.signed_in_slot + 1]
            : NSLocalizedString(@"Signed In", @"profile view controller: detail text");
    cell.detailTextLabel.text =
        live ? [NSString stringWithFormat:@"%@ · %@", signed_in,
                                          online ? NSLocalizedString(@"Online", @"profile view controller: detail text")
                                                 : NSLocalizedString(@"Offline", @"profile view controller: detail text")]
             : signed_in;
    cell.accessoryType = UITableViewCellAccessoryCheckmark;
  } else {
    cell.detailTextLabel.text =
        live ? (online ? NSLocalizedString(@"Online", @"profile view controller: detail text")
                       : NSLocalizedString(@"Offline", @"profile view controller: detail text"))
             : NSLocalizedString(@"Tap to sign in", @"profile view controller: detail text");
  }
  if (live) {
    // Tap the row to switch to it; ⓘ opens its page (account, devices and data), as Wi-Fi does.
    UIButton* info = [UIButton buttonWithType:UIButtonTypeDetailDisclosure];
    info.tintColor = [XeniaTheme accent];
    info.tag = indexPath.row;
    info.accessibilityLabel = [NSString
        stringWithFormat:NSLocalizedString(@"%@ details", @"profile view controller: ⓘ button; %@ is a gamertag"),
                         gamertag];
    [info addTarget:self action:@selector(infoTapped:) forControlEvents:UIControlEventTouchUpInside];
    // The subtitle already says Signed In, so the row needs no check mark beside ⓘ.
    cell.accessoryView = info;
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
  if (XEIsInGlassCard(self)) {
    // In the Profiles card it is the card's next page, like Add Friend in Friends: the card grows
    // to fit the avatar, field and Create Profile, and ‹ comes back to the list.
    [self.navigationController pushViewController:new_profile animated:YES];
    return;
  }
  UINavigationController* navigation =
      [[[UINavigationController alloc] initWithRootViewController:new_profile] autorelease];
  // Elsewhere (Settings), a form page like the others.
  navigation.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  navigation.view.backgroundColor = [XeniaTheme bgPrimary];
  XEConfigureDestinationPresentation(navigation, self.view, CGSizeMake(620.0, 720.0), NO);
  navigation.navigationBar.prefersLargeTitles = NO;
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

- (void)infoTapped:(UIButton*)sender {
  if (sender.tag < 0 || sender.tag >= static_cast<NSInteger>(profiles_.size())) {
    return;
  }
  const auto profile = profiles_[sender.tag];
  NSString* gamertag = ToNSString(profile.gamertag);
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  const uint64_t xuid = profile.xuid;
  BOOL (^switch_to)(void) = ^BOOL {
    return [unsafe_self signInXuid:xuid gamertag:gamertag];
  };
  void (^change_picture)(UIViewController*) = ^(UIViewController* page) {
    [unsafe_self pushPictureChooserForXuid:xuid from:page];
  };
  void (^delete_profile)(UIViewController*, UIView*) = ^(UIViewController* page, UIView* __unused source) {
    [unsafe_self confirmDeleteXuid:xuid from:page];
  };
  [self.navigationController
      pushViewController:xe_ios_new_profile_page(gamertag, profile.signed_in, switch_to,
                                                 change_picture, delete_profile)
                animated:YES];
}

- (UISwipeActionsConfiguration*)tableView:(UITableView*)__unused tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(profiles_.size())) {
    return nil;
  }
  const uint64_t xuid = profiles_[indexPath.row].xuid;
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  UIContextualAction* remove = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:NSLocalizedString(@"Delete", @"profile view controller: swipe action")
                        handler:^(UIContextualAction* __unused action, UIView* __unused source,
                                  void (^done)(BOOL)) {
                          [unsafe_self confirmDeleteXuid:xuid from:unsafe_self];
                          done(YES);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ remove ]];
}

- (BOOL)isOnlineProfile:(const xe::ui::IOSProfileSummary&)profile {
  if (!xe_ios_live_account_ui_enabled()) {
    return NO;
  }
  return profile.signed_in ? xe_ios_live_account_service().summary.online
                           : [online_names_ containsObject:ToNSString(profile.gamertag)];
}

- (void)showDeleteFailure:(NSString*)message from:(UIViewController*)presenter {
  XEPresentOKAlert(presenter,
                   NSLocalizedString(@"Couldn’t Delete Profile",
                                     @"profile view controller: alert title"),
                   message);
}

- (void)confirmDeleteXuid:(uint64_t)xuid from:(UIViewController*)presenter {
  const xe::ui::IOSProfileSummary* found = nullptr;
  for (const auto& profile : profiles_) {
    if (profile.xuid == xuid) {
      found = &profile;
    }
  }
  if (!found || !app_context_) {
    return;
  }
  if (app_context_->IsTitleOpen()) {
    [self showDeleteFailure:NSLocalizedString(@"Close the game first.",
                                              @"profile view controller: alert message")
                       from:presenter];
    return;
  }
  NSString* gamertag = ToNSString(found->gamertag);
  const BOOL online = [self isOnlineProfile:*found];
  NSString* device = UIDevice.currentDevice.model;
  NSString* message = [NSString
      stringWithFormat:NSLocalizedString(@"Its saves, achievements and settings are deleted from "
                                         @"this %@. This can’t be undone.",
                                         @"Delete profile: message; %@ is the device's kind"),
                       device];
  if (online) {
    message = [NSString
        stringWithFormat:NSLocalizedString(
                             @"Its saves are deleted from this %@ for good.\n\n"
                             @"Delete from This Device keeps the online account and gamertag. "
                             @"If no other device is on the account, only a recovery code can "
                             @"bring it back.\n\n"
                             @"Delete Account also releases the gamertag and signs out other "
                             @"devices.",
                             @"Delete profile: message for an online profile; %@ is the device's "
                             @"kind"),
                         device];
  } else if ([linked_names_ containsObject:gamertag]) {
    // Connecting to leave would take the account from the device using it, or undo Online Play
    // being turned off, so this device stays on the account.
    message = [message
        stringByAppendingFormat:
            @"\n\n%@",
            [NSString stringWithFormat:
                          NSLocalizedString(
                              @"The online account stays, with its gamertag, and this %@ may stay "
                              @"listed on it, as online play is off for this profile. To delete "
                              @"the account, turn on online play first.",
                              @"Delete profile: message; %@ is the device's kind"),
                          device]];
  }
  // Destructive confirmations are action sheets, anchored to the row on iPad.
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:NSLocalizedString(
                                                              @"Delete %@?",
                                                              @"Delete profile: title; %@ is the "
                                                              @"gamertag"),
                                                          gamertag]
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel",
                                                                      @"Delete profile: button")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  [confirm addAction:[UIAlertAction
                         actionWithTitle:online
                                             ? NSLocalizedString(@"Delete from This Device",
                                                                 @"Delete profile: button")
                                             : NSLocalizedString(@"Delete Profile",
                                                                 @"Delete profile: button")
                                   style:UIAlertActionStyleDestructive
                                 handler:^(UIAlertAction* __unused action) {
                                   [unsafe_self deleteXuid:xuid
                                                  gamertag:gamertag
                                                   account:NO
                                                      from:presenter];
                                 }]];
  if (online) {
    [confirm addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Delete Account…",
                                                                        @"Delete profile: button")
                                                style:UIAlertActionStyleDestructive
                                              handler:^(UIAlertAction* __unused action) {
                                                [unsafe_self confirmDeleteAccountOfXuid:xuid
                                                                               gamertag:gamertag
                                                                                   from:presenter];
                                              }]];
  }
  [presenter presentViewController:confirm animated:YES completion:nil];
}

// Asked again: unlike the profile's files, this reaches every device and can't be got back.
- (void)confirmDeleteAccountOfXuid:(uint64_t)xuid
                          gamertag:(NSString*)gamertag
                              from:(UIViewController*)presenter {
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:[NSString stringWithFormat:NSLocalizedString(
                                                              @"Delete the Account of %@?",
                                                              @"Delete account: title; %@ is the "
                                                              @"gamertag"),
                                                          gamertag]
                       message:[NSString
                                   stringWithFormat:
                                       NSLocalizedString(
                                           @"The gamertag is released for anyone to take, friends "
                                           @"lose it, every other device is signed out, and the "
                                           @"profile is deleted from this %@. This can’t be undone.",
                                           @"Delete account: message; %@ is the device's kind"),
                                       UIDevice.currentDevice.model]
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel",
                                                                    @"Delete account: button")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Delete Account",
                                                                    @"Delete account: button")
                                            style:UIAlertActionStyleDestructive
                                          handler:^(UIAlertAction* __unused action) {
                                            [unsafe_self deleteXuid:xuid
                                                           gamertag:gamertag
                                                            account:YES
                                                               from:presenter];
                                          }]];
  [presenter presentViewController:alert animated:YES completion:nil];
}

// Only the files, unless the account goes too. Only the signed-in profile has a connection to the
// service, so for that another one is signed in and the previous one comes back afterwards.
- (void)deleteXuid:(uint64_t)xuid
          gamertag:(NSString*)gamertag
           account:(BOOL)delete_account
              from:(UIViewController*)presenter {
  uint64_t signed_in_xuid = 0;
  for (const auto& profile : profiles_) {
    if (profile.signed_in) {
      signed_in_xuid = profile.xuid;
    }
  }
  const uint64_t restore_xuid = signed_in_xuid == xuid ? 0 : signed_in_xuid;
  if (!delete_account) {
    __unsafe_unretained XeniaProfileViewController* local_self = self;
    [xe_ios_live_account_service() removeThisDeviceOfProfile:xuid
                                                  completion:^{
                                                    [local_self finishDeleteXuid:xuid
                                                                        gamertag:gamertag
                                                                         restore:restore_xuid
                                                                            from:presenter];
                                                  }];
    return;
  }
  if (signed_in_xuid != xuid && !app_context_->SignInProfile(xuid)) {
    [self showDeleteFailure:NSLocalizedString(@"The profile couldn’t be signed in to delete its "
                                              @"online account.",
                                              @"profile view controller: alert message")
                       from:presenter];
    return;
  }
  UIAlertController* busy =
      [UIAlertController alertControllerWithTitle:NSLocalizedString(@"Deleting the Account…",
                                                                    @"Delete profile: progress")
                                          message:nil
                                   preferredStyle:UIAlertControllerStyleAlert];
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  void (^finished)(NSError*) = ^(NSError* error) {
    [busy dismissViewControllerAnimated:YES
                             completion:^{
                               if (!error) {
                                 [unsafe_self finishDeleteXuid:xuid
                                                      gamertag:gamertag
                                                       restore:restore_xuid
                                                          from:presenter];
                                 return;
                               }
                               // The answer may only have been lost, so nothing more is claimed.
                               [unsafe_self
                                   showDeleteFailure:NSLocalizedString(
                                                         @"You have to be online to delete the "
                                                         @"account, and the online service didn’t "
                                                         @"answer. The profile was kept.",
                                                         @"profile view controller: alert message")
                                                from:presenter];
                               [unsafe_self restoreSignIn:restore_xuid];
                             }];
  };
  [presenter presentViewController:busy
                          animated:YES
                        completion:^{
                          [xe_ios_live_account_service() deleteAccountOfProfile:xuid
                                                                     completion:finished];
                        }];
}

- (void)restoreSignIn:(uint64_t)xuid {
  if (xuid && app_context_) {
    app_context_->SignInProfile(xuid);
  }
  [self reloadProfiles];
}

- (void)finishDeleteXuid:(uint64_t)xuid
                gamertag:(NSString*)gamertag
                 restore:(uint64_t)restore_xuid
                    from:(UIViewController*)presenter {
  if (!app_context_ || !app_context_->DeleteProfile(xuid)) {
    [self showDeleteFailure:NSLocalizedString(@"Its files couldn’t be removed.",
                                              @"profile view controller: alert message")
                       from:presenter];
    [self restoreSignIn:restore_xuid];
    return;
  }
  [self.navigationController popToViewController:self animated:YES];
  [self restoreSignIn:restore_xuid];
  if (on_status_) {
    on_status_([NSString stringWithFormat:NSLocalizedString(
                                              @"Deleted %@.",
                                              @"profile view controller: user-visible text"),
                                          gamertag]);
  }
}

- (void)pushPictureChooserForXuid:(uint64_t)xuid from:(UIViewController*)page {
  if (!app_context_) {
    return;
  }
  // The profile's picture as stored, to mark which built-in one it is.
  std::vector<uint8_t> current;
  for (const auto& profile : app_context_->ListProfiles()) {
    if (profile.xuid == xuid) {
      current = profile.picture;
    }
  }
  NSMutableArray<XeniaGamerPicture*>* pictures = [NSMutableArray array];
  NSString* current_key = nil;
  for (const auto& built_in : app_context_->ListGamerPictures()) {
    NSData* png = [NSData dataWithBytes:built_in.png.data() length:built_in.png.size()];
    UIImage* image = [UIImage imageWithData:png];
    if (!image) {
      continue;
    }
    XeniaGamerPicture* picture = [[[XeniaGamerPicture alloc] init] autorelease];
    picture.key = ToNSString(built_in.key);
    picture.image = image;
    [pictures addObject:picture];
    if (built_in.png == current) {
      current_key = picture.key;
    }
  }
  __unsafe_unretained XeniaProfileViewController* unsafe_self = self;
  BOOL (^choose)(NSString*) = ^BOOL(NSString* key) {
    if (!unsafe_self->app_context_ ||
        !unsafe_self->app_context_->SetGamerPicture(xuid, std::string(key.UTF8String))) {
      return NO;
    }
    // Lists the profiles again, which hands every avatar the new picture.
    [unsafe_self reloadProfiles];
    return YES;
  };
  [page.navigationController
      pushViewController:[[[XeniaGamerPictureViewController alloc] initWithPictures:pictures
                                                                         currentKey:current_key
                                                                             choose:choose]
                             autorelease]
                animated:YES];
}

- (BOOL)signInXuid:(uint64_t)xuid gamertag:(NSString*)gamertag {
  if (!app_context_ || !app_context_->SignInProfile(xuid)) {
    if (on_status_) {
      on_status_(NSLocalizedString(@"Failed to sign in profile.",
                                   @"profile view controller: user-visible text"));
    }
    return NO;
  }
  if (on_status_) {
    on_status_([NSString
        stringWithFormat:NSLocalizedString(@"Signed in as %@.",
                                           @"profile view controller: user-visible text"),
                         gamertag]);
  }
  [self reloadProfiles];
  return YES;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (!app_context_) {
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
