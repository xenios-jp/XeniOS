/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_actions_view_controller.h"

#import "xenia/ui/ios/launcher/ios_game_art.h"
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
}

@synthesize actionHandler = action_handler_;
@synthesize statusText = status_text_;
@synthesize statusColor = status_color_;

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
  table_view_.rowHeight = 58.0;
  table_view_.sectionHeaderTopPadding = 0.0;
  [self.view addSubview:table_view_];

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
    [button_row_.trailingAnchor
        constraintEqualToAnchor:header_.layoutMarginsGuide.trailingAnchor],
    [button_row_.bottomAnchor constraintEqualToAnchor:header_.bottomAnchor constant:-18.0],
    [play_button.heightAnchor constraintEqualToConstant:54.0],
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
  const BOOL short_window = size.height < 500.0;
  const CGFloat art_height = short_window ? kArtHeightShort : kArtHeight;
  cover_top_.constant = art_height - (short_window ? 110.0 : 150.0);
  const UIEdgeInsets safe = self.view.safeAreaInsets;
  header_.layoutMargins =
      UIEdgeInsetsMake(0.0, MAX(safe.left, kMargin), 0.0, MAX(safe.right, kMargin));
  table_view_.separatorInset = UIEdgeInsetsMake(0.0, MAX(safe.left, kMargin) + 40.0, 0.0,
                                                MAX(safe.right, kMargin));
  header_.frame = CGRectMake(0.0, 0.0, size.width, 0.0);
  [header_ layoutIfNeeded];
  const CGFloat height =
      [header_ systemLayoutSizeFittingSize:CGSizeMake(size.width, 0.0)
             withHorizontalFittingPriority:UILayoutPriorityRequired
                   verticalFittingPriority:UILayoutPriorityFittingSizeLevel]
          .height;
  if (header_.frame.size.height != height) {
    header_.frame = CGRectMake(0.0, 0.0, size.width, height);
    table_view_.tableHeaderView = header_;
  }
  art_view_.frame = CGRectMake(0.0, 0.0, size.width, art_height);
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  art_top_.frame = CGRectMake(0.0, 0.0, size.width, 120.0);
  art_fade_.frame = art_view_.bounds;
  UIColor* page = [[XeniaTheme bgPrimary] resolvedColorWithTraitCollection:self.traitCollection];
  art_fade_.colors = @[
    (id)[page colorWithAlphaComponent:0.0].CGColor, (id)[page colorWithAlphaComponent:0.0].CGColor,
    (id)[page colorWithAlphaComponent:0.7].CGColor, (id)page.CGColor
  ];
  art_fade_.locations = @[ @0.0, @0.42, @0.75, @1.0 ];
  [CATransaction commit];
  table_view_.contentInset = UIEdgeInsetsMake(0.0, 0.0, safe.bottom + 16.0, 0.0);
  top_bar_.frame = CGRectMake(0.0, 0.0, size.width, safe.top + 60.0);
  top_title_.frame = CGRectMake(MAX(safe.left, 16.0) + 60.0, safe.top + 8.0,
                                size.width - 2.0 * (MAX(safe.left, safe.right) + 76.0), 44.0);
  [self scrollViewDidScroll:table_view_];
}

- (void)scrollViewDidScroll:(UIScrollView*)scrollView {
  if (scrollView != table_view_) {
    return;
  }
  // The bar is fully in by the time the title would pass under it.
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

- (XeniaIOSGameActionRow)rowAtIndexPath:(NSIndexPath*)indexPath {
  XeniaIOSGameActionRow row = {};
  if (indexPath.section != 0 || indexPath.row < 0 ||
      indexPath.row >= static_cast<NSInteger>(rows_.count)) {
    return row;
  }
  [rows_[indexPath.row] getValue:&row];
  return row;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
  return 1;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
  return section == 0 ? static_cast<NSInteger>(rows_.count) : 0;
}

// Plain rows on the page, no cards: a symbol, the title and a chevron.
- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kCellIdentifier = @"XeniaIOSGameActionCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kCellIdentifier] autorelease];
    cell.backgroundColor = [UIColor clearColor];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
  }
  XeniaIOSGameActionRow row = [self rowAtIndexPath:indexPath];
  UIListContentConfiguration* content = [UIListContentConfiguration cellConfiguration];
  content.text = row.title;
  content.textProperties.color = [XeniaTheme textPrimary];
  content.textProperties.font =
      xe_scaled_system_font(UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  content.image = [UIImage systemImageNamed:row.symbol];
  content.imageProperties.tintColor = [XeniaTheme textSecondary];
  content.imageToTextPadding = 16.0;
  content.imageProperties.reservedLayoutSize = CGSizeMake(24.0, 0.0);
  content.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(
      0.0, MAX(self.view.safeAreaInsets.left, kMargin) - 4.0, 0.0, 0.0);
  cell.contentConfiguration = content;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  [self perform:[self rowAtIndexPath:indexPath].action];
}

@end
