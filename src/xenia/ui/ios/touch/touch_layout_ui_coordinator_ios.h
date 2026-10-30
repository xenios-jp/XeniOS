/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_TOUCH_LAYOUT_UI_COORDINATOR_IOS_H_
#define XENIA_UI_IOS_TOUCH_TOUCH_LAYOUT_UI_COORDINATOR_IOS_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace xe {
namespace hid {
namespace touch {
class IOSTouchRuntimeModel;
}  // namespace touch
}  // namespace hid
}  // namespace xe

@class XeniaTouchLayoutLibraryItem;

@protocol XeniaIOSTouchLayoutUICoordinatorHost <NSObject>

- (xe::hid::touch::IOSTouchRuntimeModel*)touchLayoutCoordinatorRuntimeModel;
- (uint32_t)touchLayoutCoordinatorActiveTitleID;
- (std::string)touchLayoutCoordinatorActiveLocalID;
- (void)touchLayoutCoordinatorSetActiveLocalID:(const std::string&)localID;
- (BOOL)touchLayoutCoordinatorGameRunning;
- (BOOL)touchLayoutCoordinatorCanPresentPendingInstall;
- (UIViewController*)touchLayoutCoordinatorTopPresenter;
- (void)touchLayoutCoordinatorSetGameplayModalPresentationPending:(BOOL)pending;
- (void)touchLayoutCoordinatorUpdateTouchOverlayVisibilityAnimated:(BOOL)animated;
- (void)touchLayoutCoordinatorRefreshTouchOverlayLayoutModel;
- (BOOL)touchLayoutCoordinatorIsShowingLayoutLibrary;
- (void)touchLayoutCoordinatorShowLayoutLibraryWithItems:
            (NSArray<XeniaTouchLayoutLibraryItem*>*)items
                                    currentLayoutLocalID:(NSString*)currentLayoutLocalID;
- (void)touchLayoutCoordinatorSetStatusText:(NSString*)text;
- (void)touchLayoutCoordinatorPresentAlertWithTitle:(NSString*)title message:(NSString*)message;
- (void)touchLayoutCoordinatorPresentKeyboardPromptWithTitle:(NSString*)title
                                                 description:(NSString*)description
                                                 defaultText:(NSString*)defaultText
                                                  completion:(void (^)(BOOL cancelled,
                                                                       NSString* text))completion;
- (void)touchLayoutCoordinatorOpenTouchLayoutFileImportPicker;
- (void)touchLayoutCoordinatorEvaluateAutomaticStikDebugJITHandoffIfNeeded;

@end

@interface XeniaIOSTouchLayoutUICoordinator : NSObject

- (instancetype)initWithHost:(id<XeniaIOSTouchLayoutUICoordinatorHost>)host;
- (BOOL)hasPendingInstall;
- (void)applyDefaultLayoutModel;
- (void)applyLayoutModelForTitleID:(uint32_t)titleID;
- (void)saveCurrentLayoutForTitleID:(uint32_t)titleID;
- (void)presentLibrary;
- (void)applyLayoutWithLocalID:(NSString*)localID;
- (void)presentRenameSheet;
- (void)presentDeleteSheet;
- (void)renameLayoutWithLocalID:(NSString*)localID;
- (void)deleteLayoutWithLocalID:(NSString*)localID;
- (void)saveCurrentLayoutCopy;
- (void)importFromFile;
- (void)importLayoutAtURL:(NSURL*)url;
- (void)exportCurrentLayout;
- (void)exportLayoutWithLocalID:(NSString*)localID;
- (void)setLayoutDefaultForCurrentTitleWithLocalID:(NSString*)localID;
- (void)setLayoutDefaultForAllGamesWithLocalID:(NSString*)localID;
- (void)setLayoutFavoriteWithLocalID:(NSString*)localID favorite:(BOOL)favorite;
- (void)resetToOfficialPreset;
- (BOOL)handleExternalFileURL:(NSURL*)url;
- (BOOL)handleExternalSchemeURL:(NSURL*)url;
- (void)presentPendingInstallIfReady;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_TOUCH_TOUCH_LAYOUT_UI_COORDINATOR_IOS_H_
