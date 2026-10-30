/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_edit_panel_ios.h"

#include <cmath>
#include <vector>

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#include "xenia/ui/ios/touch/touch_controls_overlay_helpers_ios.h"
#include "xenia/ui/ios/touch/touch_overlay_style_ios.h"

using xe::hid::touch::IOSTouchAction;
using xe::hid::touch::IOSTouchAnalogOutput;
using xe::hid::touch::IOSTouchControlShape;
using xe::hid::touch::IOSTouchControlType;
using xe::hid::touch::IOSTouchInteractionTrigger;
using xe::hid::touch::IOSTouchTintStyle;

namespace {

constexpr CGFloat kRowInset = 10.0f;
constexpr CGFloat kHeaderHeight = 116.0f;
constexpr CGFloat kHeaderTextTop = 8.0f;
constexpr CGFloat kQuickRowTop = 66.0f;
constexpr CGFloat kQuickButtonHeight = 44.0f;
constexpr CGFloat kQuickButtonGap = 6.0f;
constexpr CGFloat kSectionHeaderHeight = 32.0f;
constexpr CGFloat kSliderRowHeight = 72.0f;
constexpr CGFloat kSwatchRowHeight = 78.0f;
constexpr CGFloat kChipRowHeight = 64.0f;
constexpr CGFloat kContentTopInset = 6.0f;
constexpr CGFloat kContentBottomInset = 14.0f;
constexpr CGFloat kMaxExpandedContentHeight = 300.0f;

UIColor* PanelFill() { return [UIColor colorWithRed:0.07f green:0.07f blue:0.08f alpha:0.97f]; }
UIColor* PrimaryText() { return [[UIColor whiteColor] colorWithAlphaComponent:0.95f]; }
UIColor* SecondaryText() { return [[UIColor whiteColor] colorWithAlphaComponent:0.55f]; }
UIColor* ChipFill() { return [[UIColor whiteColor] colorWithAlphaComponent:0.12f]; }
UIColor* DestructiveText() { return [UIColor colorWithRed:1.0f green:0.22f blue:0.20f alpha:1.0f]; }

UIButton* CreateQuickIconButton(NSString* symbol, NSString* accessibility_label, UIColor* tint,
                                id target, SEL action) {
  UIButton* button = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
  button.backgroundColor = ChipFill();
  button.layer.cornerRadius = kQuickButtonHeight * 0.5f;
  button.tintColor = tint ? tint : PrimaryText();
  UIImageSymbolConfiguration* config =
      [UIImageSymbolConfiguration configurationWithPointSize:19.0f
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* image = [UIImage systemImageNamed:symbol withConfiguration:config];
  if (image) {
    [button setImage:image forState:UIControlStateNormal];
  }
  button.accessibilityLabel = accessibility_label;
  button.accessibilityTraits = UIAccessibilityTraitButton;
  if (target && action) {
    [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
  }
  return button;
}

}  // namespace

#pragma mark - Row primitives

@interface XeTEPRowBase : UIView
- (CGFloat)rowHeight;
@end

@implementation XeTEPRowBase
- (CGFloat)rowHeight {
  return 44.0f;
}
@end

@interface XeTEPWideSlider : UISlider
@end

@implementation XeTEPWideSlider
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent*)event {
  return CGRectContainsPoint(CGRectInset(self.bounds, -18.0f, -14.0f), point);
}
@end

@interface XeTEPSectionHeader : XeTEPRowBase
- (void)setTitle:(NSString*)title;
@end

@implementation XeTEPSectionHeader {
  UILabel* label_;
}
- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    label_ = [[UILabel alloc] initWithFrame:CGRectZero];
    label_.font = [UIFont systemFontOfSize:12.0f weight:UIFontWeightSemibold];
    label_.textColor = SecondaryText();
    [self addSubview:label_];
  }
  return self;
}
- (void)dealloc {
  [label_ release];
  [super dealloc];
}
- (CGFloat)rowHeight {
  return kSectionHeaderHeight;
}
- (void)setTitle:(NSString*)title {
  label_.text = [title uppercaseString];
}
- (void)layoutSubviews {
  [super layoutSubviews];
  label_.frame = CGRectMake(kRowInset, kSectionHeaderHeight - 20.0f,
                            CGRectGetWidth(self.bounds) - kRowInset * 2.0f, 16.0f);
}
@end

@interface XeTEPScrollView : UIScrollView
@end

@implementation XeTEPScrollView
- (BOOL)touchesShouldCancelInContentView:(UIView*)view {
  if ([view isKindOfClass:[UISlider class]]) {
    return NO;
  }
  return YES;
}
@end

@interface XeTEPSliderRow : XeTEPRowBase
@property(nonatomic, copy) void (^onBegin)(void);
@property(nonatomic, copy) void (^onChange)(float value);
@property(nonatomic, copy) void (^onEnd)(void);
- (void)configureWithTitle:(NSString*)title
                     value:(float)value
                       min:(float)min
                       max:(float)max
                   percent:(BOOL)percent;
@end

@implementation XeTEPSliderRow {
  UILabel* title_label_;
  UILabel* value_label_;
  UISlider* slider_;
  BOOL percent_;
}
- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    title_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
    title_label_.font = [UIFont systemFontOfSize:15.0f weight:UIFontWeightSemibold];
    title_label_.textColor = PrimaryText();
    [self addSubview:title_label_];
    value_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
    value_label_.font = [UIFont systemFontOfSize:15.0f weight:UIFontWeightRegular];
    value_label_.textColor = SecondaryText();
    value_label_.textAlignment = NSTextAlignmentRight;
    [self addSubview:value_label_];
    slider_ = [[XeTEPWideSlider alloc] initWithFrame:CGRectZero];
    slider_.continuous = YES;
    slider_.exclusiveTouch = YES;
    slider_.minimumTrackTintColor = [XeniaTheme touchTintAmber];
    [slider_ addTarget:self
                  action:@selector(sliderChanged:)
        forControlEvents:UIControlEventValueChanged];
    [slider_ addTarget:self
                  action:@selector(sliderBegan:)
        forControlEvents:UIControlEventTouchDown];
    [slider_ addTarget:self
                  action:@selector(sliderEnded:)
        forControlEvents:(UIControlEvents)(UIControlEventTouchUpInside |
                                           UIControlEventTouchUpOutside |
                                           UIControlEventTouchCancel)];
    [self addSubview:slider_];
  }
  return self;
}
- (void)dealloc {
  [title_label_ release];
  [value_label_ release];
  [slider_ release];
  self.onBegin = nil;
  self.onChange = nil;
  self.onEnd = nil;
  [super dealloc];
}
- (CGFloat)rowHeight {
  return kSliderRowHeight;
}
- (void)configureWithTitle:(NSString*)title
                     value:(float)value
                       min:(float)min
                       max:(float)max
                   percent:(BOOL)percent {
  percent_ = percent;
  title_label_.text = title;
  slider_.minimumValue = min;
  slider_.maximumValue = max;
  slider_.value = value;
  [self updateValueLabel];
}
- (void)updateValueLabel {
  if (percent_) {
    value_label_.text = [NSString stringWithFormat:@"%.0f%%", slider_.value * 100.0f];
  } else {
    value_label_.text = [NSString stringWithFormat:@"%.2f", slider_.value];
  }
}
- (void)sliderChanged:(UISlider*)__unused sender {
  [self updateValueLabel];
  if (self.onChange) {
    self.onChange(slider_.value);
  }
}
- (void)sliderBegan:(UISlider*)__unused sender {
  if (self.onBegin) {
    self.onBegin();
  }
}
- (void)sliderEnded:(UISlider*)__unused sender {
  if (self.onEnd) {
    self.onEnd();
  }
}
- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat w = CGRectGetWidth(self.bounds) - kRowInset * 2.0f;
  title_label_.frame = CGRectMake(kRowInset, 10.0f, w * 0.6f, 22.0f);
  value_label_.frame = CGRectMake(kRowInset + w * 0.6f, 10.0f, w * 0.4f, 22.0f);
  slider_.frame = CGRectMake(kRowInset, 34.0f, w, 40.0f);
}
- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  if (self.hidden || self.alpha <= 0.01f || !self.userInteractionEnabled) {
    return nil;
  }
  CGPoint slider_point = [slider_ convertPoint:point fromView:self];
  if ([slider_ pointInside:slider_point withEvent:event]) {
    return [slider_ hitTest:slider_point withEvent:event];
  }
  return [super hitTest:point withEvent:event];
}
@end

@interface XeTEPSwatchRow : XeTEPRowBase
@property(nonatomic, copy) void (^onSelect)(NSInteger index);
- (void)configureWithTitle:(NSString*)title
                    colors:(NSArray<UIColor*>*)colors
             selectedIndex:(NSInteger)selectedIndex;
@end

@implementation XeTEPSwatchRow {
  UILabel* title_label_;
  NSMutableArray<UIButton*>* swatches_;
}
- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    title_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
    title_label_.font = [UIFont systemFontOfSize:15.0f weight:UIFontWeightSemibold];
    title_label_.textColor = PrimaryText();
    [self addSubview:title_label_];
    swatches_ = [[NSMutableArray alloc] init];
  }
  return self;
}
- (void)dealloc {
  [title_label_ release];
  [swatches_ release];
  self.onSelect = nil;
  [super dealloc];
}
- (CGFloat)rowHeight {
  return kSwatchRowHeight;
}
- (void)configureWithTitle:(NSString*)title
                    colors:(NSArray<UIColor*>*)colors
             selectedIndex:(NSInteger)selectedIndex {
  title_label_.text = title;
  for (UIButton* button in swatches_) {
    [button removeFromSuperview];
  }
  [swatches_ removeAllObjects];
  NSInteger index = 0;
  for (UIColor* color in colors) {
    UIButton* button = [UIButton buttonWithType:UIButtonTypeCustom];
    button.backgroundColor = color;
    button.layer.cornerRadius = 8.0f;
    button.tag = index;
    button.layer.borderWidth = (index == selectedIndex) ? 2.5f : 0.0f;
    button.layer.borderColor = [[UIColor whiteColor] colorWithAlphaComponent:0.95f].CGColor;
    [button addTarget:self
                  action:@selector(swatchTapped:)
        forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:button];
    [swatches_ addObject:button];
    ++index;
  }
  [self setNeedsLayout];
}
- (void)swatchTapped:(UIButton*)sender {
  if (self.onSelect) {
    self.onSelect(sender.tag);
  }
}
- (void)layoutSubviews {
  [super layoutSubviews];
  title_label_.frame =
      CGRectMake(kRowInset, 8.0f, CGRectGetWidth(self.bounds) - kRowInset * 2.0f, 18.0f);
  CGFloat size = 44.0f;
  CGFloat gap = 5.0f;
  CGFloat x = kRowInset;
  for (UIButton* button in swatches_) {
    button.frame = CGRectMake(x, 32.0f, size, size);
    x += size + gap;
  }
}
@end

@interface XeTEPChipRow : XeTEPRowBase
@property(nonatomic, readonly) UIButton* chipButton;
- (void)configureWithTitle:(NSString*)title subtitle:(NSString*)subtitle value:(NSString*)value;
@end

@implementation XeTEPChipRow {
  UILabel* title_label_;
  UILabel* subtitle_label_;
  UIButton* chip_button_;
  UIImageView* chevron_;
}
@synthesize chipButton = chip_button_;
- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    title_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
    title_label_.font = [UIFont systemFontOfSize:15.0f weight:UIFontWeightSemibold];
    title_label_.textColor = PrimaryText();
    [self addSubview:title_label_];
    subtitle_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
    subtitle_label_.font = [UIFont systemFontOfSize:12.0f weight:UIFontWeightRegular];
    subtitle_label_.textColor = SecondaryText();
    [self addSubview:subtitle_label_];
    chip_button_ = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
    chip_button_.backgroundColor = ChipFill();
    chip_button_.layer.cornerRadius = 9.0f;
    xe_apply_button_title_font(chip_button_, UIFontTextStyleSubheadline, 14.0,
                               UIFontWeightSemibold);
    [chip_button_ setTitleColor:PrimaryText() forState:UIControlStateNormal];
    [self addSubview:chip_button_];
    chevron_ = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"chevron.right"]];
    chevron_.tintColor = SecondaryText();
    chevron_.contentMode = UIViewContentModeCenter;
    [self addSubview:chevron_];
  }
  return self;
}
- (void)dealloc {
  [title_label_ release];
  [subtitle_label_ release];
  [chip_button_ release];
  [chevron_ release];
  [super dealloc];
}
- (CGFloat)rowHeight {
  return kChipRowHeight;
}
- (void)configureWithTitle:(NSString*)title subtitle:(NSString*)subtitle value:(NSString*)value {
  title_label_.text = title;
  subtitle_label_.text = subtitle;
  subtitle_label_.hidden = (subtitle.length == 0);
  [chip_button_ setTitle:value forState:UIControlStateNormal];
}
- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat w = CGRectGetWidth(self.bounds);
  CGFloat chevron_w = 14.0f;
  CGFloat chip_h = 44.0f;
  CGFloat chip_w = 148.0f;
  chevron_.frame = CGRectMake(w - kRowInset - chevron_w, (kChipRowHeight - chevron_w) * 0.5f,
                              chevron_w, chevron_w);
  CGFloat chip_x = CGRectGetMinX(chevron_.frame) - 8.0f - chip_w;
  chip_button_.frame = CGRectMake(chip_x, (kChipRowHeight - chip_h) * 0.5f, chip_w, chip_h);
  CGFloat text_w = MAX(0.0f, chip_x - kRowInset - 8.0f);
  if (subtitle_label_.hidden) {
    title_label_.frame = CGRectMake(kRowInset, (kChipRowHeight - 20.0f) * 0.5f, text_w, 20.0f);
  } else {
    title_label_.frame = CGRectMake(kRowInset, 10.0f, text_w, 20.0f);
    subtitle_label_.frame = CGRectMake(kRowInset, 32.0f, text_w, 16.0f);
  }
}
@end

#pragma mark - Panel

@implementation XeniaTouchEditPanel {
  id<XeniaTouchEditPanelDelegate> delegate_;  // assign (unowned)
  UIView* header_;
  UILabel* header_title_;
  UIButton* primary_binding_button_;
  UILabel* header_subtitle_;
  UIButton* assignment_badge_;
  UIButton* collapse_button_;
  UIView* size_stepper_;
  UIView* size_separator_;
  UIButton* smaller_button_;
  UIButton* larger_button_;
  UIButton* layout_button_;
  UIButton* behavior_button_;
  UIButton* tune_button_;
  UIButton* duplicate_button_;
  UIButton* delete_button_;
  UIScrollView* scroll_;
  UIView* content_;
  NSMutableArray<XeTEPRowBase*>* rows_;
  BOOL expanded_;
  BOOL has_control_;
  xe::hid::touch::IOSTouchControlDefinition control_;
}

@synthesize delegate = delegate_;
@synthesize expanded = expanded_;

+ (CGFloat)collapsedHeight {
  return kHeaderHeight;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }
  self.backgroundColor = PanelFill();
  self.layer.cornerRadius = 16.0f;
  self.layer.borderWidth = 1.0f;
  self.layer.borderColor = [[UIColor whiteColor] colorWithAlphaComponent:0.08f].CGColor;
  self.clipsToBounds = YES;

  header_ = [[UIView alloc] initWithFrame:CGRectZero];
  [self addSubview:header_];
  header_title_ = [[UILabel alloc] initWithFrame:CGRectZero];
  header_title_.font = [UIFont systemFontOfSize:15.0f weight:UIFontWeightBold];
  header_title_.textColor = PrimaryText();
  [header_ addSubview:header_title_];
  primary_binding_button_ = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
  primary_binding_button_.backgroundColor =
      [[XeniaTheme touchTintSky] colorWithAlphaComponent:0.18f];
  primary_binding_button_.layer.cornerRadius = 15.0f;
  primary_binding_button_.layer.borderWidth = 1.0f;
  primary_binding_button_.layer.borderColor =
      [[XeniaTheme touchTintSky] colorWithAlphaComponent:0.55f].CGColor;
  primary_binding_button_.clipsToBounds = YES;
  primary_binding_button_.tintColor = [[XeniaTheme touchTintSky] colorWithAlphaComponent:1.0f];
  primary_binding_button_.titleLabel.adjustsFontSizeToFitWidth = YES;
  primary_binding_button_.titleLabel.minimumScaleFactor = 0.68f;
  xe_apply_button_title_font(primary_binding_button_, UIFontTextStyleHeadline, 17.0,
                             UIFontWeightBold);
  [primary_binding_button_ setTitleColor:[[XeniaTheme touchTintSky] colorWithAlphaComponent:1.0f]
                                forState:UIControlStateNormal];
  primary_binding_button_.accessibilityTraits = UIAccessibilityTraitButton;
  primary_binding_button_.hidden = YES;
  [header_ addSubview:primary_binding_button_];
  header_subtitle_ = [[UILabel alloc] initWithFrame:CGRectZero];
  header_subtitle_.font = [UIFont systemFontOfSize:10.5f weight:UIFontWeightRegular];
  header_subtitle_.textColor = SecondaryText();
  header_subtitle_.adjustsFontSizeToFitWidth = YES;
  header_subtitle_.minimumScaleFactor = 0.72f;
  [header_ addSubview:header_subtitle_];
  assignment_badge_ = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
  assignment_badge_.backgroundColor = [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.16f];
  [assignment_badge_ setTitleColor:[[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.98f]
                          forState:UIControlStateNormal];
  assignment_badge_.tintColor = [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.98f];
  assignment_badge_.titleLabel.adjustsFontSizeToFitWidth = YES;
  assignment_badge_.titleLabel.minimumScaleFactor = 0.62f;
  assignment_badge_.hidden = YES;
  assignment_badge_.layer.cornerRadius = 15.0f;
  assignment_badge_.layer.borderWidth = 0.9f;
  assignment_badge_.layer.borderColor =
      [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.45f].CGColor;
  assignment_badge_.clipsToBounds = YES;
  xe_apply_button_title_font(assignment_badge_, UIFontTextStyleCaption2, 10.5,
                             UIFontWeightSemibold);
  assignment_badge_.accessibilityTraits = UIAccessibilityTraitButton;
  assignment_badge_.showsMenuAsPrimaryAction = YES;
  [header_ addSubview:assignment_badge_];
  collapse_button_ = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
  collapse_button_.backgroundColor = ChipFill();
  collapse_button_.layer.cornerRadius = 22.0f;
  collapse_button_.tintColor = PrimaryText();
  [collapse_button_ setImage:[UIImage systemImageNamed:@"chevron.right"]
                    forState:UIControlStateNormal];
  collapse_button_.accessibilityLabel = @"Expand";
  [collapse_button_ addTarget:self
                       action:@selector(collapseTapped:)
             forControlEvents:UIControlEventTouchUpInside];
  [header_ addSubview:collapse_button_];

  size_stepper_ = [[UIView alloc] initWithFrame:CGRectZero];
  size_stepper_.backgroundColor = ChipFill();
  size_stepper_.layer.cornerRadius = kQuickButtonHeight * 0.5f;
  size_stepper_.clipsToBounds = YES;
  [header_ addSubview:size_stepper_];
  smaller_button_ = CreateQuickIconButton(@"minus", @"Decrease size", PrimaryText(), self,
                                          @selector(smallerTapped:));
  smaller_button_.backgroundColor = [UIColor clearColor];
  [size_stepper_ addSubview:smaller_button_];
  larger_button_ = CreateQuickIconButton(@"plus", @"Increase size", PrimaryText(), self,
                                         @selector(largerTapped:));
  larger_button_.backgroundColor = [UIColor clearColor];
  [size_stepper_ addSubview:larger_button_];
  size_separator_ = [[UIView alloc] initWithFrame:CGRectZero];
  size_separator_.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:0.10f];
  [size_stepper_ addSubview:size_separator_];

  layout_button_ = CreateQuickIconButton(@"arrow.up.left.and.arrow.down.right", @"Match size",
                                         PrimaryText(), self, @selector(layoutTapped:));
  layout_button_.menu = [self layoutQuickMenu];
  layout_button_.showsMenuAsPrimaryAction = NO;
  [header_ addSubview:layout_button_];
  behavior_button_ =
      CreateQuickIconButton(@"ellipsis", @"Extra function", PrimaryText(), nil, nil);
  behavior_button_.showsMenuAsPrimaryAction = YES;
  [header_ addSubview:behavior_button_];
  tune_button_ = CreateQuickIconButton(@"speedometer", @"Tune response", PrimaryText(),
                                       self, @selector(tuneTapped:));
  [header_ addSubview:tune_button_];
  duplicate_button_ = CreateQuickIconButton(@"square.on.square", @"Duplicate", PrimaryText(), self,
                                            @selector(duplicateTapped:));
  [header_ addSubview:duplicate_button_];
  delete_button_ =
      CreateQuickIconButton(@"trash", @"Delete", DestructiveText(), self, @selector(deleteTapped:));
  [header_ addSubview:delete_button_];

  scroll_ = [[XeTEPScrollView alloc] initWithFrame:CGRectZero];
  scroll_.showsVerticalScrollIndicator = YES;
  scroll_.alwaysBounceVertical = YES;
  scroll_.directionalLockEnabled = YES;
  scroll_.delaysContentTouches = NO;
  scroll_.canCancelContentTouches = YES;
  scroll_.panGestureRecognizer.cancelsTouchesInView = YES;
  scroll_.hidden = YES;
  [self addSubview:scroll_];
  content_ = [[UIView alloc] initWithFrame:CGRectZero];
  [scroll_ addSubview:content_];

  rows_ = [[NSMutableArray alloc] init];
  expanded_ = NO;
  has_control_ = NO;
  return self;
}

- (void)dealloc {
  [header_title_ release];
  [primary_binding_button_ release];
  [header_subtitle_ release];
  [assignment_badge_ release];
  [collapse_button_ release];
  [size_separator_ release];
  [smaller_button_ release];
  [larger_button_ release];
  [size_stepper_ release];
  [layout_button_ release];
  [behavior_button_ release];
  [tune_button_ release];
  [duplicate_button_ release];
  [delete_button_ release];
  [header_ release];
  [content_ release];
  [scroll_ release];
  [rows_ release];
  [super dealloc];
}

#pragma mark - Public

- (void)applyControl:(const xe::hid::touch::IOSTouchControlDefinition&)control {
  control_ = control;
  has_control_ = YES;
  layout_button_.menu = [self layoutQuickMenu];
  primary_binding_button_.menu = [self primaryBindingMenu];
  primary_binding_button_.showsMenuAsPrimaryAction = primary_binding_button_.menu != nil;
  primary_binding_button_.hidden = primary_binding_button_.menu == nil;
  behavior_button_.hidden = control_.type != IOSTouchControlType::kActionButton;
  behavior_button_.enabled = !behavior_button_.hidden;
  behavior_button_.menu = [self behaviorQuickMenu];
  tune_button_.hidden = ![self selectedControlHasTunableAnalogOutput];
  tune_button_.enabled = !tune_button_.hidden;
  [self rebuildContent];
  [self updateHeader];
  [self setNeedsLayout];
}

- (void)clearControl {
  has_control_ = NO;
  primary_binding_button_.hidden = YES;
  primary_binding_button_.menu = nil;
  layout_button_.menu = nil;
  behavior_button_.hidden = YES;
  behavior_button_.enabled = NO;
  assignment_badge_.hidden = YES;
  assignment_badge_.menu = nil;
  [assignment_badge_ setTitle:nil forState:UIControlStateNormal];
  [self clearRows];
  [self setNeedsLayout];
}

- (void)setDuplicateAvailable:(BOOL)available {
  duplicate_button_.hidden = !available;
  duplicate_button_.enabled = available;
  duplicate_button_.alpha = available ? 1.0f : 0.0f;
  [self setNeedsLayout];
}

- (void)setExpanded:(BOOL)expanded animated:(BOOL)__unused animated {
  expanded_ = expanded;
  [collapse_button_
      setImage:[UIImage systemImageNamed:(expanded ? @"chevron.down" : @"chevron.right")]
      forState:UIControlStateNormal];
  collapse_button_.accessibilityLabel = expanded ? @"Collapse" : @"Expand";
  scroll_.hidden = !expanded;
  [self setNeedsLayout];
}

- (CGFloat)preferredHeightForWidth:(CGFloat)__unused width {
  if (!expanded_ || !has_control_) {
    return kHeaderHeight;
  }
  CGFloat content = kContentTopInset + kContentBottomInset;
  for (XeTEPRowBase* row in rows_) {
    content += [row rowHeight];
  }
  return kHeaderHeight + MIN(content, kMaxExpandedContentHeight);
}

#pragma mark - Layout

- (void)layoutSubviews {
  [super layoutSubviews];
  CGFloat width = CGRectGetWidth(self.bounds);
  header_.frame = CGRectMake(0.0f, 0.0f, width, kHeaderHeight);
  CGFloat collapse_w = 44.0f;
  collapse_button_.frame =
      CGRectMake(width - kRowInset - collapse_w, kHeaderTextTop, collapse_w, 44.0f);
  CGFloat top_right_x = CGRectGetMinX(collapse_button_.frame) - 8.0f;
  NSString* assignment_title = [assignment_badge_ titleForState:UIControlStateNormal];
  const BOOL show_assignment = !assignment_badge_.hidden && assignment_title.length > 0;
  if (show_assignment) {
    CGFloat assignment_max_w = MAX(88.0f, MIN(176.0f, top_right_x - kRowInset - 86.0f));
    CGSize fit = [assignment_badge_ sizeThatFits:CGSizeMake(assignment_max_w, 44.0f)];
    CGFloat assignment_w = MIN(MAX(std::ceil(fit.width) + 18.0f, 84.0f), assignment_max_w);
    assignment_badge_.frame =
        CGRectIntegral(CGRectMake(top_right_x - assignment_w, kHeaderTextTop, assignment_w, 44.0f));
    top_right_x = CGRectGetMinX(assignment_badge_.frame) - 8.0f;
  } else {
    assignment_badge_.frame = CGRectZero;
  }
  CGFloat title_w = MAX(0.0f, top_right_x - kRowInset);
  if (!primary_binding_button_.hidden) {
    CGSize fit = [primary_binding_button_ sizeThatFits:CGSizeMake(title_w, 44.0f)];
    CGFloat binding_w = MIN(MAX(std::ceil(fit.width) + 22.0f, 64.0f), MAX(64.0f, title_w));
    primary_binding_button_.frame =
        CGRectIntegral(CGRectMake(kRowInset, kHeaderTextTop, binding_w, 44.0f));
    header_title_.hidden = YES;
    header_title_.frame = CGRectZero;
  } else {
    primary_binding_button_.frame = CGRectZero;
    header_title_.hidden = NO;
    header_title_.frame = CGRectMake(kRowInset, kHeaderTextTop, MIN(title_w, 120.0f), 44.0f);
  }
  CGFloat subtitle_x = !primary_binding_button_.hidden
                           ? CGRectGetMaxX(primary_binding_button_.frame) + 8.0f
                           : kRowInset;
  if (!header_title_.hidden) {
    subtitle_x = CGRectGetMaxX(header_title_.frame) + 8.0f;
  }
  CGFloat subtitle_w = MAX(0.0f, top_right_x - subtitle_x);
  header_subtitle_.frame =
      CGRectIntegral(CGRectMake(subtitle_x, kHeaderTextTop + 13.0f, subtitle_w, 18.0f));

  CGFloat available = MAX(0.0f, width - kRowInset * 2.0f);
  CGFloat button_side = kQuickButtonHeight;
  CGFloat stepper_width = button_side * 2.0f + 4.0f;
  const BOOL show_duplicate = !duplicate_button_.hidden;
  const BOOL show_tune = !tune_button_.hidden;
  const BOOL show_behavior = !behavior_button_.hidden;

  NSMutableArray<UIButton*>* quick_buttons = [NSMutableArray arrayWithObject:layout_button_];
  if (show_behavior) {
    [quick_buttons addObject:behavior_button_];
  }
  if (show_tune) {
    [quick_buttons addObject:tune_button_];
  }
  if (show_duplicate) {
    [quick_buttons addObject:duplicate_button_];
  }
  [quick_buttons addObject:delete_button_];

  const CGFloat fixed_quick_width =
      stepper_width + button_side * static_cast<CGFloat>(quick_buttons.count);
  const CGFloat gap_count = static_cast<CGFloat>(quick_buttons.count);
  const CGFloat raw_gap =
      gap_count > 0.0f ? (available - fixed_quick_width) / gap_count : kQuickButtonGap;
  CGFloat gap = MAX(3.0f, raw_gap);
  if (fixed_quick_width + gap * gap_count > available) {
    stepper_width = button_side * 2.0f + 2.0f;
    gap = MAX(3.0f, (available - stepper_width -
                     button_side * static_cast<CGFloat>(quick_buttons.count)) /
                        gap_count);
  }
  CGFloat quick_x = kRowInset;
  size_stepper_.frame =
      CGRectIntegral(CGRectMake(quick_x, kQuickRowTop, stepper_width, kQuickButtonHeight));
  smaller_button_.frame = CGRectMake(0.0f, 0.0f, button_side, kQuickButtonHeight);
  size_separator_.frame = CGRectMake(button_side, 12.0f, 1.0f, kQuickButtonHeight - 24.0f);
  larger_button_.frame =
      CGRectMake(stepper_width - button_side, 0.0f, button_side, kQuickButtonHeight);

  quick_x = CGRectGetMaxX(size_stepper_.frame) + gap;
  for (UIButton* button in quick_buttons) {
    button.frame =
        CGRectIntegral(CGRectMake(quick_x, kQuickRowTop, button_side, kQuickButtonHeight));
    quick_x += button_side + gap;
  }
  if (!show_behavior) {
    behavior_button_.frame = CGRectZero;
  }
  if (!show_tune) {
    tune_button_.frame = CGRectZero;
  }
  if (!show_duplicate) {
    duplicate_button_.frame = CGRectZero;
  }

  if (expanded_) {
    CGFloat scroll_h = MAX(0.0f, CGRectGetHeight(self.bounds) - kHeaderHeight);
    scroll_.frame = CGRectMake(0.0f, kHeaderHeight, width, scroll_h);
    CGFloat y = kContentTopInset;
    for (XeTEPRowBase* row in rows_) {
      CGFloat h = [row rowHeight];
      row.frame = CGRectMake(0.0f, y, width, h);
      y += h;
    }
    y += kContentBottomInset;
    content_.frame = CGRectMake(0.0f, 0.0f, width, y);
    scroll_.contentSize = CGSizeMake(width, y);
  } else {
    scroll_.frame = CGRectZero;
  }
}

#pragma mark - Content

- (void)clearRows {
  for (XeTEPRowBase* row in rows_) {
    [row removeFromSuperview];
  }
  [rows_ removeAllObjects];
}

- (XeTEPSectionHeader*)addSection:(NSString*)title {
  XeTEPSectionHeader* header = [[[XeTEPSectionHeader alloc] initWithFrame:CGRectZero] autorelease];
  [header setTitle:title];
  [content_ addSubview:header];
  [rows_ addObject:header];
  return header;
}

- (XeTEPSliderRow*)addSliderRow {
  XeTEPSliderRow* row = [[[XeTEPSliderRow alloc] initWithFrame:CGRectZero] autorelease];
  [content_ addSubview:row];
  [rows_ addObject:row];
  return row;
}

- (XeTEPSwatchRow*)addSwatchRow {
  XeTEPSwatchRow* row = [[[XeTEPSwatchRow alloc] initWithFrame:CGRectZero] autorelease];
  [content_ addSubview:row];
  [rows_ addObject:row];
  return row;
}

- (XeTEPChipRow*)addChipRow {
  XeTEPChipRow* row = [[[XeTEPChipRow alloc] initWithFrame:CGRectZero] autorelease];
  [content_ addSubview:row];
  [rows_ addObject:row];
  return row;
}

- (void)updateHeader {
  NSString* name = xe::ui::XeniaTouchConfiguredControlLabelText(control_, YES);
  header_title_.text = name.length ? name : @"Control";
  NSString* primary = [self primaryBindingTitle];
  [primary_binding_button_ setTitle:(primary.length ? primary : name)
                           forState:UIControlStateNormal];
  primary_binding_button_.accessibilityLabel =
      [NSString stringWithFormat:@"Primary binding %@", primary.length ? primary : name];
  primary_binding_button_.menu = [self primaryBindingMenu];
  primary_binding_button_.showsMenuAsPrimaryAction = primary_binding_button_.menu != nil;
  primary_binding_button_.hidden = primary_binding_button_.menu == nil;
  const CGFloat width_units = MAX(1.0f, MIN(99.0f, control_.normalized_frame.width * 100.0f));
  NSString* base = [NSString stringWithFormat:@"%@, %.0fu, %.0f%%", [self typeName], width_units,
                                              control_.visual_opacity * 100.0f];
  NSString* assignment = [self assignmentSummary];
  header_subtitle_.text = base;
  UIMenu* assignment_menu = [self assignmentQuickMenu];
  if (assignment.length == 0 && assignment_menu) {
    assignment = [self emptyAssignmentTitle];
  }
  [assignment_badge_ setTitle:assignment forState:UIControlStateNormal];
  assignment_badge_.hidden = assignment_menu == nil;
  assignment_badge_.menu = assignment_menu;
  assignment_badge_.accessibilityLabel =
      assignment.length ? [NSString stringWithFormat:@"Assigned %@", assignment] : nil;
}

- (NSString*)typeName {
  switch (control_.type) {
    case IOSTouchControlType::kActionButton:
      return @"Face Button";
    case IOSTouchControlType::kMoveStick:
      return @"Analog Stick";
    case IOSTouchControlType::kLookSwipeZone:
      return @"Look Zone";
    case IOSTouchControlType::kPauseButton:
      return @"Menu Button";
  }
  return @"Control";
}

- (NSString*)displayForAction:(IOSTouchAction)action {
  return [NSString stringWithUTF8String:xe::hid::touch::IOSTouchActionDisplayName(action)];
}

- (NSString*)displayForOutput:(IOSTouchAnalogOutput)output {
  return [NSString stringWithUTF8String:xe::hid::touch::IOSTouchAnalogOutputDisplayName(output)];
}

- (NSString*)displayForTrigger:(IOSTouchInteractionTrigger)trigger {
  return [NSString
      stringWithUTF8String:xe::hid::touch::IOSTouchInteractionTriggerDisplayName(trigger)];
}

- (NSString*)primaryBindingTitle {
  if (control_.type == IOSTouchControlType::kPauseButton) {
    return @"Pause";
  }
  return [self displayForAction:control_.action];
}

- (UIMenu*)primaryBindingMenu {
  if (control_.type == IOSTouchControlType::kActionButton) {
    return [self tapActionMenu];
  }
  if (control_.type == IOSTouchControlType::kMoveStick ||
      control_.type == IOSTouchControlType::kLookSwipeZone) {
    return [self bindingMoveLookMenu];
  }
  return nil;
}

- (UIMenu*)assignmentQuickMenu {
  NSMutableArray<UIMenuElement*>* children = [NSMutableArray array];
  if (control_.type == IOSTouchControlType::kActionButton) {
    UIMenu* hold_slide = [self dragOutputMenu];
    if (hold_slide.children.count) {
      [children addObject:[UIMenu menuWithTitle:@"Held Slide" children:hold_slide.children]];
    }
    UIMenu* extra = [self behaviorQuickMenu];
    if (extra.children.count) {
      [children addObjectsFromArray:extra.children];
    }
  } else if (control_.type == IOSTouchControlType::kMoveStick) {
    [children addObject:[self dpadRingAction]];
    UIMenu* extra = [self behaviorQuickMenu];
    if (extra.children.count) {
      [children addObjectsFromArray:extra.children];
    }
  }
  return children.count ? [UIMenu menuWithTitle:@"Assignments" children:children] : nil;
}

- (NSString*)emptyAssignmentTitle {
  if (control_.type == IOSTouchControlType::kMoveStick) {
    return @"D-Pad + Extra";
  }
  if (control_.type == IOSTouchControlType::kActionButton) {
    return @"Extra";
  }
  return @"";
}

- (BOOL)hasAnyExtraAssignment {
  if (control_.type == IOSTouchControlType::kActionButton &&
      [self effectiveDragOutput:control_.drag_output
                   relativeLook:control_.enables_relative_look] != IOSTouchAnalogOutput::kNone) {
    return YES;
  }
  if (control_.type == IOSTouchControlType::kMoveStick && control_.move_with_dpad_ring) {
    return YES;
  }
  const auto& secondary = control_.secondary_behavior;
  return secondary.trigger != IOSTouchInteractionTrigger::kNone ||
         secondary.action != IOSTouchAction::kNone ||
         [self effectiveDragOutput:secondary.analog_output
                      relativeLook:secondary.enables_relative_look] != IOSTouchAnalogOutput::kNone;
}

- (NSString*)assignmentSummary {
  NSMutableArray<NSString*>* parts = [NSMutableArray arrayWithCapacity:3];
  IOSTouchAnalogOutput primary_drag = [self effectiveDragOutput:control_.drag_output
                                                   relativeLook:control_.enables_relative_look];
  if (primary_drag != IOSTouchAnalogOutput::kNone) {
    [parts addObject:[NSString
                         stringWithFormat:@"Held Slide: %@", [self displayForOutput:primary_drag]]];
  }
  if (control_.type == IOSTouchControlType::kMoveStick && control_.move_with_dpad_ring) {
    [parts addObject:@"D-Pad Ring"];
  }

  const auto& secondary = control_.secondary_behavior;
  if (secondary.trigger != IOSTouchInteractionTrigger::kNone) {
    NSString* trigger_name = secondary.trigger == IOSTouchInteractionTrigger::kHoldDrag
                                 ? @"Extra Hold Drag"
                                 : [self displayForTrigger:secondary.trigger];
    NSMutableString* behavior = [NSMutableString stringWithString:trigger_name];
    if (secondary.action != IOSTouchAction::kNone) {
      [behavior appendFormat:@": %@", [self displayForAction:secondary.action]];
    }
    IOSTouchAnalogOutput secondary_drag =
        [self effectiveDragOutput:secondary.analog_output
                     relativeLook:secondary.enables_relative_look];
    if (secondary_drag != IOSTouchAnalogOutput::kNone) {
      [behavior appendFormat:@"%@%@", secondary.action == IOSTouchAction::kNone ? @": " : @" + ",
                             [self displayForOutput:secondary_drag]];
    }
    [parts addObject:behavior];
  }

  return [parts componentsJoinedByString:@" | "];
}

- (IOSTouchAnalogOutput)effectiveDragOutput:(IOSTouchAnalogOutput)output
                               relativeLook:(bool)relative_look {
  if (output == IOSTouchAnalogOutput::kNone && relative_look) {
    return IOSTouchAnalogOutput::kLook;
  }
  return output;
}

- (BOOL)selectedControlHasTunableAnalogOutput {
  if (control_.type == IOSTouchControlType::kMoveStick ||
      control_.type == IOSTouchControlType::kLookSwipeZone) {
    return YES;
  }
  if (control_.type != IOSTouchControlType::kActionButton) {
    return NO;
  }
  if ([self effectiveDragOutput:control_.drag_output
                   relativeLook:control_.enables_relative_look] != IOSTouchAnalogOutput::kNone) {
    return YES;
  }
  return control_.secondary_behavior.trigger != IOSTouchInteractionTrigger::kNone &&
         [self effectiveDragOutput:control_.secondary_behavior.analog_output
                      relativeLook:control_.secondary_behavior.enables_relative_look] !=
             IOSTouchAnalogOutput::kNone;
}

- (void)rebuildContent {
  [self clearRows];
  if (!has_control_) {
    return;
  }
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;

  // APPEARANCE -------------------------------------------------------------
  [self addSection:@"Appearance"];

  XeTEPSliderRow* opacity = [self addSliderRow];
  [opacity configureWithTitle:@"Opacity"
                        value:control_.visual_opacity
                          min:0.2f
                          max:1.0f
                      percent:YES];
  opacity.onBegin = ^{
    [unsafe_self->delegate_ touchEditPanelDidBeginOpacityChange:unsafe_self];
  };
  opacity.onChange = ^(float value) {
    [unsafe_self->delegate_ touchEditPanel:unsafe_self didChangeOpacity:value];
  };
  opacity.onEnd = ^{
    [unsafe_self->delegate_ touchEditPanelDidEndOpacityChange:unsafe_self];
  };

  XeTEPSwatchRow* tint = [self addSwatchRow];
  NSMutableArray<UIColor*>* colors = [NSMutableArray array];
  NSInteger selected = 0;
  NSInteger tint_index = 0;
  for (IOSTouchTintStyle style : xe::hid::touch::kIOSTouchEditableTintStyles) {
    UIColor* color = xe::ui::XeniaTouchOverlayAccentColor(style, control_.type);
    [colors addObject:(color ? color : [UIColor grayColor])];
    if (style == control_.tint_style) {
      selected = tint_index;
    }
    ++tint_index;
  }
  [tint configureWithTitle:@"Tint" colors:colors selectedIndex:selected];
  tint.onSelect = ^(NSInteger index) {
    NSInteger i = 0;
    for (IOSTouchTintStyle style : xe::hid::touch::kIOSTouchEditableTintStyles) {
      if (i == index) {
        [unsafe_self->delegate_ touchEditPanel:unsafe_self didSelectTint:style];
        return;
      }
      ++i;
    }
  };

  if (control_.type == IOSTouchControlType::kActionButton) {
    XeTEPChipRow* shape = [self addChipRow];
    [shape configureWithTitle:@"Shape"
                     subtitle:nil
                        value:xe::ui::ios::touch_overlay::TouchControlShapeDisplayText(
                                  control_.shape)];
    shape.chipButton.menu = [self shapeMenu];
    shape.chipButton.showsMenuAsPrimaryAction = YES;
  }

  XeTEPChipRow* label = [self addChipRow];
  NSString* label_text = xe::ui::XeniaTouchConfiguredControlLabelText(control_, YES);
  NSString* label_value =
      control_.label_hidden ? @"Hidden" : (label_text.length ? label_text : @"Default");
  [label configureWithTitle:@"Label" subtitle:nil value:label_value];
  label.chipButton.menu = [self labelMenu];
  label.chipButton.showsMenuAsPrimaryAction = YES;

  // BEHAVIOR ---------------------------------------------------------------
  if (control_.type == IOSTouchControlType::kActionButton) {
    [self addSection:@"Behavior"];

    XeTEPChipRow* tap = [self addChipRow];
    [tap configureWithTitle:@"Tap"
                   subtitle:@"Press & release"
                      value:[self displayForAction:control_.action]];
    tap.chipButton.menu = [self tapActionMenu];
    tap.chipButton.showsMenuAsPrimaryAction = YES;

    XeTEPChipRow* drag = [self addChipRow];
    IOSTouchAnalogOutput drag_output = [self effectiveDragOutput:control_.drag_output
                                                    relativeLook:control_.enables_relative_look];
    [drag configureWithTitle:@"Held Slide"
                    subtitle:@"Slide while holding"
                       value:[self displayForOutput:drag_output]];
    drag.chipButton.menu = [self dragOutputMenu];
    drag.chipButton.showsMenuAsPrimaryAction = YES;

    XeTEPChipRow* gesture = [self addChipRow];
    [gesture configureWithTitle:@"Extra gesture"
                       subtitle:@"Flick for a 2nd input"
                          value:[self displayForTrigger:control_.secondary_behavior.trigger]];
    gesture.chipButton.menu = [self gestureTriggerMenu];
    gesture.chipButton.showsMenuAsPrimaryAction = YES;

    if (control_.secondary_behavior.trigger != IOSTouchInteractionTrigger::kNone) {
      XeTEPChipRow* then_button = [self addChipRow];
      [then_button configureWithTitle:@"Then button"
                             subtitle:nil
                                value:[self displayForAction:control_.secondary_behavior.action]];
      then_button.chipButton.menu = [self gestureActionMenu];
      then_button.chipButton.showsMenuAsPrimaryAction = YES;

      XeTEPChipRow* then_drag = [self addChipRow];
      IOSTouchAnalogOutput secondary_output =
          [self effectiveDragOutput:control_.secondary_behavior.analog_output
                       relativeLook:control_.secondary_behavior.enables_relative_look];
      [then_drag configureWithTitle:@"Then drag"
                           subtitle:nil
                              value:[self displayForOutput:secondary_output]];
      then_drag.chipButton.menu = [self gestureDragMenu];
      then_drag.chipButton.showsMenuAsPrimaryAction = YES;
    }

    if ([self selectedControlHasTunableAnalogOutput]) {
      [self addSection:@"Feel"];
      [self addTuneRow];
    }
  } else if (control_.type == IOSTouchControlType::kMoveStick ||
             control_.type == IOSTouchControlType::kLookSwipeZone) {
    [self addSection:@"Behavior"];
    XeTEPChipRow* binding = [self addChipRow];
    [binding configureWithTitle:@"Binding"
                       subtitle:nil
                          value:[self displayForAction:control_.action]];
    binding.chipButton.menu = [self bindingMoveLookMenu];
    binding.chipButton.showsMenuAsPrimaryAction = YES;

    [self addSection:@"Feel"];
    [self addTuneRow];
  }
}

- (void)addTuneRow {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  XeTEPChipRow* tune = [self addChipRow];
  [tune configureWithTitle:@"Tune response"
                  subtitle:@"Deadzone, curve, smoothing"
                     value:@"Adjust"];
  [tune.chipButton addAction:[UIAction actionWithHandler:^(__unused UIAction* action) {
                     [unsafe_self->delegate_ touchEditPanelDidRequestTune:unsafe_self];
                   }]
            forControlEvents:UIControlEventTouchUpInside];
}

#pragma mark - Pickers (reuse the UIMenu pattern; call existing-style delegate)

- (UIMenu*)tapActionMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  IOSTouchAction current = control_.action;
  NSMutableArray<UIMenuElement*>* items = [NSMutableArray array];
  for (IOSTouchAction action : xe::hid::touch::kIOSTouchEditableActions) {
    UIAction* item = [UIAction actionWithTitle:[self displayForAction:action]
                                         image:nil
                                    identifier:nil
                                       handler:^(__unused UIAction* a) {
                                         [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                                             didSelectTapAction:action];
                                       }];
    item.state = (action == current) ? UIMenuElementStateOn : UIMenuElementStateOff;
    [items addObject:item];
  }
  return [UIMenu menuWithTitle:@"" children:items];
}

- (UIMenu*)dragOutputMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  IOSTouchAnalogOutput current = [self effectiveDragOutput:control_.drag_output
                                              relativeLook:control_.enables_relative_look];
  NSMutableArray<UIMenuElement*>* items = [NSMutableArray array];
  for (IOSTouchAnalogOutput output : xe::hid::touch::kIOSTouchEditableAnalogOutputs) {
    UIAction* item = [UIAction actionWithTitle:[self displayForOutput:output]
                                         image:nil
                                    identifier:nil
                                       handler:^(__unused UIAction* a) {
                                         [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                                            didSelectDragOutput:output];
                                       }];
    item.state = (output == current) ? UIMenuElementStateOn : UIMenuElementStateOff;
    [items addObject:item];
  }
  return [UIMenu menuWithTitle:@"" children:items];
}

- (UIMenu*)gestureTriggerMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  IOSTouchInteractionTrigger current = control_.secondary_behavior.trigger;
  std::vector<IOSTouchInteractionTrigger> triggers;
  if (control_.type == IOSTouchControlType::kMoveStick) {
    triggers = {IOSTouchInteractionTrigger::kNone, IOSTouchInteractionTrigger::kDoubleTapForward};
  } else {
    triggers = {IOSTouchInteractionTrigger::kNone, IOSTouchInteractionTrigger::kHold,
                IOSTouchInteractionTrigger::kHoldDrag, IOSTouchInteractionTrigger::kDoubleTap};
  }
  NSMutableArray<UIMenuElement*>* items = [NSMutableArray array];
  for (IOSTouchInteractionTrigger trigger : triggers) {
    UIAction* item = [UIAction actionWithTitle:[self displayForTrigger:trigger]
                                         image:nil
                                    identifier:nil
                                       handler:^(__unused UIAction* a) {
                                         [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                                        didSelectGestureTrigger:trigger];
                                       }];
    item.state = (trigger == current) ? UIMenuElementStateOn : UIMenuElementStateOff;
    [items addObject:item];
  }
  return [UIMenu menuWithTitle:@"" children:items];
}

- (UIMenu*)gestureActionMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  IOSTouchAction current = control_.secondary_behavior.action;
  NSMutableArray<UIMenuElement*>* items = [NSMutableArray array];
  for (IOSTouchAction action : xe::hid::touch::kIOSTouchEditableActions) {
    UIAction* item = [UIAction actionWithTitle:[self displayForAction:action]
                                         image:nil
                                    identifier:nil
                                       handler:^(__unused UIAction* a) {
                                         [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                                         didSelectGestureAction:action];
                                       }];
    item.state = (action == current) ? UIMenuElementStateOn : UIMenuElementStateOff;
    [items addObject:item];
  }
  return [UIMenu menuWithTitle:@"" children:items];
}

- (UIMenu*)gestureDragMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  IOSTouchAnalogOutput current =
      [self effectiveDragOutput:control_.secondary_behavior.analog_output
                   relativeLook:control_.secondary_behavior.enables_relative_look];
  NSMutableArray<UIMenuElement*>* items = [NSMutableArray array];
  for (IOSTouchAnalogOutput output : xe::hid::touch::kIOSTouchEditableAnalogOutputs) {
    UIAction* item = [UIAction actionWithTitle:[self displayForOutput:output]
                                         image:nil
                                    identifier:nil
                                       handler:^(__unused UIAction* a) {
                                         [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                                     didSelectGestureDragOutput:output];
                                       }];
    item.state = (output == current) ? UIMenuElementStateOn : UIMenuElementStateOff;
    [items addObject:item];
  }
  return [UIMenu menuWithTitle:@"" children:items];
}

- (UIMenu*)bindingMoveLookMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  IOSTouchAction current = control_.action;
  IOSTouchAction choices[] = {IOSTouchAction::kMove, IOSTouchAction::kLook};
  NSMutableArray<UIMenuElement*>* items = [NSMutableArray array];
  for (IOSTouchAction action : choices) {
    UIAction* item = [UIAction actionWithTitle:[self displayForAction:action]
                                         image:nil
                                    identifier:nil
                                       handler:^(__unused UIAction* a) {
                                         [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                                             didSelectTapAction:action];
                                       }];
    item.state = (action == current) ? UIMenuElementStateOn : UIMenuElementStateOff;
    [items addObject:item];
  }
  if (control_.type == IOSTouchControlType::kMoveStick) {
    [items addObject:[self dpadRingAction]];
  }
  return [UIMenu menuWithTitle:@"" children:items];
}

- (NSArray<UIMenuElement*>*)shapeActions {
  if (control_.type != IOSTouchControlType::kActionButton) {
    return @[];
  }

  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  IOSTouchControlShape current_shape = control_.shape;
  NSMutableArray<UIMenuElement*>* items =
      [NSMutableArray arrayWithCapacity:sizeof(xe::ui::ios::touch_overlay::kEditShapeChoices) /
                                        sizeof(xe::ui::ios::touch_overlay::kEditShapeChoices[0])];
  for (IOSTouchControlShape shape : xe::ui::ios::touch_overlay::kEditShapeChoices) {
    UIAction* item = [UIAction
        actionWithTitle:xe::ui::ios::touch_overlay::TouchControlShapeDisplayText(shape)
                  image:nil
             identifier:nil
                handler:^(__unused UIAction* action) {
                  [unsafe_self->delegate_ touchEditPanel:unsafe_self didSelectShape:shape];
                }];
    item.state = shape == current_shape ? UIMenuElementStateOn : UIMenuElementStateOff;
    [items addObject:item];
  }
  return items;
}

- (UIMenu*)shapeMenu {
  NSArray<UIMenuElement*>* items = [self shapeActions];
  return [UIMenu menuWithTitle:@"" children:items];
}

- (UIMenu*)labelMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  const bool has_custom_label = xe::hid::touch::IOSTouchControlHasCustomLabel(control_);
  NSMutableArray<UIMenuElement*>* children = [NSMutableArray arrayWithCapacity:3];

  UIAction* rename = [UIAction actionWithTitle:@"Rename..."
                                         image:[UIImage systemImageNamed:@"text.cursor"]
                                    identifier:nil
                                       handler:^(__unused UIAction* action) {
                                         [unsafe_self->delegate_
                                             touchEditPanelDidRequestRenameLabel:unsafe_self];
                                       }];
  [children addObject:rename];

  UIAction* visibility =
      [UIAction actionWithTitle:(control_.label_hidden ? @"Show Label" : @"Hide Label")
                          image:[UIImage systemImageNamed:(control_.label_hidden
                                                              ? @"text.badge.checkmark"
                                                              : @"text.badge.xmark")]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                            didSelectLabelHidden:!unsafe_self->control_.label_hidden];
                        }];
  [children addObject:visibility];

  UIAction* reset = [UIAction actionWithTitle:@"Use Default Label"
                                        image:nil
                                   identifier:nil
                                      handler:^(__unused UIAction* action) {
                                        [unsafe_self->delegate_
                                            touchEditPanelDidRequestResetLabel:unsafe_self];
                                      }];
  if (!has_custom_label) {
    reset.attributes = UIMenuElementAttributesDisabled;
  }
  [children addObject:reset];

  return [UIMenu menuWithTitle:@"Label" children:children];
}

- (UIAction*)dpadRingAction {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  NSString* title = control_.move_with_dpad_ring ? @"D-Pad Ring On" : @"D-Pad Ring Off";
  UIAction* item = [UIAction
      actionWithTitle:title
                image:[UIImage systemImageNamed:@"dpad"]
           identifier:nil
              handler:^(__unused UIAction* action) {
                [unsafe_self->delegate_ touchEditPanelDidRequestToggleMoveDpadRing:unsafe_self];
              }];
  item.state = control_.move_with_dpad_ring ? UIMenuElementStateOn : UIMenuElementStateOff;
  return item;
}

- (UIMenu*)layoutQuickMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  NSMutableArray<UIMenuElement*>* children = [NSMutableArray array];
  UIAction* mirror =
      [UIAction actionWithTitle:@"Mirror"
                          image:[UIImage systemImageNamed:@"arrow.left.and.right"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self->delegate_ touchEditPanelDidRequestMirror:unsafe_self];
                        }];
  [children addObject:mirror];
  UIAction* match_size =
      [UIAction actionWithTitle:@"Match Size..."
                          image:[UIImage systemImageNamed:@"arrow.up.left.and.arrow.down.right"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self->delegate_ touchEditPanelDidRequestMatchSize:unsafe_self];
                        }];
  [children addObject:match_size];

  NSArray<UIMenuElement*>* shape_actions = [self shapeActions];
  if (shape_actions.count) {
    [children addObject:[UIMenu menuWithTitle:@"Button Shape"
                                        image:[UIImage systemImageNamed:@"capsule"]
                                   identifier:nil
                                      options:UIMenuOptionsDisplayInline
                                     children:shape_actions]];
  }
  return [UIMenu menuWithTitle:@"Layout" children:children];
}

- (UIMenu*)behaviorQuickMenu {
  __unsafe_unretained XeniaTouchEditPanel* unsafe_self = self;
  const auto& current = control_.secondary_behavior;
  IOSTouchAnalogOutput current_output = [self effectiveDragOutput:current.analog_output
                                                     relativeLook:current.enables_relative_look];
  UIAction* off =
      [UIAction actionWithTitle:@"Extra Off"
                          image:[UIImage systemImageNamed:@"xmark.circle"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [unsafe_self->delegate_ touchEditPanelDidRequestClearExtras:unsafe_self];
                        }];
  off.state = [self hasAnyExtraAssignment] ? UIMenuElementStateOff : UIMenuElementStateOn;

  UIAction* (^buttonPreset)(NSString*, IOSTouchInteractionTrigger, IOSTouchAction) =
      ^UIAction*(NSString* title, IOSTouchInteractionTrigger trigger, IOSTouchAction action) {
        UIAction* preset =
            [UIAction actionWithTitle:title
                                image:nil
                           identifier:nil
                              handler:^(__unused UIAction* selected) {
                                [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                          didSelectQuickGestureTrigger:trigger
                                                                action:action
                                                            dragOutput:IOSTouchAnalogOutput::kNone];
                              }];
        preset.state = current.trigger == trigger && current.action == action &&
                               current_output == IOSTouchAnalogOutput::kNone
                           ? UIMenuElementStateOn
                           : UIMenuElementStateOff;
        return preset;
      };

  if (control_.type == IOSTouchControlType::kMoveStick) {
    NSMutableArray<UIMenuElement*>* double_tap_forward = [NSMutableArray array];
    for (IOSTouchAction action : xe::hid::touch::kIOSTouchEditableActions) {
      if (action == IOSTouchAction::kNone) {
        continue;
      }
      NSString* action_name = [self displayForAction:action];
      [double_tap_forward
          addObject:buttonPreset([NSString stringWithFormat:@"Double Tap Forward %@", action_name],
                                 IOSTouchInteractionTrigger::kDoubleTapForward, action)];
    }
    return [UIMenu menuWithTitle:@"Extra Function"
                        children:@[
                          off,
                          [UIMenu menuWithTitle:@"Double Tap Forward" children:double_tap_forward],
                        ]];
  }

  UIAction* (^dragPreset)(NSString*, IOSTouchAnalogOutput) =
      ^UIAction*(NSString* title, IOSTouchAnalogOutput output) {
        UIAction* preset = [UIAction
            actionWithTitle:title
                      image:nil
                 identifier:nil
                    handler:^(__unused UIAction* selected) {
                      [unsafe_self->delegate_ touchEditPanel:unsafe_self
                                didSelectQuickGestureTrigger:IOSTouchInteractionTrigger::kHoldDrag
                                                      action:IOSTouchAction::kNone
                                                  dragOutput:output];
                    }];
        preset.state = current.trigger == IOSTouchInteractionTrigger::kHoldDrag &&
                               current.action == IOSTouchAction::kNone && current_output == output
                           ? UIMenuElementStateOn
                           : UIMenuElementStateOff;
        return preset;
      };

  NSMutableArray<UIMenuElement*>* hold = [NSMutableArray array];
  NSMutableArray<UIMenuElement*>* double_tap = [NSMutableArray array];
  for (IOSTouchAction action : xe::hid::touch::kIOSTouchEditableActions) {
    if (action == IOSTouchAction::kNone) {
      continue;
    }
    NSString* action_name = [self displayForAction:action];
    [hold addObject:buttonPreset([NSString stringWithFormat:@"Hold %@", action_name],
                                 IOSTouchInteractionTrigger::kHold, action)];
    [double_tap addObject:buttonPreset([NSString stringWithFormat:@"Double Tap %@", action_name],
                                       IOSTouchInteractionTrigger::kDoubleTap, action)];
  }
  NSMutableArray<UIMenuElement*>* children = [NSMutableArray array];
  [children addObject:off];
  [children addObject:[UIMenu menuWithTitle:@"Hold" children:hold]];
  [children addObject:[UIMenu menuWithTitle:@"Double Tap" children:double_tap]];
  if (control_.type == IOSTouchControlType::kActionButton) {
    NSArray<UIAction*>* drag = @[
      dragPreset(@"Look", IOSTouchAnalogOutput::kLook),
      dragPreset(@"Move", IOSTouchAnalogOutput::kMove),
    ];
    [children addObject:[UIMenu menuWithTitle:@"Extra Hold Drag" children:drag]];
  }

  return [UIMenu menuWithTitle:@"Extra Function" children:children];
}

#pragma mark - Actions

- (void)collapseTapped:(UIButton*)__unused sender {
  [self setExpanded:!expanded_ animated:YES];
  [delegate_ touchEditPanelDidToggleExpansion:self];
}

- (void)smallerTapped:(UIButton*)__unused sender {
  [delegate_ touchEditPanelDidRequestSmaller:self];
}

- (void)largerTapped:(UIButton*)__unused sender {
  [delegate_ touchEditPanelDidRequestLarger:self];
}

- (void)layoutTapped:(UIButton*)__unused sender {
  [delegate_ touchEditPanelDidRequestMatchSize:self];
}

- (void)tuneTapped:(UIButton*)__unused sender {
  [delegate_ touchEditPanelDidRequestTune:self];
}

- (void)duplicateTapped:(UIButton*)__unused sender {
  [delegate_ touchEditPanelDidRequestDuplicate:self];
}

- (void)deleteTapped:(UIButton*)__unused sender {
  [delegate_ touchEditPanelDidRequestDelete:self];
}

@end
