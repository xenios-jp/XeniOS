/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_edit_command_bar_ios.h"

#include <cmath>

#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

constexpr CGFloat kBarHeight = 48.0f;
constexpr CGFloat kPillHeight = 44.0f;
constexpr CGFloat kGap = 6.0f;
constexpr CGFloat kIconButtonWidth = 44.0f;
constexpr CGFloat kDoneWidth = 72.0f;
constexpr CGFloat kUndoRedoButtonWidth = 44.0f;

// SF Symbol names. Wrong names render blank rather than crashing.
NSString* const kLayoutsSymbol = @"rectangle.stack";
NSString* const kUndoSymbol = @"arrow.uturn.backward";
NSString* const kRedoSymbol = @"arrow.uturn.forward";
NSString* const kGridSymbol = @"square.grid.3x3.fill";
NSString* const kAddSymbol = @"plus";

UIColor* SoftFill() { return [[UIColor whiteColor] colorWithAlphaComponent:0.10f]; }
UIColor* PrimaryTint() { return [[UIColor whiteColor] colorWithAlphaComponent:0.94f]; }

UIButton* MakeIconButton(NSString* symbol_name, id target, SEL selector) {
  UIButton* button = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
  button.tintColor = PrimaryTint();
  button.layer.cornerRadius = kPillHeight * 0.5f;
  UIImageSymbolConfiguration* config =
      [UIImageSymbolConfiguration configurationWithPointSize:21.0f
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* image = [UIImage systemImageNamed:symbol_name withConfiguration:config];
  if (image) {
    [button setImage:image forState:UIControlStateNormal];
  }
  button.accessibilityTraits = UIAccessibilityTraitButton;
  [button addTarget:target action:selector forControlEvents:UIControlEventTouchUpInside];
  return button;
}

UIView* MakePill() {
  UIView* pill = [[UIView alloc] initWithFrame:CGRectZero];
  pill.backgroundColor = SoftFill();
  pill.layer.cornerRadius = kPillHeight * 0.5f;
  return pill;
}

}  // namespace

@implementation XeniaTouchEditCommandBar {
  id<XeniaTouchEditCommandBarDelegate> delegate_;  // assign (unowned)
  UIButton* name_button_;
  UIView* history_pill_;
  UIButton* undo_button_;
  UIButton* redo_button_;
  UIView* command_pill_;
  UIButton* grid_button_;
  UIButton* add_button_;
  UIButton* done_button_;
  BOOL grid_active_;
  BOOL can_undo_;
  BOOL can_redo_;
}

@synthesize delegate = delegate_;

+ (CGFloat)preferredHeight {
  return kBarHeight;
}

- (BOOL)showsHistoryControls {
  return can_undo_ || can_redo_;
}

- (CGFloat)historyPillWidth {
  if (![self showsHistoryControls]) {
    return 0.0f;
  }
  return can_undo_ && can_redo_ ? kUndoRedoButtonWidth * 2.0f : kUndoRedoButtonWidth;
}

- (CGFloat)commandPillWidth {
  return kIconButtonWidth * 3.0f;
}

- (CGFloat)preferredWidth {
  const BOOL show_history = [self showsHistoryControls];
  const CGFloat history_w = [self historyPillWidth];
  const CGFloat command_w = [self commandPillWidth];
  return history_w + (show_history ? kGap : 0.0f) + command_w + kGap + kDoneWidth;
}

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (!self) {
    return nil;
  }
  self.backgroundColor = [UIColor clearColor];

  name_button_ = MakeIconButton(kLayoutsSymbol, self, @selector(nameTapped:));
  name_button_.accessibilityLabel = @"Layout menu";
  [self setLayoutName:@"Layout"];

  history_pill_ = MakePill();
  [self addSubview:history_pill_];
  undo_button_ = MakeIconButton(kUndoSymbol, self, @selector(undoTapped:));
  undo_button_.accessibilityLabel = @"Undo";
  [history_pill_ addSubview:undo_button_];
  redo_button_ = MakeIconButton(kRedoSymbol, self, @selector(redoTapped:));
  redo_button_.accessibilityLabel = @"Redo";
  [history_pill_ addSubview:redo_button_];

  command_pill_ = MakePill();
  [self addSubview:command_pill_];
  [command_pill_ addSubview:name_button_];
  grid_button_ = MakeIconButton(kGridSymbol, self, @selector(gridTapped:));
  grid_button_.accessibilityLabel = @"Grid snap";
  [command_pill_ addSubview:grid_button_];
  add_button_ = MakeIconButton(kAddSymbol, self, @selector(addTapped:));
  add_button_.accessibilityLabel = @"Add control";
  [command_pill_ addSubview:add_button_];

  done_button_ = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
  done_button_.backgroundColor = [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.95f];
  done_button_.layer.cornerRadius = kPillHeight * 0.5f;
  xe_apply_button_title_font(done_button_, UIFontTextStyleSubheadline, 17.0, UIFontWeightSemibold);
  [done_button_ setTitleColor:[UIColor blackColor] forState:UIControlStateNormal];
  [done_button_ setTitle:@"Done" forState:UIControlStateNormal];
  done_button_.accessibilityTraits = UIAccessibilityTraitButton;
  [done_button_ addTarget:self
                   action:@selector(doneTapped:)
         forControlEvents:UIControlEventTouchUpInside];
  [self addSubview:done_button_];

  grid_active_ = NO;
  can_undo_ = NO;
  can_redo_ = NO;
  [self applyIcon:grid_button_ active:grid_active_];

  return self;
}

- (void)dealloc {
  [name_button_ release];
  [undo_button_ release];
  [redo_button_ release];
  [history_pill_ release];
  [grid_button_ release];
  [add_button_ release];
  [command_pill_ release];
  [done_button_ release];
  [super dealloc];
}

- (void)applyIcon:(UIButton*)button active:(BOOL)active {
  if (active) {
    button.backgroundColor = [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.95f];
    button.tintColor = [UIColor blackColor];
  } else {
    button.backgroundColor = [UIColor clearColor];
    button.tintColor = PrimaryTint();
  }
}

#pragma mark - Layout

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat width = CGRectGetWidth(self.bounds);
  const CGFloat height = CGRectGetHeight(self.bounds);
  const CGFloat pill_y = (height - kPillHeight) * 0.5f;
  const BOOL show_history = [self showsHistoryControls];
  const CGFloat history_w = [self historyPillWidth];
  const CGFloat command_w = [self commandPillWidth];
  const CGFloat total_w = [self preferredWidth];
  CGFloat x = MAX(0.0f, std::floor((width - total_w) * 0.5f));

  history_pill_.hidden = !show_history;
  if (show_history) {
    history_pill_.frame = CGRectMake(x, pill_y, history_w, kPillHeight);
    undo_button_.hidden = !can_undo_;
    redo_button_.hidden = !can_redo_;
    CGFloat history_x = 0.0f;
    if (can_undo_) {
      undo_button_.frame = CGRectMake(history_x, 0.0f, kUndoRedoButtonWidth, kPillHeight);
      history_x += kUndoRedoButtonWidth;
    } else {
      undo_button_.frame = CGRectZero;
    }
    if (can_redo_) {
      redo_button_.frame = CGRectMake(history_x, 0.0f, kUndoRedoButtonWidth, kPillHeight);
    } else {
      redo_button_.frame = CGRectZero;
    }
    x = CGRectGetMaxX(history_pill_.frame) + kGap;
  } else {
    history_pill_.frame = CGRectZero;
    undo_button_.hidden = YES;
    redo_button_.hidden = YES;
    undo_button_.frame = CGRectZero;
    redo_button_.frame = CGRectZero;
  }

  command_pill_.frame = CGRectMake(x, pill_y, command_w, kPillHeight);
  name_button_.hidden = NO;
  name_button_.frame = CGRectMake(kIconButtonWidth * 0.0f, 0.0f, kIconButtonWidth, kPillHeight);
  grid_button_.frame = CGRectMake(kIconButtonWidth * 1.0f, 0.0f, kIconButtonWidth, kPillHeight);
  add_button_.frame = CGRectMake(kIconButtonWidth * 2.0f, 0.0f, kIconButtonWidth, kPillHeight);
  x = CGRectGetMaxX(command_pill_.frame) + kGap;

  done_button_.frame = CGRectMake(x, pill_y, kDoneWidth, kPillHeight);
}

#pragma mark - State

- (void)setLayoutName:(NSString*)name {
  NSString* text = name.length ? name : @"Layout";
  name_button_.accessibilityLabel = [NSString stringWithFormat:@"Layout: %@", text];
}

- (void)setCanUndo:(BOOL)canUndo canRedo:(BOOL)canRedo {
  can_undo_ = canUndo;
  can_redo_ = canRedo;
  undo_button_.enabled = canUndo;
  undo_button_.alpha = canUndo ? 1.0f : 0.35f;
  redo_button_.enabled = canRedo;
  redo_button_.alpha = canRedo ? 1.0f : 0.35f;
  [self setNeedsLayout];
}

- (void)setGridActive:(BOOL)active {
  grid_active_ = active;
  [self applyIcon:grid_button_ active:active];
}

- (void)setAddMenu:(UIMenu*)menu {
  add_button_.menu = menu;
  add_button_.showsMenuAsPrimaryAction = menu != nil;
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  if (self.hidden || self.alpha <= 0.01f || !self.userInteractionEnabled) {
    return nil;
  }
  NSArray<UIView*>* subviews = self.subviews;
  for (NSInteger index = static_cast<NSInteger>(subviews.count) - 1; index >= 0; --index) {
    UIView* subview = [subviews objectAtIndex:static_cast<NSUInteger>(index)];
    if (subview.hidden || subview.alpha <= 0.01f || !subview.userInteractionEnabled) {
      continue;
    }
    CGPoint subview_point = [subview convertPoint:point fromView:self];
    UIView* hit = [subview hitTest:subview_point withEvent:event];
    if (hit) {
      return hit;
    }
  }
  return nil;
}

#pragma mark - Actions

- (void)nameTapped:(UIButton*)__unused sender {
  [delegate_ touchEditCommandBarDidRequestLayouts:self];
}

- (void)undoTapped:(UIButton*)__unused sender {
  [delegate_ touchEditCommandBarDidRequestUndo:self];
}

- (void)redoTapped:(UIButton*)__unused sender {
  [delegate_ touchEditCommandBarDidRequestRedo:self];
}

- (void)gridTapped:(UIButton*)__unused sender {
  [delegate_ touchEditCommandBarDidToggleGrid:self];
}

- (void)addTapped:(UIButton*)__unused sender {
  [delegate_ touchEditCommandBarDidRequestAdd:self];
}

- (void)doneTapped:(UIButton*)__unused sender {
  [delegate_ touchEditCommandBarDidRequestDone:self];
}

@end
