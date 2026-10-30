/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_TOUCH_CONTROLS_OVERLAY_IOS_H_
#define XENIA_UI_TOUCH_CONTROLS_OVERLAY_IOS_H_

namespace xe {
namespace hid {
namespace touch {
class IOSTouchRuntimeModel;
}  // namespace touch
}  // namespace hid
}  // namespace xe

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/touch/touch_layout_library_view_ios.h"

@interface XeniaTouchControlsOverlayView : UIView

@property(nonatomic, copy) void (^pauseHandler)(void);
@property(nonatomic, copy) void (^doneEditingHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryLoadHandler)(NSString* localID);
@property(nonatomic, copy) void (^layoutLibrarySaveCopyHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryRenameHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryDeleteHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryImportHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryExportHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryResetHandler)(void);
@property(nonatomic, copy) void (^layoutLibraryRenameLayoutHandler)(NSString* localID);
@property(nonatomic, copy) void (^layoutLibraryDeleteLayoutHandler)(NSString* localID);
@property(nonatomic, copy) void (^layoutLibraryExportLayoutHandler)(NSString* localID);
@property(nonatomic, copy) void (^layoutLibrarySetTitleDefaultHandler)(NSString* localID);
@property(nonatomic, copy) void (^layoutLibrarySetGlobalDefaultHandler)(NSString* localID);
@property(nonatomic, copy) void (^layoutLibraryFavoriteHandler)(NSString* localID, BOOL favorite);

- (instancetype)initWithRuntimeModel:(xe::hid::touch::IOSTouchRuntimeModel*)runtime_model
    NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithCoder:(NSCoder*)coder NS_UNAVAILABLE;
- (instancetype)initWithFrame:(CGRect)frame NS_UNAVAILABLE;

- (BOOL)isEditingControlsEnabled;
- (BOOL)isShowingLayoutLibrary;
- (void)setEditingControlsEnabled:(BOOL)enabled animated:(BOOL)animated;
- (void)refreshLayoutModel;
- (void)setGameplayOverlayVisible:(BOOL)visible animated:(BOOL)animated;
- (void)showLayoutLibraryWithItems:(NSArray<XeniaTouchLayoutLibraryItem*>*)items
              currentLayoutLocalID:(NSString*)currentLayoutLocalID;
- (void)hideLayoutLibrary;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_TOUCH_CONTROLS_OVERLAY_IOS_H_
