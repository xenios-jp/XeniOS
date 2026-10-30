/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_TOUCH_EDIT_PANEL_IOS_H_
#define XENIA_UI_IOS_TOUCH_TOUCH_EDIT_PANEL_IOS_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include "xenia/hid/touch/touch_layout_ios.h"

// The selected-control inspector for the redesigned touch-overlay editor. It is
// the dodging panel that the host docks beside the selected control: a compact
// floating pill with the common layout/tune operations attached, and an
// expanded config body for APPEARANCE / BEHAVIOR / FEEL.
//
// The panel only reports intent; the host applies it through its existing
// editor mutators. MRC: the panel owns its subviews and releases them in
// -dealloc; the delegate is assign (unowned).

@class XeniaTouchEditPanel;

@protocol XeniaTouchEditPanelDelegate <NSObject>
// Peek <-> full toggle (host re-sizes / re-dodges the panel frame).
- (void)touchEditPanelDidToggleExpansion:(XeniaTouchEditPanel*)panel;
// APPEARANCE
- (void)touchEditPanelDidBeginOpacityChange:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel didChangeOpacity:(float)opacity;
- (void)touchEditPanelDidEndOpacityChange:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
         didSelectTint:(xe::hid::touch::IOSTouchTintStyle)tint;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
        didSelectShape:(xe::hid::touch::IOSTouchControlShape)shape;
- (void)touchEditPanelDidRequestRenameLabel:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel didSelectLabelHidden:(BOOL)hidden;
- (void)touchEditPanelDidRequestResetLabel:(XeniaTouchEditPanel*)panel;
// BEHAVIOR
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
    didSelectTapAction:(xe::hid::touch::IOSTouchAction)action;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
    didSelectDragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
    didSelectGestureTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
    didSelectGestureAction:(xe::hid::touch::IOSTouchAction)action;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
    didSelectGestureDragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output;
- (void)touchEditPanel:(XeniaTouchEditPanel*)panel
    didSelectQuickGestureTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger
                          action:(xe::hid::touch::IOSTouchAction)action
                      dragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output;
- (void)touchEditPanelDidRequestClearExtras:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanelDidRequestToggleMoveDpadRing:(XeniaTouchEditPanel*)panel;
// FEEL
- (void)touchEditPanelDidRequestTune:(XeniaTouchEditPanel*)panel;
// QUICK ACTIONS
- (void)touchEditPanelDidRequestSmaller:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanelDidRequestLarger:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanelDidRequestMirror:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanelDidRequestMatchSize:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanelDidRequestDuplicate:(XeniaTouchEditPanel*)panel;
- (void)touchEditPanelDidRequestDelete:(XeniaTouchEditPanel*)panel;
@end

@interface XeniaTouchEditPanel : UIView

@property(nonatomic, assign) id<XeniaTouchEditPanelDelegate> delegate;
@property(nonatomic, readonly, getter=isExpanded) BOOL expanded;

- (void)applyControl:(const xe::hid::touch::IOSTouchControlDefinition&)control;
- (void)clearControl;
- (void)setDuplicateAvailable:(BOOL)available;
- (void)setExpanded:(BOOL)expanded animated:(BOOL)animated;

// Height the host should give the panel at the current width / expansion state.
- (CGFloat)preferredHeightForWidth:(CGFloat)width;
+ (CGFloat)collapsedHeight;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_TOUCH_TOUCH_EDIT_PANEL_IOS_H_
