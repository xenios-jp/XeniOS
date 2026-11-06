/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */


#import "xenia/ui/ios/launcher/ios_compat_fetch.h"

#import "xenia/ui/ios/launcher/ios_compat_cache.h"
#import "xenia/ui/ios/shared/ios_online_features.h"

static NSString* const kXeCompatETagKey = @"xenios.compat.etag";
static NSString* const kXeCompatCheckTimeKey = @"xenios.compat.lastCheck";
static const NSTimeInterval kXeCompatMinCheckInterval = 6 * 60 * 60;

static void xe_compat_stamp_check_time(void) {
  [[NSUserDefaults standardUserDefaults] setDouble:[[NSDate date] timeIntervalSince1970]
                                            forKey:kXeCompatCheckTimeKey];
}

static NSURLSession* xe_compat_url_session(void) {
  static NSURLSession* session = nil;
  static dispatch_once_t once_token;
  dispatch_once(&once_token, ^{
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration defaultSessionConfiguration];
    config.waitsForConnectivity = YES;
    config.timeoutIntervalForRequest = 20.0;
    config.timeoutIntervalForResource = 45.0;
    session = [[NSURLSession sessionWithConfiguration:config] retain];
  });
  return session;
}
static void xe_fetch_compat_data_from_urls(NSArray<NSString*>* url_strings, NSUInteger index,
                                           NSData* cached,
                                           void (^completion)(NSDictionary* _Nullable data)) {
  if (index >= [url_strings count]) {
    return;
  }

  NSURL* url = [NSURL URLWithString:url_strings[index]];
  if (!url) {
    xe_fetch_compat_data_from_urls(url_strings, index + 1, cached, completion);
    return;
  }

  // Only the live API (index 0) takes part in ETag revalidation.
  NSMutableURLRequest* request =
      [NSMutableURLRequest requestWithURL:url
                              cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                          timeoutInterval:20.0];
  NSString* etag = index == 0 ? [[NSUserDefaults standardUserDefaults] stringForKey:kXeCompatETagKey]
                              : nil;
  if (etag.length > 0) {
    [request setValue:etag forHTTPHeaderField:@"If-None-Match"];
  }

  NSURLSessionDataTask* task = [xe_compat_url_session()
      dataTaskWithRequest:request
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        NSHTTPURLResponse* http_304 = (NSHTTPURLResponse*)response;
        if (!error && etag.length > 0 && [http_304 isKindOfClass:[NSHTTPURLResponse class]] &&
            http_304.statusCode == 304) {
          xe_compat_stamp_check_time();
          return;
        }
        if (error || data.length == 0) {
          xe_fetch_compat_data_from_urls(url_strings, index + 1, cached, completion);
          return;
        }

        NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
        if (![http isKindOfClass:[NSHTTPURLResponse class]] || http.statusCode != 200) {
          xe_fetch_compat_data_from_urls(url_strings, index + 1, cached, completion);
          return;
        }

        NSArray* remote_games = xe_parse_compat_games_array(data);
        if (!remote_games) {
          xe_fetch_compat_data_from_urls(url_strings, index + 1, cached, completion);
          return;
        }
        if (remote_games.count == 0) {
          xe_fetch_compat_data_from_urls(url_strings, index + 1, cached, completion);
          return;
        }

        NSArray* cached_games = xe_parse_compat_games_array(cached);
        NSArray* merged_games = cached_games ? xe_merge_remote_compat_games_with_cached_submissions(
                                                   remote_games, cached_games)
                                             : remote_games;
        BOOL preserved_cached_submission = ![merged_games isEqualToArray:remote_games];
        NSData* data_to_store = data;
        if (preserved_cached_submission) {
          NSError* serialize_error = nil;
          NSData* serialized = [NSJSONSerialization dataWithJSONObject:merged_games
                                                               options:NSJSONWritingPrettyPrinted
                                                                 error:&serialize_error];
          if (!serialized || serialize_error) {
            xe_fetch_compat_data_from_urls(url_strings, index + 1, cached, completion);
            return;
          }
          NSMutableData* writable = [serialized mutableCopy];
          static const char newline = '\n';
          [writable appendBytes:&newline length:1];
          data_to_store = [writable autorelease];
        }

        NSDictionary* result = xe_parse_compat_json(data_to_store);
        if (!result) {
          xe_fetch_compat_data_from_urls(url_strings, index + 1, cached, completion);
          return;
        }

        if (index == 0) {
          NSString* new_etag = [http valueForHTTPHeaderField:@"ETag"];
          NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
          if (new_etag.length > 0) {
            [defaults setObject:new_etag forKey:kXeCompatETagKey];
          } else {
            [defaults removeObjectForKey:kXeCompatETagKey];
          }
          xe_compat_stamp_check_time();
        }

        if (cached.length == 0 || ![cached isEqualToData:data_to_store]) {
          [data_to_store writeToFile:xe_compat_cache_path() atomically:YES];
          dispatch_async(dispatch_get_main_queue(), ^{
            if (completion) {
              completion(result);
            }
          });
        }
      }];
  [task resume];
}

void xe_fetch_compat_data(void (^completion)(NSDictionary* _Nullable data)) {
  NSData* cached = [NSData dataWithContentsOfFile:xe_compat_cache_path()];
  NSDictionary* cached_result = cached.length > 0 ? xe_parse_compat_json(cached) : nil;
  // No usable cache: drop the ETag so the next request is a full 200.
  if (!cached_result) {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kXeCompatETagKey];
  }
  if (cached.length > 0) {
    if (cached_result) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (completion) {
          completion(cached_result);
        }
      });
    }
  }

  // Online features off: the cached list above is all there is.
  if (!xe_ios_online_features_enabled()) {
    return;
  }

  // Checked within the last 6 hours: skip the network.
  NSTimeInterval last_check =
      [[NSUserDefaults standardUserDefaults] doubleForKey:kXeCompatCheckTimeKey];
  NSTimeInterval age = [[NSDate date] timeIntervalSince1970] - last_check;
  if (cached_result && age >= 0 && age < kXeCompatMinCheckInterval) {
    return;
  }

  xe_fetch_compat_data_from_urls(
      @[
        @"https://xenios-compat-api.xenios.workers.dev/games",
        // The static site export can lag behind freshly submitted reports,
        // so use it only if the live API is unavailable.
        @"https://xenios.jp/compatibility/data.json",
      ],
      0, cached, completion);
}
