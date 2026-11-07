/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/shared/ios_party_voice.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xenia/apu/voice/party_voice.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/net/packet_transport.h"
#include "xenia/net/party_voice_wire.h"
#include "xenia/net/relay_protocol.h"
#import "xenia/ui/ios/shared/ios_live_account_real.h"
#import "xenia/ui/ios/shared/ios_live_social_bridge.h"
#import "xenia/ui/ios/shared/ios_microphone.h"
#import "xenia/ui/ios/shared/ios_online_features.h"

DECLARE_string(systemlink_lobby);

NSNotificationName const XeniaPartyVoiceDidChangeNotification = @"XeniaPartyVoiceDidChange";

namespace {

namespace voice = xe::apu::voice;

// A member not heard this long after party voice started has no direct path.
constexpr uint64_t kReachGraceMs = 5000;

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The main thread's.
struct State {
  std::unique_ptr<voice::PartyVoice> session;
  uint64_t started_ms = 0;
  // The party and member the session is for.
  std::string room;
  uint64_t xuid = 0;
  // The party Game Chat was picked in; Party Chat again in any other.
  std::string game_chat_room;
  bool muted = false;
  bool available = false;
  // The other members (blocked ones left out), with their gamertags.
  std::vector<std::pair<uint64_t, std::string>> others;
};

State& S() {
  static State* state = new State;
  return *state;
}

void PostChanged() {
  dispatch_async(dispatch_get_main_queue(), ^{
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center postNotificationName:XeniaPartyVoiceDidChangeNotification object:nil];
    // The Microphone menu rebuilds on this.
    [center postNotificationName:XeniaMicrophoneStatusDidChangeNotification object:nil];
  });
}

std::unique_ptr<voice::PartyVoice> OpenSession(const std::string& party_room, uint64_t xuid) {
  const auto code = xe::net::party_voice::VoiceRoomCode(party_room);
  if (!code) {
    XELOGW("Party voice: the party's code isn't a room code");
    return nullptr;
  }
  voice::PartyVoiceConfig config;
  config.xuid = xuid;
  const std::string lobby = cvars::systemlink_lobby;
  const xe::net::relay::Room room = xe::net::relay::PrivateRoom(*code);
  config.open_transport =
      [lobby, room](const xe::net::secure::Mac& mac)
      -> std::unique_ptr<xe::net::secure::PacketTransport> {
    xe::net::relay::RelayConfig relay;
    relay.lobby = lobby;
    relay.room = room;
    relay.report_status = false;
    relay.allowed = [] { return bool(xe_ios_online_features_enabled()); };
    std::string error;
    auto transport = xe::net::secure::OpenRelayTransport(mac, relay, error);
    if (!transport) {
      XELOGE("Party voice: no voice room: {}", error);
    }
    return transport;
  };
  config.changed = [] { PostChanged(); };
  XELOGI("Party voice: on, in the voice room of party {}", party_room);
  return std::make_unique<voice::PartyVoice>(std::move(config));
}

void Evaluate() {
  State& st = S();
  std::optional<xe::net::live::PartyState> party;
  uint64_t me = 0;
  if (auto share = xe_ios_live_social_current_share(); share && (*share)->signed_in()) {
    party = (*share)->Party();
    me = (*share)->xuid();
  }
  XeniaLiveSocial* social = [XeniaLiveSocial shared];
  std::vector<std::pair<uint64_t, std::string>> others;
  if (party) {
    for (const auto& [xuid, gamertag] : party->members) {
      if (xuid != me && ![social isBlocked:xuid]) {
        others.emplace_back(xuid, gamertag);
      }
    }
  }
  if (!party || party->room != st.game_chat_room) {
    st.game_chat_room.clear();
  }
  const bool microphone_on = xe_ios_microphone_choice() != XeniaMicrophoneChoiceOff &&
                             !xe_ios_microphone_access_denied();
  const bool available =
      party && !others.empty() && microphone_on && xe_ios_online_features_enabled();
  const bool want = available && st.game_chat_room.empty();

  bool changed = available != st.available || others != st.others;
  st.available = available;
  st.others = others;
  if (want) {
    if (!st.session || st.room != party->room || st.xuid != me) {
      st.session.reset();
      st.session = OpenSession(party->room, me);
      st.room = party->room;
      st.xuid = me;
      st.started_ms = NowMs();
      changed = true;
    }
    if (st.session) {
      std::vector<uint64_t> xuids;
      for (const auto& member : others) {
        xuids.push_back(member.first);
      }
      st.session->SetMembers(std::move(xuids));
      st.session->SetSelfMuted(st.muted);
    }
  } else if (st.session) {
    XELOGI("Party voice: off ({})", !available ? "not in a party with voice" : "game chat");
    st.session.reset();
    st.room.clear();
    changed = true;
  }
  if (changed) {
    PostChanged();
  }
}

NSString* NameOf(uint64_t xuid) {
  for (const auto& [x, gamertag] : S().others) {
    if (x == xuid) {
      return [NSString stringWithUTF8String:gamertag.c_str()];
    }
  }
  return @"?";
}

}  // namespace

void xe_ios_party_voice_start(void) {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    for (NSNotificationName name :
         {XeniaLiveSocialDidChangeNotification, XeniaMicrophoneStatusDidChangeNotification,
          NSUserDefaultsDidChangeNotification}) {
      [center addObserverForName:name
                          object:nil
                           queue:[NSOperationQueue mainQueue]
                      usingBlock:^(NSNotification* __unused note) {
                        Evaluate();
                      }];
    }
    Evaluate();
  });
}

BOOL xe_ios_party_voice_available(void) { return S().available; }

BOOL xe_ios_party_voice_party_chat(void) { return S().game_chat_room.empty(); }

void xe_ios_party_voice_set_party_chat(BOOL party_chat) {
  State& st = S();
  std::string room;
  if (!party_chat) {
    if (auto share = xe_ios_live_social_current_share()) {
      if (auto party = (*share)->Party()) {
        room = party->room;
      }
    }
    if (room.empty()) {
      return;
    }
  }
  st.game_chat_room = room;
  XELOGI("Party voice: {} chosen", party_chat ? "party chat" : "game chat");
  Evaluate();
  PostChanged();
}

BOOL xe_ios_party_voice_self_muted(void) { return S().muted; }

void xe_ios_party_voice_set_self_muted(BOOL muted) {
  State& st = S();
  st.muted = muted;
  if (st.session) {
    st.session->SetSelfMuted(muted);
  }
  PostChanged();
}

BOOL xe_ios_party_voice_active(void) { return S().session != nullptr; }

XeniaPartyVoiceMemberState xe_ios_party_voice_member_state(uint64_t xuid) {
  State& st = S();
  if (!st.session) {
    return XeniaPartyVoiceMemberStateNone;
  }
  for (const auto& member : st.session->Members()) {
    if (member.xuid != xuid) {
      continue;
    }
    if (member.talking) {
      return XeniaPartyVoiceMemberStateTalking;
    }
    if (member.muted) {
      return XeniaPartyVoiceMemberStateMuted;
    }
    if (member.connected) {
      return XeniaPartyVoiceMemberStateConnected;
    }
    return NowMs() - st.started_ms < kReachGraceMs ? XeniaPartyVoiceMemberStateNone
                                                     : XeniaPartyVoiceMemberStateUnreachable;
  }
  return XeniaPartyVoiceMemberStateNone;
}

NSString* xe_ios_party_voice_status_text(void) {
  State& st = S();
  if (!st.session) {
    return nil;
  }
  if (st.session->failed() && !st.session->running()) {
    return NSLocalizedString(@"Party chat can't use the microphone.",
                             @"Party voice status: the voice device didn't start");
  }
  NSMutableArray<NSString*>* talking = [NSMutableArray array];
  NSMutableArray<NSString*>* unreachable = [NSMutableArray array];
  size_t connected = 0;
  for (const auto& member : st.others) {
    switch (xe_ios_party_voice_member_state(member.first)) {
      case XeniaPartyVoiceMemberStateTalking:
        [talking addObject:NameOf(member.first)];
        ++connected;
        break;
      case XeniaPartyVoiceMemberStateConnected:
      case XeniaPartyVoiceMemberStateMuted:
        ++connected;
        break;
      case XeniaPartyVoiceMemberStateUnreachable:
        [unreachable addObject:NameOf(member.first)];
        break;
      default:
        break;
    }
  }
  if (talking.count == 1) {
    return [NSString stringWithFormat:NSLocalizedString(@"%@ is talking.",
                                                        @"Party voice status: %@ is a gamertag"),
                                      talking[0]];
  }
  if (talking.count > 1) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"%@ are talking.",
                                           @"Party voice status: %@ lists gamertags"),
                         [NSListFormatter localizedStringByJoiningStrings:talking]];
  }
  if (unreachable.count) {
    return [NSString
        stringWithFormat:NSLocalizedString(@"Can't reach %@, so you can't hear each other.",
                                           @"Party voice status: no direct path to these members "
                                           @"(%@ lists gamertags)"),
                         [NSListFormatter localizedStringByJoiningStrings:unreachable]];
  }
  return [NSString
      localizedStringWithFormat:NSLocalizedString(@"%lu of %lu connected.",
                                                  @"Party voice status: members reached of the "
                                                  @"other members"),
                                (unsigned long)connected, (unsigned long)st.others.size()];
}

UIMenu* xe_ios_party_voice_new_menu_section(void) {
  if (!xe_ios_party_voice_available()) {
    return nil;
  }
  const BOOL party_chat = xe_ios_party_voice_party_chat();
  UIAction* party = [UIAction
      actionWithTitle:NSLocalizedString(@"Party Chat", @"Microphone menu: talk to your party")
                image:[UIImage systemImageNamed:@"person.2.wave.2"]
           identifier:nil
              handler:^(__unused UIAction* a) {
                xe_ios_party_voice_set_party_chat(YES);
              }];
  party.subtitle = xe_ios_party_voice_status_text()
                       ?: NSLocalizedString(@"Your party hears you in any game. This game's chat "
                                            @"doesn't.",
                                            @"Microphone menu: what Party Chat does");
  party.state = party_chat ? UIMenuElementStateOn : UIMenuElementStateOff;
  UIAction* game = [UIAction
      actionWithTitle:NSLocalizedString(@"Game Chat", @"Microphone menu: talk in the game")
                image:[UIImage systemImageNamed:@"gamecontroller"]
           identifier:nil
              handler:^(__unused UIAction* a) {
                xe_ios_party_voice_set_party_chat(NO);
              }];
  game.subtitle = NSLocalizedString(@"Talk to the players in this game. Your party doesn't hear "
                                    @"you.",
                                    @"Microphone menu: what Game Chat does");
  game.state = party_chat ? UIMenuElementStateOff : UIMenuElementStateOn;
  NSMutableArray<UIMenuElement*>* children = [NSMutableArray arrayWithObjects:party, game, nil];
  if (party_chat) {
    const BOOL muted = xe_ios_party_voice_self_muted();
    UIAction* mute = [UIAction
        actionWithTitle:NSLocalizedString(@"Mute Me", @"Microphone menu: mute yourself in the party")
                  image:[UIImage systemImageNamed:@"mic.slash"]
             identifier:nil
                handler:^(__unused UIAction* a) {
                  xe_ios_party_voice_set_self_muted(!muted);
                }];
    mute.subtitle = NSLocalizedString(@"Your party doesn't hear you. You still hear them.",
                                      @"Microphone menu: what Mute Me does");
    mute.state = muted ? UIMenuElementStateOn : UIMenuElementStateOff;
    [children addObject:mute];
  }
  return [UIMenu menuWithTitle:NSLocalizedString(@"Party", @"Microphone menu section")
                         image:nil
                    identifier:nil
                       options:UIMenuOptionsDisplayInline
                      children:children];
}
