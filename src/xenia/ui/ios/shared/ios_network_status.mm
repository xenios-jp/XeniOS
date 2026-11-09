/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_network_status.h"

#include "xenia/net/network_path.h"

NSNotificationName const XeniaNetworkStatusDidChangeNotification =
    @"XeniaNetworkStatusDidChangeNotification";

BOOL xe_ios_network_available(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    xe::net::AddNetworkPathListener([] {
      dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter]
            postNotificationName:XeniaNetworkStatusDidChangeNotification
                          object:nil];
      });
    });
  });
  return xe::net::NetworkPathAvailable();
}

void xe_ios_network_status_start(void) {
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    xe_ios_network_available();
  });
}

NSString* xe_ios_no_network_title(void) {
  return NSLocalizedString(@"No Internet Connection", @"No network: title");
}

NSString* xe_ios_no_network_message(void) {
  if (xe::net::GetNetworkPathState() == xe::net::NetworkPathState::kNotAllowed) {
    return NSLocalizedString(@"Settings doesn’t allow XeniOS to use this network. Allow it there, "
                             @"or connect to Wi-Fi.",
                             @"No network: the app isn't allowed to use cellular or Wi-Fi");
  }
  return NSLocalizedString(@"Turn off Airplane Mode or connect to Wi-Fi.", @"No network: advice");
}

NSString* xe_ios_connection_failure_text(NSString* fallback) {
  if (xe_ios_network_available()) {
    return fallback;
  }
  return [NSString stringWithFormat:@"%@. %@", xe_ios_no_network_title(),
                                    xe_ios_no_network_message()];
}
