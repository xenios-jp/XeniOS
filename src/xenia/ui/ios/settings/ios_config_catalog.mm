/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_config_catalog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/ui/config_helpers.h"

#import "xenia/ui/ios/settings/ios_config_storage.h"

// SecTask entitlement APIs aren't public on iOS, so detect a granted entitlement
// by scanning the bundle's embedded provisioning profile, where granted
// entitlement keys appear verbatim in the (CMS-wrapped) Entitlements plist.
// Conservative: if there's no profile to inspect, assume granted so we never
// show a false "missing entitlement" warning.
static bool IOSReadHeadPoseEntitlement() {
  @autoreleasepool {
    NSString* path = [[NSBundle mainBundle] pathForResource:@"embedded"
                                                     ofType:@"mobileprovision"];
    if (!path.length) {
      return true;
    }
    NSData* data = [NSData dataWithContentsOfFile:path];
    if (!data || data.length == 0) {
      return true;
    }
    NSData* needle = [@"com.apple.developer.coremotion.head-pose"
        dataUsingEncoding:NSUTF8StringEncoding];
    NSRange found = [data rangeOfData:needle
                              options:0
                                range:NSMakeRange(0, data.length)];
    return found.location != NSNotFound;
  }
}

// The profile is read once; the answer can't change while the app runs.
static bool IOSHasHeadPoseEntitlement() {
  static bool granted = false;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    granted = IOSReadHeadPoseEntitlement();
  });
  return granted;
}

static std::string TrimAscii(std::string value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(start, end - start);
}

// Adds |item| and records the cvar's default so rows can show and restore it.
static void AddItem(std::vector<IOSConfigItem>& items, IOSConfigItem item) {
  if (item.storage == IOSConfigStorage::kConfigVar && !item.key.empty() &&
      item.control_type != IOSConfigControlType::kAction) {
    item.has_default = IOSConfigGetConfigVarDefaultString(item.key, &item.default_value);
  }
  items.push_back(std::move(item));
}

static void AddBoolSetting(std::vector<IOSConfigItem>& items, const std::string& key,
                           const std::string& title, const std::string& subtitle, bool fallback) {
  if (!IOSConfigHasConfigVar(key)) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kToggle;
  item.bool_value = fallback;
  IOSConfigParseBoolString(IOSConfigGetConfigVarString(key, fallback ? "true" : "false"),
                           &item.bool_value);
  AddItem(items, std::move(item));
}

static void AddUserDefaultBoolSetting(std::vector<IOSConfigItem>& items, NSString* key,
                                      const std::string& title, const std::string& subtitle,
                                      bool fallback) {
  if (!key || key.length == 0) {
    return;
  }
  IOSConfigItem item;
  item.key = std::string([key UTF8String]);
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kToggle;
  item.storage = IOSConfigStorage::kUserDefaults;
  item.bool_value = GetUserDefaultBool(key, fallback);
  item.has_default = true;
  item.default_value = fallback ? "true" : "false";
  items.push_back(std::move(item));
}

static void AddChoiceSetting(std::vector<IOSConfigItem>& items, IOSConfigControlType control_type,
                             const std::string& key, const std::string& title,
                             const std::string& subtitle, int64_t fallback,
                             std::vector<IOSConfigChoice> choices,
                             const std::string& value_unit = std::string()) {
  if (!IOSConfigHasConfigVar(key) || choices.empty()) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = control_type;
  item.value_unit = value_unit;
  item.choice_value = fallback;
  IOSConfigParseInt64String(IOSConfigGetConfigVarString(key, std::to_string(fallback)),
                            &item.choice_value);
  item.choices = std::move(choices);
  bool found = false;
  for (const IOSConfigChoice& choice : item.choices) {
    if (choice.value == item.choice_value) {
      found = true;
      break;
    }
  }
  if (!found) {
    // Keep a value the list doesn't offer (hand-edited config, older build) and show it as is;
    // it only changes if the user picks another entry.
    item.choices.insert(
        item.choices.begin(),
        {IOSConfigCustomChoiceTitle(item.choice_value, value_unit), item.choice_value, true});
  }
  AddItem(items, std::move(item));
}

static void AddStringChoiceSetting(std::vector<IOSConfigItem>& items, const std::string& key,
                                   const std::string& title, const std::string& subtitle,
                                   const std::string& fallback,
                                   std::vector<std::pair<std::string, std::string>> choices) {
  if (!IOSConfigHasConfigVar(key) || choices.empty()) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kChoiceString;
  item.string_value = IOSConfigGetConfigVarString(key, fallback);

  for (size_t i = 0; i < choices.size(); ++i) {
    item.choices.push_back({choices[i].first, static_cast<int64_t>(i)});
    item.choice_string_values.push_back(choices[i].second);
  }

  bool found = false;
  for (size_t i = 0; i < item.choice_string_values.size(); ++i) {
    if (item.choice_string_values[i] == item.string_value) {
      item.choice_value = static_cast<int64_t>(i);
      found = true;
      break;
    }
  }
  if (!found && !item.string_value.empty()) {
    item.choices.push_back({IOSConfigCustomChoiceTitle(item.string_value),
                            static_cast<int64_t>(item.choice_string_values.size()), true});
    item.choice_string_values.push_back(item.string_value);
    item.choice_value = static_cast<int64_t>(item.choice_string_values.size() - 1);
    found = true;
  }
  if (!found) {
    item.string_value = fallback;
    for (size_t i = 0; i < item.choice_string_values.size(); ++i) {
      if (item.choice_string_values[i] == fallback) {
        item.choice_value = static_cast<int64_t>(i);
        found = true;
        break;
      }
    }
  }
  if (!found) {
    item.choice_value = 0;
    item.string_value = item.choice_string_values.front();
  }

  AddItem(items, std::move(item));
}

static void AddActionSetting(std::vector<IOSConfigItem>& items, IOSConfigAction action,
                             const std::string& title, const std::string& subtitle) {
  IOSConfigItem item;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kAction;
  item.action = action;
  items.push_back(std::move(item));
}

// Fills the choices of an enum row from |options| and selects the stored value. A stored value
// that isn't an option is kept as a last "Custom (...)" entry rather than shown as the first one.
static void FillEnumChoices(IOSConfigItem* item, const std::vector<std::string>& options) {
  bool found = false;
  for (size_t i = 0; i < options.size(); ++i) {
    item->choices.push_back({options[i], static_cast<int64_t>(i)});
    item->choice_string_values.push_back(options[i]);
    if (!found && options[i] == item->string_value) {
      item->choice_value = static_cast<int64_t>(i);
      found = true;
    }
  }
  if (!found && !item->string_value.empty()) {
    item->choices.push_back({IOSConfigCustomChoiceTitle(item->string_value),
                             static_cast<int64_t>(item->choice_string_values.size()), true});
    item->choice_string_values.push_back(item->string_value);
    item->choice_value = static_cast<int64_t>(item->choice_string_values.size() - 1);
  }
}

static void AddEnumSetting(std::vector<IOSConfigItem>& items, const std::string& key,
                           const std::string& title, const std::string& subtitle) {
  if (!IOSConfigHasConfigVar(key)) {
    return;
  }
  const auto& enum_opts = xe::ui::GetKnownEnumOptions();
  auto it = enum_opts.find(key);
  if (it == enum_opts.end() || it->second.empty()) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kEnum;
  item.enum_key = key;
  item.string_value = IOSConfigGetConfigVarString(key, it->second.front());
  FillEnumChoices(&item, it->second);
  AddItem(items, std::move(item));
}

static void AddIntegerSetting(std::vector<IOSConfigItem>& items, const std::string& key,
                              const std::string& title, const std::string& subtitle) {
  if (!IOSConfigHasConfigVar(key)) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kInteger;
  int64_t parsed = 0;
  if (IOSConfigParseInt64String(IOSConfigGetConfigVarString(key, "0"), &parsed)) {
    item.integer_value = parsed;
  }
  AddItem(items, std::move(item));
}

static void AddDoubleSetting(std::vector<IOSConfigItem>& items, const std::string& key,
                             const std::string& title, const std::string& subtitle,
                             const std::string& value_unit = std::string(), double min_value = 0.0,
                             double max_value = 0.0, double step = 0.0) {
  if (!IOSConfigHasConfigVar(key)) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kDouble;
  item.value_unit = value_unit;
  item.min_value = min_value;
  item.max_value = max_value;
  item.step = step;
  std::string raw = IOSConfigGetConfigVarString(key, "0");
  if (!raw.empty()) {
    char* end = nullptr;
    double parsed = std::strtod(raw.c_str(), &end);
    if (end != raw.c_str() && *end == '\0') {
      item.double_value = parsed;
    }
  }
  AddItem(items, std::move(item));
}

std::string ChoiceTitleForItem(const IOSConfigItem& item) {
  for (const IOSConfigChoice& choice : item.choices) {
    if (choice.value == item.choice_value) {
      return choice.title;
    }
  }
  return item.choices.empty() ? std::string() : item.choices.front().title;
}

static void PushIfNotEmpty(std::vector<IOSConfigSection>& sections,
                           IOSConfigSection section) {
  if (!section.items.empty()) {
    sections.push_back(std::move(section));
  }
}

static std::vector<IOSConfigSection> BuildDisplaySections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection display;
  display.title = "Display";
  AddBoolSetting(display.items, "present_letterbox", "Letterbox",
                 "Keeps the game's aspect ratio with bars. Turn off with a widescreen patch "
                 "that fills the screen.",
                 true);
  AddChoiceSetting(display.items, IOSConfigControlType::kChoiceUInt64, "framerate_limit",
                   "Frame Rate Limit",
                   "Lower limits run cooler and save battery.",
                   0,
                   {{"Unlimited", 0},
                    {"30 FPS", 30},
                    {"45 FPS", 45},
                    {"60 FPS", 60},
                    {"90 FPS", 90},
                    {"120 FPS", 120}},
                   "FPS");
  AddBoolSetting(display.items, "guest_display_refresh_cap", "Cap Guest Display Refresh",
                 "Runs the game at console speed. Turn off only with a frame rate patch "
                 "that needs it.",
                 true);
  AddBoolSetting(display.items, "use_50Hz_mode", "Use 50Hz PAL Timing",
                 "For PAL games that need 50 Hz. Leave off for most games.",
                 false);
  PushIfNotEmpty(sections, std::move(display));
  return sections;
}

static std::vector<IOSConfigSection> BuildGraphicsSections() {
  std::vector<IOSConfigSection> sections;

  // Metal is the only renderer on iOS, so the one-option "gpu" picker isn't shown.
  IOSConfigSection backend;
  backend.title = "Image Quality";
  AddBoolSetting(backend.items, "metal_presenter_force_10bpc", "High-Precision Color",
                 "Smoother gradients, and the fastest path on Apple GPUs. Turn off only if "
                 "colors look wrong.",
                 true);
  // Values are xenos::AnisoFilter levels; 1 (1x) is the same as off, so it isn't offered.
  AddChoiceSetting(backend.items, IOSConfigControlType::kChoiceInt32, "anisotropic_override",
                   "Texture Filtering", "Keeps textures sharp at an angle. Higher uses more GPU.",
                   -1,
                   {{"Game Default", -1},
                    {"Off", 0},
                    {"2x", 2},
                    {"4x", 3},
                    {"8x", 4},
                    {"16x", 5}});
  PushIfNotEmpty(sections, std::move(backend));
  // Shader cache and async compilation live under Performance, render target path under
  // Compatibility, so each setting has one home.
  return sections;
}

static std::vector<IOSConfigSection> BuildPerformanceSections() {
  std::vector<IOSConfigSection> sections;

  IOSConfigSection caching;
  caching.title = "Shaders";
  AddBoolSetting(caching.items, "store_shaders", "Save Shaders",
                 "Games stutter less the next time you play. Turn off only if graphics "
                 "break after an update.",
                 true);
  AddBoolSetting(caching.items, "async_shader_compilation", "Compile in Background",
                 "Less stutter, but new effects can appear a moment late.",
                 true);
  PushIfNotEmpty(sections, std::move(caching));

  IOSConfigSection scheduler;
  scheduler.title = "Priority";
  scheduler.footer = "Higher priority runs games smoother and uses more battery. Changes "
                     "apply after you relaunch XeniOS.";
  AddBoolSetting(scheduler.items, "ios_gpu_commands_user_interactive_qos",
                 "Prioritize Graphics", "Gets each frame to the screen sooner.", true);
  AddBoolSetting(scheduler.items, "ios_guest_threads_user_initiated_qos",
                 "Prioritize Game", "Gives the game more CPU time.", true);
  AddStringChoiceSetting(scheduler.items, "ios_audio_worker_qos", "Audio Priority",
                         "Higher keeps sound from crackling.", "user_interactive",
                         {{"Normal", "default"},
                          {"High", "user_initiated"},
                          {"Highest", "user_interactive"}});
  AddBoolSetting(scheduler.items, "ios_emulator_thread_user_initiated_qos",
                 "Prioritize Loading", "Starts games a little faster.", false);
  PushIfNotEmpty(sections, std::move(scheduler));
  return sections;
}

static std::vector<IOSConfigSection> BuildAudioSections() {
  std::vector<IOSConfigSection> sections;

  const bool phase_selected = IOSConfigGetConfigVarString("apu", "phase") == "phase";
  const bool head_pose_entitlement = IOSHasHeadPoseEntitlement();

  IOSConfigSection audio;
  audio.title = "Audio";

  // Surface the head-pose entitlement status when PHASE is in use, since head
  // tracking silently falls back to a fixed listener without it.
  if (phase_selected && !head_pose_entitlement) {
    // The head-pose entitlement isn't in this install's signing profile (SideStore can't
    // add it), so PHASE keeps a fixed listener.
    audio.footer = "This install can't use AirPods head tracking, so spatial sound stays "
                   "fixed in front of you.";
  }
  AddStringChoiceSetting(audio.items, "apu", "Sound",
                         "Spatial plays surround sound over headphones. Applies after "
                         "relaunch.",
                         "phase", {{"Stereo", "sdl"}, {"Spatial", "phase"}});
  // Without the entitlement the switch can't work, so it isn't shown; the footer says why.
  if (head_pose_entitlement) {
    AddBoolSetting(audio.items, "apu_phase_head_tracking", "Head Tracking",
                   "Spatial sound stays with the screen as you turn your head.", false);
  }
  PushIfNotEmpty(sections, std::move(audio));
  return sections;
}

// A range shown as a slider between two words, with its value and unit beside the title.
static void AddSliderSetting(std::vector<IOSConfigItem>& items, const std::string& key,
                             const std::string& title, const std::string& value_unit,
                             double min_value, double max_value, double step,
                             const std::string& min_label, const std::string& max_label) {
  const size_t count = items.size();
  AddDoubleSetting(items, key, title, std::string(), value_unit, min_value, max_value, step);
  if (items.size() > count) {
    items.back().min_label = min_label;
    items.back().max_label = max_label;
  }
}

static std::vector<IOSConfigSection> BuildControlsSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection touch;
  AddBoolSetting(touch.items, "ios_touch_overlay", "Show Touch Controls",
                 "When no controller is connected.", true);
  AddBoolSetting(touch.items, "ios_touch_haptics", "Haptics", "", true);
  PushIfNotEmpty(sections, std::move(touch));

  IOSConfigSection look;
  look.title = "Looking Around";
  look.footer = "A shorter swipe turns at full speed sooner.";
  AddSliderSetting(look.items, "ios_touch_look_points_per_full_scale", "Swipe Distance", "pt",
                   1.0, 20.0, 0.5, "Short", "Long");
  AddSliderSetting(look.items, "ios_touch_look_vertical_scale", "Vertical Speed", "\xC3\x97",
                   0.5, 3.0, 0.1, "Slower", "Faster");
  PushIfNotEmpty(sections, std::move(look));

  IOSConfigSection low_fps;
  low_fps.title = "For Slow Games";
  low_fps.footer =
      "Keeps quick swipes and taps from being missed when a game runs below 30 fps.";
  AddDoubleSetting(low_fps.items, "ios_touch_look_hold_seconds", "Look Hold", "", "s", 0.0, 0.5,
                   0.05);
  AddDoubleSetting(low_fps.items, "ios_touch_button_tap_hold_seconds", "Tap Hold", "", "s", 0.0,
                   0.5, 0.05);
  PushIfNotEmpty(sections, std::move(low_fps));
  return sections;
}

static std::vector<IOSConfigSection> BuildCompatibilitySections() {
  std::vector<IOSConfigSection> sections;

  IOSConfigSection audio_workarounds;
  audio_workarounds.title = "Audio Workarounds";
  audio_workarounds.footer = "Only for games whose audio stutters or breaks.";
  AddChoiceSetting(audio_workarounds.items, IOSConfigControlType::kChoiceUInt32, "apu_max_queued_frames",
                   "Audio Buffer",
                   "A larger buffer stops crackling but delays sound. Applies after "
                   "relaunch.",
                   8,
                   {{"8 Frames (Default)", 8},
                    {"16 Frames", 16},
                    {"32 Frames", 32},
                    {"48 Frames", 48},
                    {"64 Frames", 64}},
                   "Frames");
  AddStringChoiceSetting(audio_workarounds.items, "xma_decoder", "Audio Decoder",
                         "Try another decoder if a game's sound is broken.", "new",
                         {{"Standard", "new"},
                          {"Legacy", "old"},
                          {"Alternate", "master"},
                          {"Off (No Game Sound)", "fake"}});
  AddBoolSetting(audio_workarounds.items, "use_dedicated_xma_thread", "Separate Audio Thread",
                 "Try only if sound stutters or the game stalls.", false);
  PushIfNotEmpty(sections, std::move(audio_workarounds));

  IOSConfigSection gpu;
  gpu.title = "GPU Workarounds";
  gpu.footer = "Leave these at their defaults unless a specific title needs them.";
  AddBoolSetting(gpu.items, "half_pixel_offset", "Half-Pixel Offset",
                 "Keeps menus and effects aligned. Turn off only if a game looks blurry.",
                 true);
  AddBoolSetting(gpu.items, "gpu_3d_to_2d_texture", "Treat 3D Textures as 2D",
                 "Fixes textures in some games. Leave on unless a game breaks.", true);
  AddBoolSetting(gpu.items, "gpu_allow_invalid_fetch_constants", "Allow Broken Textures",
                 "Can get a game to boot or draw, but may cause glitches.", true);
  AddEnumSetting(gpu.items, "readback_resolve", "Readback Resolve",
                 "Some effects need rendered images read back. Slower when on.");
  AddEnumSetting(gpu.items, "occlusion_query", "Occlusion Query",
                 "How the game finds what's hidden. Change if objects pop in or vanish.");
  PushIfNotEmpty(sections, std::move(gpu));

  IOSConfigSection memory;
  memory.title = "Memory & Boot";
  AddBoolSetting(memory.items, "mount_cache", "Cache Drive",
                 "Gives games the cache drive they expect. Turning it off can fix looping "
                 "cutscenes in Halo 3, ODST and Reach.",
                 true);
  AddBoolSetting(memory.items, "clear_memory_page_state", "Reset Memory Tracking",
                 "Clears memory tracking every frame. Try only if a game glitches.", false);
  AddBoolSetting(memory.items, "submit_on_primary_buffer_end", "Submit Work Early",
                 "Sends graphics work in smaller batches. Leave on unless a game breaks.",
                 true);
  PushIfNotEmpty(sections, std::move(memory));

  IOSConfigSection jit;
  jit.title = "Game Code";
  // Keeps host and guest stacks in step across setjmp/longjmp.
  AddBoolSetting(jit.items, "a64_enable_host_guest_stack_synchronization", "Stack Sync",
                 "Prevents crashes in some games at a small speed cost. Leave on.", true);
  PushIfNotEmpty(sections, std::move(jit));

  IOSConfigSection broker;
  broker.title = "JIT Helper";
  broker.footer = "Only matters when a JIT helper like StikDebug stays attached.";
  // If iOS denies a JIT page protection change, BRK into the attached broker so it prepares
  // the region, then retry. 0xF00D is the universal command; 0x69 is the legacy one.
  AddBoolSetting(broker.items, "ios_jit_brk_prepare_fallback", "Ask Helper When Blocked",
                 "If iOS blocks JIT memory, the helper sets it up and the game carries on.",
                 true);
  AddBoolSetting(broker.items, "ios_jit_brk_use_universal_0xf00d", "Current Helper Scripts",
                 "Turn off only for an old script that predates the universal command.",
                 true);
  PushIfNotEmpty(sections, std::move(broker));
  return sections;
}

static std::vector<IOSConfigSection> BuildSystemSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection automation;
  automation.title = "Automation";
  AddUserDefaultBoolSetting(
      automation.items, kXeniaAutoOpenStikDebugOnLaunchPreferenceKey,
      "Auto-Enable JIT via StikDebug",
      "Opens StikDebug when XeniOS starts, to turn on JIT and come back. Needs StikDebug set "
      "up with a pairing file.",
      false);
  PushIfNotEmpty(sections, std::move(automation));


  return sections;
}

static std::vector<IOSConfigSection> BuildDiagnosticsSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection diagnostics;
  diagnostics.title = "Logging";
  AddChoiceSetting(diagnostics.items, IOSConfigControlType::kChoiceInt32, "log_level",
                   "Log Detail",
                   "How much XeniOS writes to its log. More detail helps track down a problem "
                   "but can slow games down.",
                   2, {{"Errors Only", 0}, {"Warnings", 1}, {"Info", 2}, {"Debug", 3}});
  AddActionSetting(diagnostics.items, IOSConfigAction::kViewRecentLog, "View Log",
                   "Shows the log as it is written, to see why a game won't start.");
  PushIfNotEmpty(sections, std::move(diagnostics));
  return sections;
}

// Shown at the bottom of All Settings, where people look for it.
static std::vector<IOSConfigSection> BuildResetSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection reset;
  reset.title = "Reset";
  reset.footer =
      "To change settings for a single game, open Game Settings from that game's tile menu.";
  AddActionSetting(reset.items, IOSConfigAction::kResetAllSettings,
                   "Reset All Settings to Defaults",
                   "Puts every setting back to its default. Games, saves, profiles, "
                   "per-game overrides, the shader compile indicator, Online Features and "
                   "Share Diagnostics are kept.");
  PushIfNotEmpty(sections, std::move(reset));
  return sections;
}

static std::vector<IOSConfigSection> BuildGraphicsCompatSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection compat;
  compat.title = "Graphics Compatibility";
  compat.footer =
      "Live overrides for the running game. In-game changes are saved as title-specific "
      "overrides.";

  AddEnumSetting(compat.items, "readback_resolve", "Readback Resolve",
                 "Controls CPU readback of render-to-texture resolve results.");
  AddEnumSetting(compat.items, "occlusion_query", "Occlusion Query",
                 "Selects the occlusion query implementation.");
  AddBoolSetting(compat.items, "half_pixel_offset", "Half-Pixel Offset",
                 "D3D9-style sampling behavior used by most Xbox 360 rendering.", true);
  AddBoolSetting(compat.items, "gpu_3d_to_2d_texture", "Treat 3D Textures as 2D",
                 "Compatibility workaround for titles that sample 3D textures as 2D.", true);
  AddBoolSetting(compat.items, "gpu_allow_invalid_fetch_constants",
                 "Allow Invalid Fetch Constants",
                 "Allow invalid shader fetch constants to pass through.", true);
  AddBoolSetting(compat.items, "readback_memexport", "Readback Memexport",
                 "Allow CPU readback of shader memory export data.", true);
  AddBoolSetting(compat.items, "readback_memexport_fast", "Fast Readback Memexport",
                 "Use delayed readback for lower synchronization cost.", true);

  PushIfNotEmpty(sections, std::move(compat));
  return sections;
}

// "occlusion_query_fake_lower_threshold" -> "Occlusion Query Fake Lower Threshold".
// A title-case name for a cvar with no curated title: "gpu_3d_to_2d_texture" reads
// "GPU 3D to 2D Texture". Abbreviations keep their capitals and short joining words stay lower case.
static std::string ReadableTitleForKey(const std::string& key) {
  static const std::unordered_map<std::string, std::string> kSpecialWords = {
      {"2d", "2D"},       {"3d", "3D"},     {"apu", "APU"},   {"cpu", "CPU"},
      {"d3d9", "D3D9"},   {"dxbc", "DXBC"}, {"edram", "EDRAM"}, {"fsi", "FSI"},
      {"gpu", "GPU"},     {"hz", "Hz"},     {"id", "ID"},     {"jit", "JIT"},
      {"msaa", "MSAA"},   {"mrt", "MRT"},   {"pm4", "PM4"},   {"psi", "PSI"},
      {"rov", "ROV"},     {"rt", "RT"},     {"srgb", "sRGB"}, {"ui", "UI"},
      {"vs", "VS"},       {"xma", "XMA"},   {"vfs", "VFS"},   {"hid", "HID"},
  };
  static const std::unordered_set<std::string> kJoiningWords = {
      "a", "and", "as", "at", "by", "for", "in", "of", "on", "or", "the", "to", "with"};
  std::string title;
  size_t start = 0;
  while (start <= key.size()) {
    size_t end = key.find('_', start);
    if (end == std::string::npos) {
      end = key.size();
    }
    std::string word = key.substr(start, end - start);
    start = end + 1;
    if (word.empty()) {
      continue;
    }
    if (!title.empty()) {
      title.push_back(' ');
    }
    auto special = kSpecialWords.find(word);
    if (special != kSpecialWords.end()) {
      title += special->second;
    } else if (!title.empty() && kJoiningWords.count(word)) {
      title += word;
    } else {
      word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
      title += word;
    }
  }
  return title;
}

static std::vector<IOSConfigSection> BuildDebugSettingsSectionsRaw();

std::vector<IOSConfigSection> BuildDebugSettingsSections() {
  std::vector<IOSConfigSection> sections = BuildDebugSettingsSectionsRaw();
  for (IOSConfigSection& section : sections) {
    for (IOSConfigItem& item : section.items) {
      if (item.title == item.key) {
        item.title = ReadableTitleForKey(item.key);
      }
    }
  }
  return sections;
}

static std::vector<IOSConfigSection> BuildDebugSettingsSectionsRaw() {
  std::vector<IOSConfigSection> sections;

  IOSConfigSection common;
  common.title = "Common Overrides";
  AddIntegerSetting(common.items, "anisotropic_override", "anisotropic_override",
                    "Texture anisotropic filtering override (-1 = no override).");
  AddBoolSetting(common.items, "gpu_allow_invalid_fetch_constants",
                 "gpu_allow_invalid_fetch_constants",
                 "Allow invalid shader fetch constants to pass through.", true);
  AddBoolSetting(common.items, "gpu_3d_to_2d_texture", "gpu_3d_to_2d_texture",
                 "Treat 3D texture sampling as 2D for compatibility.", true);
  AddBoolSetting(common.items, "half_pixel_offset", "half_pixel_offset",
                 "D3D9-style half-pixel offset behavior.", true);
  AddBoolSetting(common.items, "submit_on_primary_buffer_end",
                 "submit_on_primary_buffer_end",
                 "Submit command buffers at primary buffer end boundaries.", true);
  AddIntegerSetting(common.items, "occlusion_query_fake_lower_threshold",
                    "occlusion_query_fake_lower_threshold",
                    "Lower fake occlusion query threshold.");
  AddIntegerSetting(common.items, "occlusion_query_fake_upper_threshold",
                    "occlusion_query_fake_upper_threshold",
                    "Upper fake occlusion query threshold.");
  AddDoubleSetting(common.items, "occlusion_query_sample_count_saturation",
                   "occlusion_query_sample_count_saturation",
                   "Compress occlusion query sample counts before guest writeback.");
  PushIfNotEmpty(sections, std::move(common));

  IOSConfigSection presentation;
  presentation.title = "Presentation / Display";
  AddBoolSetting(presentation.items, "present_letterbox", "present_letterbox",
                 "Maintain guest aspect ratio with letterboxing.", true);
  PushIfNotEmpty(sections, std::move(presentation));

  IOSConfigSection scaling;
  scaling.title = "Resolution Scaling / Resolve";
  AddBoolSetting(scaling.items, "draw_resolution_scaled_texture_offsets",
                 "draw_resolution_scaled_texture_offsets",
                 "Adjust texture offsets when draw resolution scaling is active.", true);
  AddBoolSetting(scaling.items, "readback_resolve_half_pixel_offset",
                 "readback_resolve_half_pixel_offset",
                 "Use centered sampling for scaled readback resolves.", false);
  AddBoolSetting(scaling.items, "resolve_resolution_scale_fill_half_pixel_offset",
                 "resolve_resolution_scale_fill_half_pixel_offset",
                 "Fill half-pixel offset when resolution scale changes.", true);
  PushIfNotEmpty(sections, std::move(scaling));

  IOSConfigSection shader;
  shader.title = "Shader / Driver Workarounds";
  AddBoolSetting(shader.items, "use_fuzzy_alpha_epsilon", "use_fuzzy_alpha_epsilon",
                 "Use fuzzy alpha epsilon behavior for shader output.", false);
  AddBoolSetting(shader.items, "vulkan_precise_interpolation",
                 "vulkan_precise_interpolation",
                 "Use precise interpolation on Vulkan/MoltenVK backends.", false);
  AddBoolSetting(shader.items, "dxbc_switch", "dxbc_switch",
                 "D3D12-only shader translator switch workaround.", true);
  PushIfNotEmpty(sections, std::move(shader));

  IOSConfigSection edram;
  edram.title = "EDRAM / Draw Heuristics";
  AddBoolSetting(edram.items, "execute_unclipped_draw_vs_on_cpu",
                 "execute_unclipped_draw_vs_on_cpu",
                 "Execute unclipped draw vertex shader work on the CPU.", true);
  AddBoolSetting(edram.items, "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
                 "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
                 "CPU unclipped-draw path for pixel-shader interlock render backends.", true);
  AddBoolSetting(edram.items, "execute_unclipped_draw_vs_on_cpu_with_scissor",
                 "execute_unclipped_draw_vs_on_cpu_with_scissor",
                 "Use CPU unclipped-draw handling with scissor state.", false);
  AddBoolSetting(edram.items, "mrt_edram_used_range_clamp_to_min",
                 "mrt_edram_used_range_clamp_to_min",
                 "Clamp MRT EDRAM used range to minimum bounds.", true);
  PushIfNotEmpty(sections, std::move(edram));

  IOSConfigSection depth;
  depth.title = "Depth / Precision";
  AddBoolSetting(depth.items, "depth_float24_convert_in_pixel_shader",
                 "depth_float24_convert_in_pixel_shader",
                 "Convert float24 depth values in pixel shaders.", false);
  AddBoolSetting(depth.items, "depth_float24_round", "depth_float24_round",
                 "Round float24 depth values.", false);
  AddBoolSetting(depth.items, "depth_transfer_not_equal_test",
                 "depth_transfer_not_equal_test",
                 "Use not-equal testing for depth transfers.", true);
  PushIfNotEmpty(sections, std::move(depth));

  IOSConfigSection primitive;
  primitive.title = "Primitive Conversion";
  AddBoolSetting(primitive.items, "force_convert_triangle_strips_to_lists",
                 "force_convert_triangle_strips_to_lists",
                 "Force triangle strips to triangle lists.", false);
  AddBoolSetting(primitive.items, "force_convert_triangle_fans_to_lists",
                 "force_convert_triangle_fans_to_lists",
                 "Force triangle fans to triangle lists.", false);
  AddBoolSetting(primitive.items, "force_convert_quad_lists_to_triangle_lists",
                 "force_convert_quad_lists_to_triangle_lists",
                 "Force quad lists to triangle lists.", false);
  AddBoolSetting(primitive.items, "force_convert_line_loops_to_strips",
                 "force_convert_line_loops_to_strips",
                 "Force line loops to line strips.", false);
  PushIfNotEmpty(sections, std::move(primitive));

  IOSConfigSection memory;
  memory.title = "Memory / Boot Hacks";
  AddBoolSetting(memory.items, "scribble_heap", "scribble_heap",
                 "Scribble allocated heap memory for debugging.", false);
  AddIntegerSetting(memory.items, "scribble_heap_value", "scribble_heap_value",
                    "Scribble byte value used when heap scribbling is enabled.");
  PushIfNotEmpty(sections, std::move(memory));

  IOSConfigSection logging;
  logging.title = "Diagnostics / Logging";
  AddIntegerSetting(logging.items, "log_level", "log_level", "Log verbosity level.");
  AddIntegerSetting(logging.items, "log_mask", "log_mask", "Log category mask.");
  AddBoolSetting(logging.items, "occlusion_query_log", "occlusion_query_log",
                 "Log occlusion query lifetime and summary stats.", false);
  AddBoolSetting(logging.items, "gpu_debug_markers", "gpu_debug_markers",
                 "Insert GPU debug markers for capture tools.", false);
  AddBoolSetting(logging.items, "disassemble_pm4", "disassemble_pm4",
                 "Disassemble PM4 packets in debug builds.", false);
  AddBoolSetting(logging.items, "log_guest_driven_gpu_register_written_values",
                 "log_guest_driven_gpu_register_written_values",
                 "Log guest-written GPU register values in debug builds.", false);
  AddBoolSetting(logging.items, "log_ringbuffer_kickoff_initiator_bts",
                 "log_ringbuffer_kickoff_initiator_bts",
                 "Log the pseudo-stacktrace that kicked the GPU ringbuffer.", false);
  PushIfNotEmpty(sections, std::move(logging));

  return sections;
}

static std::vector<IOSConfigSection> BuildAllIOSConfigSections() {
  std::vector<IOSConfigSection> sections;
  auto append = [&sections](std::vector<IOSConfigSection> more) {
    for (auto& section : more) {
      sections.push_back(std::move(section));
    }
  };
  append(BuildDisplaySections());
  append(BuildGraphicsSections());
  append(BuildAudioSections());
  append(BuildControlsSections());
  append(BuildPerformanceSections());
  append(BuildCompatibilitySections());
  append(BuildSystemSections());
  append(BuildDiagnosticsSections());
  return sections;
}

std::vector<IOSConfigSection> BuildAllCvarSections() {
  std::map<std::string, std::vector<IOSConfigItem>> category_items;

  if (!cvar::ConfigVars) {
    return {};
  }

  for (const auto& [name, var] : *cvar::ConfigVars) {
    if (var->is_transient() || var->category() == "Config" || IOSConfigIsHiddenKey(name) ||
        dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
      continue;
    }

    IOSConfigItem item;
    item.key = name;
    item.title = var->display_name().empty() ? name : var->display_name();
    item.subtitle = var->description();
    item.category = var->category();
    item.is_advanced = var->is_advanced();

    // Infer control type via dynamic_cast cascade.
    if (auto* bv = dynamic_cast<cvar::ConfigVar<bool>*>(var)) {
      item.control_type = IOSConfigControlType::kToggle;
      IOSConfigParseBoolString(TrimAscii(var->config_value()), &item.bool_value);
    } else if (auto* iv = dynamic_cast<cvar::ConfigVar<int32_t>*>(var)) {
      item.control_type = IOSConfigControlType::kInteger;
      int64_t parsed = 0;
      if (IOSConfigParseInt64String(TrimAscii(var->config_value()), &parsed)) {
        item.integer_value = parsed;
      }
    } else if (auto* uv = dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) {
      item.control_type = IOSConfigControlType::kInteger;
      int64_t parsed = 0;
      if (IOSConfigParseInt64String(TrimAscii(var->config_value()), &parsed)) {
        item.integer_value = parsed;
      }
    } else if (auto* u64v = dynamic_cast<cvar::ConfigVar<uint64_t>*>(var)) {
      item.control_type = IOSConfigControlType::kInteger;
      int64_t parsed = 0;
      if (IOSConfigParseInt64String(TrimAscii(var->config_value()), &parsed)) {
        item.integer_value = parsed;
      }
    } else if (auto* i64v = dynamic_cast<cvar::ConfigVar<int64_t>*>(var)) {
      item.control_type = IOSConfigControlType::kInteger;
      int64_t parsed = 0;
      if (IOSConfigParseInt64String(TrimAscii(var->config_value()), &parsed)) {
        item.integer_value = parsed;
      }
    } else if (auto* dv = dynamic_cast<cvar::ConfigVar<double>*>(var)) {
      item.control_type = IOSConfigControlType::kDouble;
      std::string raw = TrimAscii(var->config_value());
      if (!raw.empty()) {
        char* end = nullptr;
        double parsed = std::strtod(raw.c_str(), &end);
        if (end != raw.c_str() && *end == '\0') {
          item.double_value = parsed;
        }
      }
    } else if (auto* fv = dynamic_cast<cvar::ConfigVar<float>*>(var)) {
      item.control_type = IOSConfigControlType::kDouble;
      std::string raw = TrimAscii(var->config_value());
      if (!raw.empty()) {
        char* end = nullptr;
        double parsed = std::strtod(raw.c_str(), &end);
        if (end != raw.c_str() && *end == '\0') {
          item.double_value = parsed;
        }
      }
    } else if (auto* sv = dynamic_cast<cvar::ConfigVar<std::string>*>(var)) {
      const auto& enum_opts = xe::ui::GetKnownEnumOptions();
      auto enum_it = enum_opts.find(name);
      if (enum_it != enum_opts.end() && !enum_it->second.empty()) {
        item.control_type = IOSConfigControlType::kEnum;
        item.enum_key = name;
        item.string_value = IOSConfigGetConfigVarString(name, "");
        FillEnumChoices(&item, enum_it->second);
      } else {
        item.control_type = IOSConfigControlType::kString;
        item.string_value = IOSConfigGetConfigVarString(name, "");
      }
    } else if (dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
      item.control_type = IOSConfigControlType::kPath;
      item.string_value = IOSConfigGetConfigVarString(name, "");
    } else {
      // Unknown type — fall back to string display.
      item.control_type = IOSConfigControlType::kString;
      item.string_value = TrimAscii(var->config_value());
    }

    item.has_default = IOSConfigGetConfigVarDefaultString(name, &item.default_value);
    category_items[item.category].push_back(std::move(item));
  }

  std::vector<IOSConfigSection> sections;
  for (auto& [category, items] : category_items) {
    std::sort(items.begin(), items.end(),
              [](const IOSConfigItem& a, const IOSConfigItem& b) {
                return a.key < b.key;
              });
    IOSConfigSection section;
    section.title = category;
    section.items = std::move(items);
    sections.push_back(std::move(section));
  }
  std::sort(sections.begin(), sections.end(),
            [](const IOSConfigSection& a, const IOSConfigSection& b) {
              return a.title < b.title;
            });

  return sections;
}

static const IOSConfigSection* FindSection(
    const std::vector<IOSConfigSection>& sections, const std::string& title) {
  auto it = std::find_if(sections.begin(), sections.end(),
                         [&](const IOSConfigSection& section) {
                           return section.title == title;
                         });
  return it == sections.end() ? nullptr : &*it;
}

static IOSConfigSection FilterSectionByKeys(const IOSConfigSection& source,
                                            std::vector<std::string> keys) {
  IOSConfigSection section;
  section.title = source.title;
  section.footer = source.footer;
  for (const IOSConfigItem& item : source.items) {
    if (std::find(keys.begin(), keys.end(), item.key) != keys.end()) {
      section.items.push_back(item);
    }
  }
  return section;
}

static void PushFilteredSection(std::vector<IOSConfigSection>& sections,
                                const std::vector<IOSConfigSection>& all_sections,
                                const std::string& title,
                                std::vector<std::string> keys) {
  const IOSConfigSection* source = FindSection(all_sections, title);
  if (!source) {
    return;
  }
  PushIfNotEmpty(sections, FilterSectionByKeys(*source, std::move(keys)));
}

static void PushWholeSection(std::vector<IOSConfigSection>& sections,
                             const std::vector<IOSConfigSection>& all_sections,
                             const std::string& title) {
  const IOSConfigSection* source = FindSection(all_sections, title);
  if (!source) {
    return;
  }
  PushIfNotEmpty(sections, *source);
}

std::string IOSConfigCatalogTitle(IOSConfigCatalogKind kind) {
  switch (kind) {
    case IOSConfigCatalogKind::kDisplay:
      return "Display";
    case IOSConfigCatalogKind::kGraphics:
      return "Graphics";
    case IOSConfigCatalogKind::kAudio:
      return "Audio";
    case IOSConfigCatalogKind::kControls:
      return "Touch Controls";
    case IOSConfigCatalogKind::kPerformance:
      return "Performance";
    case IOSConfigCatalogKind::kCompatibility:
      return "Compatibility";
    case IOSConfigCatalogKind::kAdvanced:
    case IOSConfigCatalogKind::kDebugSettings:
      return "Advanced Debug";
    case IOSConfigCatalogKind::kDiagnostics:
      return "Diagnostics";
    case IOSConfigCatalogKind::kSystem:
      return "Automation";
    case IOSConfigCatalogKind::kPerGame:
      return "Game Settings";
    case IOSConfigCatalogKind::kGraphicsCompat:
      return "Graphics Compatibility";
    case IOSConfigCatalogKind::kAllCvars:
      return "All Config";
    case IOSConfigCatalogKind::kAllSettings:
      return "All Settings";
    case IOSConfigCatalogKind::kMain:
    default:
      return "Settings";
  }
}

// Every curated page, in hub order.
static const IOSConfigCatalogKind kCuratedPages[] = {
    IOSConfigCatalogKind::kDisplay,     IOSConfigCatalogKind::kGraphics,
    IOSConfigCatalogKind::kAudio,       IOSConfigCatalogKind::kControls,
    IOSConfigCatalogKind::kPerformance, IOSConfigCatalogKind::kCompatibility,
    IOSConfigCatalogKind::kSystem,      IOSConfigCatalogKind::kDiagnostics,
};

static std::vector<IOSConfigSection> BuildCuratedPageSections(IOSConfigCatalogKind kind) {
  switch (kind) {
    case IOSConfigCatalogKind::kDisplay:
      return BuildDisplaySections();
    case IOSConfigCatalogKind::kGraphics:
      return BuildGraphicsSections();
    case IOSConfigCatalogKind::kAudio:
      return BuildAudioSections();
    case IOSConfigCatalogKind::kControls:
      return BuildControlsSections();
    case IOSConfigCatalogKind::kPerformance:
      return BuildPerformanceSections();
    case IOSConfigCatalogKind::kCompatibility:
      return BuildCompatibilitySections();
    case IOSConfigCatalogKind::kSystem:
      return BuildSystemSections();
    case IOSConfigCatalogKind::kDiagnostics:
      return BuildDiagnosticsSections();
    default:
      return {};
  }
}

// Every curated page in one list, then everything else. A setting that appears on two pages is
// kept once, on the first.
// Section titles carry their page ("Graphics · Image Quality").
static std::vector<IOSConfigSection> BuildAllSettingsSections() {
  std::vector<IOSConfigSection> result;
  std::set<std::string> seen_keys;
  for (IOSConfigCatalogKind kind : kCuratedPages) {
    const std::string page = IOSConfigCatalogTitle(kind);
    for (IOSConfigSection& section : BuildCuratedPageSections(kind)) {
      std::vector<IOSConfigItem> kept;
      for (IOSConfigItem& item : section.items) {
        if (!item.key.empty() && !seen_keys.insert(item.key).second) {
          continue;
        }
        kept.push_back(std::move(item));
      }
      if (kept.empty()) {
        continue;
      }
      section.items = std::move(kept);
      // "Audio · Output", or just the section's name where it already starts with the page's
      // ("Display" on Display, "Audio" on Audio).
      if (section.title.empty()) {
        section.title = page;
      } else if (section.title.compare(0, page.size(), page) == 0) {
        // Already names the page.
      } else {
        section.title = page + " \xC2\xB7 " + section.title;
      }
      result.push_back(std::move(section));
    }
  }
  // Then the advanced page, and every other registered setting by its category, so the list
  // and its search hold everything the emulator reads from its config.
  auto add_rest = [&](std::vector<IOSConfigSection> sections, const std::string& prefix) {
    for (IOSConfigSection& section : sections) {
      std::vector<IOSConfigItem> kept;
      for (IOSConfigItem& item : section.items) {
        if (!item.key.empty() && !seen_keys.insert(item.key).second) {
          continue;
        }
        kept.push_back(std::move(item));
      }
      if (kept.empty()) {
        continue;
      }
      section.items = std::move(kept);
      section.title = prefix + section.title;
      result.push_back(std::move(section));
    }
  };
  add_rest(BuildDebugSettingsSections(), "Advanced \xC2\xB7 ");
  add_rest(BuildAllCvarSections(), "Emulator \xC2\xB7 ");
  for (IOSConfigSection& section : BuildResetSections()) {
    result.push_back(std::move(section));
  }
  return result;
}

std::vector<IOSConfigSection> BuildIOSConfigSectionsForKind(IOSConfigCatalogKind kind) {
  std::vector<IOSConfigSection> sections;

  switch (kind) {
    case IOSConfigCatalogKind::kDisplay:
    case IOSConfigCatalogKind::kGraphics:
    case IOSConfigCatalogKind::kAudio:
    case IOSConfigCatalogKind::kControls:
    case IOSConfigCatalogKind::kPerformance:
    case IOSConfigCatalogKind::kCompatibility:
    case IOSConfigCatalogKind::kSystem:
    case IOSConfigCatalogKind::kDiagnostics:
      return BuildCuratedPageSections(kind);

    case IOSConfigCatalogKind::kAdvanced:
    case IOSConfigCatalogKind::kDebugSettings:
      return BuildDebugSettingsSections();

    case IOSConfigCatalogKind::kPerGame: {
      // The per-title list: only settings the engine reads after the title's config is loaded
      // at launch (or on every frame), so a saved override takes effect.
      std::vector<IOSConfigSection> all_sections = BuildAllIOSConfigSections();
      PushFilteredSection(sections, all_sections, "Display",
                          {"present_letterbox", "framerate_limit", "guest_display_refresh_cap",
                           "use_50Hz_mode"});
      PushFilteredSection(sections, all_sections, "Shaders",
                          {"async_shader_compilation"});
      PushFilteredSection(sections, all_sections, "Audio Workarounds",
                          {"apu_max_queued_frames", "xma_decoder", "use_dedicated_xma_thread"});
      PushWholeSection(sections, all_sections, "GPU Workarounds");
      PushWholeSection(sections, all_sections, "Memory & Boot");
      PushWholeSection(sections, all_sections, "Game Code");
      PushWholeSection(sections, all_sections, "JIT Helper");
      IOSConfigSection all_config;
      all_config.title = "All Config";
      AddActionSetting(all_config.items, IOSConfigAction::kOpenAllConfigSettings,
                       "All Settings",
                       "Find any setting and change it just for this game.");
      PushIfNotEmpty(sections, std::move(all_config));
      IOSConfigSection reset;
      reset.title = "Reset";
      AddActionSetting(reset.items, IOSConfigAction::kResetGameSettings, "Reset Game Settings",
                       "Remove this game's changes and use your usual settings.");
      PushIfNotEmpty(sections, std::move(reset));
      return sections;
    }

    case IOSConfigCatalogKind::kGraphicsCompat:
      return BuildGraphicsCompatSections();

    case IOSConfigCatalogKind::kAllCvars:
      return BuildAllCvarSections();

    case IOSConfigCatalogKind::kAllSettings:
      return BuildAllSettingsSections();

    case IOSConfigCatalogKind::kMain:
    default: {
      std::vector<IOSConfigSection> all_sections = BuildAllIOSConfigSections();
      PushFilteredSection(sections, all_sections, "Display",
                          {"present_letterbox", "framerate_limit",
                           "guest_display_refresh_cap"});
      PushWholeSection(sections, all_sections, "Automation");
      return sections;
    }
  }
}

std::vector<IOSConfigSection> BuildIOSConfigSections() {
  return BuildIOSConfigSectionsForKind(IOSConfigCatalogKind::kMain);
}

bool IOSConfigKeyAllowsPerGame(const std::string& key) {
  if (key.empty()) {
    return false;
  }
  static const std::set<std::string> keys = []() {
    std::set<std::string> result;
    for (const IOSConfigSection& section :
         BuildIOSConfigSectionsForKind(IOSConfigCatalogKind::kPerGame)) {
      for (const IOSConfigItem& item : section.items) {
        if (!item.key.empty()) {
          result.insert(item.key);
        }
      }
    }
    return result;
  }();
  return keys.count(key) != 0;
}

static std::string FormatDoubleText(double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return buffer;
}

// A stepped value with as many decimals as its step needs and its unit: "0.10 s", "1.2×",
// "4.0 pt". "×" sits against the number, as Apple writes zoom levels.
static std::string FormatSteppedDoubleText(const IOSConfigItem& item, double value) {
  int decimals = 0;
  for (double step = item.step; decimals < 3 && step - std::floor(step) > 1e-9; step *= 10.0) {
    ++decimals;
  }
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  std::string text = buffer;
  if (item.value_unit.empty()) {
    return text;
  }
  return item.value_unit == "\xC3\x97" ? text + item.value_unit
                                        : text + " " + item.value_unit;
}

static std::string OnOffText(bool on) {
  return [(on ? NSLocalizedString(@"On", @"config catalog: toggle value")
              : NSLocalizedString(@"Off", @"config catalog: toggle value")) UTF8String];
}

std::string IOSConfigItemValueText(const IOSConfigItem& item) {
  switch (item.control_type) {
    case IOSConfigControlType::kToggle:
      return OnOffText(item.bool_value);
    case IOSConfigControlType::kInteger:
      return std::to_string(item.integer_value);
    case IOSConfigControlType::kDouble:
      return item.step > 0.0 ? FormatSteppedDoubleText(item, item.double_value)
                             : FormatDoubleText(item.double_value);
    case IOSConfigControlType::kString:
    case IOSConfigControlType::kPath:
      return item.string_value;
    case IOSConfigControlType::kAction:
      return std::string();
    default:
      return ChoiceTitleForItem(item);
  }
}

// The default as the row would show it. False when the default isn't known.
bool IOSConfigItemDefaultText(const IOSConfigItem& item, std::string* text_out) {
  if (!item.has_default || item.control_type == IOSConfigControlType::kAction) {
    return false;
  }
  std::string text = item.default_value;
  switch (item.control_type) {
    case IOSConfigControlType::kToggle: {
      bool value = false;
      if (!IOSConfigParseBoolString(item.default_value, &value)) {
        return false;
      }
      text = OnOffText(value);
    } break;
    case IOSConfigControlType::kChoiceInt32:
    case IOSConfigControlType::kChoiceUInt32:
    case IOSConfigControlType::kChoiceUInt64: {
      int64_t value = 0;
      if (!IOSConfigParseInt64String(item.default_value, &value)) {
        return false;
      }
      for (const IOSConfigChoice& choice : item.choices) {
        if (choice.value == value && !choice.is_custom) {
          text = choice.title;
        }
      }
    } break;
    case IOSConfigControlType::kChoiceString:
    case IOSConfigControlType::kEnum:
      for (size_t i = 0; i < item.choice_string_values.size() && i < item.choices.size(); ++i) {
        if (item.choice_string_values[i] == item.default_value && !item.choices[i].is_custom) {
          text = item.choices[i].title;
        }
      }
      break;
    case IOSConfigControlType::kDouble:
      if (item.step > 0.0) {
        char* end = nullptr;
        double value = std::strtod(item.default_value.c_str(), &end);
        if (end != item.default_value.c_str()) {
          text = FormatSteppedDoubleText(item, value);
        }
      }
      break;
    default:
      break;
  }
  if (text.empty()) {
    text = [NSLocalizedString(@"Empty", @"config catalog: empty default value") UTF8String];
  }
  *text_out = text;
  return true;
}

bool IOSConfigItemIsDefault(const IOSConfigItem& item) {
  if (!item.has_default) {
    return true;
  }
  switch (item.control_type) {
    case IOSConfigControlType::kToggle: {
      bool value = false;
      return !IOSConfigParseBoolString(item.default_value, &value) || value == item.bool_value;
    }
    case IOSConfigControlType::kChoiceInt32:
    case IOSConfigControlType::kChoiceUInt32:
    case IOSConfigControlType::kChoiceUInt64: {
      int64_t value = 0;
      return !IOSConfigParseInt64String(item.default_value, &value) ||
             value == item.choice_value;
    }
    case IOSConfigControlType::kInteger: {
      int64_t value = 0;
      return !IOSConfigParseInt64String(item.default_value, &value) ||
             value == item.integer_value;
    }
    case IOSConfigControlType::kDouble: {
      char* end = nullptr;
      const double value = std::strtod(item.default_value.c_str(), &end);
      return end == item.default_value.c_str() ||
             FormatDoubleText(value) == FormatDoubleText(item.double_value);
    }
    case IOSConfigControlType::kChoiceString:
    case IOSConfigControlType::kEnum:
    case IOSConfigControlType::kString:
    case IOSConfigControlType::kPath:
      return item.string_value == item.default_value;
    case IOSConfigControlType::kAction:
      return true;
  }
  return true;
}

bool IOSConfigResetItemToDefault(IOSConfigItem* item) {
  if (!item || !item->has_default || IOSConfigItemIsDefault(*item)) {
    return false;
  }
  switch (item->control_type) {
    case IOSConfigControlType::kToggle:
      return IOSConfigParseBoolString(item->default_value, &item->bool_value);
    case IOSConfigControlType::kChoiceInt32:
    case IOSConfigControlType::kChoiceUInt32:
    case IOSConfigControlType::kChoiceUInt64: {
      int64_t value = 0;
      if (!IOSConfigParseInt64String(item->default_value, &value)) {
        return false;
      }
      for (const IOSConfigChoice& choice : item->choices) {
        if (choice.value == value && !choice.is_custom) {
          item->choice_value = value;
          return true;
        }
      }
      return false;
    }
    case IOSConfigControlType::kInteger:
      return IOSConfigParseInt64String(item->default_value, &item->integer_value);
    case IOSConfigControlType::kDouble: {
      char* end = nullptr;
      const double value = std::strtod(item->default_value.c_str(), &end);
      if (end == item->default_value.c_str()) {
        return false;
      }
      item->double_value = value;
      return true;
    }
    case IOSConfigControlType::kChoiceString:
    case IOSConfigControlType::kEnum:
      for (size_t i = 0; i < item->choice_string_values.size() && i < item->choices.size(); ++i) {
        if (item->choice_string_values[i] == item->default_value && !item->choices[i].is_custom) {
          item->string_value = item->default_value;
          item->choice_value = static_cast<int64_t>(i);
          return true;
        }
      }
      return false;
    case IOSConfigControlType::kString:
    case IOSConfigControlType::kPath:
      item->string_value = item->default_value;
      return true;
    case IOSConfigControlType::kAction:
      return false;
  }
  return false;
}

std::vector<IOSConfigSearchEntry> BuildIOSConfigSearchIndex() {
  std::vector<IOSConfigSearchEntry> index;
  std::set<std::string> seen_keys;
  std::set<std::string> seen_actions;

  auto add = [&](const IOSConfigItem& item, IOSConfigCatalogKind kind, const std::string& page,
                 const std::string& section, bool advanced) {
    if (item.key.empty()) {
      if (!seen_actions.insert(item.title).second) {
        return;
      }
    } else if (!seen_keys.insert(item.key).second) {
      return;
    }
    IOSConfigSearchEntry entry;
    entry.key = item.key;
    entry.title = item.title;
    entry.subtitle = item.subtitle;
    entry.page = page;
    entry.section = section;
    entry.value_text = IOSConfigItemValueText(item);
    entry.kind = kind;
    entry.value_searchable = item.control_type != IOSConfigControlType::kToggle;
    entry.per_game = IOSConfigKeyAllowsPerGame(item.key);
    entry.advanced = advanced;
    index.push_back(std::move(entry));
  };

  // Each curated setting once (the first page that lists it), then the settings that exist only
  // in the developer pages.
  for (IOSConfigCatalogKind kind : kCuratedPages) {
    const std::string page = IOSConfigCatalogTitle(kind);
    for (const IOSConfigSection& section : BuildCuratedPageSections(kind)) {
      for (const IOSConfigItem& item : section.items) {
        add(item, kind, page, section.title, false);
      }
    }
  }
  for (const IOSConfigSection& section : BuildResetSections()) {
    for (const IOSConfigItem& item : section.items) {
      add(item, IOSConfigCatalogKind::kAllSettings,
          IOSConfigCatalogTitle(IOSConfigCatalogKind::kAllSettings), section.title, false);
    }
  }
  // The rest open in All Settings, which lists every setting.
  const std::string all_settings = IOSConfigCatalogTitle(IOSConfigCatalogKind::kAllSettings);
  for (const IOSConfigSection& section : BuildDebugSettingsSections()) {
    for (const IOSConfigItem& item : section.items) {
      add(item, IOSConfigCatalogKind::kAllSettings, all_settings, "Advanced \xC2\xB7 " + section.title,
          true);
    }
  }
  for (const IOSConfigSection& section : BuildAllCvarSections()) {
    for (const IOSConfigItem& item : section.items) {
      add(item, IOSConfigCatalogKind::kAllSettings, all_settings, "Emulator \xC2\xB7 " + section.title,
          true);
    }
  }
  return index;
}
