/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_SHARED_IOS_MICROPHONE_H_
#define XENIA_UI_IOS_SHARED_IOS_MICROPHONE_H_

#import <UIKit/UIKit.h>

// The one Microphone choice for voice chat, in Settings and in game. It is the
// voice_chat and voice_microphone settings: Off turns voice_chat off, the other
// two turn it on and pick the microphone.
typedef NS_ENUM(NSInteger, XeniaMicrophoneChoice) {
  XeniaMicrophoneChoiceOff = 0,
  // The iPhone's or iPad's own microphone. AirPods stay high quality.
  XeniaMicrophoneChoiceDevice = 1,
  // A connected headset's microphone (Bluetooth hands-free, wired, USB), or this
  // device's until one is connected.
  XeniaMicrophoneChoiceHeadset = 2,
};

// Posted on the main queue when the choice, the microphone in use, a headset or
// microphone access may have changed.
extern NSString* const XeniaMicrophoneStatusDidChangeNotification;

#ifdef __cplusplus
extern "C" {
#endif

// Once at launch, after the config is loaded: turns voice on for installs that
// saved voice_chat = false before it was a choice, and lets the choice switch
// voice while a game runs.
void xe_ios_microphone_start(void);

XeniaMicrophoneChoice xe_ios_microphone_choice(void);
// Saves the choice and applies it at once, in a running game too.
void xe_ios_microphone_set_choice(XeniaMicrophoneChoice choice);

// A title started. The first time one that uses voice chat starts with voice on,
// iOS asks for microphone access.
void xe_ios_microphone_title_started(BOOL uses_voice);

// "Off", "This iPhone", "Headset (AirPods, etc.)".
NSString* xe_ios_microphone_choice_title(XeniaMicrophoneChoice choice);
// What a choice does now, e.g. that no headset is connected; nil when nothing to say.
NSString* xe_ios_microphone_choice_detail(XeniaMicrophoneChoice choice);
// The microphone in use now, or why there is none: "Using iPhone Microphone",
// "Microphone access is off", "On in games with voice chat".
NSString* xe_ios_microphone_status_text(void);
// SF Symbol for the current state: mic.fill (this device's), headphones (a headset's
// is live), mic.slash (off) or mic.badge.xmark (no access).
NSString* xe_ios_microphone_symbol_name(void);

// The user denied (or a restriction blocks) microphone access to XeniOS.
BOOL xe_ios_microphone_access_denied(void);
// Opens XeniOS's page in the iOS Settings app.
void xe_ios_microphone_open_settings(void);

// The in-game menu's Microphone menu: what is live now as its heading, the three
// choices (the current one checked), and Allow in iOS Settings when access is off.
// A snapshot: build it again when XeniaMicrophoneStatusDidChangeNotification posts.
UIMenu* xe_ios_microphone_new_menu(void);

#ifdef __cplusplus
}
#endif

// Settings → Audio → Microphone: the three choices with what each does now,
// what is live, and a link to iOS Settings when access is off.
@interface XeniaIOSMicrophoneViewController : UITableViewController
@end

#endif  // XENIA_UI_IOS_SHARED_IOS_MICROPHONE_H_
