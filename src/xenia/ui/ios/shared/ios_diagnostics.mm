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

#include <atomic>

#include "xenia/base/logging.h"
#include "xenia/ui/presenter.h"
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

// Launches and the games they ran, oldest first: [{launch, title, at}], an
// entry when a launch starts, a game starts or ends, or diagnostics are turned
// on. Kept on the device and only while diagnostics can be sent: turning them
// off, or a launch without them, clears it. So a non-empty log at launch means
// the previous launch ended with diagnostics on, and an entry covering a time
// means they were on then, which MetricKit's crash reports (a launch later,
// with a time but no launch) need to be sent and placed.
constexpr NSUInteger kLaunchLogEntries = 32;
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
    [log addObject:@{
      @"launch" : g_launch_id,
      @"title" : @(g_title_id.load(std::memory_order_relaxed)),
      @"at" : @([NSDate date].timeIntervalSince1970),
    }];
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
  if (session) {
    session[@"end"] = UnfinishedSessionEnd(fault, state);
    xe_ios_diagnostics_enqueue(@"session", session);
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
  xe_ios_diagnostics_enqueue(@"crash", crash);
}

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

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kLaunchUploadDelaySeconds * NSEC_PER_SEC),
                   dispatch_get_main_queue(), ^{
                     xe_ios_diagnostics_upload_pending();
                   });
  });
}

NSString* xe_ios_diagnostics_launch_id(void) { return g_launch_id; }

void xe_ios_diagnostics_note_game_running(BOOL running) {
  if (g_game_running.exchange(running, std::memory_order_relaxed) == running) {
    return;
  }
  if (running) {
    if (xe_ios_diagnostics_upload_allowed()) {
      xe_ios_session_stats_begin();
    }
    return;
  }
  g_title_id.store(0, std::memory_order_relaxed);
  NoteLaunchLog();
  xe_ios_fault_record_note_title(0);
  xe_ios_state_reporting_note_title(0);
  if (NSDictionary* session = xe_ios_session_stats_end()) {
    xe_ios_diagnostics_enqueue(@"session", session);
    xe_ios_diagnostics_upload_pending();
  }
}

void xe_ios_diagnostics_note_title(uint32_t title_id, uint64_t jit_base, uint64_t jit_size) {
  g_title_id.store(title_id, std::memory_order_relaxed);
  NoteLaunchLog();
  xe_ios_session_stats_note_title(title_id);
  xe_ios_fault_record_note_title(title_id);
  xe_ios_fault_record_note_jit_range(jit_base, jit_size);
  xe_ios_state_reporting_note_title(title_id);
}

void xe_ios_diagnostics_note_metrickit_crash(NSDictionary* body, NSDate* begin, NSDate* end) {
  if (!xe_ios_diagnostics_upload_allowed() || !begin || !end ||
      [end timeIntervalSinceDate:begin] > kMaxPayloadSpanSeconds) {
    return;
  }
  NSDictionary* entry = LaunchLogEntryAt(end);
  if (!entry) {
    return;
  }
  NSMutableDictionary* crash = [NSMutableDictionary dictionaryWithDictionary:body];
  crash[@"launchId"] = entry[@"launch"];
  if (const uint32_t title_id = [entry[@"title"] unsignedIntValue]) {
    crash[@"titleId"] = [NSString stringWithFormat:@"%08X", title_id];
  }
  xe_ios_diagnostics_enqueue(@"crash", crash);
}

void xe_ios_diagnostics_settings_changed(void) {
  const BOOL allowed = xe_ios_diagnostics_upload_allowed();
  xe_ios_fault_record_note_upload_allowed(allowed);
  if (allowed) {
    NoteLaunchLog();
    xe_ios_diagnostics_upload_pending();
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
