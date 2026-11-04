/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_launcher_overlay_view.h"

#include <cmath>

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_game_tile_cell.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

@implementation XeniaIOSLauncherGameSnapshot

- (void)dealloc {
  [_title release];
  [_contentTypeName release];
  [_compatStatus release];
  [_iconData release];
  [super dealloc];
}

@end

// Target for the empty-state button. The button's action lives inside the
// collection view's configuration, so it must not retain the overlay; the
// overlay clears `owner` in -dealloc.
@interface XeniaLauncherEmptyStateActionProxy : NSObject
@property(nonatomic, assign) XeniaIOSLauncherOverlayView* owner;
- (void)importGames;
- (void)openDashboard;
@end

@interface XeniaIOSLauncherOverlayView () <UICollectionViewDataSource,
                                           UICollectionViewDataSourcePrefetching,
                                           UICollectionViewDelegateFlowLayout>
@property(nonatomic, strong) NSArray<XeniaIOSLauncherGameSnapshot*>* games;
@property(nonatomic, assign) BOOL jitAcquired;
@property(nonatomic, assign) BOOL memoryEntitlementEnabled;
- (void)importTapped:(UIButton*)sender;
- (void)dashboardTapped:(UIButton*)sender;
@end

@implementation XeniaLauncherEmptyStateActionProxy
- (void)importGames {
  if (_owner.actionsEnabled) {
    [_owner importTapped:nil];
  }
}
- (void)openDashboard {
  if (_owner.actionsEnabled) {
    [_owner dashboardTapped:nil];
  }
}
@end

namespace {
// Minimum touch target for the plain icon buttons.
constexpr CGFloat kMinTapTarget = 44.0;
// Height over width of a tile's art: a box cover (300 x 219).
constexpr CGFloat kCoverAspect = 300.0 / 219.0;
// Space above the notices and between the Library row and the first games.
constexpr CGFloat kLibraryHeaderTop = 12.0;
constexpr CGFloat kLibraryHeaderGap = 12.0;

// The Xbox 360 Dashboard tile's art, at a cover's size: the accent fading
// into the surface, under a grid of tiles. Drawn once per appearance.
UIImage* xe_dashboard_tile_art(UITraitCollection* traits) {
  static UIImage* art[2] = {nil, nil};
  UIImage*& cached = art[traits.userInterfaceStyle == UIUserInterfaceStyleDark ? 1 : 0];
  if (cached) {
    return cached;
  }
  const CGSize size = CGSizeMake(219.0, 300.0);
  UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
  format.scale = 2.0;
  format.opaque = YES;
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:size format:format] autorelease];
  UIColor* top = [[XeniaTheme accent] resolvedColorWithTraitCollection:traits];
  UIColor* bottom = [[XeniaTheme bgSurface2] resolvedColorWithTraitCollection:traits];
  UIImage* symbol =
      [[UIImage systemImageNamed:@"square.grid.2x2.fill"
               withConfiguration:[UIImageSymbolConfiguration
                                     configurationWithPointSize:64.0
                                                         weight:UIImageSymbolWeightSemibold]]
          imageWithTintColor:[UIColor colorWithWhite:1.0 alpha:0.92]
               renderingMode:UIImageRenderingModeAlwaysOriginal];
  cached = [[renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGGradientRef gradient = CGGradientCreateWithColors(
        space, (CFArrayRef) @[ (id)top.CGColor, (id)bottom.CGColor ], nullptr);
    CGContextDrawLinearGradient(context.CGContext, gradient, CGPointZero,
                                CGPointMake(0.0, size.height), 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(space);
    const CGSize symbol_size = symbol.size;
    [symbol drawInRect:CGRectMake((size.width - symbol_size.width) * 0.5,
                                  (size.height - symbol_size.height) * 0.42, symbol_size.width,
                                  symbol_size.height)];
  }] retain];
  return cached;
}

// A cover-sized placeholder for a game without cover art: the surface
// colours in a soft gradient, with the game's own icon (or a controller
// symbol) at a modest size in the middle, never stretched to fill the tile.
UIImage* xe_placeholder_cover_art(UIImage* icon, UITraitCollection* traits) {
  const CGSize size = CGSizeMake(219.0, 300.0);
  UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
  format.scale = 2.0;
  format.opaque = YES;
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:size format:format] autorelease];
  UIColor* top = [[XeniaTheme bgSurface2] resolvedColorWithTraitCollection:traits];
  UIColor* bottom = [[XeniaTheme bgSurface] resolvedColorWithTraitCollection:traits];
  UIImage* symbol = nil;
  if (!icon) {
    symbol = [[UIImage systemImageNamed:@"gamecontroller.fill"
                      withConfiguration:[UIImageSymbolConfiguration
                                            configurationWithPointSize:44.0
                                                                weight:UIImageSymbolWeightMedium]]
        imageWithTintColor:[[XeniaTheme textMuted] resolvedColorWithTraitCollection:traits]
             renderingMode:UIImageRenderingModeAlwaysOriginal];
  }
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGGradientRef gradient = CGGradientCreateWithColors(
        space, (CFArrayRef) @[ (id)top.CGColor, (id)bottom.CGColor ], nullptr);
    CGContextDrawLinearGradient(context.CGContext, gradient, CGPointZero,
                                CGPointMake(0.0, size.height), 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(space);
    if (icon) {
      const CGFloat side = 88.0;
      const CGRect rect = CGRectMake((size.width - side) * 0.5, (size.height - side) * 0.45, side,
                                     side);
      [[UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:side * 0.22] addClip];
      [icon drawInRect:rect];
    } else {
      const CGSize symbol_size = symbol.size;
      [symbol drawInRect:CGRectMake((size.width - symbol_size.width) * 0.5,
                                    (size.height - symbol_size.height) * 0.45, symbol_size.width,
                                    symbol_size.height)];
    }
  }];
}

bool xe_uses_accessibility_text(UITraitCollection* traits) {
  return UIContentSizeCategoryIsAccessibilityCategory(traits.preferredContentSizeCategory);
}

// A notice above the library: a status symbol, a short title with one line of
// detail under it, and an optional action on the trailing edge. Every notice
// uses this, so they line up and read the same.
UIView* xe_new_notice_card(UIImageView* icon, UILabel* title, UILabel* detail, UIButton* action) {
  // One row of the notices group (xe_new_notice_group), which draws the background.
  UIView* card = [[UIView alloc] init];
  card.translatesAutoresizingMaskIntoConstraints = NO;

  // |icon| shows a Settings-style badge (XESettingsBadgeImage).
  icon.translatesAutoresizingMaskIntoConstraints = NO;
  icon.contentMode = UIViewContentModeCenter;
  icon.isAccessibilityElement = NO;

  title.textColor = [XeniaTheme textPrimary];
  xe_apply_label_font(title, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
  title.numberOfLines = 0;
  title.accessibilityTraits = UIAccessibilityTraitHeader;
  detail.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(detail, UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
  detail.numberOfLines = 0;

  UIStackView* text = [[[UIStackView alloc] initWithArrangedSubviews:@[ title, detail ]] autorelease];
  text.axis = UILayoutConstraintAxisVertical;
  text.spacing = 1.0;

  NSMutableArray* items = [NSMutableArray arrayWithObjects:icon, text, nil];
  if (action) {
    [action setContentHuggingPriority:UILayoutPriorityRequired
                              forAxis:UILayoutConstraintAxisHorizontal];
    [action setContentCompressionResistancePriority:UILayoutPriorityRequired
                                            forAxis:UILayoutConstraintAxisHorizontal];
    [items addObject:action];
  }
  UIStackView* row = [[[UIStackView alloc] initWithArrangedSubviews:items] autorelease];
  row.translatesAutoresizingMaskIntoConstraints = NO;
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentCenter;
  row.spacing = 12.0;
  [card addSubview:row];
  [NSLayoutConstraint activateConstraints:@[
    [icon.widthAnchor constraintEqualToConstant:29.0],
    [icon.heightAnchor constraintEqualToConstant:29.0],
    [row.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14.0],
    [row.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-12.0],
    [row.topAnchor constraintEqualToAnchor:card.topAnchor constant:12.0],
    [row.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-12.0],
  ]];
  return card;
}

// The small capsule action a notice card carries.
// The notices in one rounded group, a hairline between rows, like an inset grouped list.
UIStackView* xe_new_notice_group(NSArray<UIView*>* rows, NSArray<UIView*>** separators_out) {
  NSMutableArray<UIView*>* items = [NSMutableArray array];
  NSMutableArray<UIView*>* separators = [NSMutableArray array];
  for (UIView* row in rows) {
    if (items.count) {
      UIView* line = [[[UIView alloc] init] autorelease];
      line.translatesAutoresizingMaskIntoConstraints = NO;
      UIView* hairline = [[[UIView alloc] init] autorelease];
      hairline.translatesAutoresizingMaskIntoConstraints = NO;
      hairline.backgroundColor = [XeniaTheme border];
      [line addSubview:hairline];
      [NSLayoutConstraint activateConstraints:@[
        [line.heightAnchor constraintEqualToConstant:1.0 / UIScreen.mainScreen.scale],
        [hairline.leadingAnchor constraintEqualToAnchor:line.leadingAnchor constant:56.0],
        [hairline.trailingAnchor constraintEqualToAnchor:line.trailingAnchor],
        [hairline.topAnchor constraintEqualToAnchor:line.topAnchor],
        [hairline.bottomAnchor constraintEqualToAnchor:line.bottomAnchor],
      ]];
      [items addObject:line];
      [separators addObject:line];
    }
    [items addObject:row];
  }
  UIStackView* group = [[UIStackView alloc] initWithArrangedSubviews:items];
  group.axis = UILayoutConstraintAxisVertical;
  group.backgroundColor = [XeniaTheme bgSurface];
  group.layer.cornerRadius = XeniaRadiusLg;
  group.layer.cornerCurve = kCACornerCurveContinuous;
  group.clipsToBounds = YES;
  if (separators_out) {
    *separators_out = separators;
  }
  return group;
}

// "● Playable": the status's colour in the dot only, the words in the secondary colour.
NSAttributedString* xe_status_text(NSString* text, UIColor* color) {
  UIFont* font = xe_scaled_system_font(UIFontTextStyleFootnote, 13.0, UIFontWeightRegular);
  NSMutableAttributedString* out = [[[NSMutableAttributedString alloc]
      initWithString:@"\u25CF "
          attributes:@{
            NSForegroundColorAttributeName : color,
            NSFontAttributeName : [UIFont systemFontOfSize:font.pointSize * 0.7],
            NSBaselineOffsetAttributeName : @(font.pointSize * 0.1),
          }] autorelease];
  [out appendAttributedString:[[[NSAttributedString alloc]
                                  initWithString:text
                                      attributes:@{
                                        NSForegroundColorAttributeName : [XeniaTheme textSecondary],
                                        NSFontAttributeName : font,
                                      }] autorelease]];
  return out;
}

UIButton* xe_new_notice_action(NSString* title) {
  UIButtonConfiguration* config = [UIButtonConfiguration tintedButtonConfiguration];
  config.title = title;
  config.baseForegroundColor = [XeniaTheme accent];
  config.baseBackgroundColor = [XeniaTheme accent];
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.contentInsets = NSDirectionalEdgeInsetsMake(7.0, 14.0, 7.0, 14.0);
  config.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
    return updated;
  };
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  return button;
}

// Two snapshots show the same game with the same content when every field the
// tile reads matches.
bool xe_snapshot_same_game(XeniaIOSLauncherGameSnapshot* a, XeniaIOSLauncherGameSnapshot* b) {
  return a.titleId == b.titleId && (a.title == b.title || [a.title isEqualToString:b.title]);
}

bool xe_snapshot_same_content(XeniaIOSLauncherGameSnapshot* a, XeniaIOSLauncherGameSnapshot* b) {
  auto same_string = [](NSString* x, NSString* y) {
    return x == y || (x.length == 0 && y.length == 0) || [x isEqualToString:y];
  };
  if (!xe_snapshot_same_game(a, b) || !same_string(a.contentTypeName, b.contentTypeName) ||
      !same_string(a.compatStatus, b.compatStatus) || a.hasCompatInfo != b.hasCompatInfo ||
      a.supportsCompatibility != b.supportsCompatibility ||
      a.supportsManageContent != b.supportsManageContent ||
      a.supportsDiscSelection != b.supportsDiscSelection ||
      a.supportsPatches != b.supportsPatches ||
      a.supportsZarConversion != b.supportsZarConversion ||
      a.supportsRemoteArt != b.supportsRemoteArt || a.discCount != b.discCount) {
    return false;
  }
  return a.iconData == b.iconData || (a.iconData.length == 0 && b.iconData.length == 0) ||
         [a.iconData isEqualToData:b.iconData];
}
}  // namespace

@implementation XeniaIOSLauncherOverlayView {
  UIView* _navSep;
  UILabel* _titleLabel;
  UIButton* _settingsButton;
  UIButton* _profileButton;
  UIButton* _openGameButton;
  UIButton* _convertLibraryButton;
  BOOL _dashboardInstalled;
  UIView* _jitWarningCard;
  UIImageView* _jitStatusIcon;
  UILabel* _jitTitleLabel;
  UILabel* _jitStatusLabel;
  UIButton* _jitOpenButton;
  UIView* _memoryWarningCard;
  UIImageView* _memoryStatusIcon;
  UILabel* _memoryStatusLabel;
  UIImageView* _jitReadyIcon;
  UILabel* _jitReadyLabel;
  UILabel* _libraryLabel;
  UIStackView* _topInfoStack;
  UIStackView* _noticeGroup;
  UIView* _noticeSeparator;
  UICollectionView* _gamesCollectionView;
  XeniaLauncherEmptyStateActionProxy* _emptyStateProxy;
  NSMutableSet<NSNumber*>* _pendingArtLoads;
  NSMutableSet<NSNumber*>* _pendingIconDecodes;
  BOOL _gamesLoaded;
  BOOL _prunedArtCache;
  NSInteger _focusedGameIndex;
  BOOL _actionsEnabled;
  BOOL _controllerNavigationEnabled;
  BOOL _libraryFocusActive;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  _focusedGameIndex = -1;
  _jitAcquired = NO;
  _memoryEntitlementEnabled = YES;
  _actionsEnabled = YES;
  _controllerNavigationEnabled = NO;
  _libraryFocusActive = NO;

  self.backgroundColor = [XeniaTheme bgPrimary];
  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  UILayoutGuide* safe = self.safeAreaLayoutGuide;
  CGFloat hPad = 16.0;

  // ── Nav bar: XeniOS (left) · gear + profile (right) ────────────────────

  _titleLabel = [[UILabel alloc] init];
  {
    UIFont* titleFont = xe_scaled_system_font(UIFontTextStyleTitle2, 22.0, UIFontWeightBold);
    NSDictionary* whiteAttrs = @{
      NSFontAttributeName : titleFont,
      NSForegroundColorAttributeName : [XeniaTheme textPrimary],
      NSKernAttributeName : @(0.8),
    };
    NSDictionary* accentAttrs = @{
      NSFontAttributeName : titleFont,
      NSForegroundColorAttributeName : [XeniaTheme accent],
      NSKernAttributeName : @(0.8),
    };
    NSMutableAttributedString* title =
        [[NSMutableAttributedString alloc] initWithString:@"Xeni" attributes:whiteAttrs];
    NSAttributedString* titleSuffix = [[NSAttributedString alloc] initWithString:@"OS"
                                                                      attributes:accentAttrs];
    [title appendAttributedString:titleSuffix];
    [titleSuffix release];
    _titleLabel.attributedText = title;
    [title release];
  }
  _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _titleLabel.adjustsFontForContentSizeCategory = YES;
  [self addSubview:_titleLabel];

  UIButtonConfiguration* settingsCfg = [UIButtonConfiguration plainButtonConfiguration];
  settingsCfg.image =
      [UIImage systemImageNamed:@"gearshape"
              withConfiguration:[[UIImageSymbolConfiguration
                                    configurationWithTextStyle:UIFontTextStyleTitle3
                                                         scale:UIImageSymbolScaleMedium]
                                    configurationByApplyingConfiguration:
                                        [UIImageSymbolConfiguration
                                            configurationWithWeight:UIImageSymbolWeightRegular]]];
  settingsCfg.baseForegroundColor = [XeniaTheme textMuted];
  settingsCfg.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
  _settingsButton = [[UIButton buttonWithConfiguration:settingsCfg primaryAction:nil] retain];
  _settingsButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_settingsButton addTarget:self
                      action:@selector(settingsTapped:)
            forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(
      _settingsButton, NSLocalizedString(@"Settings", @"launcher overlay view: user-visible text"),
      nil,
      NSLocalizedString(@"Opens XeniOS settings.", @"launcher overlay view: user-visible text"),
      UIAccessibilityTraitButton);
  [self addSubview:_settingsButton];

  UIButtonConfiguration* profileCfg = [UIButtonConfiguration plainButtonConfiguration];
  profileCfg.image =
      [UIImage systemImageNamed:@"person.circle"
              withConfiguration:[[UIImageSymbolConfiguration
                                    configurationWithTextStyle:UIFontTextStyleTitle3
                                                         scale:UIImageSymbolScaleMedium]
                                    configurationByApplyingConfiguration:
                                        [UIImageSymbolConfiguration
                                            configurationWithWeight:UIImageSymbolWeightRegular]]];
  profileCfg.baseForegroundColor = [XeniaTheme textMuted];
  profileCfg.contentInsets = NSDirectionalEdgeInsetsMake(8, 8, 8, 8);
  _profileButton = [[UIButton buttonWithConfiguration:profileCfg primaryAction:nil] retain];
  _profileButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_profileButton addTarget:self
                     action:@selector(profileTapped:)
           forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(
      _profileButton, NSLocalizedString(@"Profile", @"launcher overlay view: user-visible text"),
      nil,
      NSLocalizedString(@"Opens profile management.", @"launcher overlay view: user-visible text"),
      UIAccessibilityTraitButton);
  [self addSubview:_profileButton];

  // JIT ready indicator in nav bar: a checkmark and a short label.
  _jitReadyIcon = [[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"checkmark.circle.fill"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithTextStyle:UIFontTextStyleCaption1
                                                                       scale:
                                                                           UIImageSymbolScaleMedium]]];
  _jitReadyIcon.translatesAutoresizingMaskIntoConstraints = NO;
  _jitReadyIcon.tintColor = [XeniaTheme accent];
  _jitReadyIcon.contentMode = UIViewContentModeScaleAspectFit;
  _jitReadyIcon.isAccessibilityElement = NO;
  [self addSubview:_jitReadyIcon];

  _jitReadyLabel = [[UILabel alloc] init];
  _jitReadyLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _jitReadyLabel.text = NSLocalizedString(@"JIT Enabled", @"launcher overlay view: label text");
  _jitReadyLabel.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(_jitReadyLabel, UIFontTextStyleCaption1, 12.0, UIFontWeightSemibold);
  [self addSubview:_jitReadyLabel];

  // Thin separator below the nav bar.
  _navSep = [[UIView alloc] init];
  _navSep.translatesAutoresizingMaskIntoConstraints = NO;
  _navSep.backgroundColor = [XeniaTheme border];
  [self addSubview:_navSep];

  // ── JIT card shown only when JIT is not enabled: symbol, status and the
  // way to fix it (open StikDebug) ───────────────────────────────────────

  _jitStatusIcon = [[UIImageView alloc]
      initWithImage:XESettingsBadgeImage(@"bolt.slash.fill", [XeniaTheme statusError])];
  _jitTitleLabel = [[UILabel alloc] init];
  _jitTitleLabel.text = NSLocalizedString(@"JIT Is Off", @"launcher overlay view: JIT card title");
  _jitStatusLabel = [[UILabel alloc] init];
  _jitStatusLabel.text =
      NSLocalizedString(@"Waiting for JIT...", @"launcher overlay view: label text");
  _jitOpenButton = [xe_new_notice_action(NSLocalizedString(
      @"StikDebug", @"launcher overlay view: button that opens StikDebug")) retain];
  _jitOpenButton.hidden = YES;  // Until `openStikDebugHandler` is set.
  [_jitOpenButton addTarget:self
                     action:@selector(openStikDebugTapped:)
           forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(
      _jitOpenButton,
      NSLocalizedString(@"Open StikDebug", @"launcher overlay view: accessibility label"), nil,
      NSLocalizedString(@"Switches to StikDebug to enable JIT.",
                        @"launcher overlay view: accessibility hint"),
      UIAccessibilityTraitButton);
  _jitWarningCard = xe_new_notice_card(_jitStatusIcon, _jitTitleLabel, _jitStatusLabel,
                                       _jitOpenButton);

  // Shown when the installed signature lacks
  // com.apple.developer.kernel.increased-memory-limit.
  _memoryStatusIcon = [[UIImageView alloc]
      initWithImage:XESettingsBadgeImage(@"memorychip.fill", [XeniaTheme statusWarning])];
  UILabel* memoryTitleLabel = [[[UILabel alloc] init] autorelease];
  memoryTitleLabel.text =
      NSLocalizedString(@"Memory Limit Is Low", @"launcher overlay view: memory card title");
  _memoryStatusLabel = [[UILabel alloc] init];
  _memoryStatusLabel.text = xe_memory_entitlement_missing_status_message();
  _memoryWarningCard =
      xe_new_notice_card(_memoryStatusIcon, memoryTitleLabel, _memoryStatusLabel, nil);
  _memoryWarningCard.hidden = YES;
  XEApplyAccessibility(_memoryWarningCard, memoryTitleLabel.text, _memoryStatusLabel.text, nil,
                       UIAccessibilityTraitStaticText);

  // ── Library header: "Library" (left) + convert + "+" (right) ───────────

  UIView* libraryRow = [[UIView alloc] init];
  libraryRow.translatesAutoresizingMaskIntoConstraints = NO;

  _libraryLabel = [[UILabel alloc] init];
  _libraryLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _libraryLabel.text = NSLocalizedString(@"Library", @"launcher overlay view: label text");
  _libraryLabel.textColor = [XeniaTheme textPrimary];
  xe_apply_label_font(_libraryLabel, UIFontTextStyleTitle3, 20.0, UIFontWeightSemibold);
  _libraryLabel.accessibilityTraits = UIAccessibilityTraitHeader;
  [libraryRow addSubview:_libraryLabel];

  UIButtonConfiguration* convertCfg = [UIButtonConfiguration plainButtonConfiguration];
  convertCfg.image =
      [UIImage systemImageNamed:@"opticaldisc"
              withConfiguration:[[UIImageSymbolConfiguration
                                    configurationWithTextStyle:UIFontTextStyleSubheadline
                                                         scale:UIImageSymbolScaleMedium]
                                    configurationByApplyingConfiguration:
                                        [UIImageSymbolConfiguration
                                            configurationWithWeight:UIImageSymbolWeightSemibold]]];
  convertCfg.baseForegroundColor = [XeniaTheme textPrimary];
  convertCfg.contentInsets = NSDirectionalEdgeInsetsMake(6, 6, 6, 6);
  _convertLibraryButton = [[UIButton buttonWithConfiguration:convertCfg primaryAction:nil] retain];
  _convertLibraryButton.translatesAutoresizingMaskIntoConstraints = NO;
  _convertLibraryButton.hidden = YES;
  [_convertLibraryButton addTarget:self
                            action:@selector(convertLibraryTapped:)
                  forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(
      _convertLibraryButton,
      NSLocalizedString(@"Convert library to ZAR", @"launcher overlay view: user-visible text"),
      nil,
      NSLocalizedString(@"Converts non-ZAR games in the library.",
                        @"launcher overlay view: user-visible text"),
      UIAccessibilityTraitButton);

  UIButtonConfiguration* importCfg = [UIButtonConfiguration plainButtonConfiguration];
  importCfg.image =
      [UIImage systemImageNamed:@"plus"
              withConfiguration:[[UIImageSymbolConfiguration
                                    configurationWithTextStyle:UIFontTextStyleSubheadline
                                                         scale:UIImageSymbolScaleMedium]
                                    configurationByApplyingConfiguration:
                                        [UIImageSymbolConfiguration
                                            configurationWithWeight:UIImageSymbolWeightSemibold]]];
  importCfg.baseForegroundColor = [XeniaTheme textPrimary];
  importCfg.contentInsets = NSDirectionalEdgeInsetsMake(6, 6, 6, 6);
  _openGameButton = [[UIButton buttonWithConfiguration:importCfg primaryAction:nil] retain];
  _openGameButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_openGameButton addTarget:self
                      action:@selector(importTapped:)
            forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(
      _openGameButton,
      NSLocalizedString(@"Import game", @"launcher overlay view: user-visible text"), nil,
      NSLocalizedString(@"Opens the document picker to import a game.",
                        @"launcher overlay view: user-visible text"),
      UIAccessibilityTraitButton);

  UIStackView* libraryActionsStack = [[UIStackView alloc] initWithArrangedSubviews:@[
    xe_ios_new_system_link_online_button(), _convertLibraryButton, _openGameButton
  ]];
  libraryActionsStack.axis = UILayoutConstraintAxisHorizontal;
  libraryActionsStack.alignment = UIStackViewAlignmentCenter;
  libraryActionsStack.spacing = 4.0;
  libraryActionsStack.translatesAutoresizingMaskIntoConstraints = NO;
  [libraryRow addSubview:libraryActionsStack];

  // ── Collapsible stack: warnings → library header ───────────────────────

  NSArray<UIView*>* noticeSeparators = nil;
  _noticeGroup = xe_new_notice_group(@[ _jitWarningCard, _memoryWarningCard ], &noticeSeparators);
  _noticeSeparator = [noticeSeparators.firstObject retain];

  _topInfoStack = [[UIStackView alloc]
      initWithArrangedSubviews:@[ _noticeGroup, libraryRow ]];
  _topInfoStack.axis = UILayoutConstraintAxisVertical;
  _topInfoStack.spacing = 12;
  _topInfoStack.translatesAutoresizingMaskIntoConstraints = NO;

  // ── Games grid ─────────────────────────────────────────────────────────

  UICollectionViewFlowLayout* gridLayout = [[UICollectionViewFlowLayout alloc] init];
  gridLayout.minimumInteritemSpacing = 16;
  gridLayout.minimumLineSpacing = 20;
  gridLayout.sectionInset = UIEdgeInsetsZero;
  _gamesCollectionView = [[UICollectionView alloc] initWithFrame:CGRectZero
                                            collectionViewLayout:gridLayout];
  _gamesCollectionView.translatesAutoresizingMaskIntoConstraints = NO;
  _gamesCollectionView.dataSource = self;
  _gamesCollectionView.prefetchDataSource = self;
  _gamesCollectionView.delegate = self;
  _gamesCollectionView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  _gamesCollectionView.backgroundColor = [UIColor clearColor];
  _gamesCollectionView.alwaysBounceVertical = YES;
  [_gamesCollectionView registerClass:[XeniaGameTileCell class]
           forCellWithReuseIdentifier:@"ImportedGameCell"];
  [self addSubview:_gamesCollectionView];
  // The notices and the Library row scroll with the games, in the grid's top inset, so they
  // don't hold a third of the screen.
  [_gamesCollectionView addSubview:_topInfoStack];

  // Empty state: a content-unavailable view behind the grid, shown once the
  // first snapshot arrives with no games.
  _emptyStateProxy = [[XeniaLauncherEmptyStateActionProxy alloc] init];
  _emptyStateProxy.owner = self;
  _pendingArtLoads = [[NSMutableSet alloc] init];
  _pendingIconDecodes = [[NSMutableSet alloc] init];

  // ── Layout ─────────────────────────────────────────────────────────────

  NSLayoutConstraint* libraryRowHug = [libraryRow.heightAnchor constraintEqualToConstant:0];
  libraryRowHug.priority = UILayoutPriorityDefaultLow;
  [NSLayoutConstraint activateConstraints:@[
    // Nav bar.
    [_titleLabel.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:hPad],
    [_titleLabel.topAnchor constraintEqualToAnchor:safe.topAnchor constant:6],
    [_profileButton.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
    [_profileButton.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    [_settingsButton.trailingAnchor constraintEqualToAnchor:_profileButton.leadingAnchor
                                                   constant:-2],
    [_settingsButton.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    // JIT ready checkmark + label sit right after the title.
    [_jitReadyIcon.leadingAnchor constraintEqualToAnchor:_titleLabel.trailingAnchor constant:10],
    [_jitReadyIcon.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    [_jitReadyLabel.leadingAnchor constraintEqualToAnchor:_jitReadyIcon.trailingAnchor constant:4],
    [_jitReadyLabel.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
    [_jitReadyLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_settingsButton.leadingAnchor
                                                            constant:-8],

    // Minimum touch targets for the icon buttons.
    [_settingsButton.widthAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_settingsButton.heightAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_profileButton.widthAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_profileButton.heightAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_convertLibraryButton.widthAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_convertLibraryButton.heightAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_openGameButton.widthAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_openGameButton.heightAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],

    // Nav separator.
    [_navSep.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor constant:6],
    [_navSep.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
    [_navSep.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    [_navSep.heightAnchor constraintEqualToConstant:0.5],
    [_navSep.topAnchor constraintGreaterThanOrEqualToAnchor:_settingsButton.bottomAnchor],

    // Header stack (notices + library row), above the grid's content.
    [_topInfoStack.bottomAnchor
        constraintEqualToAnchor:_gamesCollectionView.contentLayoutGuide.topAnchor
                       constant:-kLibraryHeaderGap],
    [_topInfoStack.leadingAnchor
        constraintEqualToAnchor:_gamesCollectionView.frameLayoutGuide.leadingAnchor],
    [_topInfoStack.trailingAnchor
        constraintEqualToAnchor:_gamesCollectionView.frameLayoutGuide.trailingAnchor],

    // Library row internals.
    [_libraryLabel.leadingAnchor constraintEqualToAnchor:libraryRow.leadingAnchor],
    [_libraryLabel.centerYAnchor constraintEqualToAnchor:libraryRow.centerYAnchor],
    [_libraryLabel.trailingAnchor
        constraintLessThanOrEqualToAnchor:libraryActionsStack.leadingAnchor
                                 constant:-8],
    [libraryActionsStack.trailingAnchor constraintEqualToAnchor:libraryRow.trailingAnchor],
    [libraryActionsStack.centerYAnchor constraintEqualToAnchor:libraryRow.centerYAnchor],
    [libraryRow.heightAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    // Tall enough for its content at any text size, and no taller: the grid below has no
    // height of its own, so an open-ended row would take all of its space.
    [_libraryLabel.topAnchor constraintGreaterThanOrEqualToAnchor:libraryRow.topAnchor],
    [_libraryLabel.bottomAnchor constraintLessThanOrEqualToAnchor:libraryRow.bottomAnchor],
    [libraryActionsStack.topAnchor constraintGreaterThanOrEqualToAnchor:libraryRow.topAnchor],
    [libraryActionsStack.bottomAnchor constraintLessThanOrEqualToAnchor:libraryRow.bottomAnchor],
    libraryRowHug,

    // Games grid.
    [_gamesCollectionView.topAnchor constraintEqualToAnchor:_navSep.bottomAnchor],
    [_gamesCollectionView.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:hPad],
    [_gamesCollectionView.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor
                                                        constant:-hPad],
    [_gamesCollectionView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
  ]];

  [gridLayout release];
  [libraryActionsStack release];
  [libraryRow release];
  [self setJITAcquired:NO];
  [self setMemoryEntitlementEnabled:YES];
  [self updateNoticeGroup];
  [self applyActionsEnabledState];
  [self registerForTraitChanges:@[
    [UITraitPreferredContentSizeCategory class], [UITraitHorizontalSizeClass class],
    [UITraitVerticalSizeClass class]
  ]
                     withAction:@selector(layoutTraitsChanged)];
  return self;
}

- (void)updateNoticeGroup {
  _noticeSeparator.hidden = _jitWarningCard.hidden || _memoryWarningCard.hidden;
  _noticeGroup.hidden = _jitWarningCard.hidden && _memoryWarningCard.hidden;
  [self setNeedsLayout];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  [_topInfoStack layoutIfNeeded];
  const CGFloat top = kLibraryHeaderTop + _topInfoStack.bounds.size.height + kLibraryHeaderGap;
  const CGFloat bottom = self.safeAreaInsets.bottom + 16.0;
  UIEdgeInsets insets = _gamesCollectionView.contentInset;
  if (insets.top != top || insets.bottom != bottom) {
    // Keep the header in view when the list was scrolled to its top.
    const BOOL at_top = _gamesCollectionView.contentOffset.y <= -insets.top + 1.0;
    insets.top = top;
    insets.bottom = bottom;
    _gamesCollectionView.contentInset = insets;
    _gamesCollectionView.verticalScrollIndicatorInsets = UIEdgeInsetsMake(top, 0, bottom, 0);
    if (at_top) {
      _gamesCollectionView.contentOffset = CGPointMake(0.0, -top);
    }
  }
}

- (void)dealloc {
  _emptyStateProxy.owner = nil;
  [_emptyStateProxy release];
  [_pendingArtLoads release];
  [_pendingIconDecodes release];
  [_openStikDebugHandler release];
  [_games release];
  [_gamesCollectionView release];
  [_settingsHandler release];
  [_profileHandler release];
  [_importHandler release];
  [_bulkZarConversionHandler release];
  [_gameLaunchedHandler release];
  [_copyLaunchURLHandler release];
  [_gameSettingsHandler release];
  [_touchLayoutHandler release];
  [_compatibilityHandler release];
  [_manageContentHandler release];
  [_discSelectionHandler release];
  [_patchesHandler release];
  [_zarConversionHandler release];
  [_titleLabel release];
  [_settingsButton release];
  [_profileButton release];
  [_openGameButton release];
  [_convertLibraryButton release];
  [_dashboardHandler release];
  [_dashboardSystemSoftwareHandler release];
  [_jitWarningCard release];
  [_jitStatusIcon release];
  [_jitTitleLabel release];
  [_jitStatusLabel release];
  [_jitOpenButton release];
  [_memoryWarningCard release];
  [_memoryStatusIcon release];
  [_memoryStatusLabel release];
  [_jitReadyIcon release];
  [_jitReadyLabel release];
  [_libraryLabel release];
  [_topInfoStack release];
  [_noticeGroup release];
  [_noticeSeparator release];
  [_navSep release];
  [super dealloc];
}

#pragma mark - Button actions (forward to handlers)

- (void)settingsTapped:(UIButton*)__unused sender {
  if (_settingsHandler) {
    _settingsHandler();
  }
}

- (void)profileTapped:(UIButton*)__unused sender {
  if (_profileHandler) {
    _profileHandler();
  }
}

- (void)importTapped:(UIButton*)__unused sender {
  if (_importHandler) {
    _importHandler();
  }
}

- (void)dashboardTapped:(UIButton*)__unused sender {
  if (_dashboardHandler) {
    _dashboardHandler();
  }
}

- (void)setDashboardInstalled:(BOOL)installed {
  if (_dashboardInstalled == installed) {
    return;
  }
  _dashboardInstalled = installed;
  if (_games.count > 0) {
    [_gamesCollectionView reconfigureItemsAtIndexPaths:@[ [NSIndexPath indexPathForItem:0
                                                                              inSection:0] ]];
  } else {
    _gamesCollectionView.backgroundView = nil;
    [self updateEmptyState];
  }
}

#pragma mark - Library items

// The Dashboard tile leads the grid while the library has games, so the
// games' items come after it.
- (NSInteger)gameItemOffset {
  return _games.count > 0 ? 1 : 0;
}

- (NSInteger)libraryItemCount {
  return static_cast<NSInteger>(_games.count) + [self gameItemOffset];
}

- (BOOL)isDashboardItem:(NSInteger)item {
  return _games.count > 0 && item == 0;
}

// The game an item shows: nil for the Dashboard tile and past the end.
- (XeniaIOSLauncherGameSnapshot*)gameAtItem:(NSInteger)item {
  const NSInteger index = item - [self gameItemOffset];
  if (index < 0 || index >= static_cast<NSInteger>(_games.count)) {
    return nil;
  }
  return _games[static_cast<NSUInteger>(index)];
}

- (void)configureDashboardCell:(XeniaGameTileCell*)cell {
  NSString* title =
      NSLocalizedString(@"Xbox 360 Dashboard", @"launcher overlay view: user-visible text");
  cell.representedTitleId = 0;
  cell.titleLabel.text = title;
  cell.iconView.image = xe_dashboard_tile_art(cell.traitCollection);
  cell.contentTypePill.text = @"";
  cell.contentTypePill.hidden = YES;
  // Same form as a game's status: the dot says whether it's ready.
  cell.compatPill.attributedText = xe_status_text(
      _dashboardInstalled
          ? NSLocalizedString(@"Ready", @"launcher overlay view: Dashboard tile status, once the "
                                        @"system software is installed")
          : NSLocalizedString(@"Not Set Up", @"launcher overlay view: Dashboard tile status, "
                                             @"before the system software is installed"),
      _dashboardInstalled ? [XeniaTheme accent] : [XeniaTheme statusWarning]);
  cell.contentTypePill.backgroundColor = [UIColor clearColor];
  cell.contentTypePill.layer.borderColor = [UIColor clearColor].CGColor;
  cell.compatPill.hidden = NO;
  cell.controllerFocused =
      _controllerNavigationEnabled && _libraryFocusActive && _focusedGameIndex == 0;
  cell.accessibilityLabel = title;
  cell.accessibilityValue =
      _dashboardInstalled
          ? nil
          : NSLocalizedString(@"Not set up", @"launcher overlay view: accessibility value");
  cell.accessibilityHint = _dashboardInstalled
                               ? NSLocalizedString(@"Starts the Xbox 360 Dashboard.",
                                                   @"launcher overlay view: user-visible text")
                               : NSLocalizedString(@"Sets up the Xbox 360 Dashboard.",
                                                   @"launcher overlay view: accessibility hint");
  cell.accessibilityTraits = UIAccessibilityTraitButton;
}

- (void)openStikDebugTapped:(UIButton*)__unused sender {
  if (_openStikDebugHandler) {
    _openStikDebugHandler();
  }
}

- (void)setOpenStikDebugHandler:(void (^)(void))handler {
  if (_openStikDebugHandler == handler) {
    return;
  }
  [_openStikDebugHandler release];
  _openStikDebugHandler = [handler copy];
  _jitOpenButton.hidden = _openStikDebugHandler == nil;
}

- (void)convertLibraryTapped:(UIButton*)__unused sender {
  if (_bulkZarConversionHandler) {
    _bulkZarConversionHandler();
  }
}

#pragma mark - Focus visuals

- (void)applyActionsEnabledState {
  _settingsButton.enabled = _actionsEnabled;
  _profileButton.enabled = _actionsEnabled;
  _openGameButton.enabled = _actionsEnabled;
  _convertLibraryButton.enabled = _actionsEnabled && !_convertLibraryButton.hidden;

  CGFloat actionAlpha = _actionsEnabled ? 1.0 : 0.5;
  _settingsButton.alpha = actionAlpha;
  _profileButton.alpha = actionAlpha;
  _openGameButton.alpha = actionAlpha;
  _convertLibraryButton.alpha = _convertLibraryButton.hidden ? 0.0 : actionAlpha;
}

- (void)setActionsEnabled:(BOOL)actionsEnabled {
  if (_actionsEnabled == actionsEnabled) {
    return;
  }
  _actionsEnabled = actionsEnabled;
  [self applyActionsEnabledState];
}

- (BOOL)isActionsEnabled {
  return _actionsEnabled;
}

- (void)setButton:(UIButton*)button controllerFocused:(BOOL)focused {
  if (!button) {
    return;
  }
  button.layer.cornerRadius = XeniaRadiusMd;
  button.layer.borderWidth = focused ? 1.5 : 0.0;
  button.layer.borderColor = focused ? [XeniaTheme accent].CGColor : [UIColor clearColor].CGColor;
  button.layer.shadowColor = [XeniaTheme accent].CGColor;
  button.layer.shadowOpacity = focused ? 0.35f : 0.0f;
  button.layer.shadowRadius = focused ? 6.0f : 0.0f;
  button.layer.shadowOffset = CGSizeZero;
}

- (void)clearButtonFocusChrome:(UIButton*)button {
  if (!button) {
    return;
  }
  button.layer.cornerRadius = XeniaRadiusMd;
  button.layer.borderWidth = 0.0f;
  button.layer.borderColor = [UIColor clearColor].CGColor;
  button.layer.shadowOpacity = 0.0f;
  button.layer.shadowRadius = 0.0f;
  button.layer.shadowOffset = CGSizeZero;
}

- (void)setControllerNavigationEnabled:(BOOL)enabled
                       settingsFocused:(BOOL)settingsFocused
                        profileFocused:(BOOL)profileFocused
                         importFocused:(BOOL)importFocused
                    libraryFocusActive:(BOOL)libraryFocusActive {
  BOOL previousLibraryFocusActive = _libraryFocusActive;
  _controllerNavigationEnabled = enabled;
  _libraryFocusActive = enabled && libraryFocusActive;

  if (!_controllerNavigationEnabled) {
    _libraryFocusActive = NO;
    [self clearButtonFocusChrome:_settingsButton];
    [self clearButtonFocusChrome:_profileButton];
    [self clearButtonFocusChrome:_openGameButton];
    [self clearButtonFocusChrome:_convertLibraryButton];
    if (_focusedGameIndex >= 0 && _focusedGameIndex < [self libraryItemCount]) {
      NSIndexPath* focusedPath = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
      [_gamesCollectionView reconfigureItemsAtIndexPaths:@[ focusedPath ]];
    }
    return;
  }

  [self setButton:_settingsButton controllerFocused:settingsFocused];
  [self setButton:_profileButton controllerFocused:profileFocused];
  [self setButton:_openGameButton controllerFocused:importFocused];
  [self clearButtonFocusChrome:_convertLibraryButton];

  if (_libraryFocusActive && _focusedGameIndex < 0 && _games.count > 0) {
    [self setFocusedGameIndex:0 scroll:NO];
  }

  if (previousLibraryFocusActive != _libraryFocusActive && _focusedGameIndex >= 0 &&
      _focusedGameIndex < [self libraryItemCount]) {
    NSIndexPath* focusedPath = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
    [_gamesCollectionView reconfigureItemsAtIndexPaths:@[ focusedPath ]];
  }
}

#pragma mark - Data ingestion

- (void)updateEmptyState {
  if (!_gamesLoaded || _games.count > 0) {
    _gamesCollectionView.backgroundView = nil;
    return;
  }
  if (_gamesCollectionView.backgroundView) {
    return;
  }
  UIContentUnavailableConfiguration* empty = [UIContentUnavailableConfiguration emptyConfiguration];
  empty.image = [UIImage systemImageNamed:@"gamecontroller"];
  empty.text = NSLocalizedString(@"No Games", @"launcher overlay view: empty library title");
  empty.secondaryText = NSLocalizedString(@"Add games with Files or Finder.",
                                          @"launcher overlay view: empty library message");
  UIButtonConfiguration* importCfg = [UIButtonConfiguration filledButtonConfiguration];
  importCfg.title = NSLocalizedString(@"Import Games", @"launcher overlay view: button title");
  importCfg.baseBackgroundColor = [XeniaTheme accent];
  empty.button = importCfg;
  XeniaLauncherEmptyStateActionProxy* proxy = _emptyStateProxy;
  empty.buttonProperties.primaryAction =
      [UIAction actionWithHandler:^(__unused UIAction* action) {
        [proxy importGames];
      }];
  UIButtonConfiguration* dashboardCfg = [UIButtonConfiguration plainButtonConfiguration];
  dashboardCfg.title = _dashboardInstalled
                           ? NSLocalizedString(@"Start the Xbox 360 Dashboard",
                                               @"launcher overlay view: button title")
                           : NSLocalizedString(@"Set Up the Xbox 360 Dashboard",
                                               @"launcher overlay view: button title");
  dashboardCfg.baseForegroundColor = [XeniaTheme accent];
  empty.secondaryButton = dashboardCfg;
  empty.secondaryButtonProperties.primaryAction =
      [UIAction actionWithHandler:^(__unused UIAction* action) {
        [proxy openDashboard];
      }];
  UIContentUnavailableView* emptyView =
      [[UIContentUnavailableView alloc] initWithConfiguration:empty];
  _gamesCollectionView.backgroundView = emptyView;
  [emptyView release];
}

- (void)setGames:(NSArray<XeniaIOSLauncherGameSnapshot*>*)games {
  NSArray<XeniaIOSLauncherGameSnapshot*>* oldGames = [_games retain];
  const NSInteger oldOffset = [self gameItemOffset];
  _games = [games copy];
  const NSInteger newOffset = [self gameItemOffset];
  _gamesLoaded = YES;
  BOOL hasConvertibleGames = NO;
  for (XeniaIOSLauncherGameSnapshot* game in _games) {
    if (game.supportsZarConversion) {
      hasConvertibleGames = YES;
      break;
    }
  }
  _convertLibraryButton.hidden = !hasConvertibleGames;
  [self updateEmptyState];

  // Pair the games that are in both lists (identity: title id and title) so
  // that only inserted, removed and changed tiles are touched.
  NSOrderedCollectionDifference<XeniaIOSLauncherGameSnapshot*>* diff = [_games
      differenceFromArray:oldGames ?: @[]
              withOptions:0
     usingEquivalenceTest:^BOOL(XeniaIOSLauncherGameSnapshot* a,
                                XeniaIOSLauncherGameSnapshot* b) {
       return xe_snapshot_same_game(a, b);
     }];
  NSMutableIndexSet* removedOld = [NSMutableIndexSet indexSet];
  NSMutableIndexSet* insertedNew = [NSMutableIndexSet indexSet];
  NSMutableArray<NSIndexPath*>* deletes = [NSMutableArray array];
  NSMutableArray<NSIndexPath*>* inserts = [NSMutableArray array];
  for (NSOrderedCollectionChange* change in diff) {
    const BOOL removed = change.changeType == NSCollectionChangeRemove;
    NSIndexPath* path = [NSIndexPath
        indexPathForItem:static_cast<NSInteger>(change.index) + (removed ? oldOffset : newOffset)
               inSection:0];
    if (removed) {
      [removedOld addIndex:change.index];
      [deletes addObject:path];
    } else {
      [insertedNew addIndex:change.index];
      [inserts addObject:path];
    }
  }
  NSMutableArray<NSIndexPath*>* changed = [NSMutableArray array];
  NSInteger newFocus = -1;
  NSUInteger oldIndex = 0;
  NSUInteger newIndex = 0;
  while (oldIndex < oldGames.count && newIndex < _games.count) {
    if ([removedOld containsIndex:oldIndex]) {
      ++oldIndex;
      continue;
    }
    if ([insertedNew containsIndex:newIndex]) {
      ++newIndex;
      continue;
    }
    XeniaIOSLauncherGameSnapshot* before = oldGames[oldIndex];
    XeniaIOSLauncherGameSnapshot* after = _games[newIndex];
    if (xe_snapshot_same_content(before, after)) {
      // Carry the decoded fallback icon over so it is not decoded again.
      if (!after.decodedIcon) {
        after.decodedIcon = before.decodedIcon;
      }
    } else {
      [changed addObject:[NSIndexPath indexPathForItem:static_cast<NSInteger>(newIndex) + newOffset
                                             inSection:0]];
    }
    if (static_cast<NSInteger>(oldIndex) + oldOffset == _focusedGameIndex) {
      newFocus = static_cast<NSInteger>(newIndex) + newOffset;
    }
    ++oldIndex;
    ++newIndex;
  }

  if (_games.count == 0) {
    _focusedGameIndex = -1;
  } else if (newFocus >= 0) {
    _focusedGameIndex = newFocus;
  } else if (_focusedGameIndex < 0 || _focusedGameIndex >= [self libraryItemCount]) {
    _focusedGameIndex = 0;
  }
  [self applyActionsEnabledState];

  if (!_prunedArtCache && _games.count > 0) {
    _prunedArtCache = YES;
    NSMutableSet<NSNumber*>* keep = [NSMutableSet set];
    for (XeniaIOSLauncherGameSnapshot* game in _games) {
      [keep addObject:@(game.titleId)];
    }
    xe_prune_game_art_cache(keep);
  }

  if (deletes.count == 0 && inserts.count == 0 && changed.count == 0) {
    // Nothing the grid shows has changed: leave the cells alone.
    [oldGames release];
    return;
  }
  if (!oldGames || !self.window || self.hidden || oldOffset != newOffset) {
    // Not on screen (or first load), or the Dashboard tile came or went: a
    // plain reload is cheaper than a diff.
    [_gamesCollectionView reloadData];
    [oldGames release];
    return;
  }
  UICollectionView* collectionView = _gamesCollectionView;
  [collectionView performBatchUpdates:^{
    [collectionView deleteItemsAtIndexPaths:deletes];
    [collectionView insertItemsAtIndexPaths:inserts];
  }
      completion:^(__unused BOOL finished) {
        if (changed.count > 0) {
          [collectionView reconfigureItemsAtIndexPaths:changed];
        }
      }];
  [oldGames release];
}

- (void)setJITAcquired:(BOOL)acquired {
  BOOL previous_acquired = _jitAcquired;
  _jitAcquired = acquired;
  _jitReadyIcon.hidden = !acquired;
  _jitReadyLabel.hidden = !acquired;
  if (acquired) {
    _jitStatusLabel.text = NSLocalizedString(@"JIT Enabled", @"launcher overlay view: label text");
    _jitReadyLabel.text = NSLocalizedString(@"JIT Enabled", @"launcher overlay view: label text");
  }

  BOOL previousHidden = _jitWarningCard.hidden;
  _jitWarningCard.hidden = acquired;
  if (previousHidden != _jitWarningCard.hidden) {
    [self updateNoticeGroup];
  }
  _jitReadyLabel.accessibilityLabel =
      acquired
          ? NSLocalizedString(@"JIT is enabled", @"launcher overlay view: accessibility label")
          : NSLocalizedString(@"JIT is not enabled", @"launcher overlay view: accessibility label");
  if (previous_acquired != acquired) {
    UIAccessibilityPostNotification(
        UIAccessibilityAnnouncementNotification,
        acquired ? NSLocalizedString(@"JIT is enabled", @"launcher overlay view: user-visible text")
                 : _jitStatusLabel.text);
  }
  [self applyActionsEnabledState];
}

- (void)setJITStatusText:(NSString*)text {
  _jitStatusLabel.text = text ?: @"";
}

- (void)setMemoryEntitlementEnabled:(BOOL)enabled {
  BOOL previous_enabled = _memoryEntitlementEnabled;
  _memoryEntitlementEnabled = enabled;

  BOOL previous_hidden = _memoryWarningCard.hidden;
  _memoryWarningCard.hidden = enabled;
  if (previous_hidden != _memoryWarningCard.hidden) {
    [self updateNoticeGroup];
  }

  if (!enabled && !_memoryStatusLabel.text.length) {
    _memoryStatusLabel.text = xe_memory_entitlement_missing_status_message();
  }
  _memoryWarningCard.accessibilityValue =
      enabled ? NSLocalizedString(@"Increased memory entitlement is enabled",
                                  @"launcher overlay view: accessibility value")
              : _memoryStatusLabel.text;
  if (previous_enabled != enabled) {
    UIAccessibilityPostNotification(
        UIAccessibilityAnnouncementNotification,
        enabled ? NSLocalizedString(@"Increased memory entitlement is enabled",
                                    @"launcher overlay view: user-visible text")
                : _memoryStatusLabel.text);
  }
}

- (void)setMemoryEntitlementStatusText:(NSString*)text {
  _memoryStatusLabel.text = text ?: @"";
  _memoryWarningCard.accessibilityValue = _memoryStatusLabel.text;
}

- (void)setFocusedGameIndex:(NSInteger)index scroll:(BOOL)scroll {
  const NSInteger itemCount = [self libraryItemCount];
  if (itemCount == 0) {
    index = -1;
  } else {
    if (index < 0) {
      index = 0;
    }
    NSInteger maxIndex = itemCount - 1;
    if (index > maxIndex) {
      index = maxIndex;
    }
  }
  NSInteger previous = _focusedGameIndex;
  _focusedGameIndex = index;

  NSMutableArray<NSIndexPath*>* reloadPaths = [NSMutableArray array];
  if (previous >= 0 && previous < itemCount) {
    [reloadPaths addObject:[NSIndexPath indexPathForItem:previous inSection:0]];
  }
  if (_focusedGameIndex >= 0 && _focusedGameIndex < itemCount && _focusedGameIndex != previous) {
    [reloadPaths addObject:[NSIndexPath indexPathForItem:_focusedGameIndex inSection:0]];
  }
  if (reloadPaths.count > 0) {
    [_gamesCollectionView reconfigureItemsAtIndexPaths:reloadPaths];
  }

  if (scroll && _focusedGameIndex >= 0 && _focusedGameIndex < itemCount) {
    NSIndexPath* path = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
    [_gamesCollectionView scrollToItemAtIndexPath:path
                                 atScrollPosition:UICollectionViewScrollPositionCenteredVertically
                                         animated:YES];
  }
}

- (NSInteger)focusedGameIndex {
  return _focusedGameIndex;
}

- (void)reloadGames {
  [self reconfigureVisibleGames];
}

// Re-applies cell content to the tiles on screen without recreating them.
- (void)reconfigureVisibleGames {
  NSArray<NSIndexPath*>* visible = _gamesCollectionView.indexPathsForVisibleItems;
  if (visible.count > 0) {
    [_gamesCollectionView reconfigureItemsAtIndexPaths:visible];
  }
}

- (void)layoutTraitsChanged {
  [_gamesCollectionView.collectionViewLayout invalidateLayout];
  [self reconfigureVisibleGames];
}

- (UICollectionView*)gamesCollectionView {
  return _gamesCollectionView;
}

- (BOOL)isOverlayVisible {
  return !self.hidden;
}

- (NSInteger)pageStep {
  NSArray<NSIndexPath*>* visible = _gamesCollectionView.indexPathsForVisibleItems;
  if (visible.count > 0) {
    return visible.count;
  }
  return 6;
}

- (void)refreshChromeForCurrentTraits {
  _jitWarningCard.layer.borderColor = [XeniaTheme border].CGColor;
  _memoryWarningCard.layer.borderColor = [XeniaTheme border].CGColor;
  _jitReadyIcon.tintColor = [XeniaTheme accent];
  [self reconfigureVisibleGames];
}

- (void)setOverlayVisible:(BOOL)visible animated:(BOOL)animated {
  if (visible == !self.hidden) {
    return;
  }
  if (!animated) {
    self.hidden = !visible;
    self.alpha = visible ? 1.0 : 0.0;
    return;
  }
  if (visible) {
    self.hidden = NO;
    self.alpha = 0.0;
    [UIView animateWithDuration:0.3
                     animations:^{
                       self.alpha = 1.0;
                     }];
  } else {
    [UIView animateWithDuration:0.3
        animations:^{
          self.alpha = 0.0;
        }
        completion:^(__unused BOOL finished) {
          self.hidden = YES;
          self.alpha = 1.0;
        }];
  }
}

#pragma mark - Grid sizing helpers

- (BOOL)usesCompactLandscapeLayoutForContentSize:(CGSize)contentSize {
  // Landscape with little vertical room: a phone in landscape, or a short
  // Split View / Stage Manager window on iPad. Regular-height windows keep
  // the roomy layout even when they are wider than tall.
  if (contentSize.width <= contentSize.height) {
    return NO;
  }
  return contentSize.height < 430.0f ||
         self.traitCollection.verticalSizeClass == UIUserInterfaceSizeClassCompact;
}

- (NSInteger)columnCountForContentSize:(CGSize)contentSize {
  CGFloat contentWidth = contentSize.width;
  BOOL compactLandscape = [self usesCompactLandscapeLayoutForContentSize:contentSize];
  CGFloat gridSpacing = compactLandscape ? 12.0f : 16.0f;
  CGFloat minimumTileWidth = compactLandscape ? 170.0f : 190.0f;
  NSInteger minimumColumns = compactLandscape ? 4 : 2;
  // Wide iPad windows (13-inch, Stage Manager) have room for more columns.
  BOOL wideRegular = !compactLandscape && contentWidth >= 1100.0f &&
                     self.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular;
  NSInteger maximumColumns = compactLandscape ? 5 : (wideRegular ? 8 : 6);

  NSInteger columns = static_cast<NSInteger>(
      floor((contentWidth + gridSpacing) / (minimumTileWidth + gridSpacing)));
  columns = MAX(columns, minimumColumns);
  columns = MIN(columns, maximumColumns);
  return columns;
}

- (NSInteger)columnCount {
  return [self columnCountForContentSize:_gamesCollectionView.bounds.size];
}

- (CGFloat)interitemSpacingForCollectionView:(UICollectionView*)collectionView {
  return [self usesCompactLandscapeLayoutForContentSize:collectionView.bounds.size] ? 12.0f : 16.0f;
}

- (CGFloat)lineSpacingForCollectionView:(UICollectionView*)collectionView {
  return [self usesCompactLandscapeLayoutForContentSize:collectionView.bounds.size] ? 14.0f : 20.0f;
}

// Footer height under the cover: sized from the preferred fonts so the title
// and the metadata row never clip at larger Dynamic Type sizes. The fixed
// heights remain the floor so the default size keeps its grid.
- (CGFloat)titleHeightForCollectionView:(UICollectionView*)collectionView {
  CGFloat floorHeight =
      [self usesCompactLandscapeLayoutForContentSize:collectionView.bounds.size] ? 56.0f : 60.0f;
  UITraitCollection* traits = collectionView.traitCollection;
  BOOL accessibilitySize = xe_uses_accessibility_text(traits);
  CGFloat titleLine =
      xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold).lineHeight;
  CGFloat metaLine =
      xe_scaled_system_font(UIFontTextStyleCaption1, 12.0, UIFontWeightSemibold).lineHeight;
  CGFloat titleLines = accessibilitySize ? 2.0 : 1.0;
  CGFloat metaLines = accessibilitySize ? 2.0 : 1.0;
  // 8 top inset + title + 2 gap + metadata (+ 4 between stacked pills) + 8 bottom.
  CGFloat needed = 8.0 + ceil(titleLine * titleLines) + 2.0 + ceil(metaLine * metaLines) +
                   (accessibilitySize ? 4.0 : 0.0) + 8.0;
  return MAX(floorHeight, needed);
}

- (CGFloat)tileWidthForCollectionView:(UICollectionView*)collectionView
                              columns:(NSInteger)columns
                     interitemSpacing:(CGFloat)spacing {
  CGFloat contentWidth = collectionView.bounds.size.width;
  CGFloat totalSpacing = spacing * MAX(columns - 1, 0);
  static constexpr CGFloat kTileShadowMargin = 16.0f;
  CGFloat availableWidth = MAX(contentWidth - totalSpacing - kTileShadowMargin, 0.0f);
  CGFloat tileWidth = availableWidth / MAX(columns, 1);
  CGFloat screenScale =
      collectionView.window.screen ? collectionView.window.screen.scale : UIScreen.mainScreen.scale;
  tileWidth = floor(tileWidth * screenScale) / screenScale;
  return MAX(tileWidth, 100.0f);
}

#pragma mark - UICollectionViewDataSource

- (NSInteger)collectionView:(UICollectionView* __unused)collectionView
     numberOfItemsInSection:(NSInteger)__unused section {
  return [self libraryItemCount];
}

- (UICollectionViewCell*)collectionView:(UICollectionView*)collectionView
                 cellForItemAtIndexPath:(NSIndexPath*)indexPath {
  XeniaGameTileCell* cell =
      [collectionView dequeueReusableCellWithReuseIdentifier:@"ImportedGameCell"
                                                forIndexPath:indexPath];
  if ([self isDashboardItem:indexPath.item]) {
    [self configureDashboardCell:cell];
    xe_ios_system_link_show_online_label(cell.compatPill.superview, 0);
    return cell;
  }
  XeniaIOSLauncherGameSnapshot* game = [self gameAtItem:indexPath.item];
  if (!game) {
    cell.titleLabel.text = @"";
    cell.iconView.image = nil;
    cell.contentTypePill.text = @"";
    cell.contentTypePill.hidden = YES;
    cell.compatPill.text = @"";
    cell.compatPill.hidden = YES;
    cell.controllerFocused = NO;
    cell.accessibilityLabel = nil;
    cell.accessibilityValue = nil;
    xe_ios_system_link_show_online_label(cell.compatPill.superview, 0);
    return cell;
  }

  cell.titleLabel.text = game.title;
  cell.contentTypePill.text = game.contentTypeName ?: @"";
  cell.contentTypePill.hidden = game.contentTypeName.length == 0;
  cell.contentTypePill.textColor = [XeniaTheme textMuted];
  cell.contentTypePill.backgroundColor = [UIColor clearColor];
  cell.contentTypePill.layer.borderColor = [UIColor clearColor].CGColor;
  if (game.hasCompatInfo) {
    cell.compatPill.attributedText =
        xe_status_text(xe_compat_status_label(game.compatStatus),
                       xe_compat_status_color(game.compatStatus));
    cell.compatPill.backgroundColor = [UIColor clearColor];
    cell.compatPill.layer.borderWidth = 0.0;
    cell.compatPill.layer.borderColor = [UIColor clearColor].CGColor;
    cell.compatPill.hidden = NO;
  } else {
    cell.compatPill.text = @"";
    cell.compatPill.hidden = YES;
  }
  cell.controllerFocused =
      _controllerNavigationEnabled && _libraryFocusActive && _focusedGameIndex == indexPath.item;
  NSMutableArray<NSString*>* accessibility_values = [NSMutableArray array];
  if (game.contentTypeName.length) {
    [accessibility_values addObject:game.contentTypeName];
  }
  if (game.hasCompatInfo) {
    [accessibility_values addObject:xe_compat_status_label(game.compatStatus)];
  }
  if (NSString* online = xe_ios_system_link_online_description(game.titleId)) {
    [accessibility_values addObject:online];
  }
  cell.accessibilityLabel =
      game.title.length
          ? game.title
          : NSLocalizedString(@"Untitled game", @"launcher overlay view: accessibility label");
  cell.accessibilityValue = [accessibility_values componentsJoinedByString:@", "];
  cell.accessibilityHint = NSLocalizedString(@"Opens the game. Long press for more actions.",
                                             @"launcher overlay view: accessibility hint");
  cell.accessibilityTraits = UIAccessibilityTraitButton;

  cell.representedTitleId = game.titleId;
  xe_ios_system_link_show_online_label(cell.compatPill.superview, game.titleId);
  UIImage* cover = game.supportsRemoteArt ? xe_cached_game_art_thumbnail(game.titleId) : nil;
  if (cover) {
    cell.iconView.image = cover;
  } else {
    cell.iconView.image = [self placeholderImageForGame:game];
    if (game.supportsRemoteArt) {
      [self loadCoverForTitleId:game.titleId collectionView:collectionView];
    }
  }
  return cell;
}

#pragma mark - Cover art (off the main thread)

// Longest side, in pixels, to decode covers at: the tile height at screen
// scale, rounded up to a bucket so rotations reuse the same cache entry.
- (CGFloat)coverPixelSizeForCollectionView:(UICollectionView*)collectionView {
  CGSize size = [self collectionView:collectionView
                              layout:collectionView.collectionViewLayout
              sizeForItemAtIndexPath:[NSIndexPath indexPathForItem:0 inSection:0]];
  CGFloat scale =
      collectionView.window.screen ? collectionView.window.screen.scale : UIScreen.mainScreen.scale;
  CGFloat pixels = size.height * scale;
  return MAX(ceil(pixels / 128.0) * 128.0, 384.0);
}

// Placeholder shown until the cover arrives: the game's own icon (decoded once
// off the main thread), then the app icon.
- (UIImage*)placeholderImageForGame:(XeniaIOSLauncherGameSnapshot*)game {
  if (game.decodedIcon) {
    return game.decodedIcon;
  }
  if (game.iconData.length) {
    [self decodeIconForGame:game];
  }
  // One shared fallback per appearance.
  static UIImage* fallback[2] = {nil, nil};
  UIImage*& cached =
      fallback[self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark ? 1 : 0];
  if (!cached) {
    cached = [xe_placeholder_cover_art(nil, self.traitCollection) retain];
  }
  return cached;
}

- (void)decodeIconForGame:(XeniaIOSLauncherGameSnapshot*)game {
  NSNumber* key = @(game.titleId);
  if (game.titleId && [_pendingIconDecodes containsObject:key]) {
    return;
  }
  [_pendingIconDecodes addObject:key];
  NSData* data = [game.iconData retain];
  uint32_t titleId = game.titleId;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    UIImage* decoded = [[UIImage imageWithData:data] imageByPreparingForDisplay];
    [decoded retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      [_pendingIconDecodes removeObject:key];
      if (decoded) {
        UIImage* placeholder = xe_placeholder_cover_art(decoded, self.traitCollection);
        for (XeniaIOSLauncherGameSnapshot* candidate in _games) {
          if (candidate.titleId == titleId && candidate.iconData.length &&
              !candidate.decodedIcon) {
            candidate.decodedIcon = placeholder;
          }
        }
        [self reconfigureVisibleGamesWithTitleId:titleId];
      }
      [decoded release];
      [data release];
    });
  });
}

- (void)reconfigureVisibleGamesWithTitleId:(uint32_t)titleId {
  NSMutableArray<NSIndexPath*>* paths = [NSMutableArray array];
  for (NSIndexPath* path in _gamesCollectionView.indexPathsForVisibleItems) {
    if ([self gameAtItem:path.item].titleId == titleId) {
      [paths addObject:path];
    }
  }
  if (paths.count > 0) {
    [_gamesCollectionView reconfigureItemsAtIndexPaths:paths];
  }
}

// Decodes the cover for `titleId` on a background queue, downloading it first
// when it is not on disk. The block retains the overlay until it finishes.
- (void)loadCoverForTitleId:(uint32_t)titleId collectionView:(UICollectionView*)collectionView {
  NSNumber* key = @(titleId);
  if (!titleId || [_pendingArtLoads containsObject:key] ||
      xe_cached_game_art_thumbnail(titleId)) {
    return;
  }
  [_pendingArtLoads addObject:key];
  CGFloat pixels = [self coverPixelSizeForCollectionView:collectionView];
  xe_load_game_art_thumbnail(titleId, pixels, ^(UIImage* image) {
    if (image) {
      [_pendingArtLoads removeObject:key];
      [self reconfigureVisibleGamesWithTitleId:titleId];
      return;
    }
    // Nothing on disk yet: download, then decode what arrived.
    xe_fetch_game_art(titleId, ^(UIImage* fetched) {
      if (!fetched) {
        [_pendingArtLoads removeObject:key];
        return;
      }
      xe_load_game_art_thumbnail(titleId, pixels, ^(UIImage* decoded) {
        [_pendingArtLoads removeObject:key];
        if (decoded) {
          [self reconfigureVisibleGamesWithTitleId:titleId];
        }
      });
    });
  });
}

#pragma mark - UICollectionViewDataSourcePrefetching

- (void)collectionView:(UICollectionView*)collectionView
    prefetchItemsAtIndexPaths:(NSArray<NSIndexPath*>*)indexPaths {
  for (NSIndexPath* path in indexPaths) {
    XeniaIOSLauncherGameSnapshot* game = [self gameAtItem:path.item];
    if (game.supportsRemoteArt) {
      [self loadCoverForTitleId:game.titleId collectionView:collectionView];
    }
  }
}

#pragma mark - UICollectionViewDelegate

- (void)collectionView:(UICollectionView*)collectionView
    didSelectItemAtIndexPath:(NSIndexPath*)indexPath {
  [collectionView deselectItemAtIndexPath:indexPath animated:YES];
  if (indexPath.item < 0 || indexPath.item >= [self libraryItemCount]) {
    return;
  }
  [self setFocusedGameIndex:indexPath.item scroll:NO];
  if ([self isDashboardItem:indexPath.item]) {
    [self dashboardTapped:nil];
    return;
  }
  if (_gameLaunchedHandler) {
    _gameLaunchedHandler(static_cast<NSUInteger>(indexPath.item - [self gameItemOffset]));
  }
}

- (UITargetedPreview*)contextMenuPreviewForConfiguration:(UIContextMenuConfiguration*)configuration
                                          collectionView:(UICollectionView*)collectionView {
  id identifier = configuration.identifier;
  NSIndexPath* indexPath =
      [identifier isKindOfClass:[NSIndexPath class]] ? (NSIndexPath*)identifier : nil;
  if (!indexPath) {
    return nil;
  }
  XeniaGameTileCell* cell = (XeniaGameTileCell*)[collectionView cellForItemAtIndexPath:indexPath];
  if (![cell isKindOfClass:[XeniaGameTileCell class]]) {
    return nil;
  }
  UIPreviewParameters* parameters = [[[UIPreviewParameters alloc] init] autorelease];
  parameters.backgroundColor = [UIColor clearColor];
  parameters.visiblePath = [UIBezierPath bezierPathWithRoundedRect:cell.cardView.bounds
                                                      cornerRadius:XeniaRadiusLg];
  return [[[UITargetedPreview alloc] initWithView:cell.cardView parameters:parameters] autorelease];
}

- (void)resetContextMenuCellTransforms {
  for (UICollectionViewCell* visibleCell in _gamesCollectionView.visibleCells) {
    if (![visibleCell isKindOfClass:[XeniaGameTileCell class]]) {
      continue;
    }
    XeniaGameTileCell* gameCell = (XeniaGameTileCell*)visibleCell;
    gameCell.transform = CGAffineTransformIdentity;
    gameCell.contentView.transform = CGAffineTransformIdentity;
    gameCell.cardView.transform = CGAffineTransformIdentity;
    gameCell.layer.zPosition = gameCell.controllerFocused ? 1.0f : 0.0f;
    [gameCell setNeedsLayout];
  }
  [_gamesCollectionView.collectionViewLayout invalidateLayout];
}

- (UIContextMenuConfiguration*)dashboardContextMenuForIndexPath:(NSIndexPath*)indexPath {
  return [UIContextMenuConfiguration
      configurationWithIdentifier:indexPath
                  previewProvider:nil
                   actionProvider:^UIMenu*(NSArray<UIMenuElement*>* __unused suggestedActions) {
                     // The block retains the overlay for as long as the menu lives.
                     XeniaIOSLauncherOverlayView* owner = self;
                     UIAction* open = [UIAction
                         actionWithTitle:owner->_dashboardInstalled
                                             ? NSLocalizedString(
                                                   @"Start Dashboard",
                                                   @"launcher overlay view: menu action")
                                             : NSLocalizedString(
                                                   @"Set Up Dashboard",
                                                   @"launcher overlay view: menu action")
                                   image:[UIImage systemImageNamed:owner->_dashboardInstalled
                                                                       ? @"play.fill"
                                                                       : @"arrow.down.circle"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   [owner dashboardTapped:nil];
                                 }];
                     UIAction* system_software = [UIAction
                         actionWithTitle:NSLocalizedString(@"System Software",
                                                           @"launcher overlay view: menu action")
                                   image:[UIImage systemImageNamed:@"gearshape"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (owner->_dashboardSystemSoftwareHandler) {
                                     owner->_dashboardSystemSoftwareHandler();
                                   }
                                 }];
                     return [UIMenu menuWithChildren:@[ open, system_software ]];
                   }];
}

- (UIContextMenuConfiguration*)collectionView:(UICollectionView*)collectionView
    contextMenuConfigurationForItemAtIndexPath:(NSIndexPath*)indexPath
                                         point:(CGPoint)point {
  (void)collectionView;
  (void)point;
  if ([self isDashboardItem:indexPath.item]) {
    return [self dashboardContextMenuForIndexPath:indexPath];
  }
  if (![self gameAtItem:indexPath.item]) {
    return nil;
  }

  NSUInteger gameIndex = static_cast<NSUInteger>(indexPath.item - [self gameItemOffset]);
  return [UIContextMenuConfiguration
      configurationWithIdentifier:indexPath
                  previewProvider:nil
                   actionProvider:^UIMenu*(NSArray<UIMenuElement*>* __unused suggestedActions) {
                     // The block retains the overlay for as long as the menu lives.
                     XeniaIOSLauncherOverlayView* unsafeSelf = self;
                     if (gameIndex >= unsafeSelf->_games.count) {
                       return nil;
                     }
                     XeniaIOSLauncherGameSnapshot* game = unsafeSelf->_games[gameIndex];

                     UIAction* playAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Play", @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"play.fill"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_gameLaunchedHandler) {
                                     unsafeSelf->_gameLaunchedHandler(gameIndex);
                                   }
                                 }];
                     UIAction* gameSettingsAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Game Settings",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"slider.horizontal.3"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_gameSettingsHandler) {
                                     unsafeSelf->_gameSettingsHandler(gameIndex);
                                   }
                                 }];
                     UIAction* touchLayoutAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Touch Layout",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"hand.tap"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_touchLayoutHandler) {
                                     unsafeSelf->_touchLayoutHandler(gameIndex);
                                   }
                                 }];
                     UIAction* compatAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Compatibility",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"checkmark.shield"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_compatibilityHandler) {
                                     unsafeSelf->_compatibilityHandler(gameIndex);
                                   }
                                 }];
                     UIAction* contentAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Manage Content",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"square.stack.3d.up"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_manageContentHandler) {
                                     unsafeSelf->_manageContentHandler(gameIndex);
                                   }
                                 }];
                     UIAction* discAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Launch Disc",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"opticaldisc"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_discSelectionHandler) {
                                     unsafeSelf->_discSelectionHandler(gameIndex);
                                   }
                                 }];
                     UIAction* patchesAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Patches",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"puzzlepiece.extension"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_patchesHandler) {
                                     unsafeSelf->_patchesHandler(gameIndex);
                                   }
                                 }];
                     UIAction* zarConversionAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Convert to ZAR",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"opticaldisc"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_zarConversionHandler) {
                                     unsafeSelf->_zarConversionHandler(gameIndex);
                                   }
                                 }];
                     UIAction* copyURLAction = [UIAction
                         actionWithTitle:NSLocalizedString(@"Copy Launch URL",
                                                           @"launcher overlay view: title")
                                   image:[UIImage systemImageNamed:@"link"]
                              identifier:nil
                                 handler:^(__unused UIAction* action) {
                                   if (unsafeSelf->_copyLaunchURLHandler) {
                                     unsafeSelf->_copyLaunchURLHandler(gameIndex);
                                   }
                                 }];
                     if (!game.supportsCompatibility) {
                       compatAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsManageContent) {
                       contentAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsDiscSelection) {
                       discAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsPatches) {
                       patchesAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.supportsZarConversion) {
                       zarConversionAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     if (!game.titleId) {
                       gameSettingsAction.attributes = UIMenuElementAttributesDisabled;
                       touchLayoutAction.attributes = UIMenuElementAttributesDisabled;
                       copyURLAction.attributes = UIMenuElementAttributesDisabled;
                     }
                     return [UIMenu menuWithTitle:@""
                                         children:@[
                                           playAction, gameSettingsAction, touchLayoutAction,
                                           discAction, compatAction, contentAction, patchesAction,
                                           zarConversionAction, copyURLAction
                                         ]];
                   }];
}

- (UITargetedPreview*)collectionView:(UICollectionView*)collectionView
    previewForHighlightingContextMenuWithConfiguration:(UIContextMenuConfiguration*)configuration {
  return [self contextMenuPreviewForConfiguration:configuration collectionView:collectionView];
}

- (UITargetedPreview*)collectionView:(UICollectionView*)collectionView
    previewForDismissingContextMenuWithConfiguration:(UIContextMenuConfiguration*)configuration {
  return [self contextMenuPreviewForConfiguration:configuration collectionView:collectionView];
}

- (void)collectionView:(UICollectionView*)collectionView
    willEndContextMenuInteractionWithConfiguration:(UIContextMenuConfiguration*)configuration
                                          animator:(id<UIContextMenuInteractionAnimating>)animator {
  (void)collectionView;
  (void)configuration;
  [animator addCompletion:^{
    [self resetContextMenuCellTransforms];
  }];
}

#pragma mark - UICollectionViewDelegateFlowLayout

- (CGSize)collectionView:(UICollectionView*)collectionView
                    layout:(UICollectionViewLayout* __unused)collectionViewLayout
    sizeForItemAtIndexPath:(NSIndexPath* __unused)indexPath {
  NSInteger columns = [self columnCountForContentSize:collectionView.bounds.size];
  CGFloat spacing = [self interitemSpacingForCollectionView:collectionView];
  CGFloat tileWidth = [self tileWidthForCollectionView:collectionView
                                               columns:columns
                                      interitemSpacing:spacing];
  CGFloat imageHeight = ceil(tileWidth * kCoverAspect);
  return CGSizeMake(tileWidth, imageHeight + [self titleHeightForCollectionView:collectionView]);
}

- (CGFloat)collectionView:(UICollectionView*)collectionView
                                      layout:(UICollectionViewLayout* __unused)collectionViewLayout
    minimumInteritemSpacingForSectionAtIndex:(NSInteger)__unused section {
  return [self interitemSpacingForCollectionView:collectionView];
}

- (CGFloat)collectionView:(UICollectionView*)collectionView
                                 layout:(UICollectionViewLayout* __unused)collectionViewLayout
    minimumLineSpacingForSectionAtIndex:(NSInteger)__unused section {
  return [self lineSpacingForCollectionView:collectionView];
}

- (UIEdgeInsets)collectionView:(UICollectionView*)collectionView
                        layout:(UICollectionViewLayout* __unused)collectionViewLayout
        insetForSectionAtIndex:(NSInteger)__unused section {
  NSInteger columns = [self columnCountForContentSize:collectionView.bounds.size];
  CGFloat spacing = [self interitemSpacingForCollectionView:collectionView];
  CGFloat tileWidth = [self tileWidthForCollectionView:collectionView
                                               columns:columns
                                      interitemSpacing:spacing];
  CGFloat consumedWidth = tileWidth * columns + spacing * MAX(columns - 1, 0);
  CGFloat remainder = MAX(collectionView.bounds.size.width - consumedWidth, 0.0f);
  CGFloat screenScale =
      collectionView.window.screen ? collectionView.window.screen.scale : UIScreen.mainScreen.scale;
  CGFloat leftInset = floor((remainder * 0.5f) * screenScale) / screenScale;
  CGFloat rightInset = MAX(remainder - leftInset, 0.0f);
  return UIEdgeInsetsMake(0.0f, leftInset, 0.0f, rightInset);
}

@end
