/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_system_update_page_view_controller.h"

#import <WebKit/WebKit.h>

#include "xenia/ui/ios/launcher/ios_dashboard.h"
#import "xenia/ui/ios/shared/ios_theme.h"

namespace {

// Microsoft's support pages and the links in them stay in the app; anything
// else opens in Safari.
BOOL IsMicrosoftHost(NSString* host) {
  host = host.lowercaseString;
  for (NSString* domain in @[ @"xbox.com", @"microsoft.com" ]) {
    if ([host isEqualToString:domain] || [host hasSuffix:[@"." stringByAppendingString:domain]]) {
      return YES;
    }
  }
  return NO;
}

// The only place an update is taken from: Microsoft's download server.
BOOL IsMicrosoftDownload(NSURL* url) {
  return [url.scheme.lowercaseString isEqualToString:@"https"] &&
         [url.host.lowercaseString isEqualToString:@"download.microsoft.com"];
}

}  // namespace

@interface XeniaSystemUpdatePageViewController () <UIGestureRecognizerDelegate,
                                                   WKNavigationDelegate,
                                                   WKUIDelegate,
                                                   WKDownloadDelegate>
@end

@implementation XeniaSystemUpdatePageViewController {
  void (^download_handler_)(NSURL* file);
  WKWebView* web_view_;
  UILabel* banner_label_;
  UIImageView* banner_icon_;
  UIProgressView* progress_view_;
  WKDownload* download_;
  NSURL* download_file_;
  NSTimer* progress_timer_;
  NSUInteger find_generation_;  // Drops finds a newer load or tap has replaced.
  UIView* guide_ring_;           // Rings what Find in Page found, above the page.
  UILabel* guide_label_;
}

- (instancetype)initWithDownloadHandler:(void (^)(NSURL* file))handler {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    download_handler_ = [handler copy];
    self.title = NSLocalizedString(@"System Update", @"system update page: title");
  }
  return self;
}

- (void)dealloc {
  [progress_timer_ invalidate];
  if (download_) {
    download_.delegate = nil;
    [download_ cancel:nil];
  }
  [self removeDownloadFile];
  web_view_.navigationDelegate = nil;
  web_view_.UIDelegate = nil;
  [web_view_ stopLoading];
  [web_view_ release];
  [banner_label_ release];
  [banner_icon_ release];
  [guide_ring_ release];
  [guide_label_ release];
  [progress_view_ release];
  [download_ release];
  [download_file_ release];
  [download_handler_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [XeniaTheme bgPrimary];

  UIView* banner = [[[UIView alloc] init] autorelease];
  banner.translatesAutoresizingMaskIntoConstraints = NO;
  banner.backgroundColor = [XeniaTheme bgSurface];
  [self.view addSubview:banner];

  banner_icon_ = [[UIImageView alloc] init];
  banner_icon_.translatesAutoresizingMaskIntoConstraints = NO;
  banner_icon_.tintColor = [XeniaTheme accent];
  banner_icon_.contentMode = UIViewContentModeScaleAspectFit;
  [banner_icon_ setContentHuggingPriority:UILayoutPriorityRequired
                                  forAxis:UILayoutConstraintAxisHorizontal];
  banner_label_ = [[UILabel alloc] init];
  banner_label_.translatesAutoresizingMaskIntoConstraints = NO;
  banner_label_.numberOfLines = 0;
  banner_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
  banner_label_.adjustsFontForContentSizeCategory = YES;
  banner_label_.textColor = [XeniaTheme textPrimary];
  UIStackView* banner_stack =
      [[[UIStackView alloc] initWithArrangedSubviews:@[ banner_icon_, banner_label_ ]] autorelease];
  banner_stack.translatesAutoresizingMaskIntoConstraints = NO;
  banner_stack.axis = UILayoutConstraintAxisHorizontal;
  banner_stack.alignment = UIStackViewAlignmentCenter;
  banner_stack.spacing = 10.0;
  [banner addSubview:banner_stack];

  progress_view_ = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
  progress_view_.translatesAutoresizingMaskIntoConstraints = NO;
  progress_view_.progressTintColor = [XeniaTheme accent];
  progress_view_.hidden = YES;
  [self.view addSubview:progress_view_];

  // Nothing the page sets is kept after it closes.
  WKWebViewConfiguration* configuration = [[[WKWebViewConfiguration alloc] init] autorelease];
  configuration.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
  web_view_ = [[WKWebView alloc] initWithFrame:CGRectZero configuration:configuration];
  web_view_.translatesAutoresizingMaskIntoConstraints = NO;
  web_view_.navigationDelegate = self;
  web_view_.UIDelegate = self;
  web_view_.backgroundColor = [XeniaTheme bgPrimary];
  web_view_.findInteractionEnabled = YES;
  [self.view addSubview:web_view_];

  // A tap that opens a section of the page moves the highlight on to what it revealed.
  UITapGestureRecognizer* tap =
      [[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(pageTapped:)]
          autorelease];
  tap.cancelsTouchesInView = NO;
  tap.delegate = self;
  [web_view_ addGestureRecognizer:tap];

  // Drawn by XeniOS in the web view's scroll view, over the page and scrolling with it.
  guide_ring_ = [[UIView alloc] init];
  guide_ring_.userInteractionEnabled = NO;
  guide_ring_.hidden = YES;
  guide_ring_.layer.borderColor = [XeniaTheme accent].CGColor;
  guide_ring_.layer.borderWidth = 3.0;
  guide_ring_.layer.cornerRadius = 10.0;
  guide_ring_.backgroundColor = [[XeniaTheme accent] colorWithAlphaComponent:0.08];
  [web_view_.scrollView addSubview:guide_ring_];
  guide_label_ = [[UILabel alloc] init];
  guide_label_.userInteractionEnabled = NO;
  guide_label_.hidden = YES;
  guide_label_.text = NSLocalizedString(@"Tap here", @"system update page: label beside the "
                                                     @"part of Microsoft's page to tap next");
  guide_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
  guide_label_.font = [UIFont systemFontOfSize:guide_label_.font.pointSize weight:UIFontWeightSemibold];
  guide_label_.textColor = UIColor.whiteColor;
  guide_label_.textAlignment = NSTextAlignmentCenter;
  guide_label_.backgroundColor = [XeniaTheme accent];
  guide_label_.layer.cornerRadius = 10.0;
  guide_label_.layer.masksToBounds = YES;
  [web_view_.scrollView addSubview:guide_label_];

  UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [banner.topAnchor constraintEqualToAnchor:safe.topAnchor],
    [banner.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [banner.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [banner_stack.topAnchor constraintEqualToAnchor:banner.topAnchor constant:10.0],
    [banner_stack.bottomAnchor constraintEqualToAnchor:banner.bottomAnchor constant:-10.0],
    [banner_stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16.0],
    [banner_stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16.0],
    [banner_icon_.widthAnchor constraintEqualToConstant:22.0],
    [progress_view_.topAnchor constraintEqualToAnchor:banner.bottomAnchor],
    [progress_view_.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [progress_view_.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [web_view_.topAnchor constraintEqualToAnchor:banner.bottomAnchor],
    [web_view_.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [web_view_.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [web_view_.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
  ]];
  [self.view bringSubviewToFront:progress_view_];

  [self showInstructions];
  [self updateBarButtons];
  [self loadSupportPage];
}

- (void)loadSupportPage {
  [web_view_ loadRequest:[NSURLRequest
                             requestWithURL:[NSURL URLWithString:XeniaIOSSystemUpdateSupportURL]]];
}

#pragma mark - Banner

- (void)setBannerText:(NSString*)text symbol:(NSString*)symbol color:(UIColor*)color {
  banner_label_.text = text;
  banner_icon_.image = [UIImage systemImageNamed:symbol];
  banner_icon_.tintColor = color;
  UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, banner_label_);
}

- (void)showInstructions {
  // Until the page has drawn and its first step is found.
  [self setBannerText:NSLocalizedString(@"Microsoft's page is loading. XeniOS shows you each tap.",
                                        @"system update page: instructions while the page loads")
               symbol:@"arrow.down.circle"
                color:[XeniaTheme accent]];
}

// When none of the steps can be found on the page, as when Microsoft has changed it.
- (void)showWrittenSteps {
  [self setBannerText:NSLocalizedString(@"Under How to update, open “Copy to a USB flash drive” and "
                                        @"tap its download link. XeniOS installs the update when it "
                                        @"has downloaded.",
                                        @"system update page: instructions")
               symbol:@"arrow.down.circle"
                color:[XeniaTheme accent]];
}

- (void)showStep:(NSUInteger)step {
  NSString* text;
  switch (step) {
    case 3:
      text = NSLocalizedString(@"Step 3 of 3: Tap “XBOX 360 system update file”. XeniOS installs it "
                               @"when it has downloaded.",
                               @"system update page: last step");
      break;
    case 2:
      text = NSLocalizedString(@"Step 2 of 3: Open “Copy to a USB flash drive”.",
                               @"system update page: second step");
      break;
    default:
      text = NSLocalizedString(@"Step 1 of 3: Open “How to update”.",
                               @"system update page: first step");
      break;
  }
  [self setBannerText:text symbol:@"hand.tap" color:[XeniaTheme accent]];
}

- (void)updateBarButtons {
  if (download_) {
    self.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
        initWithTitle:NSLocalizedString(@"Stop Download",
                                        @"system update page: button, cancels the download")
                style:UIBarButtonItemStylePlain
               target:self
               action:@selector(stopDownloadTapped:)] autorelease];
    return;
  }
  UIBarButtonItem* reload =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                     target:self
                                                     action:@selector(reloadTapped:)] autorelease];
  reload.accessibilityLabel =
      NSLocalizedString(@"Reload", @"system update page: button, reloads Microsoft's page");
  self.navigationItem.rightBarButtonItem = reload;
}

- (void)reloadTapped:(id)__unused sender {
  if (web_view_.URL) {
    [web_view_ reload];
  } else {
    [self loadSupportPage];
  }
}

- (void)stopDownloadTapped:(id)__unused sender {
  [self endDownload];
  [self showInstructions];
}

#pragma mark - Find in page

// WebKit's own Find in Page, as in Safari's Find bar: it reads Microsoft's page as shown and
// selects what it finds, without running anything in the page or changing it. The download link
// is shown once its section is open, so until the user opens it the section is what's found.
- (void)findDownloadLinkAfter:(NSTimeInterval)delay attempts:(NSUInteger)attempts {
  const NSUInteger generation = ++find_generation_;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [self findDownloadLinkGeneration:generation attempts:attempts];
                 });
}

- (void)findDownloadLinkGeneration:(NSUInteger)generation attempts:(NSUInteger)attempts {
  if (generation != find_generation_ || download_ || !web_view_.window) {
    return;
  }
  [self findStep:3 text:0 generation:generation attempts:attempts];
}

// The text each step taps, last step first: the link, the section that holds it, then the
// heading that holds that. Several texts are tried for a step, in order.
static NSArray<NSString*>* StepTexts(NSUInteger step) {
  switch (step) {
    case 3:
      return @[ @"XBOX 360 system update file", @"system update file" ];
    case 2:
      return @[ @"Copy to a USB flash drive" ];
    default:
      return @[ @"How to update" ];
  }
}

- (void)findStep:(NSUInteger)step
            text:(NSUInteger)text
      generation:(NSUInteger)generation
        attempts:(NSUInteger)attempts {
  NSArray<NSString*>* texts = StepTexts(step);
  if (text >= texts.count) {
    if (step > 1) {
      [self findStep:step - 1 text:0 generation:generation attempts:attempts];
      return;
    }
    // The page draws itself after it has loaded, so look again for a while.
    if (attempts > 1) {
      [self findDownloadLinkAfter:1.0 attempts:attempts - 1];
    } else {
      [self hideGuide];
      [self showWrittenSteps];
    }
    return;
  }
  WKFindConfiguration* configuration = [[[WKFindConfiguration alloc] init] autorelease];
  [web_view_ findString:texts[text]
      withConfiguration:configuration
      completionHandler:^(WKFindResult* found) {
        if (generation != find_generation_) {
          return;
        }
        if (found.matchFound) {
          [self showGuideForStep:step generation:generation];
          return;
        }
        [self findStep:step text:text + 1 generation:generation attempts:attempts];
      }];
}

// Where Find in Page's selection is, in page coordinates. It runs in WebKit's client world, which
// the page's own scripts can't see; it reads the box and lets go of the selection, as a tap
// elsewhere would, so XeniOS's ring is the only mark on the page.
- (void)showGuideForStep:(NSUInteger)step generation:(NSUInteger)generation {
  [web_view_ callAsyncJavaScript:
                 @"const s = getSelection();"
                 @"if (!s || !s.rangeCount) return null;"
                 @"const r = s.getRangeAt(0).getBoundingClientRect();"
                 @"s.removeAllRanges();"
                 @"if (!r.width || !r.height) return null;"
                 @"return [r.left + scrollX, r.top + scrollY, r.width, r.height];"
                       arguments:nil
                         inFrame:nil
                  inContentWorld:WKContentWorld.defaultClientWorld
               completionHandler:^(id result, NSError* __unused error) {
                 if (generation != find_generation_ || download_) {
                   return;
                 }
                 NSArray* box = [result isKindOfClass:[NSArray class]] ? result : nil;
                 if (box.count != 4) {
                   [self hideGuide];
                   return;
                 }
                 const CGFloat zoom = web_view_.scrollView.zoomScale;
                 const CGRect found = CGRectMake([box[0] doubleValue] * zoom, [box[1] doubleValue] * zoom,
                                                 [box[2] doubleValue] * zoom, [box[3] doubleValue] * zoom);
                 [self placeGuideAround:found];
                 [self showStep:step];
               }];
}

- (void)placeGuideAround:(CGRect)found {
  guide_ring_.frame = CGRectInset(found, -10.0, -8.0);
  const CGSize text = [guide_label_ sizeThatFits:CGSizeMake(CGFLOAT_MAX, CGFLOAT_MAX)];
  const CGSize pill = CGSizeMake(ceil(text.width) + 20.0, ceil(text.height) + 6.0);
  guide_label_.layer.cornerRadius = pill.height / 2.0;
  CGFloat y = CGRectGetMinY(guide_ring_.frame) - pill.height - 6.0;
  if (y < web_view_.scrollView.contentOffset.y) {
    y = CGRectGetMaxY(guide_ring_.frame) + 6.0;
  }
  guide_label_.frame = CGRectMake(CGRectGetMinX(guide_ring_.frame), y, pill.width, pill.height);
  [web_view_.scrollView scrollRectToVisible:CGRectInset(CGRectUnion(guide_ring_.frame,
                                                                    guide_label_.frame),
                                                        0.0, -160.0)
                                   animated:YES];
  const BOOL was_hidden = guide_ring_.hidden;
  guide_ring_.hidden = NO;
  guide_label_.hidden = NO;
  [web_view_.scrollView bringSubviewToFront:guide_ring_];
  [web_view_.scrollView bringSubviewToFront:guide_label_];
  if (was_hidden) {
    guide_ring_.alpha = 0.0;
    guide_label_.alpha = 0.0;
    [UIView animateWithDuration:0.25
                     animations:^{
                       guide_ring_.alpha = 1.0;
                       guide_label_.alpha = 1.0;
                     }];
  }
  // A gentle pulse, unless the user has asked for less motion.
  [guide_ring_.layer removeAnimationForKey:@"pulse"];
  if (!UIAccessibilityIsReduceMotionEnabled()) {
    CABasicAnimation* pulse = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
    pulse.fromValue = @1.0;
    pulse.toValue = @1.04;
    pulse.duration = 0.8;
    pulse.autoreverses = YES;
    pulse.repeatCount = HUGE_VALF;
    pulse.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
    [guide_ring_.layer addAnimation:pulse forKey:@"pulse"];
  }
}

- (void)hideGuide {
  [guide_ring_.layer removeAnimationForKey:@"pulse"];
  guide_ring_.hidden = YES;
  guide_label_.hidden = YES;
}

- (void)pageTapped:(UITapGestureRecognizer*)__unused tap {
  [self hideGuide];
  [self findDownloadLinkAfter:0.8 attempts:3];
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer*)__unused gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer*)__unused other {
  return YES;
}

#pragma mark - Navigation

- (void)webView:(WKWebView* __unused)webView
    didStartProvisionalNavigation:(WKNavigation* __unused)navigation {
  ++find_generation_;
  [self hideGuide];
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)__unused navigation {
  if (IsMicrosoftHost(webView.URL.host)) {
    [self findDownloadLinkAfter:0.5 attempts:10];
  }
}

- (void)webView:(WKWebView* __unused)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decision_handler {
  if (action.shouldPerformDownload) {
    decision_handler(WKNavigationActionPolicyDownload);
    return;
  }
  NSURL* url = action.request.URL;
  const BOOL main_frame = !action.targetFrame || action.targetFrame.isMainFrame;
  const BOOL web = [url.scheme.lowercaseString isEqualToString:@"https"] ||
                   [url.scheme.lowercaseString isEqualToString:@"http"];
  if (main_frame && (!web || !IsMicrosoftHost(url.host))) {
    [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
    decision_handler(WKNavigationActionPolicyCancel);
    return;
  }
  decision_handler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView* __unused)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse*)response
                      decisionHandler:(void (^)(WKNavigationResponsePolicy))decision_handler {
  NSString* mime = response.response.MIMEType.lowercaseString;
  const BOOL zip = [mime isEqualToString:@"application/zip"] ||
                   [mime isEqualToString:@"application/x-zip-compressed"] ||
                   [mime isEqualToString:@"application/octet-stream"];
  if (response.forMainFrame && (zip || !response.canShowMIMEType)) {
    decision_handler(WKNavigationResponsePolicyDownload);
    return;
  }
  decision_handler(WKNavigationResponsePolicyAllow);
}

// Links that open a window of their own load here instead.
- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration* __unused)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures* __unused)features {
  if (!action.targetFrame) {
    [webView loadRequest:action.request];
  }
  return nil;
}

- (void)webView:(WKWebView* __unused)webView
    didFailProvisionalNavigation:(WKNavigation* __unused)navigation
                       withError:(NSError*)error {
  [self showPageError:error];
}

- (void)webView:(WKWebView* __unused)webView
    didFailNavigation:(WKNavigation* __unused)navigation
            withError:(NSError*)error {
  [self showPageError:error];
}

- (void)showPageError:(NSError*)error {
  // A navigation that became a download, or one the page replaced, isn't a
  // failure.
  // WebKit's 102 is a load that a download took over.
  if (download_ ||
      ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) ||
      ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102)) {
    return;
  }
  [self setBannerText:NSLocalizedString(@"Microsoft's page couldn't be loaded. Check the "
                                        @"connection, then reload.",
                                        @"system update page: error")
               symbol:@"wifi.exclamationmark"
                color:[XeniaTheme statusWarning]];
}

#pragma mark - Download

- (void)webView:(WKWebView* __unused)webView
     navigationAction:(WKNavigationAction* __unused)action
    didBecomeDownload:(WKDownload*)download {
  [self beginDownload:download];
}

- (void)webView:(WKWebView* __unused)webView
    navigationResponse:(WKNavigationResponse* __unused)response
     didBecomeDownload:(WKDownload*)download {
  [self beginDownload:download];
}

- (void)beginDownload:(WKDownload*)download {
  if (download_) {
    [download cancel:nil];
    return;
  }
  download_ = [download retain];
  download_.delegate = self;
  [self hideGuide];
  [self updateBarButtons];
}

- (void)download:(WKDownload*)download
    decideDestinationUsingResponse:(NSURLResponse*)response
                 suggestedFilename:(NSString*)suggested_filename
                 completionHandler:(void (^)(NSURL* destination))completion_handler {
  if (download != download_) {
    completion_handler(nil);
    return;
  }
  if (!IsMicrosoftDownload(response.URL) ||
      ![suggested_filename.pathExtension.lowercaseString isEqualToString:@"zip"]) {
    completion_handler(nil);
    [self endDownload];
    [self setBannerText:NSLocalizedString(@"That isn't the system update. Tap the download "
                                          @"link under “Copy to a USB flash drive”.",
                                          @"system update page: error")
                 symbol:@"exclamationmark.triangle"
                  color:[XeniaTheme statusWarning]];
    return;
  }
  NSString* name =
      [NSString stringWithFormat:@"XeniOS-SystemUpdate-%@.zip", [NSUUID UUID].UUIDString];
  NSURL* destination =
      [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name]];
  [download_file_ release];
  download_file_ = [destination retain];
  completion_handler(destination);

  progress_view_.progress = 0.0f;
  progress_view_.hidden = NO;
  [self updateDownloadProgress];
  __unsafe_unretained XeniaSystemUpdatePageViewController* unsafe_self = self;
  progress_timer_ = [NSTimer scheduledTimerWithTimeInterval:0.25
                                                    repeats:YES
                                                      block:^(NSTimer* __unused timer) {
                                                        [unsafe_self updateDownloadProgress];
                                                      }];
}

- (void)updateDownloadProgress {
  const double fraction = download_.progress.fractionCompleted;
  progress_view_.progress = static_cast<float>(fraction);
  [self setBannerText:[NSString stringWithFormat:NSLocalizedString(
                                                     @"Downloading the system update: %d%%",
                                                     @"system update page: download progress"),
                                                 static_cast<int>(fraction * 100.0)]
               symbol:@"arrow.down.circle.dotted"
                color:[XeniaTheme accent]];
}

- (void)downloadDidFinish:(WKDownload*)download {
  if (download != download_) {
    return;
  }
  NSURL* file = [[download_file_ retain] autorelease];
  [download_file_ release];
  download_file_ = nil;
  [self forgetDownload];
  [self endDownload];
  [self setBannerText:NSLocalizedString(@"Downloaded.", @"system update page: status")
               symbol:@"checkmark.circle"
                color:[XeniaTheme accent]];
  if (download_handler_) {
    download_handler_(file);
  } else {
    [[NSFileManager defaultManager] removeItemAtURL:file error:nil];
  }
}

- (void)download:(WKDownload*)download
    didFailWithError:(NSError*)error
          resumeData:(NSData* __unused)resume_data {
  if (download != download_) {
    return;
  }
  [self forgetDownload];
  [self endDownload];
  [self setBannerText:[NSString stringWithFormat:NSLocalizedString(
                                                     @"The download stopped: %@ Tap the link to "
                                                     @"try again.",
                                                     @"system update page: error; %@ is the "
                                                     @"reason"),
                                                 error.localizedDescription]
               symbol:@"exclamationmark.triangle"
                color:[XeniaTheme statusWarning]];
}

// Stops the download, if one is running, and leaves no partial file.
- (void)endDownload {
  [progress_timer_ invalidate];
  progress_timer_ = nil;
  progress_view_.hidden = YES;
  if (download_) {
    download_.delegate = nil;
    [download_ cancel:nil];
    [download_ release];
    download_ = nil;
  }
  [self removeDownloadFile];
  [self updateBarButtons];
}

// For a download that has ended by itself, so there's nothing to cancel.
- (void)forgetDownload {
  download_.delegate = nil;
  [download_ release];
  download_ = nil;
}

- (void)removeDownloadFile {
  if (download_file_) {
    [[NSFileManager defaultManager] removeItemAtURL:download_file_ error:nil];
    [download_file_ release];
    download_file_ = nil;
  }
}

@end
