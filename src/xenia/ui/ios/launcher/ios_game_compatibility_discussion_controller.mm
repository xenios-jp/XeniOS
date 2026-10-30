/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_compatibility_discussion_controller.h"

#import "xenia/ui/ios/launcher/ios_compat_data.h"

namespace {

constexpr NSInteger kXeniaDiscussionPreviewCount = 3;
constexpr NSTimeInterval kXeniaDiscussionCacheFreshSeconds = 300.0;

}  // namespace

@implementation XeniaGameCompatibilityDiscussionController {
  uint32_t title_id_;
  NSDictionary* compat_info_;
  NSMutableArray<NSDictionary*>* reports_;
  NSMutableSet<NSNumber*>* expanded_report_indexes_;
  NSString* issue_url_;
  NSInteger issue_number_;
  BOOL loading_;
  BOOL show_all_;
}

@synthesize delegate;

- (instancetype)initWithTitleID:(uint32_t)title_id compatInfo:(NSDictionary*)compat_info {
  self = [super init];
  if (self) {
    title_id_ = title_id;
    compat_info_ = [compat_info retain];
    reports_ = [[NSMutableArray alloc] init];
    expanded_report_indexes_ = [[NSMutableSet alloc] init];
    loading_ = YES;
    issue_number_ = 0;
    show_all_ = NO;
  }
  return self;
}

- (void)dealloc {
  [compat_info_ release];
  [reports_ release];
  [expanded_report_indexes_ release];
  [issue_url_ release];
  [super dealloc];
}

- (NSArray<NSDictionary*>*)reports {
  return reports_;
}

- (NSSet<NSNumber*>*)expandedReportIndexes {
  return expanded_report_indexes_;
}

- (NSString*)issueURL {
  return issue_url_;
}

- (NSInteger)issueNumber {
  return issue_number_;
}

- (BOOL)isLoading {
  return loading_;
}

- (BOOL)showAll {
  return show_all_;
}

- (void)setCompatInfo:(NSDictionary*)compat_info {
  if (compat_info_ == compat_info) {
    return;
  }
  [compat_info_ release];
  compat_info_ = [compat_info retain];
}

- (NSDictionary*)latestReport {
  if (reports_.count == 0) {
    return nil;
  }
  id report = reports_.firstObject;
  return [report isKindOfClass:[NSDictionary class]] ? report : nil;
}

- (void)notifyDidUpdate {
  [delegate compatibilityDiscussionControllerDidUpdate:self];
}

- (NSDictionary*)discussionSnapshotFromCompatibilityData {
  if (!compat_info_) {
    return nil;
  }

  NSMutableDictionary* snapshot = [NSMutableDictionary dictionary];
  id reports = compat_info_[@"reports"];
  if ([reports isKindOfClass:[NSArray class]]) {
    snapshot[@"reports"] = reports;
  }

  id issue_url = compat_info_[@"issueUrl"];
  if ([issue_url isKindOfClass:[NSString class]] && [issue_url length] > 0) {
    snapshot[@"issueUrl"] = issue_url;
  }

  id issue_number = compat_info_[@"issueNumber"];
  if ([issue_number isKindOfClass:[NSNumber class]]) {
    snapshot[@"issueNumber"] = issue_number;
  }

  return snapshot.count > 0 ? snapshot : nil;
}

- (BOOL)needsDiscussionNetworkFallback {
  if (!compat_info_) {
    return YES;
  }

  NSArray* reports =
      [compat_info_[@"reports"] isKindOfClass:[NSArray class]] ? compat_info_[@"reports"] : nil;
  BOOL has_reports = reports.count > 0;
  BOOL has_issue_url = [compat_info_[@"issueUrl"] isKindOfClass:[NSString class]] &&
                       [compat_info_[@"issueUrl"] length] > 0;
  BOOL has_issue_number = [compat_info_[@"issueNumber"] isKindOfClass:[NSNumber class]];

  return !has_reports || (!has_issue_url && !has_issue_number);
}

- (void)loadFromCompatibilityData {
  NSDictionary* snapshot = [self discussionSnapshotFromCompatibilityData];
  if (snapshot) {
    [self applyDiscussionJSON:snapshot];
    loading_ = NO;
    [self notifyDidUpdate];
  }

  if ([self needsDiscussionNetworkFallback]) {
    loading_ = YES;
    [self fetchDiscussion];
  } else if (!snapshot) {
    loading_ = NO;
  }
}

- (void)applyDiscussionJSON:(NSDictionary*)json {
  [reports_ removeAllObjects];
  [expanded_report_indexes_ removeAllObjects];

  NSArray* raw_reports = json[@"reports"];
  if ([raw_reports isKindOfClass:[NSArray class]]) {
    for (id item in raw_reports) {
      if ([item isKindOfClass:[NSDictionary class]]) {
        [reports_ addObject:item];
      }
    }
  }

  [issue_url_ release];
  issue_url_ = nil;
  id issue_url = json[@"issueUrl"];
  if ([issue_url isKindOfClass:[NSString class]] && [issue_url length] > 0) {
    issue_url_ = [issue_url copy];
  }

  issue_number_ = 0;
  id issue_number = json[@"issueNumber"];
  if ([issue_number isKindOfClass:[NSNumber class]]) {
    issue_number_ = [issue_number integerValue];
  }

  if (reports_.count <= kXeniaDiscussionPreviewCount) {
    show_all_ = NO;
  }
  if (reports_.count > 0) {
    [expanded_report_indexes_ addObject:@0];
  }
}

- (void)fetchDiscussion {
  NSString* cache_path = xe_discussion_cache_path(title_id_);
  if (reports_.count == 0) {
    NSData* cached_data = [NSData dataWithContentsOfFile:cache_path];
    if (cached_data.length > 0) {
      NSError* cache_error = nil;
      id cached_json = [NSJSONSerialization JSONObjectWithData:cached_data
                                                       options:0
                                                         error:&cache_error];
      if (!cache_error && [cached_json isKindOfClass:[NSDictionary class]]) {
        [self applyDiscussionJSON:(NSDictionary*)cached_json];
        loading_ = NO;
        [self notifyDidUpdate];
      }
    }

    NSDictionary* cache_attributes =
        [[NSFileManager defaultManager] attributesOfItemAtPath:cache_path error:nil];
    NSDate* cache_modified_date = cache_attributes[NSFileModificationDate];
    if (cache_modified_date &&
        [[NSDate date] timeIntervalSinceDate:cache_modified_date] <
            kXeniaDiscussionCacheFreshSeconds &&
        !loading_) {
      return;
    }
  }

  NSString* url_string = [NSString
      stringWithFormat:@"https://xenios-compat-api.xenios.workers.dev/games/%08X/discussion",
                       title_id_];
  NSURL* url = [NSURL URLWithString:url_string];
  if (!url) {
    loading_ = NO;
    [self notifyDidUpdate];
    return;
  }

  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithURL:url
      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        if (error || data.length == 0) {
          dispatch_async(dispatch_get_main_queue(), ^{
            self->loading_ = NO;
            [self notifyDidUpdate];
          });
          return;
        }

        NSHTTPURLResponse* http_response = (NSHTTPURLResponse*)response;
        if (![http_response isKindOfClass:[NSHTTPURLResponse class]] ||
            http_response.statusCode != 200) {
          dispatch_async(dispatch_get_main_queue(), ^{
            self->loading_ = NO;
            [self notifyDidUpdate];
          });
          return;
        }

        NSError* json_error = nil;
        id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:&json_error];
        if (json_error || ![json isKindOfClass:[NSDictionary class]]) {
          dispatch_async(dispatch_get_main_queue(), ^{
            self->loading_ = NO;
            [self notifyDidUpdate];
          });
          return;
        }

        [data writeToFile:cache_path atomically:YES];
        dispatch_async(dispatch_get_main_queue(), ^{
          self->loading_ = NO;
          [self applyDiscussionJSON:(NSDictionary*)json];
          [self notifyDidUpdate];
        });
      }];
  [task resume];
}

- (BOOL)handleDiscussionNotification:(NSNotification*)notification {
  NSNumber* updated_title_id = notification.userInfo[@"titleId"];
  if (![updated_title_id isKindOfClass:[NSNumber class]] ||
      [updated_title_id unsignedIntValue] != title_id_) {
    return NO;
  }

  NSDictionary* snapshot = xe_dictionary_from_object(notification.userInfo[@"discussion"]);
  if (snapshot) {
    [self applyDiscussionJSON:snapshot];
    loading_ = NO;
    [self notifyDidUpdate];
    return YES;
  }

  loading_ = YES;
  [self fetchDiscussion];
  return YES;
}

- (void)toggleShowAll {
  show_all_ = !show_all_;
  [self notifyDidUpdate];
}

- (void)toggleReportExpandedAtIndex:(NSInteger)report_index {
  NSNumber* boxed_index = [NSNumber numberWithInteger:report_index];
  if ([expanded_report_indexes_ containsObject:boxed_index]) {
    [expanded_report_indexes_ removeObject:boxed_index];
  } else {
    [expanded_report_indexes_ addObject:boxed_index];
  }
}

@end
