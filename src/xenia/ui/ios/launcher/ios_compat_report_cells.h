/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_COMPAT_REPORT_CELLS_H_
#define XENIA_UI_IOS_COMPAT_REPORT_CELLS_H_

#import <UIKit/UIKit.h>

#include <cstdint>

@interface XeniaCompatReportCells : NSObject
+ (UITableViewCell*)gameCellWithTitle:(NSString*)title
                              titleID:(uint32_t)title_id
                                  row:(NSInteger)row;
+ (UITableViewCell*)environmentCellForRow:(NSInteger)row;
+ (UITableViewCell*)optionsCellForSection:(NSInteger)section
                           selectedStatus:(NSInteger)selected_status
                             selectedPerf:(NSInteger)selected_perf
                                   target:(id)target
                             statusAction:(SEL)status_action
                               perfAction:(SEL)perf_action;
+ (UITableViewCell*)screenshotCellWithImage:(UIImage*)image index:(NSInteger)index;
+ (UITableViewCell*)addScreenshotCell;
+ (UITableViewCell*)submitCellSubmitting:(BOOL)submitting;
@end

#endif  // XENIA_UI_IOS_COMPAT_REPORT_CELLS_H_
