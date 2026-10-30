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

static NSURLSession* xe_compat_url_session(void) {
  static NSURLSession* session = nil;
  static dispatch_once_t once_token;
  dispatch_once(&once_token, ^{
    NSURLSessionConfiguration* config = [NSURLSessionConfiguration defaultSessionConfiguration];
    if (@available(iOS 11.0, *)) {
      config.waitsForConnectivity = YES;
    }
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

  NSURLSessionDataTask* task = [xe_compat_url_session()
        dataTaskWithURL:url
      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
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
  if (cached.length > 0) {
    NSDictionary* cached_result = xe_parse_compat_json(cached);
    if (cached_result) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (completion) {
          completion(cached_result);
        }
      });
    }
  }

  xe_fetch_compat_data_from_urls(
      @[
        @"https://xenios-compat-api.xenios.workers.dev/games",
        // The static site export can lag behind freshly submitted self-built reports,
        // so use it only if the live API is unavailable.
        @"https://xenios.jp/compatibility/data.json",
      ],
      0, cached, completion);
}
