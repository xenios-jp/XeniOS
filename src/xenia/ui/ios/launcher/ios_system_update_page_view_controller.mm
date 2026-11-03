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

@interface XeniaSystemUpdatePageViewController () <WKNavigationDelegate,
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
  [self.view addSubview:web_view_];

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
  [self setBannerText:NSLocalizedString(
                          @"Under How to update, open “Copy to a USB flash drive” and "
                          @"tap its download link. XeniOS installs the update once it has "
                          @"downloaded.",
                          @"system update page: instructions")
               symbol:@"arrow.down.circle"
                color:[XeniaTheme accent]];
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

#pragma mark - Navigation

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
