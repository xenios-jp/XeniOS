/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_alert_actions.h"

#import <objc/runtime.h>

namespace {

char kActionHandlerKey;
char kCancelHandlerKey;
char kGuestPromptKey;
// What answers a guest prompt that is not an alert (a sheet) when the game ends.
char kGuestCancelKey;

using ActionHandler = void (^)(UIAlertAction*);

ActionHandler RecordedHandler(UIAlertAction* action) {
  return objc_getAssociatedObject(action, &kActionHandlerKey);
}

NSArray<UIAlertAction*>* EnabledActions(UIAlertController* alert) {
  NSMutableArray<UIAlertAction*>* enabled = [NSMutableArray array];
  for (UIAlertAction* action in alert.actions) {
    if (action.enabled) {
      [enabled addObject:action];
    }
  }
  return enabled;
}

}  // namespace

@interface XeniaTextFieldLengthLimiter : NSObject
@property(nonatomic) NSUInteger maxLength;
@end

@implementation XeniaTextFieldLengthLimiter
- (void)textChanged:(UITextField*)field {
  NSString* text = field.text;
  if (text.length <= self.maxLength) {
    return;
  }
  // At the start of the character the limit falls in, so no surrogate pair or
  // composed character is split.
  NSUInteger cut = [text rangeOfComposedCharacterSequenceAtIndex:self.maxLength].location;
  field.text = [text substringToIndex:cut];
}
@end

@implementation UIAlertAction (XeniaRecordedHandler)

+ (void)load {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    Method original = class_getClassMethod(self, @selector(actionWithTitle:style:handler:));
    Method recording =
        class_getClassMethod(self, @selector(xenia_recordingActionWithTitle:style:handler:));
    method_exchangeImplementations(original, recording);
  });
}

+ (instancetype)xenia_recordingActionWithTitle:(NSString*)title
                                         style:(UIAlertActionStyle)style
                                       handler:(void (^)(UIAlertAction*))handler {
  // Exchanged, so this calls UIKit's original.
  UIAlertAction* action = [self xenia_recordingActionWithTitle:title style:style handler:handler];
  if (handler) {
    objc_setAssociatedObject(action, &kActionHandlerKey, handler, OBJC_ASSOCIATION_COPY_NONATOMIC);
  }
  return action;
}

@end

void XeniaAlertSetCancelHandler(UIAlertController* alert, void (^handler)(void)) {
  objc_setAssociatedObject(alert, &kCancelHandlerKey, handler, OBJC_ASSOCIATION_COPY_NONATOMIC);
}

void XeniaAlertMarkGuestPrompt(UIAlertController* alert) {
  objc_setAssociatedObject(alert, &kGuestPromptKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

void XeniaAlertPerformAction(UIAlertController* alert, UIAlertAction* action) {
  ActionHandler handler = RecordedHandler(action);
  [alert dismissViewControllerAnimated:YES
                            completion:^{
                              if (handler) {
                                handler(action);
                              }
                            }];
}

void XeniaAlertAccept(UIAlertController* alert) {
  UIAlertAction* action = alert.preferredAction;
  if (!action || !action.enabled) {
    action = nil;
    for (UIAlertAction* candidate in EnabledActions(alert)) {
      if (candidate.style != UIAlertActionStyleCancel) {
        action = candidate;
        break;
      }
    }
  }
  if (action) {
    XeniaAlertPerformAction(alert, action);
  }
}

void XeniaAlertCancel(UIAlertController* alert) {
  void (^cancel)(void) = objc_getAssociatedObject(alert, &kCancelHandlerKey);
  if (cancel) {
    [alert dismissViewControllerAnimated:YES completion:cancel];
    return;
  }
  for (UIAlertAction* action in alert.actions) {
    if (action.style == UIAlertActionStyleCancel) {
      XeniaAlertPerformAction(alert, action);
      return;
    }
  }
  [alert dismissViewControllerAnimated:YES completion:nil];
}

void XeniaAlertMoveSelection(UIAlertController* alert, NSInteger delta) {
  NSArray<UIAlertAction*>* enabled = EnabledActions(alert);
  if (!enabled.count) {
    return;
  }
  NSUInteger current = alert.preferredAction ? [enabled indexOfObject:alert.preferredAction]
                                             : NSNotFound;
  NSInteger count = static_cast<NSInteger>(enabled.count);
  NSInteger next = current == NSNotFound ? (delta > 0 ? 0 : count - 1)
                                         : ((static_cast<NSInteger>(current) + delta) % count +
                                            count) % count;
  alert.preferredAction = enabled[next];
}

void XeniaLimitTextFieldLength(UITextField* field, NSUInteger max_length) {
  static char limiter_key;
  XeniaTextFieldLengthLimiter* limiter = [[XeniaTextFieldLengthLimiter alloc] init];
  limiter.maxLength = max_length;
  [field addTarget:limiter
                action:@selector(textChanged:)
      forControlEvents:UIControlEventEditingChanged];
  // The field holds its targets weakly.
  objc_setAssociatedObject(field, &limiter_key, limiter, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
#if !__has_feature(objc_arc)
  [limiter release];
#endif
  [limiter textChanged:field];
}

UIViewController* XeniaTopPresenter(UIViewController* root) {
  UIViewController* top = root;
  while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) {
    top = top.presentedViewController;
  }
  return top;
}

void XeniaPresentGuestPrompt(UIViewController* root, UIViewController* prompt,
                             void (^failed)(void)) {
  UIViewController* top = XeniaTopPresenter(root);
  if (!top || !top.view.window) {
    if (failed) {
      failed();
    }
    return;
  }
  if ([prompt isKindOfClass:[UIAlertController class]]) {
    XeniaAlertMarkGuestPrompt((UIAlertController*)prompt);
  } else {
    // A sheet has no cancel action to run: the failure answer is its cancel.
    objc_setAssociatedObject(prompt, &kGuestPromptKey, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(prompt, &kGuestCancelKey, failed, OBJC_ASSOCIATION_COPY_NONATOMIC);
  }
  __block BOOL shown = NO;
  [top presentViewController:prompt
                    animated:YES
                  completion:^{
                    shown = YES;
                  }];
  // UIKit refuses a presentation silently, without the completion: long after
  // the animation, a prompt that never appeared is answered as failed.
  [prompt retain];
  void (^failed_copy)(void) = [failed copy];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(1.5 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   if (!shown && !prompt.presentingViewController && failed_copy) {
                     failed_copy();
                   }
                   [failed_copy release];
                   [prompt release];
                 });
}

void XeniaCancelGuestAlerts(UIViewController* root) {
  UIViewController* presented = root.presentedViewController;
  while (presented) {
    UIViewController* next = presented.presentedViewController;
    if ([presented isKindOfClass:[UIAlertController class]] &&
        objc_getAssociatedObject(presented, &kGuestPromptKey)) {
      // Whatever it presented goes with it.
      XeniaAlertCancel((UIAlertController*)presented);
      return;
    }
    if (objc_getAssociatedObject(presented, &kGuestPromptKey)) {
      // A sheet: answer it now, so the waiting thread doesn't sit out the
      // dismissal, and only once.
      void (^cancel)(void) = objc_getAssociatedObject(presented, &kGuestCancelKey);
#if !__has_feature(objc_arc)
      cancel = [[cancel copy] autorelease];  // Outlives the association.
#endif
      objc_setAssociatedObject(presented, &kGuestCancelKey, nil, OBJC_ASSOCIATION_COPY_NONATOMIC);
      if (cancel) {
        cancel();
      }
      [presented dismissViewControllerAnimated:YES completion:nil];
      return;
    }
    presented = next;
  }
}
