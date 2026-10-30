/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/settings/ios_config_storage.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <utility>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"

#import "xenia/ui/ios/shared/ios_view_helpers.h"

namespace {

std::string TrimAscii(std::string value) {
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

NSTimeInterval GetUnixTimeSeconds() { return [[NSDate date] timeIntervalSince1970]; }

NSString* const kXeniaPendingExternalLaunchPathPreferenceKey = @"ios_pending_external_launch_path";
NSString* const kXeniaPendingExternalLaunchTimestampPreferenceKey =
    @"ios_pending_external_launch_timestamp";

constexpr NSTimeInterval kXeniaPendingExternalLaunchTTLSeconds = 120.0;

}  // namespace

NSString* const kXeniaAutoOpenStikDebugOnLaunchPreferenceKey = @"ios_auto_open_stikdebug_on_launch";
NSString* const kXeniaLastAutoStikDebugAttemptTimestampPreferenceKey =
    @"ios_last_auto_stikdebug_attempt_timestamp";

static cvar::IConfigVar* GetConfigVar(const std::string& key) {
  if (!cvar::ConfigVars) {
    return nullptr;
  }
  auto it = cvar::ConfigVars->find(key);
  if (it == cvar::ConfigVars->end()) {
    return nullptr;
  }
  return it->second;
}

bool IOSConfigHasConfigVar(const std::string& key) { return GetConfigVar(key) != nullptr; }

std::string StripTomlQuotes(std::string value) {
  value = TrimAscii(std::move(value));
  if (value.size() >= 6 &&
      ((value.rfind("\"\"\"", 0) == 0 && value.compare(value.size() - 3, 3, "\"\"\"") == 0) ||
       (value.rfind("'''", 0) == 0 && value.compare(value.size() - 3, 3, "'''") == 0))) {
    value = value.substr(3, value.size() - 6);
    if (!value.empty() && value.front() == '\n') {
      value.erase(0, 1);
    }
  } else if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                   (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::string IOSConfigDecodeTomlStringValue(const std::string& value) {
  std::string trimmed = TrimAscii(value);
  if (trimmed.empty()) {
    return std::string();
  }

  try {
    toml::parse_result parsed = toml::parse("value = " + trimmed);
    if (const toml::value<std::string>* string_value = parsed["value"].as_string()) {
      return string_value->get();
    }
  } catch (const toml::parse_error&) {
  }

  return StripTomlQuotes(std::move(trimmed));
}

std::string IOSConfigNormalizeEditableStringLikeValue(const std::string& value) {
  return IOSConfigDecodeTomlStringValue(value);
}

std::string IOSConfigGetConfigVarString(const std::string& key, const std::string& fallback) {
  cvar::IConfigVar* var = GetConfigVar(key);
  if (!var) {
    return fallback;
  }
  return IOSConfigDecodeTomlStringValue(var->config_value());
}

bool IOSConfigParseBoolString(const std::string& text, bool* value_out) {
  if (!value_out) {
    return false;
  }
  std::string lower = text;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower == "true" || lower == "1") {
    *value_out = true;
    return true;
  }
  if (lower == "false" || lower == "0") {
    *value_out = false;
    return true;
  }
  return false;
}

bool IOSConfigParseInt64String(const std::string& text, int64_t* value_out) {
  if (!value_out) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  long long parsed = std::strtoll(text.c_str(), &end, 10);
  if (errno != 0 || !end || *end != '\0') {
    return false;
  }
  *value_out = static_cast<int64_t>(parsed);
  return true;
}

double ParseDoubleFallback(const std::string& text, double fallback) {
  if (text.empty()) {
    return fallback;
  }
  char* end = nullptr;
  double parsed = std::strtod(text.c_str(), &end);
  if (end != text.c_str() && *end == '\0') {
    return parsed;
  }
  return fallback;
}

void SetChoiceValueForInteger(IOSConfigItem* item, int64_t value) {
  item->choice_value = value;
  for (const IOSConfigChoice& choice : item->choices) {
    if (choice.value == value) {
      item->choice_value = choice.value;
      return;
    }
  }
}

void SetChoiceValueForString(IOSConfigItem* item, const std::string& value) {
  item->string_value = value;
  for (size_t i = 0; i < item->choice_string_values.size(); ++i) {
    if (item->choice_string_values[i] == value) {
      item->choice_value = static_cast<int64_t>(i);
      return;
    }
  }
}

bool ApplyTomlNodeToConfigItem(const toml::node* node, IOSConfigItem* item) {
  if (!node || !item || item->key.empty()) {
    return false;
  }

  cvar::IConfigVar* var = GetConfigVar(item->key);
  if (!var) {
    return false;
  }

  void* saved_state = var->SaveConfigValueState();
  var->LoadConfigValue(node);
  const std::string normalized_value = IOSConfigDecodeTomlStringValue(var->config_value());
  var->RestoreConfigValueState(saved_state);

  switch (item->control_type) {
    case IOSConfigControlType::kToggle: {
      bool parsed = false;
      if (IOSConfigParseBoolString(normalized_value, &parsed)) {
        item->bool_value = parsed;
        return true;
      }
      return false;
    }
    case IOSConfigControlType::kChoiceInt32:
    case IOSConfigControlType::kChoiceUInt32:
    case IOSConfigControlType::kChoiceUInt64:
    case IOSConfigControlType::kInteger: {
      int64_t parsed = 0;
      if (!IOSConfigParseInt64String(normalized_value, &parsed)) {
        return false;
      }
      if (item->control_type == IOSConfigControlType::kInteger) {
        item->integer_value = parsed;
      } else {
        SetChoiceValueForInteger(item, parsed);
      }
      return true;
    }
    case IOSConfigControlType::kDouble:
      item->double_value = ParseDoubleFallback(normalized_value, item->double_value);
      return true;
    case IOSConfigControlType::kChoiceString:
    case IOSConfigControlType::kEnum:
      SetChoiceValueForString(item, normalized_value);
      return true;
    case IOSConfigControlType::kString:
    case IOSConfigControlType::kPath:
      item->string_value = normalized_value;
      return true;
    case IOSConfigControlType::kAction:
      return false;
  }
}

toml::table* EnsureGameConfigCategoryTable(toml::table* config_table,
                                           const std::string& category) {
  if (!config_table || category.empty()) {
    return nullptr;
  }
  if (!config_table->contains(category)) {
    config_table->insert(category, toml::table{});
  }
  toml::table* category_table = (*config_table)[category].as_table();
  if (!category_table) {
    config_table->insert_or_assign(category, toml::table{});
    category_table = (*config_table)[category].as_table();
  }
  return category_table;
}

int64_t IntegerValueForConfigItem(const IOSConfigItem& item) {
  switch (item.control_type) {
    case IOSConfigControlType::kChoiceInt32:
    case IOSConfigControlType::kChoiceUInt32:
    case IOSConfigControlType::kChoiceUInt64:
      return item.choice_value;
    case IOSConfigControlType::kInteger:
      return item.integer_value;
    default:
      return 0;
  }
}

std::string StringValueForConfigItem(const IOSConfigItem& item) {
  return IOSConfigNormalizeEditableStringLikeValue(item.string_value);
}

bool InsertGameConfigValue(toml::table* config_table, const IOSConfigItem& item) {
  if (!config_table || item.storage != IOSConfigStorage::kConfigVar || item.key.empty() ||
      item.control_type == IOSConfigControlType::kAction) {
    return true;
  }

  cvar::IConfigVar* var = GetConfigVar(item.key);
  if (!var || var->is_transient()) {
    XELOGW("iOS settings: cannot save game override for '{}'", item.key);
    return false;
  }

  toml::table* category_table = EnsureGameConfigCategoryTable(config_table, var->category());
  if (!category_table) {
    return false;
  }
  const std::string& name = var->name();

  if (dynamic_cast<cvar::ConfigVar<bool>*>(var)) {
    category_table->insert_or_assign(name, item.bool_value);
    return true;
  }
  if (dynamic_cast<cvar::ConfigVar<int32_t>*>(var)) {
    category_table->insert_or_assign(name, static_cast<int32_t>(IntegerValueForConfigItem(item)));
    return true;
  }
  if (dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) {
    int64_t value = IntegerValueForConfigItem(item);
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
      XELOGW("iOS settings: uint32 game override out of range for '{}'", item.key);
      return false;
    }
    category_table->insert_or_assign(name, static_cast<uint32_t>(value));
    return true;
  }
  if (dynamic_cast<cvar::ConfigVar<int64_t>*>(var)) {
    category_table->insert_or_assign(name, IntegerValueForConfigItem(item));
    return true;
  }
  if (dynamic_cast<cvar::ConfigVar<uint64_t>*>(var)) {
    int64_t value = IntegerValueForConfigItem(item);
    if (value < 0) {
      XELOGW("iOS settings: uint64 game override out of range for '{}'", item.key);
      return false;
    }
    category_table->insert_or_assign(name, value);
    return true;
  }
  if (dynamic_cast<cvar::ConfigVar<double>*>(var)) {
    category_table->insert_or_assign(name, item.double_value);
    return true;
  }
  if (dynamic_cast<cvar::ConfigVar<float>*>(var)) {
    category_table->insert_or_assign(name, static_cast<double>(item.double_value));
    return true;
  }
  if (dynamic_cast<cvar::ConfigVar<std::string>*>(var) ||
      dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
    category_table->insert_or_assign(name, StringValueForConfigItem(item));
    return true;
  }

  XELOGW("iOS settings: unsupported game override type for '{}'", item.key);
  return false;
}

static NSUserDefaults* GetUserDefaults() { return [NSUserDefaults standardUserDefaults]; }

bool GetUserDefaultBool(NSString* key, bool fallback) {
  if (!key || key.length == 0) {
    return fallback;
  }
  if (![GetUserDefaults() objectForKey:key]) {
    return fallback;
  }
  return [GetUserDefaults() boolForKey:key];
}

double GetUserDefaultDouble(NSString* key, double fallback) {
  if (!key || key.length == 0) {
    return fallback;
  }
  if (![GetUserDefaults() objectForKey:key]) {
    return fallback;
  }
  return [GetUserDefaults() doubleForKey:key];
}

static NSString* GetUserDefaultString(NSString* key) {
  if (!key || key.length == 0) {
    return nil;
  }
  return [GetUserDefaults() stringForKey:key];
}

void SetUserDefaultBool(NSString* key, bool value) {
  if (!key || key.length == 0) {
    return;
  }
  [GetUserDefaults() setBool:value forKey:key];
}

void SetUserDefaultDouble(NSString* key, double value) {
  if (!key || key.length == 0) {
    return;
  }
  [GetUserDefaults() setDouble:value forKey:key];
}

static void SetUserDefaultString(NSString* key, NSString* value) {
  if (!key || key.length == 0) {
    return;
  }
  if (value.length > 0) {
    [GetUserDefaults() setObject:value forKey:key];
  } else {
    [GetUserDefaults() removeObjectForKey:key];
  }
}

void ClearPendingExternalLaunchPathPreference() {
  [GetUserDefaults() removeObjectForKey:kXeniaPendingExternalLaunchPathPreferenceKey];
  [GetUserDefaults() removeObjectForKey:kXeniaPendingExternalLaunchTimestampPreferenceKey];
}

void StorePendingExternalLaunchPathPreference(const std::filesystem::path& path) {
  NSString* path_string = ToNSString(path.string());
  if (!path_string || path_string.length == 0) {
    ClearPendingExternalLaunchPathPreference();
    return;
  }
  SetUserDefaultString(kXeniaPendingExternalLaunchPathPreferenceKey, path_string);
  SetUserDefaultDouble(kXeniaPendingExternalLaunchTimestampPreferenceKey, GetUnixTimeSeconds());
}

std::filesystem::path TakePendingExternalLaunchPathPreference() {
  NSString* path_string = GetUserDefaultString(kXeniaPendingExternalLaunchPathPreferenceKey);
  const double stored_at =
      GetUserDefaultDouble(kXeniaPendingExternalLaunchTimestampPreferenceKey, 0.0);
  ClearPendingExternalLaunchPathPreference();
  if (!path_string || path_string.length == 0 || stored_at <= 0.0) {
    return std::filesystem::path();
  }
  if ((GetUnixTimeSeconds() - stored_at) > kXeniaPendingExternalLaunchTTLSeconds) {
    XELOGW("iOS: Discarding stale deferred external launch request");
    return std::filesystem::path();
  }
  return std::filesystem::path([path_string UTF8String]).lexically_normal();
}

template <typename T>
static bool SetConfigVarValue(const std::string& key, T value) {
  cvar::IConfigVar* var = GetConfigVar(key);
  if (!var) {
    XELOGW("iOS settings: missing config var '{}'", key);
    return false;
  }
  toml::value node(value);
  var->LoadConfigValue(&node);
  return true;
}

template <typename T>
static bool LoadConfigVarValue(cvar::IConfigVar* var, T value) {
  toml::value node(value);
  var->LoadConfigValue(&node);
  return true;
}

static bool SetConfigVarIntegerValue(const std::string& key, int64_t value) {
  cvar::IConfigVar* var = GetConfigVar(key);
  if (!var) {
    XELOGW("iOS settings: missing config var '{}'", key);
    return false;
  }
  if (dynamic_cast<cvar::ConfigVar<int32_t>*>(var)) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
      XELOGW("iOS settings: integer value {} out of int32 range for '{}'", value, key);
      return false;
    }
    return LoadConfigVarValue(var, static_cast<int32_t>(value));
  }
  if (dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) {
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
      XELOGW("iOS settings: integer value {} out of uint32 range for '{}'", value, key);
      return false;
    }
    return LoadConfigVarValue(var, static_cast<uint32_t>(value));
  }
  if (dynamic_cast<cvar::ConfigVar<int64_t>*>(var)) {
    return LoadConfigVarValue(var, value);
  }
  if (dynamic_cast<cvar::ConfigVar<uint64_t>*>(var)) {
    if (value < 0) {
      XELOGW("iOS settings: negative value {} for uint64 config var '{}'", value, key);
      return false;
    }
    return LoadConfigVarValue(var, static_cast<uint64_t>(value));
  }
  XELOGW("iOS settings: config var '{}' is not an integer type", key);
  return false;
}

static bool SetConfigVarFloatingValue(const std::string& key, double value) {
  cvar::IConfigVar* var = GetConfigVar(key);
  if (!var) {
    XELOGW("iOS settings: missing config var '{}'", key);
    return false;
  }
  if (dynamic_cast<cvar::ConfigVar<double>*>(var)) {
    return LoadConfigVarValue(var, value);
  }
  if (dynamic_cast<cvar::ConfigVar<float>*>(var)) {
    return LoadConfigVarValue(var, static_cast<float>(value));
  }
  XELOGW("iOS settings: config var '{}' is not a floating-point type", key);
  return false;
}

static bool SetConfigVarStringLikeValue(const std::string& key, const std::string& value) {
  cvar::IConfigVar* var = GetConfigVar(key);
  if (!var) {
    XELOGW("iOS settings: missing config var '{}'", key);
    return false;
  }
  if (dynamic_cast<cvar::ConfigVar<std::string>*>(var) ||
      dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
    return LoadConfigVarValue(var, IOSConfigNormalizeEditableStringLikeValue(value));
  }
  XELOGW("iOS settings: config var '{}' is not a string/path type", key);
  return false;
}

bool ApplyIOSConfigSections(const std::vector<IOSConfigSection>& sections) {
  bool ok = true;
  for (const IOSConfigSection& section : sections) {
    for (const IOSConfigItem& item : section.items) {
      switch (item.control_type) {
        case IOSConfigControlType::kToggle:
          if (item.storage == IOSConfigStorage::kUserDefaults) {
            SetUserDefaultBool(ToNSString(item.key), item.bool_value);
          } else {
            ok &= SetConfigVarValue(item.key, item.bool_value);
          }
          break;
        case IOSConfigControlType::kChoiceInt32:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported integer storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarValue(item.key, static_cast<int32_t>(item.choice_value));
          break;
        case IOSConfigControlType::kChoiceUInt32:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported uint32 storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarValue(item.key, static_cast<uint32_t>(item.choice_value));
          break;
        case IOSConfigControlType::kChoiceUInt64:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported uint64 storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarValue(item.key, static_cast<uint64_t>(item.choice_value));
          break;
        case IOSConfigControlType::kChoiceString:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported string storage for '{}'", item.key);
            ok = false;
            break;
          }
          if (item.choice_value < 0 ||
              item.choice_value >= static_cast<int64_t>(item.choice_string_values.size())) {
            XELOGW("iOS settings: invalid string choice index {} for '{}'", item.choice_value,
                   item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarValue(
              item.key, item.choice_string_values[static_cast<size_t>(item.choice_value)]);
          break;
        case IOSConfigControlType::kAction:
          break;
        case IOSConfigControlType::kInteger:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported integer storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarIntegerValue(item.key, item.integer_value);
          break;
        case IOSConfigControlType::kDouble:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported double storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarFloatingValue(item.key, item.double_value);
          break;
        case IOSConfigControlType::kString:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported string storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarStringLikeValue(item.key, item.string_value);
          break;
        case IOSConfigControlType::kPath:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported path storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarStringLikeValue(item.key, item.string_value);
          break;
        case IOSConfigControlType::kEnum:
          if (item.storage != IOSConfigStorage::kConfigVar) {
            XELOGW("iOS settings: unsupported enum storage for '{}'", item.key);
            ok = false;
            break;
          }
          ok &= SetConfigVarStringLikeValue(item.key, item.string_value);
          break;
      }
    }
  }
  config::SaveConfig();
  return ok;
}

void OverlayIOSConfigSectionsFromGameConfig(std::vector<IOSConfigSection>* sections,
                                            uint32_t title_id) {
  if (!sections || !title_id) {
    return;
  }

  toml::table game_config = config::LoadGameConfig(title_id);
  for (IOSConfigSection& section : *sections) {
    for (IOSConfigItem& item : section.items) {
      if (item.storage != IOSConfigStorage::kConfigVar || item.key.empty()) {
        continue;
      }
      cvar::IConfigVar* var = GetConfigVar(item.key);
      if (!var) {
        continue;
      }
      toml::path config_key = toml::path(var->category() + "." + var->name());
      const auto config_key_node = game_config.at_path(config_key);
      if (config_key_node) {
        ApplyTomlNodeToConfigItem(config_key_node.node(), &item);
      }
    }
  }
}

bool ApplyIOSConfigSectionsToGameConfig(const std::vector<IOSConfigSection>& sections,
                                        uint32_t title_id,
                                        const std::set<std::string>& dirty_keys) {
  if (!title_id) {
    return false;
  }

  bool ok = true;
  toml::table game_config = config::LoadGameConfig(title_id);
  for (const IOSConfigSection& section : sections) {
    for (const IOSConfigItem& item : section.items) {
      if (dirty_keys.find(item.key) == dirty_keys.end()) {
        continue;
      }
      ok &= InsertGameConfigValue(&game_config, item);
    }
  }

  try {
    config::SaveGameConfig(title_id, game_config);
  } catch (const std::exception& e) {
    XELOGE("iOS settings: failed to save game config for title {:08X}: {}", title_id, e.what());
    return false;
  }
  return ok;
}
