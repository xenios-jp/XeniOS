/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_TOUCH_CONTROLS_IOS_H_
#define XENIA_UI_TOUCH_CONTROLS_IOS_H_

#ifdef __OBJC__
#import <UIKit/UIKit.h>
#include "xenia/hid/input.h"

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, XeniaTouchControlShape) {
  XeniaTouchControlShapeCircle = 0,
  XeniaTouchControlShapeRoundedRectangle = 1,
  XeniaTouchControlShapeRoundedSquare = 2,
};

typedef NS_ENUM(NSInteger, XeniaTouchStickInputMode) {
  XeniaTouchStickInputModeJoystick = 0,
  XeniaTouchStickInputModeTouchpad = 1,
};

@interface XeniaTouchControlsView : UIView
@property(nonatomic, copy, nullable) void (^stateDidChangeHandler)(void);
@property(nonatomic, copy, nullable) void (^menuButtonTappedHandler)(void);
@property(nonatomic, assign, getter=isLayoutEditingEnabled) BOOL layoutEditingEnabled;
@property(nonatomic, copy, nullable) void (^layoutEditingSelectionDidChangeHandler)(NSInteger);
@property(nonatomic, copy, nullable) void (^layoutEditingDidChangeHandler)(void);
@property(nonatomic, readonly) NSInteger selectedControlIdentifier;
- (xe::hid::X_INPUT_STATE)currentControllerState;
- (void)resetControllerState;
- (CGFloat)scaleForControlIdentifier:(NSInteger)controlIdentifier;
- (void)setScale:(CGFloat)scale forControlIdentifier:(NSInteger)controlIdentifier;
- (NSString*)titleForControlIdentifier:(NSInteger)controlIdentifier;
- (void)setTitle:(nullable NSString*)title forControlIdentifier:(NSInteger)controlIdentifier;
- (nullable UIColor*)colorForControlIdentifier:(NSInteger)controlIdentifier;
- (nullable UIColor*)storedColorForControlIdentifier:(NSInteger)controlIdentifier;
- (void)setColor:(nullable UIColor*)color forControlIdentifier:(NSInteger)controlIdentifier;
- (BOOL)isHiddenForControlIdentifier:(NSInteger)controlIdentifier;
- (void)setHidden:(BOOL)hidden forControlIdentifier:(NSInteger)controlIdentifier;
- (CGRect)frameForControlIdentifier:(NSInteger)controlIdentifier;
- (NSInteger)shapeForControlIdentifier:(NSInteger)controlIdentifier;
- (void)setShape:(NSInteger)shape forControlIdentifier:(NSInteger)controlIdentifier;
- (NSInteger)inputModeForControlIdentifier:(NSInteger)controlIdentifier;
- (void)setInputMode:(NSInteger)inputMode forControlIdentifier:(NSInteger)controlIdentifier;
- (NSDictionary*)layoutConfiguration;
- (void)applyLayoutConfiguration:(nullable NSDictionary*)configuration;
- (void)resetLayoutConfigurationToDefaults;
@end

NS_ASSUME_NONNULL_END
#endif  // __OBJC__

#endif  // XENIA_UI_TOUCH_CONTROLS_IOS_H_
