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
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
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
static bool IOSHasHeadPoseEntitlement() {
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
  items.push_back(std::move(item));
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
  items.push_back(std::move(item));
}

static void AddChoiceSetting(std::vector<IOSConfigItem>& items, IOSConfigControlType control_type,
                             const std::string& key, const std::string& title,
                             const std::string& subtitle, int64_t fallback,
                             std::vector<IOSConfigChoice> choices) {
  if (!IOSConfigHasConfigVar(key) || choices.empty()) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = control_type;
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
    item.choice_value = item.choices.front().value;
  }
  items.push_back(std::move(item));
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

  items.push_back(std::move(item));
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
  for (size_t i = 0; i < it->second.size(); ++i) {
    item.choices.push_back({it->second[i], static_cast<int64_t>(i)});
    item.choice_string_values.push_back(it->second[i]);
    if (it->second[i] == item.string_value) {
      item.choice_value = static_cast<int64_t>(i);
    }
  }
  items.push_back(std::move(item));
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
  items.push_back(std::move(item));
}

static void AddDoubleSetting(std::vector<IOSConfigItem>& items, const std::string& key,
                             const std::string& title, const std::string& subtitle) {
  if (!IOSConfigHasConfigVar(key)) {
    return;
  }
  IOSConfigItem item;
  item.key = key;
  item.title = title;
  item.subtitle = subtitle;
  item.control_type = IOSConfigControlType::kDouble;
  std::string raw = IOSConfigGetConfigVarString(key, "0");
  if (!raw.empty()) {
    char* end = nullptr;
    double parsed = std::strtod(raw.c_str(), &end);
    if (end != raw.c_str() && *end == '\0') {
      item.double_value = parsed;
    }
  }
  items.push_back(std::move(item));
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
  display.footer = "These settings affect frame pacing and the Metal presenter output.";
  AddBoolSetting(display.items, "metal_presenter_force_10bpc", "Force 10bpc Presenter Output",
                 "Metal-only. Uses RGB10A2 output, which is the default path and usually "
                 "reduces gamma-conversion cost on Apple GPUs. Disable only if colors, "
                 "captures, or display compatibility look wrong.",
                 true);
  AddBoolSetting(display.items, "present_letterbox", "Letterbox",
                 "Maintains the guest display aspect with bars when the presenter surface "
                 "does not match the guest aspect. Disable for aspect-ratio patches that "
                 "should fill the current surface.",
                 true);
  AddChoiceSetting(display.items, IOSConfigControlType::kChoiceUInt64, "framerate_limit",
                   "Frame Rate Limit",
                   "Caps host presentation only; guest timing is separate. Use this to "
                   "reduce heat and battery drain. 120 FPS only matters on high-refresh "
                   "displays.",
                   0,
                   {{"Unlimited", 0},
                    {"30 FPS", 30},
                    {"45 FPS", 45},
                    {"60 FPS", 60},
                    {"90 FPS", 90},
                    {"120 FPS", 120}});
  AddBoolSetting(display.items, "guest_display_refresh_cap", "Cap Guest Display Refresh",
                 "Keeps guest vblank at console timing instead of running as fast as "
                 "possible. Turn this off only for troubleshooting speed or timing-"
                 "sensitive boot issues.",
                 true);
  AddBoolSetting(display.items, "use_50Hz_mode", "Use 50Hz PAL Timing",
                 "Only matters when guest refresh cap is enabled. Required by some PAL "
                 "titles; leave this off for most games to keep normal 60 Hz timing.",
                 false);
  PushIfNotEmpty(sections, std::move(display));
  return sections;
}

static std::vector<IOSConfigSection> BuildGraphicsSections() {
  std::vector<IOSConfigSection> sections;

  IOSConfigSection backend;
  backend.title = "Graphics Backend";
  AddStringChoiceSetting(backend.items, "gpu", "Graphics Backend",
                         "Select the renderer used after the next full relaunch. Metal is the "
                         "primary iOS backend; Vulkan uses the bundled MoltenVK path when "
                         "that backend is compiled into the app.",
                         "metal", {{"Metal", "metal"}, {"Vulkan (MoltenVK)", "vulkan"}});
  AddIntegerSetting(backend.items, "anisotropic_override", "Anisotropic Override",
                    "Override anisotropic filtering level (-1 = auto, 0 = off, 1-5).");
  AddEnumSetting(backend.items, "render_target_path", "Render Target Path",
                 "Selects the render target implementation path after a relaunch. The "
                 "Accuracy path is currently unavailable on Metal.");
  PushIfNotEmpty(sections, std::move(backend));

  IOSConfigSection shader;
  shader.title = "Shader & Pipeline";
  shader.footer = "These settings trade stutter, battery usage, and cache size.";
  AddBoolSetting(shader.items, "store_shaders", "Persistent Shader Cache",
                 "Keeps translated shaders and pipelines on disk so later boots stutter "
                 "less. Disable only if you suspect cache corruption after an update.",
                 true);
  AddBoolSetting(shader.items, "async_shader_compilation", "Async Shader Compilation",
                 "Compiles Metal shaders and pipelines in background threads to reduce "
                 "stutter. New effects may appear a moment late; turn this off if you "
                 "prefer blocking correctness over smoother frame pacing.",
                 false);
  PushIfNotEmpty(sections, std::move(shader));
  return sections;
}

static std::vector<IOSConfigSection> BuildPerformanceSections() {
  std::vector<IOSConfigSection> sections;

  IOSConfigSection caching;
  caching.title = "Caching & Compilation";
  caching.footer = "These settings trade stutter, battery usage, and cache size.";
  AddBoolSetting(caching.items, "store_shaders", "Persistent Shader Cache",
                 "Keeps translated shaders and pipelines on disk so later boots stutter "
                 "less. Disable only if you suspect cache corruption after an update.",
                 true);
  AddBoolSetting(caching.items, "async_shader_compilation", "Async Shader Compilation",
                 "Compiles Metal shaders and pipelines in background threads to reduce "
                 "stutter. New effects may appear a moment late; turn this off if you "
                 "prefer blocking correctness over smoother frame pacing.",
                 false);
  PushIfNotEmpty(sections, std::move(caching));

  IOSConfigSection scheduler;
  scheduler.title = "Thread QoS";
  scheduler.footer = "Xenia's regular iOS threads use Default QoS. The GPU Commands "
                     "promotion is on by default; the others are experimental — leave "
                     "them off unless you are testing CPU or audio scheduling behavior "
                     "after a full relaunch.";
  AddBoolSetting(scheduler.items, "ios_gpu_commands_user_initiated_qos",
                 "GPU Commands User-Initiated QoS",
                 "Runs the Metal command processor host thread at user-initiated QoS. "
                 "Try this first if frames appear to miss submission deadlines.",
                 true);
  AddBoolSetting(scheduler.items, "ios_guest_threads_user_initiated_qos",
                 "Guest Threads User-Initiated QoS",
                 "Runs guest XThreads at user-initiated QoS. This may help CPU-bound "
                 "titles, but it can also compete with audio and rendering work.",
                 false);
  AddBoolSetting(scheduler.items, "ios_emulator_thread_user_initiated_qos",
                 "Emulator Thread User-Initiated QoS",
                 "Runs the high-level emulator setup and launch thread at "
                 "user-initiated QoS. This usually matters less than GPU commands or "
                 "guest threads.",
                 false);
  PushIfNotEmpty(sections, std::move(scheduler));
  return sections;
}

static std::vector<IOSConfigSection> BuildAudioSections() {
  std::vector<IOSConfigSection> sections;

  const bool phase_selected = IOSConfigGetConfigVarString("apu", "phase") == "phase";
  const bool head_pose_entitlement = IOSHasHeadPoseEntitlement();

  IOSConfigSection audio;
  audio.title = "Audio";
  audio.footer = "These settings control the audio backend, mute state and XMA decoding "
                 "behavior.";
  // Surface the head-pose entitlement status when PHASE is in use, since head
  // tracking silently falls back to a fixed listener without it.
  if (phase_selected && !head_pose_entitlement) {
    audio.footer =
        "PHASE backend selected. Head tracking is UNAVAILABLE: the head-pose entitlement "
        "(com.apple.developer.coremotion.head-pose) is not present in this build's code "
        "signature, so AirPods head tracking can't run — spatial audio still works with a "
        "fixed listener. Re-sign with a profile that grants that entitlement to enable it.";
  }
  AddStringChoiceSetting(audio.items, "apu", "Audio Backend",
                         "Select the audio output backend, applied after the next full "
                         "relaunch. SDL is the stereo path. PHASE renders the game's 5.1 "
                         "mix as spatial audio over headphones (Apple PHASE engine); "
                         "requires compatible headphones for the full effect.",
                         "phase", {{"SDL (Stereo)", "sdl"}, {"PHASE (Spatial)", "phase"}});
  std::string head_tracking_subtitle =
      "Only affects the PHASE backend. Tracks compatible AirPods head motion so the sound "
      "stage stays anchored to the screen as you turn your head. Applied after a full "
      "relaunch.";
  if (!head_pose_entitlement) {
    head_tracking_subtitle +=
        "  \xE2\x9A\xA0\xEF\xB8\x8F Head-pose entitlement NOT detected in this build — head "
        "tracking will not run (PHASE keeps a fixed listener).";
  }
  AddBoolSetting(audio.items, "apu_phase_head_tracking", "PHASE Head Tracking",
                 head_tracking_subtitle, false);
  AddBoolSetting(audio.items, "mute", "Mute Audio",
                 "Immediately silences all emulator audio. Useful for background testing "
                 "or silent repro runs.",
                 false);
  AddChoiceSetting(audio.items, IOSConfigControlType::kChoiceUInt32, "apu_max_queued_frames",
                   "Audio Queue Depth",
                   "Larger queues reduce underruns at the cost of latency. Use 48 or 64 "
                   "only if audio still stutters after a full relaunch.",
                   32, {{"32 Frames", 32}, {"48 Frames", 48}, {"64 Frames", 64}});
  AddStringChoiceSetting(audio.items, "xma_decoder", "XMA Decoder",
                         "Select the XMA decoder implementation. New is the current general "
                         "default; Old and Master are fallback paths for regressions, and Fake "
                         "disables XMA decode entirely.",
                         "new",
                         {{"New (Recommended)", "new"},
                          {"Old", "old"},
                          {"Master", "master"},
                          {"Fake (No XMA Audio)", "fake"}});
  AddBoolSetting(audio.items, "use_dedicated_xma_thread", "Dedicated XMA Thread",
                 "Runs XMA decode work on a separate thread. On arm64 this is off by "
                 "default; enable only if audio stutters or decode work blocks the title, "
                 "since timing can change.",
                 false);
  PushIfNotEmpty(sections, std::move(audio));
  return sections;
}

static std::vector<IOSConfigSection> BuildControlsSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection touch;
  touch.title = "Touch Behavior";
  touch.footer = "Lower look points increase camera sensitivity. Hold timings help "
                 "low-FPS games observe short touch input pulses.";
  AddBoolSetting(touch.items, "ios_touch_haptics", "Touch Haptics",
                 "Play haptic feedback on touch button presses, control selection, and "
                 "snap engagement.",
                 true);
  AddBoolSetting(touch.items, "ios_touch_overlay", "Show Touch Overlay",
                 "Show on-screen touch controls during gameplay when no hardware "
                 "controller is connected.",
                 true);
  AddDoubleSetting(touch.items, "ios_touch_look_points_per_full_scale",
                   "Look Points Per Full Scale",
                   "Swipe distance needed to drive the touch look zone to full stick output.");
  AddDoubleSetting(touch.items, "ios_touch_look_vertical_scale", "Look Vertical Scale",
                   "Extra multiplier for vertical swipe-look motion.");
  AddDoubleSetting(touch.items, "ios_touch_look_hold_seconds", "Look Hold Seconds",
                   "How long touch look motion persists for low-FPS polling.");
  AddDoubleSetting(touch.items, "ios_touch_button_tap_hold_seconds", "Button Tap Hold Seconds",
                   "How long tap-style touch buttons stay active for low-FPS polling.");
  PushIfNotEmpty(sections, std::move(touch));
  return sections;
}

static std::vector<IOSConfigSection> BuildCompatibilitySections() {
  std::vector<IOSConfigSection> sections;

  IOSConfigSection gpu;
  gpu.title = "GPU Workarounds";
  gpu.footer = "Leave these at their defaults unless a specific title needs them.";
  AddBoolSetting(gpu.items, "half_pixel_offset", "Half-Pixel Offset",
                 "D3D9-style sampling behavior. Keep this on for correct post-processing "
                 "and UI in most games; disable only if a specific title shows blurred UI "
                 "or edge artifacts.",
                 true);
  AddBoolSetting(gpu.items, "gpu_3d_to_2d_texture", "Treat 3D Textures as 2D",
                 "Compatibility workaround for titles that incorrectly sample 3D textures "
                 "as 2D. Keep this on unless it causes a specific regression.",
                 true);
  AddBoolSetting(gpu.items, "gpu_allow_invalid_fetch_constants",
                 "Allow Invalid Fetch Constants",
                 "Unsafe workaround for titles with broken texture or vertex fetch "
                 "metadata. This can help a game boot or draw, but it may also introduce "
                 "corruption or hide a deeper bug.",
                 true);
  AddEnumSetting(gpu.items, "readback_resolve", "Readback Resolve",
                 "Controls CPU readback of render-to-texture resolve results.");
  AddEnumSetting(gpu.items, "occlusion_query", "Occlusion Query",
                 "Selects the occlusion query implementation.");
  AddEnumSetting(gpu.items, "render_target_path", "Render Target Path",
                 "Selects the render target implementation path. The Accuracy path is "
                 "currently unavailable on Metal.");
  PushIfNotEmpty(sections, std::move(gpu));

  IOSConfigSection memory;
  memory.title = "Memory & Boot";
  AddBoolSetting(memory.items, "mount_cache", "Mount Cache",
                 "Mounts the Xbox cache partition for titles that expect it. Keep "
                 "this on for normal behavior, but disabling it may fix cutscene "
                 "loop issues in games like Halo 3, ODST, Reach, etc.",
                 true);
  AddBoolSetting(memory.items, "clear_memory_page_state", "Clear Memory Page State",
                 "Clear memory page tracking state between frames.", true);
  AddBoolSetting(memory.items, "submit_on_primary_buffer_end", "Submit on Primary Buffer End",
                 "Submit command buffers at primary buffer end boundaries.", true);
  PushIfNotEmpty(sections, std::move(memory));

  IOSConfigSection jit;
  jit.title = "JIT & Codegen";
  AddBoolSetting(jit.items, "a64_enable_host_guest_stack_synchronization",
                 "A64 Stack Synchronization",
                 "ARM64-only compatibility path that keeps host and guest stacks "
                 "synchronized across calls. Leave this off unless a game specifically "
                 "needs it to boot or unwind correctly.",
                 false);
  AddBoolSetting(jit.items, "ios_jit_brk_prepare_fallback",
                 "External JIT Prepare Fallback",
                 "iOS ARM64 only. If iOS denies JIT page protection changes, ask an "
                 "external broker or helper to prepare the region and retry. This is only "
                 "useful on TXM or broker setups; otherwise it is unnecessary.",
                 true);
  AddBoolSetting(jit.items, "ios_jit_brk_use_universal_0xf00d",
                 "Universal 0xF00D JIT Breakpoint",
                 "Use the modern universal BRK command for the external JIT broker. Keep "
                 "this on for current broker scripts; disable it only if you are using an "
                 "older legacy 0x69-only setup. This only matters when External JIT "
                 "Prepare Fallback is enabled.",
                 true);
  PushIfNotEmpty(sections, std::move(jit));
  return sections;
}

static std::vector<IOSConfigSection> BuildSystemSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection automation;
  automation.title = "Automation";
  automation.footer =
      "These options are stored locally in the iOS frontend rather than xenios.config.toml.";
  AddUserDefaultBoolSetting(
      automation.items, kXeniaAutoOpenStikDebugOnLaunchPreferenceKey,
      "Auto-Enable JIT via StikDebug",
      "On app open, jump into StikDebug with XeniOS's bundle ID so it can enable JIT and "
      "relaunch XeniOS. Requires StikDebug, a valid pairing file, and your normal VPN / loopback "
      "setup.",
      false);
  PushIfNotEmpty(sections, std::move(automation));

  IOSConfigSection library;
  library.title = "Library";
  library.footer =
      "Link folders from Files, USB drives, or network shares to play games without copying "
      "them into XeniOS.";
  AddActionSetting(library.items, IOSConfigAction::kManageExternalFolders,
                   "Manage External Folders",
                   "Review, add, or unlink the external folders scanned for games.");
  PushIfNotEmpty(sections, std::move(library));

  return sections;
}

static std::vector<IOSConfigSection> BuildDiagnosticsSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection diagnostics;
  diagnostics.title = "Logging";
  AddChoiceSetting(diagnostics.items, IOSConfigControlType::kChoiceInt32, "log_level",
                   "Log Verbosity",
                   "Controls how much goes into xenia.log. Higher levels help debug issues "
                   "but increase log size and background overhead.",
                   1, {{"Errors Only", 0}, {"Warnings", 1}, {"Info", 2}, {"Debug", 3}});
  AddActionSetting(diagnostics.items, IOSConfigAction::kViewRecentLog, "View Live Log",
                   "Open a live-updating xenia.log viewer so you can capture boot failures "
                   "without Xcode.");
  PushIfNotEmpty(sections, std::move(diagnostics));

  return sections;
}

static std::vector<IOSConfigSection> BuildGraphicsCompatSections() {
  std::vector<IOSConfigSection> sections;
  IOSConfigSection compat;
  compat.title = "Graphics Compatibility";
  compat.footer =
      "Live overrides for the running game. Use Compatibility or Per-Game Settings for saved "
      "config changes.";

  AddEnumSetting(compat.items, "readback_resolve", "Readback Resolve",
                 "Controls CPU readback of render-to-texture resolve results.");
  AddEnumSetting(compat.items, "occlusion_query", "Occlusion Query",
                 "Selects the occlusion query implementation.");
  AddEnumSetting(compat.items, "render_target_path", "Render Target Path",
                 "Selects the render target implementation path. The Accuracy path is "
                 "currently unavailable on Metal.");
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

std::vector<IOSConfigSection> BuildDebugSettingsSections() {
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
  AddDoubleSetting(common.items, "occlusion_query_saturation",
                   "occlusion_query_saturation",
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
                 "Adjust texture offsets when draw resolution scaling is active.", false);
  AddBoolSetting(scaling.items, "readback_resolve_half_pixel_offset",
                 "readback_resolve_half_pixel_offset",
                 "Use centered sampling for scaled readback resolves.", false);
  AddBoolSetting(scaling.items, "resolve_resolution_scale_fill_half_pixel_offset",
                 "resolve_resolution_scale_fill_half_pixel_offset",
                 "Fill half-pixel offset when resolution scale changes.", false);
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
                 "Execute unclipped draw vertex shader work on the CPU.", false);
  AddBoolSetting(edram.items, "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
                 "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
                 "CPU unclipped-draw path for pixel-shader interlock render backends.", false);
  AddBoolSetting(edram.items, "execute_unclipped_draw_vs_on_cpu_with_scissor",
                 "execute_unclipped_draw_vs_on_cpu_with_scissor",
                 "Use CPU unclipped-draw handling with scissor state.", false);
  AddBoolSetting(edram.items, "mrt_edram_used_range_clamp_to_min",
                 "mrt_edram_used_range_clamp_to_min",
                 "Clamp MRT EDRAM used range to minimum bounds.", false);
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
                 "Use not-equal testing for depth transfers.", false);
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
    if (var->is_transient() || var->category() == "Config") {
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
        for (size_t i = 0; i < enum_it->second.size(); ++i) {
          item.choices.push_back({enum_it->second[i], static_cast<int64_t>(i)});
          item.choice_string_values.push_back(enum_it->second[i]);
          if (enum_it->second[i] == item.string_value) {
            item.choice_value = static_cast<int64_t>(i);
          }
        }
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
      return "Touch Behavior";
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
      return "All Config Settings";
    case IOSConfigCatalogKind::kMain:
    default:
      return "Settings";
  }
}

std::vector<IOSConfigSection> BuildIOSConfigSectionsForKind(IOSConfigCatalogKind kind) {
  std::vector<IOSConfigSection> all_sections = BuildAllIOSConfigSections();
  std::vector<IOSConfigSection> sections;

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

    case IOSConfigCatalogKind::kAdvanced:
    case IOSConfigCatalogKind::kDebugSettings:
      return BuildDebugSettingsSections();

    case IOSConfigCatalogKind::kDiagnostics:
      return BuildDiagnosticsSections();

    case IOSConfigCatalogKind::kSystem:
      return BuildSystemSections();

    case IOSConfigCatalogKind::kPerGame:
      {
        IOSConfigSection more;
        more.title = "Search Overrides";
        AddActionSetting(more.items, IOSConfigAction::kOpenAllConfigSettings,
                         "Search All Config",
                         "Find any non-transient cvar and save it as a title-specific override.");
        AddActionSetting(more.items, IOSConfigAction::kResetGameSettings,
                         "Reset Game Settings",
                         "Delete saved overrides for this title and return to defaults.");
        PushIfNotEmpty(sections, std::move(more));
      }
      PushFilteredSection(sections, all_sections, "Display",
                          {"present_letterbox", "guest_display_refresh_cap", "use_50Hz_mode"});
      PushFilteredSection(sections, all_sections, "Graphics Backend", {"gpu"});
      PushWholeSection(sections, all_sections, "GPU Workarounds");
      PushWholeSection(sections, all_sections, "Memory & Boot");
      PushWholeSection(sections, all_sections, "JIT & Codegen");
      return sections;

    case IOSConfigCatalogKind::kGraphicsCompat:
      return BuildGraphicsCompatSections();

    case IOSConfigCatalogKind::kAllCvars:
      return BuildAllCvarSections();

    case IOSConfigCatalogKind::kMain:
    default:
      PushFilteredSection(sections, all_sections, "Display",
                          {"present_letterbox", "framerate_limit",
                           "guest_display_refresh_cap"});
      PushFilteredSection(sections, all_sections, "Audio",
                          {"mute"});
      PushWholeSection(sections, all_sections, "Automation");
      return sections;
  }
}

std::vector<IOSConfigSection> BuildIOSConfigSections() {
  return BuildIOSConfigSectionsForKind(IOSConfigCatalogKind::kMain);
}
