/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_microphone.h"
#import "xenia/ui/ios/shared/ios_party_voice.h"

#import <AVFAudio/AVFAudio.h>

#include <string>

#include "xenia/apu/voice/voice_device.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/ios/app/ios_lifecycle_audio_session.h"

#import "xenia/ui/ios/shared/ios_theme.h"

DECLARE_bool(voice_chat);

DEFINE_string(voice_microphone, "device",
              "The microphone voice chat uses on iPhone and iPad while voice_chat is on: "
              "device (its own; AirPods keep full-quality sound) or headset (a connected "
              "headset's, which makes Bluetooth audio sound like a phone call).",
              "APU");

NSString* const XeniaMicrophoneStatusDidChangeNotification = @"XeniaMicrophoneStatusChanged";

namespace {

// Set once voice_chat's default became on. Before, every install saved
// voice_chat = false without anyone choosing it.
NSString* const kVoiceChatDefaultOnMigrated = @"XeniaVoiceChatDefaultOnMigrated";

constexpr const char* kHeadsetValue = "headset";
constexpr const char* kDeviceValue = "device";

void PostStatusChanged() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSNotificationCenter defaultCenter]
        postNotificationName:XeniaMicrophoneStatusDidChangeNotification
                      object:nil];
  });
}

// Sets a cvar as the user's choice: in effect now (over a game's override too)
// and what the config file saves.
template <typename T>
void SetChosenValue(const char* name, const T& value) {
  if (!cvar::ConfigVars) {
    return;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    return;
  }
  if (auto* var = dynamic_cast<cvar::ConfigVar<T>*>(it->second)) {
    var->OverrideConfigValue(value);
  }
}

bool HeadsetValueSelected() { return cvars::voice_microphone == kHeadsetValue; }

NSString* DeviceName() {
  return [UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPad
             ? NSLocalizedString(@"iPad", @"Microphone: this device's name")
             : NSLocalizedString(@"iPhone", @"Microphone: this device's name");
}

bool IsHeadsetPort(NSString* port_type) {
  return [port_type isEqualToString:AVAudioSessionPortBluetoothHFP] ||
         [port_type isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
         [port_type isEqualToString:AVAudioSessionPortBluetoothLE] ||
         [port_type isEqualToString:AVAudioSessionPortHeadsetMic] ||
         [port_type isEqualToString:AVAudioSessionPortHeadphones] ||
         [port_type isEqualToString:AVAudioSessionPortUSBAudio] ||
         [port_type isEqualToString:AVAudioSessionPortCarAudio];
}

// AirPods, a Bluetooth or wired headset, or USB audio. Without voice the session
// lists no inputs, so the outputs count too: AirPods play over A2DP until the
// Headset choice takes their microphone.
bool HeadsetConnected() {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
    if (IsHeadsetPort(port.portType)) {
      return true;
    }
  }
  for (AVAudioSessionPortDescription* port in session.availableInputs) {
    if (IsHeadsetPort(port.portType)) {
      return true;
    }
  }
  return false;
}

AVAudioApplicationRecordPermission Permission() {
  return [AVAudioApplication sharedInstance].recordPermission;
}

// The microphone voice records from now, or nil while voice isn't running.
NSString* LiveInputName() {
  if (!xe::ui::IOSAudioSessionVoiceActive() ||
      Permission() != AVAudioApplicationRecordPermissionGranted) {
    return nil;
  }
  AVAudioSessionPortDescription* input =
      [AVAudioSession sharedInstance].currentRoute.inputs.firstObject;
  return input.portName.length ? input.portName : nil;
}

}  // namespace

void xe_ios_microphone_start(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    if (![defaults boolForKey:kVoiceChatDefaultOnMigrated]) {
      [defaults setBool:YES forKey:kVoiceChatDefaultOnMigrated];
      if (!cvars::voice_chat) {
        SetChosenValue("voice_chat", true);
        config::SaveConfigValues({"voice_chat"});
        XELOGI("Microphone: voice chat is on by default now");
      }
    }
    xe::apu::voice::SetHostVoiceSwitchable(true);
    xe::ui::IOSAudioSessionSetHeadsetMicrophone(HeadsetValueSelected());
    // Back from the Settings app (microphone access) or a headset change while away.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:UIApplicationDidBecomeActiveNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification*) {
                  PostStatusChanged();
                }];
  });
}

XeniaMicrophoneChoice xe_ios_microphone_choice(void) {
  if (!cvars::voice_chat) {
    return XeniaMicrophoneChoiceOff;
  }
  return HeadsetValueSelected() ? XeniaMicrophoneChoiceHeadset : XeniaMicrophoneChoiceDevice;
}

void xe_ios_microphone_set_choice(XeniaMicrophoneChoice choice) {
  const XeniaMicrophoneChoice old_choice = xe_ios_microphone_choice();
  if (choice != XeniaMicrophoneChoiceOff) {
    SetChosenValue("voice_microphone",
                   std::string(choice == XeniaMicrophoneChoiceHeadset ? kHeadsetValue
                                                                       : kDeviceValue));
  }
  // Off keeps the microphone picked last, for when voice is turned on again.
  // The headset thread follows voice_chat within a few milliseconds.
  SetChosenValue("voice_chat", choice != XeniaMicrophoneChoiceOff);
  config::SaveConfigValues({"voice_chat", "voice_microphone"});
  xe::ui::IOSAudioSessionSetHeadsetMicrophone(HeadsetValueSelected());
  if (choice != old_choice) {
    XELOGI("Microphone: {}", [xe_ios_microphone_choice_title(choice) UTF8String]);
  }
  PostStatusChanged();
}

void xe_ios_microphone_title_started(BOOL uses_voice) {
  if (!uses_voice || !cvars::voice_chat ||
      Permission() != AVAudioApplicationRecordPermissionUndetermined) {
    return;
  }
  XELOGI("Microphone: asking for access for a game with voice chat");
  [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
    XELOGI("Microphone: access {}", granted ? "granted" : "denied");
    PostStatusChanged();
  }];
}

NSString* xe_ios_microphone_choice_title(XeniaMicrophoneChoice choice) {
  switch (choice) {
    case XeniaMicrophoneChoiceOff:
      return NSLocalizedString(@"Off", @"Microphone choice: voice chat off");
    case XeniaMicrophoneChoiceDevice:
      return [NSString stringWithFormat:NSLocalizedString(@"This %@",
                                                          @"Microphone choice: this device's "
                                                          @"own microphone; %@ is iPhone or iPad"),
                                        DeviceName()];
    case XeniaMicrophoneChoiceHeadset:
      return NSLocalizedString(@"Headset (AirPods, etc.)",
                               @"Microphone choice: a connected headset's microphone");
  }
  return @"";
}

NSString* xe_ios_microphone_choice_detail(XeniaMicrophoneChoice choice) {
  switch (choice) {
    case XeniaMicrophoneChoiceOff:
      return NSLocalizedString(@"No voice chat. Other players don't hear you, and you don't "
                               @"hear them.",
                               @"Microphone choice detail: Off");
    case XeniaMicrophoneChoiceDevice:
      return NSLocalizedString(@"Game audio stays full quality on AirPods and headphones.",
                               @"Microphone choice detail: this device's microphone");
    case XeniaMicrophoneChoiceHeadset: {
      // Bluetooth hands-free carries the microphone at call quality, game audio too.
      NSString* call = NSLocalizedString(@"Game audio sounds like a call.",
                                         @"Microphone choice detail: headset microphone");
      if (HeadsetConnected()) {
        return call;
      }
      NSString* none = [NSString
          stringWithFormat:NSLocalizedString(@"No headset connected — using this %@.",
                                             @"Microphone choice detail: no headset; %@ is "
                                             @"iPhone or iPad"),
                           DeviceName()];
      return [NSString stringWithFormat:@"%@ %@", none, call];
    }
  }
  return nil;
}

BOOL xe_ios_microphone_access_denied(void) {
  return Permission() == AVAudioApplicationRecordPermissionDenied;
}

NSString* xe_ios_microphone_status_text(void) {
  if (xe_ios_microphone_choice() == XeniaMicrophoneChoiceOff) {
    return NSLocalizedString(@"Voice chat is off.", @"Microphone status");
  }
  if (xe_ios_microphone_access_denied()) {
    return NSLocalizedString(@"Microphone access is off for XeniOS, so voice chat is off.",
                             @"Microphone status: access denied in iOS Settings");
  }
  if (NSString* live = LiveInputName()) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"Using %@", @"Microphone status: %@ is the "
                                                        @"microphone in use, e.g. AirPods Pro"),
                         live];
  }
  return NSLocalizedString(@"On in games with voice chat.",
                           @"Microphone status: voice is on, no game is using it");
}

NSString* xe_ios_microphone_symbol_name(void) {
  if (xe_ios_microphone_choice() == XeniaMicrophoneChoiceOff) {
    return @"mic.slash";
  }
  if (xe_ios_microphone_access_denied()) {
    return @"mic.badge.xmark";
  }
  // What records now: a headset's microphone, or this device's (also until a game uses voice).
  if (xe::ui::IOSAudioSessionVoiceActive()) {
    AVAudioSessionPortDescription* input =
        [AVAudioSession sharedInstance].currentRoute.inputs.firstObject;
    if (input && ![input.portType isEqualToString:AVAudioSessionPortBuiltInMic]) {
      return @"headphones";
    }
  }
  return @"mic.fill";
}

void xe_ios_microphone_open_settings(void) {
  NSURL* url = [NSURL URLWithString:UIApplicationOpenSettingsURLString];
  if (url) {
    [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
  }
}

UIMenu* xe_ios_microphone_new_menu(void) {
  const XeniaMicrophoneChoice current = xe_ios_microphone_choice();
  NSMutableArray<UIMenuElement*>* choices = [NSMutableArray array];
  for (XeniaMicrophoneChoice choice :
       {XeniaMicrophoneChoiceOff, XeniaMicrophoneChoiceDevice, XeniaMicrophoneChoiceHeadset}) {
    UIAction* action = [UIAction actionWithTitle:xe_ios_microphone_choice_title(choice)
                                           image:nil
                                      identifier:nil
                                         handler:^(__unused UIAction* a) {
                                           xe_ios_microphone_set_choice(choice);
                                         }];
    action.subtitle = xe_ios_microphone_choice_detail(choice);
    action.state = choice == current ? UIMenuElementStateOn : UIMenuElementStateOff;
    [choices addObject:action];
  }
  NSMutableArray<UIMenuElement*>* items = [NSMutableArray array];
  [items addObject:[UIMenu menuWithTitle:@""
                                   image:nil
                              identifier:nil
                                 options:UIMenuOptionsDisplayInline
                                children:choices]];
  // Party Chat or Game Chat, and Mute Me, while in a party with someone else.
  if (UIMenu* party = xe_ios_party_voice_new_menu_section()) {
    [items addObject:party];
  }
  if (current != XeniaMicrophoneChoiceOff && xe_ios_microphone_access_denied()) {
    UIAction* settings = [UIAction
        actionWithTitle:NSLocalizedString(@"Allow in iOS Settings",
                                          @"Microphone menu: opens XeniOS's iOS Settings page")
                  image:[UIImage systemImageNamed:@"gear"]
             identifier:nil
                handler:^(__unused UIAction* a) {
                  xe_ios_microphone_open_settings();
                }];
    [items addObject:[UIMenu menuWithTitle:@""
                                     image:nil
                                identifier:nil
                                   options:UIMenuOptionsDisplayInline
                                  children:@[ settings ]]];
  }
  // The heading says what is live now.
  return [UIMenu menuWithTitle:xe_ios_microphone_status_text() children:items];
}

namespace {
enum MicrophoneSection : NSInteger {
  kChoicesSection = 0,
  kStatusSection = 1,
  kSectionCount = 2,
};
}  // namespace

@implementation XeniaIOSMicrophoneViewController

- (instancetype)init {
  if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    self.title = NSLocalizedString(@"Microphone", @"Microphone settings page title");
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.tableView.backgroundColor = [UIColor systemBackgroundColor];
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(statusChanged:)
                                               name:XeniaMicrophoneStatusDidChangeNotification
                                             object:nil];
}

- (void)statusChanged:(NSNotification*)__unused notification {
  [self.tableView reloadData];
}

- (BOOL)showsSettingsRow {
  return xe_ios_microphone_choice() != XeniaMicrophoneChoiceOff &&
         xe_ios_microphone_access_denied();
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)__unused tableView {
  return kSectionCount;
}

- (NSInteger)tableView:(UITableView*)__unused tableView numberOfRowsInSection:(NSInteger)section {
  if (section == kChoicesSection) {
    return 3;
  }
  return [self showsSettingsRow] ? 2 : 1;
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForHeaderInSection:(NSInteger)section {
  if (section == kStatusSection) {
    return NSLocalizedString(@"Now", @"Microphone settings: what is in use now");
  }
  return nil;
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForFooterInSection:(NSInteger)section {
  if (section == kChoicesSection) {
    return NSLocalizedString(@"Games with voice chat hear you as if a headset were plugged into "
                             @"the first controller. Game audio plays on connected AirPods or "
                             @"headphones, else the speaker. Changes apply at once, in a game too.",
                             @"Microphone settings footer");
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  static NSString* const kIdentifier = @"XeniaMicrophoneCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                   reuseIdentifier:kIdentifier] autorelease];
  }
  UIListContentConfiguration* content = [UIListContentConfiguration subtitleCellConfiguration];
  content.secondaryTextProperties.color = [XeniaTheme textSecondary];
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.selectionStyle = UITableViewCellSelectionStyleDefault;
  cell.accessibilityTraits = UIAccessibilityTraitButton;
  cell.accessibilityValue = nil;
  if (indexPath.section == kChoicesSection) {
    const XeniaMicrophoneChoice choice = static_cast<XeniaMicrophoneChoice>(indexPath.row);
    const bool selected = choice == xe_ios_microphone_choice();
    content.text = xe_ios_microphone_choice_title(choice);
    content.secondaryText = xe_ios_microphone_choice_detail(choice);
    cell.accessoryType =
        selected ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    if (selected) {
      cell.accessibilityTraits |= UIAccessibilityTraitSelected;
    }
  } else if (indexPath.row == 0) {
    content.text = xe_ios_microphone_status_text();
    content.image = [UIImage systemImageNamed:xe_ios_microphone_symbol_name()];
    content.imageProperties.tintColor = xe_ios_microphone_access_denied() &&
                                                xe_ios_microphone_choice() !=
                                                    XeniaMicrophoneChoiceOff
                                            ? [XeniaTheme statusWarning]
                                            : [XeniaTheme textSecondary];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.accessibilityTraits = UIAccessibilityTraitStaticText;
  } else {
    content.text = NSLocalizedString(@"Allow in iOS Settings",
                                     @"Microphone settings: opens XeniOS's iOS Settings page");
    content.textProperties.color = [XeniaTheme accent];
    content.secondaryText = NSLocalizedString(@"Turn on Microphone for XeniOS.",
                                              @"Microphone settings: what to do in iOS Settings");
  }
  cell.contentConfiguration = content;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (indexPath.section == kChoicesSection) {
    xe_ios_microphone_set_choice(static_cast<XeniaMicrophoneChoice>(indexPath.row));
    [tableView reloadData];
  } else if (indexPath.row == 1) {
    xe_ios_microphone_open_settings();
  }
}

@end
