/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_controls_overlay_ios.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/hid/touch/touch_layout_editor.h"
#include "xenia/hid/touch/touch_layout_ios.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"
#import "xenia/ui/ios/touch/touch_control_shell_view_ios.h"
#import "xenia/ui/ios/touch/touch_controls_overlay_helpers_ios.h"
#import "xenia/ui/ios/touch/touch_edit_command_bar_ios.h"
#import "xenia/ui/ios/touch/touch_layout_library_view_ios.h"
#import "xenia/ui/ios/touch/touch_overlay_edit_chrome_ios.h"
#include "xenia/ui/ios/touch/touch_overlay_edit_history_ios.h"
#include "xenia/ui/ios/touch/touch_overlay_geometry_ios.h"
#include "xenia/ui/ios/touch/touch_overlay_style_ios.h"

DECLARE_bool(ios_touch_haptics);

namespace {

constexpr CFTimeInterval kTouchButtonRetapReleaseGapSeconds = 0.012;
constexpr NSInteger kTuningSliderAnalogField = 0;
constexpr NSInteger kTuningSliderHeldLook = 1;
constexpr NSInteger kTuningSliderHeldMove = 2;
constexpr NSInteger kTuningSliderOverallSensitivity = 3;
constexpr NSInteger kTuningSectionBasic = 0;
constexpr NSInteger kTuningSectionFeel = 1;
constexpr NSInteger kTuningSectionAdvanced = 2;

enum class TouchAnalogTuningField : uint8_t {
  kDeadzone = 0,
  kActivationRadius,
  kHorizontalScale,
  kVerticalScale,
  kDiagonalScale,
  kResponseCurve,
  kAccelerationScale,
  kSmoothing,
  kMaxOutput,
  kInvertX,
  kInvertY,
};

using xe::ui::CGRectFromTouchRect;
using xe::ui::ClampNormalizedControlFrame;
using xe::ui::NormalizedControlFrameFromResolvedFrame;
using xe::ui::ResolveNormalizedControlFrame;
using xe::ui::SnapTouchEditResolvedFrame;
using xe::ui::TouchControlPositionSpaceForControlType;
using xe::ui::TouchControlSizeSpaceForControlType;
using xe::ui::TouchEditGestureMode;
using xe::ui::TouchEditSnapOptions;
using xe::ui::TouchEditSnapResult;
using xe::ui::TouchLayoutSpaceForView;
using xe::ui::TouchOverlayIsPortraitForView;
using xe::ui::TouchSafeAreaSpaceForView;
using xe::ui::XeniaTouchConfiguredControlLabelText;
using xe::ui::XeniaTouchVisibleControlLabelText;

using namespace xe::ui::ios::touch_overlay;

xe::hid::touch::IOSTouchAnalogOutput EffectiveControlDragOutput(
    const xe::hid::touch::IOSTouchControlDefinition& control) {
  if (control.drag_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    return control.drag_output;
  }
  if (control.enables_relative_look) {
    return xe::hid::touch::IOSTouchAnalogOutput::kLook;
  }
  if (control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    return control.action == xe::hid::touch::IOSTouchAction::kMove
               ? xe::hid::touch::IOSTouchAnalogOutput::kMove
               : xe::hid::touch::IOSTouchAnalogOutput::kLook;
  }
  return xe::hid::touch::IOSTouchAnalogOutput::kNone;
}

bool IsTriggerAction(xe::hid::touch::IOSTouchAction action) {
  return action == xe::hid::touch::IOSTouchAction::kLeftTrigger ||
         action == xe::hid::touch::IOSTouchAction::kRightTrigger;
}

CGPoint TouchAnalogVectorForDelta(CGPoint delta, CFTimeInterval elapsed_seconds,
                                  const xe::hid::touch::IOSTouchAnalogTuning& tuning) {
  const float safe_elapsed = std::clamp(static_cast<float>(elapsed_seconds), 1.0f / 240.0f, 0.25f);
  const float velocity_points_per_second =
      static_cast<float>(std::hypot(delta.x, delta.y)) / safe_elapsed;
  const float velocity_full_scales_per_second =
      velocity_points_per_second / std::max(TouchLookPointsPerFullScale(), 1.0f);
  return ApplyTouchAnalogTuningWithVelocity(SwipeLookVectorForDelta(delta, 1.0f), tuning,
                                            velocity_full_scales_per_second);
}

void BlendMaxMagnitude(CGPoint vector, CGPoint* accumulator) {
  if (!accumulator) {
    return;
  }
  if (std::abs(vector.x) > std::abs(accumulator->x)) {
    accumulator->x = vector.x;
  }
  if (std::abs(vector.y) > std::abs(accumulator->y)) {
    accumulator->y = vector.y;
  }
}

std::string UniqueTouchControlIdentifier(const xe::hid::touch::IOSTouchLayoutModel& layout,
                                         const std::string& base_identifier) {
  int suffix = 1;
  while (true) {
    std::string identifier =
        suffix == 1 ? base_identifier : (base_identifier + "_" + std::to_string(suffix));
    const bool exists =
        std::any_of(layout.controls.begin(), layout.controls.end(),
                    [&identifier](const xe::hid::touch::IOSTouchControlDefinition& control) {
                      return control.identifier == identifier;
                    });
    if (!exists) {
      return identifier;
    }
    ++suffix;
  }
}

}  // namespace

@interface XeniaTouchControlsOverlayView () <XeniaTouchOverlayEditChromeIOSDelegate,
                                             XeniaTouchEditCommandBarDelegate>

- (void)finalizeTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled;
- (BOOL)copySelectedAnalogTuningForPanel:(xe::hid::touch::IOSTouchAnalogTuning*)tuning
                                deadzone:(float*)deadzone
                        activationRadius:(float*)activation_radius
                                heldLook:(float*)held_look_scale
                                heldMove:(float*)held_move_scale
                            supportsHeld:(BOOL*)supports_held;
- (void)setSelectedControlAnalogTuningField:(TouchAnalogTuningField)field value:(float)value;
- (void)setSelectedControlOverallAnalogSensitivity:(float)value;
- (void)setSelectedControlHeldLookScale:(float)look_scale;
- (void)setSelectedControlHeldMoveScale:(float)move_scale;
- (void)resetSelectedControlAnalogTuning;
- (void)clearSelectedControlExtras;
- (void)setSelectedControlSecondaryBehaviorTrigger:
            (xe::hid::touch::IOSTouchInteractionTrigger)trigger
                                            action:(xe::hid::touch::IOSTouchAction)action
                                      analogOutput:(xe::hid::touch::IOSTouchAnalogOutput)output;
- (void)addActionButtonWithAction:(xe::hid::touch::IOSTouchAction)action;
- (void)addJoystickWithAction:(xe::hid::touch::IOSTouchAction)action;
- (BOOL)layoutContainsJoystickAction:(xe::hid::touch::IOSTouchAction)action;
- (UIMenu*)addControlMenu;
- (BOOL)canDuplicateSelectedControl;
- (void)addNewActionButton;
- (void)presentAnalogTuningPanel;
- (void)toggleEditGrid;
- (void)updateControlBehaviorAnnotations;

@end

@interface XeniaTouchAnalogPreviewView : UIView
- (void)setTuning:(const xe::hid::touch::IOSTouchAnalogTuning&)tuning
            deadzone:(float)deadzone
    activationRadius:(float)activation_radius;
@end

@implementation XeniaTouchAnalogPreviewView {
  xe::hid::touch::IOSTouchAnalogTuning tuning_;
  float deadzone_;
  float activation_radius_;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }
  self.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySubtle]];
  self.layer.cornerRadius = XeniaRadiusLg;
  self.layer.borderWidth = 1.0;
  self.layer.borderColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySoft]].CGColor;
  self.clipsToBounds = YES;
  tuning_ = xe::hid::touch::IOSTouchAnalogTuning();
  deadzone_ = 0.12f;
  activation_radius_ = 0.72f;
  return self;
}

- (void)setTuning:(const xe::hid::touch::IOSTouchAnalogTuning&)tuning
            deadzone:(float)deadzone
    activationRadius:(float)activation_radius {
  tuning_ = tuning;
  deadzone_ = std::clamp(deadzone, 0.0f, 0.95f);
  activation_radius_ = std::clamp(activation_radius, 0.05f, 1.0f);
  [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)__unused rect {
  CGContextRef context = UIGraphicsGetCurrentContext();
  if (!context) {
    return;
  }

  CGRect bounds = CGRectInset(self.bounds, 14.0, 12.0);
  CGFloat left_width = CGRectGetWidth(bounds) * 0.48;
  CGPoint center = CGPointMake(CGRectGetMinX(bounds) + left_width * 0.5, CGRectGetMidY(bounds));
  CGFloat radius = MIN(left_width, CGRectGetHeight(bounds)) * 0.42;

  CGContextSetLineWidth(context, 1.0);
  [[[UIColor whiteColor] colorWithAlphaComponent:0.20] setStroke];
  CGContextStrokeEllipseInRect(
      context, CGRectMake(center.x - radius, center.y - radius, radius * 2.0, radius * 2.0));

  [[[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.28] setFill];
  CGFloat deadzone_radius = radius * deadzone_;
  CGContextFillEllipseInRect(context,
                             CGRectMake(center.x - deadzone_radius, center.y - deadzone_radius,
                                        deadzone_radius * 2.0, deadzone_radius * 2.0));

  [[[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.90] setStroke];
  CGContextSetLineWidth(context, 2.0);
  CGFloat active_radius = radius * activation_radius_;
  CGContextStrokeEllipseInRect(
      context, CGRectMake(center.x - active_radius, center.y - active_radius, active_radius * 2.0,
                          active_radius * 2.0));

  CGFloat sample_x = radius * 0.62 * tuning_.horizontal_scale / 2.0f;
  CGFloat sample_y = radius * 0.48 * tuning_.vertical_scale / 2.0f;
  if (tuning_.invert_x) {
    sample_x = -sample_x;
  }
  if (tuning_.invert_y) {
    sample_y = -sample_y;
  }
  const CGFloat acceleration_boost = 1.0 + std::clamp(tuning_.acceleration_scale, 0.0f, 2.0f);
  CGPoint fast_endpoint = CGPointMake(center.x + sample_x * acceleration_boost,
                                      center.y - sample_y * acceleration_boost);
  CGFloat fast_dx = fast_endpoint.x - center.x;
  CGFloat fast_dy = fast_endpoint.y - center.y;
  const CGFloat fast_distance = std::hypot(fast_dx, fast_dy);
  if (fast_distance > active_radius && fast_distance > 0.0f) {
    const CGFloat fast_scale = active_radius / fast_distance;
    fast_endpoint.x = center.x + fast_dx * fast_scale;
    fast_endpoint.y = center.y + fast_dy * fast_scale;
  }
  if (tuning_.acceleration_scale > 0.001f) {
    [[[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.56] setStroke];
    CGContextSetLineWidth(context, 4.0);
    CGContextMoveToPoint(context, center.x, center.y);
    CGContextAddLineToPoint(context, fast_endpoint.x, fast_endpoint.y);
    CGContextStrokePath(context);
  }
  const CGFloat smoothing = std::clamp(tuning_.smoothing, 0.0f, 0.95f);
  if (smoothing > 0.001f) {
    const NSInteger dot_count = 3;
    for (NSInteger dot_index = 1; dot_index <= dot_count; ++dot_index) {
      const CGFloat mix = static_cast<CGFloat>(dot_index) / (dot_count + 1.0);
      const CGFloat alpha = 0.08 + smoothing * 0.24 * mix;
      [[[UIColor whiteColor] colorWithAlphaComponent:alpha] setFill];
      const CGFloat dot_radius = 3.0 + smoothing * 5.0 * mix;
      const CGPoint dot_center = CGPointMake(center.x + sample_x * mix, center.y - sample_y * mix);
      CGContextFillEllipseInRect(context,
                                 CGRectMake(dot_center.x - dot_radius, dot_center.y - dot_radius,
                                            dot_radius * 2.0, dot_radius * 2.0));
    }
  }
  [[[XeniaTheme touchTintMint] colorWithAlphaComponent:0.92] setStroke];
  CGContextSetLineWidth(context, 2.0);
  CGContextMoveToPoint(context, center.x, center.y);
  CGContextAddLineToPoint(context, center.x + sample_x, center.y - sample_y);
  CGContextStrokePath(context);

  CGRect curve_rect =
      CGRectMake(CGRectGetMinX(bounds) + left_width + 18.0, CGRectGetMinY(bounds),
                 CGRectGetWidth(bounds) - left_width - 18.0, CGRectGetHeight(bounds));
  [[[UIColor whiteColor] colorWithAlphaComponent:0.14] setStroke];
  CGContextSetLineWidth(context, 1.0);
  CGContextStrokeRect(context, curve_rect);

  [[[XeniaTheme touchTintMint] colorWithAlphaComponent:0.92] setStroke];
  CGContextSetLineWidth(context, 2.0);
  CGContextBeginPath(context);
  for (NSInteger point_index = 0; point_index <= 32; ++point_index) {
    CGFloat t = static_cast<CGFloat>(point_index) / 32.0;
    CGFloat response = pow(t, std::clamp(tuning_.response_curve, 0.25f, 4.0f));
    response = MIN(response, tuning_.max_output);
    CGPoint p = CGPointMake(CGRectGetMinX(curve_rect) + t * CGRectGetWidth(curve_rect),
                            CGRectGetMaxY(curve_rect) - response * CGRectGetHeight(curve_rect));
    if (point_index == 0) {
      CGContextMoveToPoint(context, p.x, p.y);
    } else {
      CGContextAddLineToPoint(context, p.x, p.y);
    }
  }
  CGContextStrokePath(context);
  if (tuning_.acceleration_scale > 0.001f) {
    [[[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.72] setStroke];
    CGContextSetLineWidth(context, 1.5);
    CGContextBeginPath(context);
    const CGFloat dash_pattern[] = {4.0, 4.0};
    CGContextSetLineDash(context, 0.0, dash_pattern, 2);
    for (NSInteger point_index = 0; point_index <= 32; ++point_index) {
      CGFloat t = static_cast<CGFloat>(point_index) / 32.0;
      CGFloat response = pow(t, std::clamp(tuning_.response_curve, 0.25f, 4.0f));
      response *= acceleration_boost;
      response = MIN(response, tuning_.max_output);
      CGPoint p = CGPointMake(CGRectGetMinX(curve_rect) + t * CGRectGetWidth(curve_rect),
                              CGRectGetMaxY(curve_rect) - response * CGRectGetHeight(curve_rect));
      if (point_index == 0) {
        CGContextMoveToPoint(context, p.x, p.y);
      } else {
        CGContextAddLineToPoint(context, p.x, p.y);
      }
    }
    CGContextStrokePath(context);
    CGContextSetLineDash(context, 0.0, nullptr, 0);
  }
}

@end

@interface XeniaTouchAnalogTuningViewController : XESheetViewController
- (instancetype)initWithOverlay:(XeniaTouchControlsOverlayView*)overlay;
- (void)addSectionLabel:(NSString*)title;
- (void)addSliderRow:(NSString*)title
               field:(TouchAnalogTuningField)field
                kind:(NSInteger)kind
                 min:(float)min_value
                 max:(float)max_value;
- (void)addSwitchRow:(NSString*)title field:(TouchAnalogTuningField)field;
- (void)layoutRow:(UIView*)row;
- (NSString*)displayValueForSliderIndex:(NSUInteger)index value:(float)value;
- (void)reloadValuesFromOverlay;
- (void)resetTunePressed:(id)sender;
@end

@implementation XeniaTouchAnalogTuningViewController {
  XeniaTouchControlsOverlayView* overlay_;  // assign
  UIScrollView* scroll_view_;
  UIView* content_view_;
  XeniaTouchAnalogPreviewView* preview_view_;
  UISegmentedControl* tuning_section_control_;
  NSMutableArray<UIView*>* rows_;
  NSMutableArray<NSNumber*>* row_sections_;
  NSMutableArray<UISlider*>* sliders_;
  NSMutableArray<UILabel*>* value_labels_;
  std::vector<TouchAnalogTuningField> slider_fields_;
  std::vector<NSInteger> slider_kinds_;
  UISwitch* invert_x_switch_;
  UISwitch* invert_y_switch_;
  NSInteger building_section_;
  NSInteger active_section_;
  BOOL supports_held_;
}

- (instancetype)initWithOverlay:(XeniaTouchControlsOverlayView*)overlay {
  if (!(self = [super initWithNibName:nil bundle:nil])) {
    return nil;
  }
  overlay_ = overlay;
  rows_ = [[NSMutableArray alloc] init];
  row_sections_ = [[NSMutableArray alloc] init];
  sliders_ = [[NSMutableArray alloc] init];
  value_labels_ = [[NSMutableArray alloc] init];
  building_section_ = kTuningSectionBasic;
  active_section_ = kTuningSectionBasic;
  return self;
}

- (void)dealloc {
  [invert_y_switch_ release];
  [invert_x_switch_ release];
  [tuning_section_control_ release];
  [preview_view_ release];
  [content_view_ release];
  [scroll_view_ release];
  [rows_ release];
  [row_sections_ release];
  [sliders_ release];
  [value_labels_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"Tune";
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  self.view.backgroundColor = [UIColor clearColor];
  self.navigationItem.rightBarButtonItem =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                     target:self
                                                     action:@selector(donePressed:)] autorelease];
  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithTitle:@"Reset"
                                        style:UIBarButtonItemStylePlain
                                       target:self
                                       action:@selector(resetTunePressed:)] autorelease];

  scroll_view_ = [[UIScrollView alloc] initWithFrame:CGRectZero];
  scroll_view_.alwaysBounceVertical = YES;
  [self.view addSubview:scroll_view_];

  content_view_ = [[UIView alloc] initWithFrame:CGRectZero];
  xe_apply_floating_window_chrome(content_view_);
  [scroll_view_ addSubview:content_view_];

  preview_view_ = [[XeniaTouchAnalogPreviewView alloc] initWithFrame:CGRectZero];
  [content_view_ addSubview:preview_view_];

  tuning_section_control_ =
      [[UISegmentedControl alloc] initWithItems:@[ @"Basic", @"Feel", @"Advanced" ]];
  tuning_section_control_.selectedSegmentIndex = active_section_;
  tuning_section_control_.backgroundColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySubtle]];
  tuning_section_control_.tintColor = [XeniaTheme touchTintAmber];
  if (@available(iOS 13.0, *)) {
    tuning_section_control_.selectedSegmentTintColor = [XeniaTheme touchTintAmber];
    [tuning_section_control_ setTitleTextAttributes:@{
      NSForegroundColorAttributeName : [[UIColor whiteColor] colorWithAlphaComponent:0.86]
    }
                                           forState:UIControlStateNormal];
    [tuning_section_control_
        setTitleTextAttributes:@{NSForegroundColorAttributeName : [UIColor blackColor]}
                      forState:UIControlStateSelected];
  }
  [tuning_section_control_ addTarget:self
                              action:@selector(tuningSectionChanged:)
                    forControlEvents:UIControlEventValueChanged];
  [content_view_ addSubview:tuning_section_control_];

  [self addSectionLabel:@"Basic"];
  [self addSliderRow:@"Deadzone" field:TouchAnalogTuningField::kDeadzone kind:0 min:0.0f max:0.95f];
  [self addSliderRow:@"Radius"
               field:TouchAnalogTuningField::kActivationRadius
                kind:0
                 min:0.05f
                 max:1.0f];
  [self addSliderRow:@"Overall Sensitivity"
               field:TouchAnalogTuningField::kHorizontalScale
                kind:kTuningSliderOverallSensitivity
                 min:0.25f
                 max:4.0f];
  [self addSliderRow:@"Output Cap"
               field:TouchAnalogTuningField::kMaxOutput
                kind:kTuningSliderAnalogField
                 min:0.1f
                 max:1.0f];

  [self addSectionLabel:@"Feel"];
  [self addSliderRow:@"Horizontal Sensitivity"
               field:TouchAnalogTuningField::kHorizontalScale
                kind:kTuningSliderAnalogField
                 min:0.25f
                 max:4.0f];
  [self addSliderRow:@"Vertical Sensitivity"
               field:TouchAnalogTuningField::kVerticalScale
                kind:kTuningSliderAnalogField
                 min:0.25f
                 max:4.0f];
  [self addSliderRow:@"Response"
               field:TouchAnalogTuningField::kResponseCurve
                kind:0
                 min:0.25f
                 max:4.0f];
  [self addSliderRow:@"Swipe Acceleration"
               field:TouchAnalogTuningField::kAccelerationScale
                kind:kTuningSliderAnalogField
                 min:0.0f
                 max:2.0f];
  [self addSliderRow:@"Input Smoothing"
               field:TouchAnalogTuningField::kSmoothing
                kind:kTuningSliderAnalogField
                 min:0.0f
                 max:0.95f];

  [self addSectionLabel:@"Advanced"];
  [self addSliderRow:@"Diagonal"
               field:TouchAnalogTuningField::kDiagonalScale
                kind:0
                 min:0.25f
                 max:4.0f];
  [self addSwitchRow:@"Invert Horizontal" field:TouchAnalogTuningField::kInvertX];
  [self addSwitchRow:@"Invert Vertical" field:TouchAnalogTuningField::kInvertY];
  [self addSliderRow:@"Held Look"
               field:TouchAnalogTuningField::kHorizontalScale
                kind:kTuningSliderHeldLook
                 min:0.25f
                 max:4.0f];
  [self addSliderRow:@"Held Move"
               field:TouchAnalogTuningField::kVerticalScale
                kind:kTuningSliderHeldMove
                 min:0.25f
                 max:4.0f];
  [self reloadValuesFromOverlay];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  scroll_view_.frame = self.view.bounds;
  const CGFloat available_width = CGRectGetWidth(self.view.bounds);
  const CGFloat available_height = CGRectGetHeight(self.view.bounds);
  const BOOL landscape_layout = available_width > available_height && available_width >= 620.0;
  const CGFloat panel_width =
      MIN(landscape_layout ? 900.0 : 560.0, MAX(320.0, available_width - 28.0));
  const CGFloat panel_x = (available_width - panel_width) * 0.5;
  CGFloat content_height = 0.0;

  if (landscape_layout) {
    const CGFloat left_width = MIN(320.0, MAX(240.0, panel_width * 0.34));
    const CGFloat right_x = 16.0 + left_width + 18.0;
    const CGFloat right_width = panel_width - right_x - 16.0;
    preview_view_.frame = CGRectMake(16.0, 16.0, left_width, 168.0);
    tuning_section_control_.frame = CGRectMake(right_x, 16.0, right_width, 34.0);
    CGFloat y = CGRectGetMaxY(tuning_section_control_.frame) + 12.0;
    for (NSUInteger row_index = 0; row_index < rows_.count; ++row_index) {
      UIView* row = [rows_ objectAtIndex:row_index];
      const NSInteger section = [[row_sections_ objectAtIndex:row_index] integerValue];
      row.hidden = section != active_section_ || row.tag == 9001;
      if (row.hidden) {
        row.frame = CGRectZero;
        continue;
      }
      CGFloat row_height = row.tag == 9001 ? 30.0 : (row.tag == 9002 ? 46.0 : 58.0);
      row.frame = CGRectMake(right_x, y, right_width, row_height);
      [self layoutRow:row];
      y += row_height;
    }
    content_height = MAX(CGRectGetMaxY(preview_view_.frame), y) + 16.0;
  } else {
    CGFloat y = 16.0;
    preview_view_.frame = CGRectMake(16.0, y, panel_width - 32.0, 126.0);
    y = CGRectGetMaxY(preview_view_.frame) + 12.0;
    tuning_section_control_.frame = CGRectMake(16.0, y, panel_width - 32.0, 34.0);
    y = CGRectGetMaxY(tuning_section_control_.frame) + 10.0;
    for (NSUInteger row_index = 0; row_index < rows_.count; ++row_index) {
      UIView* row = [rows_ objectAtIndex:row_index];
      const NSInteger section = [[row_sections_ objectAtIndex:row_index] integerValue];
      row.hidden = section != active_section_;
      if (row.hidden) {
        row.frame = CGRectZero;
        continue;
      }
      CGFloat row_height = row.tag == 9001 ? 30.0 : (row.tag == 9002 ? 46.0 : 58.0);
      row.frame = CGRectMake(0.0, y, panel_width, row_height);
      [self layoutRow:row];
      y += row_height;
    }
    content_height = y + 12.0;
  }

  content_view_.frame = CGRectMake(panel_x, 12.0, panel_width, content_height);
  scroll_view_.contentSize = CGSizeMake(available_width, content_height + 24.0);
}

- (void)addSectionLabel:(NSString*)title {
  if ([title isEqualToString:@"Basic"]) {
    building_section_ = kTuningSectionBasic;
  } else if ([title isEqualToString:@"Feel"]) {
    building_section_ = kTuningSectionFeel;
  } else {
    building_section_ = kTuningSectionAdvanced;
  }
  UILabel* label = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
  label.tag = 9001;
  label.text = title;
  label.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.60];
  xe_apply_label_font(label, UIFontTextStyleCaption1, 12.0, UIFontWeightSemibold);
  [content_view_ addSubview:label];
  [rows_ addObject:label];
  [row_sections_ addObject:@(building_section_)];
}

- (void)addSliderRow:(NSString*)title
               field:(TouchAnalogTuningField)field
                kind:(NSInteger)kind
                 min:(float)min_value
                 max:(float)max_value {
  UIView* row = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  row.tag = 9000;

  UILabel* title_label = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
  title_label.text = title;
  title_label.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.92];
  xe_apply_label_font(title_label, UIFontTextStyleSubheadline, 14.0, UIFontWeightSemibold);
  [row addSubview:title_label];

  UILabel* value_label = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
  value_label.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.66];
  value_label.textAlignment = NSTextAlignmentRight;
  xe_apply_monospaced_label_font(value_label, UIFontTextStyleCaption1, 12.0, UIFontWeightMedium);
  [row addSubview:value_label];

  UISlider* slider = [[[UISlider alloc] initWithFrame:CGRectZero] autorelease];
  slider.minimumValue = min_value;
  slider.maximumValue = max_value;
  slider.continuous = YES;
  slider.minimumTrackTintColor = [XeniaTheme touchTintAmber];
  slider.maximumTrackTintColor = [[UIColor whiteColor] colorWithAlphaComponent:0.18];
  [slider addTarget:self
                action:@selector(sliderChanged:)
      forControlEvents:UIControlEventValueChanged];
  [row addSubview:slider];

  [content_view_ addSubview:row];
  [rows_ addObject:row];
  [row_sections_ addObject:@(building_section_)];
  [sliders_ addObject:slider];
  [value_labels_ addObject:value_label];
  slider_fields_.push_back(field);
  slider_kinds_.push_back(kind);
}

- (void)addSwitchRow:(NSString*)title field:(TouchAnalogTuningField)field {
  UIView* row = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  row.tag = 9002;
  UILabel* title_label = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
  title_label.text = title;
  title_label.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.92];
  xe_apply_label_font(title_label, UIFontTextStyleSubheadline, 14.0, UIFontWeightSemibold);
  [row addSubview:title_label];

  UISwitch* toggle = [[[UISwitch alloc] initWithFrame:CGRectZero] autorelease];
  toggle.onTintColor = [XeniaTheme touchTintAmber];
  toggle.tag = field == TouchAnalogTuningField::kInvertX ? 9101 : 9102;
  [toggle addTarget:self
                action:@selector(switchChanged:)
      forControlEvents:UIControlEventValueChanged];
  [row addSubview:toggle];
  if (field == TouchAnalogTuningField::kInvertX) {
    invert_x_switch_ = [toggle retain];
  } else {
    invert_y_switch_ = [toggle retain];
  }
  [content_view_ addSubview:row];
  [rows_ addObject:row];
  [row_sections_ addObject:@(building_section_)];
}

- (void)layoutRow:(UIView*)row {
  if ([row isKindOfClass:[UILabel class]]) {
    row.frame = CGRectMake(CGRectGetMinX(row.frame) + 16.0, CGRectGetMinY(row.frame) + 8.0,
                           CGRectGetWidth(row.frame) - 32.0, 18.0);
    return;
  }
  UILabel* title_label = nil;
  UILabel* value_label = nil;
  UISlider* slider = nil;
  UISwitch* toggle = nil;
  for (UIView* subview in row.subviews) {
    if ([subview isKindOfClass:[UISlider class]]) {
      slider = (UISlider*)subview;
    } else if ([subview isKindOfClass:[UISwitch class]]) {
      toggle = (UISwitch*)subview;
    } else if ([subview isKindOfClass:[UILabel class]]) {
      if (!title_label) {
        title_label = (UILabel*)subview;
      } else {
        value_label = (UILabel*)subview;
      }
    }
  }
  const CGFloat width = CGRectGetWidth(row.bounds);
  title_label.frame = CGRectMake(16.0, 4.0, width - 132.0, 22.0);
  if (toggle) {
    toggle.frame = CGRectMake(width - 16.0 - CGRectGetWidth(toggle.bounds),
                              (CGRectGetHeight(row.bounds) - CGRectGetHeight(toggle.bounds)) * 0.5,
                              CGRectGetWidth(toggle.bounds), CGRectGetHeight(toggle.bounds));
    return;
  }
  value_label.frame = CGRectMake(width - 112.0, 5.0, 96.0, 20.0);
  slider.frame = CGRectMake(16.0, 30.0, width - 32.0, 28.0);
}

- (NSString*)displayValueForSliderIndex:(NSUInteger)index value:(float)value {
  const NSInteger kind = index < slider_kinds_.size() ? slider_kinds_[index] : 0;
  if (kind == kTuningSliderOverallSensitivity) {
    return [NSString stringWithFormat:@"%.0f%%", value * 100.0f];
  }
  if (kind == kTuningSliderHeldLook || kind == kTuningSliderHeldMove) {
    return [NSString stringWithFormat:@"%.2fx", value];
  }
  const TouchAnalogTuningField field = index < slider_fields_.size()
                                           ? slider_fields_[index]
                                           : TouchAnalogTuningField::kHorizontalScale;
  switch (field) {
    case TouchAnalogTuningField::kDeadzone:
    case TouchAnalogTuningField::kActivationRadius:
    case TouchAnalogTuningField::kAccelerationScale:
    case TouchAnalogTuningField::kSmoothing:
    case TouchAnalogTuningField::kMaxOutput:
      return [NSString stringWithFormat:@"%.0f%%", value * 100.0f];
    default:
      return [NSString stringWithFormat:@"%.2fx", value];
  }
}

- (void)reloadValuesFromOverlay {
  xe::hid::touch::IOSTouchAnalogTuning tuning;
  float deadzone = 0.0f;
  float activation_radius = 1.0f;
  float held_look = 1.0f;
  float held_move = 1.0f;
  BOOL supports_held = NO;
  if (![overlay_ copySelectedAnalogTuningForPanel:&tuning
                                         deadzone:&deadzone
                                 activationRadius:&activation_radius
                                         heldLook:&held_look
                                         heldMove:&held_move
                                     supportsHeld:&supports_held]) {
    return;
  }
  supports_held_ = supports_held;
  [preview_view_ setTuning:tuning deadzone:deadzone activationRadius:activation_radius];
  for (NSUInteger index = 0; index < sliders_.count; ++index) {
    UISlider* slider = [sliders_ objectAtIndex:index];
    float value = tuning.horizontal_scale;
    const NSInteger kind = index < slider_kinds_.size() ? slider_kinds_[index] : 0;
    if (kind == kTuningSliderHeldLook) {
      value = held_look;
      slider.enabled = supports_held_;
    } else if (kind == kTuningSliderHeldMove) {
      value = held_move;
      slider.enabled = supports_held_;
    } else if (kind == kTuningSliderOverallSensitivity) {
      value = (tuning.horizontal_scale + tuning.vertical_scale) * 0.5f;
      slider.enabled = YES;
    } else {
      switch (slider_fields_[index]) {
        case TouchAnalogTuningField::kDeadzone:
          value = deadzone;
          break;
        case TouchAnalogTuningField::kActivationRadius:
          value = activation_radius;
          break;
        case TouchAnalogTuningField::kHorizontalScale:
          value = tuning.horizontal_scale;
          break;
        case TouchAnalogTuningField::kVerticalScale:
          value = tuning.vertical_scale;
          break;
        case TouchAnalogTuningField::kDiagonalScale:
          value = tuning.diagonal_scale;
          break;
        case TouchAnalogTuningField::kResponseCurve:
          value = tuning.response_curve;
          break;
        case TouchAnalogTuningField::kAccelerationScale:
          value = tuning.acceleration_scale;
          break;
        case TouchAnalogTuningField::kSmoothing:
          value = tuning.smoothing;
          break;
        case TouchAnalogTuningField::kMaxOutput:
          value = tuning.max_output;
          break;
        default:
          break;
      }
    }
    slider.value = value;
    UILabel* value_label = [value_labels_ objectAtIndex:index];
    value_label.text = [self displayValueForSliderIndex:index value:value];
    slider.alpha = slider.enabled ? 1.0 : 0.42;
    value_label.alpha = slider.enabled ? 1.0 : 0.42;
  }
  invert_x_switch_.on = tuning.invert_x;
  invert_y_switch_.on = tuning.invert_y;
  tuning_section_control_.selectedSegmentIndex = active_section_;
}

- (void)tuningSectionChanged:(UISegmentedControl*)sender {
  active_section_ = MAX(sender.selectedSegmentIndex, 0);
  [self.view setNeedsLayout];
}

- (void)sliderChanged:(UISlider*)slider {
  NSUInteger index = [sliders_ indexOfObject:slider];
  if (index == NSNotFound || index >= slider_kinds_.size()) {
    return;
  }
  UILabel* value_label = [value_labels_ objectAtIndex:index];
  value_label.text = [self displayValueForSliderIndex:index value:slider.value];
  const NSInteger kind = slider_kinds_[index];
  if (kind == kTuningSliderHeldLook) {
    [overlay_ setSelectedControlHeldLookScale:slider.value];
  } else if (kind == kTuningSliderHeldMove) {
    [overlay_ setSelectedControlHeldMoveScale:slider.value];
  } else if (kind == kTuningSliderOverallSensitivity) {
    [overlay_ setSelectedControlOverallAnalogSensitivity:slider.value];
  } else {
    [overlay_ setSelectedControlAnalogTuningField:slider_fields_[index] value:slider.value];
  }
  [self reloadValuesFromOverlay];
}

- (void)switchChanged:(UISwitch*)toggle {
  TouchAnalogTuningField field =
      toggle.tag == 9101 ? TouchAnalogTuningField::kInvertX : TouchAnalogTuningField::kInvertY;
  [overlay_ setSelectedControlAnalogTuningField:field value:(toggle.on ? 1.0f : 0.0f)];
  [self reloadValuesFromOverlay];
}

- (void)donePressed:(id)__unused sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (void)resetTunePressed:(id)__unused sender {
  [overlay_ resetSelectedControlAnalogTuning];
  [self reloadValuesFromOverlay];
}

@end

@implementation XeniaTouchControlsOverlayView {
  xe::hid::touch::IOSTouchRuntimeModel* runtime_model_;
  NSMutableArray<XeniaTouchControlShellView*>* control_views_;
  UIButton* pause_button_;
  UIView* edit_grid_overlay_;
  CAShapeLayer* edit_grid_dots_layer_;
  UIView* edit_snap_guides_overlay_;
  CAShapeLayer* edit_snap_guides_layer_;
  XeniaPaddedLabel* edit_snap_feedback_label_;
  CGPoint edit_snap_feedback_anchor_;
  UIView* edit_safe_area_guide_;
  XeniaTouchOverlayEditChromeIOS* edit_chrome_;
  XeniaTouchEditCommandBar* edit_command_bar_;
  UIView* edit_resize_handle_;
  UIView* move_knob_;
  CADisplayLink* display_link_;
  std::vector<xe::hid::touch::IOSTouchRect> resolved_control_frames_;
  std::vector<bool> conflicting_control_indices_;
  std::vector<uint8_t> visually_active_control_indices_;
  std::vector<CFTimeInterval> recent_action_press_times_;
  std::vector<CFTimeInterval> recent_action_suppressed_until_times_;
  std::vector<CFTimeInterval> recent_secondary_press_times_;
  std::vector<CFTimeInterval> recent_secondary_candidate_times_;
  std::vector<CGPoint> recent_look_vectors_;
  std::vector<CFTimeInterval> recent_look_motion_times_;
  std::vector<CGPoint> recent_move_vectors_;
  std::vector<CFTimeInterval> recent_move_motion_times_;
  std::vector<TouchCaptureState> active_captures_;
  std::vector<CGFloat> active_snap_vertical_guides_;
  std::vector<CGFloat> active_snap_horizontal_guides_;
  xe::hid::touch::IOSTouchResolvedState last_published_state_;
  uint32_t next_packet_number_;
  BOOL gameplay_overlay_active_;
  BOOL editing_controls_enabled_;
  BOOL edit_showing_layout_library_;
  BOOL edit_wiring_visible_;
  NSUInteger selected_control_index_;
  NSUInteger move_control_index_;
  NSUInteger look_control_index_;
  NSUInteger pause_control_index_;
  BOOL edit_grid_enabled_;
  TouchOverlayEditHistoryIOS edit_history_;
  BOOL edit_chrome_drag_active_;
  BOOL edit_opacity_slider_active_;
  UITouch* edit_chrome_drag_touch_;
  CGRect edit_chrome_drag_frame_;
  CGPoint edit_chrome_drag_touch_offset_;
  CGRect edit_chrome_free_frame_;
  BOOL edit_chrome_has_free_frame_;
  BOOL edit_match_size_picker_active_;
  BOOL edit_pinch_active_;
  NSUInteger edit_pinch_control_index_;
  UITouch* edit_pinch_touch_a_;
  UITouch* edit_pinch_touch_b_;
  CGFloat edit_pinch_initial_distance_;
  xe::hid::touch::IOSTouchRect edit_pinch_initial_frame_;
  UIImpactFeedbackGenerator* haptic_press_;        // medium impact on action press
  UIImpactFeedbackGenerator* haptic_press_light_;  // light impact on stick engage
  UIImpactFeedbackGenerator* haptic_snap_;         // rigid impact on snap engage
  UISelectionFeedbackGenerator* haptic_selection_;
  BOOL snap_guides_were_visible_;
  // Tracks the orientation we last laid out in. layoutSubviews compares this
  // against the current bounds-derived orientation so it can refresh the
  // edit chrome chip when the device rotates without forcing a refresh on
  // every layout pass.
  BOOL last_layout_was_portrait_;
  BOOL last_layout_orientation_known_;
  UIView* tooltip_view_;
  UILabel* tooltip_label_;
  UILongPressGestureRecognizer* tooltip_long_press_;
}

@synthesize pauseHandler = pauseHandler_;
@synthesize doneEditingHandler = doneEditingHandler_;
@synthesize layoutLibraryHandler = layoutLibraryHandler_;
@synthesize layoutLibraryLoadHandler = layoutLibraryLoadHandler_;
@synthesize layoutLibrarySaveCopyHandler = layoutLibrarySaveCopyHandler_;
@synthesize layoutLibraryRenameHandler = layoutLibraryRenameHandler_;
@synthesize layoutLibraryDeleteHandler = layoutLibraryDeleteHandler_;
@synthesize layoutLibraryImportHandler = layoutLibraryImportHandler_;
@synthesize layoutLibraryExportHandler = layoutLibraryExportHandler_;
@synthesize layoutLibraryResetHandler = layoutLibraryResetHandler_;
@synthesize layoutLibraryRenameLayoutHandler = layoutLibraryRenameLayoutHandler_;
@synthesize layoutLibraryDeleteLayoutHandler = layoutLibraryDeleteLayoutHandler_;
@synthesize layoutLibraryExportLayoutHandler = layoutLibraryExportLayoutHandler_;
@synthesize layoutLibrarySetTitleDefaultHandler = layoutLibrarySetTitleDefaultHandler_;
@synthesize layoutLibrarySetGlobalDefaultHandler = layoutLibrarySetGlobalDefaultHandler_;
@synthesize layoutLibraryFavoriteHandler = layoutLibraryFavoriteHandler_;

- (void)createDisplayLinkIfNeeded {
  if (display_link_) {
    return;
  }
  display_link_ = [[CADisplayLink displayLinkWithTarget:self
                                               selector:@selector(displayLinkFired:)] retain];
  display_link_.paused = YES;
  [display_link_ addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (instancetype)initWithRuntimeModel:(xe::hid::touch::IOSTouchRuntimeModel*)runtime_model {
  if (!(self = [super initWithFrame:CGRectZero])) {
    return nil;
  }

  // The touch overlay sits on top of running gameplay; force dark so pause
  // glyphs, edit chrome, tooltips, and snap guides stay readable.
  self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;

  runtime_model_ = runtime_model;
  control_views_ = [[NSMutableArray alloc] init];
  move_control_index_ = NSNotFound;
  look_control_index_ = NSNotFound;
  pause_control_index_ = NSNotFound;
  next_packet_number_ = 1;
  gameplay_overlay_active_ = NO;
  editing_controls_enabled_ = NO;
  edit_showing_layout_library_ = NO;
  selected_control_index_ = NSNotFound;
  edit_chrome_drag_active_ = NO;
  edit_opacity_slider_active_ = NO;
  edit_chrome_drag_touch_ = nil;
  edit_chrome_drag_frame_ = CGRectZero;
  edit_chrome_drag_touch_offset_ = CGPointZero;
  edit_chrome_free_frame_ = CGRectZero;
  edit_chrome_has_free_frame_ = NO;
  edit_match_size_picker_active_ = NO;
  last_layout_was_portrait_ = NO;
  last_layout_orientation_known_ = NO;

  pause_button_ = [[UIButton buttonWithType:UIButtonTypeCustom] retain];
  pause_button_.backgroundColor = [UIColor clearColor];
  pause_button_.hidden = YES;
  pause_button_.accessibilityLabel = @"Pause game";
  pause_button_.accessibilityTraits = UIAccessibilityTraitButton;
  [pause_button_ addTarget:self
                    action:@selector(pauseButtonPressed:)
          forControlEvents:UIControlEventTouchUpInside];
  [self addSubview:pause_button_];

  edit_grid_overlay_ = [[UIView alloc] initWithFrame:CGRectZero];
  edit_grid_overlay_.hidden = YES;
  edit_grid_overlay_.backgroundColor = [UIColor clearColor];
  edit_grid_overlay_.userInteractionEnabled = NO;
  edit_grid_dots_layer_ = [[CAShapeLayer alloc] init];
  edit_grid_dots_layer_.fillColor = [[UIColor whiteColor] colorWithAlphaComponent:0.16].CGColor;
  edit_grid_dots_layer_.strokeColor = nil;
  [edit_grid_overlay_.layer addSublayer:edit_grid_dots_layer_];
  [self addSubview:edit_grid_overlay_];

  edit_snap_guides_overlay_ = [[UIView alloc] initWithFrame:CGRectZero];
  edit_snap_guides_overlay_.hidden = YES;
  edit_snap_guides_overlay_.backgroundColor = [UIColor clearColor];
  edit_snap_guides_overlay_.userInteractionEnabled = NO;
  edit_snap_guides_layer_ = [[CAShapeLayer alloc] init];
  edit_snap_guides_layer_.hidden = YES;
  edit_snap_guides_layer_.fillColor = nil;
  edit_snap_guides_layer_.strokeColor =
      [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.96].CGColor;
  edit_snap_guides_layer_.lineWidth = kEditSnapGuideLineWidth;
  edit_snap_guides_layer_.lineDashPattern = @[ @6.0f, @5.0f ];
  edit_snap_guides_layer_.lineCap = kCALineCapRound;
  [edit_snap_guides_overlay_.layer addSublayer:edit_snap_guides_layer_];
  edit_snap_feedback_label_ = [[XeniaPaddedLabel alloc] initWithFrame:CGRectZero];
  edit_snap_feedback_label_.hidden = YES;
  edit_snap_feedback_label_.alpha = 0.0;
  edit_snap_feedback_label_.padding = UIEdgeInsetsMake(4.0, 9.0, 4.0, 9.0);
  edit_snap_feedback_label_.backgroundColor =
      [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.95];
  edit_snap_feedback_label_.textColor = [UIColor blackColor];
  edit_snap_feedback_label_.layer.cornerRadius = 9.0;
  edit_snap_feedback_label_.clipsToBounds = YES;
  xe_apply_label_font(edit_snap_feedback_label_, UIFontTextStyleCaption1, 12.0,
                      UIFontWeightSemibold);
  [edit_snap_guides_overlay_ addSubview:edit_snap_feedback_label_];
  [self addSubview:edit_snap_guides_overlay_];

  edit_safe_area_guide_ = [[UIView alloc] initWithFrame:CGRectZero];
  edit_safe_area_guide_.hidden = YES;
  edit_safe_area_guide_.backgroundColor = [UIColor clearColor];
  edit_safe_area_guide_.layer.borderWidth = 1.0;
  edit_safe_area_guide_.layer.borderColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacityStrong]].CGColor;
  // Matches xe_apply_floating_window_chrome's 20pt and visually pairs the safe
  // area indicator with the floating editor chrome it sits behind.
  edit_safe_area_guide_.layer.cornerRadius = 20.0;
  edit_safe_area_guide_.userInteractionEnabled = NO;
  [self addSubview:edit_safe_area_guide_];

  edit_chrome_ = [[XeniaTouchOverlayEditChromeIOS alloc] initWithFrame:CGRectZero];
  edit_chrome_.delegate = self;
  [self addSubview:edit_chrome_];

  edit_command_bar_ = [[XeniaTouchEditCommandBar alloc] initWithFrame:CGRectZero];
  edit_command_bar_.delegate = self;
  edit_command_bar_.hidden = YES;
  [self addSubview:edit_command_bar_];

  edit_resize_handle_ = [[UIView alloc] initWithFrame:CGRectZero];
  edit_resize_handle_.hidden = YES;
  edit_resize_handle_.backgroundColor = [[XeniaTheme touchTintAmber] colorWithAlphaComponent:0.92];
  // Resize handle is a 28pt-diameter visual element; 14pt = 28/2 keeps the
  // circle a true circle even when the handle is offset off-corner.
  edit_resize_handle_.layer.cornerRadius = 14.0;
  edit_resize_handle_.layer.borderWidth = 1.5;
  edit_resize_handle_.layer.borderColor =
      [[UIColor blackColor] colorWithAlphaComponent:0.35].CGColor;
  [self addSubview:edit_resize_handle_];

  move_knob_ = [[UIView alloc] initWithFrame:CGRectZero];
  move_knob_.hidden = YES;
  move_knob_.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:0.18];
  move_knob_.layer.borderWidth = 1.0;
  move_knob_.layer.borderColor = [[UIColor whiteColor] colorWithAlphaComponent:0.72].CGColor;
  [self addSubview:move_knob_];

  [self createDisplayLinkIfNeeded];

  self.backgroundColor = [UIColor clearColor];
  self.opaque = NO;
  self.alpha = 0.0;
  self.hidden = YES;
  self.userInteractionEnabled = YES;
  self.multipleTouchEnabled = YES;
  edit_grid_enabled_ = YES;

  // Pre-construct haptic generators so the first press does not pay the
  // generator-init latency. The system frees them when nothing has used them
  // recently; -prepare on each fire keeps them warm during gameplay.
  haptic_press_ = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
  haptic_press_light_ =
      [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
  haptic_snap_ = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleRigid];
  haptic_selection_ = [[UISelectionFeedbackGenerator alloc] init];

  // Long-press tooltip: when the user holds a finger on a control in edit
  // mode for ~0.4s, show a bubble describing what the control is bound to.
  // Helps with discoverability without forcing the user into the binding menu.
  // Use the unified floating-window chrome so all of the iOS chrome shares the
  // same look.
  tooltip_view_ = [[UIView alloc] initWithFrame:CGRectZero];
  xe_apply_floating_window_chrome(tooltip_view_);
  tooltip_view_.alpha = 0.0f;
  tooltip_view_.userInteractionEnabled = NO;
  tooltip_view_.hidden = YES;
  [self addSubview:tooltip_view_];

  tooltip_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  tooltip_label_.numberOfLines = 0;
  tooltip_label_.textColor = [UIColor whiteColor];
  xe_apply_label_font(tooltip_label_, UIFontTextStyleCaption1, 12.0, UIFontWeightMedium);
  tooltip_label_.textAlignment = NSTextAlignmentLeft;
  tooltip_label_.backgroundColor = [UIColor clearColor];
  [tooltip_view_ addSubview:tooltip_label_];

  tooltip_long_press_ =
      [[UILongPressGestureRecognizer alloc] initWithTarget:self
                                                    action:@selector(tooltipLongPressTriggered:)];
  tooltip_long_press_.minimumPressDuration = 0.40;
  tooltip_long_press_.cancelsTouchesInView = NO;
  tooltip_long_press_.delaysTouchesBegan = NO;
  tooltip_long_press_.delaysTouchesEnded = NO;
  [self addGestureRecognizer:tooltip_long_press_];

  [self refreshLayoutModel];
  return self;
}

- (void)playPressHaptic {
  if (!cvars::ios_touch_haptics) {
    return;
  }
  [haptic_press_ impactOccurred];
  [haptic_press_ prepare];
}

- (void)playLightPressHaptic {
  if (!cvars::ios_touch_haptics) {
    return;
  }
  [haptic_press_light_ impactOccurred];
  [haptic_press_light_ prepare];
}

- (void)playSnapHaptic {
  if (!cvars::ios_touch_haptics) {
    return;
  }
  [haptic_snap_ impactOccurred];
  [haptic_snap_ prepare];
}

- (void)playSelectionHaptic {
  if (!cvars::ios_touch_haptics) {
    return;
  }
  [haptic_selection_ selectionChanged];
  [haptic_selection_ prepare];
}

#pragma mark - Hardware keyboard shortcuts (edit mode)

- (BOOL)canBecomeFirstResponder {
  return editing_controls_enabled_;
}

- (NSArray<UIKeyCommand*>*)keyCommands {
  if (!editing_controls_enabled_) {
    return @[];
  }
  UIKeyCommand* undo = [UIKeyCommand keyCommandWithInput:@"z"
                                           modifierFlags:UIKeyModifierCommand
                                                  action:@selector(keyCommandUndo:)];
  undo.discoverabilityTitle = @"Undo edit";
  UIKeyCommand* redo = [UIKeyCommand keyCommandWithInput:@"z"
                                           modifierFlags:UIKeyModifierCommand | UIKeyModifierShift
                                                  action:@selector(keyCommandRedo:)];
  redo.discoverabilityTitle = @"Redo edit";
  UIKeyCommand* duplicate = [UIKeyCommand keyCommandWithInput:@"d"
                                                modifierFlags:UIKeyModifierCommand
                                                       action:@selector(keyCommandDuplicate:)];
  duplicate.discoverabilityTitle = @"Duplicate selected control";
  UIKeyCommand* mirror = [UIKeyCommand keyCommandWithInput:@"m"
                                             modifierFlags:UIKeyModifierCommand
                                                    action:@selector(keyCommandMirror:)];
  mirror.discoverabilityTitle = @"Mirror selected control horizontally";
  UIKeyCommand* delete_command = [UIKeyCommand keyCommandWithInput:@"\b"
                                                     modifierFlags:UIKeyModifierCommand
                                                            action:@selector(keyCommandDelete:)];
  delete_command.discoverabilityTitle = @"Delete selected control";
  UIKeyCommand* done = [UIKeyCommand keyCommandWithInput:UIKeyInputEscape
                                           modifierFlags:0
                                                  action:@selector(keyCommandDone:)];
  done.discoverabilityTitle = @"Exit edit mode";
  return @[ undo, redo, duplicate, mirror, delete_command, done ];
}

- (void)keyCommandUndo:(UIKeyCommand*)__unused command {
  [self undoEditLayoutChange];
}

- (void)keyCommandRedo:(UIKeyCommand*)__unused command {
  [self redoEditLayoutChange];
}

- (void)keyCommandDuplicate:(UIKeyCommand*)__unused command {
  [self duplicateSelectedControl];
}

- (void)keyCommandMirror:(UIKeyCommand*)__unused command {
  [self mirrorSelectedControlHorizontally];
}

- (void)keyCommandDelete:(UIKeyCommand*)__unused command {
  [self deleteSelectedControl];
}

- (void)keyCommandDone:(UIKeyCommand*)__unused command {
  if (doneEditingHandler_) {
    doneEditingHandler_();
  }
}

#pragma mark - Long-press tooltip

- (void)hideTooltip {
  if (tooltip_view_.hidden) {
    return;
  }
  [UIView animateWithDuration:0.10
      animations:^{
        tooltip_view_.alpha = 0.0;
      }
      completion:^(__unused BOOL finished) {
        tooltip_view_.hidden = YES;
      }];
}

- (void)showTooltipForControlAtIndex:(NSUInteger)control_index nearPoint:(CGPoint)point {
  if (!runtime_model_) {
    return;
  }
  const auto& controls = runtime_model_->layout().controls;
  if (control_index >= controls.size()) {
    return;
  }
  const xe::hid::touch::IOSTouchControlDefinition& control = controls[control_index];

  NSMutableString* text = [NSMutableString string];
  NSString* label = XeniaTouchVisibleControlLabelText(control, NO);
  if (label.length) {
    [text appendFormat:@"%@\n", label];
  }
  [text appendFormat:@"Action: %s", xe::hid::touch::IOSTouchActionDisplayName(control.action)];
  if (control.secondary_behavior.trigger != xe::hid::touch::IOSTouchInteractionTrigger::kNone &&
      control.secondary_behavior.action != xe::hid::touch::IOSTouchAction::kNone) {
    [text
        appendFormat:@"\n2nd: %s, %s",
                     xe::hid::touch::IOSTouchInteractionTriggerDisplayName(
                         control.secondary_behavior.trigger),
                     xe::hid::touch::IOSTouchActionDisplayName(control.secondary_behavior.action)];
  }
  [text
      appendFormat:@"\nTint: %s", xe::hid::touch::IOSTouchTintStyleDisplayName(control.tint_style)];
  const bool tooltip_is_portrait = TouchOverlayIsPortraitForView(self);
  const xe::hid::touch::IOSTouchRect& tooltip_frame =
      xe::hid::touch::ActiveControlFrameForOrientation(control, tooltip_is_portrait);
  [text appendFormat:@"\nFrame: %.2f, %.2f, %.2f x %.2f", tooltip_frame.x, tooltip_frame.y,
                     tooltip_frame.width, tooltip_frame.height];

  tooltip_label_.text = text;
  const CGSize max_size = CGSizeMake(220.0f, 1000.0f);
  CGSize content_size = [tooltip_label_ sizeThatFits:max_size];
  const CGFloat tooltip_padding_x = 12.0f;
  const CGFloat tooltip_padding_y = 10.0f;
  const CGFloat tooltip_width = ceilf(content_size.width) + tooltip_padding_x * 2.0f;
  const CGFloat tooltip_height = ceilf(content_size.height) + tooltip_padding_y * 2.0f;

  // Position above the press point if there's room; otherwise below.
  CGFloat origin_x = std::clamp<CGFloat>(point.x - tooltip_width * 0.5, 8.0,
                                         CGRectGetWidth(self.bounds) - tooltip_width - 8.0);
  CGFloat origin_y = point.y - tooltip_height - 18.0;
  if (origin_y < self.safeAreaInsets.top + 8.0) {
    origin_y = point.y + 18.0;
  }
  origin_y = std::clamp<CGFloat>(
      origin_y, self.safeAreaInsets.top + 8.0,
      CGRectGetHeight(self.bounds) - tooltip_height - self.safeAreaInsets.bottom - 8.0);

  tooltip_view_.frame = CGRectMake(origin_x, origin_y, tooltip_width, tooltip_height);
  tooltip_label_.frame =
      CGRectMake(tooltip_padding_x, tooltip_padding_y, content_size.width, content_size.height);

  [self bringSubviewToFront:tooltip_view_];
  if (tooltip_view_.hidden) {
    tooltip_view_.alpha = 0.0;
    tooltip_view_.hidden = NO;
  }
  [UIView animateWithDuration:0.12
                   animations:^{
                     tooltip_view_.alpha = 1.0;
                   }];
}

- (void)tooltipLongPressTriggered:(UILongPressGestureRecognizer*)recognizer {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }
  if (recognizer.state == UIGestureRecognizerStateBegan) {
    const CGPoint point = [recognizer locationInView:self];
    const auto& controls = runtime_model_->layout().controls;
    const NSUInteger control_count =
        MIN(control_views_.count, static_cast<NSUInteger>(controls.size()));
    NSInteger best_index = -1;
    uint8_t best_priority = 0;
    for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
      if (control_index >= resolved_control_frames_.size()) {
        continue;
      }
      const xe::hid::touch::IOSTouchControlDefinition& control = controls[control_index];
      if (!TouchControlContainsPoint(control, resolved_control_frames_[control_index], point)) {
        continue;
      }
      if (best_index < 0 || control.capture_priority > best_priority) {
        best_index = static_cast<NSInteger>(control_index);
        best_priority = control.capture_priority;
      }
    }
    if (best_index >= 0) {
      [self showTooltipForControlAtIndex:static_cast<NSUInteger>(best_index) nearPoint:point];
    }
  } else if (recognizer.state == UIGestureRecognizerStateEnded ||
             recognizer.state == UIGestureRecognizerStateCancelled ||
             recognizer.state == UIGestureRecognizerStateFailed) {
    [self hideTooltip];
  }
}

- (void)willMoveToWindow:(UIWindow*)new_window {
  if (!new_window && display_link_) {
    [display_link_ invalidate];
    [display_link_ release];
    display_link_ = nil;
  } else if (new_window && !display_link_) {
    [self createDisplayLinkIfNeeded];
  }
  [super willMoveToWindow:new_window];
}

- (void)dealloc {
  [display_link_ invalidate];
  [display_link_ release];
  [haptic_press_ release];
  [haptic_press_light_ release];
  [haptic_snap_ release];
  [haptic_selection_ release];
  [tooltip_long_press_ release];
  [tooltip_label_ release];
  [tooltip_view_ release];
  [layoutLibraryFavoriteHandler_ release];
  [layoutLibrarySetGlobalDefaultHandler_ release];
  [layoutLibrarySetTitleDefaultHandler_ release];
  [layoutLibraryExportLayoutHandler_ release];
  [layoutLibraryDeleteLayoutHandler_ release];
  [layoutLibraryRenameLayoutHandler_ release];
  [layoutLibraryResetHandler_ release];
  [layoutLibraryExportHandler_ release];
  [layoutLibraryImportHandler_ release];
  [layoutLibraryDeleteHandler_ release];
  [layoutLibraryRenameHandler_ release];
  [layoutLibrarySaveCopyHandler_ release];
  [layoutLibraryLoadHandler_ release];
  [layoutLibraryHandler_ release];
  [doneEditingHandler_ release];
  [pauseHandler_ release];
  [pause_button_ release];
  [edit_snap_guides_layer_ release];
  [edit_snap_feedback_label_ release];
  [edit_snap_guides_overlay_ release];
  [edit_grid_dots_layer_ release];
  [edit_grid_overlay_ release];
  [edit_resize_handle_ release];
  edit_command_bar_.delegate = nil;
  [edit_command_bar_ release];
  edit_chrome_.delegate = nil;
  [edit_chrome_ release];
  [edit_safe_area_guide_ release];
  [move_knob_ release];
  [control_views_ release];
  [super dealloc];
}

- (BOOL)isShowingLayoutLibrary {
  return edit_showing_layout_library_;
}

- (void)showLayoutLibraryWithItems:(NSArray<XeniaTouchLayoutLibraryItem*>*)items
              currentLayoutLocalID:(NSString*)currentLayoutLocalID {
  [edit_chrome_ setLayoutLibraryItems:items currentLayoutLocalID:currentLayoutLocalID];
  [self cancelMatchSizePicker];
  edit_showing_layout_library_ = YES;
  edit_command_bar_.hidden = YES;
  edit_command_bar_.alpha = 0.0;
  edit_command_bar_.userInteractionEnabled = NO;
  [self refreshEditChromeSelection];
  [self bringSubviewToFront:edit_chrome_];
  [self setNeedsLayout];
}

- (void)hideLayoutLibrary {
  [self cancelMatchSizePicker];
  edit_showing_layout_library_ = NO;
  edit_command_bar_.hidden = !editing_controls_enabled_;
  edit_command_bar_.alpha = editing_controls_enabled_ ? 1.0 : 0.0;
  edit_command_bar_.userInteractionEnabled = editing_controls_enabled_;
  [self refreshEditChromeSelection];
  [self bringSubviewToFront:edit_command_bar_];
  [self setNeedsLayout];
}

- (std::string)selectedControlIdentifier {
  if (!runtime_model_ || selected_control_index_ == NSNotFound) {
    return std::string();
  }
  const auto& controls = runtime_model_->layout().controls;
  if (selected_control_index_ >= controls.size()) {
    return std::string();
  }
  return controls[selected_control_index_].identifier;
}

- (void)resetEditLayoutHistory {
  edit_history_.Reset();
}

- (void)seedEditLayoutHistoryIfNeeded {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }
  edit_history_.SeedIfNeeded(runtime_model_->layout(), [self selectedControlIdentifier]);
}

- (BOOL)canUndoEditLayoutChange {
  return edit_history_.CanUndo();
}

- (BOOL)canRedoEditLayoutChange {
  return edit_history_.CanRedo();
}

- (void)applyEditLayoutHistoryState:(const xe::hid::touch::IOSTouchLayoutModel&)layout
                selectingIdentifier:(const std::string&)preferred_identifier {
  if (!runtime_model_) {
    return;
  }

  edit_history_.CancelChange();
  [self clearEditSnapGuides];
  runtime_model_->SetLayout(layout);
  [self refreshLayoutModel];
  if (!preferred_identifier.empty()) {
    [self selectControlWithIdentifier:preferred_identifier];
  }
  [self publishResolvedState];
}

- (void)beginEditLayoutChangeIfNeeded {
  if (!editing_controls_enabled_ || !runtime_model_ || edit_history_.IsChangeActive()) {
    return;
  }
  edit_history_.BeginChange(runtime_model_->layout(), [self selectedControlIdentifier]);
}

- (void)finishEditLayoutChangeIfNeeded {
  if (!edit_history_.IsChangeActive() || !runtime_model_) {
    return;
  }
  const BOOL no_active_gestures = active_captures_.empty() && !edit_pinch_active_;
  if (!no_active_gestures) {
    return;
  }
  if (edit_history_.FinishChange(runtime_model_->layout(), [self selectedControlIdentifier])) {
    [self refreshEditChromeSelection];
  }
}

- (void)undoEditLayoutChange {
  if (![self canUndoEditLayoutChange] || !runtime_model_ || !active_captures_.empty() ||
      edit_pinch_active_) {
    return;
  }
  xe::hid::touch::IOSTouchLayoutModel layout;
  std::string preferred_identifier;
  if (!edit_history_.Undo(&layout, &preferred_identifier)) {
    return;
  }
  [self applyEditLayoutHistoryState:layout selectingIdentifier:preferred_identifier];
  [self refreshEditChromeSelection];
}

- (void)redoEditLayoutChange {
  if (![self canRedoEditLayoutChange] || !runtime_model_ || !active_captures_.empty() ||
      edit_pinch_active_) {
    return;
  }
  xe::hid::touch::IOSTouchLayoutModel layout;
  std::string preferred_identifier;
  if (!edit_history_.Redo(&layout, &preferred_identifier)) {
    return;
  }
  [self applyEditLayoutHistoryState:layout selectingIdentifier:preferred_identifier];
  [self refreshEditChromeSelection];
}

- (void)refreshLayoutModel {
  std::string selected_identifier;
  if (runtime_model_) {
    const auto& existing_controls = runtime_model_->layout().controls;
    if (selected_control_index_ != NSNotFound &&
        selected_control_index_ < existing_controls.size()) {
      selected_identifier = existing_controls[selected_control_index_].identifier;
    }
  }
  [self resetInteractionState];
  for (UIView* control_view in control_views_) {
    [control_view removeFromSuperview];
  }
  [control_views_ removeAllObjects];
  resolved_control_frames_.clear();
  conflicting_control_indices_.clear();
  visually_active_control_indices_.clear();
  recent_action_press_times_.clear();
  recent_action_suppressed_until_times_.clear();
  recent_secondary_press_times_.clear();
  recent_secondary_candidate_times_.clear();
  recent_look_vectors_.clear();
  recent_look_motion_times_.clear();
  recent_move_vectors_.clear();
  recent_move_motion_times_.clear();
  move_control_index_ = NSNotFound;
  look_control_index_ = NSNotFound;
  pause_control_index_ = NSNotFound;
  selected_control_index_ = NSNotFound;

  if (!runtime_model_) {
    pause_button_.hidden = YES;
    move_knob_.hidden = YES;
    return;
  }

  // Every structural layout mutation (add/duplicate/delete/import) funnels
  // through this rebuild, so republish the cross-thread controls snapshot the
  // HID driver uses for guest capability queries.
  runtime_model_->PublishControlsSnapshot();

  const auto& controls = runtime_model_->layout().controls;
  resolved_control_frames_.resize(controls.size());
  conflicting_control_indices_.assign(controls.size(), false);
  visually_active_control_indices_.assign(controls.size(), 0);
  recent_action_press_times_.assign(controls.size(), 0.0);
  recent_action_suppressed_until_times_.assign(controls.size(), 0.0);
  recent_secondary_press_times_.assign(controls.size(), 0.0);
  recent_secondary_candidate_times_.assign(controls.size(), 0.0);
  recent_look_vectors_.assign(controls.size(), CGPointZero);
  recent_look_motion_times_.assign(controls.size(), 0.0);
  recent_move_vectors_.assign(controls.size(), CGPointZero);
  recent_move_motion_times_.assign(controls.size(), 0.0);
  NSUInteger control_index = 0;
  for (const auto& control : controls) {
    XeniaTouchControlShellView* shell_view =
        [[XeniaTouchControlShellView alloc] initWithControl:control];
    [control_views_ addObject:shell_view];
    [self addSubview:shell_view];
    [shell_view release];
    switch (control.type) {
      case xe::hid::touch::IOSTouchControlType::kMoveStick:
        if (control.action == xe::hid::touch::IOSTouchAction::kLook) {
          if (look_control_index_ == NSNotFound) {
            look_control_index_ = control_index;
          }
        } else if (move_control_index_ == NSNotFound) {
          move_control_index_ = control_index;
        }
        break;
      case xe::hid::touch::IOSTouchControlType::kLookSwipeZone:
        if (look_control_index_ == NSNotFound) {
          look_control_index_ = control_index;
        }
        break;
      case xe::hid::touch::IOSTouchControlType::kPauseButton:
        pause_control_index_ = control_index;
        break;
      case xe::hid::touch::IOSTouchControlType::kActionButton:
      default:
        break;
    }
    ++control_index;
  }

  pause_button_.hidden = pause_control_index_ == NSNotFound;
  move_knob_.hidden = YES;
  edit_grid_overlay_.hidden = !editing_controls_enabled_ || !edit_grid_enabled_;
  edit_snap_guides_overlay_.hidden =
      !editing_controls_enabled_ ||
      (active_snap_vertical_guides_.empty() && active_snap_horizontal_guides_.empty());
  edit_safe_area_guide_.hidden = !editing_controls_enabled_;
  edit_chrome_.hidden = !editing_controls_enabled_;
  const BOOL command_bar_hidden =
      !editing_controls_enabled_ || edit_showing_layout_library_;
  edit_command_bar_.hidden = command_bar_hidden;
  edit_command_bar_.alpha = command_bar_hidden ? 0.0 : 1.0;
  edit_command_bar_.userInteractionEnabled = !command_bar_hidden;
  edit_resize_handle_.hidden = !editing_controls_enabled_;
  [self updateControlBehaviorAnnotations];
  [self bringSubviewToFront:edit_safe_area_guide_];
  [self bringSubviewToFront:edit_snap_guides_overlay_];
  [self bringSubviewToFront:edit_chrome_];
  if (!command_bar_hidden) {
    [self bringSubviewToFront:edit_command_bar_];
  }
  [self bringSubviewToFront:edit_resize_handle_];
  [self bringSubviewToFront:move_knob_];
  [self bringSubviewToFront:pause_button_];

  if (!selected_identifier.empty()) {
    const NSUInteger control_count =
        static_cast<NSUInteger>(runtime_model_->layout().controls.size());
    for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
      if (runtime_model_->layout().controls[control_index].identifier == selected_identifier) {
        [self setSelectedControlIndex:control_index];
        break;
      }
    }
  }
  if (editing_controls_enabled_ && selected_control_index_ == NSNotFound && runtime_model_ &&
      !runtime_model_->layout().controls.empty()) {
    [self setSelectedControlIndex:(move_control_index_ != NSNotFound ? move_control_index_ : 0)];
  } else {
    [self refreshEditChromeSelection];
    [self refreshEditPreview];
  }
  [self setNeedsLayout];
}

- (BOOL)isControlIndexCaptured:(NSUInteger)control_index {
  return std::any_of(active_captures_.begin(), active_captures_.end(),
                     [control_index](const TouchCaptureState& capture) {
                       return capture.control_index == control_index;
                     });
}

- (TouchOverlayEditChromeState)currentEditChromeState {
  TouchOverlayEditChromeState state;
  state.editing_enabled = editing_controls_enabled_;
  state.showing_layout_library = edit_showing_layout_library_;

  if (!runtime_model_) {
    return state;
  }

  const auto& controls = runtime_model_->layout().controls;
  if (selected_control_index_ != NSNotFound && selected_control_index_ < controls.size()) {
    state.has_selected_control = true;
    state.selected_control = controls[selected_control_index_];
    state.can_duplicate_selected_control = [self canDuplicateSelectedControl];
  }
  return state;
}

- (void)refreshEditChromeSelection {
  TouchOverlayEditChromeState state = [self currentEditChromeState];
  [edit_chrome_ applyState:state];
  NSString* layout_name = @"Layout";
  if (runtime_model_) {
    layout_name = [NSString stringWithUTF8String:runtime_model_->layout().display_name.c_str()];
  }
  [edit_command_bar_ setLayoutName:layout_name];
  [edit_command_bar_ setCanUndo:[self canUndoEditLayoutChange]
                         canRedo:[self canRedoEditLayoutChange]];
  [edit_command_bar_ setGridActive:edit_grid_enabled_];
  [edit_command_bar_ setAddMenu:[self addControlMenu]];
  [self updateControlBehaviorAnnotations];
  [self setNeedsLayout];
}

#pragma mark - Command bar delegate

- (void)touchEditCommandBarDidRequestLayouts:(XeniaTouchEditCommandBar*)__unused bar {
  if (layoutLibraryHandler_) {
    layoutLibraryHandler_();
  }
}
- (void)touchEditCommandBarDidRequestUndo:(XeniaTouchEditCommandBar*)__unused bar {
  [self undoEditLayoutChange];
}
- (void)touchEditCommandBarDidRequestRedo:(XeniaTouchEditCommandBar*)__unused bar {
  [self redoEditLayoutChange];
}
- (void)touchEditCommandBarDidToggleGrid:(XeniaTouchEditCommandBar*)__unused bar {
  [self toggleEditGrid];
}
- (void)touchEditCommandBarDidRequestAdd:(XeniaTouchEditCommandBar*)__unused bar {
  [self addNewActionButton];
}
- (void)touchEditCommandBarDidRequestDone:(XeniaTouchEditCommandBar*)__unused bar {
  if (doneEditingHandler_) {
    doneEditingHandler_();
  }
}

- (CGRect)selectedControlResizeHandleFrame {
  if (!editing_controls_enabled_ || selected_control_index_ == NSNotFound ||
      selected_control_index_ >= resolved_control_frames_.size()) {
    return CGRectZero;
  }

  const xe::hid::touch::IOSTouchRect& frame = resolved_control_frames_[selected_control_index_];
  const CGFloat handle_size = 28.0f;
  CGRect handle_frame =
      CGRectMake(frame.x + frame.width - handle_size * 0.5f,
                 frame.y + frame.height - handle_size * 0.5f, handle_size, handle_size);

  xe::hid::touch::IOSTouchLayoutSpace safe_area = TouchSafeAreaSpaceForView(self);
  if (safe_area.IsEmpty()) {
    safe_area = TouchLayoutSpaceForView(self);
  }
  if (!safe_area.IsEmpty()) {
    constexpr CGFloat kHandleEdgeMargin = 8.0f;
    const CGFloat min_x = safe_area.origin_x + kHandleEdgeMargin;
    const CGFloat min_y = safe_area.origin_y + kHandleEdgeMargin;
    const CGFloat max_x =
        MAX(min_x, safe_area.origin_x + safe_area.width - handle_size - kHandleEdgeMargin);
    const CGFloat max_y =
        MAX(min_y, safe_area.origin_y + safe_area.height - handle_size - kHandleEdgeMargin);
    handle_frame.origin.x =
        std::clamp<CGFloat>(handle_frame.origin.x, min_x, max_x);
    handle_frame.origin.y =
        std::clamp<CGFloat>(handle_frame.origin.y, min_y, max_y);
  }
  return handle_frame;
}

- (void)refreshEditPreview {
  [edit_chrome_ applyState:[self currentEditChromeState]];
  [self setNeedsLayout];
}

- (float)doubleTapWindowSecondsForControl:
    (const xe::hid::touch::IOSTouchControlDefinition&)control {
  return std::clamp(control.secondary_behavior.hold_seconds, 0.12f, 0.60f);
}

- (void)triggerSecondaryBehaviorPulseForControlIndex:(NSUInteger)control_index
                                              atTime:(CFTimeInterval)current_time {
  if (!runtime_model_ || control_index >= recent_secondary_press_times_.size()) {
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  if (control_index >= controls.size() ||
      controls[control_index].secondary_behavior.action == xe::hid::touch::IOSTouchAction::kNone) {
    return;
  }

  recent_secondary_press_times_[control_index] = current_time;
}

- (BOOL)hasPendingDoubleTapCandidateForControlIndex:(NSUInteger)control_index
                                             atTime:(CFTimeInterval)current_time {
  if (!runtime_model_ || control_index >= recent_secondary_candidate_times_.size()) {
    return NO;
  }

  const auto& controls = runtime_model_->layout().controls;
  if (control_index >= controls.size()) {
    return NO;
  }

  const CFTimeInterval candidate_time = recent_secondary_candidate_times_[control_index];
  if (candidate_time <= 0.0) {
    return NO;
  }

  return (current_time - candidate_time) <=
         [self doubleTapWindowSecondsForControl:controls[control_index]];
}

- (BOOL)consumeDoubleTapCandidateForControlIndex:(NSUInteger)control_index
                                          atTime:(CFTimeInterval)current_time {
  if (![self hasPendingDoubleTapCandidateForControlIndex:control_index atTime:current_time]) {
    return NO;
  }

  recent_secondary_candidate_times_[control_index] = 0.0;
  [self triggerSecondaryBehaviorPulseForControlIndex:control_index atTime:current_time];
  return YES;
}

- (void)storeDoubleTapCandidateForControlIndex:(NSUInteger)control_index
                                        atTime:(CFTimeInterval)current_time {
  if (control_index >= recent_secondary_candidate_times_.size()) {
    return;
  }
  recent_secondary_candidate_times_[control_index] = current_time;
}

- (void)setSelectedControlAction:(xe::hid::touch::IOSTouchAction)action {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  if (control.action == action) {
    return;
  }
  if (!xe::hid::touch::IsSupportedIOSTouchPrimaryAction(control.type, action)) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  if (control.type == xe::hid::touch::IOSTouchControlType::kActionButton) {
    const xe::hid::touch::IOSTouchAnalogOutput previous_drag_output =
        EffectiveControlDragOutput(control);
    const bool previous_trigger_drag_disabled =
        IsTriggerAction(control.action) &&
        previous_drag_output == xe::hid::touch::IOSTouchAnalogOutput::kNone;
    // Action buttons need their mapped XInput button bits / triggers /
    // hold-while-captured semantics reset and re-derived from the new action.
    xe::hid::touch::ConfigureIOSTouchControlAction(action, &control);
    // LT/RT default to a held look-drag helper, but a user who had already
    // turned a trigger's primary drag off should not get it silently re-added
    // while simply changing the trigger binding.
    if (previous_trigger_drag_disabled) {
      control.drag_output = xe::hid::touch::IOSTouchAnalogOutput::kNone;
      control.enables_relative_look = false;
    }
  } else {
    // Move / Look / Pause types don't carry button-mapping data; just swap
    // the action so the publish path routes the control's input to the new
    // thumbstick (or pause behaviour) without disturbing the rest of the
    // control definition.
    control.action = action;
  }
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlLabelHidden:(BOOL)hidden {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  if (control.label_hidden == hidden) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.label_hidden = hidden;
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self applyCaptureVisualState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)resetSelectedControlLabel {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  if (!xe::hid::touch::IOSTouchControlHasCustomLabel(control)) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  xe::hid::touch::ResetIOSTouchControlLabel(&control);
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self applyCaptureVisualState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlCustomLabelText:(NSString*)label_text {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  std::string next_label = label_text ? std::string(label_text.UTF8String) : std::string();
  const std::string prior_label = xe::hid::touch::IOSTouchConfiguredControlLabel(control);
  const bool prior_has_custom_label = xe::hid::touch::IOSTouchControlHasCustomLabel(control);
  [self beginEditLayoutChangeIfNeeded];
  xe::hid::touch::SetIOSTouchControlCustomLabel(std::move(next_label), &control);
  if (xe::hid::touch::IOSTouchConfiguredControlLabel(control) == prior_label &&
      xe::hid::touch::IOSTouchControlHasCustomLabel(control) == prior_has_custom_label) {
    edit_history_.CancelChange();
    return;
  }

  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self applyCaptureVisualState];
  [self finishEditLayoutChangeIfNeeded];
}

- (UIViewController*)labelEditPresenter {
  UIResponder* responder = self;
  while ((responder = responder.nextResponder)) {
    if ([responder isKindOfClass:[UIViewController class]]) {
      UIViewController* controller = (UIViewController*)responder;
      while (controller.presentedViewController &&
             !controller.presentedViewController.isBeingDismissed) {
        controller = controller.presentedViewController;
      }
      return controller;
    }
  }

  UIViewController* controller = self.window.rootViewController;
  while (controller.presentedViewController &&
         !controller.presentedViewController.isBeingDismissed) {
    controller = controller.presentedViewController;
  }
  return controller;
}

- (void)presentLabelRenameAlert {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  UIViewController* presenter = [self labelEditPresenter];
  if (!presenter) {
    return;
  }

  const auto& control = controls[selected_control_index_];
  NSString* current_label = XeniaTouchConfiguredControlLabelText(control, NO);
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:@"Control Label"
                                          message:@"Leave blank to use the default label."
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* text_field) {
    text_field.text = current_label;
    text_field.placeholder = @"Label";
    text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
    text_field.returnKeyType = UIReturnKeyDone;
  }];

  __unsafe_unretained XeniaTouchControlsOverlayView* unsafe_self = self;
  __unsafe_unretained UIAlertController* unsafe_alert = alert;
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  UIAlertAction* save_action =
      [UIAlertAction actionWithTitle:@"Save"
                               style:UIAlertActionStyleDefault
                             handler:^(__unused UIAlertAction* action_handler) {
                               UITextField* text_field = unsafe_alert.textFields.firstObject;
                               [unsafe_self setSelectedControlCustomLabelText:text_field.text];
                             }];
  [alert addAction:save_action];
  alert.preferredAction = save_action;
  [presenter presentViewController:alert animated:YES completion:nil];
}

- (BOOL)copySelectedAnalogTuningForPanel:(xe::hid::touch::IOSTouchAnalogTuning*)tuning
                                deadzone:(float*)deadzone
                        activationRadius:(float*)activation_radius
                                heldLook:(float*)held_look_scale
                                heldMove:(float*)held_move_scale
                            supportsHeld:(BOOL*)supports_held {
  if (!runtime_model_ || selected_control_index_ == NSNotFound) {
    return NO;
  }
  const auto& controls = runtime_model_->layout().controls;
  if (selected_control_index_ >= controls.size()) {
    return NO;
  }
  const auto& control = controls[selected_control_index_];
  if (tuning) {
    *tuning = control.analog_tuning;
  }
  if (deadzone) {
    *deadzone = control.deadzone;
  }
  if (activation_radius) {
    *activation_radius = control.activation_radius;
  }
  if (held_look_scale) {
    *held_look_scale = control.held_look_scale;
  }
  if (held_move_scale) {
    *held_move_scale = control.held_move_scale;
  }
  if (supports_held) {
    *supports_held = control.type == xe::hid::touch::IOSTouchControlType::kActionButton;
  }
  return YES;
}

- (void)presentAnalogTuningPanel {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }
  UIViewController* presenter = [self labelEditPresenter];
  if (!presenter) {
    return;
  }
  XeniaTouchAnalogTuningViewController* tuning_controller =
      [[XeniaTouchAnalogTuningViewController alloc] initWithOverlay:self];
  UINavigationController* navigation_controller =
      [[UINavigationController alloc] initWithRootViewController:tuning_controller];
  [tuning_controller release];
  XEConfigureTaskSheet(navigation_controller, self, CGSizeMake(760.0, 620.0), NO);
  [presenter presentViewController:navigation_controller animated:YES completion:nil];
  [navigation_controller release];
}

- (void)setSelectedControlBehaviorTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const bool supports_behavior =
      control.type == xe::hid::touch::IOSTouchControlType::kActionButton ||
      control.type == xe::hid::touch::IOSTouchControlType::kMoveStick;
  if (!supports_behavior || control.secondary_behavior.trigger == trigger) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.secondary_behavior.trigger = trigger;
  control.secondary_behavior.hold_seconds =
      xe::hid::touch::DefaultIOSTouchHoldSecondsForInteractionTrigger(trigger);
  if (trigger == xe::hid::touch::IOSTouchInteractionTrigger::kNone) {
    control.secondary_behavior.enables_relative_look = false;
    control.secondary_behavior.analog_output = xe::hid::touch::IOSTouchAnalogOutput::kNone;
  } else if (trigger == xe::hid::touch::IOSTouchInteractionTrigger::kHoldDrag) {
    control.secondary_behavior.enables_relative_look = true;
    if (control.secondary_behavior.analog_output == xe::hid::touch::IOSTouchAnalogOutput::kNone) {
      control.secondary_behavior.analog_output = xe::hid::touch::IOSTouchAnalogOutput::kLook;
    }
    if (control.secondary_behavior.relative_look_scale <= 0.0f) {
      control.secondary_behavior.relative_look_scale = 1.0f;
    }
  } else {
    control.secondary_behavior.enables_relative_look = false;
    if (control.secondary_behavior.analog_output == xe::hid::touch::IOSTouchAnalogOutput::kLook) {
      control.secondary_behavior.analog_output = xe::hid::touch::IOSTouchAnalogOutput::kNone;
    }
  }
  if (control.type == xe::hid::touch::IOSTouchControlType::kMoveStick &&
      trigger == xe::hid::touch::IOSTouchInteractionTrigger::kDoubleTapForward &&
      control.secondary_behavior.action == xe::hid::touch::IOSTouchAction::kNone) {
    control.secondary_behavior.action = xe::hid::touch::IOSTouchAction::kLeftThumb;
  }
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlBehaviorAction:(xe::hid::touch::IOSTouchAction)action {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const bool supports_behavior =
      control.type == xe::hid::touch::IOSTouchControlType::kActionButton ||
      control.type == xe::hid::touch::IOSTouchControlType::kMoveStick;
  if (!supports_behavior || control.secondary_behavior.action == action) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.secondary_behavior.action = action;
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlDragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const xe::hid::touch::IOSTouchAnalogOutput current_output = EffectiveControlDragOutput(control);
  if (control.type != xe::hid::touch::IOSTouchControlType::kActionButton ||
      (current_output == output &&
       control.enables_relative_look == (output == xe::hid::touch::IOSTouchAnalogOutput::kLook))) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.drag_output = output;
  control.enables_relative_look = output == xe::hid::touch::IOSTouchAnalogOutput::kLook;
  if (output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    control.hold_while_captured = true;
  }
  if (control.enables_relative_look) {
    control.relative_look_scale = control.analog_tuning.horizontal_scale;
  }
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlBehaviorAnalogOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const bool supports_behavior =
      control.type == xe::hid::touch::IOSTouchControlType::kActionButton ||
      control.type == xe::hid::touch::IOSTouchControlType::kMoveStick;
  if (!supports_behavior || control.secondary_behavior.analog_output == output) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.secondary_behavior.analog_output = output;
  control.secondary_behavior.enables_relative_look =
      output == xe::hid::touch::IOSTouchAnalogOutput::kLook;
  if (output != xe::hid::touch::IOSTouchAnalogOutput::kNone &&
      control.secondary_behavior.trigger == xe::hid::touch::IOSTouchInteractionTrigger::kNone) {
    control.secondary_behavior.trigger = xe::hid::touch::IOSTouchInteractionTrigger::kHoldDrag;
    control.secondary_behavior.hold_seconds =
        xe::hid::touch::DefaultIOSTouchHoldSecondsForInteractionTrigger(
            control.secondary_behavior.trigger);
  }
  if (output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    control.secondary_behavior.analog_tuning = control.analog_tuning;
  }
  if (control.secondary_behavior.enables_relative_look) {
    control.secondary_behavior.relative_look_scale =
        control.secondary_behavior.analog_tuning.horizontal_scale;
  }
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlSecondaryBehaviorTrigger:
            (xe::hid::touch::IOSTouchInteractionTrigger)trigger
                                            action:(xe::hid::touch::IOSTouchAction)action
                                      analogOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const bool supports_behavior =
      control.type == xe::hid::touch::IOSTouchControlType::kActionButton ||
      control.type == xe::hid::touch::IOSTouchControlType::kMoveStick;
  if (!supports_behavior) {
    return;
  }
  if (control.type != xe::hid::touch::IOSTouchControlType::kActionButton &&
      output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    output = xe::hid::touch::IOSTouchAnalogOutput::kNone;
  }

  auto& behavior = control.secondary_behavior;
  const bool next_relative_look = output == xe::hid::touch::IOSTouchAnalogOutput::kLook;
  if (behavior.trigger == trigger && behavior.action == action &&
      behavior.analog_output == output && behavior.enables_relative_look == next_relative_look) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  behavior.trigger = trigger;
  behavior.action = action;
  behavior.analog_output = output;
  behavior.enables_relative_look = next_relative_look;
  behavior.hold_seconds = xe::hid::touch::DefaultIOSTouchHoldSecondsForInteractionTrigger(trigger);
  if (trigger == xe::hid::touch::IOSTouchInteractionTrigger::kNone) {
    behavior.action = xe::hid::touch::IOSTouchAction::kNone;
    behavior.analog_output = xe::hid::touch::IOSTouchAnalogOutput::kNone;
    behavior.enables_relative_look = false;
  } else if (output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    behavior.analog_tuning = control.analog_tuning;
  }
  if (behavior.enables_relative_look) {
    behavior.relative_look_scale = behavior.analog_tuning.horizontal_scale;
  }

  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)clearSelectedControlExtras {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const bool supports_extras = control.type == xe::hid::touch::IOSTouchControlType::kActionButton ||
                               control.type == xe::hid::touch::IOSTouchControlType::kMoveStick;
  if (!supports_extras) {
    return;
  }

  const bool has_primary_drag =
      control.type == xe::hid::touch::IOSTouchControlType::kActionButton &&
      EffectiveControlDragOutput(control) != xe::hid::touch::IOSTouchAnalogOutput::kNone;
  const bool has_dpad_ring = control.type == xe::hid::touch::IOSTouchControlType::kMoveStick &&
                             control.move_with_dpad_ring;
  const auto& behavior = control.secondary_behavior;
  const bool has_secondary =
      behavior.trigger != xe::hid::touch::IOSTouchInteractionTrigger::kNone ||
      behavior.action != xe::hid::touch::IOSTouchAction::kNone ||
      behavior.analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone ||
      behavior.enables_relative_look;
  if (!has_primary_drag && !has_dpad_ring && !has_secondary) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  if (control.type == xe::hid::touch::IOSTouchControlType::kActionButton) {
    control.drag_output = xe::hid::touch::IOSTouchAnalogOutput::kNone;
    control.enables_relative_look = false;
  }
  if (control.type == xe::hid::touch::IOSTouchControlType::kMoveStick) {
    control.move_with_dpad_ring = false;
  }
  control.secondary_behavior = xe::hid::touch::IOSTouchInteractionBehavior{};
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlAnalogTuningField:(TouchAnalogTuningField)field value:(float)value {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  auto& tuning = control.analog_tuning;
  float* target_value = nullptr;
  bool* target_bool = nullptr;
  float min_value = 0.1f;
  float max_value = 4.0f;
  switch (field) {
    case TouchAnalogTuningField::kDeadzone:
      target_value = &control.deadzone;
      min_value = 0.0f;
      max_value = 0.95f;
      break;
    case TouchAnalogTuningField::kActivationRadius:
      target_value = &control.activation_radius;
      min_value = 0.05f;
      max_value = 1.0f;
      break;
    case TouchAnalogTuningField::kHorizontalScale:
      target_value = &tuning.horizontal_scale;
      break;
    case TouchAnalogTuningField::kVerticalScale:
      target_value = &tuning.vertical_scale;
      break;
    case TouchAnalogTuningField::kDiagonalScale:
      target_value = &tuning.diagonal_scale;
      break;
    case TouchAnalogTuningField::kResponseCurve:
      target_value = &tuning.response_curve;
      min_value = 0.25f;
      max_value = 4.0f;
      break;
    case TouchAnalogTuningField::kAccelerationScale:
      target_value = &tuning.acceleration_scale;
      min_value = 0.0f;
      max_value = 2.0f;
      break;
    case TouchAnalogTuningField::kSmoothing:
      target_value = &tuning.smoothing;
      min_value = 0.0f;
      max_value = 0.95f;
      break;
    case TouchAnalogTuningField::kMaxOutput:
      target_value = &tuning.max_output;
      min_value = 0.1f;
      max_value = 1.0f;
      break;
    case TouchAnalogTuningField::kInvertX:
      target_bool = &tuning.invert_x;
      break;
    case TouchAnalogTuningField::kInvertY:
      target_bool = &tuning.invert_y;
      break;
  }
  if (target_bool) {
    const bool next_value = value >= 0.5f;
    if (*target_bool == next_value) {
      return;
    }
    [self beginEditLayoutChangeIfNeeded];
    *target_bool = next_value;
    if (control.secondary_behavior.analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
      control.secondary_behavior.analog_tuning = tuning;
    }
    [self syncControlViewDefinitions];
    [self refreshEditChromeSelection];
    [self refreshEditPreview];
    [self applyCaptureVisualState];
    [self publishResolvedState];
    [self finishEditLayoutChangeIfNeeded];
    return;
  }
  if (!target_value) {
    return;
  }

  const float clamped_value = std::clamp(value, min_value, max_value);
  if (std::abs(*target_value - clamped_value) < 0.001f) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  *target_value = clamped_value;
  if (field == TouchAnalogTuningField::kDeadzone) {
    tuning.deadzone = control.deadzone;
  } else if (field == TouchAnalogTuningField::kActivationRadius) {
    tuning.activation_radius = control.activation_radius;
  }
  if (control.secondary_behavior.analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    control.secondary_behavior.analog_tuning = tuning;
  }
  if (control.drag_output == xe::hid::touch::IOSTouchAnalogOutput::kLook ||
      control.enables_relative_look ||
      control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    control.relative_look_scale = tuning.horizontal_scale;
  }
  if (control.secondary_behavior.analog_output == xe::hid::touch::IOSTouchAnalogOutput::kLook) {
    control.secondary_behavior.relative_look_scale =
        control.secondary_behavior.analog_tuning.horizontal_scale;
  }
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlOverallAnalogSensitivity:(float)value {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  auto& tuning = control.analog_tuning;
  const float clamped_scale = std::clamp(value, 0.25f, 4.0f);
  if (std::abs(tuning.horizontal_scale - clamped_scale) < 0.001f &&
      std::abs(tuning.vertical_scale - clamped_scale) < 0.001f) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  tuning.horizontal_scale = clamped_scale;
  tuning.vertical_scale = clamped_scale;
  if (control.secondary_behavior.analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    control.secondary_behavior.analog_tuning = tuning;
  }
  if (control.drag_output == xe::hid::touch::IOSTouchAnalogOutput::kLook ||
      control.enables_relative_look ||
      control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    control.relative_look_scale = clamped_scale;
  }
  if (control.secondary_behavior.analog_output == xe::hid::touch::IOSTouchAnalogOutput::kLook) {
    control.secondary_behavior.relative_look_scale =
        control.secondary_behavior.analog_tuning.horizontal_scale;
  }
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)resetSelectedControlAnalogTuning {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const auto default_control = xe::hid::touch::CreateDefaultIOSTouchControlDefinition(control.type);
  [self beginEditLayoutChangeIfNeeded];
  control.deadzone = default_control.deadzone;
  control.activation_radius = default_control.activation_radius;
  control.analog_tuning = xe::hid::touch::IOSTouchAnalogTuning();
  control.analog_tuning.deadzone = control.deadzone;
  control.analog_tuning.activation_radius = control.activation_radius;
  control.held_look_scale = 1.0f;
  control.held_move_scale = 1.0f;
  if (control.secondary_behavior.analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
    control.secondary_behavior.analog_tuning = control.analog_tuning;
  }
  if (control.drag_output == xe::hid::touch::IOSTouchAnalogOutput::kLook ||
      control.enables_relative_look ||
      control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    control.relative_look_scale = control.analog_tuning.horizontal_scale;
  }
  if (control.secondary_behavior.analog_output == xe::hid::touch::IOSTouchAnalogOutput::kLook) {
    control.secondary_behavior.relative_look_scale =
        control.secondary_behavior.analog_tuning.horizontal_scale;
  }
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlHeldLookScale:(float)look_scale {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const float clamped_scale = std::clamp(look_scale, 0.25f, 4.0f);
  if (control.type != xe::hid::touch::IOSTouchControlType::kActionButton ||
      std::abs(control.held_look_scale - clamped_scale) < 0.001f) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.held_look_scale = clamped_scale;
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlHeldMoveScale:(float)move_scale {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  const float clamped_scale = std::clamp(move_scale, 0.25f, 4.0f);
  if (control.type != xe::hid::touch::IOSTouchControlType::kActionButton ||
      std::abs(control.held_move_scale - clamped_scale) < 0.001f) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.held_move_scale = clamped_scale;
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlTintStyle:(xe::hid::touch::IOSTouchTintStyle)tint_style {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  if (control.tint_style == tint_style) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.tint_style = tint_style;
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlOpacity:(float)opacity {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }
  auto& control = controls[selected_control_index_];
  const float clamped = std::clamp(opacity, 0.2f, 1.0f);
  if (std::abs(control.visual_opacity - clamped) < 0.001f) {
    return;
  }
  [self beginEditLayoutChangeIfNeeded];
  control.visual_opacity = clamped;
  if (selected_control_index_ < control_views_.count) {
    [[control_views_ objectAtIndex:selected_control_index_] applyControlDefinition:control];
  }
  [self setNeedsLayout];
  [self applyCaptureVisualState];
  if (!edit_opacity_slider_active_) {
    [self finishEditLayoutChangeIfNeeded];
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
             didRequestOpacity:(float)opacity {
  [self setSelectedControlOpacity:opacity];
}

- (void)touchOverlayEditChromeDidBeginOpacityChange:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  edit_opacity_slider_active_ = YES;
  [self beginEditLayoutChangeIfNeeded];
}

- (void)touchOverlayEditChromeDidEndOpacityChange:(XeniaTouchOverlayEditChromeIOS*)__unused chrome {
  edit_opacity_slider_active_ = NO;
  [self finishEditLayoutChangeIfNeeded];
}

- (void)setSelectedControlShape:(xe::hid::touch::IOSTouchControlShape)shape {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  if (control.type != xe::hid::touch::IOSTouchControlType::kActionButton ||
      control.shape == shape) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.shape = shape;
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self setNeedsLayout];
  [self layoutIfNeeded];
  [self applyCaptureVisualState];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (BOOL)layoutContainsControlType:(xe::hid::touch::IOSTouchControlType)type {
  if (!runtime_model_) {
    return NO;
  }
  const auto& controls = runtime_model_->layout().controls;
  return std::any_of(controls.begin(), controls.end(),
                     [type](const xe::hid::touch::IOSTouchControlDefinition& control) {
                       return control.type == type;
                     });
}

- (BOOL)layoutContainsJoystickAction:(xe::hid::touch::IOSTouchAction)action {
  if (!runtime_model_) {
    return NO;
  }
  const auto& controls = runtime_model_->layout().controls;
  return std::any_of(controls.begin(), controls.end(),
                     [action](const xe::hid::touch::IOSTouchControlDefinition& control) {
                       return control.type == xe::hid::touch::IOSTouchControlType::kMoveStick &&
                              control.action == action;
                     });
}

- (void)addActionButtonWithAction:(xe::hid::touch::IOSTouchAction)action {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  auto& layout = runtime_model_->mutable_layout();
  if (layout.controls.size() >= xe::hid::touch::kMaxIOSTouchControls) {
    return;
  }

  [self clearEditSnapGuides];
  [self beginEditLayoutChangeIfNeeded];
  const bool is_portrait = TouchOverlayIsPortraitForView(self);
  xe::hid::touch::IOSTouchControlDefinition control;
  control.identifier = xe::hid::touch::MakeUniqueIOSTouchActionButtonIdentifier(layout);
  control.type = xe::hid::touch::IOSTouchControlType::kActionButton;
  control.shape = xe::hid::touch::IOSTouchControlShape::kCircle;
  control.activation_radius = 0.5f;
  control.analog_tuning.activation_radius = control.activation_radius;
  control.visual_opacity = 0.92f;
  control.capture_priority = 232;
  control.normalized_frame = xe::hid::touch::FindAvailableIOSTouchEditorControlFrame(
      layout, xe::hid::touch::SuggestedNewIOSTouchActionButtonFrame(action), control.type,
      is_portrait);
  if (is_portrait) {
    control.has_portrait_frame = true;
    control.portrait_normalized_frame = control.normalized_frame;
  }
  xe::hid::touch::ConfigureIOSTouchControlAction(action, &control);
  const std::string selected_identifier = control.identifier;
  layout.controls.push_back(std::move(control));
  [self refreshLayoutModel];
  [self selectControlWithIdentifier:selected_identifier];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)addJoystickWithAction:(xe::hid::touch::IOSTouchAction)action {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  if (action != xe::hid::touch::IOSTouchAction::kMove &&
      action != xe::hid::touch::IOSTouchAction::kLook) {
    return;
  }
  if ([self layoutContainsJoystickAction:action]) {
    return;
  }

  auto& layout = runtime_model_->mutable_layout();
  if (layout.controls.size() >= xe::hid::touch::kMaxIOSTouchControls) {
    return;
  }

  [self clearEditSnapGuides];
  [self beginEditLayoutChangeIfNeeded];
  const bool is_portrait = TouchOverlayIsPortraitForView(self);
  xe::hid::touch::IOSTouchControlDefinition control =
      xe::hid::touch::CreateDefaultIOSTouchControlDefinition(
          xe::hid::touch::IOSTouchControlType::kMoveStick);
  control.identifier = UniqueTouchControlIdentifier(
      layout, action == xe::hid::touch::IOSTouchAction::kLook ? "look_stick" : "move_stick");
  control.action = action;
  const xe::hid::touch::IOSTouchRect preferred_frame =
      action == xe::hid::touch::IOSTouchAction::kLook
          ? xe::hid::touch::IOSTouchRect{0.71f, 0.55f, 0.28f, 0.35f}
          : control.normalized_frame;
  control.normalized_frame = xe::hid::touch::FindAvailableIOSTouchEditorControlFrame(
      layout, preferred_frame, control.type, is_portrait);
  if (is_portrait) {
    control.has_portrait_frame = true;
    control.portrait_normalized_frame = control.normalized_frame;
  }
  const std::string selected_identifier = control.identifier;
  layout.controls.push_back(std::move(control));
  [self refreshLayoutModel];
  [self selectControlWithIdentifier:selected_identifier];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (UIMenu*)addControlMenu {
  __unsafe_unretained XeniaTouchControlsOverlayView* unsafe_self = self;
  const BOOL at_limit =
      !runtime_model_ ||
      runtime_model_->layout().controls.size() >= xe::hid::touch::kMaxIOSTouchControls;
  const BOOL has_move_stick =
      [self layoutContainsJoystickAction:xe::hid::touch::IOSTouchAction::kMove];
  const BOOL has_look_stick =
      [self layoutContainsJoystickAction:xe::hid::touch::IOSTouchAction::kLook];
  const BOOL has_look_zone =
      [self layoutContainsControlType:xe::hid::touch::IOSTouchControlType::kLookSwipeZone];
  const BOOL has_pause_button =
      [self layoutContainsControlType:xe::hid::touch::IOSTouchControlType::kPauseButton];

  UIAction* (^makeAction)(NSString*, NSString*, BOOL, void (^)(void)) =
      ^UIAction*(NSString* title, NSString* symbol, BOOL disabled, void (^handler)(void)) {
        UIImage* image = symbol.length ? [UIImage systemImageNamed:symbol] : nil;
        UIAction* action = [UIAction actionWithTitle:title
                                               image:image
                                          identifier:nil
                                             handler:^(__unused UIAction* action_handler) {
                                               if (handler) {
                                                 handler();
                                               }
                                             }];
        if (disabled) {
          action.attributes = UIMenuElementAttributesDisabled;
        }
        return action;
      };

  void (^addActionEntries)(NSMutableArray<UIMenuElement*>*, NSString*,
                           const xe::hid::touch::IOSTouchAction*, size_t) =
      ^(NSMutableArray<UIMenuElement*>* items, NSString* symbol,
        const xe::hid::touch::IOSTouchAction* actions, size_t count) {
        for (size_t i = 0; i < count; ++i) {
          const xe::hid::touch::IOSTouchAction action = actions[i];
          NSString* title =
              [NSString stringWithUTF8String:xe::hid::touch::IOSTouchActionDisplayName(action)];
          [items addObject:makeAction(title, symbol, at_limit, ^{
                   [unsafe_self addActionButtonWithAction:action];
                 })];
        }
      };

  NSMutableArray<UIMenuElement*>* face = [NSMutableArray arrayWithCapacity:4];
  const xe::hid::touch::IOSTouchAction face_actions[] = {
      xe::hid::touch::IOSTouchAction::kButtonA, xe::hid::touch::IOSTouchAction::kButtonB,
      xe::hid::touch::IOSTouchAction::kButtonX, xe::hid::touch::IOSTouchAction::kButtonY};
  addActionEntries(face, @"circle", face_actions, xe::countof(face_actions));

  NSMutableArray<UIMenuElement*>* shoulders = [NSMutableArray arrayWithCapacity:4];
  const xe::hid::touch::IOSTouchAction shoulder_actions[] = {
      xe::hid::touch::IOSTouchAction::kLeftBumper, xe::hid::touch::IOSTouchAction::kRightBumper,
      xe::hid::touch::IOSTouchAction::kLeftTrigger, xe::hid::touch::IOSTouchAction::kRightTrigger};
  addActionEntries(shoulders, @"rectangle.roundedtop", shoulder_actions,
                   xe::countof(shoulder_actions));

  NSMutableArray<UIMenuElement*>* system = [NSMutableArray arrayWithCapacity:10];
  const xe::hid::touch::IOSTouchAction system_actions[] = {
      xe::hid::touch::IOSTouchAction::kBack,      xe::hid::touch::IOSTouchAction::kStart,
      xe::hid::touch::IOSTouchAction::kLeftThumb, xe::hid::touch::IOSTouchAction::kRightThumb,
      xe::hid::touch::IOSTouchAction::kDpadUp,    xe::hid::touch::IOSTouchAction::kDpadDown,
      xe::hid::touch::IOSTouchAction::kDpadLeft,  xe::hid::touch::IOSTouchAction::kDpadRight};
  addActionEntries(system, @"dpad", system_actions, xe::countof(system_actions));
  [system addObject:makeAction(@"Blank Button", @"square.dashed", at_limit, ^{
            [unsafe_self addActionButtonWithAction:xe::hid::touch::IOSTouchAction::kNone];
          })];

  NSMutableArray<UIMenuElement*>* analog = [NSMutableArray arrayWithCapacity:5];
  [analog addObject:makeAction(@"Move Stick", @"circle.grid.cross", at_limit || has_move_stick, ^{
            [unsafe_self addJoystickWithAction:xe::hid::touch::IOSTouchAction::kMove];
          })];
  [analog addObject:makeAction(@"Look Stick", @"scope", at_limit || has_look_stick, ^{
            [unsafe_self addJoystickWithAction:xe::hid::touch::IOSTouchAction::kLook];
          })];
  [analog addObject:makeAction(@"Look Zone", @"rectangle.dashed", at_limit || has_look_zone, ^{
            [unsafe_self addControlOfType:xe::hid::touch::IOSTouchControlType::kLookSwipeZone];
          })];
  [analog addObject:makeAction(@"Pause Button", @"pause.fill", at_limit || has_pause_button, ^{
            [unsafe_self addControlOfType:xe::hid::touch::IOSTouchControlType::kPauseButton];
          })];

  return [UIMenu menuWithTitle:@""
                      children:@[
                        [UIMenu menuWithTitle:@"Face Buttons" children:face],
                        [UIMenu menuWithTitle:@"Shoulders / Triggers" children:shoulders],
                        [UIMenu menuWithTitle:@"Menu / D-Pad" children:system],
                        [UIMenu menuWithTitle:@"Analog / Utility" children:analog],
                      ]];
}

- (void)addControlOfType:(xe::hid::touch::IOSTouchControlType)type {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  if (runtime_model_->layout().controls.size() >= xe::hid::touch::kMaxIOSTouchControls) {
    return;
  }
  if (type == xe::hid::touch::IOSTouchControlType::kActionButton) {
    [self addNewActionButton];
    return;
  }
  if (type == xe::hid::touch::IOSTouchControlType::kMoveStick) {
    [self addJoystickWithAction:xe::hid::touch::IOSTouchAction::kMove];
    return;
  }
  if ([self layoutContainsControlType:type]) {
    return;
  }

  [self clearEditSnapGuides];
  [self beginEditLayoutChangeIfNeeded];
  auto& layout = runtime_model_->mutable_layout();
  xe::hid::touch::IOSTouchControlDefinition control =
      xe::hid::touch::CreateDefaultIOSTouchControlDefinition(type);
  const std::string identifier_base =
      type == xe::hid::touch::IOSTouchControlType::kMoveStick
          ? (control.action == xe::hid::touch::IOSTouchAction::kLook ? "look_stick" : "move_stick")
          : control.identifier;
  control.identifier = UniqueTouchControlIdentifier(layout, identifier_base);
  xe::hid::touch::IOSTouchRect& active_frame =
      xe::hid::touch::MutableActiveControlFrameForOrientation(control,
                                                              TouchOverlayIsPortraitForView(self));
  active_frame = xe::hid::touch::FindAvailableIOSTouchEditorControlFrame(
      layout, active_frame, control.type, TouchOverlayIsPortraitForView(self));
  const std::string selected_identifier = control.identifier;
  layout.controls.push_back(std::move(control));
  [self refreshLayoutModel];
  [self selectControlWithIdentifier:selected_identifier];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (NSUInteger)nearestSizeMatchControlIndexForSelectedControl {
  if (!runtime_model_ || selected_control_index_ == NSNotFound ||
      selected_control_index_ >= resolved_control_frames_.size()) {
    return NSNotFound;
  }

  const auto& controls = runtime_model_->layout().controls;
  if (selected_control_index_ >= controls.size()) {
    return NSNotFound;
  }

  const auto& selected_control = controls[selected_control_index_];
  if (selected_control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
    return NSNotFound;
  }

  const xe::hid::touch::IOSTouchRect& selected_frame =
      resolved_control_frames_[selected_control_index_];
  if (selected_frame.width <= 0.0f || selected_frame.height <= 0.0f) {
    return NSNotFound;
  }
  const CGFloat selected_center_x = selected_frame.x + selected_frame.width * 0.5f;
  const CGFloat selected_center_y = selected_frame.y + selected_frame.height * 0.5f;

  auto find_best_match = [&](BOOL same_type_only) {
    NSUInteger best_index = NSNotFound;
    CGFloat best_distance = CGFLOAT_MAX;
    for (NSUInteger control_index = 0;
         control_index < MIN(static_cast<NSUInteger>(controls.size()),
                             static_cast<NSUInteger>(resolved_control_frames_.size()));
         ++control_index) {
      if (control_index == selected_control_index_) {
        continue;
      }
      const auto& candidate = controls[control_index];
      if (candidate.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
        continue;
      }
      if (same_type_only && candidate.type != selected_control.type) {
        continue;
      }
      const xe::hid::touch::IOSTouchRect& candidate_frame = resolved_control_frames_[control_index];
      const CGFloat candidate_center_x = candidate_frame.x + candidate_frame.width * 0.5f;
      const CGFloat candidate_center_y = candidate_frame.y + candidate_frame.height * 0.5f;
      const CGFloat distance = std::hypot(candidate_center_x - selected_center_x,
                                          candidate_center_y - selected_center_y);
      if (distance < best_distance) {
        best_distance = distance;
        best_index = control_index;
      }
    }
    return best_index;
  };

  NSUInteger match_index = find_best_match(YES);
  return match_index != NSNotFound ? match_index : find_best_match(NO);
}

- (BOOL)commitSelectedControlResolvedFrame:(const xe::hid::touch::IOSTouchRect&)candidate_frame {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return NO;
  }

  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return NO;
  }

  const bool commit_is_portrait = TouchOverlayIsPortraitForView(self);
  xe::hid::touch::IOSTouchControlDefinition& match_control = controls[selected_control_index_];
  xe::hid::touch::IOSTouchLayoutSpace position_space =
      TouchControlPositionSpaceForControlType(self, match_control.type);
  xe::hid::touch::IOSTouchLayoutSpace size_space =
      TouchControlSizeSpaceForControlType(self, match_control.type);
  if (position_space.IsEmpty() || size_space.IsEmpty()) {
    return NO;
  }
  [self beginEditLayoutChangeIfNeeded];
  xe::hid::touch::IOSTouchRect& match_active_frame =
      xe::hid::touch::MutableActiveControlFrameForOrientation(match_control, commit_is_portrait);
  match_active_frame = NormalizedControlFrameFromResolvedFrame(candidate_frame, position_space,
                                                               size_space, match_control.type);
  const xe::hid::touch::IOSTouchRect committed_match_frame = match_active_frame;

  for (TouchCaptureState& capture : active_captures_) {
    if (capture.control_index == selected_control_index_) {
      capture.normalized_frame_at_capture = committed_match_frame;
      capture.anchor_point = capture.current_point;
    }
  }

  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self setNeedsLayout];
  [self layoutIfNeeded];
  [self applyCaptureVisualState];
  [self finishEditLayoutChangeIfNeeded];
  return YES;
}

- (BOOL)matchSelectedControlToControlIndex:(NSUInteger)match_index
                                     width:(BOOL)match_width
                                    height:(BOOL)match_height {
  if (match_index == NSNotFound || selected_control_index_ == NSNotFound ||
      selected_control_index_ >= resolved_control_frames_.size() ||
      match_index >= resolved_control_frames_.size() || match_index == selected_control_index_) {
    return NO;
  }

  [self clearEditSnapGuides];
  const xe::hid::touch::IOSTouchRect& selected_frame =
      resolved_control_frames_[selected_control_index_];
  const xe::hid::touch::IOSTouchRect& match_frame = resolved_control_frames_[match_index];
  xe::hid::touch::IOSTouchRect candidate = selected_frame;
  const float center_x = selected_frame.x + selected_frame.width * 0.5f;
  const float center_y = selected_frame.y + selected_frame.height * 0.5f;
  if (match_width) {
    candidate.width = match_frame.width;
    candidate.x = center_x - candidate.width * 0.5f;
  }
  if (match_height) {
    candidate.height = match_frame.height;
    candidate.y = center_y - candidate.height * 0.5f;
  }

  if (![self commitSelectedControlResolvedFrame:candidate]) {
    return NO;
  }
  if (runtime_model_ && match_index < runtime_model_->layout().controls.size()) {
    NSString* label =
        XeniaTouchConfiguredControlLabelText(runtime_model_->layout().controls[match_index], YES);
    CGPoint anchor = CGPointMake(candidate.x + candidate.width * 0.5f, candidate.y);
    [self setEditSnapFeedbackText:[NSString stringWithFormat:@"Matched %@", label ?: @"target"]
                           anchor:anchor];
    [self updateEditSnapGuidesPath];
  }
  [self playSnapHaptic];
  return YES;
}

- (void)matchSelectedControlToNearestSiblingWidth:(BOOL)match_width height:(BOOL)match_height {
  NSUInteger match_index = [self nearestSizeMatchControlIndexForSelectedControl];
  [self matchSelectedControlToControlIndex:match_index width:match_width height:match_height];
}

- (void)matchSelectedControlSizeToNearestSibling {
  [self matchSelectedControlToNearestSiblingWidth:YES height:YES];
}

- (CGRect)preferredEditChromeFrameForSafeArea:(const xe::hid::touch::IOSTouchLayoutSpace&)safe_area
                                        width:(CGFloat)chrome_width
                                       height:(CGFloat)chrome_height {
  const CGFloat chrome_margin = 14.0f;
  const auto candidates =
      EditChromeDockCandidateFrames(safe_area, chrome_margin, chrome_width, chrome_height);

  if (!runtime_model_) {
    return candidates[0];
  }

  const auto& controls = runtime_model_->layout().controls;
  CGFloat best_penalty = CGFLOAT_MAX;
  CGRect best_frame = candidates[0];
  for (CGRect candidate : candidates) {
    CGFloat penalty = 0.0f;
    for (NSUInteger control_index = 0;
         control_index < MIN(static_cast<NSUInteger>(controls.size()),
                             static_cast<NSUInteger>(resolved_control_frames_.size()));
         ++control_index) {
      if (controls[control_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
        continue;
      }
      CGRect control_frame = CGRectFromTouchRect(resolved_control_frames_[control_index]);
      CGRect intersection = CGRectIntersection(candidate, control_frame);
      if (CGRectIsNull(intersection) || CGRectIsEmpty(intersection)) {
        continue;
      }
      CGFloat weight = 1.0f;
      if (control_index == selected_control_index_) {
        weight = 7.0f;
      } else if (control_index == pause_control_index_ ||
                 controls[control_index].type ==
                     xe::hid::touch::IOSTouchControlType::kPauseButton ||
                 controls[control_index].action == xe::hid::touch::IOSTouchAction::kStart ||
                 controls[control_index].action == xe::hid::touch::IOSTouchAction::kBack ||
                 controls[control_index].action == xe::hid::touch::IOSTouchAction::kPauseMenu) {
        weight = 4.0f;
      }
      penalty += CGRectGetWidth(intersection) * CGRectGetHeight(intersection) * weight;
    }
    if (!edit_command_bar_.hidden) {
      CGRect intersection = CGRectIntersection(candidate, edit_command_bar_.frame);
      if (!CGRectIsNull(intersection) && !CGRectIsEmpty(intersection)) {
        penalty += CGRectGetWidth(intersection) * CGRectGetHeight(intersection) * 8.0f;
      }
    }
    if (penalty < best_penalty) {
      best_penalty = penalty;
      best_frame = candidate;
    }
  }
  return best_frame;
}

- (CGFloat)editCommandBarCollisionPenaltyForFrame:(CGRect)candidate
                                         safeArea:
                                             (const xe::hid::touch::IOSTouchLayoutSpace&)safe_area {
  CGFloat penalty = 0.0f;
  if (runtime_model_) {
    const auto& controls = runtime_model_->layout().controls;
    const NSUInteger control_count = MIN(static_cast<NSUInteger>(controls.size()),
                                         static_cast<NSUInteger>(resolved_control_frames_.size()));
    for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
      if (controls[control_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
        continue;
      }
      CGRect control_frame =
          CGRectInset(CGRectFromTouchRect(resolved_control_frames_[control_index]), -8.0f, -6.0f);
      CGRect intersection = CGRectIntersection(candidate, control_frame);
      if (CGRectIsNull(intersection) || CGRectIsEmpty(intersection)) {
        continue;
      }
      CGFloat weight = 1.0f;
      if (control_index == selected_control_index_) {
        weight = 8.0f;
      } else if (control_index == pause_control_index_ ||
                 controls[control_index].type ==
                     xe::hid::touch::IOSTouchControlType::kPauseButton ||
                 controls[control_index].action == xe::hid::touch::IOSTouchAction::kStart ||
                 controls[control_index].action == xe::hid::touch::IOSTouchAction::kBack ||
                 controls[control_index].action == xe::hid::touch::IOSTouchAction::kPauseMenu) {
        weight = 5.0f;
      }
      penalty += CGRectGetWidth(intersection) * CGRectGetHeight(intersection) * weight;
    }
  }

  const CGFloat top_y = safe_area.origin_y + 10.0f;
  penalty += MAX(0.0f, CGRectGetMinY(candidate) - top_y) * 0.65f;
  return penalty;
}

- (CGRect)preferredEditCommandBarFrameForSafeArea:
              (const xe::hid::touch::IOSTouchLayoutSpace&)safe_area
                                            width:(CGFloat)bar_width
                                           height:(CGFloat)bar_height {
  const CGFloat margin = 10.0f;
  const CGFloat min_x = safe_area.origin_x + margin;
  const CGFloat max_x = MAX(min_x, safe_area.origin_x + safe_area.width - bar_width - margin);
  const CGFloat min_y = safe_area.origin_y + margin;
  const CGFloat max_y = MAX(min_y, safe_area.origin_y + safe_area.height - bar_height - margin);
  const CGFloat center_x =
      std::clamp<CGFloat>(safe_area.origin_x + (safe_area.width - bar_width) * 0.5f, min_x, max_x);
  const CGFloat mid_y =
      std::clamp<CGFloat>(safe_area.origin_y + safe_area.height * 0.36f, min_y, max_y);

  std::vector<CGRect> candidates = {
      CGRectMake(center_x, min_y, bar_width, bar_height),
      CGRectMake(min_x, min_y, bar_width, bar_height),
      CGRectMake(max_x, min_y, bar_width, bar_height),
      CGRectMake(center_x, mid_y, bar_width, bar_height),
      CGRectMake(min_x, mid_y, bar_width, bar_height),
      CGRectMake(max_x, mid_y, bar_width, bar_height),
      CGRectMake(center_x, max_y, bar_width, bar_height),
      CGRectMake(min_x, max_y, bar_width, bar_height),
      CGRectMake(max_x, max_y, bar_width, bar_height),
  };

  CGFloat best_penalty = CGFLOAT_MAX;
  CGRect best_frame = candidates[0];
  for (CGRect candidate : candidates) {
    CGFloat penalty = [self editCommandBarCollisionPenaltyForFrame:candidate safeArea:safe_area];
    if (penalty < best_penalty) {
      best_penalty = penalty;
      best_frame = candidate;
    }
  }
  return CGRectIntegral(best_frame);
}

- (CGRect)clampedEditChromeFrame:(CGRect)frame
                        safeArea:(const xe::hid::touch::IOSTouchLayoutSpace&)safe_area {
  const CGFloat chrome_margin = 14.0f;
  const CGFloat min_x = safe_area.origin_x + chrome_margin;
  const CGFloat max_x =
      MAX(min_x, safe_area.origin_x + safe_area.width - CGRectGetWidth(frame) - chrome_margin);
  const CGFloat min_y = safe_area.origin_y + chrome_margin;
  const CGFloat max_y =
      MAX(min_y, safe_area.origin_y + safe_area.height - CGRectGetHeight(frame) - chrome_margin);
  frame.origin.x = std::clamp<CGFloat>(CGRectGetMinX(frame), min_x, max_x);
  frame.origin.y = std::clamp<CGFloat>(CGRectGetMinY(frame), min_y, max_y);
  frame.origin.x = std::round(frame.origin.x);
  frame.origin.y = std::round(frame.origin.y);
  return frame;
}

- (CGRect)resolvedEditChromeFrameForSafeArea:(const xe::hid::touch::IOSTouchLayoutSpace&)safe_area
                                       width:(CGFloat)chrome_width
                                      height:(CGFloat)chrome_height {
  if (edit_chrome_drag_active_) {
    CGRect drag_frame = edit_chrome_drag_frame_;
    drag_frame.size = CGSizeMake(chrome_width, chrome_height);
    return [self clampedEditChromeFrame:drag_frame safeArea:safe_area];
  }
  if (edit_chrome_has_free_frame_) {
    CGRect free_frame = edit_chrome_free_frame_;
    free_frame.size = CGSizeMake(chrome_width, chrome_height);
    return [self clampedEditChromeFrame:free_frame safeArea:safe_area];
  }
  return [self preferredEditChromeFrameForSafeArea:safe_area
                                             width:chrome_width
                                            height:chrome_height];
}

- (CGRect)editChromeHeaderDragFrame {
  if (edit_chrome_.hidden) {
    return CGRectZero;
  }
  CGRect drag_frame = edit_chrome_.frame;
  drag_frame.size.height = MIN(CGRectGetHeight(drag_frame), 80.0f);
  return drag_frame;
}

- (void)clearEditChromeDragState {
  edit_chrome_drag_active_ = NO;
  edit_chrome_drag_touch_ = nil;
  edit_chrome_drag_touch_offset_ = CGPointZero;
}

- (void)clearEditSnapGuides {
  active_snap_vertical_guides_.clear();
  active_snap_horizontal_guides_.clear();
  [self setEditSnapFeedbackText:nil anchor:CGPointZero];
  [self updateEditSnapGuidesPath];
}

- (void)positionEditSnapFeedbackLabel {
  if (edit_snap_feedback_label_.hidden || edit_snap_feedback_label_.text.length == 0) {
    return;
  }
  CGSize label_size = [edit_snap_feedback_label_ sizeThatFits:CGSizeMake(220.0, 40.0)];
  label_size.width = ceil(MIN(MAX(label_size.width, 74.0), 220.0));
  label_size.height = ceil(MIN(MAX(label_size.height, 26.0), 40.0));
  const CGFloat margin = 10.0;
  CGFloat x = edit_snap_feedback_anchor_.x - label_size.width * 0.5;
  CGFloat y = edit_snap_feedback_anchor_.y - label_size.height - 12.0;
  if (y < margin) {
    y = edit_snap_feedback_anchor_.y + 12.0;
  }
  x = MIN(MAX(x, margin), CGRectGetWidth(self.bounds) - label_size.width - margin);
  y = MIN(MAX(y, margin), CGRectGetHeight(self.bounds) - label_size.height - margin);
  edit_snap_feedback_label_.frame =
      CGRectIntegral(CGRectMake(x, y, label_size.width, label_size.height));
}

- (void)setEditSnapFeedbackText:(NSString*)text anchor:(CGPoint)anchor {
  edit_snap_feedback_anchor_ = anchor;
  edit_snap_feedback_label_.text = text;
  edit_snap_feedback_label_.hidden = text.length == 0;
  [self positionEditSnapFeedbackLabel];
}

- (NSString*)matchSizePickerPrompt {
  return @"Tap target for size";
}

- (void)cancelMatchSizePicker {
  edit_match_size_picker_active_ = NO;
}

- (void)beginMatchSizePicker {
  if (!editing_controls_enabled_ || !runtime_model_ || selected_control_index_ == NSNotFound) {
    return;
  }
  if (edit_match_size_picker_active_) {
    [self cancelMatchSizePicker];
    [self clearEditSnapGuides];
    [self refreshEditChromeSelection];
    return;
  }
  const NSUInteger match_index = [self nearestSizeMatchControlIndexForSelectedControl];
  if (match_index == NSNotFound) {
    return;
  }

  edit_match_size_picker_active_ = YES;
  CGPoint anchor = CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
  if (selected_control_index_ < resolved_control_frames_.size()) {
    const auto& selected_frame = resolved_control_frames_[selected_control_index_];
    anchor = CGPointMake(selected_frame.x + selected_frame.width * 0.5f, selected_frame.y);
  }
  [self clearEditSnapGuides];
  [self setEditSnapFeedbackText:[self matchSizePickerPrompt] anchor:anchor];
  [self updateEditSnapGuidesPath];
  [self refreshEditChromeSelection];
  [self playSelectionHaptic];
}

- (void)updateEditSnapGuidesPath {
  xe::hid::touch::IOSTouchLayoutSpace layout_space = TouchLayoutSpaceForView(self);
  const bool has_feedback = edit_snap_feedback_label_.text.length > 0;
  const bool guides_empty = !editing_controls_enabled_ || layout_space.IsEmpty() ||
                            (active_snap_vertical_guides_.empty() &&
                             active_snap_horizontal_guides_.empty() && !has_feedback);

  // Keep the overlay/layer non-hidden while in edit mode and animate opacity
  // instead of toggling hidden so the guides fade in/out smoothly as
  // the user drags a control past alignment positions, rather than flashing
  // in/out instantly.
  edit_snap_guides_overlay_.hidden = !editing_controls_enabled_;
  edit_snap_guides_layer_.hidden = !editing_controls_enabled_;

  if (!guides_empty) {
    CGMutablePathRef guide_path = CGPathCreateMutable();
    if (guide_path) {
      for (CGFloat x : active_snap_vertical_guides_) {
        CGPathMoveToPoint(guide_path, nullptr, x, layout_space.origin_y);
        CGPathAddLineToPoint(guide_path, nullptr, x, layout_space.origin_y + layout_space.height);
      }
      for (CGFloat y : active_snap_horizontal_guides_) {
        CGPathMoveToPoint(guide_path, nullptr, layout_space.origin_x, y);
        CGPathAddLineToPoint(guide_path, nullptr, layout_space.origin_x + layout_space.width, y);
      }
      edit_snap_guides_layer_.path = guide_path;
      CGPathRelease(guide_path);
    } else {
      edit_snap_guides_layer_.path = nullptr;
    }
  } else {
    edit_snap_guides_layer_.path = nullptr;
  }

  [CATransaction begin];
  [CATransaction setAnimationDuration:0.10];
  [CATransaction setAnimationTimingFunction:[CAMediaTimingFunction
                                                functionWithName:kCAMediaTimingFunctionEaseOut]];
  edit_snap_guides_layer_.opacity = guides_empty ? 0.0f : 1.0f;
  [CATransaction commit];
  [UIView animateWithDuration:0.10
      animations:^{
        edit_snap_feedback_label_.alpha =
            (!guides_empty && edit_snap_feedback_label_.text.length) ? 1.0 : 0.0;
      }
      completion:^(__unused BOOL finished) {
        edit_snap_feedback_label_.hidden =
            guides_empty || edit_snap_feedback_label_.text.length == 0;
      }];

  // Rising-edge snap haptic: only fire when guides become visible (a fresh
  // alignment engagement), not on every drag tick that keeps them present.
  const BOOL guides_visible_now =
      !active_snap_vertical_guides_.empty() || !active_snap_horizontal_guides_.empty();
  if (guides_visible_now && !snap_guides_were_visible_) {
    [self playSnapHaptic];
  }
  snap_guides_were_visible_ = guides_visible_now;
}

- (void)clearEditPinchState {
  edit_pinch_active_ = NO;
  edit_pinch_control_index_ = NSNotFound;
  edit_pinch_touch_a_ = nil;
  edit_pinch_touch_b_ = nil;
  edit_pinch_initial_distance_ = 0.0f;
  edit_pinch_initial_frame_ = {};
}

- (void)clearLookMotionState {
  std::fill(recent_look_vectors_.begin(), recent_look_vectors_.end(), CGPointZero);
  std::fill(recent_look_motion_times_.begin(), recent_look_motion_times_.end(), 0.0);
  std::fill(recent_move_vectors_.begin(), recent_move_vectors_.end(), CGPointZero);
  std::fill(recent_move_motion_times_.begin(), recent_move_motion_times_.end(), 0.0);
}

- (void)clearLookMotionStateForControlIndex:(NSUInteger)control_index {
  if (control_index >= recent_look_vectors_.size() ||
      control_index >= recent_look_motion_times_.size() ||
      control_index >= recent_move_vectors_.size() ||
      control_index >= recent_move_motion_times_.size()) {
    return;
  }
  recent_look_vectors_[control_index] = CGPointZero;
  recent_look_motion_times_[control_index] = 0.0;
  recent_move_vectors_[control_index] = CGPointZero;
  recent_move_motion_times_[control_index] = 0.0;
}

- (void)storeAnalogMotion:(CGPoint)vector
                   output:(xe::hid::touch::IOSTouchAnalogOutput)output
          forControlIndex:(NSUInteger)control_index
                   atTime:(CFTimeInterval)current_time
                   tuning:(const xe::hid::touch::IOSTouchAnalogTuning&)tuning {
  auto smooth_vector = [&](CGPoint previous_vector, CFTimeInterval previous_time) {
    const float smoothing = std::clamp(tuning.smoothing, 0.0f, 0.95f);
    if (smoothing <= 0.001f || previous_time <= 0.0 ||
        current_time - previous_time >= TouchLookHoldSeconds()) {
      return vector;
    }
    const float sample_count =
        std::clamp(static_cast<float>((current_time - previous_time) * 60.0), 0.25f, 4.0f);
    const float retain_previous = std::pow(smoothing, sample_count);
    return CGPointMake(previous_vector.x * retain_previous + vector.x * (1.0f - retain_previous),
                       previous_vector.y * retain_previous + vector.y * (1.0f - retain_previous));
  };

  switch (output) {
    case xe::hid::touch::IOSTouchAnalogOutput::kLook:
      if (control_index >= recent_look_vectors_.size() ||
          control_index >= recent_look_motion_times_.size()) {
        return;
      }
      recent_look_vectors_[control_index] = smooth_vector(recent_look_vectors_[control_index],
                                                          recent_look_motion_times_[control_index]);
      recent_look_motion_times_[control_index] = current_time;
      return;
    case xe::hid::touch::IOSTouchAnalogOutput::kMove:
      if (control_index >= recent_move_vectors_.size() ||
          control_index >= recent_move_motion_times_.size()) {
        return;
      }
      recent_move_vectors_[control_index] = smooth_vector(recent_move_vectors_[control_index],
                                                          recent_move_motion_times_[control_index]);
      recent_move_motion_times_[control_index] = current_time;
      return;
    case xe::hid::touch::IOSTouchAnalogOutput::kNone:
    default:
      return;
  }
}

- (NSString*)snapFeedbackTextForControlIndex:(NSUInteger)control_index
                                snappedFrame:(const xe::hid::touch::IOSTouchRect&)frame
                                 gestureMode:(TouchCaptureState::EditGestureMode)gesture_mode {
  if (!runtime_model_ || control_index == NSNotFound) {
    return nil;
  }
  const auto& controls = runtime_model_->layout().controls;
  const NSUInteger control_count = MIN(static_cast<NSUInteger>(controls.size()),
                                       static_cast<NSUInteger>(resolved_control_frames_.size()));
  if (control_index >= control_count) {
    return nil;
  }
  const CGFloat epsilon = 1.5;
  const CGFloat frame_center_x = frame.x + frame.width * 0.5f;
  const CGFloat frame_center_y = frame.y + frame.height * 0.5f;

  for (NSUInteger peer_index = 0; peer_index < control_count; ++peer_index) {
    if (peer_index == control_index ||
        controls[peer_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
      continue;
    }
    const auto& peer = resolved_control_frames_[peer_index];
    NSString* label = XeniaTouchConfiguredControlLabelText(controls[peer_index], YES);
    if (gesture_mode == TouchCaptureState::EditGestureMode::kResize) {
      const BOOL same_width = std::abs(peer.width - frame.width) <= epsilon;
      const BOOL same_height = std::abs(peer.height - frame.height) <= epsilon;
      if (same_width && same_height) {
        return [NSString stringWithFormat:@"Same size as %@", label ?: @"peer"];
      }
      if (same_width) {
        return [NSString stringWithFormat:@"Same width as %@", label ?: @"peer"];
      }
      if (same_height) {
        return [NSString stringWithFormat:@"Same height as %@", label ?: @"peer"];
      }
      continue;
    }

    const CGFloat peer_center_x = peer.x + peer.width * 0.5f;
    const CGFloat peer_center_y = peer.y + peer.height * 0.5f;
    const BOOL vertical_align = std::abs(peer.x - frame.x) <= epsilon ||
                                std::abs(peer_center_x - frame_center_x) <= epsilon ||
                                std::abs(peer.x + peer.width - (frame.x + frame.width)) <= epsilon;
    const BOOL horizontal_align =
        std::abs(peer.y - frame.y) <= epsilon ||
        std::abs(peer_center_y - frame_center_y) <= epsilon ||
        std::abs(peer.y + peer.height - (frame.y + frame.height)) <= epsilon;
    if (vertical_align && horizontal_align) {
      return [NSString stringWithFormat:@"Aligned with %@", label ?: @"peer"];
    }
    if (vertical_align) {
      return [NSString stringWithFormat:@"Same X as %@", label ?: @"peer"];
    }
    if (horizontal_align) {
      return [NSString stringWithFormat:@"Same Y as %@", label ?: @"peer"];
    }
  }
  return nil;
}

- (xe::hid::touch::IOSTouchRect)
    snappedResolvedFrameForControlIndex:(NSUInteger)control_index
                         candidateFrame:(const xe::hid::touch::IOSTouchRect&)candidate_frame
                               safeArea:(const xe::hid::touch::IOSTouchLayoutSpace&)safe_area
                            gestureMode:(TouchCaptureState::EditGestureMode)gesture_mode
                    preserveAspectRatio:(BOOL)preserve_aspect_ratio
                         preserveCenter:(BOOL)preserve_center {
  const auto& controls = runtime_model_->layout().controls;
  if (control_index >= controls.size()) {
    return candidate_frame;
  }
  const auto& control = controls[control_index];
  xe::hid::touch::IOSTouchLayoutSpace size_space =
      TouchControlSizeSpaceForControlType(self, control.type);
  if (safe_area.IsEmpty() || size_space.IsEmpty()) {
    return candidate_frame;
  }

  TouchEditSnapOptions options;
  options.grid_enabled = edit_grid_enabled_;
  options.grid_spacing = kEditGridSpacingPoints;
  options.move_snap_threshold =
      edit_grid_enabled_ ? kEditGridMoveSnapThresholdPoints : kEditMoveSnapThresholdPoints;
  options.resize_snap_threshold =
      edit_grid_enabled_ ? kEditGridResizeSnapThresholdPoints : kEditResizeSnapThresholdPoints;
  options.canonical_control_sizes = kEditCanonicalControlSizes;
  options.canonical_control_size_count =
      sizeof(kEditCanonicalControlSizes) / sizeof(kEditCanonicalControlSizes[0]);
  TouchEditSnapResult result = SnapTouchEditResolvedFrame(
      control_index, controls, resolved_control_frames_, candidate_frame, safe_area, size_space,
      gesture_mode == TouchCaptureState::EditGestureMode::kMove ? TouchEditGestureMode::kMove
                                                                : TouchEditGestureMode::kResize,
      preserve_aspect_ratio, preserve_center, options);
  active_snap_vertical_guides_ = std::move(result.vertical_guides);
  active_snap_horizontal_guides_ = std::move(result.horizontal_guides);
  const bool has_guides =
      !active_snap_vertical_guides_.empty() || !active_snap_horizontal_guides_.empty();
  CGPoint feedback_anchor = CGPointMake(result.frame.x + result.frame.width * 0.5f, result.frame.y);
  [self setEditSnapFeedbackText:(has_guides ? [self snapFeedbackTextForControlIndex:control_index
                                                                       snappedFrame:result.frame
                                                                        gestureMode:gesture_mode]
                                            : nil)
                         anchor:feedback_anchor];
  return result.frame;
}

- (BOOL)tryBeginEditPinchWithTouch:(UITouch*)touch atPoint:(CGPoint)point {
  if (!editing_controls_enabled_ || !runtime_model_ || edit_pinch_active_ ||
      selected_control_index_ == NSNotFound ||
      selected_control_index_ >= runtime_model_->layout().controls.size() ||
      selected_control_index_ >= resolved_control_frames_.size()) {
    return NO;
  }

  const auto& controls = runtime_model_->layout().controls;
  const auto& control = controls[selected_control_index_];
  if (!TouchControlContainsPoint(control, resolved_control_frames_[selected_control_index_],
                                 point)) {
    return NO;
  }

  const NSUInteger selected_control_index = selected_control_index_;
  auto primary_capture_it =
      std::find_if(active_captures_.begin(), active_captures_.end(),
                   [selected_control_index](const TouchCaptureState& capture) {
                     return capture.control_index == selected_control_index &&
                            capture.edit_gesture_mode == TouchCaptureState::EditGestureMode::kMove;
                   });
  if (primary_capture_it == active_captures_.end()) {
    return NO;
  }

  CGPoint primary_point = [primary_capture_it->touch locationInView:self];
  const CGFloat initial_distance = std::hypot(primary_point.x - point.x, primary_point.y - point.y);
  if (initial_distance < 18.0f) {
    return NO;
  }

  [self beginEditLayoutChangeIfNeeded];
  edit_pinch_active_ = YES;
  edit_pinch_control_index_ = selected_control_index_;
  edit_pinch_touch_a_ = primary_capture_it->touch;
  edit_pinch_touch_b_ = touch;
  edit_pinch_initial_distance_ = initial_distance;
  // Pinch always operates on the orientation currently being edited; the
  // commit path (updatePinchedControlFrame) writes back through the same
  // orientation choke point.
  const bool pinch_begin_is_portrait = TouchOverlayIsPortraitForView(self);
  edit_pinch_initial_frame_ = xe::hid::touch::ActiveControlFrameForOrientation(
      controls[selected_control_index_], pinch_begin_is_portrait);
  primary_capture_it->current_point = primary_point;
  primary_capture_it->normalized_frame_at_capture = edit_pinch_initial_frame_;
  [self clearEditSnapGuides];
  return YES;
}

- (void)endEditPinchRetainingTouch:(UITouch*)remaining_touch {
  const NSUInteger pinch_control_index = edit_pinch_control_index_;
  xe::hid::touch::IOSTouchRect current_frame = {};
  if (runtime_model_ && pinch_control_index != NSNotFound &&
      pinch_control_index < runtime_model_->layout().controls.size()) {
    const bool pinch_end_is_portrait = TouchOverlayIsPortraitForView(self);
    current_frame = xe::hid::touch::ActiveControlFrameForOrientation(
        runtime_model_->layout().controls[pinch_control_index], pinch_end_is_portrait);
  }

  [self clearEditPinchState];
  [self clearEditSnapGuides];

  if (!remaining_touch || !runtime_model_ || pinch_control_index == NSNotFound ||
      pinch_control_index >= runtime_model_->layout().controls.size()) {
    return;
  }

  CGPoint remaining_point = [remaining_touch locationInView:self];
  auto capture_it = std::find_if(active_captures_.begin(), active_captures_.end(),
                                 [remaining_touch](const TouchCaptureState& capture) {
                                   return capture.touch == remaining_touch;
                                 });
  if (capture_it != active_captures_.end()) {
    capture_it->control_index = pinch_control_index;
    capture_it->anchor_point = remaining_point;
    capture_it->current_point = remaining_point;
    capture_it->began_time = CACurrentMediaTime();
    capture_it->last_motion_time = capture_it->began_time;
    capture_it->normalized_frame_at_capture = current_frame;
    capture_it->edit_gesture_mode = TouchCaptureState::EditGestureMode::kMove;
    return;
  }

  TouchCaptureState capture;
  capture.touch = remaining_touch;
  capture.control_index = pinch_control_index;
  capture.anchor_point = remaining_point;
  capture.current_point = remaining_point;
  capture.began_time = CACurrentMediaTime();
  capture.last_motion_time = capture.began_time;
  capture.normalized_frame_at_capture = current_frame;
  capture.edit_gesture_mode = TouchCaptureState::EditGestureMode::kMove;
  active_captures_.push_back(capture);
}

- (void)updatePinchedControlFrame {
  if (!editing_controls_enabled_ || !runtime_model_ || !edit_pinch_active_ ||
      edit_pinch_control_index_ == NSNotFound ||
      edit_pinch_control_index_ >= runtime_model_->layout().controls.size()) {
    return;
  }

  xe::hid::touch::IOSTouchControlDefinition& pinch_control =
      runtime_model_->mutable_layout().controls[edit_pinch_control_index_];
  xe::hid::touch::IOSTouchLayoutSpace position_space =
      TouchControlPositionSpaceForControlType(self, pinch_control.type);
  xe::hid::touch::IOSTouchLayoutSpace size_space =
      TouchControlSizeSpaceForControlType(self, pinch_control.type);
  if (position_space.IsEmpty() || size_space.IsEmpty() || edit_pinch_initial_distance_ <= 0.0f) {
    return;
  }

  CGPoint point_a = [edit_pinch_touch_a_ locationInView:self];
  CGPoint point_b = [edit_pinch_touch_b_ locationInView:self];
  const CGFloat current_distance = std::hypot(point_a.x - point_b.x, point_a.y - point_b.y);
  const float scale =
      std::clamp(static_cast<float>(current_distance / edit_pinch_initial_distance_), 0.45f, 2.75f);
  xe::hid::touch::IOSTouchRect initial_resolved = ResolveNormalizedControlFrame(
      edit_pinch_initial_frame_, position_space, size_space, pinch_control.type);
  const float center_x = initial_resolved.x + initial_resolved.width * 0.5f;
  const float center_y = initial_resolved.y + initial_resolved.height * 0.5f;
  xe::hid::touch::IOSTouchRect candidate = initial_resolved;
  candidate.width *= scale;
  candidate.height *= scale;
  candidate.x = center_x - candidate.width * 0.5f;
  candidate.y = center_y - candidate.height * 0.5f;

  xe::hid::touch::IOSTouchRect snapped =
      [self snappedResolvedFrameForControlIndex:edit_pinch_control_index_
                                 candidateFrame:candidate
                                       safeArea:position_space
                                    gestureMode:TouchCaptureState::EditGestureMode::kResize
                            preserveAspectRatio:YES
                                 preserveCenter:YES];
  [self beginEditLayoutChangeIfNeeded];
  const bool pinch_commit_is_portrait = TouchOverlayIsPortraitForView(self);
  xe::hid::touch::IOSTouchRect& pinch_active_frame =
      xe::hid::touch::MutableActiveControlFrameForOrientation(pinch_control,
                                                              pinch_commit_is_portrait);
  pinch_active_frame = NormalizedControlFrameFromResolvedFrame(snapped, position_space, size_space,
                                                               pinch_control.type);
  const xe::hid::touch::IOSTouchRect committed_pinch_frame = pinch_active_frame;

  UITouch* pinch_touch_a = edit_pinch_touch_a_;
  auto capture_it = std::find_if(
      active_captures_.begin(), active_captures_.end(),
      [pinch_touch_a](const TouchCaptureState& capture) { return capture.touch == pinch_touch_a; });
  if (capture_it != active_captures_.end()) {
    capture_it->normalized_frame_at_capture = committed_pinch_frame;
    capture_it->current_point = point_a;
  }
}

- (void)updateConflictHighlights {
  const NSUInteger control_count =
      MIN(control_views_.count, static_cast<NSUInteger>(resolved_control_frames_.size()));
  conflicting_control_indices_.assign(control_count, false);

  if (!runtime_model_ || !editing_controls_enabled_) {
    for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
      [[control_views_ objectAtIndex:control_index] setConflictHighlighted:NO];
    }
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  for (NSUInteger left_index = 0; left_index < control_count; ++left_index) {
    if (controls[left_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
      continue;
    }
    CGRect left_frame = CGRectFromTouchRect(resolved_control_frames_[left_index]);
    for (NSUInteger right_index = left_index + 1; right_index < control_count; ++right_index) {
      if (controls[right_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
        continue;
      }
      CGRect right_frame = CGRectFromTouchRect(resolved_control_frames_[right_index]);
      if (CGRectIntersectsRect(left_frame, right_frame)) {
        conflicting_control_indices_[left_index] = true;
        conflicting_control_indices_[right_index] = true;
      }
    }
  }

  for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
    [[control_views_ objectAtIndex:control_index]
        setConflictHighlighted:conflicting_control_indices_[control_index]];
  }
}

- (void)selectControlWithIdentifier:(const std::string&)identifier {
  if (!runtime_model_) {
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  for (NSUInteger control_index = 0; control_index < controls.size(); ++control_index) {
    if (controls[control_index].identifier == identifier) {
      [self setSelectedControlIndex:control_index];
      return;
    }
  }
}

- (void)setSelectedControlIndex:(NSUInteger)selected_control_index {
  const BOOL changed = selected_control_index_ != selected_control_index;
  if (changed) {
    [self cancelMatchSizePicker];
  }
  selected_control_index_ = selected_control_index;
  if (changed && editing_controls_enabled_) {
    [self clearEditChromeDragState];
  }
  [self clearEditSnapGuides];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self applyCaptureVisualState];
  if (changed && editing_controls_enabled_ && selected_control_index != NSNotFound) {
    [self playSelectionHaptic];
  }
}

- (void)resetInteractionState {
  active_captures_.clear();
  [self cancelMatchSizePicker];
  [self clearEditChromeDragState];
  [self clearEditPinchState];
  [self clearEditSnapGuides];
  std::fill(recent_action_press_times_.begin(), recent_action_press_times_.end(), 0.0);
  std::fill(recent_action_suppressed_until_times_.begin(),
            recent_action_suppressed_until_times_.end(), 0.0);
  std::fill(recent_secondary_press_times_.begin(), recent_secondary_press_times_.end(), 0.0);
  std::fill(recent_secondary_candidate_times_.begin(), recent_secondary_candidate_times_.end(),
            0.0);
  [self clearLookMotionState];
  move_knob_.hidden = YES;
  for (XeniaTouchControlShellView* control_view in control_views_) {
    [control_view setTouchActive:NO];
  }
}

- (void)syncControlViewDefinitions {
  if (!runtime_model_) {
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  const NSUInteger control_count =
      MIN(control_views_.count, static_cast<NSUInteger>(controls.size()));
  for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
    [[control_views_ objectAtIndex:control_index] applyControlDefinition:controls[control_index]];
  }
  [self refreshEditPreview];
}

- (void)updateControlBehaviorAnnotations {
  const BOOL visible =
      editing_controls_enabled_ && edit_wiring_visible_ && !edit_showing_layout_library_;
  for (NSUInteger control_index = 0; control_index < control_views_.count; ++control_index) {
    XeniaTouchControlShellView* control_view = [control_views_ objectAtIndex:control_index];
    [control_view setBehaviorAnnotationsVisible:visible];
  }
}

- (BOOL)isEditingControlsEnabled {
  return editing_controls_enabled_;
}

- (void)setEditingControlsEnabled:(BOOL)enabled animated:(BOOL)animated {
  if (editing_controls_enabled_ == enabled) {
    return;
  }

  editing_controls_enabled_ = enabled;
  edit_opacity_slider_active_ = NO;
  [self resetInteractionState];
  if (enabled) {
    [self resetEditLayoutHistory];
    edit_grid_enabled_ = YES;
    edit_wiring_visible_ = YES;
    edit_chrome_has_free_frame_ = NO;
    [self clearEditChromeDragState];
    if (selected_control_index_ == NSNotFound && runtime_model_) {
      const auto& controls = runtime_model_->layout().controls;
      if (!controls.empty()) {
        [self
            setSelectedControlIndex:(move_control_index_ != NSNotFound ? move_control_index_ : 0)];
      }
    } else {
      [self refreshEditChromeSelection];
      [self refreshEditPreview];
    }
    edit_grid_overlay_.hidden = !edit_grid_enabled_;
    edit_grid_overlay_.alpha = edit_grid_enabled_ ? 1.0 : 0.0;
    edit_safe_area_guide_.hidden = NO;
    edit_chrome_.hidden = NO;
    edit_command_bar_.hidden = NO;
    edit_snap_guides_overlay_.hidden = YES;
    [self updateControlBehaviorAnnotations];
    [self seedEditLayoutHistoryIfNeeded];
  } else {
    [self resetEditLayoutHistory];
    edit_showing_layout_library_ = NO;
    edit_wiring_visible_ = NO;
    edit_chrome_has_free_frame_ = NO;
    [self clearEditChromeDragState];
    [self updateControlBehaviorAnnotations];
  }
  // First-responder dance so hardware-keyboard keyCommands (Command-Z,
  // Shift-Command-Z, Command-D, Command-Delete, Command-M, Esc) route here only
  // while the editor is active.
  if (enabled) {
    [self becomeFirstResponder];
  } else {
    [self resignFirstResponder];
  }
  [self setNeedsLayout];
  [self publishResolvedState];

  if (!animated) {
    edit_chrome_.alpha = enabled ? 1.0 : 0.0;
    edit_command_bar_.alpha = enabled ? 1.0 : 0.0;
    edit_safe_area_guide_.alpha = enabled ? 1.0 : 0.0;
    edit_snap_guides_overlay_.alpha = enabled ? 1.0 : 0.0;
    edit_grid_overlay_.hidden = !enabled || !edit_grid_enabled_;
    edit_safe_area_guide_.hidden = !enabled;
    edit_chrome_.hidden = !enabled;
    edit_command_bar_.hidden = !enabled;
    edit_snap_guides_overlay_.hidden = YES;
    return;
  }

  if (enabled) {
    edit_chrome_.alpha = 0.0;
    edit_command_bar_.alpha = 0.0;
    edit_safe_area_guide_.alpha = 0.0;
    edit_grid_overlay_.alpha = 0.0;
  }
  [UIView animateWithDuration:0.18
      animations:^{
        edit_chrome_.alpha = enabled ? 1.0 : 0.0;
        edit_command_bar_.alpha = enabled ? 1.0 : 0.0;
        edit_safe_area_guide_.alpha = enabled ? 1.0 : 0.0;
        edit_grid_overlay_.alpha = (enabled && edit_grid_enabled_) ? 1.0 : 0.0;
      }
      completion:^(__unused BOOL finished) {
        if (!enabled) {
          edit_snap_guides_overlay_.hidden = YES;
          edit_grid_overlay_.hidden = YES;
          edit_safe_area_guide_.hidden = YES;
          edit_chrome_.hidden = YES;
          edit_command_bar_.hidden = YES;
        }
      }];
}

- (void)adjustSelectedControlSizeByScale:(float)scale {
  if (!editing_controls_enabled_ || !runtime_model_ || scale <= 0.0f) {
    return;
  }

  auto& controls = runtime_model_->mutable_layout().controls;
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= controls.size()) {
    return;
  }

  auto& control = controls[selected_control_index_];
  xe::hid::touch::IOSTouchLayoutSpace position_space =
      TouchControlPositionSpaceForControlType(self, control.type);
  xe::hid::touch::IOSTouchLayoutSpace size_space =
      TouchControlSizeSpaceForControlType(self, control.type);
  if (position_space.IsEmpty() || size_space.IsEmpty()) {
    return;
  }

  const bool size_adjust_is_portrait = TouchOverlayIsPortraitForView(self);
  const xe::hid::touch::IOSTouchRect size_active_source =
      xe::hid::touch::ActiveControlFrameForOrientation(control, size_adjust_is_portrait);
  const float center_x = size_active_source.x + size_active_source.width * 0.5f;
  const float center_y = size_active_source.y + size_active_source.height * 0.5f;
  xe::hid::touch::IOSTouchRect candidate = size_active_source;
  candidate.width *= scale;
  candidate.height *= scale;
  candidate.x = center_x - candidate.width * 0.5f;
  candidate.y = center_y - candidate.height * 0.5f;
  candidate = ClampNormalizedControlFrame(candidate, control.type);
  [self beginEditLayoutChangeIfNeeded];
  xe::hid::touch::IOSTouchRect& size_active_frame =
      xe::hid::touch::MutableActiveControlFrameForOrientation(control, size_adjust_is_portrait);
  size_active_frame = candidate;
  const xe::hid::touch::IOSTouchRect committed_size_frame = size_active_frame;

  for (TouchCaptureState& capture : active_captures_) {
    if (capture.control_index == selected_control_index_) {
      capture.normalized_frame_at_capture = committed_size_frame;
      capture.anchor_point = capture.current_point;
    }
  }
  if (active_captures_.empty() && !edit_pinch_active_) {
    [self clearEditSnapGuides];
  }

  [self syncControlViewDefinitions];
  [self setNeedsLayout];
  [self layoutIfNeeded];
  [self applyCaptureVisualState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)addNewActionButton {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  auto& layout = runtime_model_->mutable_layout();
  if (layout.controls.size() >= xe::hid::touch::kMaxIOSTouchControls) {
    return;
  }

  [self clearEditSnapGuides];
  [self beginEditLayoutChangeIfNeeded];
  std::string selected_identifier;
  if (!xe::hid::touch::AddSuggestedActionButtonToIOSTouchLayout(
          &layout, TouchOverlayIsPortraitForView(self), &selected_identifier)) {
    [self finishEditLayoutChangeIfNeeded];
    return;
  }
  [self refreshLayoutModel];
  [self selectControlWithIdentifier:selected_identifier];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)mirrorSelectedControlHorizontally {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& layout = runtime_model_->mutable_layout();
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= layout.controls.size()) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  if (!xe::hid::touch::MirrorIOSTouchLayoutControlHorizontally(
          &layout, selected_control_index_, TouchOverlayIsPortraitForView(self))) {
    [self finishEditLayoutChangeIfNeeded];
    return;
  }
  const auto& control = layout.controls[selected_control_index_];
  [self refreshLayoutModel];
  [self selectControlWithIdentifier:control.identifier];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)toggleSelectedControlMoveDpadRing {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& layout = runtime_model_->mutable_layout();
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= layout.controls.size()) {
    return;
  }

  auto& control = layout.controls[selected_control_index_];
  if (control.type != xe::hid::touch::IOSTouchControlType::kMoveStick) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  control.move_with_dpad_ring = !control.move_with_dpad_ring;
  [self syncControlViewDefinitions];
  [self refreshEditChromeSelection];
  [self refreshEditPreview];
  [self setNeedsLayout];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (BOOL)canDuplicateSelectedControl {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return NO;
  }

  const auto& layout = runtime_model_->layout();
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= layout.controls.size() ||
      layout.controls.size() >= xe::hid::touch::kMaxIOSTouchControls) {
    return NO;
  }

  const auto& control = layout.controls[selected_control_index_];
  return control.type == xe::hid::touch::IOSTouchControlType::kActionButton;
}

- (void)duplicateSelectedControl {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& layout = runtime_model_->mutable_layout();
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= layout.controls.size()) {
    return;
  }
  if (layout.controls.size() >= xe::hid::touch::kMaxIOSTouchControls) {
    return;
  }

  if (![self canDuplicateSelectedControl]) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  std::string selected_identifier;
  if (!xe::hid::touch::DuplicateIOSTouchLayoutActionButton(&layout, selected_control_index_,
                                                           TouchOverlayIsPortraitForView(self),
                                                           &selected_identifier)) {
    [self finishEditLayoutChangeIfNeeded];
    return;
  }
  [self refreshLayoutModel];
  [self selectControlWithIdentifier:selected_identifier];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)deleteSelectedControl {
  if (!editing_controls_enabled_ || !runtime_model_) {
    return;
  }

  [self clearEditSnapGuides];
  auto& layout = runtime_model_->mutable_layout();
  if (selected_control_index_ == NSNotFound || selected_control_index_ >= layout.controls.size()) {
    return;
  }
  if (layout.controls.size() <= 1) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  if (!xe::hid::touch::DeleteIOSTouchLayoutControl(&layout, selected_control_index_)) {
    [self finishEditLayoutChangeIfNeeded];
    return;
  }
  selected_control_index_ = NSNotFound;
  [self refreshLayoutModel];
  [self publishResolvedState];
  [self finishEditLayoutChangeIfNeeded];
}

- (void)updateControlFrameForCapture:(TouchCaptureState&)capture newPoint:(CGPoint)new_point {
  if (!editing_controls_enabled_ || !runtime_model_ || capture.control_index == NSNotFound) {
    return;
  }

  auto& controls = runtime_model_->mutable_layout().controls;
  if (capture.control_index >= controls.size()) {
    return;
  }
  xe::hid::touch::IOSTouchControlDefinition& drag_control = controls[capture.control_index];
  xe::hid::touch::IOSTouchLayoutSpace position_space =
      TouchControlPositionSpaceForControlType(self, drag_control.type);
  xe::hid::touch::IOSTouchLayoutSpace size_space =
      TouchControlSizeSpaceForControlType(self, drag_control.type);
  if (position_space.IsEmpty() || size_space.IsEmpty()) {
    return;
  }

  [self beginEditLayoutChangeIfNeeded];
  xe::hid::touch::IOSTouchRect next_frame = ResolveNormalizedControlFrame(
      capture.normalized_frame_at_capture, position_space, size_space, drag_control.type);
  if (capture.edit_gesture_mode == TouchCaptureState::EditGestureMode::kResize) {
    next_frame.width += static_cast<float>(new_point.x - capture.anchor_point.x);
    next_frame.height += static_cast<float>(new_point.y - capture.anchor_point.y);
  } else {
    next_frame.x += static_cast<float>(new_point.x - capture.anchor_point.x);
    next_frame.y += static_cast<float>(new_point.y - capture.anchor_point.y);
  }
  xe::hid::touch::IOSTouchRect snapped =
      [self snappedResolvedFrameForControlIndex:capture.control_index
                                 candidateFrame:next_frame
                                       safeArea:position_space
                                    gestureMode:capture.edit_gesture_mode
                            preserveAspectRatio:NO
                                 preserveCenter:NO];
  // Drag math operates on the orientation-resolved baseline that was
  // captured at touchesBegan; route the commit back through the same
  // orientation choke point so portrait drags only mutate the portrait
  // override (and lazily promote it the very first time).
  const bool drag_commit_is_portrait = TouchOverlayIsPortraitForView(self);
  xe::hid::touch::IOSTouchRect& drag_active_frame =
      xe::hid::touch::MutableActiveControlFrameForOrientation(drag_control,
                                                              drag_commit_is_portrait);
  drag_active_frame = NormalizedControlFrameFromResolvedFrame(snapped, position_space, size_space,
                                                              drag_control.type);
}

- (void)applyCaptureVisualState {
  if (!runtime_model_) {
    move_knob_.hidden = YES;
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  const NSUInteger control_count =
      MIN(control_views_.count, static_cast<NSUInteger>(controls.size()));
  if (visually_active_control_indices_.size() != control_count) {
    visually_active_control_indices_.resize(control_count);
  }
  std::fill(visually_active_control_indices_.begin(), visually_active_control_indices_.end(), 0);
  const TouchCaptureState* move_capture = nullptr;

  for (const TouchCaptureState& capture : active_captures_) {
    if (capture.control_index >= control_count) {
      continue;
    }
    visually_active_control_indices_[capture.control_index] = 1;
    if (capture.control_index == move_control_index_) {
      move_capture = &capture;
    }
  }

  if (editing_controls_enabled_ && selected_control_index_ != NSNotFound &&
      selected_control_index_ < control_count) {
    visually_active_control_indices_[selected_control_index_] = 1;
  }

  for (NSUInteger control_index = 0; control_index < control_views_.count; ++control_index) {
    const BOOL active =
        (control_index < control_count && visually_active_control_indices_[control_index]) ? YES
                                                                                           : NO;
    [[control_views_ objectAtIndex:control_index] setTouchActive:active];
  }

  if (editing_controls_enabled_) {
    move_knob_.hidden = YES;
    return;
  }

  if (!move_capture || move_control_index_ == NSNotFound ||
      move_control_index_ >= resolved_control_frames_.size()) {
    move_knob_.hidden = YES;
    return;
  }

  const xe::hid::touch::IOSTouchRect& move_frame = resolved_control_frames_[move_control_index_];
  const xe::hid::touch::IOSTouchControlDefinition& move_control = controls[move_control_index_];
  const CGFloat outer_radius =
      MIN(move_frame.width, move_frame.height) * MAX(move_control.activation_radius, 0.24f);
  CGPoint delta = CGPointMake(move_capture->current_point.x - move_capture->anchor_point.x,
                              move_capture->current_point.y - move_capture->anchor_point.y);
  const CGFloat distance = std::hypot(delta.x, delta.y);
  if (distance > outer_radius && distance > 0.0f) {
    const CGFloat scale = outer_radius / distance;
    delta.x *= scale;
    delta.y *= scale;
  }

  const CGFloat knob_size = MIN(move_frame.width, move_frame.height) * 0.28f;
  move_knob_.bounds = CGRectMake(0.0f, 0.0f, knob_size, knob_size);
  move_knob_.center =
      CGPointMake(move_capture->anchor_point.x + delta.x, move_capture->anchor_point.y + delta.y);
  move_knob_.layer.cornerRadius = knob_size * 0.5f;
  move_knob_.hidden = NO;
}

- (void)publishResolvedState {
  if (!runtime_model_) {
    return;
  }

  xe::hid::touch::IOSTouchResolvedState state = {};
  state.gameplay_enabled = gameplay_overlay_active_ && !editing_controls_enabled_;

  if (editing_controls_enabled_) {
    if (!xe::hid::touch::TouchStatesEqualIgnoringPacket(state, last_published_state_)) {
      state.packet_number = next_packet_number_++;
      if (next_packet_number_ == 0) {
        next_packet_number_ = 1;
      }
      last_published_state_ = state;
    } else {
      state.packet_number = last_published_state_.packet_number;
    }

    runtime_model_->StoreResolvedState(state);
    [self applyCaptureVisualState];
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  const NSUInteger control_count =
      MIN(control_views_.count, static_cast<NSUInteger>(controls.size()));
  const CFTimeInterval current_time = CACurrentMediaTime();
  float held_look_scale = 1.0f;
  float held_move_scale = 1.0f;

  for (const TouchCaptureState& capture : active_captures_) {
    if (capture.control_index >= control_count ||
        capture.control_index >= resolved_control_frames_.size()) {
      continue;
    }
    const xe::hid::touch::IOSTouchControlDefinition& control = controls[capture.control_index];
    if (control.type != xe::hid::touch::IOSTouchControlType::kActionButton) {
      continue;
    }
    const xe::hid::touch::IOSTouchRect& frame = resolved_control_frames_[capture.control_index];
    const TouchInteractionBehaviorState secondary_behavior_state =
        ResolveTouchInteractionBehaviorState(control.secondary_behavior, capture, current_time);
    const bool touch_active = control.hold_while_captured || secondary_behavior_state.active ||
                              TouchControlContainsPoint(control, frame, capture.current_point);
    if (!touch_active) {
      continue;
    }
    held_look_scale *= std::clamp(control.held_look_scale, 0.25f, 4.0f);
    held_move_scale *= std::clamp(control.held_move_scale, 0.25f, 4.0f);
  }
  held_look_scale = std::clamp(held_look_scale, 0.1f, 4.0f);
  held_move_scale = std::clamp(held_move_scale, 0.1f, 4.0f);

  for (const TouchCaptureState& capture : active_captures_) {
    if (capture.control_index >= control_count ||
        capture.control_index >= resolved_control_frames_.size()) {
      continue;
    }

    const xe::hid::touch::IOSTouchControlDefinition& control = controls[capture.control_index];
    const xe::hid::touch::IOSTouchRect& frame = resolved_control_frames_[capture.control_index];

    switch (control.type) {
      case xe::hid::touch::IOSTouchControlType::kMoveStick: {
        // Move + D-Pad combo: a touch that landed on one of the four arrow
        // zones drives the corresponding D-Pad bit (press-and-hold while the
        // finger remains on the arrow) instead of the analog stick. The
        // sub-zone is decided once at touchesBegan and pinned for the
        // capture's lifetime, so a finger drifting toward the centre doesn't
        // silently turn into a stick deflection.
        switch (capture.combo_subzone) {
          case TouchCaptureState::ComboSubzone::kDpadUp:
            state.buttons |= xe::hid::X_INPUT_GAMEPAD_DPAD_UP;
            continue;
          case TouchCaptureState::ComboSubzone::kDpadDown:
            state.buttons |= xe::hid::X_INPUT_GAMEPAD_DPAD_DOWN;
            continue;
          case TouchCaptureState::ComboSubzone::kDpadLeft:
            state.buttons |= xe::hid::X_INPUT_GAMEPAD_DPAD_LEFT;
            continue;
          case TouchCaptureState::ComboSubzone::kDpadRight:
            state.buttons |= xe::hid::X_INPUT_GAMEPAD_DPAD_RIGHT;
            continue;
          case TouchCaptureState::ComboSubzone::kStick:
          case TouchCaptureState::ComboSubzone::kNone:
            break;
        }
        const CGPoint move_unit = MoveStickUnitVectorForCapture(control, frame, capture);
        if (!CGPointEqualToPoint(move_unit, CGPointZero)) {
          const float normalized_x = static_cast<float>(move_unit.x);
          const float normalized_y = static_cast<float>(move_unit.y);
          // Move-style controls can be configured with action=kLook to drive
          // the right thumbstick (FPS aim) rather than the left thumbstick
          // (movement). The visual + capture pipeline is identical; only the
          // output target changes. Per-axis analog_tuning was already applied
          // by MoveStickUnitVectorForCapture.
          if (control.action == xe::hid::touch::IOSTouchAction::kLook) {
            state.thumb_rx = xe::hid::touch::TouchAxisFromUnit(normalized_x * held_look_scale);
            state.thumb_ry = xe::hid::touch::TouchAxisFromUnit(-normalized_y * held_look_scale);
          } else {
            state.thumb_lx = xe::hid::touch::TouchAxisFromUnit(normalized_x * held_move_scale);
            state.thumb_ly = xe::hid::touch::TouchAxisFromUnit(-normalized_y * held_move_scale);
          }
        }
      } break;

      case xe::hid::touch::IOSTouchControlType::kActionButton: {
        const TouchInteractionBehaviorState secondary_behavior_state =
            ResolveTouchInteractionBehaviorState(control.secondary_behavior, capture, current_time);
        const bool touch_active = control.hold_while_captured || secondary_behavior_state.active ||
                                  TouchControlContainsPoint(control, frame, capture.current_point);
        const bool primary_suppressed =
            capture.control_index < recent_action_suppressed_until_times_.size() &&
            current_time < recent_action_suppressed_until_times_[capture.control_index];
        if (touch_active) {
          if (!primary_suppressed && !xe::hid::touch::TouchControlUsesDeferredPrimaryTap(control)) {
            xe::hid::touch::ApplyTouchActionMapping(control, &state);
          }
          if (secondary_behavior_state.active &&
              control.secondary_behavior.action != xe::hid::touch::IOSTouchAction::kNone) {
            xe::hid::touch::ApplyTouchActionMappingForAction(control.secondary_behavior.action,
                                                             &state);
          }
          if (!control.hold_while_captured && !primary_suppressed &&
              !xe::hid::touch::TouchControlUsesDeferredPrimaryTap(control) &&
              capture.control_index < recent_action_press_times_.size()) {
            recent_action_press_times_[capture.control_index] = current_time;
          }
        }
      } break;

      case xe::hid::touch::IOSTouchControlType::kLookSwipeZone:
      case xe::hid::touch::IOSTouchControlType::kPauseButton:
      default:
        break;
    }
  }

  const float button_tap_hold_seconds = TouchButtonTapHoldSeconds();
  for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
    const xe::hid::touch::IOSTouchControlDefinition& control = controls[control_index];
    if (control.type != xe::hid::touch::IOSTouchControlType::kActionButton ||
        control.hold_while_captured || control_index >= recent_action_press_times_.size()) {
      continue;
    }

    if ((current_time - recent_action_press_times_[control_index]) < button_tap_hold_seconds) {
      if (control_index < recent_action_suppressed_until_times_.size() &&
          current_time < recent_action_suppressed_until_times_[control_index]) {
        continue;
      }
      xe::hid::touch::ApplyTouchActionMapping(control, &state);
    }
  }

  for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
    const xe::hid::touch::IOSTouchControlDefinition& control = controls[control_index];
    if (control.secondary_behavior.action == xe::hid::touch::IOSTouchAction::kNone ||
        control_index >= recent_secondary_press_times_.size()) {
      continue;
    }

    if ((current_time - recent_secondary_press_times_[control_index]) < button_tap_hold_seconds) {
      xe::hid::touch::ApplyTouchActionMappingForAction(control.secondary_behavior.action, &state);
    }
  }

  // Accumulate analog drag contributions across every control with a recent
  // motion entry instead of picking only the freshest. Per-axis max-magnitude
  // blending lets several controls contribute while still allowing the
  // per-control hold/decay tail to keep low-FPS gameplay polls observing
  // transient motions.
  const float look_hold_seconds = TouchLookHoldSeconds();
  const NSUInteger look_state_count =
      MIN(control_count, static_cast<NSUInteger>(recent_look_motion_times_.size()));
  CGPoint accumulated_right_thumb = CGPointZero;
  for (NSUInteger control_index = 0; control_index < look_state_count; ++control_index) {
    const CFTimeInterval motion_time = recent_look_motion_times_[control_index];
    if (motion_time <= 0.0) {
      continue;
    }
    const CFTimeInterval look_motion_age = current_time - motion_time;
    if (look_motion_age >= look_hold_seconds) {
      continue;
    }
    const float decay =
        std::clamp(1.0f - static_cast<float>(look_motion_age / look_hold_seconds), 0.0f, 1.0f);
    const CGPoint vector = recent_look_vectors_[control_index];
    BlendMaxMagnitude(
        CGPointMake(vector.x * decay * held_look_scale, vector.y * decay * held_look_scale),
        &accumulated_right_thumb);
  }
  const NSUInteger move_state_count =
      MIN(control_count, static_cast<NSUInteger>(recent_move_motion_times_.size()));
  CGPoint accumulated_left_thumb = CGPointZero;
  bool any_left_swipe = false;
  for (NSUInteger control_index = 0; control_index < move_state_count; ++control_index) {
    const CFTimeInterval motion_time = recent_move_motion_times_[control_index];
    if (motion_time <= 0.0) {
      continue;
    }
    const CFTimeInterval move_motion_age = current_time - motion_time;
    if (move_motion_age >= look_hold_seconds) {
      continue;
    }
    const float decay =
        std::clamp(1.0f - static_cast<float>(move_motion_age / look_hold_seconds), 0.0f, 1.0f);
    const CGPoint vector = recent_move_vectors_[control_index];
    const CGPoint decayed_vector =
        CGPointMake(vector.x * decay * held_move_scale, vector.y * decay * held_move_scale);
    BlendMaxMagnitude(decayed_vector, &accumulated_left_thumb);
    if (decayed_vector.x != 0.0 || decayed_vector.y != 0.0) {
      any_left_swipe = true;
    }
  }
  // Look output: max-magnitude blend swipe contribution with whatever the
  // per-capture loop above already wrote (e.g. a MoveStick configured with
  // action=kLook).
  const int16_t swipe_rx =
      xe::hid::touch::TouchAxisFromUnit(static_cast<float>(accumulated_right_thumb.x));
  const int16_t swipe_ry =
      xe::hid::touch::TouchAxisFromUnit(static_cast<float>(accumulated_right_thumb.y));
  if (std::abs(static_cast<int>(swipe_rx)) > std::abs(static_cast<int>(state.thumb_rx))) {
    state.thumb_rx = swipe_rx;
  }
  if (std::abs(static_cast<int>(swipe_ry)) > std::abs(static_cast<int>(state.thumb_ry))) {
    state.thumb_ry = swipe_ry;
  }
  // Move output via swipe (LookSwipeZone configured with action=kMove): only
  // override the per-capture left thumb if a swipe contribution actually
  // exists, so we don't stomp on a normal MoveStick capture.
  if (any_left_swipe) {
    const int16_t swipe_lx =
        xe::hid::touch::TouchAxisFromUnit(static_cast<float>(accumulated_left_thumb.x));
    const int16_t swipe_ly =
        xe::hid::touch::TouchAxisFromUnit(static_cast<float>(accumulated_left_thumb.y));
    if (std::abs(static_cast<int>(swipe_lx)) > std::abs(static_cast<int>(state.thumb_lx))) {
      state.thumb_lx = swipe_lx;
    }
    if (std::abs(static_cast<int>(swipe_ly)) > std::abs(static_cast<int>(state.thumb_ly))) {
      state.thumb_ly = swipe_ly;
    }
  }

  if (!xe::hid::touch::TouchStatesEqualIgnoringPacket(state, last_published_state_)) {
    state.packet_number = next_packet_number_++;
    if (next_packet_number_ == 0) {
      next_packet_number_ = 1;
    }
    last_published_state_ = state;
  } else {
    state.packet_number = last_published_state_.packet_number;
  }

  runtime_model_->StoreResolvedState(state);
  [self applyCaptureVisualState];
}

- (void)setGameplayOverlayVisible:(BOOL)visible animated:(BOOL)animated {
  gameplay_overlay_active_ = visible;

  if (visible) {
    if (self.hidden) {
      self.alpha = 0.0;
    }
    self.hidden = NO;
    self.userInteractionEnabled = YES;
    display_link_.paused = NO;
    [self publishResolvedState];
    if (!animated) {
      self.alpha = 1.0;
      return;
    }
    // Spring-style entrance: subtle bounce so the overlay arrives with
    // physical weight rather than a flat fade.
    UIViewPropertyAnimator* show_animator = [[UIViewPropertyAnimator alloc] initWithDuration:0.35
                                                                                dampingRatio:0.85
                                                                                  animations:^{
                                                                                    self.alpha =
                                                                                        1.0;
                                                                                  }];
    [show_animator startAnimation];
    [show_animator release];
    return;
  }

  display_link_.paused = YES;
  self.userInteractionEnabled = NO;
  [self resetInteractionState];
  [self publishResolvedState];

  if (!animated || self.hidden) {
    self.hidden = YES;
    self.alpha = 0.0;
    return;
  }

  UIViewPropertyAnimator* hide_animator = [[UIViewPropertyAnimator alloc] initWithDuration:0.22
                                                                              dampingRatio:1.0
                                                                                animations:^{
                                                                                  self.alpha = 0.0;
                                                                                }];
  [hide_animator addCompletion:^(__unused UIViewAnimatingPosition position) {
    self.hidden = YES;
  }];
  [hide_animator startAnimation];
  [hide_animator release];
}

- (void)displayLinkFired:(CADisplayLink*)__unused display_link {
  [self publishResolvedState];
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  if (self.hidden || self.alpha <= 0.01f || !self.userInteractionEnabled ||
      !gameplay_overlay_active_) {
    return nil;
  }

  if (editing_controls_enabled_) {
    if (edit_showing_layout_library_) {
      UIView* chrome_hit = [edit_chrome_ interactiveHitTestForOverlayPoint:point
                                                                     event:event
                                                                    inView:self];
      if (chrome_hit) {
        return chrome_hit;
      }
      if (CGRectContainsPoint(edit_chrome_.frame, point)) {
        return edit_chrome_;
      }
      return nil;
    }
    if (!edit_command_bar_.hidden && edit_command_bar_.alpha > 0.01f) {
      CGPoint command_point = [edit_command_bar_ convertPoint:point fromView:self];
      UIView* command_hit = [edit_command_bar_ hitTest:command_point withEvent:event];
      if (command_hit) {
        return command_hit;
      }
    }
    UIView* chrome_hit = [edit_chrome_ interactiveHitTestForOverlayPoint:point
                                                                   event:event
                                                                  inView:self];
    if (chrome_hit) {
      return chrome_hit;
    }
    if (CGRectContainsPoint([self selectedControlResizeHandleFrame], point)) {
      return self;
    }
    if (CGRectContainsPoint([self editChromeHeaderDragFrame], point)) {
      return self;
    }
    if (CGRectContainsPoint(edit_chrome_.frame, point)) {
      return edit_chrome_;
    }
  }

  if (!editing_controls_enabled_ && pause_button_ && !pause_button_.hidden) {
    CGPoint pause_point = [pause_button_ convertPoint:point fromView:self];
    UIView* pause_hit = [pause_button_ hitTest:pause_point withEvent:event];
    if (pause_hit) {
      return pause_hit;
    }
  }

  if (!runtime_model_) {
    return nil;
  }

  const auto& controls = runtime_model_->layout().controls;
  const NSUInteger control_count =
      MIN(control_views_.count, static_cast<NSUInteger>(controls.size()));
  for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
    if (control_index >= resolved_control_frames_.size() ||
        (!editing_controls_enabled_ && control_index == pause_control_index_)) {
      continue;
    }
    if (TouchControlContainsPoint(controls[control_index], resolved_control_frames_[control_index],
                                  point)) {
      return self;
    }
  }

  return nil;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)__unused event {
  if (!runtime_model_ || !gameplay_overlay_active_) {
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  const NSUInteger control_count =
      MIN(control_views_.count, static_cast<NSUInteger>(controls.size()));
  for (UITouch* touch in touches) {
    CGPoint point = [touch locationInView:self];
    if (editing_controls_enabled_ && CGRectContainsPoint([self editChromeHeaderDragFrame], point)) {
      if (!edit_pinch_active_ && active_captures_.empty()) {
        edit_chrome_drag_active_ = YES;
        edit_chrome_drag_touch_ = touch;
        edit_chrome_drag_frame_ = edit_chrome_.frame;
        edit_chrome_drag_touch_offset_ = CGPointMake(point.x - CGRectGetMinX(edit_chrome_.frame),
                                                     point.y - CGRectGetMinY(edit_chrome_.frame));
        [self clearEditSnapGuides];
      }
      continue;
    }
    if (editing_controls_enabled_ && edit_pinch_active_) {
      continue;
    }
    if (editing_controls_enabled_ && [self tryBeginEditPinchWithTouch:touch atPoint:point]) {
      continue;
    }
    if (editing_controls_enabled_ && selected_control_index_ != NSNotFound &&
        CGRectContainsPoint([self selectedControlResizeHandleFrame], point)) {
      TouchCaptureState capture;
      capture.touch = touch;
      capture.control_index = selected_control_index_;
      capture.anchor_point = point;
      capture.current_point = point;
      capture.began_time = CACurrentMediaTime();
      capture.last_motion_time = capture.began_time;
      const bool resize_capture_is_portrait = TouchOverlayIsPortraitForView(self);
      capture.normalized_frame_at_capture = xe::hid::touch::ActiveControlFrameForOrientation(
          controls[capture.control_index], resize_capture_is_portrait);
      capture.edit_gesture_mode = TouchCaptureState::EditGestureMode::kResize;
      [self beginEditLayoutChangeIfNeeded];
      active_captures_.push_back(capture);
      [self clearEditSnapGuides];
      continue;
    }

    NSInteger best_control_index = -1;
    uint8_t best_priority = 0;

    for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
      if (control_index >= resolved_control_frames_.size() ||
          (!editing_controls_enabled_ && control_index == pause_control_index_) ||
          [self isControlIndexCaptured:control_index]) {
        continue;
      }

      const xe::hid::touch::IOSTouchControlDefinition& control = controls[control_index];
      if (!TouchControlContainsPoint(control, resolved_control_frames_[control_index], point)) {
        continue;
      }

      if (best_control_index < 0 || control.capture_priority > best_priority) {
        best_control_index = static_cast<NSInteger>(control_index);
        best_priority = control.capture_priority;
      }
    }

    if (best_control_index < 0) {
      continue;
    }

    if (editing_controls_enabled_ && edit_match_size_picker_active_) {
      const NSUInteger target_index = static_cast<NSUInteger>(best_control_index);
      if (target_index == selected_control_index_ ||
          controls[target_index].type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone) {
        CGPoint anchor = point;
        [self setEditSnapFeedbackText:@"Tap another button" anchor:anchor];
        [self updateEditSnapGuidesPath];
        continue;
      }

      [self matchSelectedControlToControlIndex:target_index width:YES height:YES];
      [self cancelMatchSizePicker];
      [self refreshEditChromeSelection];
      continue;
    }

    TouchCaptureState capture;
    capture.touch = touch;
    capture.control_index = static_cast<NSUInteger>(best_control_index);
    capture.anchor_point = point;
    capture.current_point = point;
    capture.began_time = CACurrentMediaTime();
    capture.last_motion_time = capture.began_time;
    // Capture the orientation-active frame so subsequent drag math walks
    // off the right baseline. The drag commit (updateControlFrameForCapture)
    // writes back through MutableActiveControlFrameForOrientation to
    // preserve the per-orientation split.
    const bool capture_is_portrait = TouchOverlayIsPortraitForView(self);
    capture.normalized_frame_at_capture = xe::hid::touch::ActiveControlFrameForOrientation(
        controls[capture.control_index], capture_is_portrait);
    capture.edit_gesture_mode = TouchCaptureState::EditGestureMode::kMove;
    // For Move + D-Pad combos: pin the touch to one of {stick, up, down, left,
    // right} at touchesBegan and keep it there for the capture's lifetime. The
    // publish path uses this to decide between analog stick output and a
    // discrete D-Pad bit. For non-combo controls the helper returns kStick
    // (treated as default-MoveStick / no-op for other types).
    capture.combo_subzone = TouchComboSubzoneForPoint(
        controls[capture.control_index], resolved_control_frames_[capture.control_index], point);
    if (!editing_controls_enabled_ &&
        controls[capture.control_index].type ==
            xe::hid::touch::IOSTouchControlType::kActionButton &&
        capture.control_index < recent_action_press_times_.size()) {
      const CFTimeInterval tap_tail_age =
          capture.began_time - recent_action_press_times_[capture.control_index];
      if (tap_tail_age >= 0.0 && tap_tail_age < TouchButtonTapHoldSeconds()) {
        recent_action_press_times_[capture.control_index] = 0.0;
        if (capture.control_index < recent_action_suppressed_until_times_.size()) {
          recent_action_suppressed_until_times_[capture.control_index] =
              capture.began_time + kTouchButtonRetapReleaseGapSeconds;
        }
      }
    }
    if (editing_controls_enabled_) {
      [self clearEditSnapGuides];
      [self setSelectedControlIndex:capture.control_index];
      [self beginEditLayoutChangeIfNeeded];
    } else {
      // Press haptic on gameplay captures only; editor drags use the
      // selection haptic via setSelectedControlIndex above. Look swipe zones
      // are continuous and would buzz constantly, so they do not trigger.
      switch (controls[capture.control_index].type) {
        case xe::hid::touch::IOSTouchControlType::kActionButton:
        case xe::hid::touch::IOSTouchControlType::kPauseButton:
          [self playPressHaptic];
          break;
        case xe::hid::touch::IOSTouchControlType::kMoveStick:
          // D-Pad arrow taps on a combo control feel like a button press,
          // not a stick engage. Use the medium press haptic for arrow zones
          // and reserve the lighter haptic for the analog stick centre.
          if (capture.combo_subzone == TouchCaptureState::ComboSubzone::kDpadUp ||
              capture.combo_subzone == TouchCaptureState::ComboSubzone::kDpadDown ||
              capture.combo_subzone == TouchCaptureState::ComboSubzone::kDpadLeft ||
              capture.combo_subzone == TouchCaptureState::ComboSubzone::kDpadRight) {
            [self playPressHaptic];
          } else {
            [self playLightPressHaptic];
          }
          break;
        case xe::hid::touch::IOSTouchControlType::kLookSwipeZone:
        default:
          break;
      }
    }
    active_captures_.push_back(capture);
  }

  [self publishResolvedState];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)__unused event {
  if (!runtime_model_ || !gameplay_overlay_active_) {
    return;
  }

  const auto& controls = runtime_model_->layout().controls;
  const CFTimeInterval current_time = CACurrentMediaTime();
  for (UITouch* touch in touches) {
    if (editing_controls_enabled_ && edit_chrome_drag_active_ && touch == edit_chrome_drag_touch_) {
      xe::hid::touch::IOSTouchLayoutSpace safe_area = TouchSafeAreaSpaceForView(self);
      if (!safe_area.IsEmpty()) {
        CGPoint new_point = [touch locationInView:self];
        edit_chrome_drag_frame_.origin =
            CGPointMake(new_point.x - edit_chrome_drag_touch_offset_.x,
                        new_point.y - edit_chrome_drag_touch_offset_.y);
        edit_chrome_drag_frame_ = [self clampedEditChromeFrame:edit_chrome_drag_frame_
                                                      safeArea:safe_area];
        [self setNeedsLayout];
        [self layoutIfNeeded];
      }
      continue;
    }
    if (editing_controls_enabled_ && edit_pinch_active_ &&
        (touch == edit_pinch_touch_a_ || touch == edit_pinch_touch_b_)) {
      [self updatePinchedControlFrame];
      continue;
    }

    auto capture_it =
        std::find_if(active_captures_.begin(), active_captures_.end(),
                     [touch](const TouchCaptureState& capture) { return capture.touch == touch; });
    if (capture_it == active_captures_.end() || capture_it->control_index >= controls.size()) {
      continue;
    }

    CGPoint new_point = [touch locationInView:self];
    if (editing_controls_enabled_) {
      [self updateControlFrameForCapture:*capture_it newPoint:new_point];
      capture_it->current_point = new_point;
      continue;
    }

    const xe::hid::touch::IOSTouchControlDefinition& control = controls[capture_it->control_index];
    TouchCaptureState behavior_capture = *capture_it;
    behavior_capture.current_point = new_point;
    if (control.type == xe::hid::touch::IOSTouchControlType::kMoveStick &&
        control.secondary_behavior.trigger ==
            xe::hid::touch::IOSTouchInteractionTrigger::kDoubleTapForward &&
        capture_it->control_index < resolved_control_frames_.size() &&
        !capture_it->secondary_behavior_triggered &&
        [self hasPendingDoubleTapCandidateForControlIndex:capture_it->control_index
                                                   atTime:current_time] &&
        MoveStickCaptureQualifiesForDoubleTapForward(
            control, resolved_control_frames_[capture_it->control_index], behavior_capture,
            current_time)) {
      capture_it->secondary_behavior_triggered =
          [self consumeDoubleTapCandidateForControlIndex:capture_it->control_index
                                                  atTime:current_time];
    }
    const TouchInteractionBehaviorState secondary_behavior_state =
        ResolveTouchInteractionBehaviorState(control.secondary_behavior, behavior_capture,
                                             current_time);
    const CGPoint delta = CGPointMake(new_point.x - capture_it->current_point.x,
                                      new_point.y - capture_it->current_point.y);
    const CFTimeInterval elapsed_seconds =
        std::max(current_time - capture_it->last_motion_time, 1.0 / 240.0);
    const xe::hid::touch::IOSTouchAnalogOutput primary_output = EffectiveControlDragOutput(control);
    if (primary_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
      [self
          storeAnalogMotion:TouchAnalogVectorForDelta(delta, elapsed_seconds, control.analog_tuning)
                     output:primary_output
            forControlIndex:capture_it->control_index
                     atTime:current_time
                     tuning:control.analog_tuning];
    }
    if (secondary_behavior_state.active &&
        secondary_behavior_state.analog_output != xe::hid::touch::IOSTouchAnalogOutput::kNone) {
      [self storeAnalogMotion:TouchAnalogVectorForDelta(delta, elapsed_seconds,
                                                        secondary_behavior_state.analog_tuning)
                       output:secondary_behavior_state.analog_output
              forControlIndex:capture_it->control_index
                       atTime:current_time
                       tuning:secondary_behavior_state.analog_tuning];
    }
    capture_it->current_point = new_point;
    capture_it->last_motion_time = current_time;
  }

  if (editing_controls_enabled_) {
    [self setNeedsLayout];
    [self layoutIfNeeded];
    [self applyCaptureVisualState];
    [self publishResolvedState];
    return;
  }

  [self publishResolvedState];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)__unused event {
  [self finalizeTouches:touches cancelled:NO];
}

- (void)finalizeTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled {
  if (!runtime_model_) {
    return;
  }

  UITouch* pinch_remaining_touch = nil;
  const bool ended_pinch_a = edit_pinch_active_ && [touches containsObject:edit_pinch_touch_a_];
  const bool ended_pinch_b = edit_pinch_active_ && [touches containsObject:edit_pinch_touch_b_];
  if (edit_pinch_active_) {
    if (ended_pinch_a && !ended_pinch_b) {
      pinch_remaining_touch = edit_pinch_touch_b_;
    } else if (ended_pinch_b && !ended_pinch_a) {
      pinch_remaining_touch = edit_pinch_touch_a_;
    }
  }

  const auto& controls = runtime_model_->layout().controls;
  const CFTimeInterval current_time = CACurrentMediaTime();
  for (UITouch* touch in touches) {
    if (editing_controls_enabled_ && edit_chrome_drag_active_ && touch == edit_chrome_drag_touch_) {
      if (!cancelled) {
        xe::hid::touch::IOSTouchLayoutSpace safe_area = TouchSafeAreaSpaceForView(self);
        if (!safe_area.IsEmpty()) {
          edit_chrome_free_frame_ = [self clampedEditChromeFrame:edit_chrome_drag_frame_
                                                        safeArea:safe_area];
          edit_chrome_has_free_frame_ = YES;
        }
      }
      [self clearEditChromeDragState];
      [self setNeedsLayout];
      [self layoutIfNeeded];
      continue;
    }
    auto capture_it =
        std::find_if(active_captures_.begin(), active_captures_.end(),
                     [touch](const TouchCaptureState& capture) { return capture.touch == touch; });
    if (capture_it == active_captures_.end()) {
      continue;
    }
    const CGPoint release_point = [touch locationInView:self];
    capture_it->current_point = release_point;
    if (cancelled) {
      [self clearLookMotionStateForControlIndex:capture_it->control_index];
    }
    if (!cancelled && !editing_controls_enabled_ && capture_it->control_index < controls.size() &&
        capture_it->control_index < resolved_control_frames_.size()) {
      const NSUInteger control_index = capture_it->control_index;
      const xe::hid::touch::IOSTouchControlDefinition& control = controls[control_index];
      const xe::hid::touch::IOSTouchRect& frame = resolved_control_frames_[control_index];
      switch (control.type) {
        case xe::hid::touch::IOSTouchControlType::kActionButton: {
          const bool ended_inside = TouchControlContainsPoint(control, frame, release_point);
          if (xe::hid::touch::TouchControlUsesDeferredPrimaryTap(control) &&
              control_index < recent_action_press_times_.size()) {
            const TouchInteractionBehaviorState secondary_behavior_state =
                ResolveTouchInteractionBehaviorState(control.secondary_behavior, *capture_it,
                                                     current_time);
            if (ended_inside && !secondary_behavior_state.active) {
              recent_action_press_times_[control_index] = current_time;
            }
          } else if (ended_inside && !control.hold_while_captured &&
                     control_index < recent_action_press_times_.size()) {
            CFTimeInterval press_time = current_time;
            if (control_index < recent_action_suppressed_until_times_.size()) {
              press_time =
                  std::max(press_time, recent_action_suppressed_until_times_[control_index]);
            }
            recent_action_press_times_[control_index] = press_time;
          }

          const bool quick_tap = (current_time - capture_it->began_time) <=
                                 [self doubleTapWindowSecondsForControl:control];
          if (ended_inside && quick_tap &&
              control.secondary_behavior.trigger ==
                  xe::hid::touch::IOSTouchInteractionTrigger::kDoubleTap &&
              xe::hid::touch::TouchInteractionBehaviorConfigured(control.secondary_behavior)) {
            if (![self consumeDoubleTapCandidateForControlIndex:control_index
                                                         atTime:current_time]) {
              [self storeDoubleTapCandidateForControlIndex:control_index atTime:current_time];
            }
          }
        } break;

        case xe::hid::touch::IOSTouchControlType::kMoveStick: {
          if (control.secondary_behavior.trigger ==
                  xe::hid::touch::IOSTouchInteractionTrigger::kDoubleTapForward &&
              xe::hid::touch::TouchInteractionBehaviorConfigured(control.secondary_behavior) &&
              !capture_it->secondary_behavior_triggered &&
              MoveStickCaptureQualifiesForDoubleTapForward(control, frame, *capture_it,
                                                           current_time)) {
            if (![self consumeDoubleTapCandidateForControlIndex:control_index
                                                         atTime:current_time]) {
              [self storeDoubleTapCandidateForControlIndex:control_index atTime:current_time];
            }
          }
        } break;

        case xe::hid::touch::IOSTouchControlType::kLookSwipeZone:
        case xe::hid::touch::IOSTouchControlType::kPauseButton:
        default:
          break;
      }
    }
    active_captures_.erase(capture_it);
  }

  if (edit_pinch_active_ && (ended_pinch_a || ended_pinch_b)) {
    [self endEditPinchRetainingTouch:(cancelled ? nil : pinch_remaining_touch)];
  } else if (editing_controls_enabled_ && active_captures_.empty()) {
    [self clearEditSnapGuides];
  }

  if (editing_controls_enabled_) {
    [self finishEditLayoutChangeIfNeeded];
  }

  if (editing_controls_enabled_) {
    [self setNeedsLayout];
    [self layoutIfNeeded];
  }
  [self publishResolvedState];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)__unused event {
  [self finalizeTouches:touches cancelled:YES];
}

- (void)layoutSubviews {
  [super layoutSubviews];

  if (!runtime_model_) {
    return;
  }

  xe::hid::touch::IOSTouchLayoutSpace layout_space = TouchLayoutSpaceForView(self);
  xe::hid::touch::IOSTouchLayoutSpace safe_area = TouchSafeAreaSpaceForView(self);
  edit_snap_guides_overlay_.frame = self.bounds;
  edit_grid_overlay_.frame = CGRectIntegral(CGRectFromTouchRect(xe::hid::touch::IOSTouchRect{
      layout_space.origin_x, layout_space.origin_y, layout_space.width, layout_space.height}));
  edit_safe_area_guide_.frame = CGRectIntegral(CGRectFromTouchRect(xe::hid::touch::IOSTouchRect{
      layout_space.origin_x, layout_space.origin_y, layout_space.width, layout_space.height}));
  edit_snap_guides_layer_.frame = edit_snap_guides_overlay_.bounds;
  edit_grid_dots_layer_.frame = edit_grid_overlay_.bounds;
  if (editing_controls_enabled_ && edit_grid_enabled_ &&
      CGRectGetWidth(edit_grid_overlay_.bounds) > 0.0 &&
      CGRectGetHeight(edit_grid_overlay_.bounds) > 0.0) {
    CGMutablePathRef dot_path = CGPathCreateMutable();
    if (dot_path) {
      for (CGFloat y = kEditGridSpacingPoints * 0.5f;
           y < CGRectGetHeight(edit_grid_overlay_.bounds); y += kEditGridSpacingPoints) {
        for (CGFloat x = kEditGridSpacingPoints * 0.5f;
             x < CGRectGetWidth(edit_grid_overlay_.bounds); x += kEditGridSpacingPoints) {
          CGPathAddEllipseInRect(dot_path, nullptr,
                                 CGRectMake(x - kEditGridDotRadius, y - kEditGridDotRadius,
                                            kEditGridDotRadius * 2.0f, kEditGridDotRadius * 2.0f));
        }
      }
      edit_grid_dots_layer_.path = dot_path;
      CGPathRelease(dot_path);
    } else {
      edit_grid_dots_layer_.path = nullptr;
    }
  } else {
    edit_grid_dots_layer_.path = nullptr;
  }
  [self positionEditSnapFeedbackLabel];

  const auto& controls = runtime_model_->layout().controls;
  const NSUInteger control_count =
      MIN(control_views_.count, static_cast<NSUInteger>(controls.size()));
  resolved_control_frames_.resize(control_count);
  pause_button_.hidden = pause_control_index_ == NSNotFound || editing_controls_enabled_;
  // Single source of truth for the orientation lookup; every per-frame
  // resolve below feeds resolved_control_frames_, which is what hit-testing,
  // snap targets, the edit chrome, and publishResolvedState all read.
  const bool layout_is_portrait = TouchOverlayIsPortraitForView(self);
  const BOOL orientation_flipped =
      !last_layout_orientation_known_ ||
      static_cast<BOOL>(layout_is_portrait) != last_layout_was_portrait_;
  last_layout_was_portrait_ = static_cast<BOOL>(layout_is_portrait);
  last_layout_orientation_known_ = YES;
  for (NSUInteger control_index = 0; control_index < control_count; ++control_index) {
    XeniaTouchControlShellView* control_view = [control_views_ objectAtIndex:control_index];
    const xe::hid::touch::IOSTouchControlDefinition& control = controls[control_index];
    const xe::hid::touch::IOSTouchRect& active_frame =
        xe::hid::touch::ActiveControlFrameForOrientation(control, layout_is_portrait);
    xe::hid::touch::IOSTouchLayoutSpace position_space =
        TouchControlPositionSpaceForControlType(self, control.type);
    xe::hid::touch::IOSTouchLayoutSpace size_space =
        TouchControlSizeSpaceForControlType(self, control.type);
    xe::hid::touch::IOSTouchRect frame =
        ResolveNormalizedControlFrame(active_frame, position_space, size_space, control.type);
    resolved_control_frames_[control_index] = frame;
    control_view.frame = CGRectIntegral(CGRectFromTouchRect(frame));
    if (!editing_controls_enabled_ && control_index == pause_control_index_) {
      pause_button_.frame = control_view.frame;
      pause_button_.hidden = NO;
    }
    // Full-screen Look swipe zones go fully invisible during gameplay so the
    // player's view of the game isn't obscured. The shell still receives
    // touches; only the visible chrome is suppressed. In edit mode the user
    // needs to be able to see the zone they're configuring, so keep it
    // visible there.
    const bool is_fullscreen_look =
        control.type == xe::hid::touch::IOSTouchControlType::kLookSwipeZone &&
        active_frame.width >= 0.95f && active_frame.height >= 0.95f;
    [control_view setChromeSuppressed:(is_fullscreen_look && !editing_controls_enabled_)];
  }

  const CGFloat chrome_margin = 14.0f;

  const BOOL showing_layout_library = edit_showing_layout_library_;

  // The layout library is a modal editor surface. Keep the compact command strip
  // out of the visual and hit-test stack so it cannot overlap list rows or steal
  // touches from the library controls.
  edit_command_bar_.hidden = !editing_controls_enabled_ || showing_layout_library;
  edit_command_bar_.alpha = showing_layout_library ? 0.0 : 1.0;
  edit_command_bar_.userInteractionEnabled =
      editing_controls_enabled_ && !showing_layout_library;
  if (showing_layout_library) {
    edit_command_bar_.frame = CGRectZero;
  } else {
    const CGFloat command_bar_height = [XeniaTouchEditCommandBar preferredHeight];
    const CGFloat command_bar_width =
        MIN(MAX(0.0f, safe_area.width - chrome_margin * 2.0f), [edit_command_bar_ preferredWidth]);
    edit_command_bar_.frame = [self preferredEditCommandBarFrameForSafeArea:safe_area
                                                                      width:command_bar_width
                                                                     height:command_bar_height];
  }
  xe::hid::touch::IOSTouchLayoutSpace panel_space = safe_area;

  const BOOL spacious_chrome = safe_area.width >= 700.0f;
  const BOOL inspector_expanded = [edit_chrome_ isInspectorExpanded];
  const CGFloat expanded_chrome_width = showing_layout_library
                                            ? (spacious_chrome ? 520.0f : 430.0f)
                                            : (spacious_chrome ? 388.0f : 360.0f);
  const CGFloat collapsed_chrome_width = spacious_chrome ? 372.0f : 352.0f;
  const CGFloat target_chrome_width =
      showing_layout_library
          ? expanded_chrome_width
          : (inspector_expanded ? expanded_chrome_width : collapsed_chrome_width);
  const CGFloat chrome_width = MIN(safe_area.width - chrome_margin * 2.0f, target_chrome_width);
  const CGFloat chrome_height = [edit_chrome_ preferredHeightForWidth:chrome_width
                                                      availableHeight:panel_space.height
                                                               margin:chrome_margin];
  edit_chrome_.frame = [self resolvedEditChromeFrameForSafeArea:panel_space
                                                          width:chrome_width
                                                         height:chrome_height];
  [edit_chrome_ setNeedsLayout];
  [self bringSubviewToFront:edit_chrome_];
  if (!showing_layout_library) {
    [self bringSubviewToFront:edit_command_bar_];
  }

  if (showing_layout_library) {
    edit_resize_handle_.frame = CGRectZero;
    edit_resize_handle_.hidden = YES;
    [self updateConflictHighlights];
    [self updateEditSnapGuidesPath];
    [self applyCaptureVisualState];
    return;
  }

  edit_resize_handle_.frame = CGRectIntegral([self selectedControlResizeHandleFrame]);
  edit_resize_handle_.hidden = !editing_controls_enabled_ || selected_control_index_ == NSNotFound;

  [self updateConflictHighlights];
  [self updateEditSnapGuidesPath];
  [self applyCaptureVisualState];

  // After rotation, re-sync the edit chrome chip + the More-menu copy-action
  // label so they reflect the orientation now being edited. This is cheap
  // (UI text + UIMenu rebuild) and only fires when orientation actually
  // flips, not on every layout pass.
  if (orientation_flipped && editing_controls_enabled_) {
    [self refreshEditChromeSelection];
  }
}

- (void)pauseButtonPressed:(UIButton*)__unused sender {
  if (pauseHandler_) {
    pauseHandler_();
  }
}

- (void)touchOverlayEditChromeDidRequestSmallerControl:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self adjustSelectedControlSizeByScale:0.90f];
}

- (void)touchOverlayEditChromeDidRequestMatchNearestSize:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self beginMatchSizePicker];
}

- (void)touchOverlayEditChromeDidRequestLargerControl:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self adjustSelectedControlSizeByScale:1.10f];
}

- (void)touchOverlayEditChromeDidRequestRenameLabel:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self presentLabelRenameAlert];
}

- (void)touchOverlayEditChromeDidRequestDuplicateControl:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self duplicateSelectedControl];
}

- (void)toggleEditGrid {
  const BOOL enable_snap = !edit_grid_enabled_;
  xe::hid::touch::IOSTouchLayoutSpace safe_area = TouchSafeAreaSpaceForView(self);
  if (!safe_area.IsEmpty()) {
    edit_chrome_free_frame_ = [self clampedEditChromeFrame:edit_chrome_.frame safeArea:safe_area];
    edit_chrome_has_free_frame_ = YES;
  }
  edit_grid_enabled_ = enable_snap;
  if (editing_controls_enabled_ && edit_grid_enabled_) {
    edit_grid_overlay_.hidden = NO;
  }
  [UIView animateWithDuration:0.12
      animations:^{
        edit_grid_overlay_.alpha = (editing_controls_enabled_ && edit_grid_enabled_) ? 1.0 : 0.0;
      }
      completion:^(__unused BOOL finished) {
        edit_grid_overlay_.hidden = !editing_controls_enabled_ || !edit_grid_enabled_;
      }];
  [self refreshEditChromeSelection];
  [self setNeedsLayout];
}

- (void)touchOverlayEditChromeDidRequestDeleteControl:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self deleteSelectedControl];
}

- (void)touchOverlayEditChromeDidRequestLayoutLibrary:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  if (layoutLibraryHandler_) {
    layoutLibraryHandler_();
  }
}

- (void)touchOverlayEditChromeDidRequestHideLayoutLibrary:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self hideLayoutLibrary];
}

- (void)touchOverlayEditChromeDidRequestLayoutLibrarySaveCopy:
    (XeniaTouchOverlayEditChromeIOS*)__unused chrome {
  if (layoutLibrarySaveCopyHandler_) {
    layoutLibrarySaveCopyHandler_();
  }
}

- (void)touchOverlayEditChromeDidRequestLayoutLibraryImport:
    (XeniaTouchOverlayEditChromeIOS*)__unused chrome {
  if (layoutLibraryImportHandler_) {
    layoutLibraryImportHandler_();
  }
}

- (void)touchOverlayEditChromeDidRequestLayoutLibraryReset:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  if (layoutLibraryResetHandler_) {
    layoutLibraryResetHandler_();
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestLayoutLibraryLoad:(NSString*)localID {
  if (layoutLibraryLoadHandler_) {
    layoutLibraryLoadHandler_(localID);
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestLayoutLibraryRenameLocalID:(NSString*)localID {
  if (layoutLibraryRenameLayoutHandler_) {
    layoutLibraryRenameLayoutHandler_(localID);
  } else if (layoutLibraryRenameHandler_) {
    layoutLibraryRenameHandler_();
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestLayoutLibraryDeleteLocalID:(NSString*)localID {
  if (layoutLibraryDeleteLayoutHandler_) {
    layoutLibraryDeleteLayoutHandler_(localID);
  } else if (layoutLibraryDeleteHandler_) {
    layoutLibraryDeleteHandler_();
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestLayoutLibraryExportLocalID:(NSString*)localID {
  if (layoutLibraryExportLayoutHandler_) {
    layoutLibraryExportLayoutHandler_(localID);
  } else if (layoutLibraryExportHandler_) {
    layoutLibraryExportHandler_();
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestLayoutLibrarySetTitleDefaultLocalID:(NSString*)localID {
  if (layoutLibrarySetTitleDefaultHandler_) {
    layoutLibrarySetTitleDefaultHandler_(localID);
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestLayoutLibrarySetGlobalDefaultLocalID:(NSString*)localID {
  if (layoutLibrarySetGlobalDefaultHandler_) {
    layoutLibrarySetGlobalDefaultHandler_(localID);
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestLayoutLibraryFavoriteLocalID:(NSString*)localID
                                  favorite:(BOOL)favorite {
  if (layoutLibraryFavoriteHandler_) {
    layoutLibraryFavoriteHandler_(localID, favorite);
  }
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
              didRequestAction:(xe::hid::touch::IOSTouchAction)action {
  [self setSelectedControlAction:action];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
         didRequestLabelHidden:(BOOL)hidden {
  [self setSelectedControlLabelHidden:hidden];
}

- (void)touchOverlayEditChromeDidRequestResetLabel:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self resetSelectedControlLabel];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
     didRequestBehaviorTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger {
  [self setSelectedControlBehaviorTrigger:trigger];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
      didRequestBehaviorAction:(xe::hid::touch::IOSTouchAction)action {
  [self setSelectedControlBehaviorAction:action];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
          didRequestDragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  [self setSelectedControlDragOutput:output];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestBehaviorAnalogOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  [self setSelectedControlBehaviorAnalogOutput:output];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
     didRequestBehaviorTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger
                        action:(xe::hid::touch::IOSTouchAction)action
                  analogOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  [self setSelectedControlSecondaryBehaviorTrigger:trigger action:action analogOutput:output];
}

- (void)touchOverlayEditChromeDidRequestClearSelectedControlExtras:
    (XeniaTouchOverlayEditChromeIOS*)__unused chrome {
  [self clearSelectedControlExtras];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
           didRequestTintStyle:(xe::hid::touch::IOSTouchTintStyle)tintStyle {
  [self setSelectedControlTintStyle:tintStyle];
}

- (void)touchOverlayEditChromeDidRequestAnalogTuningPanel:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self presentAnalogTuningPanel];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
               didRequestShape:(xe::hid::touch::IOSTouchControlShape)shape {
  [self setSelectedControlShape:shape];
}

- (void)touchOverlayEditChromeDidRequestMirrorControl:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self mirrorSelectedControlHorizontally];
}

- (void)touchOverlayEditChromeDidRequestToggleMoveDpadRing:(XeniaTouchOverlayEditChromeIOS*)__unused
    chrome {
  [self toggleSelectedControlMoveDpadRing];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestAddControlOfType:(xe::hid::touch::IOSTouchControlType)type {
  [self addControlOfType:type];
}

- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)__unused chrome
    didRequestAddJoystickWithAction:(xe::hid::touch::IOSTouchAction)action {
  [self addJoystickWithAction:action];
}

@end
