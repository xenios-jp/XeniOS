/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_jit_sheet_view_controller.h"

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

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

}  // namespace

@implementation XeniaIOSJITSheetViewController {
  NSString* game_title_;
  void (^open_stikdebug_)(void);
  UIView* other_ways_group_;
  UIButton* other_ways_button_;
}

- (instancetype)initWithGameTitle:(NSString*)gameTitle
                    openStikDebug:(void (^)(void))openStikDebug {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    game_title_ = [gameTitle copy];
    open_stikdebug_ = [openStikDebug copy];
    self.modalPresentationStyle = UIModalPresentationPageSheet;
    UISheetPresentationController* sheet = self.sheetPresentationController;
    sheet.detents = @[
      [UISheetPresentationControllerDetent mediumDetent],
      [UISheetPresentationControllerDetent largeDetent],
    ];
    sheet.prefersGrabberVisible = YES;
  }
  return self;
}

- (void)dealloc {
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
  summary.text = NSLocalizedString(
      @"iOS only lets XeniOS run games fast when another app switches JIT on for it.",
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
          ? [NSString stringWithFormat:NSLocalizedString(@"You come back here and %@ starts.",
                                                         @"JIT sheet: step 3; %@ is the game"),
                                       game_title_]
          : NSLocalizedString(@"You come back here and pick a game.",
                              @"JIT sheet: step 3 without a game");
  UIView* steps = NewGroup(@[
    NewStepRow(1, StepText(open_text, @[ @"StikDebug" ])),
    NewStepRow(2, StepText(enable_text, @[ @"Enable JIT" ])),
    NewStepRow(3, StepText(back_text, @[])),
  ]);

  // Script names and other JIT tools, for players who don't use StikDebug's default setup.
  UILabel* other_ways = [[[UILabel alloc] init] autorelease];
  other_ways.text = NSLocalizedString(
      @"In StikDebug, assign the universal.js or Amethyst-MeloNX.js script to XeniOS. Any other "
      @"JIT tool works too, such as an lldb broker. Keep it attached until the game starts.",
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

  UIStackView* stack = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ bolt, title, summary, steps, other_ways_group_ ]] autorelease];
  stack.axis = UILayoutConstraintAxisVertical;
  stack.alignment = UIStackViewAlignmentFill;
  stack.spacing = 8.0;
  [stack setCustomSpacing:14.0 afterView:bolt];
  [stack setCustomSpacing:26.0 afterView:summary];
  [stack setCustomSpacing:16.0 afterView:steps];
  stack.translatesAutoresizingMaskIntoConstraints = NO;

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

  // The buttons stay at the bottom while the steps scroll, so Open StikDebug is always in reach
  // (in landscape the steps alone fill the sheet).
  UIStackView* buttons = [[[UIStackView alloc]
      initWithArrangedSubviews:@[ primary, other_ways_button_ ]] autorelease];
  buttons.axis = UILayoutConstraintAxisVertical;
  buttons.alignment = UIStackViewAlignmentFill;
  buttons.spacing = 4.0;
  buttons.translatesAutoresizingMaskIntoConstraints = NO;

  UIScrollView* scroll = [[[UIScrollView alloc] init] autorelease];
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  scroll.alwaysBounceVertical = YES;
  [scroll addSubview:stack];
  [self.view insertSubview:scroll belowSubview:close];
  [self.view addSubview:buttons];

  UILayoutGuide* readable = self.view.readableContentGuide;
  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [close.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12.0],
    [close.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16.0],
    [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
    [scroll.bottomAnchor constraintEqualToAnchor:buttons.topAnchor constant:-8.0],
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [buttons.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor constant:8.0],
    [buttons.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor constant:-8.0],
    [buttons.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-8.0],
    [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:40.0],
    [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor
                                       constant:-20.0],
    [stack.leadingAnchor constraintEqualToAnchor:readable.leadingAnchor constant:8.0],
    [stack.trailingAnchor constraintEqualToAnchor:readable.trailingAnchor constant:-8.0],
  ]];
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

- (void)otherWaysTapped:(id)sender {
  (void)sender;
  // Shown in place, under the steps; the button has nothing more to offer once it has.
  [UIView animateWithDuration:UIAccessibilityIsReduceMotionEnabled() ? 0.0 : 0.3
                   animations:^{
                     other_ways_group_.hidden = NO;
                     other_ways_button_.hidden = YES;
                     [self.view layoutIfNeeded];
                   }];
  UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, other_ways_group_);
}

- (void)closeTapped:(id)sender {
  (void)sender;
  [self dismissViewControllerAnimated:YES completion:nil];
}

@end
