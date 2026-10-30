/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SYSTEM_UTILS_H_
#define XENIA_UI_IOS_SYSTEM_UTILS_H_

#import <UIKit/UIKit.h>

#include <cstdint>
#include <filesystem>

// System / runtime utilities used across the iOS UI: code-signing &
// debugger-broker readiness, JIT availability, orientation control, app
// directories and device hardware identification.

// JIT / W^X readiness.
BOOL xe_is_cs_debugged(void);
BOOL xe_can_mmap_exec_page(void);
BOOL xe_check_jit_available(void);
uint32_t xe_ios_code_sign_flags(void);

// Runtime signing entitlement readiness.
BOOL xe_has_increased_memory_limit_entitlement(void);

// iOS major-version reporting (returns -1 if unavailable, e.g. tvOS).
int xe_ios_product_major_version(void);
BOOL xe_ios_requires_debugger_broker(void);

// User-visible JIT status copy.
NSString* xe_jit_waiting_status_message(void);
NSString* xe_jit_not_detected_guidance_message(void);
NSString* xe_memory_entitlement_missing_status_message(void);
NSString* xe_memory_entitlement_not_detected_guidance_message(void);

// Decorates a CALayer with a pulsing radial ring for the JIT status indicator.
void xe_add_jit_ring_pulse(CALayer* layer, NSString* key, CGFloat end_scale, CGFloat peak_opacity,
                           CFTimeInterval duration);

// File system locations.
std::filesystem::path xe_get_ios_documents_path(void);

// Orientation helpers.
void xe_request_orientation(UIViewController* view_controller, UIInterfaceOrientationMask mask,
                            UIInterfaceOrientation orientation);
void xe_request_landscape_orientation(UIViewController* view_controller);
void xe_request_current_orientation(UIViewController* view_controller);
UIInterfaceOrientation xe_interface_orientation_from_device_orientation(
    UIDeviceOrientation orientation);
UIInterfaceOrientationMask xe_interface_orientation_mask(UIInterfaceOrientation orientation);
UIInterfaceOrientation xe_current_interface_orientation(UIView* view);

// URL context helpers.
NSURL* xe_first_open_url_context_url(NSSet<UIOpenURLContext*>* url_contexts);

// Device hardware identification.
NSString* xe_device_machine(void);
NSString* xe_device_display_name_for_machine(NSString* raw_machine);
NSString* xe_device_display_name(void);

#endif  // XENIA_UI_IOS_SYSTEM_UTILS_H_
