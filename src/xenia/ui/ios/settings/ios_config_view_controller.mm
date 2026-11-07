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

#include <cmath>
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
#import "xenia/ui/ios/shared/ios_art_wash.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

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
static const NSInteger kXeniaConfigStepperLabelTag = 9104;
static const NSInteger kXeniaConfigSliderTitleTag = 9105;
static const NSInteger kXeniaConfigSliderValueTag = 9106;
static const NSInteger kXeniaConfigSliderTag = 9107;
static const NSInteger kXeniaConfigSliderMinLabelTag = 9108;
static const NSInteger kXeniaConfigSliderMaxLabelTag = 9109;

// A stepped value on its grid. Dividing by the step's inverse (10 for 0.1) gives the nearest
// double, so 1.2 saves as 1.2, not 1.2000000000000002, and repeated steps don't drift.
static double SnapToStep(double value, double step) {
  if (step < 1.0) {
    const double per_unit = std::round(1.0 / step);
    return std::round(value * per_unit) / per_unit;
  }
  return std::round(value / step) * step;
}

static bool IsSliderItem(const IOSConfigItem* item) {
  return item && item->control_type == IOSConfigControlType::kDouble && item->step > 0.0 &&
         !item->min_label.empty() && !item->max_label.empty();
}

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
  BOOL save_scheduled_;
  BOOL game_running_;
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
@synthesize gameRunning = game_running_;
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

// Whether the page has emulator settings, which the restart notice is about.
- (BOOL)hasConfigVarItems {
  for (const IOSConfigSection& section : sections_) {
    for (const IOSConfigItem& item : section.items) {
      if (item.storage == IOSConfigStorage::kConfigVar &&
          item.control_type != IOSConfigControlType::kAction) {
        return YES;
      }
    }
  }
  return NO;
}

// Changes save as they are made, so the note above the list is only needed where they don't take
// effect right away: a title's own overrides, or any page opened while a game is running.
- (BOOL)showsRestartNotice {
  return [self hasConfigVarItems] && (game_title_id_ || game_running_);
}

- (UIView*)restartNoticeHeaderView {
  UIView* container =
      [[[UIView alloc] initWithFrame:[self initialTableAccessoryFrameWithHeight:136.0]]
          autorelease];
  container.backgroundColor = [UIColor clearColor];

  UIView* card = [[[UIView alloc] init] autorelease];
  card.translatesAutoresizingMaskIntoConstraints = NO;
  // Plain explanatory text above the list, as in Settings, not a card.
  card.backgroundColor = [UIColor clearColor];
  [container addSubview:card];

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.translatesAutoresizingMaskIntoConstraints = NO;
  title.backgroundColor = [UIColor clearColor];
  // Only the per-game page needs a heading; the global note is one line of guidance.
  title.text = game_title_id_
                   ? NSLocalizedString(@"Game-Specific Settings", @"config view controller: title")
                   : nil;
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
                             @"These apply only to %1$@ (%2$@) and take effect the next time it "
                             @"starts.",
                             @"config view controller: label text"),
                         game_name, title_id];
  } else {
    body.text = NSLocalizedString(@"A game is running. Changes take effect after you relaunch "
                                  @"XeniOS.",
                                  @"config view controller: label text");
  }
  body.textColor = [XeniaTheme textSecondary];
  body.numberOfLines = 0;
  xe_apply_label_font(body, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
  [body setContentCompressionResistancePriority:UILayoutPriorityRequired
                                        forAxis:UILayoutConstraintAxisVertical];
  [card addSubview:body];

  [NSLayoutConstraint activateConstraints:@[
    [card.topAnchor constraintEqualToAnchor:container.topAnchor constant:8.0],
    [card.leadingAnchor constraintEqualToAnchor:container.leadingAnchor constant:16.0],
    [card.trailingAnchor constraintEqualToAnchor:container.trailingAnchor constant:-16.0],
    [card.bottomAnchor constraintEqualToAnchor:container.bottomAnchor constant:-4.0],

    [title.topAnchor constraintEqualToAnchor:card.topAnchor constant:4.0],
    [title.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [title.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],

    [body.topAnchor constraintEqualToAnchor:title.bottomAnchor
                                   constant:game_title_id_ ? 4.0 : 0.0],
    [body.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:16.0],
    [body.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-16.0],
    [body.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-4.0],
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
    self.navigationItem.subtitle = game_title_;
  }
  XEUseArtWash(self);
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
    // Under the title, not floating over the rows at the bottom of a phone in landscape.
    self.navigationItem.preferredSearchBarPlacement = UINavigationItemSearchBarPlacementStacked;
    self.definesPresentationContext = YES;
  }

  if (live_override_) {
    self.tableView.tableHeaderView = [self liveOverrideHeaderView];
    self.tableView.tableFooterView = nil;
    [self updateTableHeaderAndFooterLayout];
  } else {
    self.tableView.tableHeaderView =
        [self showsRestartNotice] ? [self restartNoticeHeaderView] : nil;
    self.tableView.tableFooterView = nil;
    [self updateTableHeaderAndFooterLayout];
  }

  [self updateNavigationItems];
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
      self.tableView.tableHeaderView =
          [self showsRestartNotice] ? [self restartNoticeHeaderView] : nil;
      self.tableView.tableFooterView = nil;
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
  // Settings save as they change, as in Settings. Changes made in one pass (a reset, a picker)
  // are written together on the next turn of the run loop.
  if (!save_scheduled_) {
    save_scheduled_ = YES;
    [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      save_scheduled_ = NO;
      [self savePendingChanges];
      [self release];
    });
  }
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

  // Matches every word of the query in the key, title, description, section and the current
  // value ("Metal" finds the backend), underscores read as spaces, and keeps each result under
  // its section.
  NSArray<NSString*>* words = [query
      componentsSeparatedByCharactersInSet:[NSCharacterSet
                                               characterSetWithCharactersInString:@" _\t"]];
  auto matches = [words](const std::string& text) {
    NSString* haystack = [ToNSString(text) stringByReplacingOccurrencesOfString:@"_"
                                                                     withString:@" "];
    for (NSString* word in words) {
      if (word.length &&
          [haystack rangeOfString:word
                          options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch]
                  .location == NSNotFound) {
        return false;
      }
    }
    return true;
  };
  std::vector<IOSConfigSection> results;
  for (const IOSConfigSection& section : full_sections_) {
    IOSConfigSection filtered;
    filtered.title = section.title;
    for (const IOSConfigItem& item : section.items) {
      std::string text =
          item.key + "\n" + item.title + "\n" + item.subtitle + "\n" + section.title;
      if (item.control_type != IOSConfigControlType::kToggle) {
        text += "\n" + IOSConfigItemValueText(item);
      }
      if (matches(text)) {
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
  // Pushed, the page picks up the root's ✕ (XEApplySheetHeader).
  if ([self isRootSettingsController] && shows_root_dismiss_button_) {
    XESetSheetClose(self, self, @selector(closeTapped:));
  }
  XESetSheetActions(self, @[]);
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
  NSString* title = ToNSString(sections_[section].title);
  // A first section named like the page ("Display" in Display) needs no header.
  if (section == 0 && [title isEqualToString:self.title]) {
    return nil;
  }
  return title;
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
  if (section < 0 || section >= static_cast<NSInteger>(sections_.size())) {
    return nil;
  }
  return sections_[section].footer.empty() ? nil : ToNSString(sections_[section].footer);
}

// Description plus, for a setting that was changed, its default on a second line.
- (NSString*)secondaryTextForItem:(const IOSConfigItem&)item {
  // nil, not "", for a row with no description, so it keeps a one-line height.
  NSString* base = item.subtitle.empty() ? nil : ToNSString(item.subtitle);
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

// Number pads have no return key, so they get a Done bar to close them.
- (UIToolbar*)numberPadAccessoryToolbar {
  UIToolbar* toolbar = [[[UIToolbar alloc] initWithFrame:CGRectMake(0, 0, 320, 44)] autorelease];
  UIBarButtonItem* flexible = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                           target:nil
                           action:nil] autorelease];
  UIBarButtonItem* done = [[[UIBarButtonItem alloc]
      initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                           target:self
                           action:@selector(numberPadDoneTapped:)] autorelease];
  toolbar.items = @[ flexible, done ];
  [toolbar sizeToFit];
  return toolbar;
}

- (void)numberPadDoneTapped:(id)sender {
  [self.view endEditing:YES];
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

// Rows in the glass groups of the charcoal page, as on the Settings list.
- (void)tableView:(UITableView* __unused)tableView
      willDisplayCell:(UITableViewCell*)cell
    forRowAtIndexPath:(NSIndexPath* __unused)indexPath {
  cell.backgroundColor = [XeniaTheme glassGroupFill];
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  if (IsSliderItem([self itemAtIndexPath:indexPath])) {
    return [self sliderCellForItem:*[self itemAtIndexPath:indexPath] tableView:tableView];
  }
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
  content.textProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  // Descriptions in the small secondary style, as in Settings, and kept to three lines in the
  // long All Settings list.
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  content.secondaryTextProperties.numberOfLines =
      catalog_kind_ == IOSConfigCatalogKind::kAllSettings ? 3 : 0;
  content.secondaryTextProperties.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  content.textToSecondaryTextVerticalPadding = 3.0;
  content.prefersSideBySideTextAndSecondaryText = NO;
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(11.0, 0.0, 11.0, 0.0);

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
    if (!field.inputAccessoryView) {
      field.inputAccessoryView = [self numberPadAccessoryToolbar];
    }
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
  } else if (item->control_type == IOSConfigControlType::kDouble && item->step > 0.0) {
    content.secondaryText = [self secondaryTextForItem:*item];
    cell.contentConfiguration = content;
    cell.accessoryView = [self stepperAccessoryForItem:*item];
    cell.accessoryType = UITableViewCellAccessoryNone;
    // The stepper carries the label and value; VoiceOver adjusts it directly.
    cell.isAccessibilityElement = NO;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
  } else if (item->control_type == IOSConfigControlType::kDouble) {
    content.secondaryText = [self secondaryTextForItem:*item];
    UITextField* field = [self fieldForCell:cell
                                        tag:kXeniaConfigDoubleFieldTag
                                  endAction:@selector(doubleFieldChanged:)];
    field.keyboardType = UIKeyboardTypeDecimalPad;
    if (!field.inputAccessoryView) {
      field.inputAccessoryView = [self numberPadAccessoryToolbar];
    }
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
        destructive_action ? [UIColor systemRedColor] : [XeniaTheme textPrimary];
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
    // At large text sizes the value gets its own line under the title, as in Settings, so the
    // title isn't squeezed against it.
    const BOOL value_below =
        !value_title.empty() &&
        UIContentSizeCategoryCompareToCategory(self.traitCollection.preferredContentSizeCategory,
                                               UIContentSizeCategoryExtraLarge) !=
            NSOrderedAscending;
    if (value_below) {
      NSMutableAttributedString* secondary = [[[NSMutableAttributedString alloc]
          initWithString:ToNSString(value_title)
              attributes:@{
                NSFontAttributeName : [UIFont preferredFontForTextStyle:UIFontTextStyleBody],
                NSForegroundColorAttributeName : [XeniaTheme textSecondary],
              }] autorelease];
      if (content.secondaryText.length) {
        [secondary appendAttributedString:
                       [[[NSAttributedString alloc]
                           initWithString:[@"\n" stringByAppendingString:content.secondaryText]
                               attributes:@{
                                 NSFontAttributeName :
                                     [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote],
                                 NSForegroundColorAttributeName : [XeniaTheme textSecondary],
                               }] autorelease]];
      }
      content.secondaryAttributedText = secondary;
    }
    cell.contentConfiguration = content;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.accessoryView = XeniaConfigMakeValueAccessoryView(
        value_below ? @"" : ToNSString(value_title), [self estimatedTableAccessoryWidth] * 0.5,
        YES);
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
  reset.backgroundColor = [XeniaTheme statusWarning];
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

// A range: the title with its value and unit on one line, and under it a slider between two
// words ("Slower", "Faster"), as in the Ranges component of the style spec.
- (UITableViewCell*)sliderCellForItem:(const IOSConfigItem&)item
                            tableView:(UITableView*)tableView {
  static NSString* const kSliderCellIdentifier = @"XeniaConfigSliderCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kSliderCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kSliderCellIdentifier] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;

    UILabel* title = [[[UILabel alloc] init] autorelease];
    title.tag = kXeniaConfigSliderTitleTag;
    title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    title.adjustsFontForContentSizeCategory = YES;
    title.textColor = [XeniaTheme textPrimary];
    title.numberOfLines = 0;
    UILabel* value = [[[UILabel alloc] init] autorelease];
    value.tag = kXeniaConfigSliderValueTag;
    value.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleBody]
        scaledFontForFont:[UIFont monospacedDigitSystemFontOfSize:17.0
                                                           weight:UIFontWeightRegular]];
    value.adjustsFontForContentSizeCategory = YES;
    value.textColor = [XeniaTheme textSecondary];
    [value setContentHuggingPriority:UILayoutPriorityRequired
                             forAxis:UILayoutConstraintAxisHorizontal];
    [value setContentCompressionResistancePriority:UILayoutPriorityRequired
                                           forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* top = [[[UIStackView alloc] initWithArrangedSubviews:@[ title, value ]]
        autorelease];
    top.axis = UILayoutConstraintAxisHorizontal;
    top.alignment = UIStackViewAlignmentFirstBaseline;
    top.spacing = 8.0;

    UILabel* min_label = [[[UILabel alloc] init] autorelease];
    UILabel* max_label = [[[UILabel alloc] init] autorelease];
    min_label.tag = kXeniaConfigSliderMinLabelTag;
    max_label.tag = kXeniaConfigSliderMaxLabelTag;
    for (UILabel* label in @[ min_label, max_label ]) {
      label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
      label.adjustsFontForContentSizeCategory = YES;
      label.textColor = [XeniaTheme textMuted];
      label.isAccessibilityElement = NO;
      [label setContentHuggingPriority:UILayoutPriorityRequired
                               forAxis:UILayoutConstraintAxisHorizontal];
      [label setContentCompressionResistancePriority:UILayoutPriorityRequired
                                             forAxis:UILayoutConstraintAxisHorizontal];
    }
    UISlider* slider = [[[UISlider alloc] init] autorelease];
    slider.tag = kXeniaConfigSliderTag;
    slider.minimumTrackTintColor = [XeniaTheme accent];
    [slider addTarget:self
                  action:@selector(sliderChanged:)
        forControlEvents:UIControlEventValueChanged];
    [slider addTarget:self
                  action:@selector(sliderFinished:)
        forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside |
                         UIControlEventTouchCancel];
    UIStackView* bottom = [[[UIStackView alloc]
        initWithArrangedSubviews:@[ min_label, slider, max_label ]] autorelease];
    bottom.axis = UILayoutConstraintAxisHorizontal;
    bottom.alignment = UIStackViewAlignmentCenter;
    bottom.spacing = 12.0;

    UIStackView* stack = [[[UIStackView alloc] initWithArrangedSubviews:@[ top, bottom ]]
        autorelease];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 8.0;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [cell.contentView addSubview:stack];
    UILayoutGuide* margins = cell.contentView.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
      [stack.topAnchor constraintEqualToAnchor:margins.topAnchor constant:4.0],
      [stack.bottomAnchor constraintEqualToAnchor:margins.bottomAnchor constant:-4.0],
      [stack.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
      [stack.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
    ]];
  }

  NSString* value_text = ToNSString(IOSConfigItemValueText(item));
  UILabel* title = (UILabel*)[cell.contentView viewWithTag:kXeniaConfigSliderTitleTag];
  UILabel* value = (UILabel*)[cell.contentView viewWithTag:kXeniaConfigSliderValueTag];
  UISlider* slider = (UISlider*)[cell.contentView viewWithTag:kXeniaConfigSliderTag];
  title.text = ToNSString(item.title);
  title.isAccessibilityElement = NO;
  value.text = value_text;
  value.isAccessibilityElement = NO;
  ((UILabel*)[cell.contentView viewWithTag:kXeniaConfigSliderMinLabelTag]).text =
      ToNSString(item.min_label);
  ((UILabel*)[cell.contentView viewWithTag:kXeniaConfigSliderMaxLabelTag]).text =
      ToNSString(item.max_label);
  // A hand-edited value outside the range stays reachable.
  slider.minimumValue = static_cast<float>(MIN(item.min_value, item.double_value));
  slider.maximumValue = static_cast<float>(MAX(item.max_value, item.double_value));
  slider.value = static_cast<float>(item.double_value);
  // The slider is the one VoiceOver element: "Vertical Speed, 1.2×, adjustable".
  slider.accessibilityLabel = ToNSString(item.title);
  slider.accessibilityValue = value_text;
  return cell;
}

- (void)sliderChanged:(UISlider*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  if (!indexPath) return;
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!IsSliderItem(item)) return;

  const double snapped = SnapToStep(sender.value, item->step);
  if (snapped == item->double_value) {
    return;
  }
  item->double_value = snapped;
  NSString* text = ToNSString(IOSConfigItemValueText(*item));
  sender.accessibilityValue = text;
  UITableViewCell* cell = [self.tableView cellForRowAtIndexPath:indexPath];
  ((UILabel*)[cell.contentView viewWithTag:kXeniaConfigSliderValueTag]).text = text;

  if (sender.isTracking && !live_override_) {
    // Saved once the thumb is let go: in a game each save applies the settings live, which can
    // hold the main thread, so it shouldn't happen at every step of a drag.
    if (!item->key.empty()) {
      dirty_keys_.insert(item->key);
    }
    hasPendingChanges_ = YES;
  } else {
    [self markPendingChangesForItem:item];
  }
  if (live_override_) {
    [self applyLiveOverrideForItem:item];
  }
  [self configItemDidChange:item];
}

// The thumb settles on the step the value snapped to.
- (void)sliderFinished:(UISlider*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  IOSConfigItem* item = indexPath ? [self itemAtIndexPath:indexPath] : nullptr;
  if (IsSliderItem(item)) {
    [sender setValue:static_cast<float>(item->double_value) animated:YES];
    if (hasPendingChanges_) {
      [self markPendingChangesForItem:item];
    }
  }
}

// A stepped decimal setting: its value with its unit ("0.10 s"), then a stepper.
- (UIView*)stepperAccessoryForItem:(const IOSConfigItem&)item {
  UILabel* label = [[[UILabel alloc] init] autorelease];
  label.tag = kXeniaConfigStepperLabelTag;
  label.text = ToNSString(IOSConfigItemValueText(item));
  label.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleBody]
      scaledFontForFont:[UIFont monospacedDigitSystemFontOfSize:17.0
                                                         weight:UIFontWeightRegular]];
  label.adjustsFontForContentSizeCategory = YES;
  label.textColor = [XeniaTheme textSecondary];
  label.isAccessibilityElement = NO;

  UIStepper* stepper = [[[UIStepper alloc] init] autorelease];
  // A hand-edited value outside the range stays reachable.
  stepper.minimumValue = MIN(item.min_value, item.double_value);
  stepper.maximumValue = MAX(item.max_value, item.double_value);
  stepper.stepValue = item.step;
  stepper.value = item.double_value;
  stepper.accessibilityLabel = ToNSString(item.title);
  stepper.accessibilityValue = label.text;
  [stepper addTarget:self
                action:@selector(stepperChanged:)
      forControlEvents:UIControlEventValueChanged];

  UIStackView* stack =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ label, stepper ]] autorelease];
  stack.axis = UILayoutConstraintAxisHorizontal;
  stack.alignment = UIStackViewAlignmentCenter;
  stack.spacing = 12.0;
  const CGSize size = [stack systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
  stack.frame = CGRectMake(0.0, 0.0, ceil(size.width), ceil(size.height));
  return stack;
}

- (void)stepperChanged:(UIStepper*)sender {
  CGPoint point = [sender convertPoint:CGPointZero toView:self.tableView];
  NSIndexPath* indexPath = [self.tableView indexPathForRowAtPoint:point];
  if (!indexPath) return;
  IOSConfigItem* item = [self itemAtIndexPath:indexPath];
  if (!item || item->control_type != IOSConfigControlType::kDouble || item->step <= 0.0) return;

  item->double_value = SnapToStep(sender.value, item->step);
  NSString* text = ToNSString(IOSConfigItemValueText(*item));
  sender.accessibilityValue = text;
  UIView* stack = sender.superview;
  UILabel* label = [[stack viewWithTag:kXeniaConfigStepperLabelTag] isKindOfClass:[UILabel class]]
                       ? (UILabel*)[stack viewWithTag:kXeniaConfigStepperLabelTag]
                       : nil;
  label.text = text;
  const CGSize size = [stack systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
  stack.frame = CGRectMake(0.0, 0.0, ceil(size.width), ceil(size.height));
  [stack.superview setNeedsLayout];

  [self markPendingChangesForItem:item];
  if (live_override_) {
    [self applyLiveOverrideForItem:item];
  }
  [self configItemDidChange:item];
  [self refreshSecondaryTextForItem:item];
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

  // Advanced Debug toggles last for this session only: a harmful one would otherwise stick to
  // the title until its settings are reset, with nothing asking the player to keep it.
  const BOOL session_only = catalog_kind_ == IOSConfigCatalogKind::kDebugSettings ||
                            catalog_kind_ == IOSConfigCatalogKind::kAdvanced;
  if (game_title_id_ && !session_only) {
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
  config_vc.gameRunning = game_running_;
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

- (void)closeTapped:(id)sender {
  (void)sender;
  [self.view endEditing:YES];
  [self savePendingChanges];
  [self dismissSettingsControllerAnimated:YES];
}

- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];
  // A field still being edited commits on end editing; write it before the page goes away.
  [self.view endEditing:YES];
  [self savePendingChanges];
}

- (void)savePendingChanges {
  if (live_override_ || !hasPendingChanges_) {
    return;
  }
  const std::vector<IOSConfigSection> sections_for_save = [self sectionsForSaving];
  BOOL saved = NO;
  if (game_title_id_) {
    saved = ApplyIOSConfigSectionsToGameConfig(sections_for_save, game_title_id_, dirty_keys_) ? YES
                                                                                              : NO;
  } else {
    // The running game's threads read these cvars, so assign them while it is paused.
    bool applied = false;
    auto apply = [&]() { applied = ApplyIOSConfigSections(sections_for_save, &dirty_keys_); };
    if (app_context_) {
      app_context_->ApplyLiveSettings(apply);
    } else {
      apply();
    }
    saved = applied ? YES : NO;
  }
  hasPendingChanges_ = NO;
  dirty_keys_.clear();
  if (!saved) {
    XEPresentOKAlert(self,
                     NSLocalizedString(@"Some Settings Weren't Saved",
                                       @"config view controller: title"),
                     NSLocalizedString(@"Check xenia.log for details.",
                                       @"config view controller: message"));
  }
}

@end
