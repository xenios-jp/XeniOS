/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/touch/touch_layout_editor_view_controller_ios.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <memory>
#include <string>

#import "xenia/hid/touch/touch_layout_ios.h"
#import "xenia/ui/ios/shared/ios_system_utils.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/touch/touch_controls_overlay_ios.h"

@implementation XeniaIOSTouchLayoutEditorViewController {
  uint32_t title_id_;
  NSString* game_title_;
  std::unique_ptr<xe::hid::touch::IOSTouchRuntimeModel> runtime_model_;
  std::string active_local_id_;
  XeniaIOSTouchLayoutUICoordinator* coordinator_;
  XeniaTouchControlsOverlayView* overlay_;
  BOOL saved_on_dismiss_;
}

- (instancetype)initWithTitleID:(uint32_t)titleID title:(NSString*)title {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    title_id_ = titleID;
    game_title_ = [title copy];
    runtime_model_ = std::make_unique<xe::hid::touch::IOSTouchRuntimeModel>();
    coordinator_ = [[XeniaIOSTouchLayoutUICoordinator alloc] initWithHost:self];
  }
  return self;
}

- (void)dealloc {
  [game_title_ release];
  [coordinator_ release];
  [overlay_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];

  self.title = @"Touch Layout";
  if (game_title_.length) {
    self.navigationItem.prompt = game_title_;
  }
  self.view.backgroundColor = [UIColor blackColor];
  self.navigationItem.leftBarButtonItem =
      [[[UIBarButtonItem alloc] initWithTitle:@"Close"
                                        style:UIBarButtonItemStylePlain
                                       target:self
                                       action:@selector(doneTapped:)] autorelease];
  self.navigationItem.rightBarButtonItem =
      [[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                     target:self
                                                     action:@selector(doneTapped:)] autorelease];

  overlay_ = [[XeniaTouchControlsOverlayView alloc] initWithRuntimeModel:runtime_model_.get()];
  overlay_.translatesAutoresizingMaskIntoConstraints = NO;
  overlay_.backgroundColor = [UIColor clearColor];
  [self.view addSubview:overlay_];
  [NSLayoutConstraint activateConstraints:@[
    [overlay_.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [overlay_.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [overlay_.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [overlay_.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
  ]];

  __block XeniaIOSTouchLayoutEditorViewController* block_self = self;
  overlay_.doneEditingHandler = ^{
    [block_self doneTapped:nil];
  };
  overlay_.layoutLibraryHandler = ^{
    [block_self->coordinator_ presentLibrary];
  };
  overlay_.layoutLibraryLoadHandler = ^(NSString* localID) {
    [block_self->coordinator_ applyLayoutWithLocalID:localID];
  };
  overlay_.layoutLibrarySaveCopyHandler = ^{
    [block_self->coordinator_ saveCurrentLayoutCopy];
  };
  overlay_.layoutLibraryRenameHandler = ^{
    [block_self->coordinator_ presentRenameSheet];
  };
  overlay_.layoutLibraryDeleteHandler = ^{
    [block_self->coordinator_ presentDeleteSheet];
  };
  overlay_.layoutLibraryImportHandler = ^{
    [block_self->coordinator_ importFromFile];
  };
  overlay_.layoutLibraryExportHandler = ^{
    [block_self->coordinator_ exportCurrentLayout];
  };
  overlay_.layoutLibraryResetHandler = ^{
    [block_self->coordinator_ resetToOfficialPreset];
  };
  overlay_.layoutLibraryRenameLayoutHandler = ^(NSString* localID) {
    [block_self->coordinator_ renameLayoutWithLocalID:localID];
  };
  overlay_.layoutLibraryDeleteLayoutHandler = ^(NSString* localID) {
    [block_self->coordinator_ deleteLayoutWithLocalID:localID];
  };
  overlay_.layoutLibraryExportLayoutHandler = ^(NSString* localID) {
    [block_self->coordinator_ exportLayoutWithLocalID:localID];
  };
  overlay_.layoutLibrarySetTitleDefaultHandler = ^(NSString* localID) {
    [block_self->coordinator_ setLayoutDefaultForCurrentTitleWithLocalID:localID];
  };
  overlay_.layoutLibrarySetGlobalDefaultHandler = ^(NSString* localID) {
    [block_self->coordinator_ setLayoutDefaultForAllGamesWithLocalID:localID];
  };
  overlay_.layoutLibraryFavoriteHandler = ^(NSString* localID, BOOL favorite) {
    [block_self->coordinator_ setLayoutFavoriteWithLocalID:localID favorite:favorite];
  };

  [coordinator_ applyLayoutModelForTitleID:title_id_];
  [overlay_ setGameplayOverlayVisible:YES animated:NO];
  [overlay_ setEditingControlsEnabled:YES animated:NO];
}

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  xe_request_landscape_orientation(self);
}

- (BOOL)prefersStatusBarHidden {
  return YES;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskLandscape;
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation {
  return UIInterfaceOrientationLandscapeRight;
}

- (BOOL)shouldAutorotate {
  return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
  return YES;
}

- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
  return UIRectEdgeAll;
}

- (void)viewWillDisappear:(BOOL)animated {
  [super viewWillDisappear:animated];
  UINavigationController* nav = self.navigationController;
  if (!saved_on_dismiss_ && (self.isBeingDismissed || nav.isBeingDismissed ||
                             self.isMovingFromParentViewController)) {
    [coordinator_ saveCurrentLayoutForTitleID:title_id_];
    saved_on_dismiss_ = YES;
  }
}

- (void)viewDidDisappear:(BOOL)animated {
  [super viewDidDisappear:animated];
  UINavigationController* nav = self.navigationController;
  if (self.isBeingDismissed || nav.isBeingDismissed) {
    UIViewController* presenter = nav.presentingViewController ?: self.presentingViewController;
    xe_request_current_orientation(presenter);
  }
}

- (void)doneTapped:(id)sender {
  (void)sender;
  [coordinator_ saveCurrentLayoutForTitleID:title_id_];
  saved_on_dismiss_ = YES;
  [self dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark - XeniaIOSTouchLayoutUICoordinatorHost

- (xe::hid::touch::IOSTouchRuntimeModel*)touchLayoutCoordinatorRuntimeModel {
  return runtime_model_.get();
}

- (uint32_t)touchLayoutCoordinatorActiveTitleID {
  return title_id_;
}

- (std::string)touchLayoutCoordinatorActiveLocalID {
  return active_local_id_;
}

- (void)touchLayoutCoordinatorSetActiveLocalID:(const std::string&)localID {
  active_local_id_ = localID;
}

- (BOOL)touchLayoutCoordinatorGameRunning {
  return NO;
}

- (BOOL)touchLayoutCoordinatorCanPresentPendingInstall {
  return self.view.window &&
         [UIApplication sharedApplication].applicationState == UIApplicationStateActive &&
         !self.presentedViewController;
}

- (UIViewController*)touchLayoutCoordinatorTopPresenter {
  UIViewController* presenter = self;
  while (presenter.presentedViewController) {
    presenter = presenter.presentedViewController;
  }
  return presenter;
}

- (void)touchLayoutCoordinatorSetGameplayModalPresentationPending:(BOOL)pending {
  (void)pending;
}

- (void)touchLayoutCoordinatorUpdateTouchOverlayVisibilityAnimated:(BOOL)animated {
  (void)animated;
  [overlay_ setGameplayOverlayVisible:YES animated:NO];
}

- (void)touchLayoutCoordinatorRefreshTouchOverlayLayoutModel {
  [overlay_ refreshLayoutModel];
}

- (BOOL)touchLayoutCoordinatorIsShowingLayoutLibrary {
  return overlay_ && overlay_.isShowingLayoutLibrary;
}

- (void)touchLayoutCoordinatorShowLayoutLibraryWithItems:
            (NSArray<XeniaTouchLayoutLibraryItem*>*)items
                                    currentLayoutLocalID:(NSString*)currentLayoutLocalID {
  [overlay_ showLayoutLibraryWithItems:items currentLayoutLocalID:currentLayoutLocalID];
}

- (void)touchLayoutCoordinatorSetStatusText:(NSString*)text {
  NSString* prompt = text.length ? text : game_title_;
  self.navigationItem.prompt = prompt;
  if (!text.length) {
    return;
  }
  NSString* captured = [text copy];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if ([self.navigationItem.prompt isEqualToString:captured]) {
                     self.navigationItem.prompt = game_title_;
                   }
                   [captured release];
                 });
}

- (void)touchLayoutCoordinatorPresentAlertWithTitle:(NSString*)title message:(NSString*)message {
  XEPresentOKAlert(self, title, message);
}

- (void)touchLayoutCoordinatorPresentKeyboardPromptWithTitle:(NSString*)title
                                                description:(NSString*)description
                                                defaultText:(NSString*)defaultText
                                                 completion:(void (^)(BOOL cancelled,
                                                                      NSString* text))completion {
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title.length ? title : @"Input Required"
                                          message:description
                                   preferredStyle:UIAlertControllerStyleAlert];
  [alert addTextFieldWithConfigurationHandler:^(UITextField* text_field) {
    text_field.text = defaultText ?: @"";
    text_field.autocapitalizationType = UITextAutocapitalizationTypeNone;
    text_field.autocorrectionType = UITextAutocorrectionTypeNo;
    text_field.clearButtonMode = UITextFieldViewModeWhileEditing;
  }];
  [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:^(__unused UIAlertAction* action) {
                                            if (completion) {
                                              completion(YES, @"");
                                            }
                                          }]];
  [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                            style:UIAlertActionStyleDefault
                                          handler:^(__unused UIAlertAction* action) {
                                            UITextField* text_field = alert.textFields.firstObject;
                                            if (completion) {
                                              completion(NO, text_field.text ?: @"");
                                            }
                                          }]];
  [[self touchLayoutCoordinatorTopPresenter] presentViewController:alert animated:YES completion:nil];
}

- (void)touchLayoutCoordinatorOpenTouchLayoutFileImportPicker {
  UIDocumentPickerViewController* picker =
      [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ]];
  picker.delegate = self;
  picker.allowsMultipleSelection = NO;
  picker.shouldShowFileExtensions = YES;
  [[self touchLayoutCoordinatorTopPresenter] presentViewController:picker animated:YES completion:nil];
  [picker release];
}

- (void)touchLayoutCoordinatorEvaluateAutomaticStikDebugJITHandoffIfNeeded {
}

#pragma mark - UIDocumentPickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  (void)controller;
  NSURL* url = urls.firstObject;
  if (!url) {
    return;
  }
  BOOL access_granted = [url startAccessingSecurityScopedResource];
  [coordinator_ importLayoutAtURL:url];
  if (access_granted) {
    [url stopAccessingSecurityScopedResource];
  }
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  (void)controller;
  [self touchLayoutCoordinatorSetStatusText:@"Touch layout import cancelled."];
}

@end
