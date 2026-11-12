/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_jit_sheet_view_controller.h"

#import "xenia/ui/ios/settings/ios_config_storage.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

// Where Need Help? goes: the community's JIT setup help.
NSString* const kJITHelpURL = @"https://discord.gg/QwcTtNKTGf";

// A step's text with the app and button names in bold, as the mockup sets them.
NSAttributedString* StepText(NSString* text, NSArray<NSString*>* bold_parts) {
  UIFont* body = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  UIFont* bold = [UIFont fontWithDescriptor:[body.fontDescriptor
                                                fontDescriptorWithSymbolicTraits:
                                                    UIFontDescriptorTraitBold]
                                       size:0.0];
  NSMutableAttributedString* result = [[[NSMutableAttributedString alloc]
      initWithString:text
          attributes:@{
            NSFontAttributeName : body,
            NSForegroundColorAttributeName : [XeniaTheme textPrimary],
          }] autorelease];
  for (NSString* part in bold_parts) {
    NSRange range = [text rangeOfString:part];
    if (range.location != NSNotFound) {
      [result addAttribute:NSFontAttributeName value:bold range:range];
    }
  }
  return result;
}

// One row of the numbered list: the number in accent, then the step.
UIView* NewStepRow(NSInteger number, NSAttributedString* text) {
  UILabel* number_label = [[[UILabel alloc] init] autorelease];
  number_label.text = [NSString localizedStringWithFormat:@"%ld", (long)number];
  number_label.textColor = [XeniaTheme accent];
  number_label.isAccessibilityElement = NO;
  xe_apply_label_font(number_label, UIFontTextStyleBody, 17.0, UIFontWeightBold);
  [number_label setContentHuggingPriority:UILayoutPriorityRequired
                                  forAxis:UILayoutConstraintAxisHorizontal];

  UILabel* text_label = [[[UILabel alloc] init] autorelease];
  text_label.attributedText = text;
  text_label.numberOfLines = 0;
  text_label.adjustsFontForContentSizeCategory = YES;
  text_label.accessibilityLabel =
      [NSString localizedStringWithFormat:@"%ld. %@", (long)number, text.string];

  UIStackView* row =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ number_label, text_label ]] autorelease];
  row.axis = UILayoutConstraintAxisHorizontal;
  row.alignment = UIStackViewAlignmentFirstBaseline;
  row.spacing = 12.0;
  row.layoutMarginsRelativeArrangement = YES;
  row.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(14.0, 18.0, 14.0, 18.0);
  [number_label.widthAnchor constraintGreaterThanOrEqualToConstant:14.0].active = YES;
  return row;
}

// An inset-grouped block, as Settings draws its lists: rounded surface, hairlines between rows.
UIView* NewGroup(NSArray<UIView*>* rows) {
  UIStackView* stack = [[[UIStackView alloc] init] autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.backgroundColor = [XeniaTheme bgSurface2];
  stack.layer.cornerRadius = 26.0;
  stack.layer.cornerCurve = kCACornerCurveContinuous;
  stack.clipsToBounds = YES;
  for (NSUInteger i = 0; i < rows.count; ++i) {
    if (i > 0) {
      UIView* hairline = [[[UIView alloc] init] autorelease];
      hairline.backgroundColor = [XeniaTheme border];
      [hairline.heightAnchor constraintEqualToConstant:1.0 / UIScreen.mainScreen.scale].active =
          YES;
      UIView* inset = [[[UIView alloc] init] autorelease];
      [inset addSubview:hairline];
      hairline.translatesAutoresizingMaskIntoConstraints = NO;
      [NSLayoutConstraint activateConstraints:@[
        [hairline.leadingAnchor constraintEqualToAnchor:inset.leadingAnchor constant:18.0],
        [hairline.trailingAnchor constraintEqualToAnchor:inset.trailingAnchor],
        [hairline.topAnchor constraintEqualToAnchor:inset.topAnchor],
        [hairline.bottomAnchor constraintEqualToAnchor:inset.bottomAnchor],
      ]];
      [stack addArrangedSubview:inset];
    }
    [stack addArrangedSubview:rows[i]];
  }
  return stack;
}

// Centres a view in a scroll view's visible height, and lets it scroll once it no longer fits.
NSArray<NSLayoutConstraint*>* CenteredInScroll(UIView* view, UIScrollView* scroll, CGFloat top,
                                               CGFloat bottom) {
  UILayoutGuide* content = scroll.contentLayoutGuide;
  UILayoutGuide* frame = scroll.frameLayoutGuide;
  NSLayoutConstraint* fill = [content.heightAnchor constraintEqualToAnchor:frame.heightAnchor];
  fill.priority = UILayoutPriorityDefaultLow;
  NSLayoutConstraint* centre = [view.centerYAnchor constraintEqualToAnchor:content.centerYAnchor
                                                                  constant:(top - bottom) / 2.0];
  return @[
    [content.widthAnchor constraintEqualToAnchor:frame.widthAnchor],
    [content.heightAnchor constraintGreaterThanOrEqualToAnchor:frame.heightAnchor],
    fill,
    [view.topAnchor constraintGreaterThanOrEqualToAnchor:content.topAnchor constant:top],
    [view.bottomAnchor constraintLessThanOrEqualToAnchor:content.bottomAnchor constant:-bottom],
    centre,
  ];
}

}  // namespace

@implementation XeniaIOSJITSheetViewController {
  NSString* game_title_;
  void (^open_stikdebug_)(void);
  UIView* other_ways_group_;
  UIButton* other_ways_button_;
  UIStackView* content_stack_;
  UIStackView* button_stack_;
  // Bolt, title and summary. Above the steps, or beside them when the sheet is short.
  UIStackView* header_stack_;
  UIScrollView* header_scroll_;
  UIScrollView* scroll_;
  NSArray<NSLayoutConstraint*>* stacked_constraints_;
  NSArray<NSLayoutConstraint*>* side_constraints_;
  BOOL side_layout_;
  BOOL layout_applied_;
}

- (instancetype)initWithGameTitle:(NSString*)gameTitle
                    openStikDebug:(void (^)(void))openStikDebug {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    game_title_ = [gameTitle copy];
    open_stikdebug_ = [openStikDebug copy];
    self.modalPresentationStyle = UIModalPresentationPageSheet;
    UISheetPresentationController* sheet = self.sheetPresentationController;
    // Opens tall enough for all three steps and both buttons, never cut off mid-list.
    __unsafe_unretained XeniaIOSJITSheetViewController* unsafe_self = self;
    sheet.detents = @[
      [UISheetPresentationControllerDetent
          customDetentWithIdentifier:@"xe.jit.fit"
                            resolver:^CGFloat(
                                id<UISheetPresentationControllerDetentResolutionContext> context) {
                              return MIN([unsafe_self fittingSheetHeight],
                                         context.maximumDetentValue);
                            }],
      [UISheetPresentationControllerDetent largeDetent],
    ];
    sheet.prefersGrabberVisible = YES;
  }
  return self;
}

- (CGFloat)fittingSheetHeight {
  [self loadViewIfNeeded];
  CGFloat width = CGRectGetWidth(self.view.readableContentGuide.layoutFrame) - 16.0;
  if (width <= 0.0) {
    width = MIN(CGRectGetWidth(UIScreen.mainScreen.bounds), 672.0) - 48.0;
  }
  const CGSize target = CGSizeMake(width, UILayoutFittingCompressedSize.height);
  const CGFloat content =
      [content_stack_ systemLayoutSizeFittingSize:target
                    withHorizontalFittingPriority:UILayoutPriorityRequired
                          verticalFittingPriority:UILayoutPriorityFittingSizeLevel]
          .height;
  const CGFloat buttons =
      [button_stack_ systemLayoutSizeFittingSize:target
                   withHorizontalFittingPriority:UILayoutPriorityRequired
                         verticalFittingPriority:UILayoutPriorityFittingSizeLevel]
          .height;
  // The scroll view's 40 + 20 padding, 8 above the buttons and 8 under them.
  return 40.0 + content + 20.0 + 8.0 + buttons + 8.0;
}

- (void)dealloc {
  [content_stack_ release];
  [button_stack_ release];
  [header_stack_ release];
  [header_scroll_ release];
  [scroll_ release];
  [stacked_constraints_ release];
  [side_constraints_ release];
  [game_title_ release];
  [open_stikdebug_ release];
  [other_ways_group_ release];
  [other_ways_button_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [XeniaTheme bgSurface];

  UIButton* close = xe_make_ios_sheet_close_button(self, @selector(closeTapped:));
  [self.view addSubview:close];

  // Need help: the same round glass button as close, across from it.
  UIButtonConfiguration* help_config = [UIButtonConfiguration glassButtonConfiguration];
  help_config.image =
      [UIImage systemImageNamed:@"questionmark"
              withConfiguration:[UIImageSymbolConfiguration
                                    configurationWithTextStyle:UIFontTextStyleBody
                                                         scale:UIImageSymbolScaleMedium]];
  help_config.baseForegroundColor = [XeniaTheme textPrimary];
  help_config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  UIButton* help = [UIButton buttonWithConfiguration:help_config primaryAction:nil];
  help.translatesAutoresizingMaskIntoConstraints = NO;
  help.accessibilityLabel = NSLocalizedString(@"Need Help?", @"JIT sheet: link to help on Discord");
  [help addTarget:self action:@selector(helpTapped:) forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:help];

  UIImageView* bolt = [[[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"bolt.fill"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithPointSize:48.0
                                                                      weight:UIImageSymbolWeightMedium]]]
      autorelease];
  bolt.tintColor = [XeniaTheme accent];
  bolt.contentMode = UIViewContentModeCenter;
  bolt.isAccessibilityElement = NO;

  UILabel* title = [[[UILabel alloc] init] autorelease];
  title.text = NSLocalizedString(@"Turn On JIT", @"JIT sheet: title");
  title.textColor = [XeniaTheme textPrimary];
  title.textAlignment = NSTextAlignmentCenter;
  title.numberOfLines = 0;
  title.accessibilityTraits = UIAccessibilityTraitHeader;
  xe_apply_label_font(title, UIFontTextStyleTitle1, 28.0, UIFontWeightBold);

  UILabel* summary = [[[UILabel alloc] init] autorelease];
  summary.text = NSLocalizedString(@"Games need JIT, and iOS only lets another app turn it on.",
                                   @"JIT sheet: one-line explanation");
  summary.textColor = [XeniaTheme textSecondary];
  summary.textAlignment = NSTextAlignmentCenter;
  summary.numberOfLines = 0;
  xe_apply_label_font(summary, UIFontTextStyleBody, 17.0, UIFontWeightRegular);

  NSString* open_text = NSLocalizedString(@"Open StikDebug.", @"JIT sheet: step 1");
  NSString* enable_text =
      NSLocalizedString(@"Choose XeniOS and tap Enable JIT.", @"JIT sheet: step 2");
  NSString* back_text =
      game_title_.length
          ? [NSString
                stringWithFormat:NSLocalizedString(@"Come back, and %@ starts.",
                                                   @"JIT sheet: step 3; %@ is the game"),
                                 // A short name never breaks across lines.
                                 game_title_.length <= 24
                                     ? [game_title_ stringByReplacingOccurrencesOfString:@" "
                                                                              withString:@"\u00A0"]
                                     : game_title_]
          : NSLocalizedString(@"Come back and pick a game.", @"JIT sheet: step 3 without a game");
  UIView* steps = NewGroup(@[
    NewStepRow(1, StepText(open_text, @[ @"StikDebug" ])),
    NewStepRow(2, StepText(enable_text, @[ @"Enable JIT" ])),
    NewStepRow(3, StepText(back_text, @[])),
  ]);

  // Script names and other JIT tools, for players who don't use StikDebug's default setup.
  UILabel* other_ways = [[[UILabel alloc] init] autorelease];
  other_ways.text =
      NSLocalizedString(@"Any JIT tool works, such as StikDebug with the universal.js script.",
                        @"JIT sheet: other ways to enable JIT");
  other_ways.textColor = [XeniaTheme textSecondary];
  other_ways.numberOfLines = 0;
  xe_apply_label_font(other_ways, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  UIStackView* other_ways_row = [[[UIStackView alloc] initWithArrangedSubviews:@[ other_ways ]]
      autorelease];
  other_ways_row.layoutMarginsRelativeArrangement = YES;
  other_ways_row.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(14.0, 18.0, 14.0, 18.0);
  other_ways_group_ = [NewGroup(@[ other_ways_row ]) retain];
  other_ways_group_.hidden = YES;

  UIStackView* header =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ bolt, title, summary ]] autorelease];
  header.axis = UILayoutConstraintAxisVertical;
  header.alignment = UIStackViewAlignmentFill;
  header.spacing = 8.0;
  [header setCustomSpacing:14.0 afterView:bolt];
  header.translatesAutoresizingMaskIntoConstraints = NO;
  header_stack_ = [header retain];

  // The same setting as Settings > Startup, where the player needs it: XeniOS does step 1 by
  // itself each time it starts without JIT.
  UILabel* automatic_title = [[[UILabel alloc] init] autorelease];
  automatic_title.text =
      NSLocalizedString(@"Open StikDebug Automatically", @"JIT sheet: switch title");
  automatic_title.textColor = [XeniaTheme textPrimary];
  automatic_title.numberOfLines = 0;
  xe_apply_label_font(automatic_title, UIFontTextStyleBody, 17.0, UIFontWeightRegular);
  UILabel* automatic_detail = [[[UILabel alloc] init] autorelease];
  automatic_detail.text = NSLocalizedString(@"Each time XeniOS starts without JIT.",
                                            @"JIT sheet: switch caption");
  automatic_detail.textColor = [XeniaTheme textSecondary];
  automatic_detail.numberOfLines = 0;
  xe_apply_label_font(automatic_detail, UIFontTextStyleSubheadline, 15.0, UIFontWeightRegular);
  UIStackView* automatic_text = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ automatic_title, automatic_detail ]] autorelease];
  automatic_text.axis = UILayoutConstraintAxisVertical;
  automatic_text.spacing = 2.0;
  automatic_text.isAccessibilityElement = NO;
  automatic_title.isAccessibilityElement = NO;
  automatic_detail.isAccessibilityElement = NO;
  UISwitch* automatic_switch = [[[UISwitch alloc] init] autorelease];
  automatic_switch.on = GetUserDefaultBool(kXeniaAutoOpenStikDebugOnLaunchPreferenceKey, false);
  automatic_switch.onTintColor = [XeniaTheme accent];
  automatic_switch.accessibilityLabel = automatic_title.text;
  automatic_switch.accessibilityHint = automatic_detail.text;
  [automatic_switch addTarget:self
                       action:@selector(automaticChanged:)
             forControlEvents:UIControlEventValueChanged];
  [automatic_switch setContentHuggingPriority:UILayoutPriorityRequired
                                      forAxis:UILayoutConstraintAxisHorizontal];
  [automatic_switch setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                    forAxis:UILayoutConstraintAxisHorizontal];
  UIStackView* automatic_row = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ automatic_text, automatic_switch ]] autorelease];
  automatic_row.axis = UILayoutConstraintAxisHorizontal;
  automatic_row.alignment = UIStackViewAlignmentCenter;
  automatic_row.spacing = 12.0;
  automatic_row.layoutMarginsRelativeArrangement = YES;
  automatic_row.directionalLayoutMargins = NSDirectionalEdgeInsetsMake(12.0, 18.0, 12.0, 18.0);
  UIView* automatic_group = NewGroup(@[ automatic_row ]);

  UIStackView* stack = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ header, steps, automatic_group, other_ways_group_ ]] autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.alignment = UIStackViewAlignmentFill;
  stack.spacing = 8.0;
  [stack setCustomSpacing:26.0 afterView:header];
  [stack setCustomSpacing:12.0 afterView:steps];
  [stack setCustomSpacing:16.0 afterView:automatic_group];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  content_stack_ = [stack retain];

  // The screen's one primary button, white on dark; the tertiary one is accent text.
  UIButtonConfiguration* primary_config = [UIButtonConfiguration filledButtonConfiguration];
  primary_config.title = NSLocalizedString(@"Open StikDebug", @"JIT sheet: primary button");
  primary_config.baseBackgroundColor = [XeniaTheme textPrimary];
  primary_config.baseForegroundColor = [XeniaTheme bgPrimary];
  primary_config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  primary_config.buttonSize = UIButtonConfigurationSizeLarge;
  primary_config.titleTextAttributesTransformer =
      ^NSDictionary<NSAttributedStringKey, id>*(NSDictionary<NSAttributedStringKey, id>* in) {
        NSMutableDictionary* out = [[in mutableCopy] autorelease];
        out[NSFontAttributeName] = [UIFont
            fontWithDescriptor:[[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]
                                   fontDescriptor]
                          size:0.0];
        return out;
      };
  UIButton* primary = [UIButton buttonWithConfiguration:primary_config primaryAction:nil];
  [primary.heightAnchor constraintGreaterThanOrEqualToConstant:50.0].active = YES;
  [primary addTarget:self
                action:@selector(openTapped:)
      forControlEvents:UIControlEventTouchUpInside];

  UIButtonConfiguration* tertiary_config = [UIButtonConfiguration plainButtonConfiguration];
  tertiary_config.title =
      NSLocalizedString(@"Other Ways to Enable JIT", @"JIT sheet: tertiary button");
  tertiary_config.baseForegroundColor = [XeniaTheme accent];
  other_ways_button_ =
      [[UIButton buttonWithConfiguration:tertiary_config primaryAction:nil] retain];
  [other_ways_button_ addTarget:self
                         action:@selector(otherWaysTapped:)
               forControlEvents:UIControlEventTouchUpInside];

  // The buttons stay at the bottom while the steps scroll, so Open StikDebug is always in reach.
  UIStackView* buttons = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ primary, other_ways_button_ ]] autorelease];
  buttons.axis = UILayoutConstraintAxisVertical;
  buttons.alignment = UIStackViewAlignmentFill;
  buttons.spacing = 4.0;
  buttons.translatesAutoresizingMaskIntoConstraints = NO;
  button_stack_ = [buttons retain];

  UIScrollView* scroll = [[[UIScrollView alloc] init] autorelease];
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  scroll.alwaysBounceVertical = YES;
  [scroll addSubview:stack];
  [self.view insertSubview:scroll atIndex:0];
  [self.view addSubview:buttons];
  scroll_ = [scroll retain];

  // In landscape on a phone the heading takes the left column on its own, centred, and scrolls
  // only if large text needs it.
  UIScrollView* header_scroll = [[[UIScrollView alloc] init] autorelease];
  header_scroll.translatesAutoresizingMaskIntoConstraints = NO;
  header_scroll.hidden = YES;
  [self.view insertSubview:header_scroll atIndex:0];
  header_scroll_ = [header_scroll retain];

  UILayoutGuide* readable = self.view.readableContentGuide;
  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [close.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12.0],
    [close.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16.0],
    [help.topAnchor constraintEqualToAnchor:close.topAnchor],
    [help.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16.0],
    [help.widthAnchor constraintEqualToConstant:44.0],
    [help.heightAnchor constraintEqualToConstant:44.0],
    [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
    [scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-8.0],
    [buttons.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-8.0],
  ]];

  stacked_constraints_ = [@[
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [buttons.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor constant:8.0],
    [buttons.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor constant:-8.0],
    [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:40.0],
    [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor
                                       constant:-20.0],
    [stack.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor constant:8.0],
    [stack.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor constant:-8.0],
  ] retain];

  NSMutableArray<NSLayoutConstraint*>* side = [NSMutableArray array];
  [side addObjectsFromArray:@[
    [header_scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
    [header_scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-8.0],
    [header_scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16.0],
    [header_scroll.widthAnchor constraintEqualToAnchor:safe.widthAnchor
                                            multiplier:0.42
                                              constant:-16.0],
    [header.leadingAnchor constraintEqualToAnchor:header_scroll.frameLayoutGuide.leadingAnchor
                                         constant:8.0],
    [header.trailingAnchor constraintEqualToAnchor:header_scroll.frameLayoutGuide.trailingAnchor
                                          constant:-8.0],
    [scroll.leadingAnchor constraintEqualToAnchor:header_scroll.trailingAnchor constant:16.0],
    [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16.0],
    [stack.leadingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.leadingAnchor],
    [stack.trailingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.trailingAnchor],
    [buttons.leadingAnchor constraintEqualToAnchor:scroll.leadingAnchor],
    [buttons.trailingAnchor constraintEqualToAnchor:scroll.trailingAnchor],
  ]];
  // Both columns centre in the same band, under the corner buttons and over Open StikDebug, so
  // the heading and the steps sit level.
  [side addObjectsFromArray:CenteredInScroll(header, header_scroll, 64.0, 12.0)];
  [side addObjectsFromArray:CenteredInScroll(stack, scroll, 64.0, 12.0)];
  side_constraints_ = [side copy];
}

// Two columns when the sheet is short (a phone in landscape): the heading would otherwise push
// the steps under the buttons.
- (void)viewWillLayoutSubviews {
  [super viewWillLayoutSubviews];
  const BOOL side = self.traitCollection.verticalSizeClass == UIUserInterfaceSizeClassCompact;
  if (layout_applied_ && side == side_layout_) {
    return;
  }
  layout_applied_ = YES;
  side_layout_ = side;
  [NSLayoutConstraint deactivateConstraints:stacked_constraints_];
  [NSLayoutConstraint deactivateConstraints:side_constraints_];
  if (side) {
    [header_scroll_ addSubview:header_stack_];
  } else {
    [content_stack_ insertArrangedSubview:header_stack_ atIndex:0];
    [content_stack_ setCustomSpacing:26.0 afterView:header_stack_];
  }
  header_scroll_.hidden = !side;
  [NSLayoutConstraint activateConstraints:side ? side_constraints_ : stacked_constraints_];
}

- (void)openTapped:(id)sender {
  (void)sender;
  void (^open_stikdebug)(void) = [[open_stikdebug_ copy] autorelease];
  [self dismissViewControllerAnimated:YES
                           completion:^{
                             if (open_stikdebug) {
                               open_stikdebug();
                             }
                           }];
}

- (void)automaticChanged:(UISwitch*)sender {
  SetUserDefaultBool(kXeniaAutoOpenStikDebugOnLaunchPreferenceKey, sender.on);
}

- (void)helpTapped:(id)sender {
  (void)sender;
  NSURL* url = [NSURL URLWithString:kJITHelpURL];
  if (url) {
    [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil];
  }
}

- (void)otherWaysTapped:(id)sender {
  (void)sender;
  // Shown in place, under the steps; the button has nothing more to offer once it has.
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.0 : 0.3
                   animations:^{
                     other_ways_group_.hidden = NO;
                     other_ways_button_.hidden = YES;
                     [self.view layoutIfNeeded];
                   }];
  [self.sheetPresentationController animateChanges:^{
    [self.sheetPresentationController invalidateDetents];
  }];
  UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, other_ways_group_);
}

- (void)closeTapped:(id)sender {
  (void)sender;
  [self dismissViewControllerAnimated:YES completion:nil];
}

@end
