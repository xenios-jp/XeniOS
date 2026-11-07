/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_PARTY_VOICE_H_
#define XENIA_UI_IOS_SHARED_IOS_PARTY_VOICE_H_

#import <UIKit/UIKit.h>

// Party voice: the party's members hear each other in the library, on the
// dashboard and in any game, while this device is in Party Chat (the default
// in a party with someone else). Game Chat gives the microphone back to the
// running game, as on the 360's party screen. The Microphone choice applies:
// Off means no party voice either.

// Posted on the main queue when party voice starts or stops, or a member
// connects, talks, mutes or goes quiet.
extern NSNotificationName const XeniaPartyVoiceDidChangeNotification;

typedef NS_ENUM(NSInteger, XeniaPartyVoiceMemberState) {
  // Not in party voice (this device isn't, or they're this profile).
  XeniaPartyVoiceMemberStateNone = 0,
  XeniaPartyVoiceMemberStateConnected,
  XeniaPartyVoiceMemberStateTalking,
  XeniaPartyVoiceMemberStateMuted,
  // In the party, but no direct path reaches them, so neither hears the other.
  XeniaPartyVoiceMemberStateUnreachable,
};

#ifdef __cplusplus
extern "C" {
#endif

// Once at launch, after the Microphone choice is set up.
void xe_ios_party_voice_start(void);

// In a party with someone else, online, with the Microphone choice on.
BOOL xe_ios_party_voice_available(void);
// Party Chat (YES) or Game Chat. Back to Party Chat in every new party.
BOOL xe_ios_party_voice_party_chat(void);
void xe_ios_party_voice_set_party_chat(BOOL party_chat);
// Mute Me: the party doesn't hear this device; it still hears them.
BOOL xe_ios_party_voice_self_muted(void);
void xe_ios_party_voice_set_self_muted(BOOL muted);
// Party voice runs now.
BOOL xe_ios_party_voice_active(void);
XeniaPartyVoiceMemberState xe_ios_party_voice_member_state(uint64_t xuid);
// "Ann is talking", "2 of 3 connected", ...; nil when party voice isn't on.
NSString* xe_ios_party_voice_status_text(void);
// The Microphone menu's Party section; nil when not in a party with others.
UIMenu* xe_ios_party_voice_new_menu_section(void);

#ifdef __cplusplus
}
#endif

#endif  // XENIA_UI_IOS_SHARED_IOS_PARTY_VOICE_H_
