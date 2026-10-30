/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_art.h"

#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

NSString* xe_game_art_cache_dir(void) {
  static NSString* dir;
  if (!dir) {
    NSString* caches =
        NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    dir = [[caches stringByAppendingPathComponent:@"game-art"] retain];  // MRC
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
  }
  return dir;
}

NSString* xe_game_art_hex(uint32_t title_id) { return XEFormatTitleIDHexLower(title_id); }

NSMutableSet* xe_game_art_inflight_ids(void) {
  static NSMutableSet* inflight;
  if (!inflight) {
    inflight = [[NSMutableSet alloc] init];
  }
  return inflight;
}

NSMutableDictionary* xe_game_art_retry_after(void) {
  static NSMutableDictionary* retry_after;
  if (!retry_after) {
    retry_after = [[NSMutableDictionary alloc] init];
  }
  return retry_after;
}

bool xe_begin_game_art_fetch(NSString* hex) {
  if (!hex.length) {
    return false;
  }
  NSMutableSet* inflight = xe_game_art_inflight_ids();
  if ([inflight containsObject:hex]) {
    return false;
  }
  NSDate* retry_after = [xe_game_art_retry_after() objectForKey:hex];
  if (retry_after && [retry_after timeIntervalSinceNow] > 0) {
    return false;
  }
  [inflight addObject:hex];
  return true;
}

void xe_complete_game_art_fetch(NSString* hex, bool success) {
  if (!hex.length) {
    return;
  }
  [xe_game_art_inflight_ids() removeObject:hex];
  NSMutableDictionary* retry_after = xe_game_art_retry_after();
  if (success) {
    [retry_after removeObjectForKey:hex];
  } else {
    // Throttle repeated failed downloads while still allowing periodic retries.
    [retry_after setObject:[NSDate dateWithTimeIntervalSinceNow:300.0] forKey:hex];
  }
}

NSString* xe_game_background_cache_dir(void) {
  static NSString* dir;
  if (!dir) {
    NSString* caches =
        NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    dir = [[caches stringByAppendingPathComponent:@"game-background-art"] retain];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
  }
  return dir;
}

}  // namespace

UIImage* xe_cached_game_art(uint32_t title_id) {
  if (!title_id) {
    return nil;
  }
  NSString* path = [xe_game_art_cache_dir()
      stringByAppendingPathComponent:[xe_game_art_hex(title_id) stringByAppendingString:@".jpg"]];
  return [UIImage imageWithContentsOfFile:path];
}

UIImage* xe_cached_game_background_art(uint32_t title_id) {
  if (!title_id) {
    return nil;
  }
  NSString* path = [xe_game_background_cache_dir()
      stringByAppendingPathComponent:[xe_game_art_hex(title_id) stringByAppendingString:@".jpg"]];
  return [UIImage imageWithContentsOfFile:path];
}

void xe_fetch_game_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image)) {
  if (!title_id) {
    if (completion) {
      completion(nil);
    }
    return;
  }
  NSString* hex = xe_game_art_hex(title_id);
  if (!xe_begin_game_art_fetch(hex)) {
    return;
  }
  NSString* url_str = [NSString
      stringWithFormat:
          @"https://raw.githubusercontent.com/Element18592/360-Game-Art/main/Games/%@/cover.jpg",
          hex];
  NSURL* url = [NSURL URLWithString:url_str];
  if (!url) {
    dispatch_async(dispatch_get_main_queue(), ^{
      xe_complete_game_art_fetch(hex, false);
      if (completion) {
        completion(nil);
      }
    });
    return;
  }
  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithURL:url
      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        UIImage* image = nil;
        if (!error && data.length > 0) {
          NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
          if ([http isKindOfClass:[NSHTTPURLResponse class]] && http.statusCode == 200) {
            image = [UIImage imageWithData:data];
            if (image) {
              NSString* path = [xe_game_art_cache_dir()
                  stringByAppendingPathComponent:[hex stringByAppendingString:@".jpg"]];
              [data writeToFile:path atomically:YES];
            }
          }
        }
        const bool success = image != nil;
        dispatch_async(dispatch_get_main_queue(), ^{
          xe_complete_game_art_fetch(hex, success);
          if (completion) {
            completion(image);
          }
        });
      }];
  [task resume];
}

void xe_fetch_game_background_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image)) {
  if (!title_id) {
    if (completion) {
      completion(nil);
    }
    return;
  }
  NSString* hex_upper = XEFormatTitleIDHexUpper(title_id);
  NSString* url_str = [NSString stringWithFormat:@"https://raw.githubusercontent.com/xenia-manager/"
                                                 @"x360db/main/titles/%@/artwork/background.jpg",
                                                 hex_upper];
  NSURL* url = [NSURL URLWithString:url_str];
  if (!url) {
    if (completion) {
      completion(nil);
    }
    return;
  }
  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithURL:url
      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        UIImage* image = nil;
        if (!error && data.length > 0) {
          NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
          if ([http isKindOfClass:[NSHTTPURLResponse class]] && http.statusCode == 200) {
            image = [UIImage imageWithData:data];
            if (image) {
              NSString* path = [xe_game_background_cache_dir()
                  stringByAppendingPathComponent:[xe_game_art_hex(title_id)
                                                     stringByAppendingString:@".jpg"]];
              [data writeToFile:path atomically:YES];
            }
          }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
          if (completion) {
            completion(image);
          }
        });
      }];
  [task resume];
}
