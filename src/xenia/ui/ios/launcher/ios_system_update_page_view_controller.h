/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_UPDATE_PAGE_VIEW_CONTROLLER_H_
#define XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_UPDATE_PAGE_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

// Microsoft's support page for the Xbox 360 system update, inside the app.
// The user follows Microsoft's own link to the USB update, which XeniOS only
// accepts from Microsoft's download server. The downloaded zip goes to
// |downloadHandler| on the main thread, in the temporary folder, for the
// receiver to install and delete.
@interface XeniaSystemUpdatePageViewController : UIViewController
- (instancetype)initWithDownloadHandler:(void (^)(NSURL* file))handler;
@end

#endif  // XENIA_UI_IOS_LAUNCHER_IOS_SYSTEM_UPDATE_PAGE_VIEW_CONTROLLER_H_
