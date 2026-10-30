/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_CELLS_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_CELLS_H_

#import <UIKit/UIKit.h>

@interface XeniaGameCompatibilityCells : NSObject
+ (UITableViewCell*)detailsCellWithCompatInfo:(NSDictionary*)compat_info
                                 latestReport:(NSDictionary*)latest_report;
+ (UITableViewCell*)ctaCellWithTarget:(id)target submitAction:(SEL)submit_action;
+ (UITableViewCell*)discussionCellWithReports:(NSArray<NSDictionary*>*)reports
                        expandedReportIndexes:(NSSet<NSNumber*>*)expanded_report_indexes
                                      loading:(BOOL)loading
                                      showAll:(BOOL)show_all
                                     issueURL:(NSString*)issue_url
                                  issueNumber:(NSInteger)issue_number
                                       target:(id)target
                              viewIssueAction:(SEL)view_issue_action
                        toggleExpansionAction:(SEL)toggle_expansion_action
                           toggleReportAction:(SEL)toggle_report_action;
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_GAME_COMPATIBILITY_CELLS_H_
