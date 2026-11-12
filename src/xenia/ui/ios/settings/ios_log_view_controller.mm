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

// Which lines the console lists.
enum class LogFilter : NSInteger { kAll, kWarnings, kErrors };
enum class ConsoleLevel { kDebug, kInfo, kWarning, kError };

NSString* const kShowLogDetailDefaultsKey = @"XeniaLogShowThreadAndFrame";
constexpr size_t kMaxLogBytes = 256 * 1024;

// The console keeps the theme's dark palette in either appearance: a log reads
// like a terminal, and light grey on white did not.
UIColor* ConsoleColor(UIColor* color) {
  static UITraitCollection* dark = [[UITraitCollection
      traitCollectionWithUserInterfaceStyle:UIUserInterfaceStyleDark] retain];
  return [color resolvedColorWithTraitCollection:dark];
}

struct LogCounts {
  NSUInteger warnings = 0;
  NSUInteger errors = 0;
};

// What the logger's subsystem prefixes stand for.
NSString* SubsystemName(unichar prefix) {
  switch (prefix) {
    case 'K':
      return @"kernel";
    case 'F':
      return @"files";
    case 'G':
      return @"gpu";
    case 'A':
      return @"audio";
    case 'C':
      return @"cpu";
    default:
      return nil;
  }
}

// The log as the console shows it. A line is "<level>> f:<frame> <thread>
// <message>"; one without that start continues the line before. Each line gets
// a mark in its level's colour, its frame and thread only when `detail` is set,
// and the name before the first colon ("GuestScheduler:") picked out.
NSAttributedString* BuildConsoleText(NSString* log, LogFilter filter, BOOL detail, UIFont* font,
                                     LogCounts* counts_out) {
  UIColor* text = ConsoleColor([XeniaTheme textPrimary]);
  UIColor* secondary = ConsoleColor([XeniaTheme textSecondary]);
  UIColor* muted = ConsoleColor([XeniaTheme textMuted]);
  UIColor* accent = ConsoleColor([XeniaTheme accent]);
  UIColor* warning = ConsoleColor([XeniaTheme statusWarning]);
  UIColor* error = ConsoleColor([XeniaTheme statusError]);
  UIFont* strong = [UIFont monospacedSystemFontOfSize:font.pointSize weight:UIFontWeightSemibold];
  NSString* const mark = @"• ";
  NSMutableParagraphStyle* paragraph = [[[NSMutableParagraphStyle alloc] init] autorelease];
  // Wrapped text lines up after the mark, and lines stand apart.
  paragraph.headIndent = ceil([mark sizeWithAttributes:@{NSFontAttributeName : font}].width);
  paragraph.paragraphSpacing = round(font.pointSize * 0.5);
  paragraph.lineBreakMode = NSLineBreakByWordWrapping;

  NSMutableAttributedString* out = [[[NSMutableAttributedString alloc] init] autorelease];
  __block ConsoleLevel level = ConsoleLevel::kInfo;
  __block LogCounts counts;
  auto append = [out](NSString* string, UIColor* color, UIFont* string_font) {
    [out appendAttributedString:[[[NSAttributedString alloc]
                                    initWithString:string
                                        attributes:@{
                                          NSForegroundColorAttributeName : color,
                                          NSFontAttributeName : string_font,
                                        }] autorelease]];
  };
  auto listed = [filter](ConsoleLevel line_level) {
    return filter == LogFilter::kAll ||
           (filter == LogFilter::kWarnings && line_level >= ConsoleLevel::kWarning) ||
           line_level == ConsoleLevel::kError;
  };
  [log enumerateSubstringsInRange:NSMakeRange(0, log.length)
                          options:NSStringEnumerationByLines
                       usingBlock:^(NSString* line, __unused NSRange range,
                                    __unused NSRange enclosing, __unused BOOL* stop) {
                         if (!line.length) {
                           return;
                         }
                         const bool starts_entry = line.length >= 3 &&
                                                   [line characterAtIndex:1] == '>' &&
                                                   [line characterAtIndex:2] == ' ';
                         if (!starts_entry) {
                           // More of the entry above, in its colour.
                           if (!listed(level)) {
                             return;
                           }
                           if (out.length) {
                             append(@"\n", secondary, font);
                           }
                           append(@"  ", secondary, font);
                           append(line,
                                  level == ConsoleLevel::kError     ? error
                                  : level == ConsoleLevel::kWarning ? warning
                                                                : secondary,
                                  font);
                           return;
                         }
                         const unichar prefix = [line characterAtIndex:0];
                         level = prefix == '!'   ? ConsoleLevel::kError
                                 : prefix == 'w' ? ConsoleLevel::kWarning
                                 : prefix == 'd' ? ConsoleLevel::kDebug
                                                 : ConsoleLevel::kInfo;
                         counts.warnings += level == ConsoleLevel::kWarning;
                         counts.errors += level == ConsoleLevel::kError;
                         if (!listed(level)) {
                           return;
                         }
                         NSString* message = [line substringFromIndex:3];
                         NSString* fields = nil;
                         // "f:0002652 0001600F ": the frame and the thread.
                         if ([message hasPrefix:@"f:"]) {
                           const NSRange frame_end = [message rangeOfString:@" "];
                           if (frame_end.location != NSNotFound &&
                               message.length > frame_end.location + 9 &&
                               [message characterAtIndex:frame_end.location + 9] == ' ') {
                             fields = [message substringToIndex:frame_end.location + 10];
                             message = [message substringFromIndex:frame_end.location + 10];
                           }
                         }
                         UIColor* body = level == ConsoleLevel::kError     ? error
                                         : level == ConsoleLevel::kWarning ? warning
                                         : level == ConsoleLevel::kDebug   ? secondary
                                                                       : text;
                         if (out.length) {
                           append(@"\n", body, font);
                         }
                         append(mark, level >= ConsoleLevel::kWarning ? body : muted, strong);
                         if (detail && fields) {
                           append(fields, muted, font);
                         }
                         if (NSString* subsystem = SubsystemName(prefix)) {
                           append([subsystem stringByAppendingString:@" "], muted, font);
                         }
                         // "GuestScheduler: ...": the part that says who is talking.
                         const NSRange colon = [message rangeOfString:@": "];
                         if (colon.location != NSNotFound && colon.location > 0 &&
                             colon.location <= 32 &&
                             [message rangeOfString:@" "
                                            options:0
                                              range:NSMakeRange(0, colon.location)]
                                     .location == NSNotFound) {
                           append([message substringToIndex:colon.location + 1],
                                  level >= ConsoleLevel::kWarning ? body : accent, strong);
                           message = [message substringFromIndex:colon.location + 1];
                         }
                         append(message, body, font);
                       }];
  [out addAttribute:NSParagraphStyleAttributeName
              value:paragraph
              range:NSMakeRange(0, out.length)];
  if (counts_out) {
    *counts_out = counts;
  }
  return out;
}

}  // namespace

@interface XeniaLogViewController () <UIScrollViewDelegate, UITextViewDelegate>
@end

@implementation XeniaLogViewController {
  UIView* consoleView_;
  UITextView* textView_;
  UISegmentedControl* filterControl_;
  UIView* statusDot_;
  UILabel* footerLabel_;
  UIButton* latestButton_;
  NSTimer* autoRefreshTimer_;
  UIBarButtonItem* pauseButton_;
  UIBarButtonItem* moreButton_;
  BOOL paused_;
  LogFilter filter_;
  BOOL showDetail_;
  // The tail as it is in the file, for Copy.
  NSString* rawText_;
  LogCounts counts_;
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
  showDetail_ = [[NSUserDefaults standardUserDefaults] boolForKey:kShowLogDetailDefaultsKey];
  self.view.backgroundColor = [XeniaTheme bgPrimary];

  pauseButton_ =
      [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"pause.fill"]
                                       style:UIBarButtonItemStylePlain
                                      target:self
                                      action:@selector(togglePauseTapped:)];
  [self updatePauseButton];
  moreButton_ = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"ellipsis.circle"]
                                                  menu:[self buildActionsMenu]];
  XESetSheetActions(self, @[ pauseButton_, moreButton_ ]);

  filterControl_ = [[UISegmentedControl alloc] initWithItems:@[
    NSLocalizedString(@"All", @"log view controller: filter"),
    NSLocalizedString(@"Warnings", @"log view controller: filter"),
    NSLocalizedString(@"Errors", @"log view controller: filter"),
  ]];
  filterControl_.translatesAutoresizingMaskIntoConstraints = NO;
  filterControl_.selectedSegmentIndex = 0;
  [filterControl_ addTarget:self
                     action:@selector(filterChanged:)
           forControlEvents:UIControlEventValueChanged];
  [self.view addSubview:filterControl_];

  consoleView_ = [[UIView alloc] init];
  consoleView_.translatesAutoresizingMaskIntoConstraints = NO;
  consoleView_.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
  consoleView_.backgroundColor = ConsoleColor([XeniaTheme bgPrimary]);
  consoleView_.layer.cornerRadius = 14.0;
  consoleView_.layer.cornerCurve = kCACornerCurveContinuous;
  consoleView_.layer.borderWidth = 1.0;
  consoleView_.layer.borderColor = ConsoleColor([XeniaTheme border]).CGColor;
  consoleView_.clipsToBounds = YES;
  [self.view addSubview:consoleView_];

  textView_ = [[UITextView alloc] init];
  textView_.translatesAutoresizingMaskIntoConstraints = NO;
  textView_.editable = NO;
  textView_.selectable = YES;
  textView_.alwaysBounceVertical = YES;
  textView_.backgroundColor = [UIColor clearColor];
  textView_.indicatorStyle = UIScrollViewIndicatorStyleWhite;
  textView_.textContainerInset = UIEdgeInsetsMake(12, 12, 12, 12);
  textView_.textContainer.lineFragmentPadding = 0.0;
  textView_.delegate = self;
  [consoleView_ addSubview:textView_];

  UIButtonConfiguration* latest = [UIButtonConfiguration filledButtonConfiguration];
  latest.title = NSLocalizedString(@"Latest", @"log view controller: jump to the end");
  latest.image = [UIImage systemImageNamed:@"arrow.down"];
  latest.imagePadding = 4.0;
  latest.buttonSize = UIButtonConfigurationSizeSmall;
  latest.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  latest.baseBackgroundColor = ConsoleColor([XeniaTheme accent]);
  latest.baseForegroundColor = ConsoleColor([XeniaTheme accentFg]);
  latestButton_ = [[UIButton buttonWithConfiguration:latest primaryAction:nil] retain];
  latestButton_.translatesAutoresizingMaskIntoConstraints = NO;
  latestButton_.hidden = YES;
  [latestButton_ addTarget:self
                    action:@selector(latestTapped:)
          forControlEvents:UIControlEventTouchUpInside];
  [consoleView_ addSubview:latestButton_];

  statusDot_ = [[UIView alloc] init];
  statusDot_.translatesAutoresizingMaskIntoConstraints = NO;
  statusDot_.layer.cornerRadius = 4.0;
  [self.view addSubview:statusDot_];

  footerLabel_ = [[UILabel alloc] init];
  footerLabel_.translatesAutoresizingMaskIntoConstraints = NO;
  footerLabel_.textColor = [XeniaTheme textSecondary];
  xe_apply_label_font(footerLabel_, UIFontTextStyleCaption1, 12.0, UIFontWeightRegular);
  footerLabel_.numberOfLines = 2;
  [self.view addSubview:footerLabel_];
  [self refreshFooter];

  UILayoutGuide* guide = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [filterControl_.topAnchor constraintEqualToAnchor:guide.topAnchor constant:8],
    [filterControl_.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor constant:16],
    [filterControl_.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor constant:-16],
    [consoleView_.topAnchor constraintEqualToAnchor:filterControl_.bottomAnchor constant:10],
    [consoleView_.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor constant:12],
    [consoleView_.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor constant:-12],
    [consoleView_.bottomAnchor constraintEqualToAnchor:footerLabel_.topAnchor constant:-8],
    [textView_.topAnchor constraintEqualToAnchor:consoleView_.topAnchor],
    [textView_.leadingAnchor constraintEqualToAnchor:consoleView_.leadingAnchor],
    [textView_.trailingAnchor constraintEqualToAnchor:consoleView_.trailingAnchor],
    [textView_.bottomAnchor constraintEqualToAnchor:consoleView_.bottomAnchor],
    [latestButton_.trailingAnchor constraintEqualToAnchor:consoleView_.trailingAnchor
                                                 constant:-12],
    [latestButton_.bottomAnchor constraintEqualToAnchor:consoleView_.bottomAnchor constant:-12],
    [statusDot_.widthAnchor constraintEqualToConstant:8],
    [statusDot_.heightAnchor constraintEqualToConstant:8],
    [statusDot_.leadingAnchor constraintEqualToAnchor:guide.leadingAnchor constant:18],
    [statusDot_.centerYAnchor constraintEqualToAnchor:footerLabel_.firstBaselineAnchor
                                             constant:-4],
    [footerLabel_.leadingAnchor constraintEqualToAnchor:statusDot_.trailingAnchor constant:8],
    [footerLabel_.trailingAnchor constraintEqualToAnchor:guide.trailingAnchor constant:-16],
    [footerLabel_.bottomAnchor constraintEqualToAnchor:guide.bottomAnchor constant:-8],
  ]];
}

// Small and fixed-pitch, growing with the text size setting only as far as a
// log stays readable in a narrow sheet.
- (UIFont*)consoleFont {
  UIFont* base = [UIFont monospacedSystemFontOfSize:11.0 weight:UIFontWeightRegular];
  return [[UIFontMetrics metricsForTextStyle:UIFontTextStyleFootnote] scaledFontForFont:base
                                                                       maximumPointSize:14.0];
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
  textView_.delegate = nil;
  [pauseButton_ release];
  [moreButton_ release];
  [filterControl_ release];
  [consoleView_ release];
  [textView_ release];
  [latestButton_ release];
  [statusDot_ release];
  [footerLabel_ release];
  [rawText_ release];
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
  UIAction* detail_action = [UIAction
      actionWithTitle:NSLocalizedString(@"Show Frame and Thread", @"log view controller: option")
                image:[UIImage systemImageNamed:@"number"]
           identifier:nil
              handler:^(__unused UIAction* action) {
                [weak_self toggleDetail];
              }];
  detail_action.state = showDetail_ ? UIMenuElementStateOn : UIMenuElementStateOff;
  UIMenu* options = [UIMenu menuWithTitle:@""
                                    image:nil
                               identifier:nil
                                  options:UIMenuOptionsDisplayInline
                                 children:@[ detail_action ]];
  return [UIMenu menuWithTitle:@""
                      children:@[ copy_action, share_action, refresh_action, options ]];
}

- (void)toggleDetail {
  showDetail_ = !showDetail_;
  [[NSUserDefaults standardUserDefaults] setBool:showDetail_ forKey:kShowLogDetailDefaultsKey];
  moreButton_.menu = [self buildActionsMenu];
  [self reloadLogForced:YES];
}

- (void)filterChanged:(UISegmentedControl*)sender {
  filter_ = LogFilter(sender.selectedSegmentIndex);
  [self reloadLogForced:YES];
}

- (void)togglePauseTapped:(id)sender {
  paused_ = !paused_;
  if (paused_) {
    [self stopAutoRefresh];
  } else {
    [self startAutoRefresh];
    [self reloadLogForced:YES];
  }
  [self updatePauseButton];
  [self refreshFooter];
}

- (void)updatePauseButton {
  pauseButton_.image = [UIImage systemImageNamed:(paused_ ? @"play.fill" : @"pause.fill")];
  pauseButton_.accessibilityLabel =
      paused_ ? NSLocalizedString(@"Resume live log", @"log view controller: accessibility label")
              : NSLocalizedString(@"Pause live log", @"log view controller: accessibility label");
}

// "Live" or "Paused", what the tail holds, and how much of the file it is.
- (void)refreshFooter {
  statusDot_.backgroundColor = paused_ ? [XeniaTheme textMuted] : [XeniaTheme accent];
  footerLabel_.text = [NSString
      stringWithFormat:NSLocalizedString(@"%1$@ · Warnings %2$lu · Errors %3$lu · last %4$zu KB "
                                         @"of xenia.log",
                                         @"log view controller: footer; %1$@ is Live or Paused"),
                       paused_ ? NSLocalizedString(@"Paused", @"log view controller: state")
                               : NSLocalizedString(@"Live", @"log view controller: state"),
                       (unsigned long)counts_.warnings, (unsigned long)counts_.errors,
                       kMaxLogBytes / 1024];
}

- (void)copyLog {
  [UIPasteboard generalPasteboard].string = rawText_.length ? rawText_ : @"";
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
  // Pushed, the page picks up the root's ✕ (XEApplySheetHeader).
  if ([self shouldShowCloseButton]) {
    XESetSheetClose(self, self, @selector(closeTapped:));
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

- (void)scrollToLatest {
  [textView_ scrollRangeToVisible:NSMakeRange(textView_.textStorage.length, 0)];
  latestButton_.hidden = YES;
}

- (void)latestTapped:(id)__unused sender {
  [self scrollToLatest];
}

// Scrolled up, the view stops following the tail: Latest takes it back.
- (void)scrollViewDidScroll:(__unused UIScrollView*)scrollView {
  latestButton_.hidden = [self isNearBottom];
}

- (void)shareLogTapped:(id)sender {
  // Shares a redacted copy (container paths and UUIDs removed).
  xe_ios_present_log_share_from_bar_button(
      self, self.navigationItem.rightBarButtonItems.firstObject, NO);
}

// Reads the tail and builds the console's text off the main thread, and only
// touches the text view when the file's size or modification time changed (or
// force is set).
- (void)reloadLogForced:(BOOL)force {
  if (reading_) {
    forcePending_ = forcePending_ || force;
    return;
  }
  reading_ = YES;
  const std::filesystem::path log_path = GetLogFilePath();
  const LogFilter filter = filter_;
  const BOOL detail = showDetail_;
  UIFont* font = [self consoleFont];
  dispatch_async(readQueue_, ^{
    @autoreleasepool {
      struct stat info;
      const bool have_file = stat(log_path.c_str(), &info) == 0;
      const off_t size = have_file ? info.st_size : -1;
      const struct timespec mtime = have_file ? info.st_mtimespec : timespec{};
      NSString* raw_text = nil;
      NSAttributedString* console_text = nil;
      LogCounts counts;
      if (force || size != lastSize_ || mtime.tv_sec != lastMtime_.tv_sec ||
          mtime.tv_nsec != lastMtime_.tv_nsec) {
        lastSize_ = size;
        lastMtime_ = mtime;
        std::string content = ReadFileTail(log_path, kMaxLogBytes);
        // Older log lines can still carry container paths.
        raw_text = content.empty() ? @"" : DecodeLogBytesToNSString(xe::strip_pii(content));
        NSString* placeholder = nil;
        if (!raw_text.length) {
          placeholder = NSLocalizedString(@"Nothing logged yet. Start a game and keep this "
                                          @"open: lines appear as they are written.",
                                          @"log view controller: empty");
        } else {
          console_text = BuildConsoleText(raw_text, filter, detail, font, &counts);
          if (!console_text.length) {
            placeholder =
                filter == LogFilter::kErrors
                    ? NSLocalizedString(@"No errors in the last part of the log.",
                                        @"log view controller: empty filter")
                    : NSLocalizedString(@"No warnings or errors in the last part of the log.",
                                        @"log view controller: empty filter");
          }
        }
        if (placeholder) {
          console_text = [[[NSAttributedString alloc]
              initWithString:placeholder
                  attributes:@{
                    NSForegroundColorAttributeName : ConsoleColor([XeniaTheme textSecondary]),
                    NSFontAttributeName : font,
                  }] autorelease];
        }
      }
      dispatch_async(dispatch_get_main_queue(), ^{
        reading_ = NO;
        if (console_text) {
          [rawText_ release];
          rawText_ = [raw_text copy];
          counts_ = counts;
          const BOOL follow_tail = [self isNearBottom];
          textView_.attributedText = console_text;
          if (follow_tail) {
            [self scrollToLatest];
          }
          [self refreshFooter];
        }
        if (forcePending_) {
          forcePending_ = NO;
          [self reloadLogForced:YES];
        }
      });
    }
  });
}

@end
