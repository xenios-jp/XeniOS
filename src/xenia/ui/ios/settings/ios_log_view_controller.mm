/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_log_view_controller.h"

#include <sys/stat.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/ios/shared/ios_crash_reporting.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/ui/ios/shared/ios_theme.h"
#include "xenia/ui/ios/shared/ios_theme_controls.h"

DECLARE_path(log_file);

namespace {

std::filesystem::path GetLogFilePath() {
  if (!cvars::log_file.empty()) {
    return cvars::log_file;
  }
  return xe_get_ios_documents_path() / "xenia.log";
}

std::string ReadFileTail(const std::filesystem::path& path, size_t max_bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::string();
  }

  file.seekg(0, std::ios::end);
  const std::streamoff end = file.tellg();
  if (end <= 0) {
    return std::string();
  }

  const std::streamoff max_read = static_cast<std::streamoff>(max_bytes);
  const std::streamoff start = end > max_read ? (end - max_read) : 0;
  file.seekg(start, std::ios::beg);

  std::string content(static_cast<size_t>(end - start), '\0');
  file.read(content.data(), static_cast<std::streamsize>(content.size()));
  content.resize(static_cast<size_t>(file.gcount()));

  if (start > 0) {
    size_t first_newline = content.find('\n');
    if (first_newline != std::string::npos) {
      content.erase(0, first_newline + 1);
    }
  }
  return content;
}

NSString* DecodeLogBytesToNSString(const std::string& content) {
  if (content.empty()) {
    return @"";
  }

  NSData* log_data = [NSData dataWithBytes:content.data() length:content.size()];
  NSString* decoded =
      [[[NSString alloc] initWithData:log_data encoding:NSUTF8StringEncoding] autorelease];
  if (!decoded) {
    decoded = [[[NSString alloc]
        initWithData:log_data
            encoding:CFStringConvertEncodingToNSStringEncoding(kCFStringEncodingWindowsLatin1)]
        autorelease];
  }
  if (!decoded) {
    decoded =
        [[[NSString alloc] initWithData:log_data encoding:NSISOLatin1StringEncoding] autorelease];
  }
  if (decoded) {
    return decoded;
  }

  // Last-resort byte-preserving fallback so the UI never shows a hard decode
  // failure marker.
  NSMutableString* fallback = [NSMutableString stringWithCapacity:content.size()];
  for (unsigned char byte : content) {
    if (byte == '\n' || byte == '\r' || byte == '\t' || (byte >= 0x20 && byte <= 0x7E)) {
      [fallback appendFormat:@"%c", byte];
    } else {
      [fallback appendFormat:@"\\x%02X", byte];
    }
  }
  return fallback;
}

}  // namespace

@implementation XeniaLogViewController {
  UITextView* textView_;
  UILabel* footerLabel_;
  NSTimer* autoRefreshTimer_;
  UIBarButtonItem* pauseButton_;
  BOOL paused_;
  // Reads run on readQueue_; reading_ is main-thread only, the last-seen file
  // state is only touched on readQueue_.
  dispatch_queue_t readQueue_;
  BOOL reading_;
  BOOL forcePending_;
  off_t lastSize_;
  struct timespec lastMtime_;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = NSLocalizedString(@"Live Log", @"log view controller: title");
  readQueue_ = dispatch_queue_create("xenia.log-view.read", DISPATCH_QUEUE_SERIAL);
  lastSize_ = -1;
  self.view.backgroundColor = [UIColor systemBackgroundColor];

  pauseButton_ =
      [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"pause.fill"]
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(togglePauseTapped:)];
  [self updatePauseButton];
  UIBarButtonItem* more_button = [[[UIBarButtonItem alloc]
      initWithImage:[UIImage systemImageNamed:@"ellipsis.circle"]
               menu:[self buildActionsMenu]] autorelease];
  self.navigationItem.rightBarButtonItems = @[ more_button, pauseButton_ ];

  textView_ = [[UITextView alloc] init];
  textView_.translatesAutoresizingMaskIntoConstraints = NO;
  textView_.editable = NO;
  textView_.selectable = YES;
  textView_.alwaysBounceVertical = YES;
  textView_.backgroundColor = [XeniaTheme bgSurface2];
  xe_apply_text_view_font(textView_, UIFontTextStyleFootnote, 12.0,
                          UIFontWeightRegular, YES);
  textView_.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);
  [self.view addSubview:textView_];

  footerLabel_ = [[UILabel alloc] init];
  footerLabel_.translatesAutoresizingMaskIntoConstraints = NO;
  footerLabel_.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(footerLabel_, UIFontTextStyleCaption1, 12.0,
                      UIFontWeightRegular);
  footerLabel_.numberOfLines = 2;
  [self.view addSubview:footerLabel_];

  UILayoutGuide* guide = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [textView_.topAnchor constraintEqualToAnchor:guide.topAnchor constant:8],
    [textView_.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor constant:10],
    [textView_.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor constant:-10],
    [textView_.bottomAnchor constraintEqualToAnchor:footerLabel_.topAnchor constant:-8],
    [footerLabel_.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor constant:12],
    [footerLabel_.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor constant:-12],
    [footerLabel_.bottomAnchor constraintEqualToAnchor:guide.bottomAnchor constant:-8],
  ]];
}

- (void)viewWillAppear:(BOOL)animated {
  [super viewWillAppear:animated];
  [self updateCloseButton];
  [self reloadLogForced:YES];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [self startAutoRefresh];
}

- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];
  [self stopAutoRefresh];
}

- (void)dealloc {
  [self stopAutoRefresh];
  [pauseButton_ release];
  [textView_ release];
  [footerLabel_ release];
  if (readQueue_) {
    dispatch_release(readQueue_);
  }
  [super dealloc];
}

- (void)reloadLogTapped:(id)sender {
  [self reloadLogForced:YES];
}

- (UIMenu*)buildActionsMenu {
  __unsafe_unretained XeniaLogViewController* weak_self = self;
  UIAction* copy_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Copy", @"log view controller: title")
                          image:[UIImage systemImageNamed:@"doc.on.doc"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [weak_self copyLog];
                        }];
  UIAction* share_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Share", @"log view controller: title")
                          image:[UIImage systemImageNamed:@"square.and.arrow.up"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [weak_self shareLogTapped:nil];
                        }];
  UIAction* refresh_action =
      [UIAction actionWithTitle:NSLocalizedString(@"Refresh", @"log view controller: title")
                          image:[UIImage systemImageNamed:@"arrow.clockwise"]
                     identifier:nil
                        handler:^(__unused UIAction* action) {
                          [weak_self reloadLogForced:YES];
                        }];
  return [UIMenu menuWithTitle:@"" children:@[ copy_action, share_action, refresh_action ]];
}

- (void)togglePauseTapped:(id)sender {
  paused_ = !paused_;
  if (paused_) {
    [self stopAutoRefresh];
    footerLabel_.text = NSLocalizedString(@"Paused. Resume to continue the live tail.",
                                          @"log view controller: section footer");
  } else {
    [self startAutoRefresh];
    [self reloadLogForced:YES];
  }
  [self updatePauseButton];
}

- (void)updatePauseButton {
  pauseButton_.image = [UIImage systemImageNamed:(paused_ ? @"play.fill" : @"pause.fill")];
  pauseButton_.accessibilityLabel =
      paused_ ? NSLocalizedString(@"Resume live log", @"log view controller: accessibility label")
              : NSLocalizedString(@"Pause live log", @"log view controller: accessibility label");
}

- (void)copyLog {
  [UIPasteboard generalPasteboard].string = textView_.text.length ? textView_.text : @"";
}

- (void)closeTapped:(id)sender {
  [self dismissViewControllerAnimated:YES completion:nil];
}

- (BOOL)shouldShowCloseButton {
  UINavigationController* nav = self.navigationController;
  if (!nav || nav.presentingViewController == nil) {
    return NO;
  }
  return nav.viewControllers.count > 0 && nav.viewControllers.firstObject == self;
}

- (void)updateCloseButton {
  if ([self shouldShowCloseButton]) {
    self.navigationItem.leftBarButtonItem =
        [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                       target:self
                                                       action:@selector(closeTapped:)] autorelease];
  } else {
    self.navigationItem.leftBarButtonItem = nil;
  }
}

- (void)startAutoRefresh {
  if (autoRefreshTimer_ || paused_) {
    return;
  }
  autoRefreshTimer_ = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                       target:self
                                                     selector:@selector(autoRefreshTick:)
                                                     userInfo:nil
                                                      repeats:YES];
  autoRefreshTimer_.tolerance = 0.15;
}

- (void)stopAutoRefresh {
  if (!autoRefreshTimer_) {
    return;
  }
  [autoRefreshTimer_ invalidate];
  autoRefreshTimer_ = nil;
}

- (void)autoRefreshTick:(NSTimer* __unused)timer {
  [self reloadLogForced:NO];
}

- (BOOL)handleControllerActions:(const xe::ui::apple::ControllerActionSet&)actions {
  if (actions.context) {
    [self shareLogTapped:nil];
    return YES;
  }
  if (actions.quick_action) {
    [self reloadLogTapped:nil];
    return YES;
  }
  if (!(actions.navigate_up || actions.navigate_down || actions.page_prev || actions.page_next)) {
    return NO;
  }

  CGFloat step = MAX(40.0, CGRectGetHeight(textView_.bounds) * 0.45);
  CGFloat offset = textView_.contentOffset.y;
  if (actions.navigate_up) {
    offset -= step;
  }
  if (actions.navigate_down) {
    offset += step;
  }
  if (actions.page_prev) {
    offset -= CGRectGetHeight(textView_.bounds);
  }
  if (actions.page_next) {
    offset += CGRectGetHeight(textView_.bounds);
  }
  CGFloat max_offset = MAX(0.0, textView_.contentSize.height - CGRectGetHeight(textView_.bounds));
  offset = MIN(MAX(0.0, offset), max_offset);
  [textView_ setContentOffset:CGPointMake(0.0, offset) animated:YES];
  return YES;
}

- (BOOL)isNearBottom {
  CGFloat content_height = textView_.contentSize.height;
  CGFloat visible_height = CGRectGetHeight(textView_.bounds);
  if (content_height <= visible_height + 1.0) {
    return YES;
  }
  CGFloat bottom = textView_.contentOffset.y + visible_height;
  return bottom >= content_height - 36.0;
}

- (void)shareLogTapped:(id)sender {
  // Shares a redacted copy (container paths and UUIDs removed).
  xe_ios_present_log_share_from_bar_button(
      self, self.navigationItem.rightBarButtonItems.firstObject, NO);
}

// Reads the tail off the main thread and only touches the text view when the
// file's size or modification time changed (or force is set).
- (void)reloadLogForced:(BOOL)force {
  if (reading_) {
    forcePending_ = forcePending_ || force;
    return;
  }
  reading_ = YES;
  static constexpr size_t kMaxLogBytes = 256 * 1024;
  const std::filesystem::path log_path = GetLogFilePath();
  dispatch_async(readQueue_, ^{
    @autoreleasepool {
      struct stat info;
      const bool have_file = stat(log_path.c_str(), &info) == 0;
      const off_t size = have_file ? info.st_size : -1;
      const struct timespec mtime = have_file ? info.st_mtimespec : timespec{};
      NSString* display_text = nil;
      if (force || size != lastSize_ || mtime.tv_sec != lastMtime_.tv_sec ||
          mtime.tv_nsec != lastMtime_.tv_nsec) {
        lastSize_ = size;
        lastMtime_ = mtime;
        std::string content = ReadFileTail(log_path, kMaxLogBytes);
        if (content.empty()) {
          NSString* path_ns = ToNSString(xe::strip_pii(log_path.string()));
          display_text =
              [NSString stringWithFormat:NSLocalizedString(
                                             @"No recent log data found.\n\nExpected file:\n%@\n\n"
                                             @"Launch a game and keep this open. This view updates "
                                             @"automatically.",
                                             @"log view controller: label text"),
                                         path_ns];
        } else {
          // Older log lines can still carry container paths.
          display_text = DecodeLogBytesToNSString(xe::strip_pii(content));
        }
      }
      NSString* footer_path = ToNSString(xe::strip_pii(log_path.string()));
      dispatch_async(dispatch_get_main_queue(), ^{
        reading_ = NO;
        [self applyLogText:display_text footerPath:footer_path];
        if (forcePending_) {
          forcePending_ = NO;
          [self reloadLogForced:YES];
        }
      });
    }
  });
}

- (void)applyLogText:(NSString*)display_text footerPath:(NSString*)footer_path {
  static constexpr size_t kMaxLogBytes = 256 * 1024;
  if (display_text) {
    BOOL should_follow_tail = [self isNearBottom];
    textView_.text = display_text;
    if (should_follow_tail) {
      [textView_ scrollRangeToVisible:NSMakeRange(textView_.text.length, 0)];
    }
  }
  if (!paused_) {
    footerLabel_.text = [NSString
        stringWithFormat:NSLocalizedString(@"Live tail (0.5s). Showing last %zu KB from %@",
                                           @"log view controller: section footer"),
                         kMaxLogBytes / 1024, footer_path];
  }
}

@end
