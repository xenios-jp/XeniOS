/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_NETWORK_STATUS_H_
#define XENIA_UI_IOS_SHARED_IOS_NETWORK_STATUS_H_

#import <Foundation/Foundation.h>

// Whether the device has a network at all (xe::net::NetworkPathAvailable), for the screens.
BOOL xe_ios_network_available(void);

// Posted on the main queue when that changes.
extern NSNotificationName const XeniaNetworkStatusDidChangeNotification;

// "No Internet Connection", and what to do about it. iOS doesn't say whether Airplane Mode is on,
// so the advice covers it and Wi-Fi together.
NSString* xe_ios_no_network_title(void);
NSString* xe_ios_no_network_message(void);

// For a failure: both of the above in a sentence with no network, otherwise |fallback|.
NSString* xe_ios_connection_failure_text(NSString* fallback);

#endif  // XENIA_UI_IOS_SHARED_IOS_NETWORK_STATUS_H_
