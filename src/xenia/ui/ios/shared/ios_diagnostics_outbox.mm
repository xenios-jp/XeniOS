/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_diagnostics_outbox.h"

#include <sys/sysctl.h>
#include <zlib.h>

#include <string>

#include "xenia/base/logging.h"
#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/shared/ios_fault_record.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_session_stats.h"

namespace {

constexpr NSUInteger kMaxRecords = 50;
constexpr unsigned long long kMaxOutboxBytes = 2ull << 20;
constexpr NSTimeInterval kMaxRecordAge = 14 * 24 * 3600;
// The server takes 256 KB compressed; a larger JSON record isn't worth it.
constexpr NSUInteger kMaxRecordBytes = 256 * 1024;
constexpr NSUInteger kMaxUploadsPerRun = 20;
constexpr NSTimeInterval kFirstRetryDelay = 15 * 60;
constexpr NSTimeInterval kMaxRetryDelay = 24 * 3600;

NSString* const kEndpointInfoKey = @"XeniOSDiagnosticsURL";
// For testing against a local server: `defaults write` on a simulator, or a
// test harness.
NSString* const kEndpointOverrideKey = @"XeniaDiagnosticsURL";
NSString* const kRetryAfterKey = @"XeniaDiagnosticsRetryAfter";
NSString* const kRetryDelayKey = @"XeniaDiagnosticsRetryDelay";

dispatch_queue_t Queue() {
  static dispatch_queue_t queue =
      dispatch_queue_create("jp.xenios.diagnostics-outbox", DISPATCH_QUEUE_SERIAL);
  return queue;
}

NSString* OutboxDirectory() {
  return [NSHomeDirectory()
      stringByAppendingPathComponent:@"Library/Application Support/XeniOS/Outbox"];
}

// Outbox queue only.
BOOL g_uploading = NO;

NSString* SysctlString(const char* name) {
  size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || !size) {
    return nil;
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return nil;
  }
  return [NSString stringWithUTF8String:value.c_str()];
}

// "iPhone17,1" (hw.machine on iOS; a Mac has its model in hw.model).
NSString* DeviceModel() {
  NSString* machine = SysctlString("hw.machine");
  if ([machine containsString:@","]) {
    return machine;
  }
  return SysctlString("hw.model") ?: @"unknown";
}

NSString* OSVersion() {
  NSOperatingSystemVersion v = [NSProcessInfo processInfo].operatingSystemVersion;
  return v.patchVersion
             ? [NSString stringWithFormat:@"%ld.%ld.%ld", (long)v.majorVersion,
                                          (long)v.minorVersion, (long)v.patchVersion]
             : [NSString stringWithFormat:@"%ld.%ld", (long)v.majorVersion, (long)v.minorVersion];
}

NSDictionary* BuildInfo() {
  static NSDictionary* build = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSDictionary* compat = xe_current_compat_report_build_info();
    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    for (NSString* key in @[ @"appVersion", @"buildNumber", @"commitShort" ]) {
      if (compat[key]) {
        info[key] = compat[key];
      }
    }
    if (NSString* uuid = xe_ios_fault_record_binary_uuid()) {
      info[@"binaryUUID"] = uuid;
    }
    build = [info copy];
  });
  return build;
}

id RedactValue(id value) {
  if ([value isKindOfClass:[NSString class]]) {
    return xe_ios_diagnostics_redact(value);
  }
  if ([value isKindOfClass:[NSArray class]]) {
    NSMutableArray* result = [NSMutableArray arrayWithCapacity:[value count]];
    for (id item in value) {
      [result addObject:RedactValue(item)];
    }
    return result;
  }
  if ([value isKindOfClass:[NSDictionary class]]) {
    NSMutableDictionary* result = [NSMutableDictionary dictionaryWithCapacity:[value count]];
    for (id key in value) {
      result[key] = RedactValue(value[key]);
    }
    return result;
  }
  return value;
}

NSData* Gzip(NSData* data) {
  z_stream stream = {};
  if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
      Z_OK) {
    return nil;
  }
  NSMutableData* out = [NSMutableData dataWithLength:deflateBound(&stream, data.length)];
  stream.next_in = static_cast<Bytef*>(const_cast<void*>(data.bytes));
  stream.avail_in = static_cast<uInt>(data.length);
  stream.next_out = static_cast<Bytef*>(out.mutableBytes);
  stream.avail_out = static_cast<uInt>(out.length);
  const int result = deflate(&stream, Z_FINISH);
  out.length = stream.total_out;
  deflateEnd(&stream);
  return result == Z_STREAM_END ? out : nil;
}

// Oldest first: names start with the creation time in ms.
NSArray<NSString*>* SortedRecordNames() {
  NSArray<NSString*>* names =
      [[NSFileManager defaultManager] contentsOfDirectoryAtPath:OutboxDirectory() error:nil];
  return [[names filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.json'"]]
      sortedArrayUsingSelector:@selector(compare:)];
}

void PruneLocked() {
  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* directory = OutboxDirectory();
  NSMutableArray<NSString*>* names = [[SortedRecordNames() mutableCopy] autorelease];
  unsigned long long total = 0;
  NSMutableArray<NSNumber*>* sizes = [NSMutableArray array];
  for (NSInteger i = static_cast<NSInteger>(names.count) - 1; i >= 0; --i) {
    NSString* path = [directory stringByAppendingPathComponent:names[i]];
    NSDictionary* attributes = [fm attributesOfItemAtPath:path error:nil];
    if (!attributes || -[attributes.fileModificationDate timeIntervalSinceNow] > kMaxRecordAge) {
      [fm removeItemAtPath:path error:nil];
      [names removeObjectAtIndex:i];
      continue;
    }
    [sizes insertObject:@(attributes.fileSize) atIndex:0];
    total += attributes.fileSize;
  }
  for (NSUInteger i = 0; i < names.count && (names.count - i > kMaxRecords || total > kMaxOutboxBytes);
       ++i) {
    [fm removeItemAtPath:[directory stringByAppendingPathComponent:names[i]] error:nil];
    total -= sizes[i].unsignedLongLongValue;
  }
}

}  // namespace

// Reports go to the one endpoint and nowhere else: a redirect (a captive
// portal, an Access login page) is not followed, so its 3xx reaches the upload
// code as a failure to retry later.
@interface XeniaDiagnosticsSessionDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation XeniaDiagnosticsSessionDelegate
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
  completionHandler(nil);
}
@end

namespace {

NSURLSession* Session() {
  static NSURLSession* session = [] {
    NSURLSessionConfiguration* configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    // Low Data Mode means no diagnostics.
    configuration.allowsConstrainedNetworkAccess = NO;
    configuration.timeoutIntervalForRequest = 30;
    configuration.timeoutIntervalForResource = 60;
    configuration.HTTPShouldSetCookies = NO;
    configuration.URLCache = nil;
    XeniaDiagnosticsSessionDelegate* delegate =
        [[[XeniaDiagnosticsSessionDelegate alloc] init] autorelease];
    return [[NSURLSession sessionWithConfiguration:configuration
                                          delegate:delegate
                                     delegateQueue:nil] retain];
  }();
  return session;
}

BOOL UploadAllowedNow() {
  return xe_ios_diagnostics_upload_allowed() && !xe_ios_session_stats_running();
}

void NoteUploadFailedLocked() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  const double previous = [defaults doubleForKey:kRetryDelayKey];
  const double delay = previous > 0 ? std::min(previous * 2, kMaxRetryDelay) : kFirstRetryDelay;
  [defaults setDouble:delay forKey:kRetryDelayKey];
  [defaults setObject:[NSDate dateWithTimeIntervalSinceNow:delay] forKey:kRetryAfterKey];
}

void SendNextLocked(NSArray<NSString*>* names, NSUInteger index, NSURL* url) {
  if (index >= names.count || index >= kMaxUploadsPerRun || !UploadAllowedNow()) {
    g_uploading = NO;
    return;
  }
  NSString* path = [OutboxDirectory() stringByAppendingPathComponent:names[index]];
  NSData* json = [NSData dataWithContentsOfFile:path];
  NSData* body = json ? Gzip(json) : nil;
  if (!body) {
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    SendNextLocked(names, index + 1, url);
    return;
  }
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
  request.HTTPMethod = @"POST";
  [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  [request setValue:@"gzip" forHTTPHeaderField:@"Content-Encoding"];
  request.HTTPBody = body;
  NSURLSessionDataTask* task = [Session()
      dataTaskWithRequest:request
        completionHandler:^(NSData* __unused data, NSURLResponse* response,
                            NSError* __unused error) {
          const NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]]
                                       ? static_cast<NSHTTPURLResponse*>(response).statusCode
                                       : 0;
          dispatch_async(Queue(), ^{
            // Accepted (the Worker answers 204), or rejected for good
            // (malformed, too large): either way it's done. Anything else,
            // including a 200 from a proxy or captive portal answering for the
            // server, or a redirect: later.
            const bool done = status == 204 || status == 400 || status == 413 || status == 415 ||
                              status == 422;
            if (!done) {
              XELOGW("iOS: Diagnostics upload failed ({}), retrying later", status);
              NoteUploadFailedLocked();
              g_uploading = NO;
              return;
            }
            [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
            NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
            [defaults removeObjectForKey:kRetryDelayKey];
            [defaults removeObjectForKey:kRetryAfterKey];
            SendNextLocked(names, index + 1, url);
          });
        }];
  [task resume];
}

void UploadLocked() {
  if (g_uploading || !UploadAllowedNow()) {
    return;
  }
  NSURL* url = xe_ios_diagnostics_endpoint();
  if (!url) {
    return;
  }
  NSDate* retry_after = [[NSUserDefaults standardUserDefaults] objectForKey:kRetryAfterKey];
  if ([retry_after isKindOfClass:[NSDate class]] && retry_after.timeIntervalSinceNow > 0) {
    return;
  }
  NSArray<NSString*>* names = SortedRecordNames();
  if (!names.count) {
    return;
  }
  g_uploading = YES;
  SendNextLocked(names, 0, url);
}

}  // namespace

NSDictionary* xe_ios_diagnostics_envelope_header(void) {
  return @{
    @"build" : BuildInfo(),
    @"device" : @{
      @"model" : DeviceModel(),
      @"os" : OSVersion(),
    },
  };
}

// A file name may hold spaces ("Halo 3 Reach.iso") and a relative path may
// start anywhere ("Alice Games/Halo/default.xex"), so the file rule replaces the
// whole run of words up to the extension, from the first word that isn't plain
// message text (kFileContextWords). Kept in step with the Worker's scrub()
// (tools/ios/diagnostics/worker/lib.mjs).
#define XE_FILE_EXTENSIONS \
  "iso|xex|xexp|xbe|xcp|zar|god|stfs|svod|bin|dat|pkg|rar|zip|7z|cue|img|ini|toml|log|txt|json|plist|xml"
#define XE_FILE_CONTEXT_WORDS                                                                   \
  "a|an|the|this|that|to|of|in|on|at|from|for|with|by|and|or|not|no|is|are|was|were|be|been|"  \
  "can|cannot|can't|could|couldn't|couldnt|unable|failed|failure|fail|error|invalid|missing|"   \
  "corrupt|corrupted|unsupported|unknown|unexpected|bad|while|when|during|after|before|"        \
  "open|opening|opened|load|loading|loaded|read|reading|mount|mounting|mounted|find|found|"     \
  "parse|parsing|create|creating|created|access|accessing|extract|extracting|initialize|"       \
  "file|files|image|disc|game|title|path|folder|directory|package|container|archive|"           \
  "content|save|data|device|boot|booting|launch|launching|start|starting|run|running"

NSString* xe_ios_diagnostics_redact(NSString* text) {
  static NSArray<NSArray*>* rules = [@[
    @[
      [NSRegularExpression regularExpressionWithPattern:@"/(private/)?var/(mobile|containers)/[^\\n:]*"
                                                options:NSRegularExpressionCaseInsensitive
                                                  error:nil],
      @"<path>"
    ],
    @[
      [NSRegularExpression regularExpressionWithPattern:@"~/(Documents|Library|tmp)/[^\\n:]*"
                                                options:0
                                                  error:nil],
      @"~/$1/<file>"
    ],
    // Any other path (a Mac's /Users/<name>/..., a volume), Windows paths and
    // game files named on their own, as a fatal error's message may.
    @[
      [NSRegularExpression regularExpressionWithPattern:@"(^|[\\s'\"(=,])/[^\\s/:'\"]+/[^\\n:'\"]*"
                                                options:NSRegularExpressionAnchorsMatchLines
                                                  error:nil],
      @"$1<path>"
    ],
    @[
      [NSRegularExpression regularExpressionWithPattern:@"\\b[A-Za-z]:\\\\[^\\n:'\"]*"
                                                options:0
                                                  error:nil],
      @"<path>"
    ],
    @[
      [NSRegularExpression
          regularExpressionWithPattern:
              @"(^|\\s)(?:(?!(?:" XE_FILE_CONTEXT_WORDS ")[:,]?(?:\\s|$))"
              @"[^\\s\"'<>|][^\\s\"<>|]*[ \\t]+)*"
              @"[^\\s\"<>|]*\\.(?:" XE_FILE_EXTENSIONS ")\\b"
                               options:NSRegularExpressionCaseInsensitive |
                                       NSRegularExpressionAnchorsMatchLines
                                 error:nil],
      @"$1<file>"
    ],
    @[
      [NSRegularExpression
          regularExpressionWithPattern:@"[^\\s/\\\\'\"]+\\.(?:" XE_FILE_EXTENSIONS ")\\b"
                               options:NSRegularExpressionCaseInsensitive
                                 error:nil],
      @"<file>"
    ],
    // Quoted text: usually a name (a game's, a file's, a profile's).
    @[
      [NSRegularExpression regularExpressionWithPattern:@"\"[^\"\\n]*\"" options:0 error:nil],
      @"\"<text>\""
    ],
    @[
      [NSRegularExpression regularExpressionWithPattern:@"\u201c[^\u201d\\n]*\u201d"
                                                options:0
                                                  error:nil],
      @"\u201c<text>\u201d"
    ],
    @[
      [NSRegularExpression regularExpressionWithPattern:@"(^|\\s)'[^'\\n]+'(?=$|[\\s.,;:)])"
                                                options:NSRegularExpressionAnchorsMatchLines
                                                  error:nil],
      @"$1'<text>'"
    ],
    @[
      [NSRegularExpression
          regularExpressionWithPattern:@"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"
                               options:0
                                 error:nil],
      @"<email>"
    ],
  ] retain];
  if (!text.length) {
    return text ?: @"";
  }
  const std::string stripped = xe::strip_pii(text.UTF8String);
  NSMutableString* result = [NSMutableString stringWithUTF8String:stripped.c_str()];
  if (!result) {
    return @"";
  }
  for (NSArray* rule in rules) {
    [rule[0] replaceMatchesInString:result
                            options:0
                              range:NSMakeRange(0, result.length)
                       withTemplate:rule[1]];
  }
  return result;
}

NSURL* xe_ios_diagnostics_endpoint(void) {
  NSString* value = [[NSUserDefaults standardUserDefaults] stringForKey:kEndpointOverrideKey];
  if (!value.length) {
    id info = [[NSBundle mainBundle] objectForInfoDictionaryKey:kEndpointInfoKey];
    value = [info isKindOfClass:[NSString class]] ? info : nil;
  }
  NSURL* url = value.length ? [NSURL URLWithString:value] : nil;
  if (![url.scheme isEqualToString:@"https"] && ![url.host isEqualToString:@"localhost"] &&
      ![url.host isEqualToString:@"127.0.0.1"]) {
    return nil;
  }
  return url;
}

void xe_ios_diagnostics_enqueue(NSString* kind, NSDictionary* body) {
  if (!xe_ios_diagnostics_upload_allowed() || ![body isKindOfClass:[NSDictionary class]]) {
    return;
  }
  @autoreleasepool {
    NSMutableDictionary* envelope = [NSMutableDictionary dictionary];
    [envelope addEntriesFromDictionary:xe_ios_diagnostics_envelope_header()];
    envelope[@"schema"] = @1;
    envelope[@"kind"] = kind;
    envelope[@"id"] = [NSUUID UUID].UUIDString;
    envelope[@"body"] = RedactValue(body);
    NSData* json = [NSJSONSerialization dataWithJSONObject:envelope
                                                   options:NSJSONWritingSortedKeys
                                                     error:nil];
    if (!json || json.length > kMaxRecordBytes) {
      XELOGW("iOS: Dropped a {} diagnostics record ({} bytes)", kind.UTF8String,
             json.length);
      return;
    }
    NSString* name = [NSString stringWithFormat:@"%013llu-%@-%@.json",
                                                static_cast<unsigned long long>(
                                                    [NSDate date].timeIntervalSince1970 * 1000),
                                                kind, envelope[@"id"]];
    dispatch_async(Queue(), ^{
      NSString* directory = OutboxDirectory();
      xe_ios_diagnostics_create_directory(directory);
      [json writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES];
      PruneLocked();
    });
  }
}

void xe_ios_diagnostics_create_directory(NSString* path) {
  NSURL* url = [NSURL fileURLWithPath:path isDirectory:YES];
  [[NSFileManager defaultManager] createDirectoryAtURL:url
                           withIntermediateDirectories:YES
                                            attributes:nil
                                                 error:nil];
  [url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
}

void xe_ios_diagnostics_upload_pending(void) {
  dispatch_async(Queue(), ^{
    UploadLocked();
  });
}

void xe_ios_diagnostics_clear_outbox(void) {
  dispatch_async(Queue(), ^{
    [[NSFileManager defaultManager] removeItemAtPath:OutboxDirectory() error:nil];
  });
}

NSUInteger xe_ios_diagnostics_pending_count(void) {
  __block NSUInteger count = 0;
  dispatch_sync(Queue(), ^{
    count = SortedRecordNames().count;
  });
  return count;
}

NSArray<NSString*>* xe_ios_diagnostics_pending_reports(void) {
  __block NSMutableArray<NSString*>* reports = [[NSMutableArray alloc] init];
  dispatch_sync(Queue(), ^{
    for (NSString* name in SortedRecordNames()) {
      NSData* json =
          [NSData dataWithContentsOfFile:[OutboxDirectory() stringByAppendingPathComponent:name]];
      id object = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
      NSData* pretty =
          object ? [NSJSONSerialization
                       dataWithJSONObject:object
                                  options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys |
                                          NSJSONWritingWithoutEscapingSlashes
                                    error:nil]
                 : nil;
      if (pretty) {
        [reports addObject:[[[NSString alloc] initWithData:pretty
                                                  encoding:NSUTF8StringEncoding] autorelease]];
      }
    }
  });
  return [reports autorelease];
}
