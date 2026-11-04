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

#include <algorithm>
#include <iterator>
#include <string>

#include "xenia/base/logging.h"
#import "xenia/ui/ios/launcher/ios_compat_data.h"
#import "xenia/ui/ios/shared/ios_diagnostics.h"
#import "xenia/ui/ios/shared/ios_fault_record.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_session_stats.h"

namespace {

// The outbox. Over a cap, the oldest sessions go first, then the oldest
// crashes.
constexpr NSUInteger kMaxRecords = 50;
constexpr unsigned long long kMaxOutboxBytes = 2ull << 20;
constexpr NSTimeInterval kMaxCrashAge = 14 * 24 * 3600;
constexpr NSTimeInterval kMaxSessionAge = 48 * 3600;
// A batch, as the Worker takes it.
constexpr NSUInteger kMaxBatchRecords = 50;
// Crash records cost the Worker far more CPU than sessions (frames, text to
// scrub); this many stay well inside the Free plan's 10 ms a request.
constexpr NSUInteger kMaxCrashBatchRecords = 10;
constexpr NSUInteger kMaxBatchBytes = 64 * 1024;
constexpr NSUInteger kMaxBatchJSONBytes = 256 * 1024;
constexpr NSUInteger kMaxBatchesPerRun = 10;
// The wait after the first, second, ... failure in a row. After one more, no
// more tries until the next launch.
constexpr NSTimeInterval kRetryDelays[] = {5 * 60, 30 * 60, 2 * 3600, 12 * 3600};
constexpr NSTimeInterval kMaxRetryAfter = 48 * 3600;
// Sessions go together once the UTC day they were played is over: the Worker
// adds a batch's sessions of one game and day into one daily row (one row
// written), where sessions sent apart cost one row each. It counts a session on
// its "day" when that is the day it arrives or the day before, so held ones go
// at the first chance the next day. Sooner with kSessionFlushCount queued; at
// once the first time.
constexpr NSUInteger kSessionFlushCount = 40;
// Crash records queued per device in any 24 hours; more (a crash loop) are
// dropped. The sessions' end reasons still count every crash.
constexpr NSUInteger kMaxCrashesPerDay = 5;
// A crash whose top XeniOS frames (in the same binary) this device queued this
// recently isn't queued again.
constexpr NSTimeInterval kCrashRepeatWindow = 7 * 24 * 3600;
constexpr NSUInteger kCrashSignatureFrames = 3;
// The Worker's device share (X-Diag-Sessions-One-In): sessions are sent by the
// devices whose draw is a multiple of N, a power of two up to this, each
// weighted N.
constexpr uint32_t kMaxSessionsOneIn = 1024;

NSString* const kEndpointInfoKey = @"XeniOSDiagnosticsURL";
// For testing against a local server: `defaults write` on a simulator, or a
// test harness.
NSString* const kEndpointOverrideKey = @"XeniaDiagnosticsURL";
NSString* const kRetryAfterKey = @"XeniaDiagnosticsRetryAfter";
// Crashes refused for the day (503): crash batches wait, sessions go on.
NSString* const kCrashRetryAfterKey = @"XeniaDiagnosticsCrashRetryAfter";
NSString* const kFailuresKey = @"XeniaDiagnosticsFailures";
// The launch that ran out of retries; it sends nothing more.
NSString* const kGaveUpLaunchKey = @"XeniaDiagnosticsGaveUpLaunch";
NSString* const kSessionsSentKey = @"XeniaDiagnosticsSessionsSentAt";
// When the crash records of the last 24 hours were queued, seconds since 1970.
NSString* const kCrashTimesKey = @"XeniaDiagnosticsCrashTimes";
// {signature: seconds since 1970} of the crashes queued in kCrashRepeatWindow.
NSString* const kCrashSignaturesKey = @"XeniaDiagnosticsCrashSignatures";
NSString* const kSessionsOneInKey = @"XeniaDiagnosticsSessionsOneIn";
// This device's fixed random 32-bit draw for the share.
NSString* const kShareDrawKey = @"XeniaDiagnosticsShareDraw";

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
unsigned long long g_last_record_time = 0;

// Outbox queue only. Later than every record queued before.
unsigned long long NextRecordTime() {
  const auto now = static_cast<unsigned long long>([NSDate date].timeIntervalSince1970 * 1e6);
  g_last_record_time = std::max(now, g_last_record_time + 1);
  return g_last_record_time;
}

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

BOOL IsHeader(NSDictionary* header) {
  return [header isKindOfClass:[NSDictionary class]] &&
         [header[@"build"] isKindOfClass:[NSDictionary class]] &&
         [header[@"device"] isKindOfClass:[NSDictionary class]];
}

// A batch as sent: compact JSON, keys sorted.
NSData* BatchJSON(NSDictionary* header, NSArray<NSDictionary*>* records) {
  return [NSJSONSerialization dataWithJSONObject:@{
    @"schema" : @2,
    @"build" : header[@"build"],
    @"device" : header[@"device"],
    @"records" : records,
  }
                                         options:NSJSONWritingSortedKeys
                                           error:nil];
}

// |json| gzipped, or nil if that is over the Worker's limits.
NSData* GzipWithinLimits(NSData* json) {
  if (!json || json.length > kMaxBatchJSONBytes) {
    return nil;
  }
  NSData* gzip = Gzip(json);
  return gzip.length && gzip.length <= kMaxBatchBytes ? gzip : nil;
}

// Oldest first: names are <microseconds since 1970>-<kind>-<id>.json, the
// times distinct (NextRecordTime).
NSArray<NSString*>* SortedRecordNames() {
  NSArray<NSString*>* names =
      [[NSFileManager defaultManager] contentsOfDirectoryAtPath:OutboxDirectory() error:nil];
  return [[names filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.json'"]]
      sortedArrayUsingSelector:@selector(compare:)];
}

NSString* KindOfRecordName(NSString* name) {
  NSArray<NSString*>* parts = [name componentsSeparatedByString:@"-"];
  return parts.count > 2 ? parts[1] : @"";
}

// The queued records, oldest first: [{name, header, record}], record being
// {kind, id, body} as a batch carries it. Deletes files that don't read as one.
NSArray<NSDictionary*>* LoadRecordsLocked() {
  NSMutableArray<NSDictionary*>* entries = [NSMutableArray array];
  NSString* directory = OutboxDirectory();
  for (NSString* name in SortedRecordNames()) {
    NSString* path = [directory stringByAppendingPathComponent:name];
    NSData* json = [NSData dataWithContentsOfFile:path];
    NSDictionary* stored =
        json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
    if (!IsHeader(stored) || ![stored[@"kind"] isKindOfClass:[NSString class]] ||
        ![stored[@"id"] isKindOfClass:[NSString class]] ||
        ![stored[@"body"] isKindOfClass:[NSDictionary class]]) {
      [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
      continue;
    }
    [entries addObject:@{
      @"name" : name,
      @"header" : @{@"build" : stored[@"build"], @"device" : stored[@"device"]},
      @"record" : @{@"kind" : stored[@"kind"], @"id" : stored[@"id"], @"body" : stored[@"body"]},
    }];
  }
  return entries;
}

// The share's N: 1 until the Worker first says.
uint32_t SessionsOneIn() {
  const NSInteger n = [[NSUserDefaults standardUserDefaults] integerForKey:kSessionsOneInKey];
  return n >= 1 && n <= kMaxSessionsOneIn && !(n & (n - 1)) ? static_cast<uint32_t>(n) : 1;
}

uint32_t ShareDraw() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSNumber* draw = [defaults objectForKey:kShareDrawKey];
  if (![draw isKindOfClass:[NSNumber class]]) {
    draw = @(arc4random());
    [defaults setObject:draw forKey:kShareDrawKey];
  }
  return draw.unsignedIntValue;
}

// Whether this device sends sessions now. Raising N (by doubling) keeps a
// subset of the same devices.
BOOL InSessionShare() { return ShareDraw() % SessionsOneIn() == 0; }

// The Worker's X-Diag-Sessions-One-In, when it is a power of two up to
// kMaxSessionsOneIn; otherwise the stored N stays.
void NoteSessionsOneIn(NSString* value) {
  value = [value stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if (!value.length || value.length > 4 ||
      [value rangeOfCharacterFromSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]]
              .location != NSNotFound) {
    return;
  }
  const NSInteger n = value.integerValue;
  if (n >= 1 && n <= kMaxSessionsOneIn && !(n & (n - 1))) {
    [[NSUserDefaults standardUserDefaults] setInteger:n forKey:kSessionsOneInKey];
  }
}

// What is queued, as it will be sent: [{names, json, gzip}], in the order of
// each batch's oldest record. Records of one kind with the same header (the
// build and device of the launch they describe) share a batch, up to
// kMaxBatchRecords (kMaxCrashBatchRecords for crashes) and kMaxBatchBytes.
// Crashes and sessions never share one: the Worker refuses a batch with a
// crash whole once the day's crashes are spent, which would hold its sessions
// back a day. Sessions carry the share's weight, N now; while this device is
// out of the share, its queued sessions are deleted (others' stand for them). A
// record too large to go alone is deleted.
NSArray<NSDictionary*>* BatchesLocked() {
  NSMutableArray<NSString*>* keys = [NSMutableArray array];
  NSMutableDictionary<NSString*, NSMutableArray<NSDictionary*>*>* groups =
      [NSMutableDictionary dictionary];
  const BOOL in_share = InSessionShare();
  const uint32_t weight = SessionsOneIn();
  for (NSDictionary* entry in LoadRecordsLocked()) {
    if ([entry[@"record"][@"kind"] isEqualToString:@"session"]) {
      if (!in_share) {
        [[NSFileManager defaultManager]
            removeItemAtPath:[OutboxDirectory() stringByAppendingPathComponent:entry[@"name"]]
                       error:nil];
        continue;
      }
      NSDictionary* record = entry[@"record"];
      NSMutableDictionary* body = [NSMutableDictionary dictionaryWithDictionary:record[@"body"]];
      body[@"weight"] = @(weight);
      NSMutableDictionary* weighted = [NSMutableDictionary dictionaryWithDictionary:entry];
      weighted[@"record"] = @{@"kind" : @"session", @"id" : record[@"id"], @"body" : body};
      entry = weighted;
    }
    NSData* key_json = [NSJSONSerialization dataWithJSONObject:entry[@"header"]
                                                       options:NSJSONWritingSortedKeys
                                                         error:nil];
    NSString* header_key = [[[NSString alloc] initWithData:key_json
                                                  encoding:NSUTF8StringEncoding] autorelease];
    if (!header_key) {
      continue;
    }
    NSString* key = [entry[@"record"][@"kind"] stringByAppendingFormat:@" %@", header_key];
    if (!groups[key]) {
      groups[key] = [NSMutableArray array];
      [keys addObject:key];
    }
    [groups[key] addObject:entry];
  }
  NSMutableArray<NSDictionary*>* batches = [NSMutableArray array];
  for (NSString* key in keys) {
    NSArray<NSDictionary*>* group = groups[key];
    NSDictionary* header = group.firstObject[@"header"];
    const NSUInteger max_records =
        [group.firstObject[@"record"][@"kind"] isEqualToString:@"crash"] ? kMaxCrashBatchRecords
                                                                         : kMaxBatchRecords;
    NSMutableArray<NSString*>* names = [NSMutableArray array];
    NSMutableArray<NSDictionary*>* records = [NSMutableArray array];
    NSData* json = nil;
    NSData* gzip = nil;
    auto flush = [&]() {
      if (names.count) {
        [batches addObject:@{
          @"kind" : group.firstObject[@"record"][@"kind"],
          @"names" : [[names copy] autorelease],
          @"json" : json,
          @"gzip" : gzip,
        }];
      }
      [names removeAllObjects];
      [records removeAllObjects];
    };
    for (NSDictionary* entry in group) {
      if (records.count < max_records) {
        NSData* candidate = BatchJSON(header, [records arrayByAddingObject:entry[@"record"]]);
        if (NSData* candidate_gzip = GzipWithinLimits(candidate)) {
          [names addObject:entry[@"name"]];
          [records addObject:entry[@"record"]];
          json = candidate;
          gzip = candidate_gzip;
          continue;
        }
      }
      flush();
      NSData* alone = BatchJSON(header, @[ entry[@"record"] ]);
      NSData* alone_gzip = GzipWithinLimits(alone);
      if (!alone_gzip) {
        XELOGW("iOS: Dropped a diagnostics record too large to send ({} bytes)", alone.length);
        [[NSFileManager defaultManager]
            removeItemAtPath:[OutboxDirectory() stringByAppendingPathComponent:entry[@"name"]]
                       error:nil];
        continue;
      }
      [names addObject:entry[@"name"]];
      [records addObject:entry[@"record"]];
      json = alone;
      gzip = alone_gzip;
    }
    flush();
  }
  return batches;
}

void PruneLocked() {
  NSFileManager* fm = [NSFileManager defaultManager];
  NSString* directory = OutboxDirectory();
  NSMutableArray<NSString*>* names = [NSMutableArray array];
  NSMutableArray<NSNumber*>* sizes = [NSMutableArray array];
  unsigned long long total = 0;
  for (NSString* name in SortedRecordNames()) {
    NSString* path = [directory stringByAppendingPathComponent:name];
    NSDictionary* attributes = [fm attributesOfItemAtPath:path error:nil];
    const NSTimeInterval max_age =
        [KindOfRecordName(name) isEqualToString:@"session"] ? kMaxSessionAge : kMaxCrashAge;
    if (!attributes || -[attributes.fileModificationDate timeIntervalSinceNow] > max_age) {
      [fm removeItemAtPath:path error:nil];
      continue;
    }
    [names addObject:name];
    [sizes addObject:@(attributes.fileSize)];
    total += attributes.fileSize;
  }
  while (names.count > kMaxRecords || total > kMaxOutboxBytes) {
    NSUInteger victim = [names indexOfObjectPassingTest:^BOOL(NSString* name, NSUInteger, BOOL*) {
      return [KindOfRecordName(name) isEqualToString:@"session"];
    }];
    if (victim == NSNotFound) {
      victim = 0;
    }
    [fm removeItemAtPath:[directory stringByAppendingPathComponent:names[victim]] error:nil];
    total -= sizes[victim].unsignedLongLongValue;
    [names removeObjectAtIndex:victim];
    [sizes removeObjectAtIndex:victim];
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

NSString* CurrentLaunch() { return xe_ios_diagnostics_launch_id() ?: @""; }

// The server answered for itself: the next failure starts the ladder again.
void NoteServerAnsweredLocked() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults removeObjectForKey:kFailuresKey];
  [defaults removeObjectForKey:kRetryAfterKey];
}

void NoteUploadFailedLocked(NSInteger status) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  const NSInteger failures = [defaults integerForKey:kFailuresKey] + 1;
  if (failures <= static_cast<NSInteger>(std::size(kRetryDelays))) {
    const NSTimeInterval delay = kRetryDelays[failures - 1];
    XELOGW("iOS: Diagnostics upload failed ({}), retrying in {} min", status,
           static_cast<int>(delay / 60));
    [defaults setInteger:failures forKey:kFailuresKey];
    [defaults setObject:[NSDate dateWithTimeIntervalSinceNow:delay] forKey:kRetryAfterKey];
    return;
  }
  XELOGW("iOS: Diagnostics upload failed ({}), retrying at the next launch", status);
  [defaults removeObjectForKey:kFailuresKey];
  [defaults removeObjectForKey:kRetryAfterKey];
  [defaults setObject:CurrentLaunch() forKey:kGaveUpLaunchKey];
}

// A Retry-After of whole seconds, or -1.
NSTimeInterval RetryAfterSeconds(NSString* value) {
  value = [value stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if (!value.length || value.length > 9 ||
      [value rangeOfCharacterFromSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]]
              .location != NSNotFound) {
    return -1;
  }
  return std::min<NSTimeInterval>(value.longLongValue, kMaxRetryAfter);
}

BOOL Waiting(NSString* key) {
  NSDate* date = [[NSUserDefaults standardUserDefaults] objectForKey:key];
  return [date isKindOfClass:[NSDate class]] && date.timeIntervalSinceNow > 0;
}

void SendNextLocked(NSArray<NSDictionary*>* batches, NSUInteger index, NSURL* url) {
  if (index >= batches.count || index >= kMaxBatchesPerRun || !UploadAllowedNow()) {
    g_uploading = NO;
    return;
  }
  const BOOL crashes = [batches[index][@"kind"] isEqualToString:@"crash"];
  if (crashes && Waiting(kCrashRetryAfterKey)) {
    SendNextLocked(batches, index + 1, url);
    return;
  }
  NSArray<NSString*>* names = batches[index][@"names"];
  NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
  request.HTTPMethod = @"POST";
  [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
  [request setValue:@"gzip" forHTTPHeaderField:@"Content-Encoding"];
  request.HTTPBody = batches[index][@"gzip"];
  NSURLSessionDataTask* task = [Session()
      dataTaskWithRequest:request
        completionHandler:^(NSData* __unused data, NSURLResponse* response,
                            NSError* __unused error) {
          NSHTTPURLResponse* http = [response isKindOfClass:[NSHTTPURLResponse class]]
                                        ? static_cast<NSHTTPURLResponse*>(response)
                                        : nil;
          const NSInteger status = http ? http.statusCode : 0;
          NSString* retry_after = [http valueForHTTPHeaderField:@"Retry-After"];
          NSString* one_in = status == 204 || status == 503
                                 ? [http valueForHTTPHeaderField:@"X-Diag-Sessions-One-In"]
                                 : nil;
          dispatch_async(Queue(), ^{
            NoteSessionsOneIn(one_in);
            // Stored (204), or malformed (400, 413) and never worth resending.
            if (status == 204 || status == 400 || status == 413) {
              if (status != 204) {
                XELOGW("iOS: Diagnostics batch of {} rejected ({}), deleted", names.count,
                       status);
              }
              NSString* directory = OutboxDirectory();
              for (NSString* name in names) {
                [[NSFileManager defaultManager]
                    removeItemAtPath:[directory stringByAppendingPathComponent:name]
                               error:nil];
              }
              NoteServerAnsweredLocked();
              if (!crashes) {
                [[NSUserDefaults standardUserDefaults] setObject:[NSDate date]
                                                          forKey:kSessionsSentKey];
              }
              SendNextLocked(batches, index + 1, url);
              return;
            }
            // Crashes refused for today: kept for when the server says, while
            // sessions go on. (A session batch isn't refused so; if one were,
            // everything waits.)
            const NSTimeInterval wait = status == 503 ? RetryAfterSeconds(retry_after) : -1;
            if (wait >= 0) {
              XELOGW("iOS: Diagnostics refused for now, retrying in {} s",
                     static_cast<int64_t>(wait));
              NoteServerAnsweredLocked();
              [[NSUserDefaults standardUserDefaults]
                  setObject:[NSDate dateWithTimeIntervalSinceNow:wait]
                     forKey:crashes ? kCrashRetryAfterKey : kRetryAfterKey];
              if (crashes) {
                SendNextLocked(batches, index + 1, url);
                return;
              }
            } else {
              // No network, a 429, another 5xx, or something else answering
              // (a proxy or captive portal's 200 or redirect).
              NoteUploadFailedLocked(status);
            }
            g_uploading = NO;
          });
        }];
  [task resume];
}

NSString* UTCDay(NSDate* date) {
  static NSDateFormatter* formatter = [] {
    NSDateFormatter* f = [[NSDateFormatter alloc] init];
    f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    f.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"];
    f.dateFormat = @"yyyy-MM-dd";
    return f;
  }();
  return [formatter stringFromDate:date];
}

// Whether queued sessions go now (kSessionFlushCount and above).
BOOL SessionsDueLocked() {
  if (![[[NSUserDefaults standardUserDefaults] objectForKey:kSessionsSentKey]
          isKindOfClass:[NSDate class]]) {
    return YES;
  }
  NSString* today = UTCDay([NSDate date]);
  NSUInteger count = 0;
  for (NSString* name in SortedRecordNames()) {
    if (![KindOfRecordName(name) isEqualToString:@"session"]) {
      continue;
    }
    // The oldest first: due once its day is over.
    if (!count++) {
      NSString* path = [OutboxDirectory() stringByAppendingPathComponent:name];
      NSData* json = [NSData dataWithContentsOfFile:path];
      NSDictionary* stored =
          json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
      id day = [stored isKindOfClass:[NSDictionary class]] &&
                       [stored[@"body"] isKindOfClass:[NSDictionary class]]
                   ? stored[@"body"][@"day"]
                   : nil;
      if (![day isKindOfClass:[NSString class]]) {
        day = UTCDay([[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil]
                         .fileModificationDate ?: [NSDate date]);
      }
      if ([day compare:today] == NSOrderedAscending) {
        return YES;
      }
    }
  }
  return count >= kSessionFlushCount;
}

void UploadLocked() {
  if (g_uploading || !UploadAllowedNow()) {
    return;
  }
  NSURL* url = xe_ios_diagnostics_endpoint();
  if (!url) {
    return;
  }
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  if (NSString* gave_up = [defaults stringForKey:kGaveUpLaunchKey]) {
    if ([gave_up isEqualToString:CurrentLaunch()]) {
      return;
    }
    [defaults removeObjectForKey:kGaveUpLaunchKey];
  }
  if (Waiting(kRetryAfterKey)) {
    return;
  }
  PruneLocked();
  NSArray<NSDictionary*>* batches = BatchesLocked();
  if (!SessionsDueLocked()) {
    batches = [batches
        filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"kind != 'session'"]];
  }
  if (!batches.count) {
    return;
  }
  g_uploading = YES;
  SendNextLocked(batches, 0, url);
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

namespace {

NSString* BinaryUUIDOfHeader(NSDictionary* header) {
  id build = IsHeader(header) ? header[@"build"] : nil;
  id uuid = build[@"binaryUUID"];
  return [uuid isKindOfClass:[NSString class]] ? uuid : nil;
}

BOOL SameUUID(NSString* a, NSString* b) {
  return a && b && [a caseInsensitiveCompare:b] == NSOrderedSame;
}

}  // namespace

NSDictionary* xe_ios_diagnostics_header_for_earlier_launch(NSDictionary* logged,
                                                           NSString* binary_uuid,
                                                           NSArray<NSDictionary*>* known) {
  if (![binary_uuid isKindOfClass:[NSString class]] || !binary_uuid.length) {
    binary_uuid = nil;
  }
  if (IsHeader(logged) && (!binary_uuid || SameUUID(BinaryUUIDOfHeader(logged), binary_uuid))) {
    return logged;
  }
  NSDictionary* current = xe_ios_diagnostics_envelope_header();
  if (binary_uuid) {
    if (SameUUID(BinaryUUIDOfHeader(current), binary_uuid)) {
      return current;
    }
    for (NSDictionary* header in known) {
      if (IsHeader(header) && SameUUID(BinaryUUIDOfHeader(header), binary_uuid)) {
        return header;
      }
    }
  }
  return @{
    @"build" : binary_uuid ? @{@"binaryUUID" : binary_uuid} : @{},
    @"device" : @{@"model" : current[@"device"][@"model"]},
  };
}

// A file name may hold spaces ("Halo 3 Reach.iso") and a relative path may
// start anywhere ("Alice Games/Halo/default.xex"), so the file rule replaces the
// whole run of words up to the extension, from the first word that isn't plain
// message text (kFileContextWords). Kept in step with the Worker's scrub().
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

namespace {

// "<binary UUID> <offset> <offset> <offset>": the crash's top XeniOS frames in
// its build, or nil without XeniOS frames.
NSString* CrashSignature(NSDictionary* header, NSDictionary* body) {
  NSMutableArray<NSString*>* parts = [NSMutableArray array];
  id uuid = header[@"build"][@"binaryUUID"];
  [parts addObject:[uuid isKindOfClass:[NSString class]] ? [uuid uppercaseString] : @"?"];
  for (id frame in [body[@"frames"] isKindOfClass:[NSArray class]] ? body[@"frames"] : @[]) {
    if (parts.count > kCrashSignatureFrames) {
      break;
    }
    if ([frame isKindOfClass:[NSDictionary class]] && [frame[@"image"] isEqual:@"XeniOS"] &&
        [frame[@"offset"] isKindOfClass:[NSString class]]) {
      [parts addObject:frame[@"offset"]];
    }
  }
  return parts.count > 1 ? [parts componentsJoinedByString:@" "] : nil;
}

}  // namespace

void xe_ios_diagnostics_enqueue(NSString* kind, NSDictionary* body) {
  xe_ios_diagnostics_enqueue_with_header(kind, body, nil);
}

void xe_ios_diagnostics_enqueue_with_header(NSString* kind, NSDictionary* body,
                                            NSDictionary* header) {
  if (!xe_ios_diagnostics_upload_allowed() || ![body isKindOfClass:[NSDictionary class]]) {
    return;
  }
  @autoreleasepool {
    if (!IsHeader(header)) {
      header = xe_ios_diagnostics_envelope_header();
    }
    NSString* record_id = [NSUUID UUID].UUIDString;
    NSDictionary* record = @{@"kind" : kind, @"id" : record_id, @"body" : RedactValue(body)};
    NSMutableDictionary* stored = [NSMutableDictionary dictionaryWithDictionary:record];
    stored[@"build"] = header[@"build"];
    stored[@"device"] = header[@"device"];
    NSData* json = [NSJSONSerialization dataWithJSONObject:stored
                                                   options:NSJSONWritingSortedKeys
                                                     error:nil];
    if (!json) {
      XELOGW("iOS: Dropped a {} diagnostics record that isn't JSON", kind.UTF8String);
      return;
    }
    dispatch_async(Queue(), ^{
      // One that couldn't be sent even alone isn't kept.
      NSData* alone = BatchJSON(header, @[ record ]);
      if (!GzipWithinLimits(alone)) {
        XELOGW("iOS: Dropped a {} diagnostics record ({} bytes)", kind.UTF8String,
               alone.length);
        return;
      }
      if ([kind isEqualToString:@"session"] && !InSessionShare()) {
        return;
      }
      if ([kind isEqualToString:@"crash"]) {
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        const NSTimeInterval now = [NSDate date].timeIntervalSince1970;
        NSMutableDictionary* signatures = [NSMutableDictionary dictionary];
        NSDictionary* stored_signatures = [defaults dictionaryForKey:kCrashSignaturesKey];
        for (NSString* key in stored_signatures) {
          id time = stored_signatures[key];
          if ([time isKindOfClass:[NSNumber class]] &&
              now - [time doubleValue] < kCrashRepeatWindow) {
            signatures[key] = time;
          }
        }
        NSString* signature = CrashSignature(header, record[@"body"]);
        if (signature && signatures[signature]) {
          XELOGI("iOS: Dropped a crash record this device sent in the last week");
          return;
        }
        NSMutableArray* times = [NSMutableArray array];
        for (id time in [defaults arrayForKey:kCrashTimesKey]) {
          if ([time isKindOfClass:[NSNumber class]] && now - [time doubleValue] < 24 * 3600) {
            [times addObject:time];
          }
        }
        if (times.count >= kMaxCrashesPerDay) {
          XELOGW("iOS: Dropped a crash record, {} queued in the last day", times.count);
          return;
        }
        [times addObject:@(now)];
        [defaults setObject:times forKey:kCrashTimesKey];
        if (signature) {
          signatures[signature] = @(now);
        }
        [defaults setObject:signatures forKey:kCrashSignaturesKey];
      }
      NSString* name =
          [NSString stringWithFormat:@"%016llu-%@-%@.json", NextRecordTime(), kind, record_id];
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
    PruneLocked();
    for (NSDictionary* batch in BatchesLocked()) {
      id object = [NSJSONSerialization JSONObjectWithData:batch[@"json"] options:0 error:nil];
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
