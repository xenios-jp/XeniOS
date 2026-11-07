/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_launcher_overlay_view.h"

#import <CoreImage/CoreImage.h>

#include <cmath>

#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_game_tile_cell.h"
#import "xenia/ui/ios/launcher/ios_library_ambient_view.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

// Fetches the wide background art of |title_ids| one at a time, so a game's page has it the first
// time it opens. Each title is asked for once per run; titles the art repository has no image for
// are remembered by the art cache and not asked for again for a while.
static void XEPrefetchBackgroundArt(NSArray<NSNumber*>* title_ids, NSUInteger index) {
  static NSMutableSet<NSNumber*>* requested = [[NSMutableSet alloc] init];
  while (index < title_ids.count &&
         ([requested containsObject:title_ids[index]] ||
          xe_has_cached_game_background_art(title_ids[index].unsignedIntValue))) {
    ++index;
  }
  if (index >= title_ids.count) {
    return;
  }
  NSNumber* title_id = title_ids[index];
  [requested addObject:title_id];
  xe_fetch_game_background_art(title_id.unsignedIntValue, ^(UIImage* __unused image) {
    XEPrefetchBackgroundArt(title_ids, index + 1);
  });
}

// A small, heavily blurred copy of |image| for backdrops: art that isn't sharp enough to be shown
// large reads as colour and light instead of as blurry pixels.
static UIImage* XEBlurredBackdropImage(UIImage* image) {
  if (!image.CGImage) {
    return nil;
  }
  static CIContext* context = [[CIContext contextWithOptions:nil] retain];
  CIImage* input = [CIImage imageWithCGImage:image.CGImage];
  const CGFloat longest = MAX(input.extent.size.width, input.extent.size.height);
  if (longest <= 0.0) {
    return nil;
  }
  const CGFloat scale = MIN(1.0, 160.0 / longest);
  const CGRect extent =
      CGRectMake(0.0, 0.0, floor(input.extent.size.width * scale),
                 floor(input.extent.size.height * scale));
  CIImage* small = [input imageByApplyingTransform:CGAffineTransformMakeScale(scale, scale)];
  CIImage* blurred = [[small imageByClampingToExtent]
      imageByApplyingGaussianBlurWithSigma:MAX(6.0, extent.size.width / 14.0)];
  CGImageRef rendered = [context createCGImage:blurred fromRect:extent];
  if (!rendered) {
    return nil;
  }
  UIImage* result = [UIImage imageWithCGImage:rendered];
  CGImageRelease(rendered);
  return result;
}

@implementation XeniaIOSLauncherGameSnapshot

- (void)dealloc {
  [_title release];
  [_contentTypeName release];
  [_compatStatus release];
  [_compatPerf release];
  [_compatNotes release];
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

// The header's row. On a game page only its ✕ and ⋯ show, and the page's rows can reach up beside
// them (the first row sits level with ✕ on a phone on its side), so the rest of the row lets
// touches through.
@interface XeniaLauncherHeaderView : UIView
@property(nonatomic, assign) BOOL passesTouchesThrough;
@end

@implementation XeniaLauncherHeaderView
- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  UIView* hit = [super hitTest:point withEvent:event];
  return hit == self && _passesTouchesThrough ? nil : hit;
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

// The grid and the game's rows slide by transform while the page opens in place, and a frame is
// undefined under a transform: size and centre instead, keeping a scroll view's offset.
void XEPlaceSlidingView(UIView* view, CGRect frame) {
  if (!view) {
    return;
  }
  const CGRect bounds = view.bounds;
  view.bounds = CGRectMake(bounds.origin.x, bounds.origin.y, frame.size.width, frame.size.height);
  view.center = CGPointMake(CGRectGetMidX(frame), CGRectGetMidY(frame));
}
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

// A notice's close button stays at the row's end when its action moves under the text.
constexpr NSInteger kNoticeDismissTag = 0x2E7;

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
    if ([view isKindOfClass:[UIButton class]] && view.tag != kNoticeDismissTag) {
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
    [row insertArrangedSubview:action atIndex:2];
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
  // The action as plain accent text, like a table row's action: a capsule inside the row's own
  // grey made a second box at the row's edge and took the detail's room. The insets keep a
  // 44-point target and end the text as far from the row's edge as the symbol starts.
  UIButtonConfiguration* config = [UIButtonConfiguration plainButtonConfiguration];
  config.title = title;
  config.baseForegroundColor = [XeniaTheme accent];
  config.contentInsets = NSDirectionalEdgeInsetsMake(12.0, 8.0, 12.0, 7.0);
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
      !same_string(a.compatPerf, b.compatPerf) || !same_string(a.compatNotes, b.compatNotes) ||
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
  XeniaLauncherHeaderView* _headerView;
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
  UIView* _zarSuggestionCard;
  UILabel* _zarSuggestionTitle;
  UILabel* _zarSuggestionDetail;
  UIImageView* _memoryStatusIcon;
  UILabel* _memoryStatusLabel;
  // Above the grid: the notices, then search and sort (search and sort move into
  // the header with the side layout).
  UIStackView* _topInfoStack;
  UIStackView* _noticeGroup;
  NSArray<UIView*>* _noticeSeparators;
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
  // Only the box art: the art behind is a blur of it, and the cover itself sits on top as a card
  // at its own size (beside the art), rather than stretched across the page.
  BOOL _heroArtIsCover;
  UIImageView* _heroCover;
  // The game page beside the art: a dimmed blur of the art behind the rows, which the art fades
  // into instead of ending at an edge.
  UIImageView* _ambientArt;
  UIView* _ambientDim;
  // Behind everything: the Library Background, slow colour fields from the shown game's art.
  XeniaLibraryAmbientView* _ambientField;
  // The old game's art and text fading out over the new one; see -crossfadeHeroToGame:.
  UIView* _heroFadeSnapshot;
  // The game page in place of the grid (side layout): the game it is for, its rows, ✕ and ⋯ in
  // the header, and touch layout and settings beside Play.
  XeniaIOSLauncherGameSnapshot* _detailGame;
  // The page is animating closed: upright, its art stays pinned at the top until it has.
  BOOL _detailClosing;
  UIView* _detailRows;
  UIButton* _detailCloseButton;
  UIButton* _detailMoreButton;
  UIButton* _heroTouchButton;
  UIButton* _heroSettingsButton;
  UIButton* _heroCompatButton;
  UIStackView* _heroButtons;  // not retained; _heroText holds it
  // Art on the left and the grid on the right, for windows wider than tall.
  BOOL _sideLayout;
  UIButton* _onlineButton;
  // How far the keyboard covers the bottom of the library.
  CGFloat _keyboardOverlap;
  // The games the grid shows, in order: all of them, sorted, or the search's matches.
  NSArray<XeniaIOSLauncherGameSnapshot*>* _shownGames;
  NSString* _query;
  NSInteger _sortMode;
  UICollectionView* _gamesCollectionView;
  XeniaLauncherEmptyStateActionProxy* _emptyStateProxy;
  NSMutableSet<NSNumber*>* _pendingArtLoads;
  NSMutableSet<NSNumber*>* _pendingIconDecodes;
  BOOL _gamesLoaded;
  NSInteger _focusedGameIndex;
  BOOL _actionsEnabled;
  BOOL _controllerNavigationEnabled;
  BOOL _libraryFocusActive;
}

// A 44 pt glass circle with a white symbol, autoreleased.
- (UIButton*)newGlassCircle:(NSString*)symbol label:(NSString*)label {
  UIButtonConfiguration* config = [UIButtonConfiguration glassButtonConfiguration];
  config.image = [UIImage systemImageNamed:symbol];
  config.baseForegroundColor = UIColor.whiteColor;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.accessibilityLabel = label;
  // A symbol alone: its name shows on hover with a pointer, and on a press and hold at the
  // larger text sizes.
  button.toolTip = label;
  button.showsLargeContentViewer = YES;
  button.largeContentTitle = label;
  button.largeContentImage = [UIImage systemImageNamed:symbol];
  [button addInteraction:[[[UILargeContentViewerInteraction alloc] init] autorelease]];
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:kMinTapTarget],
    [button.heightAnchor constraintEqualToConstant:kMinTapTarget],
  ]];
  return button;
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
  _heroArt.contentMode = UIViewContentModeScaleToFill;  // cropped by layoutHeroArt
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
  _heroCover = [[UIImageView alloc] init];
  _heroCover.contentMode = UIViewContentModeScaleAspectFill;
  _heroCover.clipsToBounds = YES;
  _heroCover.layer.cornerRadius = 10.0;
  _heroCover.layer.cornerCurve = kCACornerCurveContinuous;
  _heroCover.layer.borderWidth = 0.5;
  _heroCover.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.18].CGColor;
  _heroCover.isAccessibilityElement = NO;
  _heroCover.hidden = YES;
  [_heroView addSubview:_heroCover];

  _ambientArt = [[UIImageView alloc] init];
  _ambientArt.contentMode = UIViewContentModeScaleAspectFill;
  _ambientArt.clipsToBounds = YES;
  _ambientArt.isAccessibilityElement = NO;
  _ambientArt.alpha = 0.0;
  _ambientDim = [[UIView alloc] init];
  _ambientDim.backgroundColor = [[XeniaTheme bgPrimary] colorWithAlphaComponent:0.55];
  [_ambientArt addSubview:_ambientDim];
  [self insertSubview:_ambientArt atIndex:0];
  _ambientField = [[XeniaLibraryAmbientView alloc] initWithFrame:self.bounds];
  _ambientField.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self insertSubview:_ambientField atIndex:0];

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
  playCfg.titleLineBreakMode = NSLineBreakByClipping;
  playCfg.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleHeadline, 16.0, UIFontWeightSemibold);
    return updated;
  };
  _heroPlayButton = [[UIButton buttonWithConfiguration:playCfg primaryAction:nil] retain];
  _heroPlayButton.translatesAutoresizingMaskIntoConstraints = NO;
  [_heroPlayButton setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                   forAxis:UILayoutConstraintAxisHorizontal];
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

  _heroTouchButton = [[self newGlassCircle:@"hand.raised"
                                     label:NSLocalizedString(@"Touch Layout",
                                                             @"game page: button")] retain];
  [_heroTouchButton addTarget:self
                       action:@selector(heroTouchLayoutTapped:)
             forControlEvents:UIControlEventTouchUpInside];
  _heroTouchButton.hidden = YES;
  _heroSettingsButton = [[self newGlassCircle:@"slider.horizontal.3"
                                        label:NSLocalizedString(@"Game Settings",
                                                                @"game page: button")] retain];
  [_heroSettingsButton addTarget:self
                          action:@selector(heroGameSettingsTapped:)
                forControlEvents:UIControlEventTouchUpInside];
  _heroSettingsButton.hidden = YES;
  _heroCompatButton = [xe_new_compat_capsule() retain];
  [_heroCompatButton.heightAnchor constraintGreaterThanOrEqualToConstant:kMinTapTarget].active = YES;
  _heroCompatButton.hidden = YES;

  UIStackView* heroButtons = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ _heroPlayButton, _heroMoreButton, _heroTouchButton,
                                  _heroSettingsButton, _heroCompatButton ]] autorelease];
  heroButtons.spacing = 10.0;
  heroButtons.alignment = UIStackViewAlignmentCenter;
  _heroButtons = heroButtons;
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

  _headerView = [[XeniaLauncherHeaderView alloc] init];
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
  // The glass's own shape: a layer corner radius is lost when the + menu folds back into it.
  capsule.cornerConfiguration = [UICornerConfiguration capsuleConfiguration];
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

  // Search, the globe (Public Games, with the players online on XeniOS) and sort.
  _searchRow = [[UIStackView alloc]
      initWithArrangedSubviews:@[ _searchField, xe_ios_new_system_link_globe_button(), _sortButton ]];
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
  _onlineButton = [online retain];
  [online setContentHuggingPriority:UILayoutPriorityRequired
                            forAxis:UILayoutConstraintAxisHorizontal];
  [online.heightAnchor constraintEqualToConstant:44.0].active = YES;
  _headerActions = [[UIStackView alloc] initWithArrangedSubviews:@[ online, capsule ]];
  _headerActions.translatesAutoresizingMaskIntoConstraints = NO;
  _headerActions.alignment = UIStackViewAlignmentCenter;
  _headerActions.spacing = 8.0;
  [_headerView addSubview:_headerActions];

  // The game page's ✕ and ⋯, where Library and the header's actions were.
  _detailCloseButton = [[self newGlassCircle:@"xmark"
                                       label:NSLocalizedString(@"Close", @"sheet close button")]
      retain];
  [_detailCloseButton addTarget:self
                         action:@selector(detailCloseTapped:)
               forControlEvents:UIControlEventTouchUpInside];
  _detailCloseButton.alpha = 0.0;
  _detailCloseButton.hidden = YES;
  [_headerView addSubview:_detailCloseButton];
  _detailMoreButton = [[self newGlassCircle:@"ellipsis"
                                      label:NSLocalizedString(@"More", @"game page: more button")]
      retain];
  _detailMoreButton.showsMenuAsPrimaryAction = YES;
  _detailMoreButton.alpha = 0.0;
  _detailMoreButton.hidden = YES;
  [NSLayoutConstraint activateConstraints:@[
    [_detailCloseButton.leadingAnchor constraintEqualToAnchor:_libraryLabel.leadingAnchor],
    [_detailCloseButton.centerYAnchor constraintEqualToAnchor:_headerView.centerYAnchor],
  ]];
  [self placeDetailMoreButton];

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

  // Shown while converting the library's disc images to ZAR would save real space.
  UIImageView* zarIcon = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"archivebox.fill"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithTextStyle:UIFontTextStyleBody
                                                                       scale:
                                                                           UIImageSymbolScaleMedium]]]
      autorelease];
  zarIcon.tintColor = [XeniaTheme accent];
  _zarSuggestionTitle = [[UILabel alloc] init];
  _zarSuggestionDetail = [[UILabel alloc] init];
  UIButton* zarConvert = xe_new_notice_action(
      NSLocalizedString(@"Convert", @"launcher overlay view: ZAR suggestion button"));
  [zarConvert addTarget:self
                 action:@selector(zarSuggestionTapped:)
       forControlEvents:UIControlEventTouchUpInside];
  _zarSuggestionCard =
      xe_new_notice_card(zarIcon, _zarSuggestionTitle, _zarSuggestionDetail, zarConvert);
  UIButtonConfiguration* dismissCfg = [UIButtonConfiguration plainButtonConfiguration];
  dismissCfg.image = [UIImage
      systemImageNamed:@"xmark"
      withConfiguration:[UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleFootnote
                                                                         scale:
                                                                             UIImageSymbolScaleSmall]];
  dismissCfg.baseForegroundColor = [XeniaTheme textSecondary];
  dismissCfg.contentInsets = NSDirectionalEdgeInsetsMake(8.0, 6.0, 8.0, 6.0);
  UIButton* zarDismiss = [UIButton buttonWithConfiguration:dismissCfg primaryAction:nil];
  zarDismiss.tag = kNoticeDismissTag;
  zarDismiss.accessibilityLabel =
      NSLocalizedString(@"Not Now", @"launcher overlay view: hides the ZAR suggestion");
  [zarDismiss setContentHuggingPriority:UILayoutPriorityRequired
                                forAxis:UILayoutConstraintAxisHorizontal];
  [zarDismiss addTarget:self
                 action:@selector(zarSuggestionDismissTapped:)
       forControlEvents:UIControlEventTouchUpInside];
  [(UIStackView*)_zarSuggestionCard.subviews.firstObject addArrangedSubview:zarDismiss];
  _zarSuggestionCard.hidden = YES;

  NSArray<UIView*>* noticeSeparators = nil;
  _noticeGroup = xe_new_notice_group(@[ _jitWarningCard, _memoryWarningCard, _zarSuggestionCard ],
                                     &noticeSeparators);
  _noticeSeparators = [noticeSeparators retain];

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
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center addObserver:self
             selector:@selector(keyboardWillChangeFrame:)
                 name:UIKeyboardWillChangeFrameNotification
               object:nil];
  [center addObserver:self
             selector:@selector(keyboardDidShow:)
                 name:UIKeyboardDidShowNotification
               object:nil];
  [center addObserver:self
             selector:@selector(libraryBackgroundChanged:)
                 name:XeniaLibraryBackgroundDidChangeNotification
               object:nil];
  [self applyLibraryBackground];
  return self;
}

// The page shows through where the Library Background draws.
- (void)applyLibraryBackground {
  // Like Apple Music: over the colour field the Library is drawn dark (white text) in both modes;
  // the field itself is a little brighter in light mode.
  const BOOL field = _ambientField.drawsBackground;
  self.backgroundColor = field ? UIColor.clearColor : [XeniaTheme bgPrimary];
  self.overrideUserInterfaceStyle =
      field ? UIUserInterfaceStyleDark : UIUserInterfaceStyleUnspecified;
  [self setNeedsLayout];
}

- (void)libraryBackgroundChanged:(NSNotification*)__unused notification {
  [self applyLibraryBackground];
}

- (void)setHidden:(BOOL)hidden {
  [super setHidden:hidden];
  // Hidden while a game runs; the background stops until the Library is back.
  [_ambientField visibilityMayHaveChanged];
  if (!hidden) {
    // Covers may have been dropped under memory pressure while the game ran.
    [self refreshVisibleCoversSoon];
  }
}

// One notice at a time, the one that stops games first: JIT, then the memory limit, and only
// then the suggestion to make the library smaller.
- (void)updateNoticeGroup {
  _jitWarningCard.hidden = _jitAcquired;
  _memoryWarningCard.hidden = _memoryEntitlementEnabled || !_jitAcquired;
  _zarSuggestionCard.hidden = !_zarSuggestionTitle.text.length || !_jitWarningCard.hidden ||
                              !_memoryWarningCard.hidden;
  for (UIView* separator in _noticeSeparators) {
    separator.hidden = YES;
  }
  _noticeGroup.hidden =
      _jitWarningCard.hidden && _memoryWarningCard.hidden && _zarSuggestionCard.hidden;
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
  [self updateSearchPlaceholder];
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

// The art fills the hero pinned to its left edge, so a narrow hero crops the
// right of a wide image rather than both sides (the left is where the subject
// usually stands and where the title reads from).
- (void)layoutHeroArt {
  const CGRect bounds = _heroView.bounds;
  _heroArt.frame = bounds;
  const CGSize image_size = _heroArt.image.size;
  CGRect visible = CGRectMake(0.0, 0.0, 1.0, 1.0);
  if (image_size.width > 0.0 && image_size.height > 0.0 && !CGRectIsEmpty(bounds)) {
    const CGFloat scale =
        MAX(bounds.size.width / image_size.width, bounds.size.height / image_size.height);
    visible.size.width = MIN(1.0, bounds.size.width / (image_size.width * scale));
    visible.size.height = MIN(1.0, bounds.size.height / (image_size.height * scale));
    visible.origin.y = (1.0 - visible.size.height) * 0.5;
  }
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  _heroArt.layer.contentsRect = visible;
  [CATransaction commit];
}

// Box art as a card at the top of the art column, above the title, when there is room for it.
// Upright, the blur alone carries the colour and the title stands on it.
- (void)layoutHeroCover {
  BOOL show = _heroArtIsCover && _sideLayout && !_heroView.hidden;
  if (show) {
    // The text's own frame, after a longer title wraps, not the one from the last pass.
    [_heroView layoutIfNeeded];
    const CGFloat top = CGRectGetMaxY(_headerView.frame) + 16.0;
    const CGFloat bottom = CGRectGetMinY(_heroText.frame) - 18.0;
    const CGFloat max_width = CGRectGetWidth(_heroView.bounds) - 2.0 * _heroTextLeading.constant;
    CGFloat height = MIN(bottom - top, 260.0);
    CGFloat width = floor(height * 219.0 / 300.0);
    if (width > max_width) {
      width = max_width;
      height = floor(width * 300.0 / 219.0);
    }
    show = height >= 96.0;
    if (show) {
      _heroCover.frame = CGRectMake(_heroTextLeading.constant, top, width, height);
    }
  }
  _heroCover.hidden = !show;
}

// Whether Play, the round buttons and the capsule fit across the page in one row.
- (BOOL)heroButtonRowFits {
  if (_heroCompatButton.hidden) {
    return YES;
  }
  CGFloat width = 0.0;
  NSUInteger count = 0;
  for (UIView* view in @[ _heroPlayButton, _heroMoreButton, _heroTouchButton, _heroSettingsButton,
                          _heroCompatButton ]) {
    if (view.hidden) {
      continue;
    }
    width += [view systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].width;
    ++count;
  }
  width += _heroButtons.spacing * (count > 0 ? count - 1 : 0);
  return width <= CGRectGetWidth(self.bounds) - 2.0 * _heroTextLeading.constant;
}

// Upright, the compatibility capsule ends the row of buttons. Beside the art, or when larger text
// leaves no room for it, it sits on its own line above them.
- (void)placeHeroCompatButton {
  const BOOL own_line = _sideLayout || ![self heroButtonRowFits];
  UIView* row = own_line ? _heroText : _heroButtons;
  if (_heroCompatButton.superview == row) {
    return;
  }
  [_heroCompatButton removeFromSuperview];
  if (own_line) {
    [_heroText insertArrangedSubview:_heroCompatButton
                             atIndex:[_heroText.arrangedSubviews indexOfObject:_heroButtons]];
    [_heroText setCustomSpacing:12.0 afterView:_heroCompatButton];
  } else {
    [_heroButtons addArrangedSubview:_heroCompatButton];
  }
}

// Upright, the game page's ⋯ sits in the header's corner, across from ✕. Beside the art it joins
// Play and the round buttons, and the rows start at the top, level with ✕, where it was.
- (void)placeDetailMoreButton {
  if (_sideLayout) {
    if (_detailMoreButton.superview == _heroButtons) {
      return;
    }
    [_detailMoreButton removeFromSuperview];
    const NSUInteger after = [_heroButtons.arrangedSubviews indexOfObject:_heroSettingsButton];
    [_heroButtons insertArrangedSubview:_detailMoreButton
                                atIndex:after == NSNotFound ? _heroButtons.arrangedSubviews.count
                                                            : after + 1];
    return;
  }
  if (_detailMoreButton.superview == _headerView) {
    return;
  }
  [_detailMoreButton removeFromSuperview];
  [_headerView addSubview:_detailMoreButton];
  [NSLayoutConstraint activateConstraints:@[
    [_detailMoreButton.trailingAnchor constraintEqualToAnchor:_headerActions.trailingAnchor],
    [_detailMoreButton.centerYAnchor constraintEqualToAnchor:_headerView.centerYAnchor],
  ]];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGRect bounds = self.bounds;
  const UIEdgeInsets safe = self.safeAreaInsets;
  const BOOL side = bounds.size.width > bounds.size.height;
  if (side != _sideLayout) {
    _sideLayout = side;
    [self placeSearchRow];
    [self placeHeroCompatButton];
    [self placeDetailMoreButton];
    [self updateHeaderFonts];
    [_gamesCollectionView.collectionViewLayout invalidateLayout];
    // Rotating brings other tiles on screen while covers may be landing.
    [self refreshVisibleCoversSoon];
  }
  const BOOL hero = _heroGame != nil || _detailGame != nil || _detailClosing;
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
    // The game's rows start at the top, level with ✕; the header has nothing else on that side.
    // Their list insets its first row past the fade at its top edge.
    XEPlaceSlidingView(_detailRows,
                       CGRectMake(grid_left, header_top - 8.0, bounds.size.width - grid_left,
                                  bounds.size.height - header_top + 8.0));
    top_inset = header_bottom + 6.0 + stack_block;
    _headerBackdrop.frame = CGRectMake(grid_left, 0.0, bounds.size.width - grid_left,
                                       header_bottom + 6.0);
    // Covers fade out as they reach the header instead of sliding under its controls or a bar.
    CAGradientLayer* fade = [CAGradientLayer layer];
    fade.frame = CGRectMake(0.0, 0.0, grid_frame.size.width, grid_frame.size.height);
    const CGFloat clear_to = (header_bottom + 2.0) / MAX(grid_frame.size.height, 1.0);
    const CGFloat solid_from = (header_bottom + 22.0) / MAX(grid_frame.size.height, 1.0);
    fade.colors = @[
      (id)[UIColor clearColor].CGColor, (id)[UIColor clearColor].CGColor,
      (id)[UIColor blackColor].CGColor, (id)[UIColor blackColor].CGColor
    ];
    fade.locations = @[ @0.0, @(clear_to), @(solid_from), @1.0 ];
    _gamesCollectionView.maskView = nil;
    UIView* mask = [[[UIView alloc] initWithFrame:fade.frame] autorelease];
    [mask.layer addSublayer:fade];
    _gamesCollectionView.maskView = mask;
  } else {
    // Art across the top, under the header, scrolling away with the grid.
    _gamesCollectionView.maskView = nil;
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
    // The game's rows fill the page under its art.
    XEPlaceSlidingView(_detailRows, CGRectMake(0.0, hero_height, bounds.size.width,
                                               bounds.size.height - hero_height));
  }
  XEPlaceSlidingView(_gamesCollectionView, grid_frame);
  if (hero && !side && (_detailGame || _detailClosing)) {
    // Upright, a game's page holds its art still at the top while the rows scroll under it.
    if (_heroView.superview != self) {
      [_heroView removeFromSuperview];
      [self insertSubview:_heroView belowSubview:_gamesCollectionView];
    }
    _heroView.frame = CGRectMake(0.0, 0.0, grid_frame.size.width, hero_height);
  } else if (hero && !side) {
    if (_heroView.superview != _gamesCollectionView) {
      [_heroView removeFromSuperview];
      [_gamesCollectionView insertSubview:_heroView atIndex:0];
    }
    _heroView.frame = CGRectMake(0.0, -top_inset, grid_frame.size.width, hero_height);
  }
  [self layoutHeroArt];
  [self updateFieldAnchor];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  const CGRect hero_bounds = _heroView.bounds;
  _heroTopScrim.frame = CGRectMake(0.0, 0.0, hero_bounds.size.width, 150.0);
  _heroScrim.frame = hero_bounds;
  UIColor* page = [[XeniaTheme bgPrimary] resolvedColorWithTraitCollection:self.traitCollection];
  // Beside the game's page the art dissolves into a dimmed blur of itself behind the rows.
  const BOOL ambient = side && hero && (_detailGame != nil) && _ambientArt.image != nil;
  _ambientArt.frame = bounds;
  _ambientDim.frame = _ambientArt.bounds;
  _ambientArt.alpha = ambient ? 1.0 : 0.0;
  // The dissolve masks the art and its scrims, not the hero, so the title and buttons stay whole.
  // Each mask is in its own layer's space: solid to 55% of the hero's width, clear at its edge.
  const BOOL field = _ambientField.drawsBackground;
  const BOOL dark_page = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark;
  CAGradientLayer* (^dissolve_for)(CALayer*) = ^CAGradientLayer*(CALayer* layer) {
    const CGRect frame = [_heroView.layer convertRect:layer.bounds fromLayer:layer];
    const CGFloat width = MAX(frame.size.width, 1.0);
    CAGradientLayer* dissolve = [CAGradientLayer layer];
    dissolve.frame = layer.bounds;
    dissolve.colors = @[ (id)UIColor.blackColor.CGColor, (id)UIColor.blackColor.CGColor,
                         (id)UIColor.clearColor.CGColor, (id)UIColor.clearColor.CGColor ];
    // Over a light background the dark art fades over a shorter run, which would otherwise read
    // as a grey haze.
    const CGFloat solid = field && !dark_page ? 0.75 : 0.55;
    const CGFloat solid_to = (hero_bounds.size.width * solid - frame.origin.x) / width;
    const CGFloat clear_at = (hero_bounds.size.width - frame.origin.x) / width;
    dissolve.locations = @[ @0.0, @(MAX(solid_to, 0.0)), @(MAX(clear_at, 0.0)), @1.0 ];
    dissolve.startPoint = CGPointMake(0.0, 0.5);
    dissolve.endPoint = CGPointMake(1.0, 0.5);
    return dissolve;
  };
  // Over the Library Background the art dissolves into it at the bottom (or, beside the grid,
  // at its right) instead of ending in the page's colour.
  CAGradientLayer* (^fade_down)(CALayer*) = ^CAGradientLayer*(CALayer* layer) {
    const CGRect frame = [_heroView.layer convertRect:layer.bounds fromLayer:layer];
    const CGFloat height = MAX(frame.size.height, 1.0);
    CAGradientLayer* dissolve = [CAGradientLayer layer];
    dissolve.frame = layer.bounds;
    dissolve.colors = @[ (id)UIColor.blackColor.CGColor, (id)UIColor.blackColor.CGColor,
                         (id)UIColor.clearColor.CGColor, (id)UIColor.clearColor.CGColor ];
    const CGFloat solid_to =
        (hero_bounds.size.height * (dark_page ? 0.6 : 0.75) - frame.origin.y) / height;
    const CGFloat clear_at = (hero_bounds.size.height - frame.origin.y) / height;
    dissolve.locations = @[ @0.0, @(MAX(solid_to, 0.0)), @(MAX(clear_at, 0.0)), @1.0 ];
    dissolve.startPoint = CGPointMake(0.5, 0.0);
    dissolve.endPoint = CGPointMake(0.5, 1.0);
    return dissolve;
  };
  _heroView.layer.mask = nil;
  // A cover's blur behind the hero: over the Library Background the field draws it, lined up
  // and toned like the rest of the page, so the page reads as one piece from top to bottom.
  _heroArt.alpha = field && _heroArtIsCover && !ambient ? 0.0 : 1.0;
  if (ambient || (field && side)) {
    _heroArt.layer.mask = dissolve_for(_heroArt.layer);
    _heroScrim.mask = dissolve_for(_heroScrim);
    _heroTopScrim.mask = dissolve_for(_heroTopScrim);
    _heroView.backgroundColor = UIColor.clearColor;
  } else if (field) {
    _heroArt.layer.mask = fade_down(_heroArt.layer);
    _heroScrim.mask = fade_down(_heroScrim);
    _heroTopScrim.mask = nil;
    _heroView.backgroundColor = UIColor.clearColor;
  } else {
    _heroArt.layer.mask = nil;
    _heroScrim.mask = nil;
    _heroTopScrim.mask = nil;
    _heroView.backgroundColor = [XeniaTheme bgPrimary];
  }
  if (field && !ambient) {
    // Over the Library Background: a dark shade toward the white text at the bottom only. Fading
    // to the page colour would haze the art where it meets the field; the mask does that edge.
    UIColor* shade = UIColor.blackColor;
    _heroScrim.colors = @[
      (id)[shade colorWithAlphaComponent:0.0].CGColor,
      (id)[shade colorWithAlphaComponent:0.0].CGColor,
      (id)[shade colorWithAlphaComponent:0.45].CGColor
    ];
    _heroScrim.locations = @[ @0.0, @0.45, @1.0 ];
    _heroScrim.startPoint = CGPointMake(0.5, 0.0);
    _heroScrim.endPoint = CGPointMake(0.5, 1.0);
  } else if (side && ambient) {
    // Darkens toward the text at the bottom only; the right edge is the mask's.
    _heroScrim.colors = @[
      (id)[page colorWithAlphaComponent:0.0].CGColor, (id)[page colorWithAlphaComponent:0.0].CGColor,
      (id)[page colorWithAlphaComponent:0.5].CGColor
    ];
    _heroScrim.locations = @[ @0.0, @0.45, @1.0 ];
    _heroScrim.startPoint = CGPointMake(0.5, 0.0);
    _heroScrim.endPoint = CGPointMake(0.5, 1.0);
  } else if (side) {
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
  [self layoutHeroCover];

  const CGFloat bottom = MAX(side ? 0.0 : safe.bottom, _keyboardOverlap) + 16.0;
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
  if (_sideLayout || _detailGame) {
    alpha = 0.0;
    // A scroll view's mask moves with its content; keep the fade pinned under the header.
    UIView* mask = _gamesCollectionView.maskView;
    if (mask) {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];
      mask.frame = CGRectMake(0.0, _gamesCollectionView.contentOffset.y,
                              CGRectGetWidth(mask.bounds), CGRectGetHeight(mask.bounds));
      [CATransaction commit];
    }
  } else {
    const CGFloat header_bottom = CGRectGetMaxY(_headerView.frame);
    const CGFloat content_top = -_gamesCollectionView.contentInset.top;
    const CGFloat under = _heroGame ? (CGRectGetHeight(_heroView.frame) - header_bottom) : 0.0;
    const CGFloat scrolled = _gamesCollectionView.contentOffset.y - content_top - under;
    alpha = MIN(MAX(scrolled / 24.0, 0.0), 1.0);
  }
  _headerBackdrop.alpha = alpha;
  _libraryLabel.layer.shadowOpacity = (alpha > 0.5 && !_sideLayout) ? 0.0f : 0.35f;
  // Over the art the title is white, as the hero's own text is; on the bar it takes the theme's.
  const BOOL over_art = !_heroView.hidden && alpha <= 0.5;
  _libraryLabel.textColor = over_art ? UIColor.whiteColor : [XeniaTheme textPrimary];
  if (over_art != _headerOverArt) {
    _headerOverArt = over_art;
    if (_headerOverArtChangedHandler) {
      _headerOverArtChangedHandler();
    }
  }
}

- (void)scrollViewDidScroll:(UIScrollView*)scrollView {
  if (scrollView == _gamesCollectionView) {
    [self updateHeaderBackdrop];
    [self updateFieldAnchor];
  }
}

// Tells the Library Background where the hero's art is, so it lines a blurred copy up with it.
- (void)updateFieldAnchor {
  BOOL shown = _heroArt.image && _heroView.superview && _ambientField.superview;
  // From the hero up: the art itself may be hidden while the field draws it (a cover's blur).
  for (UIView* view = _heroView; shown && view && view != self; view = view.superview) {
    shown = !view.hidden && view.alpha > 0.01;
  }
  [_ambientField
      setAnchorFrame:shown ? [_heroArt convertRect:_heroArt.bounds toView:_ambientField]
                           : CGRectNull
        contentsRect:_heroArt.layer.contentsRect];
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
  if (_detailGame) {
    // The page in place keeps its game; the hero comes back to this one when it closes.
    return;
  }
  [self applyHeroGame:latest];
}

// The hero's art: wide background art fills it; box art (|is_cover|) is shown as a card over a
// blur of itself, since it is too small to fill the page sharply.
- (void)setHeroImage:(UIImage*)image isCover:(BOOL)is_cover {
  _heroArtIsCover = image && is_cover;
  UIImage* blurred = XEBlurredBackdropImage(image);
  _heroArt.image = _heroArtIsCover ? blurred : image;
  _heroCover.image = _heroArtIsCover ? image : nil;
  _ambientArt.image = blurred;
  [_ambientField setGameImage:image
                          key:image ? [NSString stringWithFormat:@"%08X-%d", _heroArtTitleId,
                                                                 _heroArtIsCover]
                                    : nil];
  [self layoutHeroArt];
  [self setNeedsLayout];
}

// The hero's title, line under it and art, for the last game played or the open page's game.
- (void)applyHeroGame:(XeniaIOSLauncherGameSnapshot*)latest {
  if (!latest) {
    _heroArtTitleId = 0;
    [self setHeroImage:nil isCover:NO];
    return;
  }
  _heroTitle.text = latest.title;
  NSRelativeDateTimeFormatter* formatter = [[[NSRelativeDateTimeFormatter alloc] init] autorelease];
  formatter.unitsStyle = NSRelativeDateTimeFormatterUnitsStyleFull;
  // Under a minute (or a clock a little ahead) would read "in 0 seconds".
  NSDate* now = [NSDate date];
  NSString* when =
      !latest.lastPlayed ? nil
      : [now timeIntervalSinceDate:latest.lastPlayed] < 60.0
          ? NSLocalizedString(@"Played just now", @"launcher overlay view: Continue Playing")
          : [NSString stringWithFormat:NSLocalizedString(@"Played %@",
                                                         @"launcher overlay view: Continue "
                                                         @"Playing; %@ is like \"2 hours ago\""),
                                       [formatter localizedStringForDate:latest.lastPlayed
                                                          relativeToDate:now]];
  NSString* compat = latest.hasCompatInfo ? xe_compat_status_label(latest.compatStatus) : nil;
  _heroDetail.text = when && compat ? [NSString stringWithFormat:@"%@ · %@", when, compat]
                                    : (when ?: compat);
  _heroPlayButton.accessibilityLabel = [NSString
      stringWithFormat:NSLocalizedString(@"Play %@", @"launcher overlay view: accessibility "
                                                     @"label; %@ is the game"),
                       latest.title];
  if (_heroArtTitleId != latest.titleId) {
    _heroArtTitleId = latest.titleId;
    // The wide background art, else the cover, else the game's own art.
    UIImage* background = xe_cached_game_background_art(latest.titleId);
    [self setHeroImage:background ?: (xe_cached_game_art(latest.titleId) ?: latest.decodedIcon)
               isCover:background == nil];
    if (latest.supportsRemoteArt && !background) {
      const uint32_t title_id = latest.titleId;
      xe_fetch_game_background_art(title_id, ^(UIImage* image) {
        if (image && self->_heroArtTitleId == title_id) {
          [self setHeroImage:image isCover:NO];
        } else if (!image && self->_heroArtTitleId == title_id && !self->_heroArt.image) {
          xe_fetch_game_art(title_id, ^(UIImage* cover) {
            if (cover && self->_heroArtTitleId == title_id && !self->_heroArt.image) {
              [self setHeroImage:cover isCover:YES];
            }
          });
        }
      });
    }
  }
  [self setNeedsLayout];
}

// The game the hero shows: the open page's, else the last one played.
- (NSUInteger)heroGameIndex {
  XeniaIOSLauncherGameSnapshot* game = _detailGame ?: _heroGame;
  return game ? [_games indexOfObjectIdenticalTo:game] : NSNotFound;
}

- (void)heroTouchLayoutTapped:(UIButton*)__unused sender {
  const NSUInteger index = [self heroGameIndex];
  if (index != NSNotFound && _touchLayoutHandler) {
    _touchLayoutHandler(index);
  }
}

- (void)heroGameSettingsTapped:(UIButton*)__unused sender {
  const NSUInteger index = [self heroGameIndex];
  if (index != NSNotFound && _gameSettingsHandler) {
    _gameSettingsHandler(index);
  }
}

- (void)detailCloseTapped:(UIButton*)__unused sender {
  [self hideGameDetailAnimated:YES];
}

#pragma mark - Game page in place

// Phones, either way up; an iPad keeps the page as a sheet.
- (BOOL)canShowGameDetailInPlace {
  const CGSize size = self.bounds.size;
  return self.window && MIN(size.width, size.height) < 500.0;
}

- (BOOL)showsGameDetail {
  return _detailGame != nil;
}

// What changes between the Library and a game's page; everything else stays where it is.
- (void)applyDetailState:(BOOL)detail {
  _heroCaption.hidden = detail || !_heroGame;
  _heroDetail.hidden = detail;
  _heroCaption.alpha = _heroCaption.hidden ? 0.0 : 1.0;
  _heroDetail.alpha = detail ? 0.0 : 1.0;
  _heroMoreButton.hidden = detail;
  _heroMoreButton.alpha = detail ? 0.0 : 1.0;
  const BOOL game_buttons = detail && _detailGame.titleId != 0;
  _heroTouchButton.hidden = !game_buttons;
  _heroSettingsButton.hidden = !game_buttons;
  _heroTouchButton.alpha = game_buttons ? 1.0 : 0.0;
  _heroSettingsButton.alpha = game_buttons ? 1.0 : 0.0;
  const BOOL compat = game_buttons && _detailGame.supportsCompatibility;
  _heroCompatButton.hidden = !compat;
  _heroCompatButton.alpha = compat ? 1.0 : 0.0;
  [self placeHeroCompatButton];
  // Title straight onto the buttons once the lines between them have gone.
  [_heroText setCustomSpacing:detail ? 16.0 : UIStackViewSpacingUseDefault afterView:_heroTitle];

  _libraryLabel.alpha = detail ? 0.0 : 1.0;
  _headerActions.alpha = detail ? 0.0 : 1.0;
  _headerActions.transform = detail ? CGAffineTransformMakeTranslation(-24.0, 0.0)
                                    : CGAffineTransformIdentity;
  _headerActions.userInteractionEnabled = !detail;
  _headerView.passesTouchesThrough = detail;
  _detailCloseButton.alpha = detail ? 1.0 : 0.0;
  _detailCloseButton.transform =
      detail ? CGAffineTransformIdentity : CGAffineTransformMakeScale(0.8, 0.8);
  const BOOL more = detail && _detailMoreButton.menu.children.count > 0;
  _detailMoreButton.alpha = more ? 1.0 : 0.0;
  _detailMoreButton.transform =
      more ? CGAffineTransformIdentity : CGAffineTransformMakeScale(0.8, 0.8);

  // The grid gives way toward the art and the rows come in from the far side: from the right
  // beside the art, from below under it.
  _gamesCollectionView.alpha = detail ? 0.0 : 1.0;
  _gamesCollectionView.transform = !detail       ? CGAffineTransformIdentity
                                   : _sideLayout ? CGAffineTransformMakeTranslation(-40.0, 0.0)
                                                 : CGAffineTransformMakeTranslation(0.0, 40.0);
  _detailRows.alpha = detail ? 1.0 : 0.0;
  _detailRows.transform = detail ? CGAffineTransformIdentity : [self detailRowsAwayTransform];
}

- (CGAffineTransform)detailRowsAwayTransform {
  return _sideLayout ? CGAffineTransformMakeTranslation(56.0, 0.0)
                     : CGAffineTransformMakeTranslation(0.0, 56.0);
}

- (void)showGameDetailForGameIndex:(NSUInteger)gameIndex
                          rowsView:(UIView*)rowsView
                          moreMenu:(UIMenu*)moreMenu {
  if (gameIndex >= _games.count || !rowsView) {
    return;
  }
  XeniaIOSLauncherGameSnapshot* game = _games[gameIndex];
  if (_detailRows && _detailRows != rowsView) {
    [_detailRows removeFromSuperview];
    [_detailRows release];
    _detailRows = nil;
  }
  // Upright with the grid scrolled past the art, the art isn't on screen to turn into the page;
  // it fades in at the top instead.
  const BOOL hero_on_screen =
      _sideLayout || !_heroGame ||
      CGRectGetMaxY([_heroView convertRect:_heroView.bounds toView:self]) > 120.0;
  const BOOL had_hero = _heroGame != nil && hero_on_screen;
  const BOOL other_game = _detailGame ? _detailGame != game : _heroGame != game;
  [_detailGame release];
  _detailGame = [game retain];
  _detailRows = [rowsView retain];
  [self insertSubview:_detailRows aboveSubview:_gamesCollectionView];
  __unsafe_unretained XeniaIOSLauncherOverlayView* unsafe_self = self;
  xe_update_compat_capsule(_heroCompatButton, game.hasCompatInfo ? game.compatStatus : nil,
                           game.compatPerf, game.compatNotes, ^{
                             const NSUInteger index = [unsafe_self heroGameIndex];
                             if (index != NSNotFound && unsafe_self->_compatibilityHandler) {
                               unsafe_self->_compatibilityHandler(index);
                             }
                           });
  _detailMoreButton.menu = moreMenu;
  _detailCloseButton.hidden = NO;
  _detailMoreButton.hidden = moreMenu.children.count == 0;

  // Start from where the Library is, with the rows waiting off to the right.
  [UIView performWithoutAnimation:^{
    [self applyDetailState:NO];
    if (!had_hero) {
      // No art yet: it fades in with this game rather than sliding in.
      [self applyHeroGame:game];
      _heroView.alpha = 0.0;
    }
    [self setNeedsLayout];
    [self layoutIfNeeded];
  }];
  if (other_game && had_hero) {
    // The art and title turn to this game in place.
    [self crossfadeHeroToGame:game];
  }
  // The grid leaves first, so the rows never come in over it.
  [UIView animateWithDuration:0.16
                        delay:0.0
                      options:UIViewAnimationOptionCurveEaseIn |
                              UIViewAnimationOptionBeginFromCurrentState
                   animations:^{
                     _gamesCollectionView.alpha = 0.0;
                   }
                   completion:nil];
  UIViewPropertyAnimator* animator =
      [[[UIViewPropertyAnimator alloc] initWithDuration:0.5
                                           dampingRatio:0.88
                                             animations:^{
                                               [self applyDetailState:YES];
                                               _heroView.alpha = 1.0;
                                               [_heroText layoutIfNeeded];
                                               [self layoutIfNeeded];
                                             }] autorelease];
  [animator addCompletion:^(UIViewAnimatingPosition position) {
    if (position == UIViewAnimatingPositionEnd && _detailGame) {
      _gamesCollectionView.hidden = YES;
      UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, _heroTitle);
    }
  }];
  [animator startAnimationAfterDelay:0.06];
}

- (void)hideGameDetailAnimated:(BOOL)animated {
  if (!_detailGame) {
    return;
  }
  XeniaIOSLauncherGameSnapshot* shown = [[_detailGame retain] autorelease];
  [_detailGame release];
  _detailGame = nil;
  UIView* rows = [[_detailRows retain] autorelease];
  [_detailRows release];
  _detailRows = nil;
  _gamesCollectionView.hidden = NO;
  _detailClosing = YES;
  // Upright with the grid scrolled past where the art sits in it, the art fades out at the top
  // rather than dropping back into the grid off screen.
  const BOOL art_returns_on_screen =
      _sideLayout || _gamesCollectionView.contentOffset.y <=
                         -_gamesCollectionView.contentInset.top + 1.0;
  void (^finish)(void) = ^{
    // Upright, the art goes back into the grid, in the place it now covers.
    _detailClosing = NO;
    [UIView performWithoutAnimation:^{
      [self setNeedsLayout];
      [self layoutIfNeeded];
    }];
    [rows removeFromSuperview];
    _detailCloseButton.hidden = YES;
    _detailMoreButton.hidden = YES;
    _detailMoreButton.menu = nil;
    if (_gameDetailClosedHandler) {
      _gameDetailClosedHandler();
    }
  };
  // The rows keep sliding with the state below until they are gone.
  UIView* detail_rows = rows;
  void (^back)(void) = ^{
    [self applyDetailState:NO];
    detail_rows.alpha = 0.0;
    detail_rows.transform = [self detailRowsAwayTransform];
    if (!_heroGame || !art_returns_on_screen) {
      _heroView.alpha = 0.0;
    }
    [self setNeedsLayout];
    [_heroText layoutIfNeeded];
    [self layoutIfNeeded];
  };
  if (!animated) {
    back();
    _heroView.alpha = 1.0;
    [self applyHeroGame:_heroGame];
    finish();
    return;
  }
  if (_heroGame && _heroGame != shown) {
    [self crossfadeHeroToGame:_heroGame];
  }
  // The rows leave first, so they never sit over the grid coming back.
  [UIView animateWithDuration:0.16
                        delay:0.0
                      options:UIViewAnimationOptionCurveEaseIn |
                              UIViewAnimationOptionBeginFromCurrentState
                   animations:^{
                     detail_rows.alpha = 0.0;
                     detail_rows.transform = [self detailRowsAwayTransform];
                   }
                   completion:nil];
  UIViewPropertyAnimator* animator =
      [[[UIViewPropertyAnimator alloc] initWithDuration:0.45 dampingRatio:0.92 animations:back]
          autorelease];
  [animator addCompletion:^(UIViewAnimatingPosition __unused position) {
    _heroView.alpha = 1.0;
    if (!_heroGame) {
      [self applyHeroGame:nil];
    }
    finish();
    UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, _libraryLabel);
  }];
  [animator startAnimationAfterDelay:0.08];
}

// The art and text turn to another game in place. A snapshot of the old ones fades out over the
// new: a cross-dissolve transition on the hero itself froze half-way when layout swapped the
// hero's mask and scrim during it, leaving both games' text on screen. The snapshot is removed
// when the fade ends, by the next fade, or after a deadline, whichever comes first.
- (void)crossfadeHeroToGame:(XeniaIOSLauncherGameSnapshot*)game {
  [self removeHeroFadeSnapshot];
  UIView* snapshot = _heroView.window ? [_heroView snapshotViewAfterScreenUpdates:NO] : nil;
  if (snapshot) {
    snapshot.frame = _heroView.frame;
    snapshot.userInteractionEnabled = NO;
    [_heroView.superview insertSubview:snapshot aboveSubview:_heroView];
    _heroFadeSnapshot = [snapshot retain];
  }
  [self applyHeroGame:game];
  if (!snapshot) {
    return;
  }
  [UIView animateWithDuration:0.3
      delay:0.0
      options:UIViewAnimationOptionAllowUserInteraction | UIViewAnimationOptionCurveEaseInOut
      animations:^{
        snapshot.alpha = 0.0;
      }
      completion:^(BOOL __unused finished) {
        if (_heroFadeSnapshot == snapshot) {
          [self removeHeroFadeSnapshot];
        }
      }];
  [snapshot retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.6 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (_heroFadeSnapshot == snapshot) {
                     [self removeHeroFadeSnapshot];
                   }
                   [snapshot release];
                 });
}

- (void)removeHeroFadeSnapshot {
  [_heroFadeSnapshot.layer removeAllAnimations];
  [_heroFadeSnapshot removeFromSuperview];
  [_heroFadeSnapshot release];
  _heroFadeSnapshot = nil;
}

- (void)heroPlayTapped:(UIButton*)__unused sender {
  const NSUInteger index = [self heroGameIndex];
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

#pragma mark - Keyboard

- (void)keyboardWillChangeFrame:(NSNotification*)notification {
  if (!self.window) {
    return;
  }
  const CGRect frame =
      [self convertRect:[notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue]
               fromView:nil];
  const CGFloat overlap = MAX(0.0, CGRectGetMaxY(self.bounds) - CGRectGetMinY(frame));
  if (overlap == _keyboardOverlap) {
    return;
  }
  _keyboardOverlap = overlap;
  [self setNeedsLayout];
  [self layoutIfNeeded];
  if (overlap == 0.0) {
    // Back inside the list's range: typing can leave it scrolled past either end.
    UIScrollView* grid = _gamesCollectionView;
    const CGFloat min_y = -grid.contentInset.top;
    const CGFloat max_y = MAX(min_y, grid.contentSize.height + grid.contentInset.bottom -
                                         CGRectGetHeight(grid.bounds));
    const CGFloat y = MIN(MAX(grid.contentOffset.y, min_y), max_y);
    if (y != grid.contentOffset.y) {
      [grid setContentOffset:CGPointMake(grid.contentOffset.x, y) animated:YES];
    }
  }
}

// Searching upright, the search row moves up under the header so the matches show above the keys.
- (void)keyboardDidShow:(NSNotification*)__unused notification {
  if (!_searchField.isFirstResponder || _searchRow.superview != _topInfoStack) {
    return;
  }
  UIScrollView* grid = _gamesCollectionView;
  const CGFloat row_top =
      CGRectGetMinY([_searchRow convertRect:_searchRow.bounds toView:grid]);
  const CGFloat header_bottom = CGRectGetMaxY(_headerView.frame) - CGRectGetMinY(grid.frame);
  const CGFloat y = MAX(row_top - header_bottom - 8.0, -grid.contentInset.top);
  // After UIKit's own keyboard adjustment, which runs once this notification has been handled.
  dispatch_async(dispatch_get_main_queue(), ^{
    [grid setContentOffset:CGPointMake(grid.contentOffset.x, y) animated:YES];
  });
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
  // The header's field is narrow beside the title; the count would be cut off at larger sizes.
  _searchField.placeholder =
      _sideLayout ? NSLocalizedString(@"Search", @"launcher overlay view: search placeholder")
      : _games.count == 1
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
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  _emptyStateProxy.owner = nil;
  [_emptyStateProxy release];
  [_pendingArtLoads release];
  [_pendingIconDecodes release];
  [_openStikDebugHandler release];
  [_headerOverArtChangedHandler release];
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
  [self removeHeroFadeSnapshot];
  [_heroScrim release];
  [_heroCover release];
  [_ambientArt release];
  [_ambientDim release];
  [_ambientField release];
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
  [_detailGame release];
  [_detailRows release];
  [_onlineButton release];
  [_detailCloseButton release];
  [_detailMoreButton release];
  [_heroTouchButton release];
  [_heroSettingsButton release];
  [_heroCompatButton release];
  [_gameDetailClosedHandler release];
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
  [_zarSuggestionCard release];
  [_zarSuggestionTitle release];
  [_zarSuggestionDetail release];
  [_zarSuggestionHandler release];
  [_zarSuggestionDismissHandler release];
  [_memoryStatusIcon release];
  [_memoryStatusLabel release];
  [_libraryLabel release];
  [_topInfoStack release];
  [_noticeGroup release];
  [_noticeSeparators release];
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

- (void)setZarSuggestionTitle:(NSString*)title detail:(NSString*)detail {
  _zarSuggestionTitle.text = title;
  _zarSuggestionDetail.text = detail;
  _zarSuggestionCard.accessibilityLabel = title;
  [self updateNoticeGroup];
}

- (void)zarSuggestionTapped:(UIButton*)__unused sender {
  if (_zarSuggestionHandler) {
    _zarSuggestionHandler();
  }
}

- (void)zarSuggestionDismissTapped:(UIButton*)__unused sender {
  [UIView animateWithDuration:0.25
                   animations:^{
                     [self setZarSuggestionTitle:nil detail:nil];
                     [self layoutIfNeeded];
                   }];
  if (_zarSuggestionDismissHandler) {
    _zarSuggestionDismissHandler();
  }
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
  // Every focusable control is a capsule or a circle.
  button.layer.cornerRadius = CGRectGetHeight(button.bounds) / 2.0;
  button.layer.borderWidth = focused ? 2.0 : 0.0;
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
  if (CGRectGetHeight(button.bounds) > 0.0) {
    button.layer.cornerRadius = CGRectGetHeight(button.bounds) / 2.0;
  }
  button.layer.borderWidth = 0.0f;
  button.layer.borderColor = [UIColor clearColor].CGColor;
  button.layer.shadowOpacity = 0.0f;
  button.layer.shadowRadius = 0.0f;
  button.layer.shadowOffset = CGSizeZero;
}

// The views a controller can focus besides the games, with the ring drawn round each one's shape.
- (NSArray<UIView*>*)controllerFocusViews {
  NSMutableArray<UIView*>* views = [NSMutableArray array];
  for (UIView* view in @[ _searchField, _sortButton, _onlineButton, _openGameButton,
                          _settingsButton, _profileButton, _heroPlayButton, _heroMoreButton ]) {
    if (view) {
      [views addObject:view];
    }
  }
  return views;
}

- (UIView*)viewForControllerFocusTarget:(XeniaIOSLauncherFocusTarget)target {
  switch (target) {
    case XeniaIOSLauncherFocusTargetSearch:
      return _searchField;
    case XeniaIOSLauncherFocusTargetSort:
      return _sortButton;
    case XeniaIOSLauncherFocusTargetOnline:
      return _onlineButton;
    case XeniaIOSLauncherFocusTargetImport:
      return _openGameButton;
    case XeniaIOSLauncherFocusTargetSettings:
      return _settingsButton;
    case XeniaIOSLauncherFocusTargetProfile:
      return _profileButton;
    case XeniaIOSLauncherFocusTargetHeroPlay:
      return _heroPlayButton;
    case XeniaIOSLauncherFocusTargetHeroMore:
      return _heroMoreButton;
    default:
      return nil;
  }
}

- (void)setControllerNavigationEnabled:(BOOL)enabled
                           focusTarget:(XeniaIOSLauncherFocusTarget)target {
  BOOL previousLibraryFocusActive = _libraryFocusActive;
  _controllerNavigationEnabled = enabled;
  _libraryFocusActive = enabled && target == XeniaIOSLauncherFocusTargetLibrary;

  UIView* focused = enabled ? [self viewForControllerFocusTarget:target] : nil;
  for (UIView* view in [self controllerFocusViews]) {
    if (view == focused) {
      [self setButton:(UIButton*)view controllerFocused:YES];
    } else {
      [self clearButtonFocusChrome:(UIButton*)view];
    }
  }
  // Upright, Continue Playing and Search scroll with the games: bring them back to be seen.
  if (focused && !_sideLayout &&
      (target == XeniaIOSLauncherFocusTargetHeroPlay ||
       target == XeniaIOSLauncherFocusTargetHeroMore ||
       target == XeniaIOSLauncherFocusTargetSearch || target == XeniaIOSLauncherFocusTargetSort)) {
    [_gamesCollectionView
        setContentOffset:CGPointMake(0.0, -_gamesCollectionView.adjustedContentInset.top)
                animated:YES];
  }

  if (_libraryFocusActive && _focusedGameIndex < 0 && _games.count > 0) {
    [self setFocusedGameIndex:0 scroll:NO];
  }

  if (previousLibraryFocusActive != _libraryFocusActive && _focusedGameIndex >= 0 &&
      _focusedGameIndex < [self libraryItemCount]) {
    NSIndexPath* focusedPath = [NSIndexPath indexPathForItem:_focusedGameIndex inSection:0];
    [_gamesCollectionView reconfigureItemsAtIndexPaths:@[ focusedPath ]];
  }
}

- (BOOL)controllerShowsHero {
  return _heroGame != nil && _detailGame == nil && !_heroView.hidden;
}

- (BOOL)controllerShowsSearch {
  return !_searchRow.hidden && _searchRow.superview != nil;
}

- (BOOL)controllerSideLayout {
  return _sideLayout;
}

- (BOOL)controllerSearchEditing {
  return _searchField.isFirstResponder;
}

- (void)endControllerSearch {
  [_searchField resignFirstResponder];
}

- (UIAlertController*)activateControllerFocusTarget:(XeniaIOSLauncherFocusTarget)target {
  switch (target) {
    case XeniaIOSLauncherFocusTargetSearch:
      [_searchField becomeFirstResponder];
      return nil;
    case XeniaIOSLauncherFocusTargetOnline:
      [_onlineButton sendActionsForControlEvents:UIControlEventTouchUpInside];
      return nil;
    case XeniaIOSLauncherFocusTargetHeroPlay:
      [self heroPlayTapped:_heroPlayButton];
      return nil;
    case XeniaIOSLauncherFocusTargetHeroMore:
      [self heroMoreTapped:_heroMoreButton];
      return nil;
    case XeniaIOSLauncherFocusTargetSort:
      return [self controllerSheetForMenu:_sortButton.menu
                                    title:NSLocalizedString(@"Sort", @"launcher overlay view: "
                                                                     @"sort sheet title")
                                   source:_sortButton];
    default:
      return nil;
  }
}

// A menu as an action sheet, for a controller, which can't open the menu itself. Its current
// choice is in bold.
- (UIAlertController*)controllerSheetForMenu:(UIMenu*)menu
                                       title:(NSString*)title
                                      source:(UIView*)source {
  UIAlertController* sheet =
      [UIAlertController alertControllerWithTitle:title
                                          message:nil
                                   preferredStyle:UIAlertControllerStyleActionSheet];
  NSMutableArray<UIMenuElement*>* pending =
      [NSMutableArray arrayWithArray:menu.children ?: @[]];
  while (pending.count) {
    UIMenuElement* element = [[pending.firstObject retain] autorelease];
    [pending removeObjectAtIndex:0];
    if ([element isKindOfClass:[UIMenu class]]) {
      [pending insertObjects:((UIMenu*)element).children
                   atIndexes:[NSIndexSet
                                 indexSetWithIndexesInRange:NSMakeRange(
                                                                0, ((UIMenu*)element)
                                                                       .children.count)]];
      continue;
    }
    if (![element isKindOfClass:[UIAction class]]) {
      continue;
    }
    UIAction* action = (UIAction*)element;
    UIAlertAction* choice = [UIAlertAction
        actionWithTitle:action.title
                  style:(action.attributes & UIMenuElementAttributesDestructive)
                            ? UIAlertActionStyleDestructive
                            : UIAlertActionStyleDefault
                                                   handler:^(UIAlertAction* __unused alert) {
                                                     [action performWithSender:nil target:nil];
                                                   }];
    [sheet addAction:choice];
    if (action.state == UIMenuElementStateOn) {
      sheet.preferredAction = choice;
    }
  }
  [sheet addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"Cancel", @"Alert button: cancel")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  sheet.popoverPresentationController.sourceView = source;
  sheet.popoverPresentationController.sourceRect = source.bounds;
  return sheet;
}

- (NSArray<UIButton*>*)controllerDetailButtons {
  NSMutableArray<UIButton*>* buttons = [NSMutableArray array];
  if (!_detailGame) {
    return buttons;
  }
  for (UIButton* button in @[ _heroPlayButton, _heroTouchButton, _heroSettingsButton,
                              _detailMoreButton, _heroCompatButton ]) {
    if (button && !button.hidden && button.alpha > 0.01 && button.window) {
      [buttons addObject:button];
    }
  }
  return buttons;
}

- (NSUInteger)controllerDetailButtonCount {
  return [self controllerDetailButtons].count;
}

- (void)setControllerDetailButtonFocus:(NSUInteger)index {
  NSArray<UIButton*>* buttons = [self controllerDetailButtons];
  for (UIButton* button in @[ _heroPlayButton, _heroTouchButton, _heroSettingsButton,
                              _detailMoreButton, _heroCompatButton ]) {
    if (button) {
      [self clearButtonFocusChrome:button];
    }
  }
  if (index < buttons.count) {
    [self setButton:buttons[index] controllerFocused:YES];
  }
}

- (UIAlertController*)activateControllerDetailButton:(NSUInteger)index {
  NSArray<UIButton*>* buttons = [self controllerDetailButtons];
  if (index >= buttons.count) {
    return nil;
  }
  UIButton* button = buttons[index];
  if (button.showsMenuAsPrimaryAction && button.menu) {
    return [self controllerSheetForMenu:button.menu title:_detailGame.title source:button];
  }
  [button sendActionsForControlEvents:UIControlEventTouchUpInside];
  return nil;
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
  NSMutableArray<NSNumber*>* art_titles = [NSMutableArray array];
  for (XeniaIOSLauncherGameSnapshot* game in _games) {
    if (game.supportsRemoteArt && game.titleId) {
      [art_titles addObject:@(game.titleId)];
    }
  }
  XEPrefetchBackgroundArt(art_titles, 0);

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
  // Room for the longest title (two lines, three at accessibility sizes) so every tile in a row
  // has the same height; the status follows the title.
  const CGFloat title_lines = accessibilitySize ? 3.0 : 2.0;
  // 7 above the title + title + 1 gap + status.
  return 7.0 + ceil(titleLine * title_lines) + 1.0 + ceil(metaLine);
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
  cell.discBadge.text =
      game.discCount > 1
          ? [NSString stringWithFormat:NSLocalizedString(@"%lu Discs",
                                                         @"launcher overlay view: badge on a "
                                                         @"game's cover; %lu is the disc count"),
                                       (unsigned long)game.discCount]
          : @"";
  cell.discBadge.hidden = game.discCount <= 1;
  cell.contentTypePill.text = game.contentTypeName ?: @"";
  // One line under the title: the status when there is one, otherwise the tag. A reused cell
  // doesn't always lay out again, so this is decided here rather than left to -layoutSubviews.
  cell.contentTypePill.hidden = game.contentTypeName.length == 0 || game.hasCompatInfo;
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
  if (cell.discBadge.text.length) {
    [accessibility_values addObject:cell.discBadge.text];
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
      [self refreshVisibleCoversLoading:NO];
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
          [self refreshVisibleCoversLoading:NO];
        }
      });
    });
  });
}

// Puts the cached cover on every visible tile that isn't showing it, and with |load| starts loading
// the ones that aren't cached. Tiles are matched by the title they show, not by index path: a cover
// that lands while the grid is mid-update or mid-rotation would otherwise miss its tile, which then
// kept its placeholder with the cover sitting in the cache.
- (void)refreshVisibleCoversLoading:(BOOL)load {
  for (UICollectionViewCell* visible in _gamesCollectionView.visibleCells) {
    if (![visible isKindOfClass:[XeniaGameTileCell class]]) {
      continue;
    }
    XeniaGameTileCell* cell = (XeniaGameTileCell*)visible;
    const uint32_t titleId = cell.representedTitleId;
    if (!titleId) {
      continue;
    }
    UIImage* cover = xe_cached_game_art_thumbnail(titleId);
    if (cover) {
      if (cell.iconView.image != cover) {
        cell.iconView.image = cover;
      }
      continue;
    }
    if (!load) {
      continue;
    }
    for (XeniaIOSLauncherGameSnapshot* game in _shownGames) {
      if (game.titleId == titleId) {
        if (game.supportsRemoteArt) {
          [self loadCoverForTitleId:titleId collectionView:_gamesCollectionView];
        }
        break;
      }
    }
  }
}

// After the grid has laid out its new tiles.
- (void)refreshVisibleCoversSoon {
  [self retain];
  dispatch_async(dispatch_get_main_queue(), ^{
    [self refreshVisibleCoversLoading:YES];
    [self release];
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
