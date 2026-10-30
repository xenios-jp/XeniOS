/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_overlay_edit_chrome_ios.h"

#import "xenia/ui/ios/shared/ios_theme_controls.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"
#import "xenia/ui/ios/touch/touch_controls_overlay_helpers_ios.h"
#import "xenia/ui/ios/touch/touch_edit_panel_ios.h"
#import "xenia/ui/ios/touch/touch_layout_library_controller_ios.h"

namespace {

using namespace xe::ui::ios::touch_overlay;

constexpr CGFloat kChromeInset = 14.0f;
constexpr CGFloat kHeaderGap = 8.0f;
constexpr CGFloat kLayoutsWidth = 64.0f;

UIButton* CreateChromeButton(NSString* title, id target, SEL selector) {
  UIButton* button = [[UIButton buttonWithType:UIButtonTypeSystem] retain];
  button.hidden = YES;
  button.backgroundColor = [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySoft]];
  button.layer.cornerRadius = XeniaRadiusLg;
  xe_apply_button_title_font(button, UIFontTextStyleFootnote, 13.0, UIFontWeightSemibold);
  button.titleLabel.adjustsFontSizeToFitWidth = YES;
  button.titleLabel.minimumScaleFactor = 0.60f;
  [button setTitleColor:[[UIColor whiteColor] colorWithAlphaComponent:0.94]
               forState:UIControlStateNormal];
  [button setTitle:title forState:UIControlStateNormal];
  button.accessibilityLabel = title;
  button.accessibilityTraits = UIAccessibilityTraitButton;
  [button addTarget:target action:selector forControlEvents:UIControlEventTouchUpInside];
  return button;
}

}  // namespace

@interface XeniaTouchOverlayEditChromeIOS () <XeniaTouchEditPanelDelegate>
@end

@implementation XeniaTouchOverlayEditChromeIOS {
  id<XeniaTouchOverlayEditChromeIOSDelegate> delegate_;
  XeniaTouchEditPanel* edit_panel_;
  UILabel* title_label_;
  UILabel* selection_label_;
  UIButton* library_button_;
  UISegmentedControl* layout_library_filter_control_;
  UIButton* layout_library_save_button_;
  UIButton* layout_library_import_button_;
  UIButton* layout_library_reset_button_;
  UITableView* layout_library_table_;
  XeniaTouchLayoutLibraryTableController* layout_library_controller_;
  xe::ui::ios::touch_overlay::TouchOverlayEditChromeState state_;
}

@synthesize delegate = delegate_;

- (instancetype)initWithFrame:(CGRect)frame {
  if (!(self = [super initWithFrame:frame])) {
    return nil;
  }

  self.hidden = YES;
  xe_apply_floating_window_chrome(self);

  title_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  title_label_.backgroundColor = [UIColor clearColor];
  title_label_.text = @"Edit";
  xe_apply_label_font(title_label_, UIFontTextStyleBody, 16.0, UIFontWeightSemibold);
  title_label_.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.94];
  title_label_.adjustsFontSizeToFitWidth = YES;
  title_label_.minimumScaleFactor = 0.70f;
  title_label_.accessibilityTraits = UIAccessibilityTraitHeader;
  [self addSubview:title_label_];

  selection_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  selection_label_.backgroundColor = [UIColor clearColor];
  xe_apply_label_font(selection_label_, UIFontTextStyleSubheadline, 15.0, UIFontWeightSemibold);
  selection_label_.textColor = [[UIColor whiteColor] colorWithAlphaComponent:0.82];
  selection_label_.adjustsFontSizeToFitWidth = YES;
  selection_label_.minimumScaleFactor = 0.75f;
  [self addSubview:selection_label_];

  library_button_ = CreateChromeButton(@"Layouts", self, @selector(layoutLibraryButtonPressed:));
  [self addSubview:library_button_];

  layout_library_filter_control_ =
      [[UISegmentedControl alloc] initWithItems:@[ @"Official", @"Saved", @"Favorites" ]];
  layout_library_filter_control_.selectedSegmentIndex = XeniaTouchLayoutLibraryFilterOfficial;
  layout_library_filter_control_.hidden = YES;
  layout_library_filter_control_.backgroundColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySubtle]];
  layout_library_filter_control_.tintColor = [XeniaTheme touchTintAmber];
  if (@available(iOS 13.0, *)) {
    layout_library_filter_control_.selectedSegmentTintColor = [XeniaTheme touchTintAmber];
    [layout_library_filter_control_
        setTitleTextAttributes:@{
          NSForegroundColorAttributeName : [[UIColor whiteColor] colorWithAlphaComponent:0.86]
        }
                    forState:UIControlStateNormal];
    [layout_library_filter_control_
        setTitleTextAttributes:@{NSForegroundColorAttributeName : [UIColor blackColor]}
                    forState:UIControlStateSelected];
  }
  [layout_library_filter_control_ addTarget:self
                                     action:@selector(layoutLibraryFilterChanged:)
                           forControlEvents:UIControlEventValueChanged];
  [self addSubview:layout_library_filter_control_];

  layout_library_save_button_ =
      CreateChromeButton(@"Save Copy", self, @selector(layoutLibrarySaveButtonPressed:));
  layout_library_save_button_.hidden = YES;
  [self addSubview:layout_library_save_button_];
  layout_library_import_button_ =
      CreateChromeButton(@"Import", self, @selector(layoutLibraryImportButtonPressed:));
  layout_library_import_button_.hidden = YES;
  [self addSubview:layout_library_import_button_];
  layout_library_reset_button_ =
      CreateChromeButton(@"Reset", self, @selector(layoutLibraryResetButtonPressed:));
  layout_library_reset_button_.hidden = YES;
  [layout_library_reset_button_ setTitleColor:[[XeniaTheme statusWarning] colorWithAlphaComponent:0.98]
                                     forState:UIControlStateNormal];
  [self addSubview:layout_library_reset_button_];

  layout_library_table_ = [[UITableView alloc] initWithFrame:CGRectZero
                                                       style:UITableViewStylePlain];
  layout_library_table_.hidden = YES;
  layout_library_table_.backgroundColor = [UIColor clearColor];
  layout_library_table_.separatorStyle = UITableViewCellSeparatorStyleNone;
  layout_library_table_.separatorInset = UIEdgeInsetsMake(0, 16, 0, 16);
  layout_library_table_.rowHeight = 52.0f;
  layout_library_table_.estimatedRowHeight = 52.0f;
  layout_library_table_.estimatedSectionHeaderHeight = 0.01f;
  layout_library_table_.estimatedSectionFooterHeight = 0.01f;
  if (@available(iOS 15.0, *)) {
    layout_library_table_.sectionHeaderTopPadding = 0.0f;
  }
  layout_library_table_.separatorColor =
      [[UIColor whiteColor] colorWithAlphaComponent:[XeniaTheme opacitySoft]];
  if (@available(iOS 11.0, *)) {
    layout_library_table_.contentInsetAdjustmentBehavior = UIScrollViewContentInsetAdjustmentNever;
  }
  layout_library_controller_ = [[XeniaTouchLayoutLibraryTableController alloc] init];
  __unsafe_unretained XeniaTouchOverlayEditChromeIOS* unsafe_self = self;
  layout_library_controller_.loadHandler = ^(NSString* local_id) {
    [unsafe_self->delegate_ touchOverlayEditChrome:unsafe_self
                       didRequestLayoutLibraryLoad:local_id];
  };
  layout_library_controller_.renameLayoutHandler = ^(NSString* local_id) {
    [unsafe_self->delegate_ touchOverlayEditChrome:unsafe_self
              didRequestLayoutLibraryRenameLocalID:local_id];
  };
  layout_library_controller_.deleteLayoutHandler = ^(NSString* local_id) {
    [unsafe_self->delegate_ touchOverlayEditChrome:unsafe_self
              didRequestLayoutLibraryDeleteLocalID:local_id];
  };
  layout_library_controller_.exportLayoutHandler = ^(NSString* local_id) {
    [unsafe_self->delegate_ touchOverlayEditChrome:unsafe_self
              didRequestLayoutLibraryExportLocalID:local_id];
  };
  layout_library_controller_.setTitleDefaultHandler = ^(NSString* local_id) {
    [unsafe_self->delegate_ touchOverlayEditChrome:unsafe_self
     didRequestLayoutLibrarySetTitleDefaultLocalID:local_id];
  };
  layout_library_controller_.setGlobalDefaultHandler = ^(NSString* local_id) {
    [unsafe_self->delegate_ touchOverlayEditChrome:unsafe_self
    didRequestLayoutLibrarySetGlobalDefaultLocalID:local_id];
  };
  layout_library_controller_.favoriteLayoutHandler = ^(NSString* local_id, BOOL favorite) {
    [unsafe_self->delegate_ touchOverlayEditChrome:unsafe_self
        didRequestLayoutLibraryFavoriteLocalID:local_id
                                      favorite:favorite];
  };
  layout_library_table_.dataSource = layout_library_controller_;
  layout_library_table_.delegate = layout_library_controller_;
  layout_library_table_.indicatorStyle = UIScrollViewIndicatorStyleWhite;
  [self addSubview:layout_library_table_];

  edit_panel_ = [[XeniaTouchEditPanel alloc] initWithFrame:CGRectZero];
  edit_panel_.delegate = self;
  edit_panel_.hidden = YES;
  [self addSubview:edit_panel_];

  return self;
}

- (void)dealloc {
  delegate_ = nil;
  [edit_panel_ release];
  [layout_library_controller_ release];
  [layout_library_table_ release];
  [layout_library_reset_button_ release];
  [layout_library_import_button_ release];
  [layout_library_save_button_ release];
  [layout_library_filter_control_ release];
  [library_button_ release];
  [selection_label_ release];
  [title_label_ release];
  [super dealloc];
}

- (void)setLayoutLibraryItems:(NSArray<XeniaTouchLayoutLibraryItem*>*)items
         currentLayoutLocalID:(NSString*)currentLayoutLocalID {
  [layout_library_controller_ setItems:items currentLayoutLocalID:currentLayoutLocalID];
  [layout_library_table_ reloadData];
}

- (void)applyState:(const xe::ui::ios::touch_overlay::TouchOverlayEditChromeState&)state {
  state_ = state;

  if (state_.showing_layout_library) {
    edit_panel_.hidden = YES;
    title_label_.text = @"Layouts";
    title_label_.hidden = !state_.editing_enabled;
    selection_label_.text = @"Apply a layout or manage saved copies";
    selection_label_.hidden = !state_.editing_enabled;
    [library_button_ setTitle:@"Back" forState:UIControlStateNormal];
    library_button_.hidden = !state_.editing_enabled;
    layout_library_filter_control_.hidden = !state_.editing_enabled;
    layout_library_save_button_.hidden = !state_.editing_enabled;
    layout_library_import_button_.hidden = !state_.editing_enabled;
    layout_library_reset_button_.hidden = !state_.editing_enabled;
    layout_library_table_.hidden = !state_.editing_enabled;
    [self setNeedsLayout];
    return;
  }

  title_label_.hidden = YES;
  selection_label_.hidden = YES;
  library_button_.hidden = YES;
  layout_library_filter_control_.hidden = YES;
  layout_library_save_button_.hidden = YES;
  layout_library_import_button_.hidden = YES;
  layout_library_reset_button_.hidden = YES;
  layout_library_table_.hidden = YES;

  if (state_.has_selected_control) {
    [edit_panel_ applyControl:state_.selected_control];
    [edit_panel_ setDuplicateAvailable:state_.can_duplicate_selected_control];
    edit_panel_.hidden = NO;
  } else {
    [edit_panel_ clearControl];
    edit_panel_.hidden = YES;
  }
  [self setNeedsLayout];
}

- (CGFloat)preferredHeightForWidth:(CGFloat)width
                   availableHeight:(CGFloat)availableHeight
                            margin:(CGFloat)margin {
  CGFloat desired_height = 0.0f;
  if (state_.showing_layout_library) {
    desired_height = 320.0f;
  } else if (state_.has_selected_control) {
    desired_height = [edit_panel_ preferredHeightForWidth:width];
  }
  return MIN(desired_height, MAX(160.0f, availableHeight - margin * 2.0f));
}

- (BOOL)isInspectorExpanded {
  return edit_panel_.expanded;
}

- (void)layoutSubviews {
  [super layoutSubviews];

  if (state_.showing_layout_library) {
    edit_panel_.frame = CGRectZero;
    const CGFloat local_width = CGRectGetWidth(self.bounds);
    const CGFloat local_height = CGRectGetHeight(self.bounds);
    library_button_.frame =
        CGRectMake(local_width - kChromeInset - kLayoutsWidth, 8.0f, kLayoutsWidth, 34.0f);
    title_label_.frame =
        CGRectMake(kChromeInset, 10.0f,
                   MAX(0.0f, CGRectGetMinX(library_button_.frame) - kChromeInset - 8.0f), 22.0f);
    selection_label_.frame =
        CGRectMake(kChromeInset, 38.0f, local_width - kChromeInset * 2.0f, 20.0f);

    CGFloat current_y = 68.0f;
    layout_library_filter_control_.frame =
        CGRectMake(kChromeInset, current_y, local_width - kChromeInset * 2.0f, 32.0f);
    current_y = CGRectGetMaxY(layout_library_filter_control_.frame) + 8.0f;

    const CGFloat footer_height = 32.0f;
    const CGFloat footer_y = local_height - footer_height - 10.0f;
    NSArray<UIButton*>* footer_buttons =
        @[ layout_library_import_button_, layout_library_save_button_, layout_library_reset_button_ ];
    LayoutVisibleButtonsRow(footer_y, kChromeInset, local_width - kChromeInset * 2.0f,
                            footer_height, kHeaderGap, footer_buttons);
    layout_library_table_.frame =
        CGRectMake(kChromeInset, current_y, local_width - kChromeInset * 2.0f,
                   MAX(0.0f, footer_y - current_y - 8.0f));
    return;
  }

  layout_library_filter_control_.frame = CGRectZero;
  layout_library_save_button_.frame = CGRectZero;
  layout_library_import_button_.frame = CGRectZero;
  layout_library_reset_button_.frame = CGRectZero;
  layout_library_table_.frame = CGRectZero;
  title_label_.frame = CGRectZero;
  selection_label_.frame = CGRectZero;
  library_button_.frame = CGRectZero;
  edit_panel_.frame = edit_panel_.hidden ? CGRectZero : self.bounds;
}

- (UIView*)interactiveHitTestForOverlayPoint:(CGPoint)point
                                       event:(UIEvent*)event
                                      inView:(UIView*)overlayView {
  if (self.hidden || self.alpha <= 0.01f || !self.userInteractionEnabled) {
    return nil;
  }
  for (UIView* subview in [self.subviews reverseObjectEnumerator]) {
    if (subview.hidden || subview.alpha <= 0.01f || !subview.userInteractionEnabled) {
      continue;
    }
    CGPoint local_point = [subview convertPoint:point fromView:overlayView];
    UIView* hit = [subview hitTest:local_point withEvent:event];
    if (hit) {
      if (subview == edit_panel_ && hit == edit_panel_) {
        continue;
      }
      return hit;
    }
  }
  CGPoint chrome_point = [self convertPoint:point fromView:overlayView];
  if (!layout_library_table_.hidden &&
      CGRectContainsPoint(layout_library_table_.frame, chrome_point)) {
    return layout_library_table_;
  }
  return nil;
}

- (void)layoutLibraryButtonPressed:(UIButton*)__unused sender {
  if (state_.showing_layout_library) {
    [delegate_ touchOverlayEditChromeDidRequestHideLayoutLibrary:self];
    return;
  }
  [delegate_ touchOverlayEditChromeDidRequestLayoutLibrary:self];
}

- (void)layoutLibraryFilterChanged:(UISegmentedControl*)sender {
  [layout_library_controller_ setFilter:static_cast<XeniaTouchLayoutLibraryFilter>(
                                            MAX(sender.selectedSegmentIndex, 0))];
  [layout_library_table_ reloadData];
}

- (void)layoutLibrarySaveButtonPressed:(UIButton*)__unused sender {
  [delegate_ touchOverlayEditChromeDidRequestLayoutLibrarySaveCopy:self];
}

- (void)layoutLibraryImportButtonPressed:(UIButton*)__unused sender {
  [delegate_ touchOverlayEditChromeDidRequestLayoutLibraryImport:self];
}

- (void)layoutLibraryResetButtonPressed:(UIButton*)__unused sender {
  [delegate_ touchOverlayEditChromeDidRequestLayoutLibraryReset:self];
}

#pragma mark Panel delegate

- (void)touchEditPanelDidToggleExpansion:(XeniaTouchEditPanel*)__unused panel {
  [self setNeedsLayout];
  [self.superview setNeedsLayout];
}
- (void)touchEditPanelDidBeginOpacityChange:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidBeginOpacityChange:self];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel didChangeOpacity:(float)opacity {
  [delegate_ touchOverlayEditChrome:self didRequestOpacity:opacity];
}
- (void)touchEditPanelDidEndOpacityChange:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidEndOpacityChange:self];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
         didSelectTint:(xe::hid::touch::IOSTouchTintStyle)tint {
  [delegate_ touchOverlayEditChrome:self didRequestTintStyle:tint];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
        didSelectShape:(xe::hid::touch::IOSTouchControlShape)shape {
  [delegate_ touchOverlayEditChrome:self didRequestShape:shape];
}
- (void)touchEditPanelDidRequestRenameLabel:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestRenameLabel:self];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
    didSelectLabelHidden:(BOOL)hidden {
  [delegate_ touchOverlayEditChrome:self didRequestLabelHidden:hidden];
}
- (void)touchEditPanelDidRequestResetLabel:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestResetLabel:self];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
    didSelectTapAction:(xe::hid::touch::IOSTouchAction)action {
  [delegate_ touchOverlayEditChrome:self didRequestAction:action];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
   didSelectDragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  [delegate_ touchOverlayEditChrome:self didRequestDragOutput:output];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
    didSelectGestureTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger {
  [delegate_ touchOverlayEditChrome:self didRequestBehaviorTrigger:trigger];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
    didSelectGestureAction:(xe::hid::touch::IOSTouchAction)action {
  [delegate_ touchOverlayEditChrome:self didRequestBehaviorAction:action];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
    didSelectGestureDragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  [delegate_ touchOverlayEditChrome:self didRequestBehaviorAnalogOutput:output];
}
- (void)touchEditPanel:(XeniaTouchEditPanel*)__unused panel
    didSelectQuickGestureTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger
                          action:(xe::hid::touch::IOSTouchAction)action
                      dragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output {
  [delegate_ touchOverlayEditChrome:self
          didRequestBehaviorTrigger:trigger
                              action:action
                        analogOutput:output];
}
- (void)touchEditPanelDidRequestClearExtras:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestClearSelectedControlExtras:self];
}
- (void)touchEditPanelDidRequestToggleMoveDpadRing:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestToggleMoveDpadRing:self];
}
- (void)touchEditPanelDidRequestTune:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestAnalogTuningPanel:self];
}
- (void)touchEditPanelDidRequestSmaller:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestSmallerControl:self];
}
- (void)touchEditPanelDidRequestLarger:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestLargerControl:self];
}
- (void)touchEditPanelDidRequestMirror:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestMirrorControl:self];
}
- (void)touchEditPanelDidRequestMatchSize:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestMatchNearestSize:self];
}
- (void)touchEditPanelDidRequestDuplicate:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestDuplicateControl:self];
}
- (void)touchEditPanelDidRequestDelete:(XeniaTouchEditPanel*)__unused panel {
  [delegate_ touchOverlayEditChromeDidRequestDeleteControl:self];
}

@end
