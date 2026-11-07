/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_LIVE_INVITE_BANNER_H_
#define XENIA_UI_IOS_SHARED_IOS_LIVE_INVITE_BANNER_H_

#import <UIKit/UIKit.h>

#import "xenia/ui/ios/shared/ios_live_social_bridge.h"

// "<Gamertag> invited you to <Game>", with Decline and Accept, dropped in from the top of the
// screen over the library or a game. It leaves by itself after a while; the invite stays in
// Friends until it expires.
@interface XeniaIOSLiveInviteBanner : UIView

// Shows |invite| in |host| (replacing a banner already there). |over_game| keeps the dark look
// over game video.
+ (void)showInvite:(XeniaLiveInvite*)invite
            inView:(UIView*)host
          overGame:(BOOL)over_game
            accept:(void (^)(XeniaLiveInvite* invite))accept
           decline:(void (^)(XeniaLiveInvite* invite))decline;
// Takes down a banner in |host|, if any.
+ (void)dismissInView:(UIView*)host;

@end

#endif  // XENIA_UI_IOS_SHARED_IOS_LIVE_INVITE_BANNER_H_
