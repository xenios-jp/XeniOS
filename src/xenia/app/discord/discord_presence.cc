/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2020 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include "discord_presence.h"
#include <ctime>
#include "third_party/discord-rpc/include/discord_rpc.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

// TODO: This library has been deprecated in favor of Discord's GameSDK.
namespace xe {
namespace discord {

void HandleDiscordReady(const DiscordUser* request) {}
void HandleDiscordError(int errorCode, const char* message) {}
void HandleDiscordJoinGame(const char* joinSecret) {}
void HandleDiscordJoinRequest(const DiscordUser* request) {}
void HandleDiscordSpectateGame(const char* spectateSecret) {}

bool DiscordPresence::initialized_ = false;

void DiscordPresence::Initialize() {
  if (initialized_) {
    return;
  }
  DiscordEventHandlers handlers = {};
  handlers.ready = &HandleDiscordReady;
  handlers.errored = &HandleDiscordError;
  handlers.joinGame = &HandleDiscordJoinGame;
  handlers.joinRequest = &HandleDiscordJoinRequest;
  handlers.spectateGame = &HandleDiscordSpectateGame;
  Discord_Initialize("1425285186387578910", &handlers, 0, "");
  initialized_ = true;
}

void DiscordPresence::PlayingTitle(const std::string_view game_title) {
  auto details = std::string(game_title);
  DiscordRichPresence discordPresence = {};
  discordPresence.state = "In Game";
  discordPresence.details = details.c_str();
  // TODO(gibbed): we don't have state icons yet.
  // discordPresence.smallImageKey = "app";
  // discordPresence.largeImageKey = "state_ingame";
  discordPresence.largeImageKey = "app";
#if XE_PLATFORM_MAC
  discordPresence.largeImageText = "XeniOS";
#else
  discordPresence.largeImageText = "Xenia Edge - Experimental Testing branch";
#endif
  discordPresence.startTimestamp = time(0);
  discordPresence.instance = 1;
  Discord_UpdatePresence(&discordPresence);
}

void DiscordPresence::Shutdown() {
  if (!initialized_) {
    return;
  }
  Discord_Shutdown();
  initialized_ = false;
}

}  // namespace discord
}  // namespace xe
