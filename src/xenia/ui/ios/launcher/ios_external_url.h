/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_EXTERNAL_URL_H_
#define XENIA_UI_IOS_EXTERNAL_URL_H_

#import <Foundation/Foundation.h>

#include <cstdint>
#include <filesystem>

#include "xenia/ui/ios/app/windowed_app_context_ios.h"

namespace xe {
namespace ui {

NSString* DecodeURLComponent(NSString* value);
NSString* NormalizeURLToken(NSString* value);
bool ExtractLaunchPathFromExternalURL(NSURL* url, std::filesystem::path* path_out);
bool IsExternalGameInfoRequestURL(NSURL* url, NSString** callback_scheme_out);
bool ExtractLaunchTitleIDFromExternalURL(NSURL* url, uint32_t* title_id_out,
                                         IOSGameSystem* system_out, bool* system_present_out);

NSString* xe_game_system_url_value(IOSGameSystem system);
NSString* xe_launch_url_for_title_id(uint32_t title_id, IOSGameSystem system);
NSString* xe_game_info_callback_provider(NSURL* request_url);
NSURL* xe_stikdebug_enable_jit_url_for_bundle_identifier(NSString* bundle_identifier);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IOS_EXTERNAL_URL_H_
