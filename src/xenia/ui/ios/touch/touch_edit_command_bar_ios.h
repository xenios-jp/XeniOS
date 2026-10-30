/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_TOUCH_EDIT_COMMAND_BAR_IOS_H_
#define XENIA_UI_IOS_TOUCH_TOUCH_EDIT_COMMAND_BAR_IOS_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

// Compact global chrome for the redesigned touch-overlay editor. The host
// docks this strip where it collides least with authored controls; per-control
// edits live in XeniaTouchEditPanel.
//
// MRC module: callers retain via property setters; this view owns its subviews
// and releases them in -dealloc. The delegate is assign (unowned).

@class XeniaTouchEditCommandBar;

@protocol XeniaTouchEditCommandBarDelegate <NSObject>
- (void)touchEditCommandBarDidRequestLayouts:(XeniaTouchEditCommandBar*)bar;
- (void)touchEditCommandBarDidRequestUndo:(XeniaTouchEditCommandBar*)bar;
- (void)touchEditCommandBarDidRequestRedo:(XeniaTouchEditCommandBar*)bar;
- (void)touchEditCommandBarDidToggleGrid:(XeniaTouchEditCommandBar*)bar;
- (void)touchEditCommandBarDidRequestAdd:(XeniaTouchEditCommandBar*)bar;
- (void)touchEditCommandBarDidRequestDone:(XeniaTouchEditCommandBar*)bar;
@end

@interface XeniaTouchEditCommandBar : UIView

@property(nonatomic, assign) id<XeniaTouchEditCommandBarDelegate> delegate;

// Suggested fixed height for a manual-frame host.
+ (CGFloat)preferredHeight;
- (CGFloat)preferredWidth;

// State the host pushes in from the active overlay/editor model.
- (void)setLayoutName:(NSString*)name;
- (void)setCanUndo:(BOOL)canUndo canRedo:(BOOL)canRedo;
- (void)setGridActive:(BOOL)active;  // grid-snap on/off
- (void)setAddMenu:(UIMenu*)menu;    // nil falls back to delegate callback

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_TOUCH_TOUCH_EDIT_COMMAND_BAR_IOS_H_
