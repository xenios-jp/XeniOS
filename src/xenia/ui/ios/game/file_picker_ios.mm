/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/file_picker.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "xenia/base/logging.h"

namespace {

NSMutableArray<NSURL*>* XeniaActiveFilePickerSecurityScopedURLs() {
  static NSMutableArray<NSURL*>* urls = [[NSMutableArray alloc] init];
  return urls;
}

void XeniaClearActiveFilePickerSecurityScopes() {
  NSMutableArray<NSURL*>* urls = XeniaActiveFilePickerSecurityScopedURLs();
  for (NSURL* url in urls) {
    [url stopAccessingSecurityScopedResource];
  }
  [urls removeAllObjects];
}

UIViewController* XeniaFilePickerTopPresenter() {
  UIApplication* application = [UIApplication sharedApplication];
  UIWindow* key_window = nil;
  for (UIScene* scene in application.connectedScenes) {
    if (![scene isKindOfClass:[UIWindowScene class]] ||
        scene.activationState != UISceneActivationStateForegroundActive) {
      continue;
    }
    UIWindowScene* window_scene = (UIWindowScene*)scene;
    for (UIWindow* window in window_scene.windows) {
      if (window.isKeyWindow) {
        key_window = window;
        break;
      }
    }
    if (key_window) {
      break;
    }
  }
  if (!key_window) {
    for (UIScene* scene in application.connectedScenes) {
      if (![scene isKindOfClass:[UIWindowScene class]]) {
        continue;
      }
      UIWindowScene* window_scene = (UIWindowScene*)scene;
      key_window = window_scene.windows.firstObject;
      if (key_window) {
        break;
      }
    }
  }

  UIViewController* presenter = key_window.rootViewController;
  while (presenter.presentedViewController) {
    presenter = presenter.presentedViewController;
  }
  if ([presenter isKindOfClass:[UINavigationController class]]) {
    presenter = ((UINavigationController*)presenter).visibleViewController;
  } else if ([presenter isKindOfClass:[UITabBarController class]]) {
    presenter = ((UITabBarController*)presenter).selectedViewController;
  }
  return presenter;
}

}  // namespace

// Delegate for handling document picker results.
@interface XeniaDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
// Called once, on the main thread, with the picked URLs, or nil if cancelled.
@property(nonatomic, copy) void (^completion)(NSArray<NSURL*>*);
@end

@implementation XeniaDocumentPickerDelegate

- (void)finishWithURLs:(NSArray<NSURL*>*)urls {
  void (^completion)(NSArray<NSURL*>*) = [[self.completion retain] autorelease];
  self.completion = nil;
  if (completion) {
    completion(urls);
  }
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  [self finishWithURLs:urls];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  [self finishWithURLs:nil];
}

- (void)dealloc {
  [_completion release];
  [super dealloc];
}

@end

namespace xe {
namespace ui {

class IOSFilePicker : public FilePicker {
 public:
  IOSFilePicker() = default;
  ~IOSFilePicker() override = default;

  bool Show(Window* parent_window) override;
};

std::unique_ptr<FilePicker> FilePicker::Create() { return std::make_unique<IOSFilePicker>(); }

bool IOSFilePicker::Show(Window* parent_window) {
  @autoreleasepool {
    // Build the list of content types from extensions.
    NSMutableArray<UTType*>* content_types = [NSMutableArray array];

    // Always include common game file types.
    [content_types addObject:[UTType typeWithFilenameExtension:@"iso"]];
    [content_types addObject:[UTType typeWithFilenameExtension:@"xex"]];
    [content_types addObject:[UTType typeWithFilenameExtension:@"zar"]];
    // STFS containers often have no extension, so include generic data.
    [content_types addObject:UTTypeData];

    // Add any extensions the caller specified.
    for (const auto& ext_pair : extensions_) {
      NSString* ext = [NSString stringWithUTF8String:ext_pair.second.c_str()];
      // Extensions may contain semicolons for multiple patterns.
      NSArray<NSString*>* parts = [ext componentsSeparatedByString:@";"];
      for (NSString* part in parts) {
        NSString* clean =
            [part stringByTrimmingCharactersInSet:[NSCharacterSet
                                                      characterSetWithCharactersInString:@"*. "]];
        if (clean.length > 0) {
          UTType* type = [UTType typeWithFilenameExtension:clean];
          if (type) {
            [content_types addObject:type];
          }
        }
      }
    }

    // Show() returns the choice, which can't be waited for on the main
    // thread without re-entering its run loop under the modal.
    if ([NSThread isMainThread]) {
      XELOGE("IOSFilePicker: Show() can't wait for the picker on the main thread");
      return false;
    }

    NSURL* directory_url = nil;
    if (!initial_directory_.empty()) {
      NSString* dir_path = [NSString stringWithUTF8String:initial_directory_.c_str()];
      directory_url = [NSURL fileURLWithPath:dir_path isDirectory:YES];
    }
    const BOOL multiple = multi_selection_ ? YES : NO;

    __block NSArray<NSURL*>* picked_urls = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
      UIViewController* presenting_vc = XeniaFilePickerTopPresenter();
      if (!presenting_vc) {
        XELOGE("IOSFilePicker: No presenting view controller available");
        dispatch_semaphore_signal(done);
        return;
      }
      UIDocumentPickerViewController* picker =
          [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:content_types];
      picker.allowsMultipleSelection = multiple;
      picker.directoryURL = directory_url;
      // The picker holds its delegate weakly; the completion keeps it alive
      // until the picker answers.
      XeniaDocumentPickerDelegate* delegate = [[XeniaDocumentPickerDelegate alloc] init];
      delegate.completion = ^(NSArray<NSURL*>* urls) {
        picked_urls = [urls copy];
        [delegate autorelease];
        dispatch_semaphore_signal(done);
      };
      picker.delegate = delegate;
      [presenting_vc presentViewController:picker animated:YES completion:nil];
      [picker release];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    dispatch_release(done);
    [picked_urls autorelease];
    if (!picked_urls.count) {
      return false;
    }

    XeniaClearActiveFilePickerSecurityScopes();
    selected_files_.clear();
    for (NSURL* url in picked_urls) {
      if (!url.fileURL) {
        continue;
      }
      if ([url startAccessingSecurityScopedResource]) {
        [XeniaActiveFilePickerSecurityScopedURLs() addObject:url];
      }
      std::string path_str = std::string([url.path UTF8String]);
      selected_files_.push_back(std::filesystem::path(path_str));
    }

    return !selected_files_.empty();
  }
}

}  // namespace ui
}  // namespace xe
