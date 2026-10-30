/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_log_view_controller.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

#include "xenia/base/cvar.h"
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
  NSString* decoded = [[NSString alloc] initWithData:log_data encoding:NSUTF8StringEncoding];
  if (!decoded) {
    decoded = [[NSString alloc]
        initWithData:log_data
            encoding:CFStringConvertEncodingToNSStringEncoding(kCFStringEncodingWindowsLatin1)];
  }
  if (!decoded) {
    decoded = [[NSString alloc] initWithData:log_data encoding:NSISOLatin1StringEncoding];
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
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = @"Live Log";
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
  [self reloadLog];
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
  [super dealloc];
}

- (void)reloadLogTapped:(id)sender {
  [self reloadLog];
}

- (UIMenu*)buildActionsMenu {
  __unsafe_unretained XeniaLogViewController* weak_self = self;
  UIAction* copy_action =
      [UIAction actionWithTitle:@"Copy"
                          image:[UIImage systemImageNamed:@"doc.on.doc"]
                     identifier:nil
                        handler:^(__unused UIAction* action) { [weak_self copyLog]; }];
  UIAction* share_action =
      [UIAction actionWithTitle:@"Share"
                          image:[UIImage systemImageNamed:@"square.and.arrow.up"]
                     identifier:nil
                        handler:^(__unused UIAction* action) { [weak_self shareLogTapped:nil]; }];
  UIAction* refresh_action =
      [UIAction actionWithTitle:@"Refresh"
                          image:[UIImage systemImageNamed:@"arrow.clockwise"]
                     identifier:nil
                        handler:^(__unused UIAction* action) { [weak_self reloadLog]; }];
  return [UIMenu menuWithTitle:@"" children:@[ copy_action, share_action, refresh_action ]];
}

- (void)togglePauseTapped:(id)sender {
  paused_ = !paused_;
  if (paused_) {
    [self stopAutoRefresh];
    footerLabel_.text = @"Paused. Resume to continue the live tail.";
  } else {
    [self startAutoRefresh];
    [self reloadLog];
  }
  [self updatePauseButton];
}

- (void)updatePauseButton {
  pauseButton_.image = [UIImage systemImageNamed:(paused_ ? @"play.fill" : @"pause.fill")];
  pauseButton_.accessibilityLabel = paused_ ? @"Resume live log" : @"Pause live log";
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
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(closeTapped:)];
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
  [self reloadLog];
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
  std::filesystem::path log_path = GetLogFilePath();
  NSString* log_path_ns = ToNSString(log_path.string());
  if (![[NSFileManager defaultManager] fileExistsAtPath:log_path_ns]) {
    UIAlertController* alert =
        [UIAlertController alertControllerWithTitle:@"Log Not Found"
                                            message:@"No xenia.log file exists yet."
                                     preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                              style:UIAlertActionStyleDefault
                                            handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
    return;
  }

  NSURL* log_url = [NSURL fileURLWithPath:log_path_ns];
  UIActivityViewController* share_controller =
      [[UIActivityViewController alloc] initWithActivityItems:@[ log_url ]
                                        applicationActivities:nil];
  UIPopoverPresentationController* popover = share_controller.popoverPresentationController;
  if (popover) {
    popover.barButtonItem = self.navigationItem.rightBarButtonItems.lastObject;
  }
  [self presentViewController:share_controller animated:YES completion:nil];
}

- (void)reloadLog {
  static constexpr size_t kMaxLogBytes = 256 * 1024;
  BOOL should_follow_tail = [self isNearBottom];
  std::filesystem::path log_path = GetLogFilePath();
  std::string content = ReadFileTail(log_path, kMaxLogBytes);
  NSString* log_path_ns = ToNSString(log_path.string());

  NSString* display_text = nil;
  if (content.empty()) {
    display_text =
        [NSString stringWithFormat:@"No recent log data found.\n\nExpected file:\n%@\n\n"
                                   @"Launch a game and keep this open. This view updates "
                                   @"automatically.",
                                   log_path_ns];
  } else {
    display_text = DecodeLogBytesToNSString(content);
  }

  if (!display_text) {
    display_text = @"";
  }
  if (![textView_.text isEqualToString:display_text]) {
    textView_.text = display_text;
  }
  if (should_follow_tail) {
    [textView_ scrollRangeToVisible:NSMakeRange(textView_.text.length, 0)];
  }

  footerLabel_.text = [NSString stringWithFormat:@"Live tail (0.5s). Showing last %zu KB from %@",
                                                 kMaxLogBytes / 1024, log_path_ns];
}

@end
