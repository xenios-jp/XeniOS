/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_CONTROLLER_IOS_H_
#define XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_CONTROLLER_IOS_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/touch/touch_layout_library_view_ios.h"

typedef NS_ENUM(NSInteger, XeniaTouchLayoutLibraryFilter) {
  XeniaTouchLayoutLibraryFilterOfficial = 0,
  XeniaTouchLayoutLibraryFilterSaved,
  XeniaTouchLayoutLibraryFilterFavorites,
};

@interface XeniaTouchLayoutLibraryTableController
    : NSObject <UITableViewDataSource, UITableViewDelegate>

@property(nonatomic, copy) void (^loadHandler)(NSString* localID);
@property(nonatomic, copy) void (^renameLayoutHandler)(NSString* localID);
@property(nonatomic, copy) void (^deleteLayoutHandler)(NSString* localID);
@property(nonatomic, copy) void (^exportLayoutHandler)(NSString* localID);
@property(nonatomic, copy) void (^setTitleDefaultHandler)(NSString* localID);
@property(nonatomic, copy) void (^setGlobalDefaultHandler)(NSString* localID);
@property(nonatomic, copy) void (^favoriteLayoutHandler)(NSString* localID, BOOL favorite);

- (void)setItems:(NSArray<XeniaTouchLayoutLibraryItem*>*)items
    currentLayoutLocalID:(NSString*)currentLayoutLocalID;
- (void)setFilter:(XeniaTouchLayoutLibraryFilter)filter;

@end

#endif  // XENIA_UI_IOS_TOUCH_LAYOUT_LIBRARY_CONTROLLER_IOS_H_
