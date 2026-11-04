/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_dashboard_setup_view_controller.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <memory>

#include "xenia/ui/ios/launcher/ios_dashboard.h"
#import "xenia/ui/ios/launcher/ios_system_update_page_view_controller.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

enum class SetupStage {
  kGetUpdate,
  kInstalling,
  kInstalled,
};

constexpr CGFloat kContentWidth = 440.0;

}  // namespace

@interface XeniaDashboardSetupViewController () <UIDocumentPickerDelegate>
@end

@implementation XeniaDashboardSetupViewController {
  void (^start_handler_)(void);
  SetupStage stage_;
  std::shared_ptr<std::atomic<bool>> cancel_;
  UIImageView* hero_view_;
  UILabel* title_label_;
  UILabel* body_label_;
  UIButton* primary_button_;
  UIButton* secondary_button_;
  UIProgressView* progress_view_;
  UILabel* footnote_label_;
}

- (instancetype)initWithStartHandler:(void (^)(void))start_handler {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    start_handler_ = [start_handler copy];
    stage_ = xe::ui::IsIOSDashboardInstalled() ? SetupStage::kInstalled : SetupStage::kGetUpdate;
    self.title = NSLocalizedString(@"Xbox 360 Dashboard", @"dashboard setup: title");
  }
  return self;
}

- (void)dealloc {
  if (cancel_) {
    cancel_->store(true);
  }
  [start_handler_ release];
  [hero_view_ release];
  [title_label_ release];
  [body_label_ release];
  [primary_button_ release];
  [secondary_button_ release];
  [progress_view_ release];
  [footnote_label_ release];
  [super dealloc];
}

- (UILabel*)newLabelWithStyle:(UIFontTextStyle)style color:(UIColor*)color {
  UILabel* label = [[UILabel alloc] init];
  label.numberOfLines = 0;
  label.textAlignment = NSTextAlignmentCenter;
  label.font = [UIFont preferredFontForTextStyle:style];
  label.adjustsFontForContentSizeCategory = YES;
  label.textColor = color;
  return label;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [XeniaTheme bgPrimary];

  hero_view_ = [[UIImageView alloc] init];
  hero_view_.contentMode = UIViewContentModeScaleAspectFit;
  hero_view_.tintColor = [XeniaTheme accent];
  hero_view_.preferredSymbolConfiguration =
      [UIImageSymbolConfiguration configurationWithPointSize:56.0 weight:UIImageSymbolWeightMedium];
  hero_view_.isAccessibilityElement = NO;

  title_label_ = [self newLabelWithStyle:UIFontTextStyleTitle2 color:[XeniaTheme textPrimary]];
  title_label_.font = [UIFontMetrics.defaultMetrics
      scaledFontForFont:[UIFont systemFontOfSize:24.0 weight:UIFontWeightBold]];
  title_label_.accessibilityTraits = UIAccessibilityTraitHeader;
  body_label_ = [self newLabelWithStyle:UIFontTextStyleBody color:[XeniaTheme textSecondary]];
  footnote_label_ = [self newLabelWithStyle:UIFontTextStyleFootnote color:[XeniaTheme textMuted]];

  UIButtonConfiguration* primary = [UIButtonConfiguration filledButtonConfiguration];
  // The screen's one primary button: white on dark, as in the Games and TV apps.
  primary.baseBackgroundColor = [XeniaTheme textPrimary];
  primary.baseForegroundColor = [XeniaTheme bgPrimary];
  primary.buttonSize = UIButtonConfigurationSizeLarge;
  primary.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
  primary.imagePadding = 8.0;
  primary_button_ = [[UIButton buttonWithConfiguration:primary primaryAction:nil] retain];
  [primary_button_ addTarget:self
                      action:@selector(primaryTapped:)
            forControlEvents:UIControlEventTouchUpInside];

  UIButtonConfiguration* secondary = [UIButtonConfiguration plainButtonConfiguration];
  secondary.baseForegroundColor = [XeniaTheme accent];
  secondary.buttonSize = UIButtonConfigurationSizeLarge;
  secondary_button_ = [[UIButton buttonWithConfiguration:secondary primaryAction:nil] retain];
  [secondary_button_ addTarget:self
                        action:@selector(secondaryTapped:)
              forControlEvents:UIControlEventTouchUpInside];

  progress_view_ = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
  progress_view_.progressTintColor = [XeniaTheme accent];

  UIStackView* stack = [[[UIStackView alloc] initWithArrangedSubviews:@[
    hero_view_, title_label_, body_label_, progress_view_, primary_button_, secondary_button_,
    footnote_label_
  ]] autorelease];
  stack.translatesAutoresizingMaskIntoConstraints = NO;
  stack.axis = UILayoutConstraintAxisVertical;
  stack.alignment = UIStackViewAlignmentFill;
  stack.spacing = 12.0;
  [stack setCustomSpacing:20.0 afterView:hero_view_];
  [stack setCustomSpacing:24.0 afterView:body_label_];
  [stack setCustomSpacing:24.0 afterView:progress_view_];

  UIScrollView* scroll = [[[UIScrollView alloc] init] autorelease];
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  scroll.alwaysBounceVertical = YES;
  [self.view addSubview:scroll];
  [scroll addSubview:stack];

  UILayoutGuide* content = scroll.contentLayoutGuide;
  UILayoutGuide* frame = scroll.frameLayoutGuide;
  NSLayoutConstraint* preferred_width = [stack.widthAnchor constraintEqualToConstant:kContentWidth];
  preferred_width.priority = UILayoutPriorityDefaultHigh;
  NSLayoutConstraint* centered = [stack.centerYAnchor constraintEqualToAnchor:frame.centerYAnchor];
  centered.priority = UILayoutPriorityDefaultLow;
  [NSLayoutConstraint activateConstraints:@[
    [scroll.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
    [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [stack.topAnchor constraintGreaterThanOrEqualToAnchor:content.topAnchor constant:24.0],
    [stack.bottomAnchor constraintLessThanOrEqualToAnchor:content.bottomAnchor constant:-24.0],
    [content.heightAnchor constraintGreaterThanOrEqualToAnchor:frame.heightAnchor],
    centered,
    [stack.centerXAnchor constraintEqualToAnchor:frame.centerXAnchor],
    [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:frame.leadingAnchor constant:24.0],
    [stack.trailingAnchor constraintLessThanOrEqualToAnchor:frame.trailingAnchor constant:-24.0],
    preferred_width,
  ]];

  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                               target:self
                                       action:@selector(closeTapped:)] autorelease];
  [self showStage];
}

#pragma mark - Stages

- (void)setButton:(UIButton*)button title:(NSString*)title symbol:(NSString*)symbol {
  UIButtonConfiguration* configuration = [[button.configuration copy] autorelease];
  configuration.title = title;
  configuration.image = symbol ? [UIImage systemImageNamed:symbol] : nil;
  button.configuration = configuration;
  button.hidden = title == nil;
}

- (void)showStage {
  const BOOL installing = stage_ == SetupStage::kInstalling;
  progress_view_.hidden = !installing;
  self.navigationItem.leftBarButtonItem.enabled = !installing;
  // Swiping the sheet away would leave an install running.
  self.navigationController.modalInPresentation = installing;
  switch (stage_) {
    case SetupStage::kGetUpdate:
      hero_view_.image = [UIImage systemImageNamed:@"square.grid.2x2.fill"];
      title_label_.text =
          NSLocalizedString(@"Set Up the Xbox 360 Dashboard", @"dashboard setup: title");
      body_label_.text = NSLocalizedString(
          @"The dashboard comes with Microsoft's free Xbox 360 system update. Download it from "
          @"Microsoft's support page, and XeniOS installs it.",
          @"dashboard setup: message");
      [self
          setButton:primary_button_
              title:NSLocalizedString(@"Get the Update from Microsoft", @"dashboard setup: button")
             symbol:@"arrow.down.circle.fill"];
      [self setButton:secondary_button_
                title:NSLocalizedString(@"Choose a Downloaded Update",
                                        @"dashboard setup: button, picks the update in Files")
               symbol:nil];
      footnote_label_.text =
          NSLocalizedString(@"The update is about 116 MB.", @"dashboard setup: footnote");
      break;
    case SetupStage::kInstalling:
      hero_view_.image = [UIImage systemImageNamed:@"arrow.down.circle.dotted"];
      title_label_.text =
          NSLocalizedString(@"Installing the System Software", @"dashboard setup: title");
      body_label_.text =
          NSLocalizedString(@"Keep XeniOS open until it finishes.", @"dashboard setup: message");
      [self setButton:primary_button_ title:nil symbol:nil];
      [self setButton:secondary_button_
                title:NSLocalizedString(@"Cancel", @"Alert button: cancel")
               symbol:nil];
      footnote_label_.text = nil;
      break;
    case SetupStage::kInstalled:
      hero_view_.image = [UIImage systemImageNamed:@"checkmark.circle.fill"];
      title_label_.text = NSLocalizedString(@"The Dashboard Is Ready", @"dashboard setup: title");
      body_label_.text = NSLocalizedString(@"Start it now, or later from its tile in the library.",
                                           @"dashboard setup: message");
      [self setButton:primary_button_
                title:NSLocalizedString(@"Start Dashboard", @"dashboard setup: button")
               symbol:@"play.fill"];
      [self setButton:secondary_button_ title:nil symbol:nil];
      footnote_label_.text = nil;
      break;
  }
  footnote_label_.hidden = footnote_label_.text.length == 0;
  UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, title_label_);
}

#pragma mark - Actions

- (void)closeTapped:(id)__unused sender {
  [self.navigationController.presentingViewController dismissViewControllerAnimated:YES
                                                                         completion:nil];
}

- (void)primaryTapped:(id)__unused sender {
  if (stage_ == SetupStage::kInstalled) {
    void (^start)(void) = [[start_handler_ retain] autorelease];
    [self.navigationController.presentingViewController dismissViewControllerAnimated:YES
                                                                           completion:^{
                                                                             if (start) {
                                                                               start();
                                                                             }
                                                                           }];
    return;
  }
  if (stage_ == SetupStage::kGetUpdate) {
    __unsafe_unretained XeniaDashboardSetupViewController* unsafe_self = self;
    XeniaSystemUpdatePageViewController* page =
        [[[XeniaSystemUpdatePageViewController alloc] initWithDownloadHandler:^(NSURL* file) {
          [unsafe_self.navigationController popToViewController:unsafe_self animated:YES];
          [unsafe_self installFromURL:file];
        }] autorelease];
    [self.navigationController pushViewController:page animated:YES];
  }
}

- (void)secondaryTapped:(id)__unused sender {
  if (stage_ == SetupStage::kInstalling) {
    if (cancel_) {
      cancel_->store(true);
    }
    return;
  }
  if (stage_ == SetupStage::kGetUpdate) {
    // A copy, so the install reads local files the picker's access doesn't
    // have to cover.
    UIDocumentPickerViewController* picker = [[[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[ UTTypeZIP, UTTypeFolder ]
                            asCopy:YES] autorelease];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
  }
}

- (void)documentPicker:(UIDocumentPickerViewController* __unused)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  if (NSURL* url = urls.firstObject) {
    [self installFromURL:url];
  }
}

#pragma mark - Install

- (void)setProgressPercent:(int)percent {
  progress_view_.progress = percent / 100.0f;
  footnote_label_.text = [NSString
      stringWithFormat:NSLocalizedString(@"%d%% done",
                                         @"system software view controller: install progress"),
                       percent];
  footnote_label_.hidden = NO;
}

- (void)installFromURL:(NSURL*)url {
  if (stage_ == SetupStage::kInstalling) {
    return;
  }
  stage_ = SetupStage::kInstalling;
  progress_view_.progress = 0.0f;
  [self showStage];
  [self setProgressPercent:0];

  auto cancel = std::make_shared<std::atomic<bool>>(false);
  cancel_ = cancel;
  [self retain];
  xe::ui::InstallIOSSystemSoftwareAsync(
      url, cancel,
      ^(int percent) {
        if (stage_ == SetupStage::kInstalling) {
          [self setProgressPercent:percent];
        }
      },
      ^(xe::ui::SystemFilesResult result) {
        [self finishInstallWithResult:result source:url];
        [self release];
      });
}

- (void)finishInstallWithResult:(xe::ui::SystemFilesResult)result source:(NSURL*)source {
  cancel_.reset();
  // The download and the picker's copy are only for the install.
  NSString* temporary = NSTemporaryDirectory().stringByStandardizingPath;
  if ([source.path.stringByStandardizingPath hasPrefix:temporary]) {
    [[NSFileManager defaultManager] removeItemAtURL:source error:nil];
  }
  [[NSNotificationCenter defaultCenter]
      postNotificationName:XeniaIOSSystemSoftwareDidChangeNotification
                    object:nil];
  const BOOL installed = result == xe::ui::SystemFilesResult::kSuccess;
  stage_ = installed ? SetupStage::kInstalled : SetupStage::kGetUpdate;
  [self showStage];
  if (NSString* message = xe::ui::IOSSystemSoftwareInstallFailureMessage(result)) {
    XEPresentOKAlert(self,
                     NSLocalizedString(@"Couldn't Install the System Software",
                                       @"system software view controller: title"),
                     message);
  }
}

@end
