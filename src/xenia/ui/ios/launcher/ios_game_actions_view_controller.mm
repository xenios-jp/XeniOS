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
#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/launcher/ios_content_management.h"
#import "xenia/ui/ios/launcher/ios_game_art.h"
#import "xenia/ui/ios/launcher/ios_game_content_view_controller.h"
#import "xenia/ui/ios/launcher/ios_game_patches_view_controller.h"
#import "xenia/ui/ios/launcher/ios_system_link_view_controller.h"
#import "xenia/ui/ios/launcher/ios_zar_conversion_coordinator.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_system_link.h"
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
NSString* const kChildCellIdentifier = @"XeniaIOSGameChildCell";

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
  // A symbol alone: its name shows on hover with a pointer, and on a press and hold at the
  // larger text sizes.
  button.toolTip = label;
  button.showsLargeContentViewer = YES;
  button.largeContentTitle = label;
  button.largeContentImage = [UIImage systemImageNamed:symbol];
  [button addInteraction:[[[UILargeContentViewerInteraction alloc] init] autorelease]];
  [NSLayoutConstraint activateConstraints:@[
    [button.widthAnchor constraintEqualToConstant:side],
    [button.heightAnchor constraintEqualToConstant:side],
  ]];
  return button;
}

}  // namespace

// What a row under a heading is.
typedef NS_ENUM(NSInteger, XeniaGameRowChildKind) {
  XeniaGameRowChildKindInfo,    // reads only
  XeniaGameRowChildKindItem,    // a thing to pick, such as a disc
  XeniaGameRowChildKindButton,  // does something
  XeniaGameRowChildKindSwitch,
  XeniaGameRowChildKindCheck,   // one of a set; |on| is the chosen one
};

// A row under a heading on the game page.
@interface XeniaGameRowChild : NSObject
@property(nonatomic, assign) XeniaGameRowChildKind kind;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* detail;
@property(nonatomic, assign) BOOL on;
@property(nonatomic, assign) BOOL destructive;
// It opens a page of its own, so it has a chevron.
@property(nonatomic, assign) BOOL opensPage;
// A tap, or a switch flipped (|on| is already the new state).
@property(nonatomic, copy) void (^handler)(XeniaGameRowChild* child, UIView* source);
// Swiping it away: the swipe action's title and what it does.
@property(nonatomic, copy) NSString* deleteTitle;
@property(nonatomic, copy) void (^deleteHandler)(UIView* source);
@end

@implementation XeniaGameRowChild
- (void)dealloc {
  [_title release];
  [_detail release];
  [_handler release];
  [_deleteTitle release];
  [_deleteHandler release];
  [super dealloc];
}
@end

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
  BOOL shows_rows_only_;
  // The rows open under each heading that is open, by action.
  NSMutableDictionary<NSNumber*, NSArray<XeniaGameRowChild*>*>* shown_children_;
  std::vector<IOSInstalledContentEntry> content_;
  BOOL content_loaded_;
  uint64_t content_generation_;
  XeniaGameContentImporter* content_importer_;
  id<XeniaGameContentHost> content_host_;  // not retained
  NSArray<NSString*>* disc_labels_;
  NSArray<NSString*>* disc_details_;
  void (^disc_handler_)(NSUInteger disc_index);
  NSString* compatibility_status_;
  NSString* compatibility_performance_;
  NSString* compatibility_notes_;
  void (^conversion_choices_loader_)(void (^)(NSString*, BOOL,
                                              NSArray<XeniaIOSZarConversionChoice*>*));
  NSString* conversion_summary_;
  NSArray<XeniaIOSZarConversionChoice*>* conversion_choices_;
  BOOL conversion_loading_;
}

@synthesize actionHandler = action_handler_;
@synthesize statusText = status_text_;
@synthesize statusColor = status_color_;
@synthesize appContext = app_context_;
@synthesize showsRowsOnly = shows_rows_only_;
@synthesize discLabels = disc_labels_;
@synthesize discDetails = disc_details_;
@synthesize discHandler = disc_handler_;
@synthesize contentHost = content_host_;
@synthesize compatibilityStatus = compatibility_status_;
@synthesize compatibilityPerformance = compatibility_performance_;
@synthesize compatibilityNotes = compatibility_notes_;
@synthesize conversionChoicesLoader = conversion_choices_loader_;

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
    shown_children_ = [[NSMutableDictionary alloc] init];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [shown_children_ release];
  [content_importer_ release];
  [disc_labels_ release];
  [disc_details_ release];
  [disc_handler_ release];
  [compatibility_status_ release];
  [compatibility_performance_ release];
  [compatibility_notes_ release];
  [conversion_choices_loader_ release];
  [conversion_summary_ release];
  [conversion_choices_ release];
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
  // On the phone page this is the one place compatibility shows: a row reads as what it is.
  if (supports_compatibility_ && shows_rows_only_) {
    add(XeniaIOSGameActionCompatibility,
        NSLocalizedString(@"Compatibility", @"game page: row"), @"checkmark.seal");
  }
  if (supports_patches_) {
    add(XeniaIOSGameActionPatches,
        NSLocalizedString(@"Patches", @"game page: row"), @"puzzlepiece.extension");
  }
  if (supports_manage_content_) {
    add(XeniaIOSGameActionManageContent,
        NSLocalizedString(@"Updates & Add-ons", @"game page: row"), @"square.stack.3d.up");
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
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center addObserver:self
             selector:@selector(contentChanged:)
                 name:XeniaGameContentDidChangeNotification
               object:nil];
  [center addObserver:self
             selector:@selector(systemLinkChanged:)
                 name:XeniaIOSSystemLinkOnlineDidChangeNotification
               object:nil];
  // The headings say what they're set to, so what they need loads now.
  if (supports_patches_ && app_context_) {
    [self loadPatches];
  }
  if (supports_manage_content_) {
    [self contentChanged:nil];
  }
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
  // Another page may have changed the patches while this one was under it.
  if (patches_loaded_) {
    [self loadPatches];
    [self refreshChildrenForAction:XeniaIOSGameActionPatches];
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
  // With compatibility data, the status line below says it instead, a tap from the reports.
  if (status_text_.length && !supports_compatibility_) {
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
  if (supports_compatibility_) {
    UIButton* capsule = xe_new_compat_capsule();  // the status line, under the title
    __unsafe_unretained XeniaIOSGameActionsViewController* unsafe_self = self;
    xe_update_compat_capsule(capsule, compatibility_status_, compatibility_performance_,
                             compatibility_notes_, ^{
                               [unsafe_self perform:XeniaIOSGameActionCompatibility];
                             });
    capsule.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
    [title_stack_ addArrangedSubview:capsule];
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
    // Beside the art the rows start at the top of the screen, so the first one starts below the
    // fade rather than in it.
    const CGFloat top = size.width > size.height ? 16.0 : 0.0;
    const BOOL at_top = table_view_.contentOffset.y <= -table_view_.contentInset.top + 1.0;
    table_view_.contentInset = UIEdgeInsetsMake(top, 0.0, safe.bottom + 16.0, 0.0);
    if (at_top) {
      table_view_.contentOffset = CGPointMake(0.0, -top);
    }
    [self applyTableInsets];
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

  [self applyTableInsets];

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

// Walks the content tree off the main thread; a newer load supersedes one still in flight.
- (void)loadContentThen:(void (^)(void))then {
  const uint64_t generation = ++content_generation_;
  const uint32_t title_id = title_id_;
  void (^then_copy)(void) = [[then copy] autorelease];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    std::vector<IOSInstalledContentEntry> content = xe_list_installed_content(title_id);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (generation != content_generation_) {
        return;
      }
      content_ = std::move(content);
      content_loaded_ = YES;
      if (then_copy) {
        then_copy();
      }
    });
  });
}

- (void)contentChanged:(NSNotification* __unused)note {
  [self loadContentThen:^{
    [self refreshChildrenForAction:XeniaIOSGameActionManageContent];
  }];
}

- (void)systemLinkChanged:(NSNotification* __unused)note {
  [self refreshChildrenForAction:XeniaIOSGameActionSystemLink];
}

#pragma mark - Rows and what opens under them

- (XeniaIOSGameActionRow)actionRowAtIndex:(NSUInteger)index {
  XeniaIOSGameActionRow row = {};
  [rows_[index] getValue:&row];
  return row;
}

- (NSArray<XeniaGameRowChild*>*)shownChildrenForAction:(XeniaIOSGameAction)action {
  return shown_children_[@(action)];
}

// The table row an action's heading is on, or NSNotFound.
- (NSInteger)tableRowForAction:(XeniaIOSGameAction)action {
  NSInteger table_row = 0;
  for (NSUInteger i = 0; i < rows_.count; ++i) {
    const XeniaIOSGameActionRow row = [self actionRowAtIndex:i];
    if (row.action == action) {
      return table_row;
    }
    table_row += 1 + NSInteger([self shownChildrenForAction:row.action].count);
  }
  return NSNotFound;
}

// What a table row shows: a heading, or one of the rows open under it.
- (XeniaIOSGameActionRow)rowForTableRow:(NSInteger)table_row child:(XeniaGameRowChild**)child {
  *child = nil;
  NSInteger at = 0;
  for (NSUInteger i = 0; i < rows_.count; ++i) {
    const XeniaIOSGameActionRow row = [self actionRowAtIndex:i];
    if (at == table_row) {
      return row;
    }
    NSArray<XeniaGameRowChild*>* children = [self shownChildrenForAction:row.action];
    if (table_row <= at + NSInteger(children.count)) {
      *child = children[NSUInteger(table_row - at - 1)];
      return row;
    }
    at += 1 + NSInteger(children.count);
  }
  return XeniaIOSGameActionRow{};
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  NSInteger count = NSInteger(rows_.count);
  for (NSArray* children in shown_children_.allValues) {
    count += NSInteger(children.count);
  }
  return section == 0 ? count : 0;
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  // The table's safe area follows the frame set above only once the view has laid out, so the
  // rows take their margins again; otherwise they keep the full-width sensor-housing inset.
  [self applyTableInsets];

}

- (void)applyTableInsets {
  const UIEdgeInsets table_safe = table_view_.safeAreaInsets;
  const CGFloat symbol = shows_rows_only_ ? kSymbolWidth + kSymbolPadding : 40.0;
  table_view_.separatorInset = UIEdgeInsetsMake(0.0, table_safe.left + kMargin + symbol, 0.0,
                                                table_safe.right + kMargin);
  for (UITableViewCell* cell in table_view_.visibleCells) {
    [self applyMarginsToCell:cell];
  }
  // Each row copies the table's separator inset, so they follow it too.
  [self updateSeparators];
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
    // These margins start from the safe area already. Rows under a heading start where its
    // title does.
    const BOOL child = [cell.reuseIdentifier isEqualToString:kChildCellIdentifier];
    content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(
        child ? 10.0 : 0.0, kMargin - 4.0 + (child ? kSymbolWidth + kSymbolPadding : 0.0),
        child ? 10.0 : 0.0, 0.0);
    cell.contentConfiguration = content;
  }
}

// What a heading says on its trailing side: "2 On", "Playable", "Public Lobby".
- (NSString*)summaryForAction:(XeniaIOSGameAction)action {
  switch (action) {
    case XeniaIOSGameActionCompatibility:
      return status_text_.length
                 ? status_text_
                 : NSLocalizedString(@"Not Tested", @"game page: Compatibility row; no reports");
    case XeniaIOSGameActionPatches: {
      if (!patches_loaded_ || patches_.empty()) {
        return nil;
      }
      NSUInteger enabled = 0;
      for (const auto& entry : patches_) {
        enabled += entry.second.is_enabled ? 1 : 0;
      }
      return enabled ? [NSString stringWithFormat:NSLocalizedString(
                                                      @"%lu On", @"game page: Patches row; %lu "
                                                                 @"is how many patches are on"),
                                                  static_cast<unsigned long>(enabled)]
                     : NSLocalizedString(@"Off", @"game page: Patches row; no patch is on");
    }
    case XeniaIOSGameActionManageContent:
      if (!content_loaded_) {
        return nil;
      }
      return content_.empty()
                 ? NSLocalizedString(@"None", @"game page: Updates & Add-ons row; nothing added")
                 : [NSString localizedStringWithFormat:@"%lu", (unsigned long)content_.size()];
    case XeniaIOSGameActionSystemLink: {
      if (!xe_ios_online_features_enabled()) {
        return NSLocalizedString(@"Off", @"game page: System Link row; Online Features are off");
      }
      NSString* code = xe_ios_system_link_room(title_id_);
      return code ? [NSString stringWithFormat:NSLocalizedString(
                                                   @"Room %@", @"game page: System Link row; %@ "
                                                               @"is the private room's code"),
                                               code]
                  : NSLocalizedString(@"Public Lobby", @"game page: System Link row");
    }
    case XeniaIOSGameActionLaunchDisc:
      return disc_labels_.count
                 ? [NSString localizedStringWithFormat:NSLocalizedString(
                                                           @"%lu Discs",
                                                           @"game page: Launch Disc row"),
                                                       (unsigned long)disc_labels_.count]
                 : nil;
    default:
      return nil;
  }
}

- (XeniaGameRowChild*)newChild:(XeniaGameRowChildKind)kind
                         title:(NSString*)title
                        detail:(NSString*)detail {
  XeniaGameRowChild* child = [[[XeniaGameRowChild alloc] init] autorelease];
  child.kind = kind;
  child.title = title;
  child.detail = detail;
  return child;
}

// The rows that open under a heading, from what is known now.
- (NSArray<XeniaGameRowChild*>*)childrenForAction:(XeniaIOSGameAction)action {
  NSMutableArray<XeniaGameRowChild*>* children = [NSMutableArray array];
  __unsafe_unretained XeniaIOSGameActionsViewController* unsafe_self = self;
  switch (action) {
    case XeniaIOSGameActionPatches: {
      if (!patches_loaded_) {
        [self loadPatches];
      }
      if (patches_.empty()) {
        [children addObject:[self newChild:XeniaGameRowChildKindInfo
                                     title:NSLocalizedString(@"No patches for this game.",
                                                             @"game page: under Patches")
                                    detail:NSLocalizedString(
                                               @"Add its .patch.toml files to the patches "
                                               @"folder in Files.",
                                               @"game page: under Patches")]];
        break;
      }
      for (size_t i = 0; i < patches_.size(); ++i) {
        const auto& patch = patches_[i].second;
        XeniaGameRowChild* child =
            [self newChild:XeniaGameRowChildKindSwitch
                     title:XeniaPatchDisplayName(patch)
                    detail:patch.description.empty() ? nil : ToNSString(patch.description)];
        child.on = patch.is_enabled;
        child.handler = ^(XeniaGameRowChild* toggled, UIView* __unused source) {
          [unsafe_self setPatch:i enabled:toggled.on];
        };
        [children addObject:child];
      }
    } break;
    case XeniaIOSGameActionManageContent: {
      for (const IOSInstalledContentEntry& entry : content_) {
        NSString* kind = xe_installed_content_kind_label(entry.kind);
        XeniaGameRowChild* child =
            [self newChild:XeniaGameRowChildKindInfo
                     title:ToNSString(entry.name)
                    detail:entry.linked
                               ? [NSString stringWithFormat:NSLocalizedString(
                                                                @"%@ · Linked",
                                                                @"game page: an add-on linked "
                                                                @"from a folder; %@ is its kind"),
                                                            kind]
                               : kind];
        child.deleteTitle =
            entry.linked ? NSLocalizedString(@"Unlink", @"game page: swipe action")
                         : NSLocalizedString(@"Delete", @"game page: swipe action");
        const IOSInstalledContentEntry entry_copy = entry;
        child.deleteHandler = ^(UIView* source) {
          XeniaConfirmRemoveInstalledContent(
              unsafe_self, entry_copy, source,
              ^{
                [unsafe_self.contentHost refreshImportedGamesAsync];
              },
              nil);
        };
        [children addObject:child];
      }
      XeniaGameRowChild* add =
          [self newChild:XeniaGameRowChildKindButton
                   title:NSLocalizedString(@"Add from Files…", @"game page: under Updates & "
                                                               @"Add-ons")
                  detail:content_.empty()
                             ? NSLocalizedString(@"Title updates and DLC for this game.",
                                                 @"game page: under Updates & Add-ons")
                             : nil];
      add.handler = ^(XeniaGameRowChild* __unused tapped, UIView* __unused source) {
        [unsafe_self addContent];
      };
      [children addObject:add];
    } break;
    case XeniaIOSGameActionSystemLink:
      [self addSystemLinkChildrenTo:children];
      break;
    case XeniaIOSGameActionLaunchDisc:
      for (NSUInteger i = 0; i < disc_labels_.count; ++i) {
        XeniaGameRowChild* child =
            [self newChild:XeniaGameRowChildKindItem
                     title:disc_labels_[i]
                    detail:i < disc_details_.count ? disc_details_[i] : nil];
        child.handler = ^(XeniaGameRowChild* __unused tapped, UIView* __unused source) {
          if (unsafe_self.discHandler) {
            unsafe_self.discHandler(i);
          }
        };
        [children addObject:child];
      }
      break;
    case XeniaIOSGameActionConvertToZar:
      [children addObject:[self newChild:XeniaGameRowChildKindInfo
                                   title:conversion_summary_
                                             ?: NSLocalizedString(@"Estimating its size…",
                                                                  @"game page: Convert to ZAR")
                                  detail:nil]];
      for (XeniaIOSZarConversionChoice* choice in conversion_choices_) {
        XeniaGameRowChild* child = [self newChild:XeniaGameRowChildKindButton
                                            title:choice.title
                                           detail:choice.detail];
        child.destructive = choice.destructive;
        void (^start)(void) = choice.handler;
        child.handler = ^(XeniaGameRowChild* __unused tapped, UIView* __unused source) {
          if (start) {
            start();
          }
        };
        [children addObject:child];
      }
      break;
    default:
      break;
  }
  return children;
}

// The room choices from the System Link sheet: the public lobby or a private room.
- (void)addSystemLinkChildrenTo:(NSMutableArray<XeniaGameRowChild*>*)children {
  __unsafe_unretained XeniaIOSGameActionsViewController* unsafe_self = self;
  const uint32_t title_id = title_id_;
  if (!xe_ios_online_features_enabled()) {
    XeniaGameRowChild* turn_on =
        [self newChild:XeniaGameRowChildKindButton
                 title:NSLocalizedString(@"Turn On Online Features",
                                         @"game page: under System Link")
                detail:NSLocalizedString(@"System Link plays over the internet.",
                                         @"game page: under System Link")];
    turn_on.handler = ^(XeniaGameRowChild* __unused tapped, UIView* __unused source) {
      xe_ios_confirm_turn_on_online_features(unsafe_self, ^{
        xe_ios_set_online_features_enabled(YES);
        [unsafe_self refreshChildrenForAction:XeniaIOSGameActionSystemLink];
      });
    };
    [children addObject:turn_on];
    return;
  }
  NSString* code = xe_ios_system_link_room(title_id);
  NSString* lobby_detail = NSLocalizedString(@"Everyone playing this game.",
                                             @"game page: under System Link");
  if (NSString* players = xe_ios_system_link_lobby_players_text(title_id)) {
    lobby_detail = [lobby_detail stringByAppendingFormat:@" %@", players];
  }
  XeniaGameRowChild* lobby =
      [self newChild:XeniaGameRowChildKindCheck
               title:NSLocalizedString(@"Public Lobby", @"game page: under System Link")
              detail:lobby_detail];
  lobby.on = code == nil;
  lobby.handler = ^(XeniaGameRowChild* __unused tapped, UIView* __unused source) {
    xe_ios_set_system_link_room(title_id, nil);
    [unsafe_self refreshChildrenForAction:XeniaIOSGameActionSystemLink];
  };
  [children addObject:lobby];
  if (code) {
    XeniaGameRowChild* room = [self
        newChild:XeniaGameRowChildKindCheck
           title:[NSString stringWithFormat:NSLocalizedString(@"Private Room %@",
                                                              @"game page: under System Link; "
                                                              @"%@ is the room code"),
                                            code]
          detail:NSLocalizedString(@"Only players with the code.",
                                   @"game page: under System Link")];
    room.on = YES;
    [children addObject:room];
  }
  XeniaGameRowChild* create =
      [self newChild:XeniaGameRowChildKindButton
               title:NSLocalizedString(@"Create Private Room", @"game page: under System Link")
              detail:nil];
  create.handler = ^(XeniaGameRowChild* __unused tapped, UIView* source) {
    xe_ios_set_system_link_room(title_id, xe_ios_new_system_link_room_code());
    [unsafe_self refreshChildrenForAction:XeniaIOSGameActionSystemLink];
    xe_ios_share_system_link_invite(unsafe_self, title_id, unsafe_self->game_title_, source);
  };
  [children addObject:create];
  XeniaGameRowChild* join =
      [self newChild:XeniaGameRowChildKindButton
               title:NSLocalizedString(@"Join with Code…", @"game page: under System Link")
              detail:nil];
  join.handler = ^(XeniaGameRowChild* __unused tapped, UIView* __unused source) {
    xe_ios_present_system_link_join_alert(unsafe_self, title_id, ^{
      [unsafe_self refreshChildrenForAction:XeniaIOSGameActionSystemLink];
    });
  };
  [children addObject:join];
  if (code) {
    XeniaGameRowChild* share =
        [self newChild:XeniaGameRowChildKindButton
                 title:NSLocalizedString(@"Share Invite…", @"game page: under System Link")
                detail:nil];
    share.handler = ^(XeniaGameRowChild* __unused tapped, UIView* source) {
      xe_ios_share_system_link_invite(unsafe_self, title_id, unsafe_self->game_title_, source);
    };
    [children addObject:share];
  }
}

- (UITableViewCell*)childCell:(XeniaGameRowChild*)child tableView:(UITableView*)tableView {
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kChildCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kChildCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor clearColor];
  }
  [self configureChildCell:cell child:child];
  return cell;
}

// A row under a heading: its title, a line or two under it, and a switch or a checkmark when it
// has one. Rows that do something read in the accent colour; information reads quieter.
- (void)configureChildCell:(UITableViewCell*)cell child:(XeniaGameRowChild*)child {
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.text = child.title;
  const BOOL button = child.kind == XeniaGameRowChildKindButton;
  content.textProperties.color = child.destructive ? [UIColor systemRedColor]
                                 : button          ? [XeniaTheme accent]
                                 : child.kind == XeniaGameRowChildKindInfo
                                     ? [XeniaTheme textSecondary]
                                     : [XeniaTheme textPrimary];
  content.textProperties.font =
      xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  if (child.detail.length) {
    content.secondaryText = child.detail;
    content.secondaryTextProperties.color = [XeniaTheme textSecondary];
    content.secondaryTextProperties.font =
        xe_scaled_system_font(UIFontTextStyleSubheadline, 14.0, UIFontWeightRegular);
    content.secondaryTextProperties.numberOfLines = 3;
    content.textToSecondaryTextVerticalPadding = 2.0;
  }
  cell.contentConfiguration = content;
  cell.accessoryView = nil;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.tintColor = [XeniaTheme accent];
  switch (child.kind) {
    case XeniaGameRowChildKindSwitch: {
      UISwitch* toggle = [[[UISwitch alloc] init] autorelease];
      toggle.on = child.on;
      toggle.accessibilityLabel = child.title;
      __unsafe_unretained XeniaIOSGameActionsViewController* unsafe_self = self;
      [toggle addAction:[UIAction actionWithHandler:^(UIAction* action) {
                [unsafe_self child:child switchedTo:((UISwitch*)action.sender).on];
              }]
          forControlEvents:UIControlEventValueChanged];
      cell.accessoryView = toggle;
    } break;
    case XeniaGameRowChildKindCheck:
      cell.accessoryType =
          child.on ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
      break;
    default:
      break;
  }
  if (child.opensPage) {
    cell.accessoryView = [self newChevron];
  }
  cell.selectionStyle = child.kind == XeniaGameRowChildKindInfo
                            ? UITableViewCellSelectionStyleNone
                            : UITableViewCellSelectionStyleDefault;
  cell.accessibilityTraits =
      (child.kind == XeniaGameRowChildKindInfo ? UIAccessibilityTraitStaticText
                                               : UIAccessibilityTraitButton) |
      (child.kind == XeniaGameRowChildKindCheck && child.on ? UIAccessibilityTraitSelected : 0);
}

- (UIImageView*)newChevron {
  UIImageView* chevron = [[[UIImageView alloc]
      initWithImage:[UIImage
                        systemImageNamed:@"chevron.right"
                        withConfiguration:[UIImageSymbolConfiguration
                                              configurationWithPointSize:14.0
                                                                  weight:
                                                                      UIImageSymbolWeightSemibold]]]
      autorelease];
  chevron.tintColor = [UIColor tertiaryLabelColor];
  return chevron;
}

// Every heading is one 53-pt row (rule 3's 52-pt row plus the hairline), scaled with the text
// size. Self-sizing gave a heading with no value, Convert to ZAR, ~30 pt more than the rest;
// the rows under a heading still size to what they hold.
- (CGFloat)tableView:(UITableView*)tableView heightForRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaGameRowChild* child = nil;
  [self rowForTableRow:indexPath.row child:&child];
  if (child) {
    return UITableViewAutomaticDimension;
  }
  return ceil([[UIFontMetrics metricsForTextStyle:UIFontTextStyleBody]
      scaledValueForValue:53.0 compatibleWithTraitCollection:tableView.traitCollection]);
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaGameRowChild* child = nil;
  [self rowForTableRow:indexPath.row child:&child];
  UITableViewCell* cell = child ? [self childCell:child tableView:tableView]
                                : [self actionCellAtIndexPath:indexPath tableView:tableView];
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

// A heading: its symbol, title, what it is set to, and a chevron that turns down while it's open.
- (void)configureActionCell:(UITableViewCell*)cell atIndexPath:(NSIndexPath*)indexPath {
  XeniaGameRowChild* child = nil;
  const XeniaIOSGameActionRow row = [self rowForTableRow:indexPath.row child:&child];
  NSString* summary = [self summaryForAction:row.action];
  // Every heading takes the value layout, with or without a value, so rows share one height.
  UIListContentConfiguration* content = [UIListContentConfiguration valueCellConfiguration];
  content.text = row.title;
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.font =
      xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  content.image = [UIImage systemImageNamed:row.symbol];
  content.imageProperties.tintColor = [XeniaTheme textSecondary];
  content.imageToTextPadding = kSymbolPadding;
  content.imageProperties.reservedLayoutSize = CGSizeMake(kSymbolWidth, 0.0);
  if (summary) {
    content.secondaryText = summary;
    content.secondaryTextProperties.color = [XeniaTheme textSecondary];
    content.secondaryTextProperties.font =
        xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
    // Status is a dot and words (rule 5).
    if (row.action == XeniaIOSGameActionCompatibility && status_color_) {
      NSMutableAttributedString* value = [[[NSMutableAttributedString alloc]
          initWithString:[@"\u25CF  " stringByAppendingString:summary]
              attributes:@{NSForegroundColorAttributeName : [XeniaTheme textSecondary]}]
          autorelease];
      [value addAttribute:NSForegroundColorAttributeName value:status_color_
                    range:NSMakeRange(0, 1)];
      content.secondaryAttributedText = value;
    }
  }
  cell.contentConfiguration = content;
  const BOOL open = [self shownChildrenForAction:row.action] != nil;
  UIImageView* chevron = [self newChevron];
  chevron.transform = open ? CGAffineTransformMakeRotation(M_PI_2) : CGAffineTransformIdentity;
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.accessoryView = chevron;
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  cell.accessibilityValue =
      [NSString stringWithFormat:@"%@%@", summary ? [summary stringByAppendingString:@", "] : @"",
                                 open ? NSLocalizedString(@"Expanded", @"game page: row state")
                                      : NSLocalizedString(@"Collapsed", @"game page: row state")];
}

// A heading's summary and chevron, on the cell already showing.
- (void)refreshHeadingForAction:(XeniaIOSGameAction)action animated:(BOOL)animated {
  const NSInteger table_row = [self tableRowForAction:action];
  if (table_row == NSNotFound) {
    return;
  }
  NSIndexPath* path = [NSIndexPath indexPathForRow:table_row inSection:0];
  UITableViewCell* cell = [table_view_ cellForRowAtIndexPath:path];
  if (!cell) {
    return;
  }
  const CGAffineTransform turned = cell.accessoryView.transform;
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

- (NSArray<NSIndexPath*>*)childPathsFromRow:(NSInteger)heading_row count:(NSUInteger)count {
  NSMutableArray<NSIndexPath*>* paths = [NSMutableArray array];
  for (NSUInteger i = 0; i < count; ++i) {
    [paths addObject:[NSIndexPath indexPathForRow:heading_row + 1 + NSInteger(i) inSection:0]];
  }
  return paths;
}

// Opens or closes the rows under a heading. Some of what they show loads first.
- (void)toggleAction:(XeniaIOSGameAction)action {
  if (![self shownChildrenForAction:action]) {
    if (action == XeniaIOSGameActionManageContent && !content_loaded_) {
      [self loadContentThen:^{
        [self toggleAction:action];
      }];
      return;
    }
    if (action == XeniaIOSGameActionSystemLink) {
      xe_ios_system_link_refresh_online();
    }
    if (action == XeniaIOSGameActionConvertToZar && conversion_choices_loader_ &&
        !conversion_loading_) {
      conversion_loading_ = YES;
      __unsafe_unretained XeniaIOSGameActionsViewController* unsafe_self = self;
      conversion_choices_loader_(^(NSString* summary, BOOL __unused estimating,
                                   NSArray<XeniaIOSZarConversionChoice*>* choices) {
        [unsafe_self->conversion_summary_ release];
        unsafe_self->conversion_summary_ = [summary copy];
        [unsafe_self->conversion_choices_ release];
        unsafe_self->conversion_choices_ = [choices copy];
        [unsafe_self refreshChildrenForAction:XeniaIOSGameActionConvertToZar];
      });
    }
  }
  if ([self tableRowForAction:action] == NSNotFound) {
    return;
  }
  NSArray<XeniaGameRowChild*>* shown = [self shownChildrenForAction:action];
  NSArray<XeniaGameRowChild*>* children = shown ? nil : [self childrenForAction:action];
  // One heading is open at a time, so the page keeps its shape: opening one closes the other.
  NSMutableArray<NSNumber*>* closing = [NSMutableArray array];
  NSMutableArray<NSIndexPath*>* removed = [NSMutableArray array];
  for (NSNumber* open in shown_children_.allKeys) {
    if (shown && open.integerValue != action) {
      continue;
    }
    const XeniaIOSGameAction open_action = XeniaIOSGameAction(open.integerValue);
    [closing addObject:open];
    [removed addObjectsFromArray:[self childPathsFromRow:[self tableRowForAction:open_action]
                                                   count:shown_children_[open].count]];
  }
  __block NSInteger heading_row = NSNotFound;
  __block NSArray<NSIndexPath*>* paths = nil;
  [table_view_ performBatchUpdates:^{
    [shown_children_ removeObjectsForKeys:closing];
    [table_view_ deleteRowsAtIndexPaths:removed withRowAnimation:UITableViewRowAnimationFade];
    heading_row = [self tableRowForAction:action];
    if (!shown) {
      shown_children_[@(action)] = children;
      paths = [self childPathsFromRow:heading_row count:children.count];
      [table_view_ insertRowsAtIndexPaths:paths withRowAnimation:UITableViewRowAnimationFade];
    }
  }
                        completion:nil];
  // The headings keep their places, so their chevrons and summaries change in place.
  for (NSNumber* closed in closing) {
    if (closed.integerValue != action) {
      [self refreshHeadingForAction:XeniaIOSGameAction(closed.integerValue) animated:YES];
    }
  }
  [self refreshHeadingForAction:action animated:YES];
  [self updateSeparators];
  if (children.count) {
    // The rows read under their heading, so it goes to the top when they don't fit.
    const CGFloat needed = table_view_.estimatedRowHeight * (paths.count + 1);
    if (needed > CGRectGetHeight(table_view_.bounds) - table_view_.contentInset.bottom) {
      [table_view_ scrollToRowAtIndexPath:[NSIndexPath indexPathForRow:heading_row inSection:0]
                         atScrollPosition:UITableViewScrollPositionTop
                                 animated:YES];
    } else {
      [table_view_ scrollToRowAtIndexPath:paths.lastObject
                         atScrollPosition:UITableViewScrollPositionNone
                                 animated:YES];
    }
  }
}

// What's open under a heading changed: the rows follow, in place when there are as many.
- (void)refreshChildrenForAction:(XeniaIOSGameAction)action {
  NSArray<XeniaGameRowChild*>* shown = [self shownChildrenForAction:action];
  if (shown && table_view_) {
    NSArray<XeniaGameRowChild*>* children = [self childrenForAction:action];
    const NSInteger heading_row = [self tableRowForAction:action];
    if (children.count == shown.count) {
      shown_children_[@(action)] = children;
      for (NSUInteger i = 0; i < children.count; ++i) {
        UITableViewCell* cell = [table_view_
            cellForRowAtIndexPath:[NSIndexPath indexPathForRow:heading_row + 1 + NSInteger(i)
                                                     inSection:0]];
        if (cell) {
          [self configureChildCell:cell child:children[i]];
          [self applyMarginsToCell:cell];
        }
      }
    } else {
      [table_view_ performBatchUpdates:^{
        shown_children_[@(action)] = children;
        [table_view_ deleteRowsAtIndexPaths:[self childPathsFromRow:heading_row count:shown.count]
                           withRowAnimation:UITableViewRowAnimationFade];
        [table_view_
            insertRowsAtIndexPaths:[self childPathsFromRow:heading_row count:children.count]
                  withRowAnimation:UITableViewRowAnimationFade];
      }
                            completion:nil];
      [self updateSeparators];
    }
  }
  [self refreshHeadingForAction:action animated:NO];
}

- (void)child:(XeniaGameRowChild*)child switchedTo:(BOOL)on {
  child.on = on;
  if (child.handler) {
    child.handler(child, nil);
  }
}

- (void)setPatch:(size_t)index enabled:(BOOL)enabled {
  if (index >= patches_.size()) {
    return;
  }
  auto& entry = patches_[index];
  std::string status;
  const bool saved = app_context_ && app_context_->SetPatchEnabled(
                                         title_id_, entry.first, entry.second.patch_index,
                                         enabled, &status);
  if (!saved) {
    XEPresentOKAlert(
        self, NSLocalizedString(@"Patch Not Saved", @"game patches view controller: user-visible text"),
        status.empty() ? NSLocalizedString(@"Failed to update patch state.",
                                           @"game patches view controller: user-visible text")
                       : ToNSString(status));
  } else {
    entry.second.is_enabled = enabled;
  }
  // A switch that didn't save goes back.
  [self refreshChildrenForAction:XeniaIOSGameActionPatches];
}

- (void)addContent {
  if (!content_importer_) {
    content_importer_ = [[XeniaGameContentImporter alloc] initWithTitleID:title_id_
                                                                     host:content_host_
                                                                presenter:self];
  }
  [content_importer_ present];
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  XeniaGameRowChild* child = nil;
  const XeniaIOSGameActionRow row = [self rowForTableRow:indexPath.row child:&child];
  if (child) {
    UITableViewCell* cell = [tableView cellForRowAtIndexPath:indexPath];
    switch (child.kind) {
      case XeniaGameRowChildKindInfo:
        break;
      case XeniaGameRowChildKindSwitch: {
        // The whole row flips its switch.
        UISwitch* toggle = (UISwitch*)cell.accessoryView;
        if ([toggle isKindOfClass:[UISwitch class]]) {
          [toggle setOn:!toggle.on animated:YES];
        }
        [self child:child switchedTo:!child.on];
      } break;
      default:
        if (child.handler) {
          child.handler(child, cell ?: tableView);
        }
        break;
    }
    return;
  }
  // Compatibility is its own page; every other heading opens in place.
  if (row.action == XeniaIOSGameActionCompatibility) {
    [self perform:XeniaIOSGameActionCompatibility];
    return;
  }
  [self toggleAction:row.action];
}

- (UISwipeActionsConfiguration*)tableView:(UITableView*)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaGameRowChild* child = nil;
  [self rowForTableRow:indexPath.row child:&child];
  if (!child.deleteHandler) {
    return [UISwipeActionsConfiguration configurationWithActions:@[]];
  }
  void (^remove)(UIView*) = child.deleteHandler;
  UIContextualAction* action = [UIContextualAction
      contextualActionWithStyle:UIContextualActionStyleDestructive
                          title:child.deleteTitle
                        handler:^(UIContextualAction* __unused contextual, UIView* source,
                                  void (^completion)(BOOL)) {
                          remove([tableView cellForRowAtIndexPath:indexPath] ?: source);
                          completion(YES);
                        }];
  return [UISwipeActionsConfiguration configurationWithActions:@[ action ]];
}

@end
