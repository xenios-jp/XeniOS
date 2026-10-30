/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_DISCUSSION_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_DISCUSSION_CONTROLLER_H_

#import <Foundation/Foundation.h>

#include <cstdint>

@class XeniaGameCompatibilityDiscussionController;

@protocol XeniaGameCompatibilityDiscussionControllerDelegate <NSObject>
- (void)compatibilityDiscussionControllerDidUpdate:
    (XeniaGameCompatibilityDiscussionController*)controller;
@end

@interface XeniaGameCompatibilityDiscussionController : NSObject
@property(nonatomic, assign) id<XeniaGameCompatibilityDiscussionControllerDelegate> delegate;
@property(nonatomic, readonly) NSArray<NSDictionary*>* reports;
@property(nonatomic, readonly) NSSet<NSNumber*>* expandedReportIndexes;
@property(nonatomic, readonly) NSString* issueURL;
@property(nonatomic, readonly) NSInteger issueNumber;
@property(nonatomic, readonly, getter=isLoading) BOOL loading;
@property(nonatomic, readonly) BOOL showAll;

- (instancetype)initWithTitleID:(uint32_t)title_id compatInfo:(NSDictionary*)compat_info;
- (void)setCompatInfo:(NSDictionary*)compat_info;
- (NSDictionary*)latestReport;
- (void)loadFromCompatibilityData;
- (BOOL)handleDiscussionNotification:(NSNotification*)notification;
- (void)toggleShowAll;
- (void)toggleReportExpandedAtIndex:(NSInteger)report_index;
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_DISCUSSION_CONTROLLER_H_
