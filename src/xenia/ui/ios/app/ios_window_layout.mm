/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/app/ios_window_layout.h"

#include <algorithm>

namespace {

NSString* const kXeniaIOSWindowScalingModeKey = @"XeniaWindowScalingMode";
NSString* const kXeniaIOSPortraitOffsetXKey = @"XeniaPortraitWindowOffsetX";
NSString* const kXeniaIOSPortraitOffsetYKey = @"XeniaPortraitWindowOffsetY";

CGFloat ClampNormalizedOffset(CGFloat value) {
  return std::clamp<CGFloat>(value, -1.0, 1.0);
}

}  // namespace

XeniaIOSWindowScalingMode XeniaIOSCurrentWindowScalingMode(void) {
  // NSUserDefaults integerForKey: returns 0 if unset, which maps to Fit.
  NSInteger raw =
      [[NSUserDefaults standardUserDefaults] integerForKey:kXeniaIOSWindowScalingModeKey];
  if (raw < XeniaIOSWindowScalingModeFit || raw > XeniaIOSWindowScalingModeZoom) {
    return XeniaIOSWindowScalingModeFit;
  }
  return static_cast<XeniaIOSWindowScalingMode>(raw);
}

void XeniaIOSSetCurrentWindowScalingMode(XeniaIOSWindowScalingMode mode) {
  [[NSUserDefaults standardUserDefaults] setInteger:mode forKey:kXeniaIOSWindowScalingModeKey];
}

CGPoint XeniaIOSCurrentPortraitWindowOffset(void) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  return CGPointMake(ClampNormalizedOffset(static_cast<CGFloat>(
                         [defaults doubleForKey:kXeniaIOSPortraitOffsetXKey])),
                     ClampNormalizedOffset(static_cast<CGFloat>(
                         [defaults doubleForKey:kXeniaIOSPortraitOffsetYKey])));
}

void XeniaIOSSetPortraitWindowOffset(CGPoint offset) {
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults setDouble:ClampNormalizedOffset(offset.x) forKey:kXeniaIOSPortraitOffsetXKey];
  [defaults setDouble:ClampNormalizedOffset(offset.y) forKey:kXeniaIOSPortraitOffsetYKey];
}

void XeniaIOSResetPortraitWindowOffset(void) {
  XeniaIOSSetPortraitWindowOffset(CGPointZero);
}

CGRect XeniaIOSMetalViewFrameForParent(CGRect parent, CGFloat guest_aspect,
                                       XeniaIOSWindowScalingMode mode,
                                       CGPoint portrait_offset) {
  if (CGRectIsEmpty(parent)) {
    return CGRectZero;
  }

  const CGFloat parent_aspect = parent.size.width / MAX(parent.size.height, 1.0);
  const BOOL is_portrait = parent.size.height >= parent.size.width;
  CGRect frame = parent;
  switch (mode) {
    case XeniaIOSWindowScalingModeStretch:
      frame = parent;
      break;
    case XeniaIOSWindowScalingModeZoom: {
      if (parent_aspect > guest_aspect) {
        frame.size.width = parent.size.width;
        frame.size.height = parent.size.width / guest_aspect;
      } else {
        frame.size.height = parent.size.height;
        frame.size.width = parent.size.height * guest_aspect;
      }
      frame.origin.x = parent.origin.x + (parent.size.width - frame.size.width) * 0.5;
      frame.origin.y = parent.origin.y + (parent.size.height - frame.size.height) * 0.5;
      break;
    }
    case XeniaIOSWindowScalingModeFit:
    default: {
      if (parent_aspect > guest_aspect) {
        frame.size.height = parent.size.height;
        frame.size.width = parent.size.height * guest_aspect;
      } else {
        frame.size.width = parent.size.width;
        frame.size.height = parent.size.width / guest_aspect;
      }
      frame.origin.x = parent.origin.x + (parent.size.width - frame.size.width) * 0.5;
      frame.origin.y = parent.origin.y + (parent.size.height - frame.size.height) * 0.5;
      break;
    }
  }

  if (is_portrait && mode == XeniaIOSWindowScalingModeFit) {
    const CGFloat slack_x = MAX(parent.size.width - frame.size.width, 0.0);
    const CGFloat slack_y = MAX(parent.size.height - frame.size.height, 0.0);
    frame.origin.x += slack_x * 0.5 * ClampNormalizedOffset(portrait_offset.x);
    frame.origin.y += slack_y * 0.5 * ClampNormalizedOffset(portrait_offset.y);
  }

  return frame;
}

CGPoint XeniaIOSPortraitWindowOffsetByApplyingDrag(CGPoint current_offset, CGSize slack,
                                                   CGPoint translation) {
  CGPoint next_offset = current_offset;
  if (slack.width > 0.0) {
    next_offset.x =
        ClampNormalizedOffset(next_offset.x + (translation.x * 2.0 / slack.width));
  }
  if (slack.height > 0.0) {
    next_offset.y =
        ClampNormalizedOffset(next_offset.y + (translation.y * 2.0 / slack.height));
  }
  return next_offset;
}
