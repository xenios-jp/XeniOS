/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CONFIG_STORAGE_H_
#define XENIA_UI_IOS_CONFIG_STORAGE_H_

#import <UIKit/UIKit.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "xenia/ui/ios/settings/ios_config_models.h"

extern NSString* const kXeniaAutoOpenStikDebugOnLaunchPreferenceKey;
extern NSString* const kXeniaLastAutoStikDebugAttemptTimestampPreferenceKey;

// Writes the items of |sections| back to their cvars / user defaults. With |dirty_keys|, only
// items whose key is in the set are written, so values the user didn't touch stay as they are.
bool ApplyIOSConfigSections(const std::vector<IOSConfigSection>& sections,
                            const std::set<std::string>* dirty_keys = nullptr);
// Puts every user-facing cvar back to its default and saves the config. The path cvars
// (IOSConfigIsHiddenKey) and transient cvars are left alone.
bool ResetAllIOSConfigToDefaults(void);
// Cvars that must never be listed or edited in the UI (storage locations, log file).
bool IOSConfigIsHiddenKey(const std::string& key);
// Checks a numeric field against the range of the cvar's type. Returns false and fills
// |message| (user-readable) when |value| can't be stored.
bool IOSConfigValidateIntegerValue(const std::string& key, int64_t value, std::string* message);
bool IOSConfigValidateDoubleValue(const std::string& key, double value, std::string* message);

// Reads a title's saved overrides from disk. Only file I/O, no cvar access, so it may run off the
// main thread; hand the result to OverlayIOSConfigSectionsFromGameConfig on the main thread.
std::shared_ptr<void> LoadIOSGameConfigSnapshot(uint32_t title_id);
// Shows a title's saved overrides in |sections|. Loads them now when |snapshot| is null.
void OverlayIOSConfigSectionsFromGameConfig(std::vector<IOSConfigSection>* sections,
                                            uint32_t title_id,
                                            const std::shared_ptr<void>& snapshot = nullptr);
bool ApplyIOSConfigSectionsToGameConfig(const std::vector<IOSConfigSection>& sections,
                                        uint32_t title_id, const std::set<std::string>& dirty_keys);

// NSUserDefaults helpers re-used by the launcher main view controller.
bool GetUserDefaultBool(NSString* key, bool fallback);
double GetUserDefaultDouble(NSString* key, double fallback);
void SetUserDefaultBool(NSString* key, bool value);
void SetUserDefaultDouble(NSString* key, double value);

// Pending external-launch path persistence: when the launcher hands off to
// StikDebug to enable JIT, the path of the game that triggered the launch is
// persisted here so that on the way back in we can resume it.
void ClearPendingExternalLaunchPathPreference(void);
void StorePendingExternalLaunchPathPreference(const std::filesystem::path& path);
std::filesystem::path TakePendingExternalLaunchPathPreference(void);

// Storage helpers used by the settings catalog builder.
bool IOSConfigHasConfigVar(const std::string& key);
std::string IOSConfigGetConfigVarString(const std::string& key, const std::string& fallback);
std::string IOSConfigDecodeTomlStringValue(const std::string& value);
std::string IOSConfigNormalizeEditableStringLikeValue(const std::string& value);
// The cvar's default value as text (string defaults are unquoted). False without such a cvar.
bool IOSConfigGetConfigVarDefaultString(const std::string& key, std::string* value_out);
// Titles of a choice row for a stored value the list doesn't offer, e.g. "40 FPS (custom)".
std::string IOSConfigCustomChoiceTitle(int64_t value, const std::string& unit);
std::string IOSConfigCustomChoiceTitle(const std::string& value);
bool IOSConfigParseBoolString(const std::string& text, bool* value_out);
bool IOSConfigParseInt64String(const std::string& text, int64_t* value_out);

#endif  // XENIA_UI_IOS_CONFIG_STORAGE_H_
