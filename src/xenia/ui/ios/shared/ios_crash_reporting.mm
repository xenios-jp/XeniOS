/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_crash_reporting.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/system_ios.h"
#include "xenia/ui/ios/shared/ios_diagnostics.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"

#if __has_include(<MetricKit/MetricKit.h>)
#import <MetricKit/MetricKit.h>
#define XE_IOS_HAS_METRICKIT 1
#else
#define XE_IOS_HAS_METRICKIT 0
#endif

DECLARE_path(log_file);

#if XE_IOS_HAS_METRICKIT
// Crash and hang diagnostics arrive from MetricKit on a later launch. Saved
// next to the session state so Share Log can attach them, and their crashes
// sent as diagnostics.
@interface XeniaMetricSubscriber : NSObject <MXMetricManagerSubscriber>
@end
#endif

namespace {

constexpr NSTimeInterval kFatalAlertTimeoutSeconds = 300.0;
// How long a FatalError on a worker thread waits for the main thread to start
// showing the box before giving up (the main thread may be waiting on it).
constexpr int64_t kFatalAlertStartTimeoutSeconds = 5;
constexpr size_t kMaxSavedDiagnostics = 5;
constexpr NSUInteger kMaxMetricKitFrames = 64;
constexpr NSUInteger kMaxTerminationReasonChars = 300;
constexpr size_t kMaxNoteChars = 600;

// States of the previous/current session.
NSString* const kStateForeground = @"foreground";
NSString* const kStateBackground = @"background";
NSString* const kStateEnded = @"ended";
NSString* const kStateFatal = @"fatal";

std::mutex& StateMutex() {
  static std::mutex& mutex = *new std::mutex();
  return mutex;
}

// All guarded by StateMutex().
NSMutableDictionary* g_state = nil;
std::filesystem::path g_log_path;
BOOL g_session_started = NO;
BOOL g_fatal_in_progress = NO;
// Set on a thread that hit FatalError after another thread already had.
thread_local bool t_fatal_secondary = false;

// Main thread only.
BOOL g_offered_this_launch = NO;

NSString* ToNS(std::string_view text) {
  NSString* result = [[[NSString alloc] initWithBytes:text.data()
                                               length:text.size()
                                             encoding:NSUTF8StringEncoding] autorelease];
  if (!result) {
    result = [[[NSString alloc] initWithBytes:text.data()
                                       length:text.size()
                                     encoding:NSISOLatin1StringEncoding] autorelease];
  }
  return result ? result : @"";
}

std::string Redacted(NSString* text) {
  return xe::strip_pii(text ? [text UTF8String] : "");
}

NSString* SupportDirectory() {
  return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/XeniOS"];
}

NSString* StatePath() {
  return [SupportDirectory() stringByAppendingPathComponent:@"session_state.plist"];
}

NSString* DiagnosticsDirectory() {
  return [SupportDirectory() stringByAppendingPathComponent:@"Diagnostics"];
}

NSString* ShareRootDirectory() {
  return [NSTemporaryDirectory() stringByAppendingPathComponent:@"xenios-share"];
}

std::filesystem::path CurrentLogPath() {
  {
    std::lock_guard<std::mutex> lock(StateMutex());
    if (!g_log_path.empty()) {
      return g_log_path;
    }
  }
  if (!cvars::log_file.empty()) {
    return cvars::log_file;
  }
  return xe_get_ios_documents_path() / "xenia.log";
}

std::filesystem::path PreviousLogPath(const std::filesystem::path& log_path) {
  return log_path.parent_path() / "xenia.previous.log";
}

// Writes the state file. Caller holds StateMutex().
void WriteStateLocked() {
  if (!g_state) {
    return;
  }
  [g_state writeToFile:StatePath() atomically:YES];
}

// Sets the session state unless a fatal error was already recorded.
void SetSessionState(NSString* state) {
  std::lock_guard<std::mutex> lock(StateMutex());
  if (!g_state || [g_state[@"state"] isEqualToString:kStateFatal]) {
    return;
  }
  if ([g_state[@"state"] isEqualToString:state]) {
    return;
  }
  g_state[@"state"] = state;
  WriteStateLocked();
}

NSString* AppVersionString() {
  NSDictionary* info = [[NSBundle mainBundle] infoDictionary];
  NSString* version = info[@"CFBundleShortVersionString"];
  NSString* build = info[@"CFBundleVersion"];
  return [NSString stringWithFormat:@"%@ (%@)", version ? version : @"?", build ? build : @"?"];
}

// Identifies this install: the bundle's path (a new container on every install)
// and the executable's modification time. Installing or updating the app kills
// the running copy, which must not read as a crash at the next launch.
NSString* AppInstallString() {
  NSBundle* bundle = [NSBundle mainBundle];
  NSDate* modified = [[[NSFileManager defaultManager] attributesOfItemAtPath:bundle.executablePath
                                                                       error:nil] fileModificationDate];
  return [NSString stringWithFormat:@"%@|%.0f", bundle.bundlePath, modified.timeIntervalSince1970];
}

// ---- Redacted copies for sharing ----------------------------------------

// Copies src to dst through strip_pii in line-aligned chunks, so a multi-MB
// log is never held whole in memory (a single-line JSON file is, but those are
// small). JSON files also get "\/" turned into "/" first, which is
// equivalent JSON and lets path redaction see through the escaping.
bool WriteRedactedCopy(NSString* src, NSString* dst, bool json) {
  FILE* in = fopen([src fileSystemRepresentation], "rb");
  if (!in) {
    return false;
  }
  FILE* out = fopen([dst fileSystemRepresentation], "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  auto emit = [&](std::string& chunk) {
    if (json) {
      for (size_t pos = chunk.find("\\/"); pos != std::string::npos; pos = chunk.find("\\/", pos)) {
        chunk.erase(pos, 1);
      }
    }
    const std::string redacted = xe::strip_pii(chunk);
    fwrite(redacted.data(), 1, redacted.size(), out);
  };
  constexpr size_t kChunkBytes = 1024 * 1024;
  std::string buffer;
  std::string block(64 * 1024, '\0');
  size_t read;
  while ((read = fread(block.data(), 1, block.size(), in)) > 0) {
    buffer.append(block.data(), read);
    if (buffer.size() >= kChunkBytes) {
      const size_t newline = buffer.rfind('\n');
      if (newline != std::string::npos) {
        std::string chunk = buffer.substr(0, newline + 1);
        buffer.erase(0, newline + 1);
        emit(chunk);
      }
    }
  }
  if (!buffer.empty()) {
    emit(buffer);
  }
  fclose(in);
  const bool ok = fclose(out) == 0;
  return ok;
}

NSString* SessionSummaryLocked() {
  NSString* state = g_state[@"prev_state"];
  if (!state) {
    return nil;
  }
  NSString* ended = @"normally or in the background";
  if ([state isEqualToString:kStateFatal]) {
    ended = @"a fatal error";
  } else if ([state isEqualToString:kStateForeground]) {
    ended = @"in the foreground (crash, watchdog or foreground kill)";
  }
  NSMutableString* summary = [NSMutableString string];
  [summary appendFormat:@"XeniOS previous session\nVersion: %@\n",
                        g_state[@"prev_version"] ?: @"?"];
  [summary appendFormat:@"Game running: %@\n",
                        [g_state[@"prev_game"] boolValue] ? @"yes" : @"no"];
  [summary appendFormat:@"Ended: %@\n", ended];
  NSString* message = g_state[@"prev_message"];
  if (message.length) {
    [summary appendFormat:@"Message: %s\n", xe::strip_pii([message UTF8String]).c_str()];
  }
  return summary;
}

NSArray<NSString*>* NewestDiagnosticFiles(NSUInteger count) {
  NSFileManager* fm = [NSFileManager defaultManager];
  NSArray<NSString*>* names = [fm contentsOfDirectoryAtPath:DiagnosticsDirectory() error:nil];
  // Names start with a sortable timestamp.
  NSPredicate* is_json = [NSPredicate predicateWithFormat:@"SELF ENDSWITH '.json'"];
  NSArray<NSString*>* sorted = [[names filteredArrayUsingPredicate:is_json]
      sortedArrayUsingSelector:@selector(compare:)];
  NSMutableArray<NSString*>* result = [NSMutableArray array];
  for (NSInteger i = static_cast<NSInteger>(sorted.count) - 1;
       i >= 0 && result.count < count; --i) {
    [result addObject:[DiagnosticsDirectory() stringByAppendingPathComponent:sorted[i]]];
  }
  return result;
}

UIViewController* TopPresenter() {
  UIWindowScene* best = nil;
  for (UIScene* scene in [UIApplication sharedApplication].connectedScenes) {
    if (![scene isKindOfClass:[UIWindowScene class]]) {
      continue;
    }
    if (scene.activationState == UISceneActivationStateForegroundActive) {
      best = static_cast<UIWindowScene*>(scene);
      break;
    }
    if (!best && scene.activationState == UISceneActivationStateForegroundInactive) {
      best = static_cast<UIWindowScene*>(scene);
    }
  }
  if (!best) {
    return nil;
  }
  UIWindow* window = nil;
  for (UIWindow* candidate in best.windows) {
    if (candidate.isKeyWindow) {
      window = candidate;
      break;
    }
  }
  if (!window) {
    window = best.windows.firstObject;
  }
  UIViewController* top = window.rootViewController;
  while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) {
    top = top.presentedViewController;
  }
  return top;
}

UIViewController* TopOf(UIViewController* presenter) {
  UIViewController* top = presenter ? presenter : TopPresenter();
  while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) {
    top = top.presentedViewController;
  }
  return top;
}

// Main thread. Returns nil if there was nowhere to present.
UIAlertController* PresentSingleButtonAlert(NSString* title, NSString* message, NSString* button,
                                            void (^dismissed)(void)) {
  UIViewController* top = TopPresenter();
  if (!top) {
    return nil;
  }
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:message
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:button
                                            style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction* __unused action) {
                                            if (dismissed) {
                                              dismissed();
                                            }
                                          }]];
  [top presentViewController:alert animated:YES completion:nil];
  return alert;
}

// Any thread. Returns once the user dismissed the alert, nothing could be
// shown, or the timeout passed. On the main thread this spins the run loop.
void ShowBlockingAlert(NSString* title, NSString* message, NSString* button) {
  @autoreleasepool {
    const BOOL on_main = [NSThread isMainThread];
    dispatch_semaphore_t started = on_main ? nil : dispatch_semaphore_create(0);
    dispatch_semaphore_t finished = on_main ? nil : dispatch_semaphore_create(0);
    __block BOOL done = NO;
    void (^finish)(void) = ^{
      done = YES;
      if (finished) {
        dispatch_semaphore_signal(finished);
      }
    };
    void (^start)(void) = ^{
      if (started) {
        dispatch_semaphore_signal(started);
      }
      UIAlertController* alert = PresentSingleButtonAlert(title, message, button, finish);
      if (!alert) {
        finish();
        return;
      }
      // If UIKit refused to present, no button will ever be tapped.
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(),
                     ^{
                       if (!done && !alert.presentingViewController) {
                         finish();
                       }
                     });
    };
    if (on_main) {
      start();
      const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + kFatalAlertTimeoutSeconds;
      while (!done && CFAbsoluteTimeGetCurrent() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
      }
    } else {
      dispatch_async(dispatch_get_main_queue(), start);
      if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW,
                                                         kFatalAlertStartTimeoutSeconds *
                                                             NSEC_PER_SEC)) == 0) {
        dispatch_semaphore_wait(finished,
                                dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(
                                                                     kFatalAlertTimeoutSeconds *
                                                                     NSEC_PER_SEC)));
      }
    }
    [started release];
    [finished release];
  }
}

// ---- Hooks installed into xenia-base --------------------------------------

void HookNoteFatalError(std::string_view message) {
  @autoreleasepool {
    NSString* text = ToNS(message);
    if (text.length > kMaxNoteChars) {
      const NSRange cut = [text rangeOfComposedCharacterSequenceAtIndex:kMaxNoteChars - 1];
      text = [text substringToIndex:NSMaxRange(cut)];
    }
    std::lock_guard<std::mutex> lock(StateMutex());
    if (g_fatal_in_progress) {
      // Keep the first error's message; this thread waits in the message box hook.
      t_fatal_secondary = true;
      return;
    }
    g_fatal_in_progress = YES;
    if (g_state) {
      g_state[@"state"] = kStateFatal;
      g_state[@"message"] = text;
      WriteStateLocked();
    }
  }
}

void HookShowMessageBox(xe::SimpleMessageBoxType type, std::string_view message) {
  if (t_fatal_secondary) {
    // The first fataling thread owns the alert and the exit; exiting here would
    // close its alert while the user reads it.
    for (;;) {
      if ([NSThread isMainThread]) {
        @autoreleasepool {
          CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        }
      } else {
        sleep(3600);
      }
    }
  }
  @autoreleasepool {
    BOOL fatal;
    {
      std::lock_guard<std::mutex> lock(StateMutex());
      fatal = g_fatal_in_progress;
    }
    NSString* title =
        type == xe::SimpleMessageBoxType::Error
            ? NSLocalizedString(@"XeniOS Error", @"Title of a fatal or error message box")
        : type == xe::SimpleMessageBoxType::Warning
            ? NSLocalizedString(@"XeniOS Warning", @"Title of a warning message box")
            : NSLocalizedString(@"XeniOS", @"Title of an information message box");
    // Messages can carry host paths from exceptions and config errors.
    NSString* body = ToNS(xe::strip_pii(message));
    if (fatal) {
      body = [body
          stringByAppendingString:
              NSLocalizedString(
                  @"\n\nXeniOS has to close. The log is saved; open XeniOS again to share it.",
                  @"Message box addendum when the app must close")];
    }
    ShowBlockingAlert(title, body,
                      fatal ? NSLocalizedString(@"Close", @"Message box button")
                            : NSLocalizedString(@"OK", @"Message box button"));
  }
}

void HookOpenURL(std::string_view url) {
  @autoreleasepool {
    if (url.empty() || url.size() > 2048) {
      return;
    }
    NSString* url_string = ToNS(url);
    dispatch_async(dispatch_get_main_queue(), ^{
      NSURL* target = [NSURL URLWithString:url_string];
      NSString* scheme = target.scheme.lowercaseString;
      if (!target || !([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"])) {
        XELOGW("iOS: Ignoring non-web URL: {}", Redacted(url_string));
        return;
      }
      [[UIApplication sharedApplication] openURL:target
                                         options:@{}
                               completionHandler:^(BOOL success) {
                                 if (!success) {
                                   XELOGW("iOS: Could not open URL: {}", Redacted(url_string));
                                 }
                               }];
    });
  }
}

// Opens the Files app at a folder inside this app's Documents (the app
// exposes Documents to Files). shareddocuments:// is not a documented API, so
// anything outside Documents, or a failed open, is only logged.
void HookOpenFileExplorer(const std::filesystem::path& path) {
  @autoreleasepool {
    NSString* path_string = ToNS(path.string());
    dispatch_async(dispatch_get_main_queue(), ^{
      NSFileManager* fm = [NSFileManager defaultManager];
      BOOL is_directory = NO;
      NSString* target_path = [path_string stringByStandardizingPath];
      if (![fm fileExistsAtPath:target_path isDirectory:&is_directory]) {
        XELOGD("iOS: Not opening Files; path does not exist: {}", Redacted(path_string));
        return;
      }
      if (!is_directory) {
        target_path = [target_path stringByDeletingLastPathComponent];
      }
      NSString* documents =
          [ToNS(xe_get_ios_documents_path().string()) stringByResolvingSymlinksInPath];
      NSString* resolved = [target_path stringByResolvingSymlinksInPath];
      if (!([resolved isEqualToString:documents] ||
            [resolved hasPrefix:[documents stringByAppendingString:@"/"]])) {
        XELOGD("iOS: Not opening Files; path is outside Documents: {}", Redacted(path_string));
        return;
      }
      NSURLComponents* components = [[[NSURLComponents alloc] init] autorelease];
      components.scheme = @"shareddocuments";
      components.host = @"";  // shareddocuments:///path; unverified, needs a device check
      components.path = resolved;
      NSURL* url = components.URL;
      if (!url) {
        return;
      }
      [[UIApplication sharedApplication] openURL:url
                                         options:@{}
                               completionHandler:^(BOOL success) {
                                 if (!success) {
                                   XELOGD("iOS: Files did not open: {}", Redacted(path_string));
                                 }
                               }];
    });
  }
}

// ---- Log sharing ----------------------------------------------------------

void PresentShare(UIViewController* presenter, UIView* source_view,
                  UIBarButtonItem* bar_button, BOOL previous_session) {
  UIViewController* top = TopOf(presenter);
  if (!top) {
    return;
  }
  const std::filesystem::path log_path = CurrentLogPath();
  NSString* source_log = ToNS((previous_session ? PreviousLogPath(log_path) : log_path).string());
  const char* log_name = previous_session ? "xenia.previous.log" : "xenia.log";
  if (![[NSFileManager defaultManager] fileExistsAtPath:source_log]) {
    UIAlertController* alert = [UIAlertController
        alertControllerWithTitle:NSLocalizedString(
                                     @"Log Not Found",
                                     @"Alert title: the log file to share does not exist")
                         message:[NSString stringWithFormat:
                                               NSLocalizedString(
                                                   @"No %s file exists yet.",
                                                   @"Alert message; %s is the log file name"),
                                               log_name]
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert
        addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button: dismiss")
                                           style:UIAlertActionStyleDefault
                                         handler:nil]];
    [top presentViewController:alert animated:YES completion:nil];
    return;
  }

  // Anchor for the iPad popover, kept as plain values for the async hop.
  UIView* anchor_view = source_view ? source_view : top.view;
  const CGRect anchor_rect = source_view
                                 ? source_view.bounds
                                 : CGRectMake(CGRectGetMidX(top.view.bounds),
                                              CGRectGetMidY(top.view.bounds), 1, 1);
  const BOOL no_arrow = !source_view && !bar_button;
  NSString* summary = nil;
  if (previous_session) {
    std::lock_guard<std::mutex> lock(StateMutex());
    summary = [SessionSummaryLocked() retain];
  }

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      NSFileManager* fm = [NSFileManager defaultManager];
      NSString* directory = [ShareRootDirectory()
          stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
      [fm createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
      NSMutableArray<NSURL*>* items = [NSMutableArray array];
      NSString* log_copy =
          [directory stringByAppendingPathComponent:[NSString stringWithUTF8String:log_name]];
      if (WriteRedactedCopy(source_log, log_copy, false)) {
        [items addObject:[NSURL fileURLWithPath:log_copy]];
      }
      if (summary) {
        NSString* summary_path =
            [directory stringByAppendingPathComponent:@"xenia.previous.session.txt"];
        if ([summary writeToFile:summary_path
                      atomically:NO
                        encoding:NSUTF8StringEncoding
                           error:nil]) {
          [items addObject:[NSURL fileURLWithPath:summary_path]];
        }
        for (NSString* diagnostic in NewestDiagnosticFiles(2)) {
          NSString* copy = [directory stringByAppendingPathComponent:diagnostic.lastPathComponent];
          if (WriteRedactedCopy(diagnostic, copy, true)) {
            [items addObject:[NSURL fileURLWithPath:copy]];
          }
        }
      }
      [summary release];
      dispatch_async(dispatch_get_main_queue(), ^{
        if (items.count == 0) {
          [fm removeItemAtPath:directory error:nil];
          return;
        }
        UIActivityViewController* share_controller =
            [[[UIActivityViewController alloc] initWithActivityItems:items
                                               applicationActivities:nil] autorelease];
        share_controller.completionWithItemsHandler =
            ^(UIActivityType __unused type, BOOL __unused completed, NSArray* __unused returned,
              NSError* __unused error) {
              [[NSFileManager defaultManager] removeItemAtPath:directory error:nil];
            };
        UIPopoverPresentationController* popover = share_controller.popoverPresentationController;
        if (popover) {
          if (bar_button) {
            popover.barButtonItem = bar_button;
          } else {
            popover.sourceView = anchor_view;
            popover.sourceRect = anchor_rect;
            if (no_arrow) {
              popover.permittedArrowDirections = 0;
            }
          }
        }
        [TopOf(presenter) presentViewController:share_controller animated:YES completion:nil];
      });
    }
  });
}

}  // namespace

#if XE_IOS_HAS_METRICKIT
namespace {

// The crashing thread's frames, top first, as crash records carry them:
// [{image, offset}], with binaryUUID on XeniOS's own frames for symbolication.
// The other threads stay on the device.
NSArray* MetricKitFrames(MXCallStackTree* tree) {
  NSData* json = [tree JSONRepresentation];
  id root = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
  NSArray* stacks = [root isKindOfClass:[NSDictionary class]] ? root[@"callStacks"] : nil;
  if (![stacks isKindOfClass:[NSArray class]]) {
    return @[];
  }
  NSDictionary* stack = nil;
  for (id candidate in stacks) {
    if ([candidate isKindOfClass:[NSDictionary class]] &&
        (!stack || [candidate[@"threadAttributed"] boolValue])) {
      stack = candidate;
      if ([candidate[@"threadAttributed"] boolValue]) {
        break;
      }
    }
  }
  NSMutableArray* frames = [NSMutableArray array];
  id level = stack[@"callStackRootFrames"];
  while ([level isKindOfClass:[NSArray class]] && [level count] &&
         frames.count < kMaxMetricKitFrames) {
    NSDictionary* frame = [level firstObject];
    if (![frame isKindOfClass:[NSDictionary class]]) {
      break;
    }
    NSString* image = frame[@"binaryName"];
    image = [image isKindOfClass:[NSString class]] ? image : @"unknown";
    NSMutableDictionary* out = [NSMutableDictionary dictionary];
    out[@"image"] = image;
    out[@"offset"] = [NSString
        stringWithFormat:@"0x%llx", [frame[@"offsetIntoBinaryTextSegment"] unsignedLongLongValue]];
    if ([image isEqualToString:@"XeniOS"] && [frame[@"binaryUUID"] isKindOfClass:[NSString class]]) {
      out[@"binaryUUID"] = frame[@"binaryUUID"];
    }
    [frames addObject:out];
    level = frame[@"subFrames"];
  }
  return frames;
}

NSDictionary* MetricKitCrashBody(MXCrashDiagnostic* crash) {
  NSMutableDictionary* body = [NSMutableDictionary dictionary];
  body[@"source"] = @"metrickit";
  body[@"type"] = @"exception";
  if (crash.signal) {
    body[@"signal"] = crash.signal;
  }
  if (crash.exceptionType) {
    body[@"exceptionType"] = crash.exceptionType;
  }
  if (crash.exceptionCode) {
    body[@"exceptionCodes"] = @[ [NSString
        stringWithFormat:@"0x%llx", crash.exceptionCode.unsignedLongLongValue] ];
  }
  // Why the system ended the app, such as a watchdog's 0x8badf00d.
  if (NSString* reason = crash.terminationReason; reason.length) {
    body[@"terminationReason"] = reason.length > kMaxTerminationReasonChars
                                     ? [reason substringToIndex:kMaxTerminationReasonChars]
                                     : reason;
  }
  body[@"frames"] = MetricKitFrames(crash.callStackTree);
  return body;
}

}  // namespace

@implementation XeniaMetricSubscriber

- (void)didReceiveDiagnosticPayloads:(NSArray<MXDiagnosticPayload*>*)payloads {
  @autoreleasepool {
    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* directory = DiagnosticsDirectory();
    [fm createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter* formatter = [[[NSDateFormatter alloc] init] autorelease];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.dateFormat = @"yyyyMMdd-HHmmss";
    NSString* stamp = [formatter stringFromDate:[NSDate date]];
    NSUInteger index = 0;
    for (MXDiagnosticPayload* payload in payloads) {
      for (MXCrashDiagnostic* crash in payload.crashDiagnostics) {
        xe_ios_diagnostics_note_metrickit_crash(MetricKitCrashBody(crash), payload.timeStampBegin,
                                                payload.timeStampEnd);
      }
      NSData* json = [payload JSONRepresentation];
      if (!json.length) {
        continue;
      }
      NSString* name = [NSString stringWithFormat:@"%@-%lu.json", stamp, (unsigned long)index++];
      [json writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES];
    }
    NSArray<NSString*>* names = [[fm contentsOfDirectoryAtPath:directory error:nil]
        sortedArrayUsingSelector:@selector(compare:)];
    for (NSInteger i = 0; i + static_cast<NSInteger>(kMaxSavedDiagnostics) <
                          static_cast<NSInteger>(names.count);
         ++i) {
      [fm removeItemAtPath:[directory stringByAppendingPathComponent:names[i]] error:nil];
    }
    XELOGI("iOS: Saved {} MetricKit diagnostic payload(s)", payloads.count);
  }
}

@end
#endif  // XE_IOS_HAS_METRICKIT

void xe_ios_begin_session_log(const std::filesystem::path& log_path) {
  @autoreleasepool {
    {
      std::lock_guard<std::mutex> lock(StateMutex());
      if (g_session_started) {
        return;
      }
      g_session_started = YES;
      g_log_path = log_path;
    }

    NSFileManager* fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:SupportDirectory()
        withIntermediateDirectories:YES
                         attributes:nil
                              error:nil];
    // Leftovers of shares that never completed (the app was killed meanwhile).
    [fm removeItemAtPath:ShareRootDirectory() error:nil];

    NSDictionary* previous = [NSDictionary dictionaryWithContentsOfFile:StatePath()];
    NSString* previous_state = previous[@"state"];
    // Last seen in the foreground, or a recorded FatalError. A background or
    // normal end is not a crash: iOS kills suspended apps without telling us.
    // Neither is a foreground end followed by a different install: installing
    // the app kills the copy that was running.
    const BOOL same_install = [previous[@"install"] isEqualToString:AppInstallString()];
    const BOOL died = ([previous_state isEqualToString:kStateForeground] && same_install) ||
                      [previous_state isEqualToString:kStateFatal];
    const BOOL was_pending = [previous[@"pending"] boolValue];

    NSMutableDictionary* state = [NSMutableDictionary dictionary];
    if (died) {
      state[@"pending"] = @YES;
      state[@"prev_state"] = previous_state;
      state[@"prev_version"] = previous[@"version"] ?: @"?";
      state[@"prev_game"] = previous[@"game"] ?: @NO;
      if (previous[@"message"]) {
        state[@"prev_message"] = previous[@"message"];
      }
    } else if (was_pending) {
      // Keep the older crash report until it has been offered.
      for (NSString* key in @[ @"pending", @"prev_state", @"prev_version", @"prev_game",
                               @"prev_message" ]) {
        if (previous[key]) {
          state[key] = previous[key];
        }
      }
    }

    // Keep the previous session's log. A pending crash log is only replaced by
    // a newer crash, not by an uneventful session in between.
    if (died || !was_pending) {
      const std::filesystem::path previous_log = PreviousLogPath(log_path);
      std::error_code error;
      if (std::filesystem::exists(log_path, error)) {
        std::filesystem::rename(log_path, previous_log, error);
      }
    }

    const BOOL in_background =
        [NSThread isMainThread] &&
        [UIApplication sharedApplication].applicationState == UIApplicationStateBackground;
    state[@"state"] = in_background ? kStateBackground : kStateForeground;
    state[@"version"] = AppVersionString();
    state[@"install"] = AppInstallString();
    state[@"game"] = @NO;
    {
      std::lock_guard<std::mutex> lock(StateMutex());
      g_state = [state retain];
      WriteStateLocked();
    }

    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSOperationQueue* main_queue = [NSOperationQueue mainQueue];
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification
                        object:nil
                         queue:main_queue
                    usingBlock:^(NSNotification* __unused note) {
                      SetSessionState(kStateBackground);
                    }];
    [center addObserverForName:UIApplicationWillEnterForegroundNotification
                        object:nil
                         queue:main_queue
                    usingBlock:^(NSNotification* __unused note) {
                      SetSessionState(kStateForeground);
                    }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil
                         queue:main_queue
                    usingBlock:^(NSNotification* __unused note) {
                      SetSessionState(kStateForeground);
                    }];
    [center addObserverForName:UIApplicationWillTerminateNotification
                        object:nil
                         queue:main_queue
                    usingBlock:^(NSNotification* __unused note) {
                      SetSessionState(kStateEnded);
                    }];

    xe::IOSSystemHooks hooks = {};
    hooks.show_message_box = HookShowMessageBox;
    hooks.note_fatal_error = HookNoteFatalError;
    hooks.open_url = HookOpenURL;
    hooks.open_file_explorer = HookOpenFileExplorer;
    xe::SetIOSSystemHooks(hooks);

#if XE_IOS_HAS_METRICKIT
    // Held for the life of the process.
    [[MXMetricManager sharedManager] addSubscriber:[[XeniaMetricSubscriber alloc] init]];
#endif
    xe_ios_diagnostics_start(previous);
  }
}

void xe_ios_session_note_game_running(BOOL running) {
  xe_ios_diagnostics_note_game_running(running);
  std::lock_guard<std::mutex> lock(StateMutex());
  if (!g_state || [g_state[@"game"] boolValue] == running ||
      [g_state[@"state"] isEqualToString:kStateFatal]) {
    return;
  }
  g_state[@"game"] = @(running);
  WriteStateLocked();
}

void xe_ios_offer_previous_session_report_if_needed(UIViewController* presenter) {
  if (g_offered_this_launch || !presenter || !presenter.view.window ||
      presenter.presentedViewController) {
    return;
  }
  NSString* summary_text = nil;
  {
    std::lock_guard<std::mutex> lock(StateMutex());
    if (!g_state || ![g_state[@"pending"] boolValue] || [g_state[@"game"] boolValue]) {
      return;
    }
    NSString* message = g_state[@"prev_message"];
    NSMutableString* text = [NSMutableString string];
    if ([g_state[@"prev_game"] boolValue]) {
      [text appendString:NSLocalizedString(
                             @"XeniOS quit during a game. Share the log to help fix it.",
                             @"Alert message after a crash during a game")];
    } else {
      [text appendString:NSLocalizedString(
                             @"Share the log to help fix it.",
                             @"Alert message after a crash outside a game")];
    }
    if (message.length) {
      [text appendFormat:@"\n\n%s", xe::strip_pii([message UTF8String]).c_str()];
    }
    summary_text = text;
  }
  if (![[NSFileManager defaultManager]
          fileExistsAtPath:ToNS(PreviousLogPath(CurrentLogPath()).string())]) {
    g_offered_this_launch = YES;
    std::lock_guard<std::mutex> lock(StateMutex());
    [g_state removeObjectForKey:@"pending"];
    WriteStateLocked();
    return;
  }

  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:
          NSLocalizedString(@"XeniOS Quit Unexpectedly",
                            @"Alert title: shown after the app crashed in the previous session")
                       message:summary_text
                preferredStyle:UIAlertControllerStyleAlert];
  UIViewController* weak_presenter = presenter;
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Dismiss", @"Alert button: dismiss")
                                 style:UIAlertActionStyleCancel
                               handler:nil]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Share Log",
                                                         @"Alert button: shares the log file")
                                 style:UIAlertActionStyleDefault
                               handler:^(UIAlertAction* __unused action) {
                                 xe_ios_present_log_share(weak_presenter, nil, YES);
                               }]];
  alert.preferredAction = alert.actions.lastObject;
  // Offered once per crash, whatever the user picks, but only counted once the
  // alert is really on screen; a failed present is retried at the next appearance.
  [presenter presentViewController:alert
                          animated:YES
                        completion:^{
                          g_offered_this_launch = YES;
                          std::lock_guard<std::mutex> lock(StateMutex());
                          [g_state removeObjectForKey:@"pending"];
                          WriteStateLocked();
                        }];
}

void xe_ios_present_log_share(UIViewController* presenter, UIView* source_view,
                              BOOL previous_session) {
  PresentShare(presenter, source_view, nil, previous_session);
}

void xe_ios_present_log_share_from_bar_button(UIViewController* presenter,
                                              UIBarButtonItem* bar_button,
                                              BOOL previous_session) {
  PresentShare(presenter, nil, bar_button, previous_session);
}

void xe_ios_present_init_failure_alert(UIViewController* presenter, NSString* message) {
  UIViewController* top = TopOf(presenter);
  if (!top) {
    return;
  }
  UIAlertController* alert = [UIAlertController
      alertControllerWithTitle:NSLocalizedString(@"XeniOS could not start",
                                                 @"Alert title: the emulator failed to start")
                       message:message
                preferredStyle:UIAlertControllerStyleAlert];
  [alert addAction:[UIAlertAction actionWithTitle:NSLocalizedString(@"OK", @"Alert button: dismiss")
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
  [alert addAction:[UIAlertAction
                       actionWithTitle:NSLocalizedString(@"Share Log",
                                                         @"Alert button: shares the log file")
                                 style:UIAlertActionStyleDefault
                               handler:^(UIAlertAction* __unused action) {
                                 xe_ios_present_log_share(presenter, nil, NO);
                               }]];
  [top presentViewController:alert animated:YES completion:nil];
}
