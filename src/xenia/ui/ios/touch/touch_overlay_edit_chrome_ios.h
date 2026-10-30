/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_TOUCH_OVERLAY_EDIT_CHROME_IOS_H_
#define XENIA_UI_IOS_TOUCH_TOUCH_OVERLAY_EDIT_CHROME_IOS_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include "xenia/hid/touch/touch_layout_ios.h"

@class XeniaTouchLayoutLibraryItem;
@class XeniaTouchOverlayEditChromeIOS;

namespace xe::ui::ios::touch_overlay {

struct TouchOverlayEditChromeState {
  bool editing_enabled = false;
  bool showing_layout_library = false;
  bool has_selected_control = false;
  bool can_duplicate_selected_control = false;
  xe::hid::touch::IOSTouchControlDefinition selected_control;
};

}  // namespace xe::ui::ios::touch_overlay

@protocol XeniaTouchOverlayEditChromeIOSDelegate <NSObject>

- (void)touchOverlayEditChromeDidRequestSmallerControl:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestMatchNearestSize:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestLargerControl:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestRenameLabel:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestDuplicateControl:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestDeleteControl:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestLayoutLibrary:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestHideLayoutLibrary:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestLayoutLibrarySaveCopy:
    (XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestLayoutLibraryImport:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestLayoutLibraryReset:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestLayoutLibraryLoad:(NSString*)localID;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestLayoutLibraryRenameLocalID:(NSString*)localID;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestLayoutLibraryDeleteLocalID:(NSString*)localID;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestLayoutLibraryExportLocalID:(NSString*)localID;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestLayoutLibrarySetTitleDefaultLocalID:(NSString*)localID;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestLayoutLibrarySetGlobalDefaultLocalID:(NSString*)localID;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestLayoutLibraryFavoriteLocalID:(NSString*)localID
                                  favorite:(BOOL)favorite;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
              didRequestAction:(xe::hid::touch::IOSTouchAction)action;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
             didRequestOpacity:(float)opacity;
- (void)touchOverlayEditChromeDidBeginOpacityChange:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidEndOpacityChange:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
         didRequestLabelHidden:(BOOL)hidden;
- (void)touchOverlayEditChromeDidRequestResetLabel:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
     didRequestBehaviorTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
      didRequestBehaviorAction:(xe::hid::touch::IOSTouchAction)action;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
          didRequestDragOutput:(xe::hid::touch::IOSTouchAnalogOutput)output;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestBehaviorAnalogOutput:(xe::hid::touch::IOSTouchAnalogOutput)output;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
     didRequestBehaviorTrigger:(xe::hid::touch::IOSTouchInteractionTrigger)trigger
                        action:(xe::hid::touch::IOSTouchAction)action
                  analogOutput:(xe::hid::touch::IOSTouchAnalogOutput)output;
- (void)touchOverlayEditChromeDidRequestClearSelectedControlExtras:
    (XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
           didRequestTintStyle:(xe::hid::touch::IOSTouchTintStyle)tintStyle;
- (void)touchOverlayEditChromeDidRequestAnalogTuningPanel:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
               didRequestShape:(xe::hid::touch::IOSTouchControlShape)shape;
- (void)touchOverlayEditChromeDidRequestMirrorControl:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChromeDidRequestToggleMoveDpadRing:(XeniaTouchOverlayEditChromeIOS*)chrome;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestAddControlOfType:(xe::hid::touch::IOSTouchControlType)type;
- (void)touchOverlayEditChrome:(XeniaTouchOverlayEditChromeIOS*)chrome
    didRequestAddJoystickWithAction:(xe::hid::touch::IOSTouchAction)action;

@end

@interface XeniaTouchOverlayEditChromeIOS : UIView

@property(nonatomic, assign) id<XeniaTouchOverlayEditChromeIOSDelegate> delegate;

- (void)applyState:(const xe::ui::ios::touch_overlay::TouchOverlayEditChromeState&)state;
- (CGFloat)preferredHeightForWidth:(CGFloat)width
                   availableHeight:(CGFloat)availableHeight
                            margin:(CGFloat)margin;
- (BOOL)isInspectorExpanded;
- (UIView*)interactiveHitTestForOverlayPoint:(CGPoint)point
                                       event:(UIEvent*)event
                                      inView:(UIView*)overlayView;
- (void)setLayoutLibraryItems:(NSArray<XeniaTouchLayoutLibraryItem*>*)items
         currentLayoutLocalID:(NSString*)currentLayoutLocalID;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_TOUCH_TOUCH_OVERLAY_EDIT_CHROME_IOS_H_
