/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_system_utils.h"

#include <TargetConditionals.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "xenia/base/logging.h"

extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);

#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0
#endif
#ifndef CS_DEBUGGED
#define CS_DEBUGGED 0x10000000
#endif
#ifndef CS_OPS_ENTITLEMENTS_BLOB
#define CS_OPS_ENTITLEMENTS_BLOB 7
#endif

namespace {

NSString* const kXeniaIOSIncreasedMemoryLimitEntitlement =
    @"com.apple.developer.kernel.increased-memory-limit";

constexpr uint32_t kCodeSigningEntitlementsMagic = 0xFADE7171u;
constexpr uint32_t kCodeSigningDerEntitlementsMagic = 0xFADE7172u;

uint32_t ReadBigEndianU32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

NSData* ExtractEntitlementsPlistData(const std::vector<uint8_t>& blob) {
  if (blob.size() < 8) {
    return nil;
  }

  const uint32_t magic = ReadBigEndianU32(blob.data());
  if (magic != kCodeSigningEntitlementsMagic && magic != kCodeSigningDerEntitlementsMagic) {
    return [NSData dataWithBytes:blob.data() length:blob.size()];
  }

  const uint32_t length = ReadBigEndianU32(blob.data() + 4);
  if (length <= 8 || length > blob.size()) {
    return nil;
  }

  return [NSData dataWithBytes:blob.data() + 8 length:length - 8];
}

BOOL EntitlementsBlobContainsKey(const std::vector<uint8_t>& blob, NSString* key) {
  if (!key.length || blob.empty()) {
    return NO;
  }
  NSData* plist_data = ExtractEntitlementsPlistData(blob);
  if (plist_data.length > 0) {
    NSError* error = nil;
    id plist = [NSPropertyListSerialization propertyListWithData:plist_data
                                                         options:0
                                                          format:nil
                                                           error:&error];
    if ([plist isKindOfClass:[NSDictionary class]]) {
      id value = [(NSDictionary*)plist objectForKey:key];
      if ([value respondsToSelector:@selector(boolValue)]) {
        return [value boolValue] ? YES : NO;
      }
      return value != nil ? YES : NO;
    }
  }

  const char* key_utf8 = [key UTF8String];
  if (!key_utf8 || !key_utf8[0]) {
    return NO;
  }
  std::string haystack(reinterpret_cast<const char*>(blob.data()), blob.size());
  return haystack.find(key_utf8) != std::string::npos ? YES : NO;
}

}  // namespace

BOOL xe_is_cs_debugged(void) {
  int flags = 0;
  return !csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) && (flags & CS_DEBUGGED);
}

BOOL xe_can_mmap_exec_page(void) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    page_size = 16384;
  }
  void* test =
      mmap(NULL, (size_t)page_size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (test == MAP_FAILED) {
    return NO;
  }
  munmap(test, (size_t)page_size);
  return YES;
}

int xe_ios_product_major_version(void) {
#if TARGET_OS_TV
  return -1;
#else
  size_t version_size = 0;
  if (sysctlbyname("kern.osproductversion", nullptr, &version_size, nullptr, 0) == 0 &&
      version_size > 0) {
    std::string version(version_size, '\0');
    if (sysctlbyname("kern.osproductversion", version.data(), &version_size, nullptr, 0) == 0 &&
        version_size > 0) {
      if (!version.empty() && version.back() == '\0') {
        version.pop_back();
      }
      int parsed_major = 0;
      size_t index = 0;
      while (index < version.size() && version[index] >= '0' && version[index] <= '9') {
        parsed_major = parsed_major * 10 + (version[index] - '0');
        ++index;
      }
      if (parsed_major > 0) {
        return parsed_major;
      }
    }
  }
  return -1;
#endif
}

BOOL xe_ios_requires_debugger_broker(void) {
  const int major = xe_ios_product_major_version();
  return major >= 26;
}

NSString* xe_jit_waiting_status_message(void) {
  return @"JIT is not active. In StikDebug, assign Amethyst-MeloNX.js or universal.js.";
}

NSString* xe_jit_not_detected_guidance_message(void) {
  return @"JIT is not active. In StikDebug, assign Amethyst-MeloNX.js or universal.js.";
}

void xe_add_jit_ring_pulse(CALayer* layer, NSString* key, CGFloat end_scale, CGFloat peak_opacity,
                           CFTimeInterval duration) {
  if ([layer animationForKey:key]) {
    return;
  }

  CAMediaTimingFunction* ease_out =
      [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];

  CAKeyframeAnimation* scale = [CAKeyframeAnimation animationWithKeyPath:@"transform.scale"];
  scale.values = @[ @1.0, @(1.0 + (end_scale - 1.0) * 0.45), @(end_scale) ];
  scale.keyTimes = @[ @0.0, @0.42, @1.0 ];
  scale.timingFunctions = @[ ease_out, ease_out ];
  scale.calculationMode = kCAAnimationCubic;

  CAKeyframeAnimation* fade = [CAKeyframeAnimation animationWithKeyPath:@"opacity"];
  fade.values = @[ @0.0, @(peak_opacity), @(peak_opacity * 0.55), @0.0 ];
  fade.keyTimes = @[ @0.0, @0.2, @0.58, @1.0 ];
  fade.timingFunctions = @[ ease_out, ease_out, ease_out ];
  fade.calculationMode = kCAAnimationLinear;

  CAAnimationGroup* group = [CAAnimationGroup animation];
  group.animations = @[ scale, fade ];
  group.duration = duration;
  group.repeatCount = HUGE_VALF;
  group.removedOnCompletion = NO;
  group.fillMode = kCAFillModeBoth;
  [layer addAnimation:group forKey:key];
}

// A plain RX mmap probe can succeed on iOS 18.5 while guest JIT execution is
// still unavailable. Treat debugger/JIT-enabled process state as part of the
// runtime readiness check so the launcher and automation don't produce false
// positives.
BOOL xe_check_jit_available(void) { return xe_is_cs_debugged() && xe_can_mmap_exec_page(); }

uint32_t xe_ios_code_sign_flags(void) {
  int flags = 0;
  return !csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) ? static_cast<uint32_t>(flags) : 0u;
}

BOOL xe_has_increased_memory_limit_entitlement(void) {
  std::vector<uint8_t> blob(256 * 1024);
  if (csops(getpid(), CS_OPS_ENTITLEMENTS_BLOB, blob.data(), blob.size()) != 0) {
    return NO;
  }
  return EntitlementsBlobContainsKey(blob, kXeniaIOSIncreasedMemoryLimitEntitlement);
}

NSString* xe_memory_entitlement_missing_status_message(void) {
  return @"Memory entitlement missing. Games will likely crash. Enable Get More RAM, then refresh "
         @"XeniOS.";
}

NSString* xe_memory_entitlement_not_detected_guidance_message(void) {
  return @"The increased-memory entitlement is not enabled in the installed app. Most games will "
         @"likely crash without it.\n\nEnable Get More RAM for XeniOS, then refresh or reinstall "
         @"the app so it is signed with increased memory.";
}

std::filesystem::path xe_get_ios_documents_path(void) {
  @autoreleasepool {
    NSArray* paths =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if (paths.count > 0) {
      return std::filesystem::path([paths[0] UTF8String]);
    }
    return std::filesystem::path([NSHomeDirectory() UTF8String]) / "Documents";
  }
}

void xe_request_orientation(UIViewController* view_controller, UIInterfaceOrientationMask mask,
                            UIInterfaceOrientation orientation) {
  if (!view_controller) {
    return;
  }
  (void)orientation;
#if !TARGET_OS_TV
  [view_controller setNeedsUpdateOfSupportedInterfaceOrientations];
  if (@available(iOS 16.0, *)) {
    UIWindowScene* scene = view_controller.view.window.windowScene;
    if (!scene) {
      for (UIScene* connected_scene in [UIApplication sharedApplication].connectedScenes) {
        if ([connected_scene isKindOfClass:[UIWindowScene class]]) {
          scene = (UIWindowScene*)connected_scene;
          break;
        }
      }
    }
    if (scene) {
      UIWindowSceneGeometryPreferencesIOS* preferences =
          [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
      [scene requestGeometryUpdateWithPreferences:preferences
                                     errorHandler:^(NSError* error) {
                                       XELOGW("iOS: Orientation geometry update failed: {}",
                                              [[error localizedDescription] UTF8String]);
                                     }];
    }
  }
  [UIViewController attemptRotationToDeviceOrientation];
#endif
}

void xe_request_landscape_orientation(UIViewController* view_controller) {
  xe_request_orientation(view_controller, UIInterfaceOrientationMaskLandscape,
                         UIInterfaceOrientationLandscapeRight);
}

UIInterfaceOrientation xe_interface_orientation_from_device_orientation(
    UIDeviceOrientation orientation) {
  switch (orientation) {
    case UIDeviceOrientationLandscapeLeft:
      return UIInterfaceOrientationLandscapeRight;
    case UIDeviceOrientationLandscapeRight:
      return UIInterfaceOrientationLandscapeLeft;
    case UIDeviceOrientationPortrait:
      return UIInterfaceOrientationPortrait;
    case UIDeviceOrientationPortraitUpsideDown:
      return UIInterfaceOrientationPortraitUpsideDown;
    default:
      return UIInterfaceOrientationUnknown;
  }
}

UIInterfaceOrientationMask xe_interface_orientation_mask(UIInterfaceOrientation orientation) {
  switch (orientation) {
    case UIInterfaceOrientationLandscapeLeft:
    case UIInterfaceOrientationLandscapeRight:
      return UIInterfaceOrientationMaskLandscape;
    case UIInterfaceOrientationPortraitUpsideDown:
      return UIInterfaceOrientationMaskPortraitUpsideDown;
    case UIInterfaceOrientationPortrait:
    default:
      return UIInterfaceOrientationMaskPortrait;
  }
}

UIInterfaceOrientation xe_current_interface_orientation(UIView* view) {
#if !TARGET_OS_TV
  UIWindowScene* scene = view.window.windowScene;
  if (!scene) {
    for (UIScene* connected_scene in [UIApplication sharedApplication].connectedScenes) {
      if ([connected_scene isKindOfClass:[UIWindowScene class]]) {
        scene = (UIWindowScene*)connected_scene;
        break;
      }
    }
  }
  if (scene && scene.interfaceOrientation != UIInterfaceOrientationUnknown) {
    return scene.interfaceOrientation;
  }
#endif
  UIInterfaceOrientation device_orientation =
      xe_interface_orientation_from_device_orientation([UIDevice currentDevice].orientation);
  if (device_orientation != UIInterfaceOrientationUnknown) {
    return device_orientation;
  }
  return UIInterfaceOrientationPortrait;
}

void xe_request_current_orientation(UIViewController* view_controller) {
  if (!view_controller) {
    return;
  }
  UIInterfaceOrientation orientation = UIInterfaceOrientationUnknown;
#if !TARGET_OS_TV
  UIWindowScene* scene = view_controller.view.window.windowScene;
  if (scene && scene.interfaceOrientation != UIInterfaceOrientationUnknown) {
    orientation = scene.interfaceOrientation;
  }
#endif
  if (orientation == UIInterfaceOrientationUnknown) {
    orientation =
        xe_interface_orientation_from_device_orientation([UIDevice currentDevice].orientation);
  }
  if (orientation == UIInterfaceOrientationUnknown) {
    [view_controller setNeedsUpdateOfSupportedInterfaceOrientations];
    [UIViewController attemptRotationToDeviceOrientation];
    return;
  }
  xe_request_orientation(view_controller, xe_interface_orientation_mask(orientation), orientation);
}

NSURL* xe_first_open_url_context_url(NSSet<UIOpenURLContext*>* url_contexts) {
  if (!url_contexts || url_contexts.count == 0) {
    return nil;
  }
  UIOpenURLContext* context = [url_contexts anyObject];
  return context.URL;
}

NSString* xe_device_machine(void) {
  size_t size = 0;
  sysctlbyname("hw.machine", nullptr, &size, nullptr, 0);
  if (size == 0) {
    return @"Unknown";
  }

  char* machine = static_cast<char*>(malloc(size));
  if (!machine) {
    return @"Unknown";
  }
  sysctlbyname("hw.machine", machine, &size, nullptr, 0);
  NSString* value = [NSString stringWithCString:machine encoding:NSUTF8StringEncoding];
  free(machine);
  return value ?: @"Unknown";
}

NSString* xe_device_display_name_for_machine(NSString* raw_machine) {
  if (![raw_machine isKindOfClass:[NSString class]]) {
    return @"Unknown";
  }
  NSString* machine = [raw_machine
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if (machine.length == 0) {
    return @"Unknown";
  }
  static NSDictionary<NSString*, NSString*>* overrides = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSMutableDictionary<NSString*, NSString*>* map = [NSMutableDictionary dictionary];
    void (^add_names)(NSArray<NSString*>*, NSString*) =
        ^(NSArray<NSString*>* codes, NSString* name) {
          for (NSString* code in codes) {
            map[code] = name;
          }
        };

    add_names(@[ @"iPhone13,1" ], @"iPhone 12 Mini");
    add_names(@[ @"iPhone13,2" ], @"iPhone 12");
    add_names(@[ @"iPhone13,3" ], @"iPhone 12 Pro");
    add_names(@[ @"iPhone13,4" ], @"iPhone 12 Pro Max");
    add_names(@[ @"iPhone14,2" ], @"iPhone 13 Pro");
    add_names(@[ @"iPhone14,3" ], @"iPhone 13 Pro Max");
    add_names(@[ @"iPhone14,4" ], @"iPhone 13 Mini");
    add_names(@[ @"iPhone14,5" ], @"iPhone 13");
    add_names(@[ @"iPhone14,6" ], @"iPhone SE 3rd Gen");
    add_names(@[ @"iPhone14,7" ], @"iPhone 14");
    add_names(@[ @"iPhone14,8" ], @"iPhone 14 Plus");
    add_names(@[ @"iPhone15,2" ], @"iPhone 14 Pro");
    add_names(@[ @"iPhone15,3" ], @"iPhone 14 Pro Max");
    add_names(@[ @"iPhone15,4" ], @"iPhone 15");
    add_names(@[ @"iPhone15,5" ], @"iPhone 15 Plus");
    add_names(@[ @"iPhone16,1" ], @"iPhone 15 Pro");
    add_names(@[ @"iPhone16,2" ], @"iPhone 15 Pro Max");
    add_names(@[ @"iPhone17,1" ], @"iPhone 16 Pro");
    add_names(@[ @"iPhone17,2" ], @"iPhone 16 Pro Max");
    add_names(@[ @"iPhone17,3" ], @"iPhone 16");
    add_names(@[ @"iPhone17,4" ], @"iPhone 16 Plus");
    add_names(@[ @"iPhone17,5" ], @"iPhone 16e");
    add_names(@[ @"iPhone18,1" ], @"iPhone 17 Pro");
    add_names(@[ @"iPhone18,2" ], @"iPhone 17 Pro Max");
    add_names(@[ @"iPhone18,3" ], @"iPhone 17");
    add_names(@[ @"iPhone18,4" ], @"iPhone Air");

    // Collapse WiFi and cellular iPad SKUs to a single product label.
    add_names(@[ @"iPad8,9", @"iPad8,10" ], @"iPad Pro 11 inch 4th Gen");
    add_names(@[ @"iPad8,11", @"iPad8,12" ], @"iPad Pro 12.9 inch 4th Gen");
    add_names(@[ @"iPad11,1", @"iPad11,2" ], @"iPad mini 5th Gen");
    add_names(@[ @"iPad11,3", @"iPad11,4" ], @"iPad Air 3rd Gen");
    add_names(@[ @"iPad11,6", @"iPad11,7" ], @"iPad 8th Gen");
    add_names(@[ @"iPad12,1", @"iPad12,2" ], @"iPad 9th Gen");
    add_names(@[ @"iPad13,1", @"iPad13,2" ], @"iPad Air 4th Gen");
    add_names(@[ @"iPad13,4", @"iPad13,5", @"iPad13,6", @"iPad13,7" ], @"iPad Pro 11 inch 5th Gen");
    add_names(@[ @"iPad13,8", @"iPad13,9", @"iPad13,10", @"iPad13,11" ],
              @"iPad Pro 12.9 inch 5th Gen");
    add_names(@[ @"iPad13,16", @"iPad13,17" ], @"iPad Air 5th Gen");
    add_names(@[ @"iPad13,18", @"iPad13,19" ], @"iPad 10th Gen");
    add_names(@[ @"iPad14,1", @"iPad14,2" ], @"iPad mini 6th Gen");
    add_names(@[ @"iPad14,3", @"iPad14,4" ], @"iPad Pro 11 inch 4th Gen");
    add_names(@[ @"iPad14,5", @"iPad14,6" ], @"iPad Pro 12.9 inch 6th Gen");
    add_names(@[ @"iPad14,8", @"iPad14,9" ], @"iPad Air 11 inch 6th Gen");
    add_names(@[ @"iPad14,10", @"iPad14,11" ], @"iPad Air 13 inch 6th Gen");
    add_names(@[ @"iPad15,3", @"iPad15,4" ], @"iPad Air 11-inch 7th Gen");
    add_names(@[ @"iPad15,5", @"iPad15,6" ], @"iPad Air 13-inch 7th Gen");
    add_names(@[ @"iPad15,7", @"iPad15,8" ], @"iPad 11th Gen");
    add_names(@[ @"iPad16,1", @"iPad16,2" ], @"iPad mini 7th Gen");
    add_names(@[ @"iPad16,3", @"iPad16,4" ], @"iPad Pro 11 inch 5th Gen");
    add_names(@[ @"iPad16,5", @"iPad16,6" ], @"iPad Pro 12.9 inch 7th Gen");

    overrides = [map copy];
  });

  NSString* display_name = overrides[machine];
  if ([display_name isKindOfClass:[NSString class]] && display_name.length > 0) {
    return display_name;
  }
  return machine;
}

NSString* xe_device_display_name(void) {
  return xe_device_display_name_for_machine(xe_device_machine());
}
