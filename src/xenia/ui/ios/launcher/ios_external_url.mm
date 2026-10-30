/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/ios/launcher/ios_external_url.h"

#include <cerrno>
#include <cstdlib>

#include "xenia/ui/ios/launcher/ios_game_library.h"
#include "xenia/ui/ios/shared/ios_system_utils.h"
#include "xenia/ui/ios/shared/ios_view_helpers.h"

namespace xe {
namespace ui {
namespace {

NSString* URLQueryItemValueCaseInsensitive(
    NSURLComponents* components, NSArray<NSString*>* candidate_keys) {
  if (!components || candidate_keys.count == 0) {
    return nil;
  }
  for (NSString* key in candidate_keys) {
    for (NSURLQueryItem* item in components.queryItems) {
      if (!item.name || [item.name caseInsensitiveCompare:key] != NSOrderedSame) {
        continue;
      }
      NSString* value = NormalizeURLToken(item.value);
      if (value.length > 0) {
        return value;
      }
    }
  }
  return nil;
}

NSString* ExternalURLActionName(NSURL* url) {
  if (!url) {
    return nil;
  }
  NSString* host = NormalizeURLToken(url.host);
  if (host.length > 0) {
    return [host lowercaseString];
  }
  for (NSString* path_component in url.pathComponents) {
    NSString* component = NormalizeURLToken(path_component);
    if (!component || [component isEqualToString:@"/"]) {
      continue;
    }
    return [component lowercaseString];
  }
  return nil;
}

bool BuildLaunchPathFromURLValue(NSString* value,
                                 std::filesystem::path* path_out) {
  if (!path_out) {
    return false;
  }
  NSString* normalized = DecodeURLComponent(value);
  if (!normalized || normalized.length == 0) {
    return false;
  }

  NSURL* nested_url = [NSURL URLWithString:normalized];
  if (nested_url && nested_url.isFileURL) {
    normalized = nested_url.path;
  }
  if (!normalized || normalized.length == 0 ||
      [normalized isEqualToString:@"/"]) {
    return false;
  }

  if ([normalized hasPrefix:@"private/"]) {
    normalized = [@"/" stringByAppendingString:normalized];
  } else if (![normalized hasPrefix:@"/"]) {
    normalized = [ToNSString(xe_get_ios_documents_path().string())
        stringByAppendingPathComponent:normalized];
  }

  *path_out = std::filesystem::path([normalized UTF8String]).lexically_normal();
  return !path_out->empty();
}

bool ParseTitleIDFromURLValue(NSString* value, uint32_t* title_id_out) {
  if (!value || !title_id_out) {
    return false;
  }
  NSString* normalized = DecodeURLComponent(value);
  if (!normalized) {
    return false;
  }
  normalized = [normalized
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if ([normalized hasPrefix:@"0x"] || [normalized hasPrefix:@"0X"]) {
    normalized = [normalized substringFromIndex:2];
  }
  if (normalized.length != 8) {
    return false;
  }
  const char* utf8 = [normalized UTF8String];
  if (!utf8 || !utf8[0]) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long parsed = std::strtoul(utf8, &end, 16);
  if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX ||
      parsed == 0) {
    return false;
  }
  *title_id_out = static_cast<uint32_t>(parsed);
  return true;
}

bool ParseGameSystemFromURLValue(NSString* value, IOSGameSystem* system_out) {
  if (!value || !system_out) {
    return false;
  }
  NSString* normalized = NormalizeURLToken(value);
  if (!normalized) {
    return false;
  }
  normalized = [normalized lowercaseString];
  if ([normalized isEqualToString:@"xbox360"] ||
      [normalized isEqualToString:@"360"] ||
      [normalized isEqualToString:@"xenia"]) {
    *system_out = IOSGameSystem::kXbox360;
    return true;
  }
  return false;
}

}  // namespace

NSString* DecodeURLComponent(NSString* value) {
  if (!value || value.length == 0) {
    return nil;
  }
  NSString* decoded = [value stringByRemovingPercentEncoding];
  if (decoded && decoded.length > 0) {
    return decoded;
  }
  return value;
}

NSString* NormalizeURLToken(NSString* value) {
  NSString* decoded = DecodeURLComponent(value);
  if (!decoded) {
    return nil;
  }
  NSString* trimmed =
      [decoded stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
  return trimmed.length > 0 ? trimmed : nil;
}

bool ExtractLaunchPathFromExternalURL(NSURL* url,
                                      std::filesystem::path* path_out) {
  if (!url || !path_out) {
    return false;
  }

  if (url.isFileURL) {
    const char* url_path = [url.path UTF8String];
    if (url_path && url_path[0]) {
      *path_out = std::filesystem::path(url_path).lexically_normal();
      return true;
    }
  }

  NSURLComponents* components =
      [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
  if (components) {
    NSArray<NSURLQueryItem*>* query_items = components.queryItems;
    NSArray<NSString*>* candidate_keys = @[ @"path", @"file", @"game", @"rom", @"url" ];
    for (NSString* key in candidate_keys) {
      for (NSURLQueryItem* item in query_items) {
        if (!item.name || [item.name caseInsensitiveCompare:key] != NSOrderedSame) {
          continue;
        }
        if (BuildLaunchPathFromURLValue(item.value, path_out)) {
          return true;
        }
      }
    }
  }

  if (BuildLaunchPathFromURLValue(url.path, path_out)) {
    return true;
  }

  BOOL host_looks_like_path = NO;
  if (url.host && url.host.length > 0) {
    host_looks_like_path = [url.host hasPrefix:@"/"] ||
                           [url.host hasPrefix:@"private/"] ||
                           [url.host hasPrefix:@"%2F"] ||
                           [url.host hasPrefix:@"%2f"];
  }
  if (host_looks_like_path &&
      (!url.path || url.path.length == 0 || [url.path isEqualToString:@"/"]) &&
      BuildLaunchPathFromURLValue(url.host, path_out)) {
    return true;
  }

  return false;
}

bool IsExternalGameInfoRequestURL(NSURL* url, NSString** callback_scheme_out) {
  if (callback_scheme_out) {
    *callback_scheme_out = nil;
  }
  if (!url || url.isFileURL) {
    return false;
  }
  NSString* action = ExternalURLActionName(url);
  if (!action || [action caseInsensitiveCompare:@"gameinfo"] != NSOrderedSame) {
    return false;
  }
  NSURLComponents* components =
      [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
  if (callback_scheme_out) {
    *callback_scheme_out = URLQueryItemValueCaseInsensitive(
        components, @[ @"scheme", @"callback-scheme", @"callback_scheme" ]);
  }
  return true;
}

bool ExtractLaunchTitleIDFromExternalURL(NSURL* url, uint32_t* title_id_out,
                                         IOSGameSystem* system_out,
                                         bool* system_present_out) {
  if (!url || !title_id_out || url.isFileURL) {
    return false;
  }

  if (system_out) {
    *system_out = IOSGameSystem::kXbox360;
  }
  if (system_present_out) {
    *system_present_out = false;
  }

  NSURLComponents* components =
      [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
  if (components) {
    if (system_out) {
      NSArray<NSString*>* system_keys = @[ @"system", @"platform", @"core" ];
      for (NSString* key in system_keys) {
        for (NSURLQueryItem* item in components.queryItems) {
          if (!item.name || [item.name caseInsensitiveCompare:key] != NSOrderedSame) {
            continue;
          }
          if (ParseGameSystemFromURLValue(item.value, system_out)) {
            if (system_present_out) {
              *system_present_out = true;
            }
            break;
          }
        }
      }
    }
    NSArray<NSString*>* candidate_keys = @[ @"title-id", @"title_id", @"titleid", @"tid" ];
    for (NSString* key in candidate_keys) {
      for (NSURLQueryItem* item in components.queryItems) {
        if (!item.name || [item.name caseInsensitiveCompare:key] != NSOrderedSame) {
          continue;
        }
        if (ParseTitleIDFromURLValue(item.value, title_id_out)) {
          return true;
        }
      }
    }
  }

  NSArray<NSString*>* path_components = url.pathComponents;
  for (NSString* component in [path_components reverseObjectEnumerator]) {
    if (ParseTitleIDFromURLValue(component, title_id_out)) {
      return true;
    }
  }

  return ParseTitleIDFromURLValue(url.host, title_id_out);
}

NSString* xe_game_system_url_value(IOSGameSystem system) {
  switch (system) {
    case IOSGameSystem::kXbox360:
    default:
      return @"xbox360";
  }
}

NSString* xe_launch_url_for_title_id(uint32_t title_id, IOSGameSystem system) {
  if (!title_id) {
    return nil;
  }
  NSURLComponents* components = [[[NSURLComponents alloc] init] autorelease];
  components.scheme = @"xenios";
  components.host = @"launch";
  components.queryItems = @[
    [NSURLQueryItem queryItemWithName:@"title-id" value:ToNSString(FormatTitleID(title_id))],
    [NSURLQueryItem queryItemWithName:@"system" value:xe_game_system_url_value(system)]
  ];
  return components.URL.absoluteString;
}

NSString* xe_game_info_callback_provider(NSURL* request_url) {
  NSString* scheme = NormalizeURLToken(request_url ? request_url.scheme : nil);
  if (!scheme || scheme.length == 0) {
    return @"xenios";
  }
  return [scheme lowercaseString];
}

NSURL* xe_stikdebug_enable_jit_url_for_bundle_identifier(
    NSString* bundle_identifier) {
  if (!bundle_identifier || bundle_identifier.length == 0) {
    return nil;
  }
  NSURLComponents* components = [[[NSURLComponents alloc] init] autorelease];
  components.scheme = @"stikjit";
  components.host = @"enable-jit";
  components.queryItems = @[ [NSURLQueryItem queryItemWithName:@"bundle-id"
                                                         value:bundle_identifier] ];
  return components.URL;
}

}  // namespace ui
}  // namespace xe
