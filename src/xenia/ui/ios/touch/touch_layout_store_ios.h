/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_TOUCH_LAYOUT_STORE_IOS_H_
#define XENIA_UI_IOS_TOUCH_LAYOUT_STORE_IOS_H_

#import <Foundation/Foundation.h>

#include <filesystem>
#include <string>
#include <vector>

#include "xenia/hid/touch/touch_layout_ios.h"
#include "xenia/ui/ios/touch/touch_layout_library_ios.h"

namespace xe {
namespace ui {

std::filesystem::path IOSTouchLayoutsDirectory();
std::filesystem::path IOSTouchLayoutPathForLocalID(const std::string& local_id);
bool EnsureIOSTouchLayoutsDirectory(NSString** error_out = nil);
bool WriteIOSTouchLayoutModel(const xe::hid::touch::IOSTouchLayoutModel& layout,
                              const std::filesystem::path& layout_path, NSString** error_out = nil);
bool LoadIOSTouchLayoutModelAtPath(const std::filesystem::path& layout_path,
                                   xe::hid::touch::IOSTouchLayoutModel* layout_out,
                                   NSString** error_out = nil);
void EnsureOfficialIOSTouchLayoutPresets();
std::vector<IOSTouchLocalLayoutInfo> AvailableLocalIOSTouchLayouts();
std::string UniqueIOSTouchLayoutLocalIDForBaseName(
    NSString* base_name, const std::string& existing_local_id = std::string());

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_TOUCH_LAYOUT_STORE_IOS_H_
