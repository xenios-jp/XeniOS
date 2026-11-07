/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CONFIG_MODELS_H_
#define XENIA_UI_IOS_CONFIG_MODELS_H_

#include <cstdint>
#include <string>
#include <vector>

// Plain-data view models that the iOS settings sheet
// (XeniaConfigViewController) builds at runtime from xenia cvars and
// NSUserDefaults, and that the choice list sheet
// (XeniaChoiceListViewController) consumes when picking a value.
//
// Kept here in one header so the two sheets and any future config-related
// view controllers reference the same type definitions.

enum class IOSConfigControlType {
  kToggle,
  kChoiceInt32,
  kChoiceUInt32,
  kChoiceUInt64,
  kChoiceString,
  kAction,
  kInteger,
  kDouble,
  kString,
  kPath,
  kEnum,
};

enum class IOSConfigStorage {
  kConfigVar,
  kUserDefaults,
};

enum class IOSConfigCatalogKind {
  kMain,
  kDisplay,
  kGraphics,
  kAudio,
  kControls,
  kPerformance,
  kCompatibility,
  kAdvanced,
  kDebugSettings,
  kDiagnostics,
  kSystem,
  kPerGame,
  kGraphicsCompat,
  kAllCvars,
  // Every curated setting in one list, de-duplicated by key. Visible to everyone.
  kAllSettings,
  // Rooms and voice chat.
  kOnline,
};

enum class IOSConfigAction {
  kNone,
  kOpenAdvancedSettings,
  kOpenDiagnosticsSettings,
  kOpenAllConfigSettings,
  kViewRecentLog,
  kResetGameSettings,
  kResetAllSettings,
  kManageExternalFolders,
  // Settings → Audio → Microphone, the voice chat choice.
  kOpenMicrophone,
};

struct IOSConfigChoice {
  std::string title;
  int64_t value = 0;
  // A value the list doesn't offer (hand-edited config, older build), kept so it shows as it is.
  // It marks the current value and can't be picked.
  bool is_custom = false;
};

struct IOSConfigItem {
  std::string key;
  std::string title;
  std::string subtitle;
  IOSConfigControlType control_type = IOSConfigControlType::kToggle;
  IOSConfigStorage storage = IOSConfigStorage::kConfigVar;
  bool bool_value = false;
  int64_t choice_value = 0;
  int64_t integer_value = 0;
  double double_value = 0.0;
  std::string string_value;
  IOSConfigAction action = IOSConfigAction::kNone;
  std::vector<IOSConfigChoice> choices;
  std::vector<std::string> choice_string_values;
  std::string category;
  bool is_advanced = false;
  std::string enum_key;
  // The cvar's default (or the frontend default), as text; |has_default| is false when unknown.
  bool has_default = false;
  std::string default_value;
  // Unit shown after a custom number, e.g. "FPS" ("40 FPS (custom)"), or after a stepped one.
  std::string value_unit;
  // A decimal setting with |step| > 0 shows its value with |value_unit| and a stepper over
  // [|min_value|, |max_value|] instead of a text field.
  double step = 0.0;
  double min_value = 0.0;
  double max_value = 0.0;
  // With both set, a stepped setting shows a slider between these words ("Slower", "Faster").
  std::string min_label;
  std::string max_label;
};

struct IOSConfigSection {
  std::string title;
  std::string footer;
  std::vector<IOSConfigItem> items;
};

#endif  // XENIA_UI_IOS_CONFIG_MODELS_H_
