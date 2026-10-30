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
#include <set>
#include <string>
#include <vector>

#include "xenia/ui/ios/settings/ios_config_models.h"

extern NSString* const kXeniaAutoOpenStikDebugOnLaunchPreferenceKey;
extern NSString* const kXeniaLastAutoStikDebugAttemptTimestampPreferenceKey;

bool ApplyIOSConfigSections(const std::vector<IOSConfigSection>& sections);
void OverlayIOSConfigSectionsFromGameConfig(std::vector<IOSConfigSection>* sections,
                                            uint32_t title_id);
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
bool IOSConfigParseBoolString(const std::string& text, bool* value_out);
bool IOSConfigParseInt64String(const std::string& text, int64_t* value_out);

#endif  // XENIA_UI_IOS_CONFIG_STORAGE_H_
