/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/app/windowed_app_context_ios.h"

#include "xenia/base/logging.h"

#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>

#import "xenia/ui/ios/shared/ios_alert_actions.h"

namespace xe {
namespace ui {

IOSWindowedAppContext::IOSWindowedAppContext() = default;

IOSWindowedAppContext::~IOSWindowedAppContext() = default;

namespace {

bool IsCancelTitle(NSString* title) {
  for (NSString* cancel in @[ @"Cancel", @"No", @"Back" ]) {
    if ([title caseInsensitiveCompare:cancel] == NSOrderedSame) {
      return true;
    }
  }
  return false;
}

// A guest's message box. The first Cancel or No button gets the cancel style
// (UIKit allows one); the guest's default stays the preferred action.
UIAlertController* GuestMessageBox(NSString* title, NSString* text, NSArray<NSString*>* buttons,
                                   uint32_t default_button, void (^picked)(uint32_t)) {
  UIAlertController* alert =
      [UIAlertController alertControllerWithTitle:title
                                          message:text
                                   preferredStyle:UIAlertControllerStyleAlert];
  bool has_cancel = false;
  for (NSUInteger i = 0; i < buttons.count; ++i) {
    UIAlertActionStyle style = UIAlertActionStyleDefault;
    if (!has_cancel && buttons.count > 1 && IsCancelTitle(buttons[i])) {
      style = UIAlertActionStyleCancel;
      has_cancel = true;
    }
    UIAlertAction* action =
        [UIAlertAction actionWithTitle:buttons[i]
                                 style:style
                               handler:^(__unused UIAlertAction* picked_action) {
                                 if (picked) {
                                   picked(static_cast<uint32_t>(i));
                                 }
                               }];
    [alert addAction:action];
    if (i == default_button) {
      alert.preferredAction = action;
    }
  }
  return alert;
}

}  // namespace

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
    // The answer can't be waited for here. A notice with one button is still
    // worth showing, but a choice can't be made on the user's behalf.
    if (prompt_buttons.count > 1) {
      XELOGW("PromptMessageBoxUI: \"{}\" asks for a choice on the UI thread, not shown",
             title);
      if (selected_button_out) {
        *selected_button_out = selected_button;
      }
      return false;
    }
    UIAlertController* alert =
        GuestMessageBox(title_ns, text_ns, prompt_buttons, selected_button, nil);
    XeniaPresentGuestPrompt(base_view_controller, alert, nil);
    if (selected_button_out) {
      *selected_button_out = selected_button;
    }
    return true;
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    UIAlertController* alert =
        GuestMessageBox(title_ns, text_ns, prompt_buttons, selected_button, ^(uint32_t picked) {
          selected_button = picked;
          shown = YES;
          dispatch_semaphore_signal(sem);
        });
    // B on a controller, or leaving the game, answers it as cancelled.
    XeniaAlertSetCancelHandler(alert, ^{
      dispatch_semaphore_signal(sem);
    });
    XeniaPresentGuestPrompt(base_view_controller, alert, ^{
      dispatch_semaphore_signal(sem);
    });
  });

  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  dispatch_release(sem);
  if (selected_button_out) {
    *selected_button_out = selected_button;
  }
  return shown ? true : false;
}

void IOSWindowedAppContext::CancelGuestPrompts() const {
  UIViewController* view_controller = view_controller_;
  if (!view_controller) {
    return;
  }
  if ([NSThread isMainThread]) {
    XeniaCancelGuestAlerts(view_controller);
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    XeniaCancelGuestAlerts(view_controller);
  });
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
