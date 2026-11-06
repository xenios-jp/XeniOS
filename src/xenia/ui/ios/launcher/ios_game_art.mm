/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_game_art.h"

#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

// Longest side, in pixels, of a cover stored on disk. Anything larger is
// downsampled before it is written.
constexpr CGFloat kMaxStoredArtPixels = 1024.0;
// Covers not in the library and untouched for this long are pruned.
constexpr NSTimeInterval kArtPruneAge = 30.0 * 24.0 * 3600.0;
// How long a title the art repository has no image for waits before asking again.
constexpr NSTimeInterval kArtMissingRetryAge = 7.0 * 24.0 * 3600.0;

NSString* xe_art_cache_dir_named(NSString* name) {
  NSString* caches =
      NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
  NSString* dir = [caches stringByAppendingPathComponent:name];
  [[NSFileManager defaultManager] createDirectoryAtPath:dir
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return dir;
}

// The directory statics are first touched from the main thread and from the
// NSURLSession and image queues, so they initialise through dispatch_once.
NSString* xe_game_art_cache_dir(void) {
  static NSString* dir;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    dir = [xe_art_cache_dir_named(@"game-art") retain];  // MRC
  });
  return dir;
}

NSString* xe_game_background_cache_dir(void) {
  static NSString* dir;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    dir = [xe_art_cache_dir_named(@"game-background-art") retain];
  });
  return dir;
}

NSString* xe_game_art_hex(uint32_t title_id) { return XEFormatTitleIDHexLower(title_id); }

NSString* xe_art_path(NSString* dir, uint32_t title_id) {
  return [dir stringByAppendingPathComponent:[xe_game_art_hex(title_id)
                                                 stringByAppendingString:@".jpg"]];
}

// Pre-decoded, downsampled covers keyed by title id. Filled off the main
// thread; NSCache evicts under pressure and the memory warning observer
// below empties it outright.
NSCache<NSNumber*, UIImage*>* xe_art_thumbnail_cache(void) {
  static NSCache<NSNumber*, UIImage*>* cache;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    cache = [[NSCache alloc] init];
    cache.name = @"xenia.game-art.thumbnails";
    cache.countLimit = 160;
    [[NSNotificationCenter defaultCenter]
        addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* __unused note) {
                  [cache removeAllObjects];
                }];
  });
  return cache;
}

dispatch_queue_t xe_art_decode_queue(void) {
  static dispatch_queue_t queue;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0);
    queue = dispatch_queue_create("xenia.game-art.decode", attr);
  });
  return queue;
}

// Reads and decodes `source` at no more than `max_pixels` on the longest side.
// The bitmap is decoded immediately so the first draw on the main thread does
// not pay for it.
UIImage* xe_art_decode_thumbnail(CGImageSourceRef source, CGFloat max_pixels) {
  if (!source || CGImageSourceGetCount(source) == 0) {
    return nil;
  }
  NSDictionary* options = @{
    (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
    (id)kCGImageSourceCreateThumbnailWithTransform : @YES,
    (id)kCGImageSourceShouldCacheImmediately : @YES,
    (id)kCGImageSourceThumbnailMaxPixelSize : @(max_pixels),
  };
  CGImageRef cg = CGImageSourceCreateThumbnailAtIndex(source, 0, (CFDictionaryRef)options);
  if (!cg) {
    return nil;
  }
  UIImage* image = [UIImage imageWithCGImage:cg scale:1.0 orientation:UIImageOrientationUp];
  CGImageRelease(cg);
  return image;
}

// Reads the pixel size from the header only.
bool xe_art_header_size(CGImageSourceRef source, CGSize* size_out) {
  if (!source || CGImageSourceGetCount(source) == 0 ||
      CGImageSourceGetStatus(source) != kCGImageStatusComplete) {
    return false;
  }
  NSDictionary* props =
      CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
  NSNumber* w = props[(id)kCGImagePropertyPixelWidth];
  NSNumber* h = props[(id)kCGImagePropertyPixelHeight];
  if (w.doubleValue <= 0 || h.doubleValue <= 0) {
    return false;
  }
  *size_out = CGSizeMake(w.doubleValue, h.doubleValue);
  return true;
}

// Validates downloaded bytes with an ImageIO header check (no full decode),
// downsamples oversized art, and writes the result to `path`. Returns the
// decoded image, or nil when the bytes are not an image.
UIImage* xe_art_store_download(NSData* data, NSString* path) {
  CGImageSourceRef source = CGImageSourceCreateWithData((CFDataRef)data, nullptr);
  if (!source) {
    return nil;
  }
  CGSize size = CGSizeZero;
  UIImage* image = nil;
  if (xe_art_header_size(source, &size)) {
    if (MAX(size.width, size.height) > kMaxStoredArtPixels) {
      image = xe_art_decode_thumbnail(source, kMaxStoredArtPixels);
      if (image) {
        NSMutableData* jpeg = [NSMutableData data];
        CGImageDestinationRef dest = CGImageDestinationCreateWithData(
            (CFMutableDataRef)jpeg, (CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
        if (dest) {
          CGImageDestinationAddImage(
              dest, image.CGImage,
              (CFDictionaryRef) @{(id)kCGImageDestinationLossyCompressionQuality : @0.85});
          if (CGImageDestinationFinalize(dest)) {
            [jpeg writeToFile:path atomically:YES];
          }
          CFRelease(dest);
        }
      }
    } else {
      image = xe_art_decode_thumbnail(source, kMaxStoredArtPixels);
      if (image) {
        [data writeToFile:path atomically:YES];
      }
    }
  }
  CFRelease(source);
  return image;
}

// Fetch bookkeeping. Everything below is touched on the main thread only.
// Keys are "cover:<hex>" / "background:<hex>" so the two fetchers share one
// throttle without colliding.
NSMutableDictionary<NSString*, NSMutableArray*>* xe_art_waiters(void) {
  static NSMutableDictionary* waiters;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    waiters = [[NSMutableDictionary alloc] init];
  });
  return waiters;
}

NSMutableDictionary<NSString*, NSDate*>* xe_art_retry_after(void) {
  static NSMutableDictionary* retry_after;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    retry_after = [[NSMutableDictionary alloc] init];
  });
  return retry_after;
}

void xe_art_call_all(NSArray* completions, UIImage* image) {
  for (void (^completion)(UIImage*) in completions) {
    completion(image);
  }
}

// Shared cover/background fetcher. Duplicate requests for a key that is in
// flight, or recently failed, are folded into the same waiter list and every
// completion is invoked exactly once on the main thread (nil on failure).
void xe_fetch_art(NSString* key, NSURL* url, NSString* dir, uint32_t title_id,
                  void (^completion)(UIImage* _Nullable image)) {
  if (![NSThread isMainThread]) {
    void (^copied)(UIImage*) = [completion copy];
    dispatch_async(dispatch_get_main_queue(), ^{
      xe_fetch_art(key, url, dir, title_id, copied);
    });
    [copied release];
    return;
  }
  if (!url) {
    if (completion) {
      completion(nil);
    }
    return;
  }
  NSMutableDictionary* waiters = xe_art_waiters();
  NSMutableArray* pending = waiters[key];
  if (pending) {
    if (completion) {
      [pending addObject:[[completion copy] autorelease]];
    }
    return;
  }
  NSDate* retry_after = xe_art_retry_after()[key];
  NSString* path = xe_art_path(dir, title_id);
  // The art repository has no image for some titles: a 404 is remembered on
  // disk, so those aren't asked for again on every launch.
  NSString* missing_path = [path.stringByDeletingPathExtension
      stringByAppendingPathExtension:@"missing"];
  NSDate* missing_since = [[NSFileManager defaultManager] attributesOfItemAtPath:missing_path
                                                                           error:nil]
                              .fileModificationDate;
  if ((retry_after && [retry_after timeIntervalSinceNow] > 0) ||
      (missing_since && -[missing_since timeIntervalSinceNow] < kArtMissingRetryAge)) {
    if (completion) {
      completion(nil);
    }
    return;
  }
  pending = [NSMutableArray array];
  if (completion) {
    [pending addObject:[[completion copy] autorelease]];
  }
  waiters[key] = pending;

  NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithURL:url
      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        UIImage* image = nil;
        NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
        const bool is_http = !error && [http isKindOfClass:[NSHTTPURLResponse class]];
        if (is_http && http.statusCode == 200 && data.length > 0) {
          image = xe_art_store_download(data, path);
          if (image) {
            [[NSFileManager defaultManager] removeItemAtPath:missing_path error:nil];
          }
        } else if (is_http && http.statusCode == 404) {
          [[NSData data] writeToFile:missing_path atomically:YES];
        }
        // A dropped or timed-out connection (cellular, a tunnel) is worth asking again soon; a
        // server that answered without the image, much later.
        const NSTimeInterval retry_delay = is_http ? 300.0 : 30.0;
        [image retain];
        dispatch_async(dispatch_get_main_queue(), ^{
          NSArray* completions = [waiters[key] retain];
          [waiters removeObjectForKey:key];
          if (image) {
            [xe_art_retry_after() removeObjectForKey:key];
            [xe_art_thumbnail_cache() removeObjectForKey:@(title_id)];
          } else {
            // Throttle repeated failed downloads while still allowing
            // periodic retries.
            xe_art_retry_after()[key] = [NSDate dateWithTimeIntervalSinceNow:retry_delay];
          }
          xe_art_call_all(completions, image);
          [completions release];
          [image release];
        });
      }];
  [task resume];
}

}  // namespace

UIImage* xe_cached_game_art(uint32_t title_id) {
  if (!title_id) {
    return nil;
  }
  return [UIImage imageWithContentsOfFile:xe_art_path(xe_game_art_cache_dir(), title_id)];
}

UIImage* xe_cached_game_background_art(uint32_t title_id) {
  if (!title_id) {
    return nil;
  }
  return [UIImage imageWithContentsOfFile:xe_art_path(xe_game_background_cache_dir(), title_id)];
}

BOOL xe_has_cached_game_background_art(uint32_t title_id) {
  return title_id && [[NSFileManager defaultManager]
                         fileExistsAtPath:xe_art_path(xe_game_background_cache_dir(), title_id)];
}

UIImage* xe_cached_game_art_thumbnail(uint32_t title_id) {
  if (!title_id) {
    return nil;
  }
  return [xe_art_thumbnail_cache() objectForKey:@(title_id)];
}

void xe_load_game_art_thumbnail(uint32_t title_id, CGFloat max_pixels,
                                void (^completion)(UIImage* _Nullable image)) {
  void (^copied)(UIImage*) = [completion copy];
  if (!title_id) {
    if (copied) {
      copied(nil);
    }
    [copied release];
    return;
  }
  NSString* path = xe_art_path(xe_game_art_cache_dir(), title_id);
  dispatch_async(xe_art_decode_queue(), ^{
    UIImage* image = nil;
    NSURL* file = [NSURL fileURLWithPath:path];
    CGImageSourceRef source = CGImageSourceCreateWithURL((CFURLRef)file, nullptr);
    if (source) {
      image = xe_art_decode_thumbnail(source, MAX(max_pixels, 64.0));
      CFRelease(source);
    }
    if (image) {
      [xe_art_thumbnail_cache() setObject:image forKey:@(title_id)];
      // Pruning goes by when a cover was last shown, not when it was downloaded.
      [[NSFileManager defaultManager] setAttributes:@{NSFileModificationDate : [NSDate date]}
                                       ofItemAtPath:path
                                              error:nil];
    }
    [image retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      if (copied) {
        copied(image);
      }
      [copied release];
      [image release];
    });
  });
}

void xe_purge_game_art_cache(void) { [xe_art_thumbnail_cache() removeAllObjects]; }

void xe_prune_game_art_cache(NSSet<NSNumber*>* keep_title_ids) {
  NSSet* keep = [keep_title_ids copy];
  dispatch_async(xe_art_decode_queue(), ^{
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* dir = xe_game_art_cache_dir();
    NSDate* cutoff = [NSDate dateWithTimeIntervalSinceNow:-kArtPruneAge];
    for (NSString* name in [fm contentsOfDirectoryAtPath:dir error:nil]) {
      NSString* stem = name.stringByDeletingPathExtension;
      unsigned int value = 0;
      if (![[NSScanner scannerWithString:stem] scanHexInt:&value] ||
          [keep containsObject:@(value)]) {
        continue;
      }
      NSString* path = [dir stringByAppendingPathComponent:name];
      NSDate* modified = [fm attributesOfItemAtPath:path error:nil].fileModificationDate;
      if (modified && [modified compare:cutoff] == NSOrderedAscending) {
        [fm removeItemAtPath:path error:nil];
      }
    }
    [keep release];
  });
}

void xe_fetch_game_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image)) {
  if (!title_id || !xe_ios_online_features_enabled()) {
    if (completion) {
      completion(nil);
    }
    return;
  }
  NSString* hex = xe_game_art_hex(title_id);
  NSString* url_str = [NSString
      stringWithFormat:
          @"https://raw.githubusercontent.com/Element18592/360-Game-Art/main/Games/%@/cover.jpg",
          hex];
  xe_fetch_art([@"cover:" stringByAppendingString:hex], [NSURL URLWithString:url_str],
               xe_game_art_cache_dir(), title_id, completion);
}

void xe_fetch_game_background_art(uint32_t title_id, void (^completion)(UIImage* _Nullable image)) {
  if (!title_id || !xe_ios_online_features_enabled()) {
    if (completion) {
      completion(nil);
    }
    return;
  }
  NSString* url_str = [NSString stringWithFormat:@"https://raw.githubusercontent.com/xenia-manager/"
                                                 @"x360db/main/titles/%@/artwork/background.jpg",
                                                 XEFormatTitleIDHexUpper(title_id)];
  xe_fetch_art([@"background:" stringByAppendingString:xe_game_art_hex(title_id)],
               [NSURL URLWithString:url_str], xe_game_background_cache_dir(), title_id,
               completion);
}
