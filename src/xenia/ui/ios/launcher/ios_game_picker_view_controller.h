/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_GAME_PICKER_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_GAME_PICKER_VIEW_CONTROLLER_H_

#ifdef __OBJC__

#import <UIKit/UIKit.h>

#include <cstdint>

#import "xenia/ui/ios/shared/ios_view_helpers.h"

@interface XeniaIOSGamePickerItem : NSObject

@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* subtitle;
@property(nonatomic, assign) uint32_t titleID;
@property(nonatomic, assign) NSUInteger gameIndex;

+ (instancetype)itemWithTitle:(NSString*)title
                     subtitle:(NSString*)subtitle
                      titleID:(uint32_t)titleID
                    gameIndex:(NSUInteger)gameIndex;

@end

@interface XeniaIOSGamePickerViewController : XESheetTableViewController

@property(nonatomic, copy) void (^selectionHandler)(NSUInteger gameIndex);

- (instancetype)initWithTitle:(NSString*)title
                       prompt:(NSString*)prompt
                        items:(NSArray<XeniaIOSGamePickerItem*>*)items;

@end

#endif  // __OBJC__

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_GAME_PICKER_VIEW_CONTROLLER_H_
