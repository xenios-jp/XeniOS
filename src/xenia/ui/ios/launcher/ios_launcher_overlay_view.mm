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
  [_lastPlayed release];
  [super dealloc];
}

@end

namespace {
NSString* const kLastPlayedDefaultsKey = @"XeniaLastPlayedDates";
NSString* const kLibrarySortDefaultsKey = @"XeniaLibrarySort";

NSString* xe_last_played_key(uint32_t title_id) {
  return [NSString stringWithFormat:@"%08X", title_id];
}
}  // namespace

void XeniaIOSRecordGamePlayed(uint32_t title_id) {
  if (!title_id) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSMutableDictionary* dates =
      [[[defaults dictionaryForKey:kLastPlayedDefaultsKey] mutableCopy] autorelease]
          ?: [NSMutableDictionary dictionary];
  dates[xe_last_played_key(title_id)] = [NSDate date];
  [defaults setObject:dates forKey:kLastPlayedDefaultsKey];
}

NSDate* XeniaIOSLastPlayedDate(uint32_t title_id) {
  if (!title_id) {
    return nil;
  }
  id date = [[NSUserDefaults standardUserDefaults]
      dictionaryForKey:kLastPlayedDefaultsKey][xe_last_played_key(title_id)];
  return [date isKindOfClass:[NSDate class]] ? date : nil;
}

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
                                           UICollectionViewDelegateFlowLayout,
                                           UITextFieldDelegate>
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
// Side margin of the library: the title, the notices and the tiles' edges all line up on it.
constexpr CGFloat kLibraryMargin = 16.0;
// Height over width of a tile's art: a box cover (300 x 219).
constexpr CGFloat kCoverAspect = 300.0 / 219.0;
// Space above the notices and between the Library row and the first games.
constexpr CGFloat kLibraryHeaderTop = 12.0;
constexpr CGFloat kLibraryHeaderGap = 12.0;

// The Xbox 360 Dashboard tile's art, at a cover's size: Xbox green with a soft
// highlight behind a grid of tiles. Drawn once.
UIImage* xe_dashboard_tile_art(UITraitCollection* __unused traits) {
  static UIImage* cached = nil;
  if (cached) {
    return cached;
  }
  const CGSize size = CGSizeMake(219.0, 300.0);
  UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
  format.scale = 2.0;
  format.opaque = YES;
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:size format:format] autorelease];
  UIImage* symbol =
      [[UIImage systemImageNamed:@"square.grid.2x2.fill"
               withConfiguration:[UIImageSymbolConfiguration
                                     configurationWithPointSize:52.0
                                                         weight:UIImageSymbolWeightMedium]]
          imageWithTintColor:UIColor.whiteColor
               renderingMode:UIImageRenderingModeAlwaysOriginal];
  cached = [[renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    CGContextRef cg = context.CGContext;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    // 160°: from the top left to the bottom right, light to deep green.
    CGGradientRef green = CGGradientCreateWithColors(
        space,
        (CFArrayRef) @[
          (id)[UIColor colorWithRed:0x3f / 255.0 green:0xbf / 255.0 blue:0x5a / 255.0 alpha:1.0]
              .CGColor,
          (id)[UIColor colorWithRed:0x0f / 255.0 green:0x5f / 255.0 blue:0x2a / 255.0 alpha:1.0]
              .CGColor
        ],
        nullptr);
    CGContextDrawLinearGradient(cg, green, CGPointMake(size.width * 0.33, 0.0),
                                CGPointMake(size.width * 0.67, size.height),
                                kCGGradientDrawsBeforeStartLocation |
                                    kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(green);
    // The highlight, an ellipse above the middle.
    CGGradientRef glow = CGGradientCreateWithColors(
        space,
        (CFArrayRef) @[
          (id)[UIColor colorWithWhite:1.0 alpha:0.18].CGColor,
          (id)[UIColor colorWithWhite:1.0 alpha:0.0].CGColor
        ],
        nullptr);
    CGContextSaveGState(cg);
    CGContextTranslateCTM(cg, size.width * 0.5, size.height * 0.35);
    CGContextScaleCTM(cg, 1.0, (size.height * 0.6) / (size.width * 0.8));
    CGContextDrawRadialGradient(cg, glow, CGPointZero, 0.0, CGPointZero, size.width * 0.8 * 0.7,
                                0);
    CGContextRestoreGState(cg);
    CGGradientRelease(glow);
    CGColorSpaceRelease(space);
    const CGSize symbol_size = symbol.size;
    [symbol drawInRect:CGRectMake((size.width - symbol_size.width) * 0.5,
                                  (size.height - symbol_size.height) * 0.5, symbol_size.width,
                                  symbol_size.height)];
  }] retain];
  return cached;
}

// A cover for a game with no art and no icon: its name, set large on the surface
// colours, so the grid never shows a row of identical boxes.
UIImage* xe_title_cover_art(NSString* title, UITraitCollection* traits) {
  const CGSize size = CGSizeMake(219.0, 300.0);
  UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
  format.scale = 2.0;
  format.opaque = YES;
  UIGraphicsImageRenderer* renderer =
      [[[UIGraphicsImageRenderer alloc] initWithSize:size format:format] autorelease];
  UIColor* top = [[XeniaTheme bgSurface2] resolvedColorWithTraitCollection:traits];
  UIColor* bottom = [[XeniaTheme bgSurface] resolvedColorWithTraitCollection:traits];
  UIColor* ink = [[XeniaTheme textPrimary] resolvedColorWithTraitCollection:traits];
  NSMutableParagraphStyle* paragraph = [[[NSMutableParagraphStyle alloc] init] autorelease];
  paragraph.lineHeightMultiple = 0.92;
  paragraph.lineBreakMode = NSLineBreakByWordWrapping;
  NSDictionary* attributes = @{
    NSFontAttributeName : [UIFont systemFontOfSize:30.0 weight:UIFontWeightHeavy],
    NSForegroundColorAttributeName : [ink colorWithAlphaComponent:0.88],
    NSParagraphStyleAttributeName : paragraph,
    NSKernAttributeName : @(-0.6),
  };
  return [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGGradientRef gradient = CGGradientCreateWithColors(
        space, (CFArrayRef) @[ (id)top.CGColor, (id)bottom.CGColor ], nullptr);
    CGContextDrawLinearGradient(context.CGContext, gradient, CGPointZero,
                                CGPointMake(0.0, size.height), 0);
    CGGradientRelease(gradient);
    CGColorSpaceRelease(space);
    const CGRect text_rect = CGRectMake(18.0, 22.0, size.width - 36.0, size.height - 44.0);
    [title ?: @"" drawWithRect:text_rect
                       options:NSStringDrawingUsesLineFragmentOrigin |
                               NSStringDrawingTruncatesLastVisibleLine
                    attributes:attributes
                       context:nil];
  }];
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

  // |icon| is a tinted symbol, sized like a Settings badge.
  icon.translatesAutoresizingMaskIntoConstraints = NO;
  icon.contentMode = UIViewContentModeCenter;
  icon.isAccessibilityElement = NO;

  // One slim line: the title, then the detail beside it in the secondary colour.
  title.textColor = [XeniaTheme textPrimary];
  xe_apply_label_font(title, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
  title.numberOfLines = 1;
  title.accessibilityTraits = UIAccessibilityTraitHeader;
  [title setContentCompressionResistancePriority:UILayoutPriorityRequired
                                         forAxis:UILayoutConstraintAxisHorizontal];
  [title setContentHuggingPriority:UILayoutPriorityRequired
                           forAxis:UILayoutConstraintAxisHorizontal];
  detail.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(detail, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  detail.numberOfLines = 1;
  detail.lineBreakMode = NSLineBreakByTruncatingTail;
  [detail setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                          forAxis:UILayoutConstraintAxisHorizontal];

  UIStackView* text = [[[UIStackView alloc] initWithArrangedSubviews:@[ title, detail ]] autorelease];
  text.axis = UILayoutConstraintAxisHorizontal;
  text.alignment = UIStackViewAlignmentFirstBaseline;
  text.spacing = 6.0;

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
  row.spacing = 10.0;
  [card addSubview:row];
  [NSLayoutConstraint activateConstraints:@[
    [icon.widthAnchor constraintEqualToConstant:22.0],
    [icon.heightAnchor constraintEqualToConstant:22.0],
    [row.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:14.0],
    [row.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-7.0],
    [row.topAnchor constraintEqualToAnchor:card.topAnchor constant:7.0],
    [row.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-7.0],
    [card.heightAnchor constraintGreaterThanOrEqualToConstant:44.0],
  ]];
  return card;
}

// At accessibility text sizes the action moves under the detail, so the text keeps the full
// width instead of wrapping one word per line beside the button.
void xe_set_notice_card_stacked(UIView* card, bool stacked) {
  UIStackView* row = (UIStackView*)card.subviews.firstObject;
  if (![row isKindOfClass:[UIStackView class]] || row.arrangedSubviews.count < 2) {
    return;
  }
  UIStackView* text = (UIStackView*)row.arrangedSubviews[1];
  UIView* action = nil;
  for (UIView* view in [row.arrangedSubviews arrayByAddingObjectsFromArray:text.arrangedSubviews]) {
    if ([view isKindOfClass:[UIButton class]]) {
      action = view;
    }
  }
  row.alignment = stacked ? UIStackViewAlignmentTop : UIStackViewAlignmentCenter;
  text.axis = stacked ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
  text.alignment = stacked ? UIStackViewAlignmentLeading : UIStackViewAlignmentFirstBaseline;
  for (UIView* view in text.arrangedSubviews) {
    if ([view isKindOfClass:[UILabel class]]) {
      ((UILabel*)view).numberOfLines = stacked ? 0 : 1;
    }
  }
  if (!action || (action.superview == text) == stacked) {
    return;
  }
  [action retain];
  [action removeFromSuperview];
  if (stacked) {
    [text addArrangedSubview:action];
    [text setCustomSpacing:8.0 afterView:text.arrangedSubviews[text.arrangedSubviews.count - 2]];
  } else {
    [row addArrangedSubview:action];
  }
  [action release];
}

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
        [hairline.leadingAnchor constraintEqualToAnchor:line.leadingAnchor constant:57.0],
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
  group.backgroundColor = [[XeniaTheme textPrimary] colorWithAlphaComponent:0.08];
  group.layer.cornerRadius = 22.0;
  group.layer.cornerCurve = kCACornerCurveContinuous;
  group.clipsToBounds = YES;
  if (separators_out) {
    *separators_out = separators;
  }
  return group;
}

// "● Playable": the status's colour in the dot only, the words in the secondary colour.
NSAttributedString* xe_status_text(NSString* text, UIColor* color) {
  UIFont* font = xe_scaled_system_font(UIFontTextStyleCaption1, 12.0, UIFontWeightRegular);
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
  // A grey capsule with the action in the accent, so the row's one action reads as one.
  UIButtonConfiguration* config = [UIButtonConfiguration filledButtonConfiguration];
  config.title = title;
  config.baseForegroundColor = [XeniaTheme accent];
  config.baseBackgroundColor = [[XeniaTheme textPrimary] colorWithAlphaComponent:0.14];
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  config.contentInsets = NSDirectionalEdgeInsetsMake(5.0, 12.0, 5.0, 12.0);
  config.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleFootnote, 14.0, UIFontWeightSemibold);
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
  // The header: the large "Library" title, then Online, the settings capsule and,
  // with the side layout, search and sort. It floats over the art and the grid.
  UIView* _headerView;
  UIVisualEffectView* _headerBackdrop;
  UIStackView* _headerActions;
  UILabel* _libraryLabel;
  UIButton* _settingsButton;
  UIButton* _profileButton;
  UIButton* _openGameButton;
  BOOL _dashboardInstalled;
  UIView* _jitWarningCard;
  UIImageView* _jitStatusIcon;
  UILabel* _jitTitleLabel;
  UILabel* _jitStatusLabel;
  UIButton* _jitOpenButton;
  UIView* _memoryWarningCard;
  UIImageView* _memoryStatusIcon;
  UILabel* _memoryStatusLabel;
  // Above the grid: the notices, then search and sort (search and sort move into
  // the header with the side layout).
  UIStackView* _topInfoStack;
  UIStackView* _noticeGroup;
  UIView* _noticeSeparator;
  UIStackView* _searchRow;
  UISearchTextField* _searchField;
  UIButton* _sortButton;
  NSLayoutConstraint* _searchWidth;
  // Continue Playing: the last game played, as full-bleed art with Play.
  UIView* _heroView;
  UIImageView* _heroArt;
  CAGradientLayer* _heroScrim;
  CAGradientLayer* _heroTopScrim;
  UIStackView* _heroText;
  UILabel* _heroCaption;
  UILabel* _heroTitle;
  UILabel* _heroDetail;
  UIButton* _heroPlayButton;
  UIButton* _heroMoreButton;
  NSLayoutConstraint* _heroTextLeading;
  NSLayoutConstraint* _heroTextBottom;
  XeniaIOSLauncherGameSnapshot* _heroGame;
  uint32_t _heroArtTitleId;
  // Art on the left and the grid on the right, for windows wider than tall.
  BOOL _sideLayout;
  // The games the grid shows, in order: all of them, sorted, or the search's matches.
  NSArray<XeniaIOSLauncherGameSnapshot*>* _shownGames;
  NSString* _query;
  NSInteger _sortMode;
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

- (UIButton*)newHeaderIconButton:(NSString*)symbol action:(SEL)action {
  UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
  config.image = [UIImage
      systemImageNamed:symbol
      withConfiguration:[UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleBody
                                                                         scale:
                                                                             UIImageSymbolScaleLarge]];
  config.baseForegroundColor = [XeniaTheme textPrimary];
  config.contentInsets = NSDirectionalEdgeInsetsZero;
  UIButton* button = [[UIButton buttonWithConfiguration:config primaryAction:nil] retain];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  if (action) {
    [button addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
  }
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:40.0],
    [button.heightAnchor constraintEqualToConstant:40.0],
  ]];
  return button;
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
  _shownGames = [@[] retain];
  _query = [@"" copy];
  _sortMode = [[NSUserDefaults standardUserDefaults] integerForKey:kLibrarySortDefaultsKey];

  self.backgroundColor = [XeniaTheme bgPrimary];
  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  const CGFloat hPad = kLibraryMargin;

  // ── Continue Playing ───────────────────────────────────────────────────
  // Laid out by hand in -layoutSubviews: across the top of the page, scrolling
  // with the grid, or down the left side with the side layout.

  _heroView = [[UIView alloc] init];
  _heroView.clipsToBounds = YES;
  _heroView.hidden = YES;
  _heroView.backgroundColor = [XeniaTheme bgPrimary];
  _heroArt = [[UIImageView alloc] init];
  _heroArt.contentMode = UIViewContentModeScaleAspectFill;
  _heroArt.clipsToBounds = YES;
  _heroArt.isAccessibilityElement = NO;
  [_heroView addSubview:_heroArt];
  // A dim band under the header, and the fade into the page behind the text.
  _heroTopScrim = [[CAGradientLayer layer] retain];
  _heroTopScrim.colors =
      @[ (id)[UIColor colorWithWhite:0.0 alpha:0.4].CGColor, (id)[UIColor clearColor].CGColor ];
  [_heroView.layer addSublayer:_heroTopScrim];
  _heroScrim = [[CAGradientLayer layer] retain];
  [_heroView.layer addSublayer:_heroScrim];

  _heroCaption = [[UILabel alloc] init];
  _heroCaption.text =
      NSLocalizedString(@"Continue Playing", @"launcher overlay view: label above the last game");
  _heroCaption.textColor = [UIColor colorWithWhite:1.0 alpha:0.75];
  xe_apply_label_font(_heroCaption, UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  _heroTitle = [[UILabel alloc] init];
  _heroTitle.textColor = UIColor.whiteColor;
  _heroTitle.numberOfLines = 2;
  _heroTitle.adjustsFontSizeToFitWidth = YES;
  _heroTitle.minimumScaleFactor = 0.7;
  _heroTitle.accessibilityTraits = UIAccessibilityTraitHeader;
  _heroDetail = [[UILabel alloc] init];
  _heroDetail.textColor = [UIColor colorWithWhite:1.0 alpha:0.7];
  _heroDetail.numberOfLines = 0;
  xe_apply_label_font(_heroDetail, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);

  // White on the art in either appearance: the one primary button on the page.
  UIButtonConfiguration* playCfg = [UIButtonConfiguration filledButtonConfiguration];
  playCfg.title = NSLocalizedString(@"Play", @"launcher overlay view: Continue Playing button");
  playCfg.image = [UIImage
      systemImageNamed:@"play.fill"
      withConfiguration:[UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleBody
                                                                         scale:
                                                                             UIImageSymbolScaleSmall]];
  playCfg.imagePadding = 6.0;
  playCfg.baseBackgroundColor = UIColor.whiteColor;
  playCfg.baseForegroundColor = UIColor.blackColor;
  playCfg.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  playCfg.contentInsets = NSDirectionalEdgeInsetsMake(0.0, 26.0, 0.0, 26.0);
  playCfg.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleHeadline, 16.0, UIFontWeightSemibold);
    return updated;
  };
  _heroPlayButton = [[UIButton buttonWithConfiguration:playCfg primaryAction:nil] retain];
  _heroPlayButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_heroPlayButton addTarget:self
                      action:@selector(heroPlayTapped:)
            forControlEvents:UIControlEventTouchUpInside];

  UIButtonConfiguration* moreCfg = [UIButtonConfiguration glassButtonConfiguration];
  moreCfg.image = [UIImage systemImageNamed:@"ellipsis"];
  moreCfg.baseForegroundColor = UIColor.whiteColor;
  moreCfg.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  _heroMoreButton = [[UIButton buttonWithConfiguration:moreCfg primaryAction:nil] retain];
  _heroMoreButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_heroMoreButton addTarget:self
                      action:@selector(heroMoreTapped:)
            forControlEvents:UIControlEventTouchUpInside];
  _heroMoreButton.accessibilityLabel =
      NSLocalizedString(@"More", @"launcher overlay view: Continue Playing actions button");

  UIStackView* heroButtons = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ _heroPlayButton, _heroMoreButton ]] autorelease];
  heroButtons.spacing = 10.0;
  heroButtons.alignment = UIStackViewAlignmentCenter;
  _heroText = [[UIStackView alloc]
      initWithArrangedSubviews:@[ _heroCaption, _heroTitle, _heroDetail, heroButtons ]];
  _heroText.axis = UILayoutConstraintAxisVertical;
  _heroText.alignment = UIStackViewAlignmentLeading;
  _heroText.spacing = 2.0;
  [_heroText setCustomSpacing:14.0 afterView:_heroDetail];
  _heroText.translatesAutoresizingMaskIntoConstraints = NO;
  [_heroView addSubview:_heroText];
  _heroTextLeading = [[_heroText.leadingAnchor constraintEqualToAnchor:_heroView.leadingAnchor
                                                              constant:hPad] retain];
  _heroTextBottom = [[_heroText.bottomAnchor constraintEqualToAnchor:_heroView.bottomAnchor
                                                            constant:-20.0] retain];
  [NSLayoutConstraint activateConstraints:@[
    _heroTextLeading,
    _heroTextBottom,
    [_heroText.trailingAnchor constraintLessThanOrEqualToAnchor:_heroView.trailingAnchor
                                                       constant:-hPad],
    [_heroPlayButton.heightAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget],
    [_heroMoreButton.widthAnchor constraintEqualToConstant:kMinTapTarget],
    [_heroMoreButton.heightAnchor constraintEqualToConstant:kMinTapTarget],
  ]];

  // ── Header: Library (left) · Online and the settings capsule (right) ───

  _headerBackdrop = [[UIVisualEffectView alloc]
      initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterial]];
  _headerBackdrop.alpha = 0.0;
  _headerBackdrop.userInteractionEnabled = NO;
  [self addSubview:_headerBackdrop];

  _headerView = [[UIView alloc] init];
  [self addSubview:_headerView];

  _libraryLabel = [[UILabel alloc] init];
  _libraryLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _libraryLabel.text = NSLocalizedString(@"Library", @"launcher overlay view: label text");
  _libraryLabel.textColor = [XeniaTheme textPrimary];
  _libraryLabel.adjustsFontSizeToFitWidth = YES;
  _libraryLabel.minimumScaleFactor = 0.6;
  _libraryLabel.accessibilityTraits = UIAccessibilityTraitHeader;
  // Readable over bright art.
  _libraryLabel.layer.shadowColor = UIColor.blackColor.CGColor;
  _libraryLabel.layer.shadowOpacity = 0.35f;
  _libraryLabel.layer.shadowRadius = 6.0f;
  _libraryLabel.layer.shadowOffset = CGSizeMake(0.0, 1.0);
  [_libraryLabel setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                                 forAxis:UILayoutConstraintAxisHorizontal];
  [_headerView addSubview:_libraryLabel];

  _openGameButton = [self newHeaderIconButton:@"plus" action:@selector(importTapped:)];
  XEApplyAccessibility(
      _openGameButton,
      NSLocalizedString(@"Add Games", @"launcher overlay view: user-visible text"), nil,
      NSLocalizedString(@"Import games, link a folder, or convert the library.",
                        @"launcher overlay view: user-visible text"),
      UIAccessibilityTraitButton);
  _settingsButton = [self newHeaderIconButton:@"gearshape" action:@selector(settingsTapped:)];
  XEApplyAccessibility(
      _settingsButton, NSLocalizedString(@"Settings", @"launcher overlay view: user-visible text"),
      nil,
      NSLocalizedString(@"Opens XeniOS settings.", @"launcher overlay view: user-visible text"),
      UIAccessibilityTraitButton);
  _profileButton = [self newHeaderIconButton:@"person.crop.circle"
                                      action:@selector(profileTapped:)];
  XEApplyAccessibility(
      _profileButton, NSLocalizedString(@"Profile", @"launcher overlay view: user-visible text"),
      nil,
      NSLocalizedString(@"Opens profile management.", @"launcher overlay view: user-visible text"),
      UIAccessibilityTraitButton);

  // +, settings and the profile share one glass capsule.
  UIVisualEffectView* capsule =
      [[[UIVisualEffectView alloc] initWithEffect:xe_make_chrome_visual_effect(NO)] autorelease];
  capsule.translatesAutoresizingMaskIntoConstraints = NO;
  capsule.layer.cornerRadius = 22.0;
  capsule.layer.cornerCurve = kCACornerCurveContinuous;
  capsule.clipsToBounds = YES;
  UIStackView* capsuleStack = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ _openGameButton, _settingsButton, _profileButton ]] autorelease];
  capsuleStack.translatesAutoresizingMaskIntoConstraints = NO;
  capsuleStack.spacing = 2.0;
  capsuleStack.alignment = UIStackViewAlignmentCenter;
  [capsule.contentView addSubview:capsuleStack];
  [NSLayoutConstraint activateConstraints:@[
    [capsule.heightAnchor constraintEqualToConstant:44.0],
    [capsuleStack.leadingAnchor constraintEqualToAnchor:capsule.contentView.leadingAnchor
                                               constant:4.0],
    [capsuleStack.trailingAnchor constraintEqualToAnchor:capsule.contentView.trailingAnchor
                                                constant:-4.0],
    [capsuleStack.centerYAnchor constraintEqualToAnchor:capsule.contentView.centerYAnchor],
  ]];

  // ── Search and sort ────────────────────────────────────────────────────

  _searchField = [[UISearchTextField alloc] init];
  _searchField.translatesAutoresizingMaskIntoConstraints = NO;
  _searchField.delegate = self;
  _searchField.borderStyle = UITextBorderStyleNone;
  _searchField.backgroundColor = [[XeniaTheme textPrimary] colorWithAlphaComponent:0.1];
  _searchField.layer.cornerRadius = 22.0;
  _searchField.layer.cornerCurve = kCACornerCurveContinuous;
  _searchField.clipsToBounds = YES;
  _searchField.textColor = [XeniaTheme textPrimary];
  _searchField.font = xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  _searchField.adjustsFontForContentSizeCategory = YES;
  _searchField.returnKeyType = UIReturnKeySearch;
  _searchField.autocorrectionType = UITextAutocorrectionTypeNo;
  _searchField.enablesReturnKeyAutomatically = NO;
  // Sort keeps its one line; the field gives up the width.
  [_searchField setContentCompressionResistancePriority:UILayoutPriorityDefaultLow
                                                forAxis:UILayoutConstraintAxisHorizontal];
  [_searchField setContentHuggingPriority:UILayoutPriorityDefaultLow
                                  forAxis:UILayoutConstraintAxisHorizontal];
  [_searchField addTarget:self
                   action:@selector(searchChanged:)
         forControlEvents:UIControlEventEditingChanged];

  UIButtonConfiguration* sortCfg = [UIButtonConfiguration filledButtonConfiguration];
  sortCfg.baseBackgroundColor = [[XeniaTheme textPrimary] colorWithAlphaComponent:0.1];
  sortCfg.baseForegroundColor = [XeniaTheme textPrimary];
  sortCfg.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  sortCfg.image = [UIImage
      systemImageNamed:@"chevron.down"
      withConfiguration:[UIImageSymbolConfiguration
                            configurationWithTextStyle:UIFontTextStyleCaption1
                                                 scale:UIImageSymbolScaleMedium]];
  sortCfg.imagePlacement = NSDirectionalRectEdgeTrailing;
  sortCfg.imagePadding = 4.0;
  sortCfg.contentInsets = NSDirectionalEdgeInsetsMake(0.0, 16.0, 0.0, 14.0);
  sortCfg.title = NSLocalizedString(@"Sort", @"launcher overlay view: sort menu button");
  sortCfg.titleLineBreakMode = NSLineBreakByClipping;
  sortCfg.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
    return updated;
  };
  _sortButton = [[UIButton buttonWithConfiguration:sortCfg primaryAction:nil] retain];
  _sortButton.translatesAutoresizingMaskIntoConstraints = NO;
  _sortButton.showsMenuAsPrimaryAction = YES;
  [_sortButton setContentHuggingPriority:UILayoutPriorityRequired
                                 forAxis:UILayoutConstraintAxisHorizontal];
  [_sortButton setContentCompressionResistancePriority:UILayoutPriorityRequired
                                               forAxis:UILayoutConstraintAxisHorizontal];
  [self updateSortMenu];

  _searchRow = [[UIStackView alloc] initWithArrangedSubviews:@[ _searchField, _sortButton ]];
  _searchRow.spacing = 10.0;
  _searchRow.alignment = UIStackViewAlignmentFill;
  _searchRow.hidden = YES;  // Until there are games.
  _searchWidth = [[_searchField.widthAnchor constraintEqualToConstant:210.0] retain];
  [NSLayoutConstraint activateConstraints:@[
    [_searchField.heightAnchor constraintEqualToConstant:44.0],
    [_sortButton.heightAnchor constraintEqualToConstant:44.0],
  ]];

  // Online, the capsule's height and only as wide as its text.
  UIButton* online = xe_ios_new_system_link_online_button();
  [online setContentHuggingPriority:UILayoutPriorityRequired
                            forAxis:UILayoutConstraintAxisHorizontal];
  [online.heightAnchor constraintEqualToConstant:44.0].active = YES;
  _headerActions = [[UIStackView alloc] initWithArrangedSubviews:@[ online, capsule ]];
  _headerActions.translatesAutoresizingMaskIntoConstraints = NO;
  _headerActions.alignment = UIStackViewAlignmentCenter;
  _headerActions.spacing = 8.0;
  [_headerView addSubview:_headerActions];

  // ── Notices: one row each, while something needs fixing ────────────────

  _jitStatusIcon = [[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"bolt.fill"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithTextStyle:UIFontTextStyleBody
                                                                       scale:
                                                                           UIImageSymbolScaleMedium]]];
  _jitStatusIcon.tintColor = [XeniaTheme sectionIconOrange];
  _jitTitleLabel = [[UILabel alloc] init];
  _jitTitleLabel.text = NSLocalizedString(@"JIT Is Off", @"launcher overlay view: JIT card title");
  _jitStatusLabel = [[UILabel alloc] init];
  _jitStatusLabel.text =
      NSLocalizedString(@"Games need it to start.", @"launcher overlay view: JIT card detail");
  _jitOpenButton = [xe_new_notice_action(NSLocalizedString(
      @"Turn On", @"launcher overlay view: button that turns on JIT")) retain];
  _jitOpenButton.hidden = YES;  // Until `openStikDebugHandler` is set.
  [_jitOpenButton addTarget:self
                     action:@selector(openStikDebugTapped:)
           forControlEvents:UIControlEventTouchUpInside];
  XEApplyAccessibility(
      _jitOpenButton,
      NSLocalizedString(@"Turn On JIT", @"launcher overlay view: accessibility label"), nil,
      NSLocalizedString(@"Switches to StikDebug to enable JIT.",
                        @"launcher overlay view: accessibility hint"),
      UIAccessibilityTraitButton);
  _jitWarningCard = xe_new_notice_card(_jitStatusIcon, _jitTitleLabel, _jitStatusLabel,
                                       _jitOpenButton);

  // Shown when the installed signature lacks
  // com.apple.developer.kernel.increased-memory-limit.
  _memoryStatusIcon = [[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"memorychip.fill"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithTextStyle:UIFontTextStyleBody
                                                                       scale:
                                                                           UIImageSymbolScaleMedium]]];
  _memoryStatusIcon.tintColor = [XeniaTheme statusWarning];
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

  NSArray<UIView*>* noticeSeparators = nil;
  _noticeGroup = xe_new_notice_group(@[ _jitWarningCard, _memoryWarningCard ], &noticeSeparators);
  _noticeSeparator = [noticeSeparators.firstObject retain];

  _topInfoStack = [[UIStackView alloc] initWithArrangedSubviews:@[ _noticeGroup, _searchRow ]];
  _topInfoStack.axis = UILayoutConstraintAxisVertical;
  _topInfoStack.spacing = 18.0;
  _topInfoStack.translatesAutoresizingMaskIntoConstraints = NO;

  // ── Games grid ─────────────────────────────────────────────────────────

  UICollectionViewFlowLayout* gridLayout = [[UICollectionViewFlowLayout alloc] init];
  gridLayout.minimumInteritemSpacing = 16;
  gridLayout.minimumLineSpacing = 20;
  gridLayout.sectionInset = UIEdgeInsetsZero;
  _gamesCollectionView = [[UICollectionView alloc] initWithFrame:CGRectZero
                                            collectionViewLayout:gridLayout];
  _gamesCollectionView.dataSource = self;
  _gamesCollectionView.prefetchDataSource = self;
  _gamesCollectionView.delegate = self;
  _gamesCollectionView.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  _gamesCollectionView.backgroundColor = [UIColor clearColor];
  _gamesCollectionView.alwaysBounceVertical = YES;
  _gamesCollectionView.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
  [_gamesCollectionView registerClass:[XeniaGameTileCell class]
           forCellWithReuseIdentifier:@"ImportedGameCell"];
  [self insertSubview:_gamesCollectionView belowSubview:_headerBackdrop];
  // The art, the notices and search scroll with the games, in the grid's top inset.
  [_gamesCollectionView addSubview:_topInfoStack];

  // Empty state: a content-unavailable view behind the grid, shown once the
  // first snapshot arrives with no games.
  _emptyStateProxy = [[XeniaLauncherEmptyStateActionProxy alloc] init];
  _emptyStateProxy.owner = self;
  _pendingArtLoads = [[NSMutableSet alloc] init];
  _pendingIconDecodes = [[NSMutableSet alloc] init];

  // ── Layout ─────────────────────────────────────────────────────────────
  // The header, the art and the grid have frames from -layoutSubviews; what is
  // inside them uses constraints.

  [NSLayoutConstraint activateConstraints:@[
    [_libraryLabel.leadingAnchor constraintEqualToAnchor:_headerView.leadingAnchor],
    [_libraryLabel.centerYAnchor constraintEqualToAnchor:_headerView.centerYAnchor],
    [_libraryLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_headerActions.leadingAnchor
                                                           constant:-8.0],
    [_headerActions.trailingAnchor constraintEqualToAnchor:_headerView.trailingAnchor],
    [_headerActions.centerYAnchor constraintEqualToAnchor:_headerView.centerYAnchor],

    // Notices and search, above the grid's content.
    [_topInfoStack.bottomAnchor
        constraintEqualToAnchor:_gamesCollectionView.contentLayoutGuide.topAnchor
                       constant:-kLibraryHeaderGap],
    [_topInfoStack.leadingAnchor
        constraintEqualToAnchor:_gamesCollectionView.frameLayoutGuide.leadingAnchor
                       constant:hPad],
    [_topInfoStack.trailingAnchor
        constraintEqualToAnchor:_gamesCollectionView.frameLayoutGuide.trailingAnchor
                       constant:-hPad],
  ]];

  [gridLayout release];
  [self setJITAcquired:NO];
  [self setMemoryEntitlementEnabled:YES];
  [self updateNoticeGroup];
  [self applyActionsEnabledState];
  [self updateNoticeCardsForTextSize];
  [self updateHeaderFonts];
  [self registerForTraitChanges:@[
    [UITraitPreferredContentSizeCategory class], [UITraitHorizontalSizeClass class],
    [UITraitVerticalSizeClass class]
  ]
                     withAction:@selector(layoutTraitsChanged)];
  return self;
}

// One notice at a time, the one that stops games first: JIT, then the memory limit.
- (void)updateNoticeGroup {
  _jitWarningCard.hidden = _jitAcquired;
  _memoryWarningCard.hidden = _memoryEntitlementEnabled || !_jitAcquired;
  _noticeSeparator.hidden = YES;
  _noticeGroup.hidden = _jitWarningCard.hidden && _memoryWarningCard.hidden;
  [self setNeedsLayout];
}

// Large titles: 34 pt, or 30 pt in the shorter side layout; the game's name a little bigger.
- (void)updateHeaderFonts {
  const CGFloat title_size = _sideLayout ? 30.0 : 34.0;
  _libraryLabel.font = xe_scaled_system_font(UIFontTextStyleLargeTitle, title_size,
                                             UIFontWeightBold);
  _heroTitle.font = xe_scaled_system_font(UIFontTextStyleLargeTitle, _sideLayout ? 32.0 : 34.0,
                                          UIFontWeightHeavy);
}

// Search and sort sit in the header with the side layout, and above the grid otherwise.
- (void)placeSearchRow {
  _searchWidth.active = _sideLayout;
  if (_sideLayout) {
    if (_searchRow.superview != _headerActions) {
      [_searchRow removeFromSuperview];
      [_headerActions insertArrangedSubview:_searchRow atIndex:0];
    }
  } else if (_searchRow.superview != _topInfoStack) {
    [_searchRow removeFromSuperview];
    [_topInfoStack addArrangedSubview:_searchRow];
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGRect bounds = self.bounds;
  const UIEdgeInsets safe = self.safeAreaInsets;
  const BOOL side = bounds.size.width > bounds.size.height;
  if (side != _sideLayout) {
    _sideLayout = side;
    [self placeSearchRow];
    [self updateHeaderFonts];
    [_gamesCollectionView.collectionViewLayout invalidateLayout];
  }
  const BOOL hero = _heroGame != nil;
  _heroView.hidden = !hero;

  // Header: clear of the Dynamic Island and the corners on both sides.
  const CGFloat left = MAX(safe.left, kLibraryMargin);
  const CGFloat right = MAX(safe.right, kLibraryMargin);
  const CGFloat header_height = 52.0;
  const CGFloat header_top = side ? MAX(safe.top, 10.0) : safe.top;
  _headerView.frame =
      CGRectMake(left, header_top, bounds.size.width - left - right, header_height);
  const CGFloat header_bottom = CGRectGetMaxY(_headerView.frame);
  _headerBackdrop.frame = CGRectMake(0.0, 0.0, bounds.size.width, header_bottom);

  [_topInfoStack layoutIfNeeded];
  const CGFloat stack_height = _topInfoStack.hidden ? 0.0 : _topInfoStack.bounds.size.height;
  const CGFloat stack_block =
      stack_height > 0.0 ? kLibraryHeaderTop + stack_height + kLibraryHeaderGap : kLibraryHeaderTop;

  CGRect grid_frame;
  CGFloat top_inset;
  CGFloat hero_height = 0.0;
  if (side) {
    // Art down the left side, under the header; the grid to its right, below the header.
    const CGFloat hero_width = hero ? floor(bounds.size.width * 0.42) : 0.0;
    if (hero) {
      if (_heroView.superview != self) {
        [_heroView removeFromSuperview];
        [self insertSubview:_heroView belowSubview:_gamesCollectionView];
      }
      _heroView.frame = CGRectMake(0.0, 0.0, hero_width, bounds.size.height);
      _heroTextLeading.constant = left;
      _heroTextBottom.constant = -MAX(safe.bottom, 24.0);
    }
    // The grid runs up under the header, which blurs in behind it as it scrolls, so rows
    // don't end at a hard edge.
    const CGFloat grid_left = hero ? hero_width : safe.left;
    grid_frame = CGRectMake(grid_left, 0.0, bounds.size.width - grid_left - safe.right,
                            bounds.size.height);
    top_inset = header_bottom + 6.0 + stack_block;
    _headerBackdrop.frame = CGRectMake(grid_left, 0.0, bounds.size.width - grid_left,
                                       header_bottom + 6.0);
  } else {
    // Art across the top, under the header, scrolling away with the grid.
    grid_frame = bounds;
    if (hero) {
      [_heroText layoutIfNeeded];
      const CGFloat text_height =
          [_heroText systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].height;
      hero_height = MAX(318.0, header_bottom + 24.0 + text_height + 20.0);
      _heroTextLeading.constant = left + 4.0;
      _heroTextBottom.constant = -20.0;
    }
    top_inset = (hero ? hero_height : header_bottom) + stack_block;
  }
  _gamesCollectionView.frame = grid_frame;
  if (hero && !side) {
    if (_heroView.superview != _gamesCollectionView) {
      [_heroView removeFromSuperview];
      [_gamesCollectionView insertSubview:_heroView atIndex:0];
    }
    _heroView.frame = CGRectMake(0.0, -top_inset, grid_frame.size.width, hero_height);
  }
  _heroArt.frame = _heroView.bounds;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  const CGRect hero_bounds = _heroView.bounds;
  _heroTopScrim.frame = CGRectMake(0.0, 0.0, hero_bounds.size.width, 150.0);
  _heroScrim.frame = hero_bounds;
  UIColor* page = [[XeniaTheme bgPrimary] resolvedColorWithTraitCollection:self.traitCollection];
  if (side) {
    // Fades into the page on the right, where the grid starts.
    _heroScrim.colors = @[
      (id)[page colorWithAlphaComponent:0.0].CGColor, (id)[page colorWithAlphaComponent:0.0].CGColor,
      (id)[page colorWithAlphaComponent:0.65].CGColor, (id)page.CGColor
    ];
    _heroScrim.locations = @[ @0.0, @0.5, @0.8, @1.0 ];
    _heroScrim.startPoint = CGPointMake(0.0, 0.5);
    _heroScrim.endPoint = CGPointMake(1.0, 0.5);
  } else {
    // Fades into the page at the bottom, behind the text.
    _heroScrim.colors = @[
      (id)[page colorWithAlphaComponent:0.0].CGColor, (id)[page colorWithAlphaComponent:0.0].CGColor,
      (id)[page colorWithAlphaComponent:0.55].CGColor, (id)page.CGColor
    ];
    _heroScrim.locations = @[ @0.0, @0.38, @0.68, @1.0 ];
    _heroScrim.startPoint = CGPointMake(0.5, 0.0);
    _heroScrim.endPoint = CGPointMake(0.5, 1.0);
  }
  [CATransaction commit];

  const CGFloat bottom = (side ? 0.0 : safe.bottom) + 16.0;
  UIEdgeInsets insets = _gamesCollectionView.contentInset;
  if (insets.top != top_inset || insets.bottom != bottom) {
    // Keep the top in view when the list was scrolled to its top.
    const BOOL at_top = _gamesCollectionView.contentOffset.y <= -insets.top + 1.0;
    insets.top = top_inset;
    insets.bottom = bottom;
    _gamesCollectionView.contentInset = insets;
    _gamesCollectionView.verticalScrollIndicatorInsets =
        UIEdgeInsetsMake(header_bottom, 0, bottom, 0);
    if (at_top) {
      _gamesCollectionView.contentOffset = CGPointMake(0.0, -top_inset);
    }
  }
  [self updateHeaderBackdrop];
}

// The header gets a bar behind it once the art or the first row scrolls under it.
- (void)updateHeaderBackdrop {
  CGFloat alpha = 0.0;
  if (_sideLayout) {
    const CGFloat scrolled =
        _gamesCollectionView.contentOffset.y + _gamesCollectionView.contentInset.top;
    alpha = MIN(MAX(scrolled / 24.0, 0.0), 1.0);
  } else {
    const CGFloat header_bottom = CGRectGetMaxY(_headerView.frame);
    const CGFloat content_top = -_gamesCollectionView.contentInset.top;
    const CGFloat under = _heroGame ? (CGRectGetHeight(_heroView.frame) - header_bottom) : 0.0;
    const CGFloat scrolled = _gamesCollectionView.contentOffset.y - content_top - under;
    alpha = MIN(MAX(scrolled / 24.0, 0.0), 1.0);
  }
  _headerBackdrop.alpha = alpha;
  _libraryLabel.layer.shadowOpacity = (alpha > 0.5 && !_sideLayout) ? 0.0f : 0.35f;
}

- (void)scrollViewDidScroll:(UIScrollView*)scrollView {
  if (scrollView == _gamesCollectionView) {
    [self updateHeaderBackdrop];
  }
}

#pragma mark - Continue Playing

// The game played most recently that is still in the library.
- (void)updateHero {
  XeniaIOSLauncherGameSnapshot* latest = nil;
  for (XeniaIOSLauncherGameSnapshot* game in _games) {
    if (game.lastPlayed && (!latest || [game.lastPlayed compare:latest.lastPlayed] ==
                                           NSOrderedDescending)) {
      latest = game;
    }
  }
  [_heroGame release];
  _heroGame = [latest retain];
  if (!latest) {
    _heroArtTitleId = 0;
    _heroArt.image = nil;
    [self setNeedsLayout];
    return;
  }
  _heroTitle.text = latest.title;
  NSRelativeDateTimeFormatter* formatter = [[[NSRelativeDateTimeFormatter alloc] init] autorelease];
  formatter.unitsStyle = NSRelativeDateTimeFormatterUnitsStyleFull;
  NSString* when = [NSString
      stringWithFormat:NSLocalizedString(@"Played %@",
                                         @"launcher overlay view: Continue Playing; %@ is like "
                                         @"\"2 hours ago\""),
                       [formatter localizedStringForDate:latest.lastPlayed relativeToDate:[NSDate date]]];
  _heroDetail.text = latest.hasCompatInfo
                         ? [NSString stringWithFormat:@"%@ · %@", when,
                                                      xe_compat_status_label(latest.compatStatus)]
                         : when;
  _heroPlayButton.accessibilityLabel = [NSString
      stringWithFormat:NSLocalizedString(@"Play %@", @"launcher overlay view: accessibility "
                                                     @"label; %@ is the game"),
                       latest.title];
  if (_heroArtTitleId != latest.titleId) {
    _heroArtTitleId = latest.titleId;
    // The wide background art, else the cover, else the game's own art.
    _heroArt.image = xe_cached_game_background_art(latest.titleId)
                         ?: (xe_cached_game_art(latest.titleId) ?: latest.decodedIcon);
    if (latest.supportsRemoteArt && !xe_cached_game_background_art(latest.titleId)) {
      const uint32_t title_id = latest.titleId;
      xe_fetch_game_background_art(title_id, ^(UIImage* image) {
        if (image && self->_heroArtTitleId == title_id) {
          self->_heroArt.image = image;
        } else if (!image && self->_heroArtTitleId == title_id && !self->_heroArt.image) {
          xe_fetch_game_art(title_id, ^(UIImage* cover) {
            if (cover && self->_heroArtTitleId == title_id && !self->_heroArt.image) {
              self->_heroArt.image = cover;
            }
          });
        }
      });
    }
  }
  [self setNeedsLayout];
}

- (void)heroPlayTapped:(UIButton*)__unused sender {
  const NSUInteger index = _heroGame ? [_games indexOfObjectIdenticalTo:_heroGame] : NSNotFound;
  if (index != NSNotFound && _actionsEnabled && _gameLaunchedHandler) {
    _gameLaunchedHandler(index);
  }
}

- (void)heroMoreTapped:(UIButton*)__unused sender {
  const NSUInteger index = _heroGame ? [_games indexOfObjectIdenticalTo:_heroGame] : NSNotFound;
  if (index != NSNotFound && _gameActionsHandler) {
    _gameActionsHandler(index);
  }
}

#pragma mark - Search and sort

- (void)updateSortMenu {
  NSArray<NSString*>* titles = @[
    NSLocalizedString(@"Name", @"launcher overlay view: sort by name"),
    NSLocalizedString(@"Recently Played", @"launcher overlay view: sort by last played"),
  ];
  NSMutableArray<UIMenuElement*>* actions = [NSMutableArray array];
  // Not retained (MRC __block): the menu belongs to the overlay's own button.
  __block XeniaIOSLauncherOverlayView* owner = self;
  for (NSUInteger i = 0; i < titles.count; ++i) {
    UIAction* action = [UIAction actionWithTitle:titles[i]
                                           image:nil
                                      identifier:nil
                                         handler:^(__unused UIAction* a) {
                                           [owner setSortMode:static_cast<NSInteger>(i)];
                                         }];
    action.state = static_cast<NSInteger>(i) == _sortMode ? UIMenuElementStateOn
                                                          : UIMenuElementStateOff;
    [actions addObject:action];
  }
  _sortButton.menu = [UIMenu menuWithTitle:NSLocalizedString(@"Sort By",
                                                             @"launcher overlay view: sort menu")
                                  children:actions];
  _sortButton.accessibilityValue = titles[static_cast<NSUInteger>(
      MIN(MAX(_sortMode, (NSInteger)0), (NSInteger)titles.count - 1))];
}

- (void)setSortMode:(NSInteger)mode {
  if (mode == _sortMode) {
    return;
  }
  _sortMode = mode;
  [[NSUserDefaults standardUserDefaults] setInteger:mode forKey:kLibrarySortDefaultsKey];
  [self updateSortMenu];
  [self reloadShownGames];
}

- (void)searchChanged:(UITextField*)__unused field {
  NSString* query = [_searchField.text
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if ([query isEqualToString:_query]) {
    return;
  }
  [_query release];
  _query = [query copy];
  [self reloadShownGames];
}

- (BOOL)textFieldShouldReturn:(UITextField*)textField {
  [textField resignFirstResponder];
  return NO;
}

// The games to show from `games`: the search's matches, in the chosen order.
- (NSArray<XeniaIOSLauncherGameSnapshot*>*)shownGamesFrom:
    (NSArray<XeniaIOSLauncherGameSnapshot*>*)games {
  NSArray<XeniaIOSLauncherGameSnapshot*>* shown = games ?: @[];
  if (_query.length) {
    shown = [shown filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(
                                                   XeniaIOSLauncherGameSnapshot* game,
                                                   __unused NSDictionary* bindings) {
                     return [game.title rangeOfString:_query
                                              options:NSCaseInsensitiveSearch |
                                                      NSDiacriticInsensitiveSearch]
                                .location != NSNotFound;
                   }]];
  }
  if (_sortMode == 1) {
    // Most recent first; games never played keep their order after them.
    shown = [shown sortedArrayWithOptions:NSSortStable
                          usingComparator:^NSComparisonResult(XeniaIOSLauncherGameSnapshot* a,
                                                              XeniaIOSLauncherGameSnapshot* b) {
                            if (a.lastPlayed && b.lastPlayed) {
                              return [b.lastPlayed compare:a.lastPlayed];
                            }
                            if (a.lastPlayed || b.lastPlayed) {
                              return a.lastPlayed ? NSOrderedAscending : NSOrderedDescending;
                            }
                            return NSOrderedSame;
                          }];
  }
  return shown;
}

- (void)updateSearchPlaceholder {
  _searchField.placeholder =
      _games.count == 1
          ? NSLocalizedString(@"Search 1 Game", @"launcher overlay view: search placeholder")
          : [NSString stringWithFormat:NSLocalizedString(@"Search %lu Games",
                                                         @"launcher overlay view: search "
                                                         @"placeholder; %lu is the game count"),
                                       (unsigned long)_games.count];
}

- (void)reloadShownGames {
  [_shownGames release];
  _shownGames = [[self shownGamesFrom:_games] copy];
  if (_focusedGameIndex >= [self libraryItemCount]) {
    _focusedGameIndex = [self libraryItemCount] > 0 ? 0 : -1;
  }
  [_gamesCollectionView reloadData];
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
  [_addMenu release];
  [_gameLaunchedHandler release];
  [_copyLaunchURLHandler release];
  [_gameSettingsHandler release];
  [_touchLayoutHandler release];
  [_compatibilityHandler release];
  [_manageContentHandler release];
  [_discSelectionHandler release];
  [_patchesHandler release];
  [_zarConversionHandler release];
  [_headerView release];
  [_headerBackdrop release];
  [_headerActions release];
  [_searchRow release];
  [_searchField release];
  [_sortButton release];
  [_searchWidth release];
  [_heroView release];
  [_heroArt release];
  [_heroScrim release];
  [_heroTopScrim release];
  [_heroText release];
  [_heroCaption release];
  [_heroTitle release];
  [_heroDetail release];
  [_heroPlayButton release];
  [_heroMoreButton release];
  [_heroTextLeading release];
  [_heroTextBottom release];
  [_heroGame release];
  [_shownGames release];
  [_query release];
  [_gameActionsHandler release];
  [_settingsButton release];
  [_profileButton release];
  [_openGameButton release];
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
  [_libraryLabel release];
  [_topInfoStack release];
  [_noticeGroup release];
  [_noticeSeparator release];
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
  if ([self gameItemOffset]) {
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
// The Dashboard tile leads the grid while the library has games and no
// search is narrowing it.
- (NSInteger)gameItemOffsetFor:(NSArray<XeniaIOSLauncherGameSnapshot*>*)games {
  return games.count > 0 && _query.length == 0 ? 1 : 0;
}

- (NSInteger)gameItemOffset {
  return [self gameItemOffsetFor:_games];
}

- (NSInteger)libraryItemCount {
  return static_cast<NSInteger>(_shownGames.count) + [self gameItemOffset];
}

- (BOOL)isDashboardItem:(NSInteger)item {
  return [self gameItemOffset] && item == 0;
}

// The game an item shows: nil for the Dashboard tile and past the end.
- (XeniaIOSLauncherGameSnapshot*)gameAtItem:(NSInteger)item {
  const NSInteger index = item - [self gameItemOffset];
  if (index < 0 || index >= static_cast<NSInteger>(_shownGames.count)) {
    return nil;
  }
  return _shownGames[static_cast<NSUInteger>(index)];
}

- (NSInteger)gameIndexForItem:(NSInteger)item {
  XeniaIOSLauncherGameSnapshot* game = [self gameAtItem:item];
  const NSUInteger index = game ? [_games indexOfObjectIdenticalTo:game] : NSNotFound;
  return index == NSNotFound ? -1 : static_cast<NSInteger>(index);
}

- (NSInteger)itemForGameIndex:(NSInteger)gameIndex {
  if (gameIndex < 0 || gameIndex >= static_cast<NSInteger>(_games.count)) {
    return -1;
  }
  const NSUInteger shown =
      [_shownGames indexOfObjectIdenticalTo:_games[static_cast<NSUInteger>(gameIndex)]];
  return shown == NSNotFound ? -1 : static_cast<NSInteger>(shown) + [self gameItemOffset];
}

- (void)configureDashboardCell:(XeniaGameTileCell*)cell {
  NSString* title =
      NSLocalizedString(@"Xbox 360 Dashboard", @"launcher overlay view: user-visible text");
  cell.representedTitleId = 0;
  // The full name doesn't fit under a cover; the art already says Xbox.
  cell.titleLabel.text =
      NSLocalizedString(@"Dashboard", @"launcher overlay view: the Xbox 360 Dashboard tile");
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

// Touch opens the menu straight from +; a controller press still goes through importHandler.
- (void)setAddMenu:(UIMenu*)addMenu {
  if (_addMenu == addMenu) {
    return;
  }
  [_addMenu release];
  _addMenu = [addMenu retain];
  _openGameButton.menu = addMenu;
  _openGameButton.showsMenuAsPrimaryAction = addMenu != nil;
}

#pragma mark - Focus visuals

- (void)applyActionsEnabledState {
  _settingsButton.enabled = _actionsEnabled;
  _profileButton.enabled = _actionsEnabled;
  _openGameButton.enabled = _actionsEnabled;

  CGFloat actionAlpha = _actionsEnabled ? 1.0 : 0.5;
  _settingsButton.alpha = actionAlpha;
  _profileButton.alpha = actionAlpha;
  _openGameButton.alpha = actionAlpha;
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
    if (_focusedGameIndex >= 0 && _focusedGameIndex < [self libraryItemCount]) {
      NSIndexPath* focusedPath = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
      [_gamesCollectionView reconfigureItemsAtIndexPaths:@[ focusedPath ]];
    }
    return;
  }

  [self setButton:_settingsButton controllerFocused:settingsFocused];
  [self setButton:_profileButton controllerFocused:profileFocused];
  [self setButton:_openGameButton controllerFocused:importFocused];

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
  importCfg.baseBackgroundColor = [XeniaTheme textPrimary];
  importCfg.baseForegroundColor = [XeniaTheme bgPrimary];
  importCfg.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
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
  // The grid shows `_shownGames`, so the diff runs over what it showed and what it will show.
  const BOOL hadGames = _games != nil;
  NSArray<XeniaIOSLauncherGameSnapshot*>* oldGames = [_shownGames retain];
  const NSInteger oldOffset = [self gameItemOffset];
  [_games release];
  _games = [games copy];
  [_shownGames release];
  _shownGames = [[self shownGamesFrom:_games] copy];
  const NSInteger newOffset = [self gameItemOffset];
  _gamesLoaded = YES;
  [self updateEmptyState];
  [self updateSearchPlaceholder];
  _searchRow.hidden = _games.count == 0;
  [self updateHero];

  // Pair the games that are in both lists (identity: title id and title) so
  // that only inserted, removed and changed tiles are touched.
  NSOrderedCollectionDifference<XeniaIOSLauncherGameSnapshot*>* diff = [_shownGames
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
  while (oldIndex < oldGames.count && newIndex < _shownGames.count) {
    if ([removedOld containsIndex:oldIndex]) {
      ++oldIndex;
      continue;
    }
    if ([insertedNew containsIndex:newIndex]) {
      ++newIndex;
      continue;
    }
    XeniaIOSLauncherGameSnapshot* before = oldGames[oldIndex];
    XeniaIOSLauncherGameSnapshot* after = _shownGames[newIndex];
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
  if (!hadGames || !self.window || self.hidden || oldOffset != newOffset) {
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

  [self updateNoticeGroup];
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

  [self updateNoticeGroup];

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

- (void)updateNoticeCardsForTextSize {
  const bool stacked = xe_uses_accessibility_text(self.traitCollection);
  xe_set_notice_card_stacked(_jitWarningCard, stacked);
  xe_set_notice_card_stacked(_memoryWarningCard, stacked);
}

- (void)layoutTraitsChanged {
  [self updateNoticeCardsForTextSize];
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
  CGFloat gridSpacing = 12.0f;
  // Phones in portrait show three covers across, like Photos' and Files' grids.
  const BOOL compactWidth =
      self.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassCompact;
  CGFloat minimumTileWidth = compactLandscape ? 170.0f : (compactWidth ? 100.0f : 190.0f);
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
  return 12.0f;
}

- (CGFloat)lineSpacingForCollectionView:(UICollectionView*)collectionView {
  return [self usesCompactLandscapeLayoutForContentSize:collectionView.bounds.size] ? 14.0f : 18.0f;
}

// Footer height under the cover, from the fonts it shows (the title, then the status line in
// xe_status_text's font), so it fits its text at every Dynamic Type size.
- (CGFloat)titleHeightForCollectionView:(UICollectionView*)collectionView {
  BOOL accessibilitySize = xe_uses_accessibility_text(collectionView.traitCollection);
  CGFloat titleLine =
      xe_scaled_system_font(UIFontTextStyleFootnote, 13.5, UIFontWeightSemibold).lineHeight;
  CGFloat metaLine =
      xe_scaled_system_font(UIFontTextStyleCaption1, 12.0, UIFontWeightRegular).lineHeight;
  CGFloat lines = accessibilitySize ? 2.0 : 1.0;
  // 7 above the title + title + 1 gap + status (+ 4 between stacked items).
  return 7.0 + ceil(titleLine * lines) + 1.0 + ceil(metaLine * lines) +
         (accessibilitySize ? 4.0 : 0.0);
}

- (CGFloat)tileWidthForCollectionView:(UICollectionView*)collectionView
                              columns:(NSInteger)columns
                     interitemSpacing:(CGFloat)spacing {
  CGFloat contentWidth = collectionView.bounds.size.width;
  CGFloat totalSpacing = spacing * MAX(columns - 1, 0);
  CGFloat availableWidth = MAX(contentWidth - totalSpacing - 2.0 * kLibraryMargin, 0.0f);
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
  // The game's name as its cover, drawn once per name and appearance.
  static NSCache<NSString*, UIImage*>* covers = [[NSCache alloc] init];
  NSString* key = [NSString
      stringWithFormat:@"%d|%@",
                       (int)(self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark),
                       game.title ?: @""];
  UIImage* cover = [covers objectForKey:key];
  if (!cover) {
    cover = xe_title_cover_art(game.title, self.traitCollection);
    [covers setObject:cover forKey:key];
  }
  return cover;
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
    const NSInteger gameIndex = [self gameIndexForItem:indexPath.item];
    if (gameIndex >= 0) {
      _gameLaunchedHandler(static_cast<NSUInteger>(gameIndex));
    }
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
                                                      cornerRadius:cell.cardView.layer.cornerRadius];
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

  NSUInteger gameIndex = static_cast<NSUInteger>([self gameIndexForItem:indexPath.item]);
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
                     // Grouped like a system menu, divided by section, with only the actions
                     // that apply to this game.
                     auto group = ^UIMenu*(NSArray<UIAction*>* actions) {
                       return [UIMenu menuWithTitle:@""
                                              image:nil
                                         identifier:nil
                                            options:UIMenuOptionsDisplayInline
                                           children:actions];
                     };
                     NSMutableArray<UIAction*>* setup = [NSMutableArray array];
                     NSMutableArray<UIAction*>* content = [NSMutableArray array];
                     NSMutableArray<UIAction*>* info = [NSMutableArray array];
                     if (game.titleId) {
                       [setup addObject:gameSettingsAction];
                       [setup addObject:touchLayoutAction];
                     }
                     if (game.supportsPatches) {
                       [setup addObject:patchesAction];
                     }
                     if (game.supportsDiscSelection) {
                       [content addObject:discAction];
                     }
                     if (game.supportsManageContent) {
                       [content addObject:contentAction];
                     }
                     if (game.supportsZarConversion) {
                       [content addObject:zarConversionAction];
                     }
                     if (game.supportsCompatibility) {
                       [info addObject:compatAction];
                     }
                     if (game.titleId) {
                       [info addObject:copyURLAction];
                     }
                     NSMutableArray<UIMenuElement*>* children =
                         [NSMutableArray arrayWithObject:group(@[ playAction ])];
                     for (NSArray<UIAction*>* actions in @[ setup, content, info ]) {
                       if (actions.count) {
                         [children addObject:group(actions)];
                       }
                     }
                     return [UIMenu menuWithTitle:@"" children:children];
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
