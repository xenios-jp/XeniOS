/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/app/windowed_app_context_ios.h"

#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>

namespace xe {
namespace ui {

IOSWindowedAppContext::IOSWindowedAppContext() = default;

IOSWindowedAppContext::~IOSWindowedAppContext() = default;

bool IOSWindowedAppContext::PromptMessageBoxUI(const std::string& title, const std::string& text,
                                               const std::vector<std::string>& buttons,
                                               uint32_t default_button,
                                               uint32_t* selected_button_out) const {
  if (message_box_prompt_callback_) {
    return message_box_prompt_callback_(title, text, buttons, default_button, selected_button_out);
  }
  if (!view_controller_) {
    if (selected_button_out) {
      *selected_button_out = default_button;
    }
    return false;
  }

  __block uint32_t selected_button = default_button;
  __block BOOL shown = NO;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);

  NSString* title_ns =
      title.empty() ? @"Message Box" : [NSString stringWithUTF8String:title.c_str()];
  NSString* text_ns = text.empty() ? @"" : [NSString stringWithUTF8String:text.c_str()];

  NSMutableArray<NSString*>* prompt_buttons = [NSMutableArray arrayWithCapacity:buttons.size()];
  for (const auto& button : buttons) {
    NSString* button_ns = button.empty() ? @"OK" : [NSString stringWithUTF8String:button.c_str()];
    [prompt_buttons addObject:button_ns];
  }
  if (prompt_buttons.count == 0) {
    [prompt_buttons addObject:@"OK"];
  }
  if (selected_button >= prompt_buttons.count) {
    selected_button = static_cast<uint32_t>(prompt_buttons.count - 1);
  }

  UIViewController* base_view_controller = view_controller_;
  if ([NSThread isMainThread]) {
    dispatch_release(sem);
    UIViewController* presenter = base_view_controller;
    while (presenter.presentedViewController) {
      presenter = presenter.presentedViewController;
    }
    if (!presenter) {
      if (selected_button_out) {
        *selected_button_out = selected_button;
      }
      return false;
    }

    UIAlertController* alert =
        [UIAlertController alertControllerWithTitle:title_ns
                                            message:text_ns
                                     preferredStyle:UIAlertControllerStyleAlert];
    UIAlertAction* preferred_action = nil;
    for (NSUInteger i = 0; i < prompt_buttons.count; ++i) {
      UIAlertAction* action =
          [UIAlertAction actionWithTitle:prompt_buttons[i]
                                   style:UIAlertActionStyleDefault
                                 handler:nil];
      [alert addAction:action];
      if (i == selected_button) {
        preferred_action = action;
      }
    }
    if (preferred_action) {
      alert.preferredAction = preferred_action;
    }
    [presenter presentViewController:alert animated:YES completion:nil];
    if (selected_button_out) {
      *selected_button_out = selected_button;
    }
    return true;
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    UIViewController* presenter = base_view_controller;
    while (presenter.presentedViewController) {
      presenter = presenter.presentedViewController;
    }
    if (!presenter) {
      dispatch_semaphore_signal(sem);
      return;
    }

    UIAlertController* alert =
        [UIAlertController alertControllerWithTitle:title_ns
                                            message:text_ns
                                     preferredStyle:UIAlertControllerStyleAlert];
    UIAlertAction* preferred_action = nil;
    for (NSUInteger i = 0; i < prompt_buttons.count; ++i) {
      UIAlertAction* action =
          [UIAlertAction actionWithTitle:prompt_buttons[i]
                                   style:UIAlertActionStyleDefault
                                 handler:^(__unused UIAlertAction* picked_action) {
                                   selected_button = static_cast<uint32_t>(i);
                                   shown = YES;
                                   dispatch_semaphore_signal(sem);
                                 }];
      [alert addAction:action];
      if (i == selected_button) {
        preferred_action = action;
      }
    }
    if (preferred_action) {
      alert.preferredAction = preferred_action;
    }
    [presenter presentViewController:alert animated:YES completion:nil];
  });

  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  dispatch_release(sem);
  if (selected_button_out) {
    *selected_button_out = selected_button;
  }
  return shown ? true : false;
}

void IOSWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Use GCD to schedule pending function execution on the main thread.
  dispatch_async(dispatch_get_main_queue(), ^{
    ExecutePendingFunctionsFromUIThread();
  });
}

void IOSWindowedAppContext::NotifyGuestDisplayAspectRatioChanged() const {
  UIViewController* view_controller = view_controller_;
  if (!view_controller) {
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    [view_controller.view setNeedsLayout];
  });
}

void IOSWindowedAppContext::PlatformQuitFromUIThread() {
  // iOS apps don't self-terminate; the OS manages the lifecycle.
  // Suspending to background is the closest equivalent.
}

}  // namespace ui
}  // namespace xe
