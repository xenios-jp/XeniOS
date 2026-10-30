/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_COMPAT_REPORT_SUBMISSION_H_
#define XENIA_UI_IOS_COMPAT_REPORT_SUBMISSION_H_

#import <PhotosUI/PhotosUI.h>
#import <UIKit/UIKit.h>

#include <cstdint>

typedef void (^XeniaCompatReportSubmissionCompletion)(NSString* issue_url,
                                                      NSDictionary* compat_info,
                                                      NSDictionary* discussion_snapshot,
                                                      NSString* error_title,
                                                      NSString* error_message);

typedef void (^XeniaCompatReportScreenshotLoadCompletion)(NSArray<UIImage*>* images);

@interface XeniaCompatReportSubmission : NSObject
+ (void)submitReportForTitleID:(uint32_t)title_id
                         title:(NSString*)title
                        status:(NSString*)status
                          perf:(NSString*)perf
                         notes:(NSString*)notes
                   screenshots:(NSArray<UIImage*>*)screenshots
                    completion:(XeniaCompatReportSubmissionCompletion)completion;
+ (void)loadSanitizedScreenshotsFromPickerResults:(NSArray<PHPickerResult*>*)results
                                   availableSlots:(NSUInteger)available_slots
                                       completion:
                                           (XeniaCompatReportScreenshotLoadCompletion)completion;
@end

#endif  // XENIA_UI_IOS_COMPAT_REPORT_SUBMISSION_H_
