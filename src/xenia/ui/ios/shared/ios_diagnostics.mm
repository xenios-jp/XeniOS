/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_diagnostics.h"

#import <UIKit/UIKit.h>

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "xenia/base/backend_failure.h"
#include "xenia/base/logging.h"
#include "xenia/base/system.h"
#include "xenia/ui/presenter.h"
#import "xenia/ui/ios/shared/ios_crash_reporting.h"
#import "xenia/ui/ios/shared/ios_diagnostics_outbox.h"
#import "xenia/ui/ios/shared/ios_fault_record.h"
#import "xenia/ui/ios/shared/ios_online_features.h"
#import "xenia/ui/ios/shared/ios_session_stats.h"
#import "xenia/ui/ios/shared/ios_state_reporting.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_theme_controls.h"

namespace {

// Pending records go out this long after launch, not during it.
constexpr int64_t kLaunchUploadDelaySeconds = 15;
constexpr NSUInteger kMaxMessageChars = 600;

NSString* g_launch_id = nil;
std::atomic<bool> g_game_running{false};
std::atomic<uint32_t> g_title_id{0};
// Apple's state reporting was told the running title, which it is only while
// xe_ios_title_reporting_allowed.
std::atomic<bool> g_title_reported{false};

void ReportTitleState(uint32_t title_id) {
  if (title_id && xe_ios_title_reporting_allowed()) {
    xe_ios_state_reporting_note_title(title_id);
    g_title_reported.store(true, std::memory_order_relaxed);
  } else if (g_title_reported.exchange(false, std::memory_order_relaxed)) {
    xe_ios_state_reporting_note_title(0);
  }
}

// Launches and the games they ran, oldest first: [{launch, title, at, header}],
// one entry per launch, made when it starts or diagnostics are turned on and
// updated in place with the last game it ran (so playing doesn't push the
// launches of the last day or two out: a MetricKit crash report comes up to a
// day late). Kept on the device and only while diagnostics can be sent:
// turning them off, or a launch without them, clears it. So a non-empty log at
// launch means the previous launch ended with diagnostics on, and an entry
// covering a time means they were on then, which MetricKit's crash reports (a
// launch later, with a time but no launch) need to be sent and placed. header
// is the launch's build and device (xe_ios_diagnostics_envelope_header), which
// records about it found by a later launch carry. crashed is set once the app
// has queued its own crash record for the launch with XeniOS frames: the
// Worker keeps one report per launch and prefers that one to MetricKit's, so
// MetricKit's isn't sent. (One without such frames, a fatal error's, doesn't
// win over MetricKit's, which then goes too.)
constexpr NSUInteger kLaunchLogEntries = 64;
NSString* const kLaunchLogKey = @"XeniaDiagnosticsLaunchLog";
// A MetricKit payload spanning longer than this can't be placed in a launch.
constexpr NSTimeInterval kMaxPayloadSpanSeconds = 60;

NSObject* LaunchLogLock() {
  static NSObject* lock = [[NSObject alloc] init];
  return lock;
}

void ClearLaunchLog() {
  @synchronized(LaunchLogLock()) {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:kLaunchLogKey];
  }
}

void NoteLaunchLog() {
  if (!g_launch_id || !xe_ios_diagnostics_upload_allowed()) {
    return;
  }
  @synchronized(LaunchLogLock()) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSMutableArray* log = [NSMutableArray arrayWithArray:[defaults arrayForKey:kLaunchLogKey] ?: @[]];
    const uint32_t title_id = g_title_id.load(std::memory_order_relaxed);
    NSDictionary* last = log.lastObject;
    if ([last isKindOfClass:[NSDictionary class]] &&
        [last[@"launch"] isEqual:g_launch_id]) {
      // This launch's entry keeps its start time; it only learns the game.
      if (!title_id || [last[@"title"] isEqual:@(title_id)]) {
        return;
      }
      NSMutableDictionary* updated = [NSMutableDictionary dictionaryWithDictionary:last];
      updated[@"title"] = @(title_id);
      log[log.count - 1] = updated;
    } else {
      [log addObject:@{
        @"launch" : g_launch_id,
        @"title" : @(title_id),
        @"at" : @([NSDate date].timeIntervalSince1970),
        @"header" : xe_ios_diagnostics_envelope_header(),
      }];
    }
    if (log.count > kLaunchLogEntries) {
      [log removeObjectsInRange:NSMakeRange(0, log.count - kLaunchLogEntries)];
    }
    [defaults setObject:log forKey:kLaunchLogKey];
  }
}

// The entry of an earlier launch in effect at |time|, or nil.
NSDictionary* LaunchLogEntryAt(NSDate* time) {
  NSDictionary* found = nil;
  @synchronized(LaunchLogLock()) {
    for (id entry in [[NSUserDefaults standardUserDefaults] arrayForKey:kLaunchLogKey]) {
      if (![entry isKindOfClass:[NSDictionary class]] ||
          ![entry[@"at"] isKindOfClass:[NSNumber class]]) {
        continue;
      }
      if ([entry[@"at"] doubleValue] > time.timeIntervalSince1970) {
        break;
      }
      found = entry;
    }
    found = [[found retain] autorelease];
  }
  return [found[@"launch"] isKindOfClass:[NSString class]] &&
                 ![found[@"launch"] isEqualToString:g_launch_id]
             ? found
             : nil;
}

// The headers of the launches in the log, newest first.
NSArray<NSDictionary*>* LoggedHeaders() {
  NSMutableArray<NSDictionary*>* headers = [NSMutableArray array];
  @synchronized(LaunchLogLock()) {
    for (id entry in [[[NSUserDefaults standardUserDefaults] arrayForKey:kLaunchLogKey]
             reverseObjectEnumerator]) {
      if ([entry isKindOfClass:[NSDictionary class]] &&
          [entry[@"header"] isKindOfClass:[NSDictionary class]]) {
        [headers addObject:entry[@"header"]];
      }
    }
  }
  return headers;
}

void NoteLaunchCrashed(NSString* launch_id) {
  @synchronized(LaunchLogLock()) {
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSMutableArray* log =
        [NSMutableArray arrayWithArray:[defaults arrayForKey:kLaunchLogKey] ?: @[]];
    for (NSUInteger i = log.count; i-- > 0;) {
      if ([log[i] isKindOfClass:[NSDictionary class]] && [log[i][@"launch"] isEqual:launch_id]) {
        NSMutableDictionary* entry = [NSMutableDictionary dictionaryWithDictionary:log[i]];
        entry[@"crashed"] = @YES;
        log[i] = entry;
        [defaults setObject:log forKey:kLaunchLogKey];
        return;
      }
    }
  }
}

// The launch log's header for |launch_id|, or, without one, for its last
// entry: at launch, before this launch's entry, the previous launch.
NSDictionary* LoggedHeaderForPreviousLaunch(NSString* launch_id) {
  NSDictionary* found = nil;
  @synchronized(LaunchLogLock()) {
    NSArray* log = [[NSUserDefaults standardUserDefaults] arrayForKey:kLaunchLogKey];
    for (id entry in log) {
      if ([entry isKindOfClass:[NSDictionary class]] &&
          (!found || [entry[@"launch"] isEqual:launch_id])) {
        found = entry;
      }
    }
    if (![found[@"launch"] isEqual:launch_id]) {
      found = [log.lastObject isKindOfClass:[NSDictionary class]] ? log.lastObject : nil;
    }
    found = [[found[@"header"] retain] autorelease];
  }
  return [found isKindOfClass:[NSDictionary class]] ? found : nil;
}

// How a session the previous launch didn't end came to an end, from its fault
// record and session marker.
NSString* UnfinishedSessionEnd(NSDictionary* fault, NSString* state) {
  if (fault) {
    return @"crash";
  }
  if ([state isEqualToString:@"fatal"]) {
    return @"fatal";
  }
  // In the foreground with no fault: a memory-limit kill, the watchdog, or a
  // crash outside Xenia's handler. MetricKit or the extension tells which.
  if ([state isEqualToString:@"foreground"]) {
    return @"killed";
  }
  // Suspended apps end without notice: the system or the app switcher.
  if ([state isEqualToString:@"background"]) {
    return @"background";
  }
  if ([state isEqualToString:@"ended"]) {
    return @"terminated";
  }
  return @"unknown";
}

// |diagnostics_were_on|: at the end of the previous launch. Otherwise what it
// left is deleted unsent.
void QueuePreviousLaunch(NSDictionary* previous_marker, BOOL diagnostics_were_on) {
  NSDictionary* fault = xe_ios_fault_record_take_previous();
  NSMutableDictionary* session = xe_ios_session_stats_take_unfinished();
  // The next-launch notice says so whether or not diagnostics are on.
  if ([fault[@"inJit"] isKindOfClass:[NSNumber class]] && [fault[@"inJit"] boolValue]) {
    xe_ios_note_previous_crash_in_jit();
  }
  if (!diagnostics_were_on) {
    return;
  }
  NSString* state = previous_marker[@"state"];
  const BOOL fatal = [state isEqualToString:@"fatal"];
  // The checkpoint keeps its launch for the crash record; the session record
  // goes without it.
  NSString* session_launch_id = session[@"launchId"];
  [[session_launch_id retain] autorelease];
  [session removeObjectForKey:@"launchId"];
  // Both records are about the previous launch, and carry its build.
  NSString* fault_launch_id =
      [fault[@"launchId"] isKindOfClass:[NSString class]] ? fault[@"launchId"] : nil;
  NSDictionary* logged = LoggedHeaderForPreviousLaunch(fault_launch_id ?: session_launch_id);
  if (session) {
    session[@"end"] = UnfinishedSessionEnd(fault, state);
    xe_ios_diagnostics_enqueue_with_header(
        @"session", session,
        xe_ios_diagnostics_header_for_earlier_launch(logged, nil, LoggedHeaders()));
  }
  if (!fault && !fatal) {
    return;
  }
  NSMutableDictionary* crash = [NSMutableDictionary dictionary];
  crash[@"source"] = @"app";
  crash[@"type"] = fault ? @"fault" : @"fatal";
  if (fault) {
    [crash addEntriesFromDictionary:fault];
  }
  if (session_launch_id && !crash[@"launchId"]) {
    crash[@"launchId"] = session_launch_id;
  }
  if (session[@"titleId"] && !crash[@"titleId"]) {
    crash[@"titleId"] = session[@"titleId"];
  }
  if ([previous_marker[@"version"] isKindOfClass:[NSString class]]) {
    crash[@"appVersion"] = previous_marker[@"version"];
  }
  NSString* message = previous_marker[@"message"];
  if (fatal && [message isKindOfClass:[NSString class]] && message.length) {
    crash[@"message"] = message.length > kMaxMessageChars
                            ? [message substringToIndex:kMaxMessageChars]
                            : message;
  }
  NSString* binary_uuid =
      [fault[@"binaryUUID"] isKindOfClass:[NSString class]] ? fault[@"binaryUUID"] : nil;
  xe_ios_diagnostics_enqueue_with_header(
      @"crash", crash,
      xe_ios_diagnostics_header_for_earlier_launch(logged, binary_uuid, LoggedHeaders()));
  BOOL xenios_frames = NO;
  for (id frame in [crash[@"frames"] isKindOfClass:[NSArray class]] ? crash[@"frames"] : @[]) {
    if ([frame isKindOfClass:[NSDictionary class]] && [frame[@"image"] isEqual:@"XeniOS"]) {
      xenios_frames = YES;
    }
  }
  if (xenios_frames && [crash[@"launchId"] isKindOfClass:[NSString class]]) {
    NoteLaunchCrashed(crash[@"launchId"]);
  }
}

// ---- CPU backend failures (xe::NoteBackendFailure)

// Kinds already shown and recorded in this game session, a bit each: every
// failure counts in the session, but one of a kind alerts and records once.
std::atomic<uint32_t> g_backend_failures_seen{0};

NSString* BackendFailureName(xe::BackendFailure failure) {
  switch (failure) {
    case xe::BackendFailure::kCompileFailure:
      return @"compile";
    case xe::BackendFailure::kCodeCache:
      return @"code-cache";
    case xe::BackendFailure::kJitUnavailable:
      return @"jit-unavailable";
    case xe::BackendFailure::kBackendException:
      return @"exception";
  }
  return @"exception";
}

// Called on the failing guest or compiler thread, which mustn't wait on the
// alert: the record and the alert go from a queue of their own.
void HandleBackendFailure(xe::BackendFailure failure, uint32_t guest_address, bool continues) {
  xe_ios_session_stats_note_backend_failure();
  const uint32_t bit = 1u << static_cast<uint32_t>(failure);
  if (g_backend_failures_seen.fetch_or(bit, std::memory_order_relaxed) & bit) {
    return;
  }
  const uint32_t title_id = g_title_id.load(std::memory_order_relaxed);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    @autoreleasepool {
      NSString* name = BackendFailureName(failure);
      NSMutableDictionary* record = [NSMutableDictionary dictionary];
      record[@"source"] = @"app";
      record[@"type"] = @"backend";
      record[@"failure"] = name;
      record[@"continued"] = @(continues);
      if (guest_address) {
        record[@"guestAddress"] = [NSString stringWithFormat:@"%08X", guest_address];
      }
      if (title_id) {
        record[@"titleId"] = [NSString stringWithFormat:@"%08X", title_id];
      }
      if (g_launch_id) {
        record[@"launchId"] = g_launch_id;
      }
      record[@"message"] =
          guest_address ? [NSString stringWithFormat:@"A64 %@ failure at guest %08X", name,
                                                     guest_address]
                        : [NSString stringWithFormat:@"A64 %@ failure", name];
      // Only with Online Features and Share Diagnostics on (the outbox checks).
      xe_ios_diagnostics_enqueue(@"crash", record);
      // A backend that couldn't start fails the launch, which says so itself.
      if (!continues) {
        return;
      }
      NSString* text = NSLocalizedString(
          @"XeniOS couldn't translate part of this game's code to run on this device (a JIT "
          @"error). The game will probably freeze, crash or misbehave when it reaches that code. "
          @"Quitting and starting it again won't fix it. With Share Diagnostics on, a report goes "
          @"to the developers.",
          @"Alert when the CPU recompiler fails during a game");
      xe::ShowSimpleMessageBox(xe::SimpleMessageBoxType::Warning, text.UTF8String);
    }
  });
}

#if TARGET_OS_SIMULATOR || !defined(NDEBUG)
// Development checks of the paths above, from launch arguments (never in a
// device release build):
//   -XeniOSDebugBackendFailure compile|code-cache|exception|jit-unavailable
//       in a stand-in game session that ends 5 s later (session and crash records);
//   -XeniOSDebugFatal YES       xe::FatalError;
//   -XeniOSDebugJitCrash YES    a jump into a page noted as JIT code, so the
//       next launch shows the JIT crash notice.
void RunDebugTriggers() {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString* failure = [defaults stringForKey:@"XeniOSDebugBackendFailure"];
  const BOOL fatal = [defaults boolForKey:@"XeniOSDebugFatal"];
  const BOOL jit_crash = [defaults boolForKey:@"XeniOSDebugJitCrash"];
  if (!failure && !fatal && !jit_crash) {
    return;
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    if (failure) {
      const NSArray<NSString*>* names = @[ @"compile", @"code-cache", @"jit-unavailable", @"exception" ];
      const NSUInteger index = [names indexOfObject:failure];
      if (index == NSNotFound) {
        return;
      }
      XELOGW("iOS: Debug trigger: backend failure {}", failure.UTF8String);
      xe_ios_diagnostics_note_game_running(YES);
      xe_ios_diagnostics_note_title(0x4D5307E6, 0, 0);
      std::thread([index] {
        xe::NoteBackendFailure(static_cast<xe::BackendFailure>(index), 0x82001234,
                               index != 2);
        xe::NoteBackendFailure(static_cast<xe::BackendFailure>(index), 0x82005678,
                               index != 2);
      }).detach();
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                     dispatch_get_main_queue(), ^{
                       xe_ios_diagnostics_note_game_running(NO);
                     });
    }
    if (fatal) {
      std::thread([] { xe::FatalError("Debug trigger: a forced fatal error."); }).detach();
    }
    if (jit_crash) {
      const size_t size = getpagesize();
      void* page = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (page == MAP_FAILED) {
        return;
      }
      XELOGW("iOS: Debug trigger: jumping into a non-executable page noted as JIT code");
      xe_ios_fault_record_note_jit_range(reinterpret_cast<uint64_t>(page), size);
      reinterpret_cast<void (*)()>(page)();
    }
  });
}
#endif  // TARGET_OS_SIMULATOR || !defined(NDEBUG)

}  // namespace

// Settings > About > Pending Reports. Laid out like the log viewer: the text
// view is a constrained subview and gets its text once it is on screen.
@interface XeniaPendingReportsViewController : UIViewController
@end

@implementation XeniaPendingReportsViewController {
  UITextView* text_view_;
}

- (void)dealloc {
  [text_view_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Pending Reports",
                                 @"Diagnostics: crash reports and statistics not sent yet");
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  text_view_ = [[UITextView alloc] init];
  text_view_.translatesAutoresizingMaskIntoConstraints = NO;
  text_view_.editable = NO;
  text_view_.selectable = YES;
  text_view_.alwaysBounceVertical = YES;
  text_view_.backgroundColor = [XeniaTheme bgSurface2];
  text_view_.textColor = [XeniaTheme textPrimary];
  text_view_.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);
  [self.view addSubview:text_view_];
  UILayoutGuide* guide = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [text_view_.topAnchor constraintEqualToAnchor:guide.topAnchor constant:8],
    [text_view_.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor constant:10],
    [text_view_.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor constant:-10],
    [text_view_.bottomAnchor constraintEqualToAnchor:guide.bottomAnchor constant:-8],
  ]];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  NSArray<NSString*>* reports = xe_ios_diagnostics_pending_reports();
  if (reports.count) {
    xe_apply_text_view_font(text_view_, UIFontTextStyleFootnote, 12.0, UIFontWeightRegular, YES);
    text_view_.text = [reports componentsJoinedByString:@"\n\n"];
  } else {
    xe_apply_text_view_font(text_view_, UIFontTextStyleBody, 17.0, UIFontWeightRegular, NO);
    text_view_.text = NSLocalizedString(@"No reports are waiting to be sent.",
                                        @"Diagnostics: the pending reports screen when empty");
  }
}

@end

void xe_ios_diagnostics_start(NSDictionary* previous_marker) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    g_launch_id = [[NSUUID UUID].UUIDString retain];
    BOOL diagnostics_were_on;
    @synchronized(LaunchLogLock()) {
      diagnostics_were_on =
          [[NSUserDefaults standardUserDefaults] arrayForKey:kLaunchLogKey].count != 0;
    }
    QueuePreviousLaunch(previous_marker, diagnostics_were_on);
    if (xe_ios_diagnostics_upload_allowed()) {
      NoteLaunchLog();
    } else {
      ClearLaunchLog();
    }

    NSData* header = [NSJSONSerialization dataWithJSONObject:xe_ios_diagnostics_envelope_header()
                                                     options:NSJSONWritingSortedKeys
                                                       error:nil];
    xe_ios_fault_record_install(g_launch_id, xe_ios_diagnostics_endpoint().absoluteString,
                                header);
    xe_ios_fault_record_note_upload_allowed(xe_ios_diagnostics_upload_allowed());
    xe::ui::Presenter::SetGuestOutputRefreshObserver(xe_ios_session_stats_note_guest_frame);
    xe::SetBackendFailureHandler(HandleBackendFailure);
#if TARGET_OS_SIMULATOR || !defined(NDEBUG)
    RunDebugTriggers();
#endif

    // Pending records go out at launch and on coming back to the foreground,
    // and at no other time.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kLaunchUploadDelaySeconds * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
                     xe_ios_diagnostics_upload_pending();
                   });
    [[NSNotificationCenter defaultCenter]
        addObserverForName:UIApplicationWillEnterForegroundNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* __unused note) {
                  dispatch_after(
                      dispatch_time(DISPATCH_TIME_NOW, kLaunchUploadDelaySeconds * NSEC_PER_SEC),
                      dispatch_get_main_queue(), ^{
                        xe_ios_diagnostics_upload_pending();
                      });
                }];
  });
}

NSString* xe_ios_diagnostics_launch_id(void) { return g_launch_id; }

void xe_ios_diagnostics_note_game_running(BOOL running) {
  if (g_game_running.exchange(running, std::memory_order_relaxed) == running) {
    return;
  }
  if (running) {
    g_backend_failures_seen.store(0, std::memory_order_relaxed);
    if (xe_ios_diagnostics_upload_allowed()) {
      xe_ios_session_stats_begin();
    }
    return;
  }
  g_title_id.store(0, std::memory_order_relaxed);
  NoteLaunchLog();
  xe_ios_fault_record_note_title(0);
  ReportTitleState(0);
  if (NSDictionary* session = xe_ios_session_stats_end()) {
    xe_ios_diagnostics_enqueue(@"session", session);
  }
}

void xe_ios_diagnostics_note_title(uint32_t title_id, uint64_t jit_base, uint64_t jit_size) {
  g_title_id.store(title_id, std::memory_order_relaxed);
  NoteLaunchLog();
  xe_ios_session_stats_note_title(title_id);
  xe_ios_fault_record_note_title(title_id);
  xe_ios_fault_record_note_jit_range(jit_base, jit_size);
  ReportTitleState(title_id);
}

void xe_ios_diagnostics_note_metrickit_crash(NSDictionary* body, NSDate* begin, NSDate* end) {
  if (!xe_ios_diagnostics_upload_allowed() || !begin || !end ||
      [end timeIntervalSinceDate:begin] > kMaxPayloadSpanSeconds) {
    return;
  }
  NSDictionary* entry = LaunchLogEntryAt(end);
  if (!entry || [entry[@"crashed"] boolValue]) {
    return;
  }
  NSMutableDictionary* crash = [NSMutableDictionary dictionaryWithDictionary:body];
  crash[@"launchId"] = entry[@"launch"];
  if (const uint32_t title_id = [entry[@"title"] unsignedIntValue]) {
    crash[@"titleId"] = [NSString stringWithFormat:@"%08X", title_id];
  }
  // The build of the launch it placed the crash in, unless XeniOS's frames
  // name another binary.
  NSString* binary_uuid = nil;
  for (id frame in [body[@"frames"] isKindOfClass:[NSArray class]] ? body[@"frames"] : @[]) {
    if ([frame isKindOfClass:[NSDictionary class]] &&
        [frame[@"binaryUUID"] isKindOfClass:[NSString class]]) {
      binary_uuid = frame[@"binaryUUID"];
      break;
    }
  }
  xe_ios_diagnostics_enqueue_with_header(
      @"crash", crash,
      xe_ios_diagnostics_header_for_earlier_launch(entry[@"header"], binary_uuid,
                                                   LoggedHeaders()));
}

void xe_ios_diagnostics_settings_changed(void) {
  const BOOL allowed = xe_ios_diagnostics_upload_allowed();
  xe_ios_fault_record_note_upload_allowed(allowed);
  // Apple's state reporting hears the title only while xe_ios_title_reporting_allowed.
  ReportTitleState(
      xe_ios_title_reporting_allowed() && g_game_running.load(std::memory_order_relaxed)
          ? g_title_id.load(std::memory_order_relaxed)
          : 0);
  if (allowed) {
    NoteLaunchLog();
  } else {
    // Nothing recorded while they were on is sent later, and nothing more is
    // recorded.
    xe_ios_session_stats_discard();
    ClearLaunchLog();
    xe_ios_diagnostics_clear_outbox();
  }
}

UIViewController* xe_ios_diagnostics_make_pending_reports_controller(void) {
  return [[[XeniaPendingReportsViewController alloc] init] autorelease];
}
