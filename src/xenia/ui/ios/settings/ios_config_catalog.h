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

#endif  // XENIA_UI_IOS_CONFIG_CATALOG_H_
