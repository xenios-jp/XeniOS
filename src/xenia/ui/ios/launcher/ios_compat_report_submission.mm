/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_compat_report_submission.h"

#include "xenia/base/logging.h"
#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

void xe_finish_report_submission(XeniaCompatReportSubmissionCompletion completion,
                                 NSString* issue_url, NSDictionary* compat_info,
                                 NSDictionary* discussion_snapshot, NSString* error_title,
                                 NSString* error_message) {
  if (!completion) {
    return;
  }
  completion(issue_url, compat_info, discussion_snapshot, error_title, error_message);
}

UIImage* xe_sanitized_report_screenshot(UIImage* image) {
  if (!image) {
    return nil;
  }

  CGFloat max_dimension = 1280.0;
  CGSize size = image.size;
  CGFloat scale = 1.0;
  if (size.width > max_dimension || size.height > max_dimension) {
    scale = (size.width > size.height) ? (max_dimension / size.width)
                                       : (max_dimension / size.height);
  }
  CGSize new_size = CGSizeMake(size.width * scale, size.height * scale);
  UIGraphicsBeginImageContextWithOptions(new_size, NO, 1.0);
  [image drawInRect:CGRectMake(0, 0, new_size.width, new_size.height)];
  UIImage* sanitized_image = UIGraphicsGetImageFromCurrentImageContext();
  UIGraphicsEndImageContext();
  return sanitized_image;
}

}  // namespace

@implementation XeniaCompatReportSubmission

+ (void)submitReportForTitleID:(uint32_t)title_id
                          title:(NSString*)title
                         status:(NSString*)status
                           perf:(NSString*)perf
                          notes:(NSString*)notes
                    screenshots:(NSArray<UIImage*>*)screenshots
                     completion:(XeniaCompatReportSubmissionCompletion)completion {
  if (!xe_ios_online_features_enabled()) {
    xe_finish_report_submission(completion, nil, nil, nil,
                                NSLocalizedString(@"Online Features Are Off",
                                                  @"compat report submission: user-visible text"),
                                xe_ios_online_features_off_message());
    return;
  }

  NSMutableArray<NSString*>* screenshot_data =
      [NSMutableArray arrayWithCapacity:screenshots.count];
  NSUInteger screenshot_total_bytes = 0;
  for (UIImage* image in screenshots) {
    NSData* jpeg = UIImageJPEGRepresentation(image, 0.8);
    if (!jpeg) {
      continue;
    }
    screenshot_total_bytes += jpeg.length;
    [screenshot_data addObject:[jpeg base64EncodedStringWithOptions:0]];
  }

  NSString* device_machine = xe_device_machine();
  NSString* device_display = xe_device_display_name();
  NSDictionary* build_info = xe_current_compat_report_build_info();
  NSDictionary* payload = @{
    @"titleId" : XEFormatTitleIDHexUpper(title_id),
    @"title" : title ?: @"",
    @"status" : status ?: @"",
    @"perf" : perf ?: @"",
    @"platform" : @"ios",
    @"device" : device_display ?: @"Unknown",
    @"deviceMachine" : device_machine ?: @"Unknown",
    @"osVersion" : [UIDevice currentDevice].systemVersion ?: @"",
    @"arch" : @"arm64",
    @"gpuBackend" : @"msl",
    @"notes" : notes ?: @"",
    @"tags" : @[],
    @"screenshots" : screenshot_data,
    @"build" : build_info,
  };

  NSError* json_error = nil;
  NSData* request_body = [NSJSONSerialization dataWithJSONObject:payload
                                                         options:0
                                                           error:&json_error];
  if (!request_body) {
    NSString* message = json_error.localizedDescription
                            ?: NSLocalizedString(@"Unable to serialize the report payload.",
                                                 @"compat report submission: message");
    xe_finish_report_submission(
        completion, nil, nil, nil,
        NSLocalizedString(@"Submission Failed", @"compat report submission: user-visible text"),
        message);
    return;
  }

  XELOGI("iOS compat submit: title_id={:08X} status={} perf={} build={} "
         "commit={} screenshots={} screenshot_bytes={} body_bytes={}",
         title_id, [status UTF8String], [perf UTF8String],
         [xe_compat_build_label(build_info) UTF8String],
         [xe_string_from_object(build_info[@"commitShort"]) UTF8String], (int)screenshot_data.count,
         static_cast<unsigned long long>(screenshot_total_bytes),
         static_cast<unsigned long long>(request_body.length));

  NSURL* url = [NSURL URLWithString:@"https://xenios-compat-api.xenios.workers.dev/report"];
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
  request.HTTPMethod = @"POST";
  [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  [request setValue:@"Bearer xenios-compat-report" forHTTPHeaderField:@"Authorization"];
  request.HTTPBody = request_body;

  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
      dataTaskWithRequest:request
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
          dispatch_async(dispatch_get_main_queue(), ^{
            if (error) {
              XELOGW("iOS compat submit: network error domain={} code={}",
                     [error.domain UTF8String], static_cast<long>(error.code));
              xe_finish_report_submission(
                  completion, nil, nil, nil,
                  NSLocalizedString(@"Couldn't Send Report",
                                    @"compat report submission: user-visible text"),
                  NSLocalizedString(@"XeniOS couldn't reach the report server. Check your "
                                    @"connection and try again.",
                                    @"compat report submission: user-visible text"));
              return;
            }

            NSHTTPURLResponse* http_response = (NSHTTPURLResponse*)response;
            NSInteger status_code = [http_response isKindOfClass:[NSHTTPURLResponse class]]
                                        ? http_response.statusCode
                                        : 0;

            if (![http_response isKindOfClass:[NSHTTPURLResponse class]] || status_code < 200 ||
                status_code >= 300) {
              // The status goes to the log; the server's reply text is not shown to the user.
              XELOGW("iOS compat submit: server returned HTTP {} ({} byte reply)",
                     static_cast<long>(status_code), static_cast<unsigned long>(data.length));
              NSString* message =
                  status_code == 429
                      ? NSLocalizedString(
                            @"Too many reports were sent recently. Please try again later.",
                            @"compat report submission: message")
                  : (status_code >= 400 && status_code < 500)
                      ? NSLocalizedString(
                            @"The server didn't accept this report. Please try again later.",
                            @"compat report submission: user-visible text")
                      : NSLocalizedString(
                            @"The report server is having trouble right now. Please try again "
                            @"later.",
                            @"compat report submission: user-visible text");
              xe_finish_report_submission(
                  completion, nil, nil, nil,
                  NSLocalizedString(@"Couldn't Send Report",
                                    @"compat report submission: user-visible text"),
                  message);
              return;
            }

            NSString* issue_url = nil;
            if (data.length > 0) {
              id response_json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
              if ([response_json isKindOfClass:[NSDictionary class]] &&
                  [response_json[@"issueUrl"] isKindOfClass:[NSString class]]) {
                issue_url = response_json[@"issueUrl"];
                // The link is opened later; only accept an https URL.
                NSURL* parsed_issue_url = [NSURL URLWithString:issue_url];
                if (![parsed_issue_url.scheme isEqualToString:@"https"]) {
                  issue_url = nil;
                }
              }
            }

            NSDictionary* local_report =
                xe_build_local_compat_report(device_display, device_machine,
                                             [UIDevice currentDevice].systemVersion, status, perf,
                                             notes, build_info);
            NSDictionary* compat_info =
                xe_update_cached_compat_entry_for_submission(title_id, title, local_report,
                                                             issue_url);
            NSDictionary* discussion_snapshot =
                xe_cache_discussion_snapshot_for_submission(title_id, compat_info);

            xe_finish_report_submission(completion, issue_url, compat_info, discussion_snapshot,
                                        nil, nil);
          });
        }];
  [task resume];
}

+ (void)loadSanitizedScreenshotsFromPickerResults:(NSArray<PHPickerResult*>*)results
                                  availableSlots:(NSUInteger)available_slots
                                      completion:
                                          (XeniaCompatReportScreenshotLoadCompletion)completion {
  if (results.count == 0 || available_slots == 0) {
    if (completion) {
      completion(@[]);
    }
    return;
  }

  NSUInteger count = MIN(results.count, available_slots);
  NSMutableArray<UIImage*>* images = [[NSMutableArray alloc] initWithCapacity:count];
  dispatch_group_t group = dispatch_group_create();

  for (NSUInteger index = 0; index < count; ++index) {
    PHPickerResult* result = results[index];
    dispatch_group_enter(group);
    [result.itemProvider
        loadObjectOfClass:[UIImage class]
        completionHandler:^(id<NSItemProviderReading> object, NSError* __unused error) {
          UIImage* sanitized_image = xe_sanitized_report_screenshot((UIImage*)object);
          if (sanitized_image) {
            @synchronized(images) {
              [images addObject:sanitized_image];
            }
          }
          dispatch_group_leave(group);
        }];
  }

  dispatch_group_notify(group, dispatch_get_main_queue(), ^{
    if (completion) {
      completion(images);
    }
    [images release];
  });
}

@end
