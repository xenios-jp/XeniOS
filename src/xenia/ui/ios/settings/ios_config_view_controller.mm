/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_config_view_controller.h"

#include "xenia/ui/ios/shared/ios_edge_adapter.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/config_helpers.h"

#import "xenia/ui/ios/app/ios_landscape_navigation_controller.h"
#import "xenia/ui/ios/app/ios_window_layout.h"
#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_external_folders_view_controller.h"
#import "xenia/ui/ios/settings/ios_choice_list_view_controller.h"
#import "xenia/ui/ios/settings/ios_config_catalog.h"
#import "xenia/ui/ios/settings/ios_config_builder.h"
#import "xenia/ui/ios/settings/ios_config_models.h"
#import "xenia/ui/ios/settings/ios_config_storage.h"
#import "xenia/ui/ios/settings/ios_debug_settings_view_controller.h"
#import "xenia/ui/ios/settings/ios_log_view_controller.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

NSString* const kXeniOSWebsiteURL = @"https://xenios.jp";
NSString* const kXeniOSDiscordURL = @"https://discord.gg/QwcTtNKTGf";
NSString* const kXeniOSGitHubURL = @"https://github.com/xenios-jp/XeniOS";
NSString* const kXeniOSKoFiURL = @"https://ko-fi.com/xenios";

typedef NS_ENUM(NSInteger, XeniaConfigFooterLinkTag) {
  kXeniaConfigFooterLinkWebsite = 1,
  kXeniaConfigFooterLinkGitHub = 2,
  kXeniaConfigFooterLinkDiscord = 3,
  kXeniaConfigFooterLinkKoFi = 4,
};

void OpenExternalURLString(NSString* url_string) {
  if (!url_string || url_string.length == 0) {
    return;
  }
  NSURL* url = [NSURL URLWithString:url_string];
  if (!url) {
    XELOGW("iOS: Failed to create URL from string: {}",
           xe::strip_pii(url_string ? [url_string UTF8String] : "(null)"));
    return;
  }
  [[UIApplication sharedApplication] openURL:url
      options:@{}
      completionHandler:^(BOOL success) {
        if (!success) {
          XELOGW("iOS: Failed to open external URL: {}", xe::strip_pii([url_string UTF8String]));
        }
      }];
}

bool OverrideCvarByName(const std::string& key, bool value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  auto* cv = dynamic_cast<cvar::ConfigVar<bool>*>(it->second);
  if (!cv) return false;
  cv->SetGameConfigValue(value);
  return true;
}
bool OverrideCvarByName(const std::string& key, int32_t value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  auto* cv = dynamic_cast<cvar::ConfigVar<int32_t>*>(it->second);
  if (!cv) return false;
  cv->SetGameConfigValue(value);
  return true;
}
bool OverrideCvarByName(const std::string& key, int64_t value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  auto* cv = dynamic_cast<cvar::ConfigVar<int64_t>*>(it->second);
  if (!cv) return false;
  cv->SetGameConfigValue(value);
  return true;
}
bool OverrideCvarByName(const std::string& key, double value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  auto* cv = dynamic_cast<cvar::ConfigVar<double>*>(it->second);
  if (!cv) return false;
  cv->SetGameConfigValue(value);
  return true;
}
bool OverrideCvarByName(const std::string& key, const std::string& value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  auto* cv = dynamic_cast<cvar::ConfigVar<std::string>*>(it->second);
  if (!cv) return false;
  cv->SetGameConfigValue(value);
  return true;
}
bool OverrideCvarByName(const std::string& key, const std::filesystem::path& value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  auto* cv = dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(it->second);
  if (!cv) return false;
  cv->SetGameConfigValue(value);
  return true;
}

bool IsLetterboxBlockedByWindowStretch(const IOSConfigItem& item) {
  return item.key == "present_letterbox" &&
         XeniaIOSCurrentWindowScalingMode() == XeniaIOSWindowScalingModeStretch;
}

bool OverrideIntegerCvarByName(const std::string& key, int64_t value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  cvar::IConfigVar* var = it->second;
  if (auto* cv = dynamic_cast<cvar::ConfigVar<int32_t>*>(var)) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    cv->SetGameConfigValue(static_cast<int32_t>(value));
    return true;
  }
  if (auto* cv = dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) {
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
    cv->SetGameConfigValue(static_cast<uint32_t>(value));
    return true;
  }
  if (auto* cv = dynamic_cast<cvar::ConfigVar<int64_t>*>(var)) {
    cv->SetGameConfigValue(value);
    return true;
  }
  if (auto* cv = dynamic_cast<cvar::ConfigVar<uint64_t>*>(var)) {
    if (value < 0) {
      return false;
    }
    cv->SetGameConfigValue(static_cast<uint64_t>(value));
    return true;
  }
  return false;
}

bool OverrideFloatingCvarByName(const std::string& key, double value) {
  if (!cvar::ConfigVars) return false;
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) return false;
  cvar::IConfigVar* var = it->second;
  if (auto* cv = dynamic_cast<cvar::ConfigVar<double>*>(var)) {
    cv->SetGameConfigValue(value);
    return true;
  }
  if (auto* cv = dynamic_cast<cvar::ConfigVar<float>*>(var)) {
    cv->SetGameConfigValue(static_cast<float>(value));
    return true;
  }
  return false;
}

bool OverrideStringLikeCvarByName(const std::string& key, const std::string& value) {
  if (OverrideCvarByName(key, value)) {
    return true;
  }
  return OverrideCvarByName(key, std::filesystem::path(value));
}

// Assigns the item's value to its cvar as a game override. Runs inside ApplyLiveSettings.
void ApplyLiveOverrideValue(const IOSConfigItem& item) {
  switch (item.control_type) {
    case IOSConfigControlType::kToggle:
      OverrideCvarByName(item.key, item.bool_value);
      break;
    case IOSConfigControlType::kInteger:
      OverrideIntegerCvarByName(item.key, item.integer_value);
      break;
    case IOSConfigControlType::kDouble:
      OverrideFloatingCvarByName(item.key, item.double_value);
      break;
    case IOSConfigControlType::kString:
    case IOSConfigControlType::kPath:
      OverrideStringLikeCvarByName(item.key, item.string_value);
      break;
    case IOSConfigControlType::kEnum:
    case IOSConfigControlType::kChoiceString:
      OverrideStringLikeCvarByName(item.key, item.string_value);
      break;
    case IOSConfigControlType::kChoiceInt32:
      OverrideIntegerCvarByName(item.key, item.choice_value);
      break;
    case IOSConfigControlType::kChoiceUInt32:
      OverrideIntegerCvarByName(item.key, item.choice_value);
      break;
    case IOSConfigControlType::kChoiceUInt64:
      OverrideIntegerCvarByName(item.key, item.choice_value);
      break;
    default:
      break;
  }
}

}  // namespace

UIView* XeniaConfigMakeValueAccessoryView(NSString* value, CGFloat max_width, BOOL with_chevron) {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.text = value;
  label.textColor = [XeniaTheme textSecondary];
  label.numberOfLines = 1;
  label.lineBreakMode = NSLineBreakByTruncatingTail;
  label.textAlignment = NSTextAlignmentRight;
  xe_apply_label_font(label, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  [label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                         forAxis:UILayoutConstraintAxisHorizontal];
  [label.widthAnchor constraintLessThanOrEqualToConstant:MAX(max_width, 44.0)].active = YES;

  NSMutableArray<UIView*>* views = [NSMutableArray arrayWithObject:label];
  if (with_chevron) {
    UIImageSymbolConfiguration* symbol_config = [UIImageSymbolConfiguration
        configurationWithPointSize:13.0
                            weight:UIImageSymbolWeightSemibold];
    UIImageView* chevron = [[[UIImageView alloc]
        initWithImage:[UIImage systemImageNamed:@"chevron.right"
                              withConfiguration:symbol_config]] autorelease];
    chevron.tintColor = [UIColor tertiaryLabelColor];
    [chevron setContentHuggingPriority:UILayoutPriorityRequired
                               forAxis:UILayoutConstraintAxisHorizontal];
    [views addObject:chevron];
  }
  UIStackView* stack = [[[UIStackView alloc] initWithArrangedSubviews:views] autorelease];
  stack.axis = UILayoutConstraintAxisHorizontal;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.spacing = 8.0;
  // The row is the VoiceOver element and speaks the value; this view is only drawn.
  stack.isAccessibilityElement = NO;
  const CGSize size = [stack systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
  stack.frame = CGRectMake(0.0, 0.0, ceil(size.width), ceil(size.height));
  return stack;
}

// Text fields reused across rows are told apart by tag so a row only reuses a field wired for its
// own kind of value.
static const NSInteger kXeniaConfigIntegerFieldTag = 9101;
static const NSInteger kXeniaConfigDoubleFieldTag = 9102;
static const NSInteger kXeniaConfigStringFieldTag = 9103;

@interface XeniaConfigViewController () <UISearchResultsUpdating>
- (void)confirmResetGameSettings;
- (void)confirmResetAllSettings;
@end

@implementation XeniaConfigViewController {
  IOSConfigCatalogKind catalog_kind_;
  std::vector<IOSConfigSection> sections_;
  std::set<std::string> dirty_keys_;
  BOOL hasPendingChanges_;
  BOOL shows_root_dismiss_button_;
  BOOL live_override_;
  xe::ui::IOSWindowedAppContext* app_context_;
  uint32_t game_title_id_;
  NSString* game_title_;
  UIBarButtonItem* saveButton_;
  void (^dismissal_handler_)(void);
  // Pages with search keep the unfiltered list here; sections_ is what the table shows.
  std::vector<IOSConfigSection> full_sections_;
  UISearchController* search_controller_;
  NSString* focus_key_;
  NSIndexPath* focus_path_;
  BOOL focus_flash_pending_;
  BOOL loading_game_config_;
  std::shared_ptr<void> game_config_snapshot_;
}

@synthesize dismissalHandler = dismissal_handler_;
@synthesize showsRootDismissButton = shows_root_dismiss_button_;
@synthesize liveOverride = live_override_;
@synthesize appContext = app_context_;

- (NSString*)focusKey {
  return focus_key_;
}

- (void)setFocusKey:(NSString*)focusKey {
  if (focus_key_ == focusKey) {
    return;
  }
  [focus_key_ release];
  focus_key_ = [focusKey copy];
}

- (instancetype)initWithStyle:(UITableViewStyle)style {
  return [self initWithCatalogKind:IOSConfigCatalogKind::kMain style:style];
}

- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                              style:(UITableViewStyle)style {
  return [self initWithCatalogKind:catalogKind style:style gameTitleID:0 gameTitle:nil];
}

- (instancetype)initWithCatalogKind:(IOSConfigCatalogKind)catalogKind
                               style:(UITableViewStyle)style
                         gameTitleID:(uint32_t)gameTitleID
                           gameTitle:(NSString*)gameTitle {
  self = [super initWithStyle:style];
  if (self) {
    catalog_kind_ = catalogKind;
    game_title_id_ = gameTitleID;
    game_title_ = [gameTitle copy];
    shows_root_dismiss_button_ = YES;
  }
  return self;
}

- (IOSConfigCatalogKind)catalogKind {
  return catalog_kind_;
}

- (uint32_t)gameTitleID {
  return game_title_id_;
}

- (NSString*)gameTitle {
  return game_title_;
}

- (std::vector<IOSConfigSection>)buildSections {
  std::vector<IOSConfigSection> sections = BuildIOSConfigSectionsForKind(catalog_kind_);
  if (game_title_id_) {
    // The first build uses the file read off the main thread; later ones (after a reset) read
    // it again.
    std::shared_ptr<void> snapshot = std::move(game_config_snapshot_);
    game_config_snapshot_.reset();
    OverlayIOSConfigSectionsFromGameConfig(&sections, game_title_id_, snapshot);
  }
  return sections;
}

// Game Settings, All Settings and the developer pages are long lists, so they get search.
- (BOOL)supportsSearch {
  switch (catalog_kind_) {
    case IOSConfigCatalogKind::kPerGame:
    case IOSConfigCatalogKind::kAllSettings:
    case IOSConfigCatalogKind::kAllCvars:
    case IOSConfigCatalogKind::kAdvanced:
    case IOSConfigCatalogKind::kDebugSettings:
      return YES;
    default:
      return NO;
  }
}

- (std::vector<IOSConfigSection>)sectionsForSaving {
  return [self supportsSearch] ? full_sections_ : sections_;
}

- (void)replaceSections:(std::vector<IOSConfigSection>)newSections {
  sections_ = std::move(newSections);
  [self.tableView reloadData];
}

// Rebuilds the rows from the catalog (after a reset) and applies the current search again.
- (void)reloadSectionsFromCatalog {
  full_sections_ = [self buildSections];
  if ([self supportsSearch] && search_controller_.searchBar.text.length) {
    [self updateSearchResultsForSearchController:search_controller_];
    return;
  }
  [self replaceSections:full_sections_];
}

- (void)dealloc {
  [search_controller_ release];
  [focus_key_ release];
  [focus_path_ release];
  [game_title_ release];
  [saveButton_ release];
  [dismissal_handler_ release];
  [super dealloc];
}

- (CGFloat)estimatedTableAccessoryWidth {
  CGFloat width = CGRectGetWidth(self.tableView.bounds);
  if (width <= 0.0) {
    width = CGRectGetWidth(self.view.bounds);
  }
  if (width <= 0.0 && self.navigationController) {
    width = CGRectGetWidth(self.navigationController.view.bounds);
  }
  if (width <= 0.0) {
    width = CGRectGetWidth(UIScreen.mainScreen.bounds);
  }
  return MAX(width, 1.0);
}

- (CGRect)initialTableAccessoryFrameWithHeight:(CGFloat)height {
  return CGRectMake(0.0, 0.0, [self estimatedTableAccessoryWidth], height);
}

- (UIView*)restartNoticeHeaderView {
  UIView* container =
      [[[UIView alloc] initWithFrame:[self initialTableAccessoryFrameWithHeight:136.0]]
          autorelease];
  container.backgroundColor = [UIColor clearColor];

  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  card.backgroundColor = [XeniaTheme bgSurface];
  card.layer.cornerRadius = 12.0;
  card.layer.borderWidth = 0.5;
  card.layer.borderColor = [XeniaTheme border].CGColor;
  [container addSubview:card];

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.translatesAutoresizingMaskIntoConstraints = NO;
  title.backgroundColor = [UIColor clearColor];
  title.text =
      game_title_id_
          ? NSLocalizedString(@"Game-Specific Settings", @"config view controller: title")
          : NSLocalizedString(@"Change Settings Before Launch", @"config view controller: title");
  title.textColor = [XeniaTheme textPrimary];
  title.numberOfLines = 0;
  xe_apply_label_font(title, UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold);
  [title setContentCompressionResistancePriority:UILayoutPriorityRequired
                                         forAxis:UILayoutConstraintAxisVertical];
  [card addSubview:title];

  UILabel* body = [[[UILabel alloc] init] autorelease];
  body.translatesAutoresizingMaskIntoConstraints = NO;
  body.backgroundColor = [UIColor clearColor];
  if (game_title_id_) {
    NSString* title_id = XEFormatTitleIDHexUpper(game_title_id_);
    NSString* game_name = game_title_.length
                              ? game_title_
                              : NSLocalizedString(@"this game", @"config view controller: title");
    body.text = [NSString
        stringWithFormat:NSLocalizedString(
                             @"Overrides here apply only to %1$@ (%2$@). They are read when the title "
                             @"launches, so relaunch the game after saving.",
                             @"config view controller: label text"),
                         game_name, title_id];
  } else {
    body.text =
        NSLocalizedString(@"To avoid partial updates, change settings before launching a game. If "
                          @"you save changes while a game is already running, fully relaunch "
                          @"XeniOS before testing them.",
                          @"config view controller: label text");
  }
  body.textColor = [XeniaTheme textSecondary];
  body.numberOfLines = 0;
  xe_apply_label_font(body, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  [body setContentCompressionResistancePriority:UILayoutPriorityRequired
                                        forAxis:UILayoutConstraintAxisVertical];
  [card addSubview:body];

  [NSLayoutConstraint activateConstraints:@[
    [card.topAnchor constraintEqualToAnchor:container.topAnchor constant:8.0],
    [card.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:16.0],
    [card.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-16.0],
    [card.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-4.0],

    [title.topAnchor constraintEqualToAnchor:card.topAnchor constant:14.0],
    [title.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14.0],
    [title.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-14.0],

    [body.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:6.0],
    [body.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14.0],
    [body.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-14.0],
    [body.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-14.0],
  ]];

  return container;
}

- (UIView*)liveOverrideHeaderView {
  UIView* container =
      [[[UIView alloc] initWithFrame:[self initialTableAccessoryFrameWithHeight:48.0]]
          autorelease];
  container.backgroundColor = [UIColor clearColor];

  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.translatesAutoresizingMaskIntoConstraints = NO;
  label.backgroundColor = [UIColor clearColor];
  label.text = game_title_id_
                   ? NSLocalizedString(@"Changes apply immediately and are saved for this game",
                                       @"config view controller: title")
                   : NSLocalizedString(@"Changes apply immediately for this session",
                                       @"config view controller: title");
  label.textAlignment = NSTextAlignmentCenter;
  label.textColor = [XeniaTheme textMuted];
  // Wraps at large text sizes and in longer languages; the layout helper measures the height.
  label.numberOfLines = 0;
  xe_apply_label_font(label, UIFontTextStyleCaption1, 12.0, UIFontWeightMedium);
  [label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                         forAxis:UILayoutConstraintAxisVertical];
  [container addSubview:label];

  [NSLayoutConstraint activateConstraints:@[
    [label.topAnchor constraintEqualToAnchor:container.topAnchor constant:14.0],
    [label.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-14.0],
    [label.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:16.0],
    [label.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-16.0],
  ]];

  return container;
}

- (void)updateTableAccessoryLayoutForView:(UIView*)view isHeader:(BOOL)isHeader {
  if (!view) {
    return;
  }
  [UIView performWithoutAnimation:^{
  CGRect bounds = self.tableView.bounds;
  CGRect accessory_frame = bounds;
  CGFloat left_inset = 0.0;
  CGFloat right_inset = 0.0;
  CGRect safe_frame = self.tableView.safeAreaLayoutGuide.layoutFrame;
  if (!CGRectIsEmpty(safe_frame) && CGRectGetWidth(safe_frame) > 0.0) {
    left_inset = MAX(left_inset, CGRectGetMinX(safe_frame));
    right_inset = MAX(right_inset, CGRectGetMaxX(bounds) - CGRectGetMaxX(safe_frame));
  }
  UIEdgeInsets adjusted_insets = self.tableView.adjustedContentInset;
  left_inset = MAX(left_inset, adjusted_insets.left);
  right_inset = MAX(right_inset, adjusted_insets.right);
  if (left_inset + right_inset < CGRectGetWidth(bounds)) {
    accessory_frame.origin.x = left_inset;
    accessory_frame.size.width = CGRectGetWidth(bounds) - left_inset - right_inset;
  }

  CGFloat width = CGRectGetWidth(accessory_frame);
  if (width <= 0.0) {
    width = CGRectGetWidth(self.view.bounds);
    accessory_frame.origin.x = 0.0;
    accessory_frame.size.width = width;
  }
  if (width <= 0.0 && self.navigationController) {
    width = CGRectGetWidth(self.navigationController.view.bounds);
    accessory_frame.origin.x = 0.0;
    accessory_frame.size.width = width;
  }
  if (width <= 0.0) {
    width = CGRectGetWidth(UIScreen.mainScreen.bounds);
    accessory_frame.origin.x = 0.0;
    accessory_frame.size.width = width;
  }
  if (width <= 0.0) {
    return;
  }
  CGRect frame = view.frame;
  frame.origin.x = CGRectGetMinX(accessory_frame);
  frame.size.width = width;
  view.frame = frame;
  [view setNeedsLayout];
  [view layoutIfNeeded];
  CGSize fitting_size =
      [view systemLayoutSizeFittingSize:CGSizeMake(width, UILayoutFittingCompressedSize.height)
          withHorizontalFittingPriority:UILayoutPriorityRequired
                verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
  CGFloat target_height = ceil(fitting_size.height);
  if (target_height <= 0.0) {
    return;
  }
  if (fabs(CGRectGetHeight(view.frame) - target_height) <= 0.5 &&
      fabs(CGRectGetWidth(view.frame) - width) <= 0.5) {
    return;
  }
  frame.size.height = target_height;
  view.frame = frame;
  if (isHeader) {
    self.tableView.tableHeaderView = view;
  } else {
    self.tableView.tableFooterView = view;
  }
  }];
}

- (void)updateTableHeaderAndFooterLayout {
  [self updateTableAccessoryLayoutForView:self.tableView.tableHeaderView isHeader:YES];
  [self updateTableAccessoryLayoutForView:self.tableView.tableFooterView isHeader:NO];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = ToNSString(IOSConfigCatalogTitle(catalog_kind_));
  if (game_title_id_ && game_title_.length) {
    self.navigationItem.prompt = game_title_;
  }
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.separatorInset = UIEdgeInsetsMake(0, 16, 0, 16);
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 132.0;
  self.tableView.sectionHeaderTopPadding = 0;
  hasPendingChanges_ = NO;
  if (game_title_id_) {
    // Reading the title's saved overrides is a file read and a TOML parse; keep it off the main
    // thread and show the rows when it is done.
    loading_game_config_ = YES;
    UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge] autorelease];
    spinner.color = [XeniaTheme textSecondary];
    [spinner startAnimating];
    self.tableView.backgroundView = spinner;
    const uint32_t title_id = game_title_id_;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::shared_ptr<void> snapshot = LoadIOSGameConfigSnapshot(title_id);
      dispatch_async(dispatch_get_main_queue(), ^{
        game_config_snapshot_ = snapshot;
        loading_game_config_ = NO;
        self.tableView.backgroundView = nil;
        [self reloadSectionsFromCatalog];
        [self scrollToFocusRow];
        [self flashFocusRow];
      });
    });
  } else {
    full_sections_ = [self buildSections];
    sections_ = full_sections_;
  }
  focus_flash_pending_ = focus_key_.length > 0;

  if ([self supportsSearch]) {
    search_controller_ = [[UISearchController alloc] initWithSearchResultsController:nil];
    search_controller_.searchResultsUpdater = self;
    search_controller_.obscuresBackgroundDuringPresentation = NO;
    search_controller_.hidesNavigationBarDuringPresentation = NO;
    search_controller_.searchBar.placeholder =
        NSLocalizedString(@"Search Settings", @"config view controller: placeholder");
    self.navigationItem.searchController = search_controller_;
    self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;
  }

  if (live_override_) {
    self.tableView.tableHeaderView = [self liveOverrideHeaderView];
    self.tableView.tableFooterView = nil;
    [self updateTableHeaderAndFooterLayout];
  } else {
    self.tableView.tableHeaderView = [self restartNoticeHeaderView];
    self.tableView.tableFooterView = [self versionFooterView];
    [self updateTableHeaderAndFooterLayout];
  }

  if (!live_override_) {
    saveButton_ = [[UIBarButtonItem alloc]
        initWithTitle:NSLocalizedString(@"Save", @"config view controller: title")
                style:UIBarButtonItemStyleDone
               target:self
               action:@selector(saveTapped:)];
    saveButton_.enabled = NO;
  }
  [self updateNavigationItems];
}

- (void)footerLinkTapped:(UIButton*)sender {
  switch ((XeniaConfigFooterLinkTag)sender.tag) {
    case kXeniaConfigFooterLinkWebsite:
      OpenExternalURLString(kXeniOSWebsiteURL);
      break;
    case kXeniaConfigFooterLinkGitHub:
      OpenExternalURLString(kXeniOSGitHubURL);
      break;
    case kXeniaConfigFooterLinkDiscord:
      OpenExternalURLString(kXeniOSDiscordURL);
      break;
    case kXeniaConfigFooterLinkKoFi:
      OpenExternalURLString(kXeniOSKoFiURL);
      break;
    default:
      break;
  }
}

- (UIView*)versionFooterView {
  NSString* footer_text = xe_user_facing_build_label(xe_current_compat_report_build_info());
  const BOOL has_footer_text = footer_text.length > 0;
  NSString* memorial_text =
      NSLocalizedString(@"XeniOS is one of several apps with dedication keeping the memory of "
                        @"\"Lily\" alive 11/03/2023",
                        @"config view controller: label text");
  UIView* footer =
      [[[UIView alloc]
          initWithFrame:[self initialTableAccessoryFrameWithHeight:(has_footer_text ? 176.0
                                                                                    : 78.0)]]
          autorelease];
  footer.backgroundColor = [UIColor clearColor];

  UILabel* links_label = [[[UILabel alloc] init] autorelease];
  links_label.translatesAutoresizingMaskIntoConstraints = NO;
  links_label.backgroundColor = [UIColor clearColor];
  links_label.text = NSLocalizedString(@"Links", @"config view controller: label text");
  links_label.textAlignment = NSTextAlignmentCenter;
  links_label.textColor = [XeniaTheme textMuted];
  links_label.numberOfLines = 1;
  xe_apply_label_font(links_label, UIFontTextStyleCaption1, 12.0, UIFontWeightSemibold);
  [links_label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                               forAxis:UILayoutConstraintAxisVertical];
  [footer addSubview:links_label];

  UIStackView* links_row = [[[UIStackView alloc] initWithArrangedSubviews:@[
    xe_make_settings_footer_button(
        @"", @"globe", NSLocalizedString(@"Website", @"config view controller: section footer"),
        kXeniaConfigFooterLinkWebsite, YES, self, @selector(footerLinkTapped:)),
    xe_make_settings_footer_button(
        @"SettingsLinkGitHub", @"chevron.left.forwardslash.chevron.right", @"GitHub",
        kXeniaConfigFooterLinkGitHub, YES, self, @selector(footerLinkTapped:)),
    xe_make_settings_footer_button(@"SettingsLinkDiscord", @"bubble.left.and.bubble.right",
                                   @"Discord", kXeniaConfigFooterLinkDiscord, YES, self,
                                   @selector(footerLinkTapped:)),
    xe_make_settings_footer_button(@"SettingsLinkKoFi", @"cup.and.saucer", @"Ko-fi",
                                   kXeniaConfigFooterLinkKoFi, YES, self,
                                   @selector(footerLinkTapped:)),
  ]] autorelease];
  links_row.translatesAutoresizingMaskIntoConstraints = NO;
  links_row.axis = UILayoutConstraintAxisHorizontal;
  links_row.alignment = UIStackViewAlignmentCenter;
  links_row.distribution = UIStackViewDistributionEqualCentering;
  links_row.spacing = 18.0;
  [footer addSubview:links_row];

  UIView* build_separator = nil;
  UILabel* build_label = nil;
  UILabel* memorial_label = nil;
  if (has_footer_text) {
    build_separator = [[[UIView alloc] init] autorelease];
    build_separator.translatesAutoresizingMaskIntoConstraints = NO;
    build_separator.backgroundColor = [XeniaTheme border];
    [footer addSubview:build_separator];

    build_label = [[[UILabel alloc] init] autorelease];
    build_label.translatesAutoresizingMaskIntoConstraints = NO;
    build_label.backgroundColor = [UIColor clearColor];
    build_label.text = footer_text;
    build_label.textAlignment = NSTextAlignmentCenter;
    build_label.textColor = [XeniaTheme textMuted];
    build_label.numberOfLines = 1;
    xe_apply_label_font(build_label, UIFontTextStyleFootnote, 14.0, UIFontWeightMedium);
    [build_label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                 forAxis:UILayoutConstraintAxisVertical];
    [footer addSubview:build_label];

    memorial_label = [[[UILabel alloc] init] autorelease];
    memorial_label.translatesAutoresizingMaskIntoConstraints = NO;
    memorial_label.backgroundColor = [UIColor clearColor];
    memorial_label.text = memorial_text;
    memorial_label.textAlignment = NSTextAlignmentCenter;
    memorial_label.textColor = [XeniaTheme textSecondary];
    memorial_label.numberOfLines = 0;
    xe_apply_label_font(memorial_label, UIFontTextStyleCaption1, 12.0, UIFontWeightRegular);
    [memorial_label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                    forAxis:UILayoutConstraintAxisVertical];
    [footer addSubview:memorial_label];
  }

  NSMutableArray<NSLayoutConstraint*>* constraints = [NSMutableArray arrayWithArray:@[
    [links_label.topAnchor constraintEqualToAnchor:footer.topAnchor constant:6],
    [links_label.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:24],
    [links_label.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-24],
    [links_row.topAnchor constraintEqualToAnchor:links_label.bottomAnchor constant:8],
    [links_row.leadingAnchor constraintGreaterThanOrEqualToAnchor:footer.leadingAnchor constant:24],
    [links_row.trailingAnchor constraintLessThanOrEqualToAnchor:footer.trailingAnchor constant:-24],
    [links_row.centerXAnchor constraintEqualToAnchor:footer.centerXAnchor],
  ]];

  if (has_footer_text) {
    [constraints addObjectsFromArray:@[
      [build_separator.topAnchor constraintEqualToAnchor:links_row.bottomAnchor constant:16],
      [build_separator.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:24],
      [build_separator.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-24],
      [build_separator.heightAnchor constraintEqualToConstant:0.5],
      [build_label.topAnchor constraintEqualToAnchor:build_separator.bottomAnchor constant:12],
      [build_label.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:24],
      [build_label.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-24],
      [memorial_label.topAnchor constraintEqualToAnchor:build_label.bottomAnchor constant:8],
      [memorial_label.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor constant:24],
      [memorial_label.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor constant:-24],
      [memorial_label.bottomAnchor constraintEqualToAnchor:footer.bottomAnchor constant:-8],
    ]];
  } else {
    [constraints addObject:[links_row.bottomAnchor constraintEqualToAnchor:footer.bottomAnchor
                                                                  constant:-10]];
  }

  [NSLayoutConstraint activateConstraints:constraints];
  return footer;
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  [self updateTableHeaderAndFooterLayout];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self updateNavigationItems];
  [self updateTableHeaderAndFooterLayout];
  [self scrollToFocusRow];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [self flashFocusRow];
}

// Scrolls the row named by focusKey into view, once. Rows are found in the unfiltered list.
- (void)scrollToFocusRow {
  if (!focus_key_.length || focus_path_ || loading_game_config_) {
    return;
  }
  const std::string key = [focus_key_ UTF8String];
  const std::vector<IOSConfigSection>& sections =
      [self supportsSearch] ? full_sections_ : sections_;
  for (size_t section_index = 0; section_index < sections.size() && !focus_path_; ++section_index) {
    const std::vector<IOSConfigItem>& items = sections[section_index].items;
    for (size_t row = 0; row < items.size(); ++row) {
      if (items[row].key.empty() ? items[row].title == key : items[row].key == key) {
        focus_path_ = [[NSIndexPath indexPathForRow:static_cast<NSInteger>(row)
                                          inSection:static_cast<NSInteger>(section_index)] retain];
        break;
      }
    }
  }
  if (!focus_path_ || static_cast<size_t>(focus_path_.section) >= sections_.size()) {
    return;
  }
  // Row heights are estimates until measured, so scroll, lay out and scroll again.
  [self.tableView layoutIfNeeded];
  [self.tableView scrollToRowAtIndexPath:focus_path_
                        atScrollPosition:UITableViewScrollPositionMiddle
                                animated:NO];
  [self.tableView layoutIfNeeded];
  [self.tableView scrollToRowAtIndexPath:focus_path_
                        atScrollPosition:UITableViewScrollPositionMiddle
                                animated:NO];
}

// Briefly tints the focused row so the eye finds it, and moves VoiceOver there.
- (void)flashFocusRow {
  if (!focus_flash_pending_ || !focus_path_ || loading_game_config_ || !self.tableView.window) {
    return;
  }
  UITableViewCell* cell = [self.tableView cellForRowAtIndexPath:focus_path_];
  if (!cell) {
    return;
  }
  focus_flash_pending_ = NO;
  UIView* flash = [[[UIView alloc] initWithFrame:cell.bounds] autorelease];
  flash.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  flash.userInteractionEnabled = NO;
  flash.isAccessibilityElement = NO;
  flash.layer.cornerRadius = 10.0;
  flash.backgroundColor = [self.view.tintColor colorWithAlphaComponent:0.28];
  [cell addSubview:flash];
  [UIView animateWithDuration:1.0
      delay:0.5
      options:UIViewAnimationOptionAllowUserInteraction
      animations:^{
        flash.alpha = 0.0;
      }
      completion:^(__unused BOOL finished) {
        [flash removeFromSuperview];
      }];
  UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, cell);
}

- (void)viewSafeAreaInsetsDidChange {
  [super viewSafeAreaInsetsDidChange];
  [self updateTableHeaderAndFooterLayout];
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if (self.isViewLoaded && previousTraitCollection &&
      ![self.traitCollection.preferredContentSizeCategory
          isEqualToString:previousTraitCollection.preferredContentSizeCategory]) {
    // Text field widths are measured from the font, so lay the rows out again.
    NSArray<NSIndexPath*>* visible = self.tableView.indexPathsForVisibleRows;
    if (visible.count) {
      [self.tableView reloadRowsAtIndexPaths:visible withRowAnimation:UITableViewRowAnimationNone];
    }
  }
  if ([self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    // Header card's CGColor border was captured against the prior style; the
    // simplest live-refresh is to rebuild the table's accessory views so the
    // dynamic XeniaTheme accessors resolve against the new trait collection.
    if (live_override_) {
      self.tableView.tableHeaderView = [self liveOverrideHeaderView];
    } else {
      self.tableView.tableHeaderView = [self restartNoticeHeaderView];
      self.tableView.tableFooterView = [self versionFooterView];
    }
    [self updateTableHeaderAndFooterLayout];
  }
}

- (void)markPendingChangesForItem:(const IOSConfigItem*)item {
  if (live_override_) {
    return;
  }
  if (item && !item->key.empty()) {
    dirty_keys_.insert(item->key);
  }
  hasPendingChanges_ = YES;
  saveButton_.enabled = YES;
}

// Pages with search edit a filtered copy of the rows; keep the unfiltered list (what Save writes)
// in step.
- (void)configItemDidChange:(const IOSConfigItem*)item {
  if (!item || item->key.empty() || ![self supportsSearch]) {
    return;
  }
  for (IOSConfigSection& section : full_sections_) {
    for (IOSConfigItem& full_item : section.items) {
      if (full_item.key == item->key) {
        full_item = *item;
        return;
      }
    }
  }
}

// Updates the "Default: ..." line of a visible row after its value changed, without reloading
// the row (a reload would cut the switch animation or the text field short).
- (void)refreshSecondaryTextForItem:(const IOSConfigItem*)item {
  if (!item || game_title_id_ || IsLetterboxBlockedByWindowStretch(*item)) {
    return;
  }
  for (NSIndexPath* path in self.tableView.indexPathsForVisibleRows) {
    if ([self itemAtIndexPath:path] != item) {
      continue;
    }
    UITableViewCell* cell = [self.tableView cellForRowAtIndexPath:path];
    UIListContentConfiguration* content =
        [cell.contentConfiguration isKindOfClass:[UIListContentConfiguration class]]
            ? [[(UIListContentConfiguration*)cell.contentConfiguration copy] autorelease]
            : nil;
    if (!content) {
      return;
    }
    NSString* text = [self secondaryTextForItem:*item];
    if ([content.secondaryText isEqualToString:text]) {
      return;
    }
    content.secondaryText = text;
    cell.contentConfiguration = content;
    if ([cell.accessoryView isKindOfClass:[UISwitch class]]) {
      cell.accessoryView.accessibilityHint = text;
    }
    [self.tableView beginUpdates];
    [self.tableView endUpdates];
    return;
  }
}

#pragma mark - UISearchResultsUpdating

- (void)updateSearchResultsForSearchController:(UISearchController*)searchController {
  NSString* query = [searchController.searchBar.text
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (query.length == 0) {
    [self replaceSections:full_sections_];
    return;
  }

  // Matches the key, title, description and the current value ("Metal" finds the backend), and
  // keeps each result under its section.
  auto matches = [query](const std::string& text) {
    if (text.empty()) {
      return false;
    }
    return [ToNSString(text) rangeOfString:query
                                   options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch]
               .location != NSNotFound;
  };
  std::vector<IOSConfigSection> results;
  for (const IOSConfigSection& section : full_sections_) {
    IOSConfigSection filtered;
    filtered.title = section.title;
    for (const IOSConfigItem& item : section.items) {
      if (matches(item.key) || matches(item.title) || matches(item.subtitle) ||
          (item.control_type != IOSConfigControlType::kToggle &&
           matches(IOSConfigItemValueText(item)))) {
        filtered.items.push_back(item);
      }
    }
    if (!filtered.items.empty()) {
      results.push_back(std::move(filtered));
    }
  }
  [self replaceSections:std::move(results)];
}

- (BOOL)isRootSettingsController {
  UINavigationController* navigation_controller = self.navigationController;
  if (!navigation_controller) {
    return YES;
  }
  return navigation_controller.viewControllers.count == 0 ||
         navigation_controller.viewControllers.firstObject == self;
}

- (void)updateNavigationItems {
  if ([self isRootSettingsController]) {
    if (shows_root_dismiss_button_) {
      self.navigationItem.leftBarButtonItem =
          [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                         target:self
                                                         action:@selector(cancelTapped:)]
              autorelease];
    } else {
      self.navigationItem.leftBarButtonItem = nil;
    }
  } else {
    self.navigationItem.leftBarButtonItem = nil;
  }
  self.navigationItem.rightBarButtonItem = live_override_ ? nil : saveButton_;
}

- (IOSConfigItem*)itemAtIndexPath:(NSIndexPath*)indexPath {
  if (indexPath.section < 0 || indexPath.section >= static_cast<NSInteger>(sections_.size())) {
    return nullptr;
  }
  IOSConfigSection& section = sections_[indexPath.section];
  if (indexPath.row < 0 || indexPath.row >= static_cast<NSInteger>(section.items.size())) {
    return nullptr;
  }
  return &section.items[indexPath.row];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return static_cast<NSInteger>(sections_.size());
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  if (section < 0 || section >= static_cast<NSInteger>(sections_.size())) {
    return 0;
  }
  return static_cast<NSInteger>(sections_[section].items.size());
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
  if (section < 0 || section >= static_cast<NSInteger>(sections_.size())) {
    return nil;
  }
  return ToNSString(sections_[section].title);
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
  if (section < 0 || section >= static_cast<NSInteger>(sections_.size())) {
    return nil;
  }
  return sections_[section].footer.empty() ? nil : ToNSString(sections_[section].footer);
}

// Description plus, for a setting that was changed, its default on a second line.
- (NSString*)secondaryTextForItem:(const IOSConfigItem&)item {
  NSString* base = ToNSString(item.subtitle);
  if (game_title_id_ || item.control_type == IOSConfigControlType::kAction ||
      IsLetterboxBlockedByWindowStretch(item) || IOSConfigItemIsDefault(item)) {
    return base;
  }
  std::string default_text;
  if (!IOSConfigItemDefaultText(item, &default_text)) {
    return base;
  }
  NSString* note = [NSString
      stringWithFormat:NSLocalizedString(@"Default: %@",
                                         @"config view controller: default of a changed setting"),
                       ToNSString(default_text)];
  return base.length ? [NSString stringWithFormat:@"%@\n%@", base, note] : note;
}

// Width for a number or text field: grows with the text size and the text, up to half the row.
- (CGFloat)fieldWidthForText:(NSString*)text font:(UIFont*)font baseWidth:(CGFloat)base_width {
  UIFontMetrics* metrics = [UIFontMetrics metricsForTextStyle:UIFontTextStyleBody];
  const CGFloat minimum = ceil([metrics scaledValueForValue:base_width]);
  const CGFloat maximum = MAX(minimum, floor([self estimatedTableAccessoryWidth] * 0.5));
  const CGFloat text_width =
      ceil([text ?: @"" sizeWithAttributes:@{NSFontAttributeName : font}].width) + 8.0;
  return MIN(MAX(minimum, text_width), maximum);
}

// The switch a toggle row shows. A cell that already holds one keeps it (the action is wired
// once); only its state is updated.
- (UISwitch*)switchForCell:(UITableViewCell*)cell {
  if ([cell.accessoryView isKindOfClass:[UISwitch class]]) {
    return (UISwitch*)cell.accessoryView;
  }
  UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
  [toggle addTarget:self
                action:@selector(toggleChanged:)
      forControlEvents:UIControlEventValueChanged];
  return toggle;
}

// The text field a number or string row shows, reused when the cell already holds one wired for
// the same kind of value and nobody is typing in it.
- (UITextField*)fieldForCell:(UITableViewCell*)cell
                         tag:(NSInteger)tag
                   endAction:(SEL)end_action {
  if ([cell.accessoryView isKindOfClass:[UITextField class]] && cell.accessoryView.tag == tag &&
      !((UITextField*)cell.accessoryView).isFirstResponder) {
    return (UITextField*)cell.accessoryView;
  }
  UITextField* field = [[[UITextField alloc] init] autorelease];
  field.tag = tag;
  [field addTarget:self action:end_action forControlEvents:UIControlEventEditingDidEnd];
  [field addTarget:self
                action:@selector(textFieldEditingChanged:)
      forControlEvents:UIControlEventEditingChanged];
  return field;
}

- (void)configureField:(UITextField*)field
                  text:(NSString*)text
                  font:(UIFont*)font
             baseWidth:(CGFloat)base_width {
  field.text = text;
  field.textAlignment = NSTextAlignmentRight;
  field.font = font;
  field.adjustsFontForContentSizeCategory = YES;
  field.textColor = [XeniaTheme textSecondary];
  field.frame = CGRectMake(0, 0, [self fieldWidthForText:text font:font baseWidth:base_width],
                           MAX(32.0, ceil(font.lineHeight) + 8.0));
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaConfigCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kCellIdentifier] autorelease];
  }

  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item) {
    cell.contentConfiguration = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.accessoryView = nil;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
  }

  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = ToNSString(item->title);
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.numberOfLines = 0;
  content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.secondaryTextProperties.numberOfLines = 0;
  content.secondaryTextProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  content.prefersSideBySideTextAndSecondaryText = NO;
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(12.0, 0.0, 12.0, 0.0);

  UIFont* field_font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleBody]
      scaledFontForFont:[UIFont monospacedDigitSystemFontOfSize:16.0 weight:UIFontWeightRegular]];
  UIFont* text_font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];

  if (item->control_type == IOSConfigControlType::kToggle) {
    const bool letterbox_blocked = IsLetterboxBlockedByWindowStretch(*item);
    if (letterbox_blocked) {
      item->bool_value = false;
      content.textProperties.color = [XeniaTheme textSecondary];
      content.secondaryText =
          ToNSString(item->subtitle + " Stretch screen mode disables this.");
    } else {
      content.secondaryText = [self secondaryTextForItem:*item];
    }
    UISwitch* toggle = [self switchForCell:cell];
    toggle.on = item->bool_value;
    toggle.enabled = !letterbox_blocked;
    toggle.accessibilityLabel = content.text;
    toggle.accessibilityValue =
        toggle.on ? NSLocalizedString(@"On", @"config view controller: accessibility value")
                  : NSLocalizedString(@"Off", @"config view controller: accessibility value");
    toggle.accessibilityHint = content.secondaryText;
    cell.contentConfiguration = content;
    cell.accessoryView = toggle;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.isAccessibilityElement = NO;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (item->control_type == IOSConfigControlType::kInteger) {
    content.secondaryText = [self secondaryTextForItem:*item];
    UITextField* field = [self fieldForCell:cell
                                        tag:kXeniaConfigIntegerFieldTag
                                  endAction:@selector(integerFieldChanged:)];
    field.keyboardType = UIKeyboardTypeNumbersAndPunctuation;
    [self configureField:field
                    text:[NSString stringWithFormat:@"%lld", item->integer_value]
                    font:field_font
               baseWidth:100.0];
    cell.contentConfiguration = content;
    cell.accessoryView = field;
    cell.accessoryType = UITableViewCellAccessoryNone;
    // The row is the VoiceOver element (the field alone has no label); activating it
    // focuses the field through didSelectRow.
    field.isAccessibilityElement = NO;
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = content.text;
    cell.accessibilityValue = field.text;
    cell.accessibilityHint =
        [NSLocalizedString(@"Double tap to edit. ", @"config view controller: accessibility hint")
            stringByAppendingString:content.secondaryText ?: @""];
    cell.accessibilityTraits = UIAccessibilityTraitNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (item->control_type == IOSConfigControlType::kDouble) {
    content.secondaryText = [self secondaryTextForItem:*item];
    UITextField* field = [self fieldForCell:cell
                                        tag:kXeniaConfigDoubleFieldTag
                                  endAction:@selector(doubleFieldChanged:)];
    field.keyboardType = UIKeyboardTypeDecimalPad;
    [self configureField:field
                    text:[NSString stringWithFormat:@"%.6g", item->double_value]
                    font:field_font
               baseWidth:100.0];
    cell.contentConfiguration = content;
    cell.accessoryView = field;
    cell.accessoryType = UITableViewCellAccessoryNone;
    // The row is the VoiceOver element (the field alone has no label); activating it
    // focuses the field through didSelectRow.
    field.isAccessibilityElement = NO;
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = content.text;
    cell.accessibilityValue = field.text;
    cell.accessibilityHint =
        [NSLocalizedString(@"Double tap to edit. ", @"config view controller: accessibility hint")
            stringByAppendingString:content.secondaryText ?: @""];
    cell.accessibilityTraits = UIAccessibilityTraitNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (item->control_type == IOSConfigControlType::kString ||
             item->control_type == IOSConfigControlType::kPath) {
    content.secondaryText = [self secondaryTextForItem:*item];
    UITextField* field = [self fieldForCell:cell
                                        tag:kXeniaConfigStringFieldTag
                                  endAction:@selector(stringFieldChanged:)];
    field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    field.autocorrectionType = UITextAutocorrectionTypeNo;
    [self configureField:field text:ToNSString(item->string_value) font:text_font baseWidth:120.0];
    cell.contentConfiguration = content;
    cell.accessoryView = field;
    cell.accessoryType = UITableViewCellAccessoryNone;
    // The row is the VoiceOver element (the field alone has no label); activating it
    // focuses the field through didSelectRow.
    field.isAccessibilityElement = NO;
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = content.text;
    cell.accessibilityValue = field.text;
    cell.accessibilityHint =
        [NSLocalizedString(@"Double tap to edit. ", @"config view controller: accessibility hint")
            stringByAppendingString:content.secondaryText ?: @""];
    cell.accessibilityTraits = UIAccessibilityTraitNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (item->control_type == IOSConfigControlType::kAction) {
    const bool destructive_action = item->action == IOSConfigAction::kResetGameSettings ||
                                    item->action == IOSConfigAction::kResetAllSettings;
    content.textProperties.color =
        destructive_action ? [UIColor systemRedColor] : self.view.tintColor;
    content.secondaryText = ToNSString(item->subtitle);
    cell.contentConfiguration = content;
    cell.accessoryView = nil;
    cell.accessoryType = destructive_action ? UITableViewCellAccessoryNone
                                            : UITableViewCellAccessoryDisclosureIndicator;
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = content.text;
    cell.accessibilityValue = content.secondaryText;
    cell.accessibilityHint =
        item->action == IOSConfigAction::kResetAllSettings
            ? NSLocalizedString(@"Asks before putting every setting back to its default.",
                                @"config view controller: accessibility hint")
        : destructive_action ? NSLocalizedString(@"Deletes saved game-specific overrides.",
                                                 @"config view controller: user-visible text")
                             : NSLocalizedString(@"Opens another settings page.",
                                                 @"config view controller: user-visible text");
    cell.accessibilityTraits = UIAccessibilityTraitButton;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  } else {
    // kChoiceInt32, kChoiceUInt32, kChoiceUInt64, kChoiceString, kEnum: the value sits on the
    // trailing edge, like in Settings; the description stays below the title.
    std::string value_title = ChoiceTitleForItem(*item);
    content.secondaryText = [self secondaryTextForItem:*item];
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.accessoryView = XeniaConfigMakeValueAccessoryView(
        ToNSString(value_title), [self estimatedTableAccessoryWidth] * 0.5, YES);
    cell.isAccessibilityElement = YES;
    cell.accessibilityLabel = ToNSString(item->title);
    cell.accessibilityValue = value_title.empty() ? nil : ToNSString(value_title);
    cell.accessibilityHint = NSLocalizedString(@"Opens choices for this setting.",
                                               @"config view controller: accessibility hint");
    cell.accessibilityTraits = UIAccessibilityTraitButton;
    cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  }

  return cell;
}

// Swipe to put one changed setting back to its default.
- (UISwipeActionsConfiguration*)tableView:(UITableView*)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item || game_title_id_ || item->control_type == IOSConfigControlType::kAction ||
      IsLetterboxBlockedByWindowStretch(*item)) {
    return nil;
  }
  IOSConfigItem probe = *item;
  if (!IOSConfigResetItemToDefault(&probe)) {
    return nil;
  }
  UIContextualAction* reset = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleNormal
                          title:NSLocalizedString(@"Reset",
                                                  @"config view controller: swipe action title")
                        handler:^(__unused UIContextualAction* action,
                                  __unused UIView* source_view, void (^completion)(BOOL)) {
                          IOSConfigItem* current = [self itemAtIndexPath:indexPath];
                          if (current && IOSConfigResetItemToDefault(current)) {
                            [self markPendingChangesForItem:current];
                            if (live_override_) {
                              [self applyLiveOverrideForItem:current];
                            }
                            [self configItemDidChange:current];
                            [self.tableView reloadRowsAtIndexPaths:@[ indexPath ]
                                                  withRowAnimation:UITableViewRowAnimationNone];
                          }
                          completion(YES);
                        }];
  reset.backgroundColor = [XeniaTheme sectionIconOrange];
  return [UISwipeActionsConfiguration configurationWithActions:@[ reset ]];
}

- (void)toggleChanged:(UISwitch*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  if (!indexPath) {
    return;
  }
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item || item->control_type != IOSConfigControlType::kToggle) {
    return;
  }
  if (IsLetterboxBlockedByWindowStretch(*item)) {
    item->bool_value = false;
    [sender setOn:NO animated:YES];
    sender.accessibilityValue =
        NSLocalizedString(@"Off", @"config view controller: accessibility value");
    [self markPendingChangesForItem:item];
    if (live_override_) {
      [self applyLiveOverrideForItem:item];
    }
    [self configItemDidChange:item];
    return;
  }
  item->bool_value = sender.isOn;
  sender.accessibilityValue =
      sender.isOn ? NSLocalizedString(@"On", @"config view controller: accessibility value")
                  : NSLocalizedString(@"Off", @"config view controller: accessibility value");
  [self markPendingChangesForItem:item];
  if (live_override_) {
    [self applyLiveOverrideForItem:item];
  }
  [self configItemDidChange:item];
  [self refreshSecondaryTextForItem:item];
}

- (void)textFieldEditingChanged:(UITextField*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  if (!indexPath) {
    return;
  }
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item) {
    return;
  }
  const char* utf8 = [sender.text UTF8String];
  switch (item->control_type) {
    case IOSConfigControlType::kInteger: {
      if (utf8) {
        char* end = nullptr;
        errno = 0;
        long long parsed = std::strtoll(utf8, &end, 10);
        if (end != utf8 && *end == '\0' && errno == 0 &&
            IOSConfigValidateIntegerValue(item->key, parsed, nullptr)) {
          item->integer_value = static_cast<int64_t>(parsed);
          [self configItemDidChange:item];
        }
      }
    } break;
    case IOSConfigControlType::kDouble: {
      if (utf8) {
        char* end = nullptr;
        double parsed = std::strtod(utf8, &end);
        if (end != utf8 && *end == '\0' &&
            IOSConfigValidateDoubleValue(item->key, parsed, nullptr)) {
          item->double_value = parsed;
          [self configItemDidChange:item];
        }
      }
    } break;
    case IOSConfigControlType::kString:
    case IOSConfigControlType::kPath:
      item->string_value = utf8 ? utf8 : "";
      [self configItemDidChange:item];
      break;
    default:
      return;
  }
  [self markPendingChangesForItem:item];
  [self.tableView cellForRowAtIndexPath:indexPath].accessibilityValue = sender.text;
}

- (void)integerFieldChanged:(UITextField*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  if (!indexPath) return;
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item || item->control_type != IOSConfigControlType::kInteger) return;

  const char* utf8 = [sender.text UTF8String];
  if (!utf8) return;
  char* end = nullptr;
  errno = 0;
  long long parsed = std::strtoll(utf8, &end, 10);
  std::string problem;
  if (end == utf8 || *end != '\0' || errno == ERANGE) {
    problem = "Enter a whole number.";
  } else if (!IOSConfigValidateIntegerValue(item->key, parsed, &problem)) {
    // |problem| holds the allowed range.
  } else {
    item->integer_value = static_cast<int64_t>(parsed);
    [self markPendingChangesForItem:item];
    if (live_override_) {
      [self applyLiveOverrideForItem:item];
    }
    [self configItemDidChange:item];
    [self refreshSecondaryTextForItem:item];
    return;
  }
  sender.text = [NSString stringWithFormat:@"%lld", item->integer_value];
  [self.tableView cellForRowAtIndexPath:indexPath].accessibilityValue = sender.text;
  XEPresentOKAlert(
      self, NSLocalizedString(@"Value Not Accepted", @"config view controller: user-visible text"),
      [NSString stringWithFormat:NSLocalizedString(@"%1$@ was not changed. %2$@",
                                                   @"config view controller: user-visible text"),
                                 ToNSString(item->title), ToNSString(problem)]);
}

- (void)doubleFieldChanged:(UITextField*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  if (!indexPath) return;
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item || item->control_type != IOSConfigControlType::kDouble) return;

  const char* utf8 = [sender.text UTF8String];
  if (!utf8) return;
  char* end = nullptr;
  double parsed = std::strtod(utf8, &end);
  std::string problem;
  if (end == utf8 || *end != '\0') {
    problem = "Enter a number.";
  } else if (!IOSConfigValidateDoubleValue(item->key, parsed, &problem)) {
    // |problem| holds the allowed range.
  } else {
    item->double_value = parsed;
    [self markPendingChangesForItem:item];
    if (live_override_) {
      [self applyLiveOverrideForItem:item];
    }
    [self configItemDidChange:item];
    [self refreshSecondaryTextForItem:item];
    return;
  }
  sender.text = [NSString stringWithFormat:@"%.6g", item->double_value];
  [self.tableView cellForRowAtIndexPath:indexPath].accessibilityValue = sender.text;
  XEPresentOKAlert(
      self, NSLocalizedString(@"Value Not Accepted", @"config view controller: user-visible text"),
      [NSString stringWithFormat:NSLocalizedString(@"%1$@ was not changed. %2$@",
                                                   @"config view controller: user-visible text"),
                                 ToNSString(item->title), ToNSString(problem)]);
}

- (void)stringFieldChanged:(UITextField*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  if (!indexPath) return;
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item) return;
  if (item->control_type != IOSConfigControlType::kString &&
      item->control_type != IOSConfigControlType::kPath) {
    return;
  }

  item->string_value = sender.text ? [sender.text UTF8String] : "";
  [self markPendingChangesForItem:item];
  if (live_override_) {
    [self applyLiveOverrideForItem:item];
  }
  [self configItemDidChange:item];
  [self refreshSecondaryTextForItem:item];
}

- (void)applyLiveOverrideForItem:(IOSConfigItem*)item {
  if (!item || item->key.empty()) return;
  // The game's threads read these cvars, so assign them while it is paused.
  const IOSConfigItem value = *item;
  auto apply = [value]() { ApplyLiveOverrideValue(value); };
  if (app_context_) {
    app_context_->ApplyLiveSettings(apply);
  } else {
    apply();
  }

  if (game_title_id_) {
    IOSConfigSection section;
    section.items.push_back(*item);
    std::vector<IOSConfigSection> sections;
    sections.push_back(std::move(section));
    std::set<std::string> dirty_key{item->key};
    ApplyIOSConfigSectionsToGameConfig(sections, game_title_id_, dirty_key);
  }
}

- (void)pushCatalogKind:(IOSConfigCatalogKind)catalogKind {
  XeniaConfigViewController* config_vc = nil;
  if (catalogKind == IOSConfigCatalogKind::kAllCvars ||
      catalogKind == IOSConfigCatalogKind::kDebugSettings ||
      catalogKind == IOSConfigCatalogKind::kAdvanced) {
    config_vc = [[XeniaIOSDebugSettingsViewController alloc]
        initWithCatalogKind:catalogKind
               liveOverride:NO
                gameTitleID:game_title_id_
                  gameTitle:game_title_];
  } else {
    config_vc = [[XeniaConfigViewController alloc] initWithCatalogKind:catalogKind
                                                                  style:UITableViewStyleInsetGrouped
                                                            gameTitleID:game_title_id_
                                                              gameTitle:game_title_];
  }
  config_vc.dismissalHandler = self.dismissalHandler;
  config_vc.showsRootDismissButton = self.showsRootDismissButton;
  config_vc.appContext = app_context_;
  [self.navigationController pushViewController:config_vc animated:YES];
  [config_vc release];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item) {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    return;
  }

  if (item->control_type == IOSConfigControlType::kToggle) {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    return;
  }

  // Text-input types: focus the accessory text field.
  if (item->control_type == IOSConfigControlType::kInteger ||
      item->control_type == IOSConfigControlType::kDouble ||
      item->control_type == IOSConfigControlType::kString ||
      item->control_type == IOSConfigControlType::kPath) {
    UITableViewCell* cell = [tableView cellForRowAtIndexPath:indexPath];
    if ([cell.accessoryView isKindOfClass:[UITextField class]]) {
      [(UITextField*)cell.accessoryView becomeFirstResponder];
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    return;
  }

  if (item->control_type == IOSConfigControlType::kAction) {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    switch (item->action) {
      case IOSConfigAction::kOpenAdvancedSettings:
        [self pushCatalogKind:IOSConfigCatalogKind::kAdvanced];
        break;
      case IOSConfigAction::kOpenDiagnosticsSettings:
        [self pushCatalogKind:IOSConfigCatalogKind::kDiagnostics];
        break;
      case IOSConfigAction::kOpenAllConfigSettings:
        [self pushCatalogKind:IOSConfigCatalogKind::kAllCvars];
        break;
      case IOSConfigAction::kViewRecentLog: {
        XeniaLogViewController* log_vc = [[XeniaLogViewController alloc] init];
        [self.navigationController pushViewController:log_vc animated:YES];
        [log_vc release];
      } break;
      case IOSConfigAction::kManageExternalFolders: {
        XeniaIOSExternalFoldersViewController* folders_vc =
            [[XeniaIOSExternalFoldersViewController alloc] init];
        [self.navigationController pushViewController:folders_vc animated:YES];
        [folders_vc release];
      } break;
      case IOSConfigAction::kResetGameSettings:
        [self confirmResetGameSettings];
        break;
      case IOSConfigAction::kResetAllSettings:
        [self confirmResetAllSettings];
        break;
      case IOSConfigAction::kNone:
      default:
        break;
    }
    return;
  }

  NSIndexPath* selected_index_path = indexPath;
  XeniaChoiceListViewController* choice_vc = [[XeniaChoiceListViewController alloc]
      initWithTitle:ToNSString(item->title)
           subtitle:ToNSString(item->subtitle)
            choices:item->choices
      selectedValue:item->choice_value
        onSelection:^(int64_t selected_value) {
          IOSConfigItem* selected_item = [self itemAtIndexPath:selected_index_path];
          if (!selected_item) {
            return;
          }
          selected_item->choice_value = selected_value;
          if ((selected_item->control_type == IOSConfigControlType::kChoiceString ||
               selected_item->control_type == IOSConfigControlType::kEnum) &&
              selected_value >= 0 &&
              selected_value < static_cast<int64_t>(selected_item->choice_string_values.size())) {
            selected_item->string_value =
                selected_item->choice_string_values[static_cast<size_t>(selected_value)];
          }
          [self markPendingChangesForItem:selected_item];
          if (live_override_) {
            [self applyLiveOverrideForItem:selected_item];
          }
          [self configItemDidChange:selected_item];
          [self.tableView reloadRowsAtIndexPaths:@[ selected_index_path ]
                                withRowAnimation:UITableViewRowAnimationNone];
        }];
  [self.navigationController pushViewController:choice_vc animated:YES];
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

- (void)confirmResetGameSettings {
  if (!game_title_id_) {
    return;
  }

  NSString* game_name = game_title_.length
                            ? game_title_
                            : NSLocalizedString(@"this game", @"config view controller: title");
  NSString* message = [NSString
      stringWithFormat:NSLocalizedString(
                           @"Delete saved overrides for %@ and discard unsaved changes in "
                           @"this sheet?",
                           @"config view controller: message"),
                       game_name];
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Reset Game Settings?",
                                                 @"config view controller: title")
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm addAction:[UIAlertAction actionWithTitle:NSLocalizedString(
                                                        @"Cancel", @"config view controller: title")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
  [confirm
      addAction:
          [UIAlertAction
              actionWithTitle:NSLocalizedString(@"Reset", @"config view controller: title")
                        style:UIAlertActionStyleDestructive
                      handler:^(__unused UIAlertAction* action) {
                        const bool deleted = xe::ui::DeleteIOSGameConfig(game_title_id_);
                        if (deleted) {
                          dirty_keys_.clear();
                          hasPendingChanges_ = NO;
                          saveButton_.enabled = NO;
                          [self reloadSectionsFromCatalog];
                        }

                        NSString* title =
                            deleted
                                ? NSLocalizedString(@"Game Settings Reset",
                                                    @"config view controller: title")
                                : NSLocalizedString(@"Game Settings Not Reset",
                                                    @"config view controller: user-visible text");
                        NSString* result_message =
                            deleted
                                ? NSLocalizedString(@"Deleted title-specific overrides. "
                                                    @"Relaunch the game before testing.",
                                                    @"config view controller: user-visible text")
                                : NSLocalizedString(@"Failed to delete title-specific "
                                                    @"overrides. Check xenia.log.",
                                                    @"config view controller: user-visible text");
                        UIAlertController* result = [UIAlertController
                            alertControllerWithTitle:title
                                             message:result_message
                                      preferredStyle:UIAlertControllerStyleAlert];
                        [result addAction:[UIAlertAction
                                              actionWithTitle:NSLocalizedString(
                                                                  @"Done",
                                                                  @"config view controller: title")
                                                        style:UIAlertActionStyleDefault
                                                      handler:nil]];
                        [self presentViewController:result animated:YES completion:nil];
                      }]];
  [self presentViewController:confirm animated:YES completion:nil];
}

- (void)confirmResetAllSettings {
  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Reset All Settings?",
                                                 @"config view controller: title")
                       message:NSLocalizedString(
                                   @"Every XeniOS setting goes back to its default, including the "
                                   @"display scaling mode. Games, saves, profiles and their "
                                   @"sign-ins, touch layouts, per-game overrides, the shader "
                                   @"compile indicator, Online Features and Share Diagnostics "
                                   @"are kept. Relaunch XeniOS afterwards.",
                                   @"config view controller: message")
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm addAction:[UIAlertAction actionWithTitle:NSLocalizedString(
                                                        @"Cancel", @"config view controller: title")
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
  [confirm
      addAction:[UIAlertAction
                    actionWithTitle:NSLocalizedString(@"Reset", @"config view controller: title")
                              style:UIAlertActionStyleDestructive
                            handler:^(__unused UIAlertAction* action) {
                              bool ok = false;
                              auto reset = [&ok]() { ok = ResetAllIOSConfigToDefaults(); };
                              if (app_context_) {
                                app_context_->ApplyLiveSettings(reset);
                              } else {
                                reset();
                              }
                              dirty_keys_.clear();
                              hasPendingChanges_ = NO;
                              saveButton_.enabled = NO;
                              [self reloadSectionsFromCatalog];
                              XEPresentOKAlert(
                                  self,
                                  ok ? NSLocalizedString(
                                           @"Settings Reset",
                                           @"config view controller: user-visible text")
                                     : NSLocalizedString(
                                           @"Reset Failed",
                                           @"config view controller: user-visible text"),
                                  ok ? NSLocalizedString(
                                           @"All settings are back to their "
                                           @"defaults. Relaunch XeniOS before "
                                           @"testing.",
                                           @"config view controller: user-visible text")
                                     : NSLocalizedString(
                                           @"Settings could not be reset. Check "
                                           @"xenia.log.",
                                           @"config view controller: user-visible text"));
                            }]];
  [self presentViewController:confirm animated:YES completion:nil];
}

- (void)dismissSettingsControllerAnimated:(BOOL)animated {
  void (^dismissal_handler)(void) = [dismissal_handler_ copy];
  [self dismissViewControllerAnimated:animated
                           completion:^{
                             if (dismissal_handler) {
                               dismissal_handler();
                             }
                             [dismissal_handler release];
                           }];
}

- (void)cancelTapped:(id)sender {
  if (!hasPendingChanges_) {
    [self dismissSettingsControllerAnimated:YES];
    return;
  }

  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Discard Changes?",
                                                 @"config view controller: title")
                       message:NSLocalizedString(@"You have unsaved setting changes.",
                                                 @"config view controller: message")
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Keep Editing",
                                                                 @"config view controller: title")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];
  [confirm
      addAction:[UIAlertAction
                    actionWithTitle:NSLocalizedString(@"Discard", @"config view controller: title")
                              style:UIAlertActionStyleDestructive
                            handler:^(__unused UIAlertAction* action) {
                              [self dismissSettingsControllerAnimated:YES];
                            }]];
  [self presentViewController:confirm animated:YES completion:nil];
}

- (void)backTapped:(id)sender {
  (void)sender;
  if (!hasPendingChanges_) {
    [self.navigationController popViewControllerAnimated:YES];
    return;
  }

  UIAlertController* confirm = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"Discard Changes?",
                                                 @"config view controller: title")
                       message:NSLocalizedString(@"You have unsaved setting changes.",
                                                 @"config view controller: message")
                preferredStyle:UIAlertControllerStyleAlert];
  [confirm
      addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Keep Editing",
                                                                 @"config view controller: title")
                                         style:UIAlertActionStyleCancel
                                       handler:nil]];
  [confirm
      addAction:[UIAlertAction
                    actionWithTitle:NSLocalizedString(@"Discard", @"config view controller: title")
                              style:UIAlertActionStyleDestructive
                            handler:^(__unused UIAlertAction* action) {
                              [self.navigationController popViewControllerAnimated:YES];
                            }]];
  [self presentViewController:confirm animated:YES completion:nil];
}

- (void)saveTapped:(id)sender {
  if (live_override_) {
    return;
  }
  [self.view endEditing:YES];
  [self.tableView endEditing:YES];
  const std::vector<IOSConfigSection> sections_for_save = [self sectionsForSaving];
  BOOL saved = game_title_id_
                   ? (ApplyIOSConfigSectionsToGameConfig(sections_for_save, game_title_id_,
                                                         dirty_keys_)
                          ? YES
                          : NO)
                   : (ApplyIOSConfigSections(sections_for_save, &dirty_keys_) ? YES : NO);
  hasPendingChanges_ = NO;
  dirty_keys_.clear();
  saveButton_.enabled = NO;

  NSString* title = nil;
  NSString* message = nil;
  if (game_title_id_) {
    title = saved ? NSLocalizedString(@"Game Settings Saved", @"config view controller: title")
                  : NSLocalizedString(@"Save Completed With Warnings",
                                      @"config view controller: title");
    message = saved ? NSLocalizedString(
                          @"Saved title-specific overrides. Relaunch the game before testing them.",
                          @"config view controller: message")
                    : NSLocalizedString(
                          @"Some title-specific overrides could not be saved. Check xenia.log.",
                          @"config view controller: message");
  } else {
    title = saved ? NSLocalizedString(@"Settings Saved", @"config view controller: title")
                  : NSLocalizedString(@"Save Completed With Warnings",
                                      @"config view controller: title");
    message =
        saved
            ? NSLocalizedString(@"Saved to XeniOS settings.\n\nFor reliable results, change "
                                @"settings before launching a game. If you saved while a game was "
                                @"already running, fully relaunch XeniOS before testing.",
                                @"config view controller: message")
            : NSLocalizedString(@"Some settings could not be applied. Check xenia.log.\n\nAny "
                                @"settings that were saved should be tested after a full XeniOS "
                                @"relaunch, or by changing them before launching a game.",
                                @"config view controller: user-visible text");
  }
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Done", @"config view controller: title")
                                 style:UIAlertActionStyleDefault
                               handler:^(__unused UIAlertAction* action) {
                                 [self dismissSettingsControllerAnimated:YES];
                               }]];
  [self presentViewController:alert animated:YES completion:nil];
}

@end
