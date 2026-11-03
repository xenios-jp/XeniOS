/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_CONFIG_CATALOG_H_
#define XENIA_UI_IOS_CONFIG_CATALOG_H_

#include <string>
#include <vector>

#include "xenia/ui/ios/settings/ios_config_models.h"

std::vector<IOSConfigSection> BuildIOSConfigSections(void);
std::vector<IOSConfigSection> BuildIOSConfigSectionsForKind(
    IOSConfigCatalogKind kind);
std::vector<IOSConfigSection> BuildAllCvarSections(void);
std::vector<IOSConfigSection> BuildDebugSettingsSections(void);
std::string IOSConfigCatalogTitle(IOSConfigCatalogKind kind);
std::string ChoiceTitleForItem(const IOSConfigItem& item);

// True when the title-specific settings sheet (kPerGame) offers |key|.
bool IOSConfigKeyAllowsPerGame(const std::string& key);

// The row's current value as short text ("On", the choice title, the number).
std::string IOSConfigItemValueText(const IOSConfigItem& item);
// The default as the row would show it; false when it isn't known (actions, user defaults
// without one).
bool IOSConfigItemDefaultText(const IOSConfigItem& item, std::string* text_out);
bool IOSConfigItemIsDefault(const IOSConfigItem& item);
// Puts the row's value back to its default. False when it already is, or the default isn't one
// of the row's choices.
bool IOSConfigResetItemToDefault(IOSConfigItem* item);

// One searchable setting. The index lists every curated setting once (de-duplicated by key), then
// the Advanced Debug and raw cvar settings that have no curated row (|advanced|, which need
// developer mode to edit).
struct IOSConfigSearchEntry {
  std::string key;  // Empty for actions, which are found by |title|.
  std::string title;
  std::string subtitle;
  std::string page;     // "Graphics"
  std::string section;  // "Shader & Pipeline"
  std::string value_text;
  IOSConfigCatalogKind kind = IOSConfigCatalogKind::kMain;  // The page that edits it.
  bool per_game = false;
  bool advanced = false;
  bool value_searchable = false;  // The value text is worth matching (not On/Off).
};
// Builds a fresh index (reads every setting's current value); callers cache it per search session.
std::vector<IOSConfigSearchEntry> BuildIOSConfigSearchIndex(void);

#endif  // XENIA_UI_IOS_CONFIG_CATALOG_H_
