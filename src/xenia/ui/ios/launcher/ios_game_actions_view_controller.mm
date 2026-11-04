/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_actions_view_controller.h"

#include <string>
#include <utility>
#include <vector>

#import "xenia/ui/ios/app/windowed_app_context_ios.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_game_patches_view_controller.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

struct XeniaIOSGameActionRow {
  XeniaIOSGameAction action;
  NSString* title;
  NSString* symbol;
};

// The art's height, behind the status bar, before it fades into the page.
constexpr CGFloat kArtHeight = 330.0;
constexpr CGFloat kArtHeightShort = 230.0;
constexpr CGFloat kMargin = 20.0;
constexpr CGFloat kCoverWidth = 88.0;
// Where a row's title starts after its symbol, which the patch switches under Patches line up with.
constexpr CGFloat kSymbolWidth = 24.0;
constexpr CGFloat kSymbolPadding = 16.0;
NSString* const kActionCellIdentifier = @"XeniaIOSGameActionCell";
NSString* const kPatchCellIdentifier = @"XeniaIOSGamePatchCell";

UIButton* NewGlassCircle(NSString* symbol, CGFloat side, NSString* label) {
  UIButtonConfiguration* config = [UIButtonConfiguration glassButtonConfiguration];
  config.image = [UIImage
      systemImageNamed:symbol
      withConfiguration:[UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleBody
                                                                         scale:
                                                                             UIImageSymbolScaleLarge]];
  config.baseForegroundColor = UIColor.whiteColor;
  config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  UIButton* button = [UIButton buttonWithConfiguration:config primaryAction:nil];
  button.translatesAutoresizingMaskIntoConstraints = NO;
  button.accessibilityLabel = label;
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:side],
    [button.heightAnchor constraintEqualToConstant:side],
  ]];
  return button;
}

}  // namespace

@interface XeniaIOSGameActionsViewController () <UITableViewDataSource, UITableViewDelegate>
@end

@implementation XeniaIOSGameActionsViewController {
  NSString* game_title_;
  uint32_t title_id_;
  BOOL supports_compatibility_;
  BOOL supports_manage_content_;
  BOOL supports_disc_selection_;
  BOOL supports_patches_;
  BOOL supports_zar_conversion_;
  NSArray<NSValue*>* rows_;
  void (^action_handler_)(XeniaIOSGameAction action);
  NSString* status_text_;
  UIColor* status_color_;

  UITableView* table_view_;
  UIView* header_;
  UIImageView* art_view_;
  CAGradientLayer* art_fade_;
  CAGradientLayer* art_top_;
  UIImageView* cover_view_;
  UIStackView* title_stack_;
  UIStackView* button_row_;
  UIButton* close_button_;
  UIButton* more_button_;
  UIVisualEffectView* top_bar_;
  UILabel* top_title_;
  NSLayoutConstraint* cover_top_;

  xe::ui::IOSWindowedAppContext* app_context_;
  // Each patch with the file it comes from, read when Patches first opens.
  std::vector<std::pair<std::string, xe::ui::IOSPatchEntrySummary>> patches_;
  BOOL patches_loaded_;
  BOOL patches_expanded_;
  BOOL shows_rows_only_;
}

@synthesize actionHandler = action_handler_;
@synthesize statusText = status_text_;
@synthesize statusColor = status_color_;
@synthesize appContext = app_context_;
@synthesize showsRowsOnly = shows_rows_only_;

- (instancetype)initWithGameTitle:(NSString*)gameTitle
                          titleID:(uint32_t)titleID
            supportsCompatibility:(BOOL)supportsCompatibility
            supportsManageContent:(BOOL)supportsManageContent
            supportsDiscSelection:(BOOL)supportsDiscSelection
                  supportsPatches:(BOOL)supportsPatches
            supportsZarConversion:(BOOL)supportsZarConversion {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    game_title_ = [gameTitle copy];
    title_id_ = titleID;
    supports_compatibility_ = supportsCompatibility;
    supports_manage_content_ = supportsManageContent;
    supports_disc_selection_ = supportsDiscSelection;
    supports_patches_ = supportsPatches;
    supports_zar_conversion_ = supportsZarConversion;
  }
  return self;
}

- (void)dealloc {
  [game_title_ release];
  [rows_ release];
  [action_handler_ release];
  [status_text_ release];
  [status_color_ release];
  [table_view_ release];
  [header_ release];
  [art_view_ release];
  [art_fade_ release];
  [art_top_ release];
  [cover_view_ release];
  [title_stack_ release];
  [button_row_ release];
  [close_button_ release];
  [more_button_ release];
  [top_bar_ release];
  [top_title_ release];
  [cover_top_ release];
  [super dealloc];
}

// The rows under Play: what the game has, in the order people reach for it.
- (NSArray<NSValue*>*)buildRows {
  NSMutableArray<NSValue*>* values = [NSMutableArray array];
  auto add = [values](XeniaIOSGameAction action, NSString* title, NSString* symbol) {
    XeniaIOSGameActionRow row = {action, title, symbol};
    [values addObject:[NSValue valueWithBytes:&row objCType:@encode(XeniaIOSGameActionRow)]];
  };
  if (supports_patches_) {
    add(XeniaIOSGameActionPatches,
        NSLocalizedString(@"Patches", @"game page: row"), @"puzzlepiece.extension");
  }
  if (supports_manage_content_) {
    add(XeniaIOSGameActionManageContent,
        NSLocalizedString(@"Updates & Add-ons", @"game page: row"), @"square.stack.3d.up");
  }
  if (supports_compatibility_) {
    add(XeniaIOSGameActionCompatibility,
        NSLocalizedString(@"Compatibility", @"game page: row"), @"checkmark.shield");
  }
  if (title_id_) {
    add(XeniaIOSGameActionSystemLink,
        NSLocalizedString(@"System Link", @"game page: row"), @"network");
  }
  if (supports_disc_selection_) {
    add(XeniaIOSGameActionLaunchDisc,
        NSLocalizedString(@"Launch Disc", @"game page: row"), @"opticaldisc");
  }
  if (supports_zar_conversion_) {
    add(XeniaIOSGameActionConvertToZar,
        NSLocalizedString(@"Convert to ZAR", @"game page: row"), @"archivebox");
  }
  return values;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = game_title_;
  self.view.backgroundColor = [XeniaTheme bgPrimary];
  rows_ = [[self buildRows] retain];

  table_view_ = [[UITableView alloc] initWithFrame:self.view.bounds style:UITableViewStylePlain];
  table_view_.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  table_view_.dataSource = self;
  table_view_.delegate = self;
  table_view_.backgroundColor = [UIColor clearColor];
  table_view_.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  table_view_.separatorColor = [XeniaTheme border];
  table_view_.separatorInset = UIEdgeInsetsMake(0.0, kMargin, 0.0, kMargin);
  table_view_.rowHeight = UITableViewAutomaticDimension;
  table_view_.estimatedRowHeight = 58.0;
  table_view_.sectionHeaderTopPadding = 0.0;
  // Insets come from viewWillLayoutSubviews alone, measured from the screen edge.
  table_view_.insetsContentViewsToSafeArea = NO;
  table_view_.cellLayoutMarginsFollowReadableWidth = NO;
  [self.view addSubview:table_view_];
  if (shows_rows_only_) {
    self.view.backgroundColor = [UIColor clearColor];
    return;
  }

  [self buildHeader];

  // Once the title scrolls under ✕ and ⋯, a bar blurs in behind them with the name.
  top_bar_ = [[UIVisualEffectView alloc]
      initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemChromeMaterial]];
  top_bar_.alpha = 0.0;
  UIView* hairline = [[[UIView alloc] init] autorelease];
  hairline.backgroundColor = [XeniaTheme border];
  hairline.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
  hairline.frame = CGRectMake(0.0, 59.5, 100.0, 0.5);
  [top_bar_.contentView addSubview:hairline];
  top_title_ = [[UILabel alloc] init];
  top_title_.text = game_title_;
  top_title_.textColor = [XeniaTheme textPrimary];
  top_title_.textAlignment = NSTextAlignmentCenter;
  xe_apply_label_font(top_title_, UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold);
  top_title_.accessibilityTraits = UIAccessibilityTraitHeader;
  [top_bar_.contentView addSubview:top_title_];
  [self.view addSubview:top_bar_];

  // ✕ and ⋯ float over the art and stay put as the page scrolls.
  close_button_ = [NewGlassCircle(@"xmark", 44.0,
                                  NSLocalizedString(@"Close", @"sheet close button")) retain];
  [close_button_ addTarget:self
                    action:@selector(cancelTapped:)
          forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:close_button_];
  more_button_ = [NewGlassCircle(@"ellipsis", 44.0,
                                 NSLocalizedString(@"More", @"game page: more button")) retain];
  more_button_.menu = [self moreMenu];
  more_button_.showsMenuAsPrimaryAction = YES;
  more_button_.hidden = more_button_.menu.children.count == 0;
  [self.view addSubview:more_button_];
  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [close_button_.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16.0],
    [close_button_.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8.0],
    [more_button_.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16.0],
    [more_button_.topAnchor constraintEqualToAnchor:close_button_.topAnchor],
  ]];
  [self loadArt];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self.navigationController setNavigationBarHidden:YES animated:animated];
  // The full Patches page may have changed them while this page was under it.
  if (patches_loaded_) {
    [self loadPatches];
    [table_view_ reloadData];
  }
}

- (UIMenu*)moreMenu {
  if (!title_id_) {
    return [UIMenu menuWithChildren:@[]];
  }
  __block XeniaIOSGameActionsViewController* owner = self;  // MRC: not retained.
  UIAction* copy_url = [UIAction
      actionWithTitle:NSLocalizedString(@"Copy Launch URL", @"game page: more menu")
                image:[UIImage systemImageNamed:@"link"]
           identifier:nil
              handler:^(__unused UIAction* action) {
                [owner perform:XeniaIOSGameActionCopyLaunchURL];
              }];
  UIAction* reset = [UIAction
      actionWithTitle:NSLocalizedString(@"Reset Game Settings", @"game page: more menu")
                image:[UIImage systemImageNamed:@"arrow.counterclockwise"]
           identifier:nil
              handler:^(__unused UIAction* action) {
                [owner perform:XeniaIOSGameActionResetGameSettings];
              }];
  reset.attributes = UIMenuElementAttributesDestructive;
  return [UIMenu menuWithChildren:@[
    copy_url, [UIMenu menuWithTitle:@""
                              image:nil
                         identifier:nil
                            options:UIMenuOptionsDisplayInline
                           children:@[ reset ]]
  ]];
}

- (void)buildHeader {
  header_ = [[UIView alloc] init];
  header_.insetsLayoutMarginsFromSafeArea = NO;
  header_.clipsToBounds = NO;

  art_view_ = [[UIImageView alloc] init];
  art_view_.contentMode = UIViewContentModeScaleAspectFill;
  art_view_.clipsToBounds = YES;
  art_view_.backgroundColor = [XeniaTheme bgSurface];
  art_view_.isAccessibilityElement = NO;
  [header_ addSubview:art_view_];
  art_top_ = [[CAGradientLayer layer] retain];
  art_top_.colors =
      @[ (id)[UIColor colorWithWhite:0.0 alpha:0.35].CGColor, (id)[UIColor clearColor].CGColor ];
  [art_view_.layer addSublayer:art_top_];
  art_fade_ = [[CAGradientLayer layer] retain];
  [art_view_.layer addSublayer:art_fade_];

  cover_view_ = [[UIImageView alloc] init];
  cover_view_.translatesAutoresizingMaskIntoConstraints = NO;
  cover_view_.contentMode = UIViewContentModeScaleAspectFill;
  cover_view_.clipsToBounds = YES;
  cover_view_.backgroundColor = [XeniaTheme bgSurface2];
  cover_view_.layer.cornerRadius = 8.0;
  cover_view_.layer.cornerCurve = kCACornerCurveContinuous;
  cover_view_.layer.borderWidth = 0.5;
  cover_view_.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.12].CGColor;
  cover_view_.isAccessibilityElement = NO;
  [header_ addSubview:cover_view_];

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.text = game_title_;
  title.textColor = [XeniaTheme textPrimary];
  title.font = xe_scaled_system_font(UIFontTextStyleLargeTitle, 32.0, UIFontWeightHeavy);
  title.adjustsFontForContentSizeCategory = YES;
  title.numberOfLines = 3;
  title.adjustsFontSizeToFitWidth = YES;
  title.minimumScaleFactor = 0.6;
  title.accessibilityTraits = UIAccessibilityTraitHeader;
  NSMutableArray<UIView*>* lines = [NSMutableArray arrayWithObject:title];
  if (status_text_.length) {
    UILabel* status = [[[UILabel alloc] init] autorelease];
    UIFont* font = xe_scaled_system_font(UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
    NSMutableAttributedString* text = [[[NSMutableAttributedString alloc]
        initWithString:@"● "
            attributes:@{
              NSForegroundColorAttributeName : status_color_ ?: [XeniaTheme textSecondary],
              NSFontAttributeName : [UIFont systemFontOfSize:font.pointSize * 0.7],
              NSBaselineOffsetAttributeName : @(font.pointSize * 0.1),
            }] autorelease];
    [text appendAttributedString:[[[NSAttributedString alloc]
                                     initWithString:status_text_
                                         attributes:@{
                                           NSForegroundColorAttributeName :
                                               [XeniaTheme textSecondary],
                                           NSFontAttributeName : font,
                                         }] autorelease]];
    status.attributedText = text;
    [lines addObject:status];
  }
  title_stack_ = [[UIStackView alloc] initWithArrangedSubviews:lines];
  title_stack_.translatesAutoresizingMaskIntoConstraints = NO;
  title_stack_.axis = UILayoutConstraintAxisVertical;
  title_stack_.spacing = 4.0;
  // Keeps the title and status legible where they sit over bright art.
  title_stack_.layer.shadowColor = UIColor.blackColor.CGColor;
  title_stack_.layer.shadowOpacity = 0.6;
  title_stack_.layer.shadowRadius = 10.0;
  title_stack_.layer.shadowOffset = CGSizeZero;
  [header_ addSubview:title_stack_];

  // Play is the one white button; touch layout and settings sit beside it as glass circles.
  UIButtonConfiguration* play = [UIButtonConfiguration filledButtonConfiguration];
  play.title = NSLocalizedString(@"Play", @"game page: play button");
  play.image = [UIImage
      systemImageNamed:@"play.fill"
      withConfiguration:[UIImageSymbolConfiguration configurationWithTextStyle:UIFontTextStyleBody
                                                                         scale:
                                                                             UIImageSymbolScaleSmall]];
  play.imagePadding = 8.0;
  play.baseBackgroundColor = [XeniaTheme textPrimary];
  play.baseForegroundColor = [XeniaTheme bgPrimary];
  play.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  play.titleTextAttributesTransformer = ^NSDictionary<NSAttributedStringKey, id>*(
      NSDictionary<NSAttributedStringKey, id>* attributes) {
    NSMutableDictionary* updated = [[attributes mutableCopy] autorelease];
    updated[NSFontAttributeName] =
        xe_scaled_system_font(UIFontTextStyleHeadline, 17.0, UIFontWeightSemibold);
    return updated;
  };
  UIButton* play_button = [UIButton buttonWithConfiguration:play primaryAction:nil];
  play_button.translatesAutoresizingMaskIntoConstraints = NO;
  [play_button addTarget:self
                  action:@selector(playTapped:)
        forControlEvents:UIControlEventTouchUpInside];
  NSMutableArray<UIView*>* buttons = [NSMutableArray arrayWithObject:play_button];
  if (title_id_) {
    UIButton* touch = NewGlassCircle(
        @"hand.raised", 54.0, NSLocalizedString(@"Touch Layout", @"game page: button"));
    [touch addTarget:self
                  action:@selector(touchLayoutTapped:)
        forControlEvents:UIControlEventTouchUpInside];
    UIButton* settings = NewGlassCircle(
        @"slider.horizontal.3", 54.0, NSLocalizedString(@"Game Settings", @"game page: button"));
    [settings addTarget:self
                  action:@selector(settingsTapped:)
        forControlEvents:UIControlEventTouchUpInside];
    [buttons addObject:touch];
    [buttons addObject:settings];
  }
  button_row_ = [[UIStackView alloc] initWithArrangedSubviews:buttons];
  button_row_.translatesAutoresizingMaskIntoConstraints = NO;
  button_row_.spacing = 12.0;
  button_row_.alignment = UIStackViewAlignmentCenter;
  [header_ addSubview:button_row_];

  cover_top_ = [[cover_view_.topAnchor constraintEqualToAnchor:header_.topAnchor
                                                      constant:kArtHeight - 150.0] retain];
  [NSLayoutConstraint activateConstraints:@[
    cover_top_,
    [cover_view_.leadingAnchor constraintEqualToAnchor:header_.layoutMarginsGuide.leadingAnchor],
    [cover_view_.widthAnchor constraintEqualToConstant:kCoverWidth],
    [cover_view_.heightAnchor constraintEqualToAnchor:cover_view_.widthAnchor
                                           multiplier:300.0 / 219.0],
    [title_stack_.leadingAnchor constraintEqualToAnchor:cover_view_.trailingAnchor
                                               constant:18.0],
    [title_stack_.trailingAnchor
        constraintEqualToAnchor:header_.layoutMarginsGuide.trailingAnchor],
    [title_stack_.bottomAnchor constraintEqualToAnchor:cover_view_.bottomAnchor constant:-4.0],
    [title_stack_.topAnchor constraintGreaterThanOrEqualToAnchor:header_.topAnchor],
    [button_row_.topAnchor constraintEqualToAnchor:cover_view_.bottomAnchor constant:26.0],
    [button_row_.leadingAnchor constraintEqualToAnchor:header_.layoutMarginsGuide.leadingAnchor],
    // Full width on a phone in portrait; a readable width beside the cover when wider.
    [button_row_.trailingAnchor
        constraintLessThanOrEqualToAnchor:header_.layoutMarginsGuide.trailingAnchor],
    [button_row_.widthAnchor constraintLessThanOrEqualToConstant:460.0],
    [button_row_.bottomAnchor constraintEqualToAnchor:header_.bottomAnchor constant:-18.0],
    [play_button.heightAnchor constraintEqualToConstant:54.0],
    ({
      NSLayoutConstraint* fill = [button_row_.trailingAnchor
          constraintEqualToAnchor:header_.layoutMarginsGuide.trailingAnchor];
      fill.priority = UILayoutPriorityDefaultHigh;
      fill;
    }),
  ]];
  table_view_.tableHeaderView = header_;
}

// Background art, or the cover blown up behind it when there is none.
- (void)loadArt {
  if (!title_id_) {
    return;
  }
  UIImage* cover = xe_cached_game_art(title_id_);
  cover_view_.image = cover;
  art_view_.image = xe_cached_game_background_art(title_id_) ?: cover;
  const uint32_t title_id = title_id_;
  [self retain];
  if (!cover) {
    xe_fetch_game_art(title_id, ^(UIImage* image) {
      if (image) {
        cover_view_.image = image;
        if (!art_view_.image) {
          art_view_.image = image;
        }
      }
    });
  }
  xe_fetch_game_background_art(title_id, ^(UIImage* image) {
    if (image) {
      art_view_.image = image;
    }
    [self release];
  });
}

- (void)viewWillLayoutSubviews {
  [super viewWillLayoutSubviews];
  const CGSize size = self.view.bounds.size;
  const UIEdgeInsets safe = self.view.safeAreaInsets;
  if (shows_rows_only_) {
    table_view_.frame = self.view.bounds;
    // Rows fade out as they scroll up under the Library's header, as its covers do.
    CAGradientLayer* fade = [CAGradientLayer layer];
    fade.frame = self.view.bounds;
    fade.colors = @[ (id)UIColor.clearColor.CGColor, (id)UIColor.blackColor.CGColor ];
    fade.locations = @[ @0.0, @(16.0 / MAX(size.height, 1.0)) ];
    UIView* mask = [[[UIView alloc] initWithFrame:self.view.bounds] autorelease];
    [mask.layer addSublayer:fade];
    self.view.maskView = mask;
    table_view_.contentInset = UIEdgeInsetsMake(0.0, 0.0, safe.bottom + 16.0, 0.0);
    const UIEdgeInsets table_safe = table_view_.safeAreaInsets;
    table_view_.separatorInset = UIEdgeInsetsMake(
        0.0, table_safe.left + kMargin + kSymbolWidth + kSymbolPadding, 0.0,
        table_safe.right + kMargin);
    for (UITableViewCell* cell in table_view_.visibleCells) {
      [self applyMarginsToCell:cell];
    }
    return;
  }
  // A phone on its side: art, cover, title and Play on the left half, the rows on the right, so
  // the whole page fits on one screen instead of a portrait page stretched wide.
  const BOOL side = size.width > size.height && size.height < 500.0;
  // The left column is as wide as Play and its two buttons need, so the rows sit right beside
  // them instead of past an empty stretch of art.
  const CGFloat column =
      side ? floor(MIN(size.width * 0.52, MAX(size.width * 0.4, safe.left + kMargin + 300.0)))
           : size.width;
  const CGFloat art_height = side ? size.height : (size.height < 500.0 ? kArtHeightShort : kArtHeight);
  const CGFloat leading = safe.left + kMargin;
  const CGFloat trailing = side ? size.width - column + 8.0 : safe.right + kMargin;
  header_.layoutMargins = UIEdgeInsetsMake(0.0, leading, 0.0, trailing);

  // In the side layout the art stays put behind the left column; otherwise it scrolls with the
  // header.
  UIView* art_parent = side ? self.view : header_;
  if (art_view_.superview != art_parent) {
    [art_view_ removeFromSuperview];
    [art_parent insertSubview:art_view_ atIndex:0];
  }

  cover_top_.constant = side ? 0.0 : art_height - 150.0;
  header_.frame = CGRectMake(0.0, 0.0, size.width, 0.0);
  [header_ layoutIfNeeded];
  const CGFloat height =
      [header_ systemLayoutSizeFittingSize:CGSizeMake(size.width, 0.0)
             withHorizontalFittingPriority:UILayoutPriorityRequired
                   verticalFittingPriority:UILayoutPriorityFittingSizeLevel]
          .height;
  if (side) {
    // Centred in the space under ✕, nudged down a little so it sits on the art's lower half.
    const CGFloat top = safe.top + 60.0;
    const CGFloat y = MAX(top, top + (size.height - top - safe.bottom - height) * 0.6);
    if (table_view_.tableHeaderView == header_) {
      table_view_.tableHeaderView = nil;
    }
    if (header_.superview != self.view) {
      [header_ removeFromSuperview];
      [self.view insertSubview:header_ aboveSubview:art_view_];
    }
    header_.frame = CGRectMake(0.0, y, size.width, height);
    table_view_.frame = CGRectMake(column, 0.0, size.width - column, size.height);
    // The first row lines up with the top of the cover.
    table_view_.contentInset = UIEdgeInsetsMake(y, 0.0, safe.bottom + 16.0, 0.0);
  } else {
    if (header_.superview == self.view) {
      [header_ removeFromSuperview];
    }
    table_view_.frame = self.view.bounds;
    if (header_.frame.size.height != height || table_view_.tableHeaderView != header_) {
      header_.frame = CGRectMake(0.0, 0.0, size.width, height);
      table_view_.tableHeaderView = header_;
    }
    table_view_.contentInset = UIEdgeInsetsMake(0.0, 0.0, safe.bottom + 16.0, 0.0);
  }
  [self.view bringSubviewToFront:top_bar_];
  [self.view bringSubviewToFront:close_button_];
  [self.view bringSubviewToFront:more_button_];

  const UIEdgeInsets table_safe = table_view_.safeAreaInsets;
  table_view_.separatorInset =
      UIEdgeInsetsMake(0.0, table_safe.left + kMargin + 40.0, 0.0, table_safe.right + kMargin);
  for (UITableViewCell* cell in table_view_.visibleCells) {
    [self applyMarginsToCell:cell];
  }

  art_view_.frame = CGRectMake(0.0, 0.0, side ? column + 120.0 : size.width, art_height);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  art_top_.frame = CGRectMake(0.0, 0.0, art_view_.bounds.size.width, 120.0);
  art_fade_.frame = art_view_.bounds;
  UIColor* page = [[XeniaTheme bgPrimary] resolvedColorWithTraitCollection:self.traitCollection];
  art_fade_.colors = @[
    (id)[page colorWithAlphaComponent:0.0].CGColor, (id)[page colorWithAlphaComponent:0.0].CGColor,
    (id)[page colorWithAlphaComponent:0.7].CGColor, (id)page.CGColor
  ];
  if (side) {
    // Fades into the page towards the rows, and darkens under the title.
    art_fade_.startPoint = CGPointMake(0.0, 0.3);
    art_fade_.endPoint = CGPointMake(1.0, 0.7);
    art_fade_.locations = @[ @0.0, @0.35, @0.7, @0.9 ];
  } else {
    art_fade_.startPoint = CGPointMake(0.5, 0.0);
    art_fade_.endPoint = CGPointMake(0.5, 1.0);
    art_fade_.locations = @[ @0.0, @0.42, @0.75, @1.0 ];
  }
  [CATransaction commit];
  top_bar_.frame = CGRectMake(0.0, 0.0, size.width, safe.top + 60.0);
  top_title_.frame = CGRectMake(MAX(safe.left, 16.0) + 60.0, safe.top + 8.0,
                                size.width - 2.0 * (MAX(safe.left, safe.right) + 76.0), 44.0);
  [self scrollViewDidScroll:table_view_];
}

- (void)scrollViewDidScroll:(UIScrollView*)scrollView {
  if (scrollView != table_view_) {
    return;
  }
  // The bar is fully in by the time the title would pass under it. The side layout keeps the
  // title in view, so it never needs one.
  if (table_view_.tableHeaderView != header_) {
    top_bar_.alpha = 0.0;
    return;
  }
  const CGFloat bar = self.view.safeAreaInsets.top + 60.0;
  const CGFloat start = cover_top_.constant + 40.0 - bar;
  top_bar_.alpha = MIN(1.0, MAX(0.0, (scrollView.contentOffset.y - start) / 40.0));
}

- (void)perform:(XeniaIOSGameAction)action {
  if (action_handler_) {
    action_handler_(action);
  }
}

- (void)playTapped:(UIButton*)__unused sender {
  [self perform:XeniaIOSGameActionPlay];
}

- (void)touchLayoutTapped:(UIButton*)__unused sender {
  [self perform:XeniaIOSGameActionTouchLayout];
}

- (void)settingsTapped:(UIButton*)__unused sender {
  [self perform:XeniaIOSGameActionGameSettings];
}

- (void)cancelTapped:(id)sender {
  (void)sender;
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)loadPatches {
  patches_.clear();
  if (app_context_ && title_id_) {
    for (const auto& file : app_context_->ListPatchFiles(title_id_)) {
      for (const auto& patch : file.patches) {
        patches_.emplace_back(file.filename, patch);
      }
    }
  }
  patches_loaded_ = YES;
}

// The Patches row's place among the rows, or NSNotFound.
- (NSInteger)patchesRow {
  for (NSUInteger i = 0; i < rows_.count; ++i) {
    XeniaIOSGameActionRow row = {};
    [rows_[i] getValue:&row];
    if (row.action == XeniaIOSGameActionPatches) {
      return static_cast<NSInteger>(i);
    }
  }
  return NSNotFound;
}

- (NSInteger)shownPatchCount {
  return patches_expanded_ ? static_cast<NSInteger>(patches_.size()) : 0;
}

// The patch a table row shows when Patches is open, or -1 for an action row.
- (NSInteger)patchIndexForRow:(NSInteger)row {
  const NSInteger patches_row = [self patchesRow];
  if (patches_row == NSNotFound || row <= patches_row ||
      row > patches_row + [self shownPatchCount]) {
    return -1;
  }
  return row - patches_row - 1;
}

- (XeniaIOSGameActionRow)rowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaIOSGameActionRow row = {};
  if (indexPath.section != 0 || [self patchIndexForRow:indexPath.row] >= 0) {
    return row;
  }
  NSInteger index = indexPath.row;
  const NSInteger patches_row = [self patchesRow];
  if (patches_row != NSNotFound && index > patches_row) {
    index -= [self shownPatchCount];
  }
  if (index < 0 || index >= static_cast<NSInteger>(rows_.count)) {
    return row;
  }
  [rows_[index] getValue:&row];
  return row;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? static_cast<NSInteger>(rows_.count) + [self shownPatchCount] : 0;
}

// The symbol sits on the page's leading edge and the chevron on its trailing one, both clear of
// the sensor housing in landscape.
- (void)applyMarginsToCell:(UITableViewCell*)cell {
  const UIEdgeInsets safe = table_view_.safeAreaInsets;
  cell.insetsLayoutMarginsFromSafeArea = NO;
  cell.preservesSuperviewLayoutMargins = NO;
  cell.layoutMargins = UIEdgeInsetsMake(0.0, safe.left + kMargin, 0.0, safe.right + kMargin);
  UIListContentConfiguration* content = (UIListContentConfiguration*)cell.contentConfiguration;
  if ([content isKindOfClass:[UIListContentConfiguration class]]) {
    content = [[content copy] autorelease];
    // These margins start from the safe area already. Patch switches start where row titles do.
    const BOOL patch = [cell.reuseIdentifier isEqualToString:kPatchCellIdentifier];
    content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(
        patch ? 8.0 : 0.0, kMargin - 4.0 + (patch ? kSymbolWidth + kSymbolPadding : 0.0),
        patch ? 8.0 : 0.0, 0.0);
    cell.contentConfiguration = content;
  }
}

// Plain rows on the page, no cards: a symbol, the title and a chevron.
// "2 On", or "Off" when none is.
- (NSString*)patchesSummary {
  NSUInteger enabled = 0;
  for (const auto& entry : patches_) {
    enabled += entry.second.is_enabled ? 1 : 0;
  }
  return enabled ? [NSString stringWithFormat:NSLocalizedString(@"%lu On",
                                                                @"game page: Patches row; %lu is "
                                                                @"how many patches are on"),
                                              static_cast<unsigned long>(enabled)]
                 : NSLocalizedString(@"Off", @"game page: Patches row; no patch is on");
}

// A switch under Patches: the patch's name, its description under it when it has one.
- (UITableViewCell*)patchCellForRow:(NSInteger)patch_index tableView:(UITableView*)tableView {
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kPatchCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kPatchCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor clearColor];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
    [toggle addTarget:self
                  action:@selector(patchSwitchChanged:)
        forControlEvents:UIControlEventValueChanged];
    cell.accessoryView = toggle;
  }
  const auto& patch = patches_[static_cast<size_t>(patch_index)].second;
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = XeniaPatchDisplayName(patch);
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.font =
      xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  if (!patch.description.empty()) {
    content.secondaryText = ToNSString(patch.description);
    content.secondaryTextProperties.color = [XeniaTheme textSecondary];
    content.secondaryTextProperties.font =
        xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular);
    content.secondaryTextProperties.numberOfLines = 2;
    content.textToSecondaryTextVerticalPadding = 2.0;
  }
  cell.contentConfiguration = content;
  UISwitch* toggle = (UISwitch*)cell.accessoryView;
  toggle.tag = patch_index;
  toggle.on = patch.is_enabled;
  toggle.accessibilityLabel = content.text;
  return cell;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  const NSInteger patch_index = [self patchIndexForRow:indexPath.row];
  UITableViewCell* cell = nil;
  if (patch_index >= 0) {
    cell = [self patchCellForRow:patch_index tableView:tableView];
  } else {
    cell = [self actionCellAtIndexPath:indexPath tableView:tableView];
  }
  [self applyMarginsToCell:cell];
  [self applySeparatorToCell:cell atRow:indexPath.row];
  return cell;
}

// Separators go between rows only, never under the last one.
- (void)applySeparatorToCell:(UITableViewCell*)cell atRow:(NSInteger)row {
  const BOOL last = row == [table_view_ numberOfRowsInSection:0] - 1;
  cell.separatorInset =
      last ? UIEdgeInsetsMake(0.0, CGRectGetWidth(table_view_.bounds) + 1000.0, 0.0, 0.0)
           : table_view_.separatorInset;
}

// After rows come or go, the row that is last changes.
- (void)updateSeparators {
  for (UITableViewCell* cell in table_view_.visibleCells) {
    NSIndexPath* path = [table_view_ indexPathForCell:cell];
    if (path) {
      [self applySeparatorToCell:cell atRow:path.row];
    }
  }
}

- (UITableViewCell*)actionCellAtIndexPath:(NSIndexPath*)indexPath
                                tableView:(UITableView*)tableView {
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kActionCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kActionCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor clearColor];
  }
  [self configureActionCell:cell atIndexPath:indexPath];
  return cell;
}

- (void)configureActionCell:(UITableViewCell*)cell atIndexPath:(NSIndexPath*)indexPath {
  XeniaIOSGameActionRow row = [self rowAtIndexPath:indexPath];
  const BOOL patches = row.action == XeniaIOSGameActionPatches;
  UIListContentConfiguration* content = patches && patches_loaded_ && !patches_.empty()
                                            ? [UIListContentConfiguration valueCellConfiguration]
                                            : [UIListContentConfiguration cellConfiguration];
  content.text = row.title;
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.font =
      xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  content.image = [UIImage systemImageNamed:row.symbol];
  content.imageProperties.tintColor = [XeniaTheme textSecondary];
  content.imageToTextPadding = kSymbolPadding;
  content.imageProperties.reservedLayoutSize = CGSizeMake(kSymbolWidth, 0.0);
  if (patches && patches_loaded_ && !patches_.empty()) {
    content.secondaryText = [self patchesSummary];
    content.secondaryTextProperties.color = [XeniaTheme textSecondary];
    content.secondaryTextProperties.font =
        xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  }
  cell.contentConfiguration = content;
  // One chevron for every row; Patches opens in place, so its chevron turns down instead.
  {
    UIImageView* chevron = [[[UIImageView alloc]
        initWithImage:[UIImage
                          systemImageNamed:@"chevron.right"
                          withConfiguration:[UIImageSymbolConfiguration
                                                configurationWithPointSize:14.0
                                                                    weight:
                                                                        UIImageSymbolWeightSemibold]]]
        autorelease];
    chevron.tintColor = [UIColor tertiaryLabelColor];
    chevron.transform = patches && patches_expanded_ ? CGAffineTransformMakeRotation(M_PI_2)
                                                     : CGAffineTransformIdentity;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.accessoryView = chevron;
    cell.accessibilityTraits = UIAccessibilityTraitButton;
    cell.accessibilityValue =
        !patches ? nil
        : patches_expanded_ ? NSLocalizedString(@"Expanded", @"game page: Patches row state")
                            : NSLocalizedString(@"Collapsed", @"game page: Patches row state");
  }
}

// The Patches row's count and chevron, on the cell already showing.
- (void)refreshPatchesRow:(BOOL)animated {
  const NSInteger patches_row = [self patchesRow];
  if (patches_row == NSNotFound) {
    return;
  }
  NSIndexPath* path = [NSIndexPath indexPathForRow:patches_row inSection:0];
  UITableViewCell* cell = [table_view_ cellForRowAtIndexPath:path];
  if (!cell) {
    return;
  }
  UIView* chevron = cell.accessoryView;
  const CGAffineTransform turned = chevron.transform;
  [self configureActionCell:cell atIndexPath:path];
  [self applyMarginsToCell:cell];
  if (animated && cell.accessoryView) {
    const CGAffineTransform target = cell.accessoryView.transform;
    cell.accessoryView.transform = turned;
    [UIView animateWithDuration:0.3
                     animations:^{
                       cell.accessoryView.transform = target;
                     }];
  }
}

- (void)togglePatchesAtRow:(NSInteger)patches_row {
  if (!patches_loaded_) {
    [self loadPatches];
  }
  // Nothing to switch: the page says where patch files go.
  if (patches_.empty()) {
    [self perform:XeniaIOSGameActionPatches];
    return;
  }
  NSMutableArray<NSIndexPath*>* paths = [NSMutableArray array];
  for (size_t i = 0; i < patches_.size(); ++i) {
    [paths addObject:[NSIndexPath indexPathForRow:patches_row + 1 + static_cast<NSInteger>(i)
                                        inSection:0]];
  }
  patches_expanded_ = !patches_expanded_;
  [table_view_ performBatchUpdates:^{
    if (patches_expanded_) {
      [table_view_ insertRowsAtIndexPaths:paths withRowAnimation:UITableViewRowAnimationFade];
    } else {
      [table_view_ deleteRowsAtIndexPaths:paths withRowAnimation:UITableViewRowAnimationFade];
    }
  }
                        completion:nil];
  // Patches keeps its place, so its chevron and count change in place.
  [self refreshPatchesRow:YES];
  [self updateSeparators];
  if (patches_expanded_) {
    // The switches read under their heading, so Patches goes to the top when they don't fit.
    const CGFloat needed = table_view_.estimatedRowHeight * (paths.count + 1);
    if (needed > CGRectGetHeight(table_view_.bounds) - table_view_.contentInset.bottom) {
      [table_view_ scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:patches_row inSection:0]
                         atScrollPosition:UITableViewScrollPositionTop
                                 animated:YES];
    } else {
      [table_view_ scrollToRowAtIndexPath:paths.lastObject
                         atScrollPosition:UITableViewScrollPositionNone
                                 animated:YES];
    }
  }
}

- (void)patchSwitchChanged:(UISwitch*)sender {
  const NSInteger index = sender.tag;
  if (index < 0 || index >= static_cast<NSInteger>(patches_.size())) {
    return;
  }
  auto& entry = patches_[static_cast<size_t>(index)];
  std::string status;
  const bool saved = app_context_ && app_context_->SetPatchEnabled(
                                         title_id_, entry.first, entry.second.patch_index,
                                         sender.on, &status);
  if (!saved) {
    sender.on = !sender.on;
    XEPresentOKAlert(
        self, NSLocalizedString(@"Patch Not Saved", @"game patches view controller: user-visible text"),
        status.empty() ? NSLocalizedString(@"Failed to update patch state.",
                                           @"game patches view controller: user-visible text")
                       : ToNSString(status));
    return;
  }
  entry.second.is_enabled = sender.on;
  [self refreshPatchesRow:NO];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  const NSInteger patch_index = [self patchIndexForRow:indexPath.row];
  if (patch_index >= 0) {
    // The whole row flips its switch.
    UISwitch* toggle = (UISwitch*)[tableView cellForRowAtIndexPath:indexPath].accessoryView;
    if ([toggle isKindOfClass:[UISwitch class]]) {
      [toggle setOn:!toggle.on animated:YES];
      [self patchSwitchChanged:toggle];
    }
    return;
  }
  XeniaIOSGameActionRow row = [self rowAtIndexPath:indexPath];
  if (row.action == XeniaIOSGameActionPatches && app_context_) {
    [self togglePatchesAtRow:indexPath.row];
    return;
  }
  [self perform:row.action];
}

@end
