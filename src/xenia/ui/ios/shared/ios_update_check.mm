/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_update_check.h"

@implementation XeniaUpdateInfo
- (void)dealloc {
  [_currentVersionText release];
  [_latestVersionString release];
  [_latestVersionText release];
  [_downloadURLString release];
  [super dealloc];
}
@end

namespace {

// Canonical sources. The release page carries the .ipa directly; the downloads
// page is the human-facing fallback if a release has no html_url.
NSString* const kLatestReleaseURL =
    @"https://api.github.com/repos/xenios-jp/XeniOS/releases/latest";
NSString* const kDownloadFallbackURL = @"https://xenios.jp/downloads";

NSString* const kLastCheckTimeKey = @"XeniaUpdateLastCheckTime";
NSString* const kCachedLatestVersionKey = @"XeniaUpdateCachedLatestVersion";
NSString* const kCachedLatestBuildKey = @"XeniaUpdateCachedLatestBuild";
NSString* const kCachedDownloadURLKey = @"XeniaUpdateCachedDownloadURL";

const NSTimeInterval kCheckThrottleSeconds = 24 * 60 * 60;

NSURLSession* UpdateCheckSession() {
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

NSString* FirstMatchGroup(NSString* source, NSString* pattern, NSRegularExpressionOptions options) {
  if (!source.length) {
    return nil;
  }
  NSError* error = nil;
  NSRegularExpression* regex = [NSRegularExpression regularExpressionWithPattern:pattern
                                                                         options:options
                                                                           error:&error];
  if (!regex) {
    return nil;
  }
  NSTextCheckingResult* match = [regex firstMatchInString:source
                                                  options:0
                                                    range:NSMakeRange(0, source.length)];
  if (!match || match.numberOfRanges < 2) {
    return nil;
  }
  NSRange range = [match rangeAtIndex:1];
  if (range.location == NSNotFound) {
    return nil;
  }
  return [source substringWithRange:range];
}

// Pulls the dotted version (e.g. "2.0", "2.0.1") out of a tag or release name.
// Falls back to a bare "vN" when there is no dotted form.
NSString* ExtractVersionString(NSString* source) {
  NSString* dotted = FirstMatchGroup(source, @"v?(\\d+(?:\\.\\d+)+)", 0);
  if (dotted.length) {
    return dotted;
  }
  return FirstMatchGroup(source, @"v(\\d+)", 0);
}

// Pulls the build counter out of "build-<N>" (tag), then "<N>" trailing the
// release name (e.g. "2.0-9712"). Build numbers are large, so require >=3
// digits for the trailing fallback to avoid grabbing a version component.
long long ExtractBuildNumber(NSString* tag, NSString* name) {
  for (NSString* source in @[ tag ?: @"", name ?: @"" ]) {
    NSString* explicit_build =
        FirstMatchGroup(source, @"build[-_]?(\\d+)", NSRegularExpressionCaseInsensitive);
    if (explicit_build.length) {
      return [explicit_build longLongValue];
    }
  }
  NSString* trailing = FirstMatchGroup(name, @"(\\d{3,})\\s*$", 0);
  return trailing.length ? [trailing longLongValue] : 0;
}

long long ParseLeadingInteger(NSString* text) {
  if (![text isKindOfClass:[NSString class]]) {
    return 0;
  }
  return [text longLongValue];
}

NSInteger CompareVersionStrings(NSString* lhs, NSString* rhs) {
  NSArray<NSString*>* lhs_parts = [(lhs ?: @"") componentsSeparatedByString:@"."];
  NSArray<NSString*>* rhs_parts = [(rhs ?: @"") componentsSeparatedByString:@"."];
  NSUInteger count = MAX(lhs_parts.count, rhs_parts.count);
  for (NSUInteger i = 0; i < count; ++i) {
    long long lhs_value = i < lhs_parts.count ? [lhs_parts[i] longLongValue] : 0;
    long long rhs_value = i < rhs_parts.count ? [rhs_parts[i] longLongValue] : 0;
    if (lhs_value != rhs_value) {
      return lhs_value < rhs_value ? -1 : 1;
    }
  }
  return 0;
}

NSString* VersionText(NSString* version, long long build) {
  if (version.length && build > 0) {
    return [NSString stringWithFormat:@"%@ (%lld)", version, build];
  }
  if (version.length) {
    return version;
  }
  if (build > 0) {
    return [NSString stringWithFormat:@"build %lld", build];
  }
  return nil;
}

XeniaUpdateInfo* MakeUpdateInfo(NSString* latest_version, long long latest_build,
                                NSString* download_url) {
  XeniaUpdateInfo* info = [[[XeniaUpdateInfo alloc] init] autorelease];
  info.currentVersionText = xe_current_app_version_text();
  info.latestVersionString = latest_version;
  info.latestBuild = latest_build;
  info.downloadURLString = download_url.length ? download_url : kDownloadFallbackURL;
  info.latestVersionText = VersionText(latest_version, latest_build);
  if (xe_is_development_build()) {
    info.status = XeniaUpdateStatusDevelopmentBuild;
  } else if (latest_version.length || latest_build > 0) {
    NSInteger comparison =
        xe_compare_app_versions(xe_current_app_version_string(), xe_current_app_build_number(),
                                latest_version, latest_build);
    info.status = comparison < 0 ? XeniaUpdateStatusUpdateAvailable : XeniaUpdateStatusUpToDate;
  } else {
    info.status = XeniaUpdateStatusUnknown;
  }
  return info;
}

// Builds an info object from the cached release values (or "unknown" when no
// successful check has ever completed) and delivers it on the main queue.
void DeliverCachedResult(void (^completion)(XeniaUpdateInfo* info)) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* version = [defaults stringForKey:kCachedLatestVersionKey];
  long long build = (long long)[defaults integerForKey:kCachedLatestBuildKey];
  NSString* url = [defaults stringForKey:kCachedDownloadURLKey];
  XeniaUpdateInfo* info = MakeUpdateInfo(version.length ? version : nil, build, url);
  dispatch_async(dispatch_get_main_queue(), ^{
    if (completion) {
      completion(info);
    }
  });
}

}  // namespace

NSString* xe_current_app_version_string(void) {
  id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
  return [value isKindOfClass:[NSString class]] ? value : nil;
}

long long xe_current_app_build_number(void) {
  id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:(NSString*)kCFBundleVersionKey];
  if ([value isKindOfClass:[NSNumber class]]) {
    return [value longLongValue];
  }
  return ParseLeadingInteger(value);
}

NSString* xe_current_app_version_text(void) {
  NSString* text = VersionText(xe_current_app_version_string(), xe_current_app_build_number());
  return text.length ? text : @"Development Build";
}

BOOL xe_is_development_build(void) {
  // The CMake placeholder (1.0 / build 1) means the bundle was not stamped by
  // tools/build_apple_release.sh. Real builds carry the commit-count build
  // number, so a build number of 1 (or 0) marks a local/dev build.
  return xe_current_app_build_number() <= 1;
}

NSInteger xe_compare_app_versions(NSString* lhs_version, long long lhs_build, NSString* rhs_version,
                                  long long rhs_build) {
  NSInteger version_comparison = CompareVersionStrings(lhs_version, rhs_version);
  if (version_comparison != 0) {
    return version_comparison;
  }
  if (lhs_build != rhs_build) {
    return lhs_build < rhs_build ? -1 : 1;
  }
  return 0;
}

void xe_check_for_update(BOOL force_refresh, void (^completion)(XeniaUpdateInfo* info)) {
  // Unstamped local/dev builds can't be meaningfully compared to releases.
  // Report the development status without touching the network.
  if (xe_is_development_build()) {
    XeniaUpdateInfo* info = MakeUpdateInfo(nil, 0, nil);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (completion) {
        completion(info);
      }
    });
    return;
  }

  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];

  if (!force_refresh) {
    double last_check = [defaults doubleForKey:kLastCheckTimeKey];
    double now = [[NSDate date] timeIntervalSince1970];
    if (last_check > 0 && (now - last_check) < kCheckThrottleSeconds &&
        [defaults objectForKey:kCachedLatestVersionKey]) {
      DeliverCachedResult(completion);
      return;
    }
  }

  NSMutableURLRequest* request =
      [NSMutableURLRequest requestWithURL:[NSURL URLWithString:kLatestReleaseURL]];
  // GitHub's API rejects requests without a User-Agent, and the explicit Accept
  // header pins the JSON media type.
  [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
  [request setValue:@"XeniOS-UpdateCheck" forHTTPHeaderField:@"User-Agent"];

  NSURLSessionDataTask* task = [UpdateCheckSession()
      dataTaskWithRequest:request
        completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
          NSHTTPURLResponse* http = [response isKindOfClass:[NSHTTPURLResponse class]]
                                        ? (NSHTTPURLResponse*)response
                                        : nil;
          if (error || !data.length || !http || http.statusCode != 200) {
            DeliverCachedResult(completion);
            return;
          }
          id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
          if (![parsed isKindOfClass:[NSDictionary class]]) {
            DeliverCachedResult(completion);
            return;
          }
          NSDictionary* release = (NSDictionary*)parsed;
          NSString* tag =
              [release[@"tag_name"] isKindOfClass:[NSString class]] ? release[@"tag_name"] : nil;
          NSString* name =
              [release[@"name"] isKindOfClass:[NSString class]] ? release[@"name"] : nil;
          NSString* html_url =
              [release[@"html_url"] isKindOfClass:[NSString class]] ? release[@"html_url"] : nil;

          NSString* latest_version = ExtractVersionString(tag) ?: ExtractVersionString(name);
          long long latest_build = ExtractBuildNumber(tag, name);
          if (!latest_version.length && latest_build == 0) {
            DeliverCachedResult(completion);
            return;
          }

          [defaults setObject:(latest_version ?: @"") forKey:kCachedLatestVersionKey];
          [defaults setInteger:(NSInteger)latest_build forKey:kCachedLatestBuildKey];
          [defaults setObject:(html_url ?: kDownloadFallbackURL) forKey:kCachedDownloadURLKey];
          [defaults setDouble:[[NSDate date] timeIntervalSince1970] forKey:kLastCheckTimeKey];

          XeniaUpdateInfo* info = MakeUpdateInfo(latest_version, latest_build, html_url);
          dispatch_async(dispatch_get_main_queue(), ^{
            if (completion) {
              completion(info);
            }
          });
        }];
  [task resume];
}
