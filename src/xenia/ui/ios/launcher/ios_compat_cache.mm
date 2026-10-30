/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */


#import "xenia/ui/ios/launcher/ios_compat_cache.h"

#import "xenia/ui/ios/shared/ios_view_helpers.h"

NSString* xe_discussion_cache_path(uint32_t title_id) {
  NSString* caches =
      NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
  return [caches
      stringByAppendingPathComponent:[NSString stringWithFormat:@"discussion-%08X.json", title_id]];
}

NSString* xe_compat_cache_path(void) {
  NSString* caches =
      NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
  return [caches stringByAppendingPathComponent:@"compat-data.json"];
}

static NSString* xe_current_iso_date_string(void) {
  static NSDateFormatter* formatter = nil;
  static dispatch_once_t once_token;
  dispatch_once(&once_token, ^{
    formatter = [[NSDateFormatter alloc] init];
    formatter.dateFormat = @"yyyy-MM-dd";
    formatter.locale = [[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"] autorelease];
    formatter.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
  });
  return [formatter stringFromDate:[NSDate date]];
}

static NSInteger xe_issue_number_from_url(NSString* issue_url) {
  NSURL* url = [NSURL URLWithString:issue_url];
  NSString* last_component = url.pathComponents.lastObject;
  NSInteger issue_number = last_component.integerValue;
  return issue_number > 0 ? issue_number : 0;
}

NSDictionary* xe_build_local_compat_report(NSString* device, NSString* device_machine,
                                           NSString* os_version, NSString* status, NSString* perf,
                                           NSString* notes, NSDictionary* build_info) {
  NSMutableDictionary* report = [NSMutableDictionary
      dictionaryWithObjectsAndKeys:device ?: @"Unknown", @"device", @"ios", @"platform",
                                   os_version ?: @"", @"osVersion", @"arm64", @"arch", @"msl",
                                   @"gpuBackend", status ?: @"nothing", @"status", perf ?: @"n/a",
                                   @"perf", xe_current_iso_date_string(), @"date", notes ?: @"",
                                   @"notes", @"app", @"source", nil];
  if ([device_machine isKindOfClass:[NSString class]] && device_machine.length > 0) {
    report[@"deviceMachine"] = device_machine;
  }
  NSDictionary* sanitized_build = xe_compat_build_info_from_entry(@{@"build" : build_info ?: @{}});
  if (sanitized_build) {
    report[@"build"] = sanitized_build;
  }
  return report;
}

NSDictionary* xe_update_cached_compat_entry_for_submission(uint32_t title_id, NSString* title,
                                                           NSDictionary* report,
                                                           NSString* issue_url) {
  NSString* cache_path = xe_compat_cache_path();
  NSData* cached_data = [NSData dataWithContentsOfFile:cache_path];
  NSMutableArray* entries = [NSMutableArray array];
  if (cached_data.length > 0) {
    id parsed = [NSJSONSerialization JSONObjectWithData:cached_data options:0 error:nil];
    if ([parsed isKindOfClass:[NSArray class]]) {
      [entries addObjectsFromArray:(NSArray*)parsed];
    }
  }

  NSString* title_id_string = XEFormatTitleIDHexUpper(title_id);
  NSUInteger existing_index = NSNotFound;
  NSDictionary* current_entry = nil;
  for (NSUInteger i = 0; i < entries.count; ++i) {
    id item = entries[i];
    if (![item isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSString* cached_title_id = xe_string_from_object(item[@"titleId"]);
    if ([cached_title_id caseInsensitiveCompare:title_id_string] == NSOrderedSame) {
      existing_index = i;
      current_entry = item;
      break;
    }
  }

  NSMutableArray* reports = [NSMutableArray arrayWithObject:report];
  NSArray* existing_reports =
      [current_entry[@"reports"] isKindOfClass:[NSArray class]] ? current_entry[@"reports"] : nil;
  if (existing_reports) {
    [reports addObjectsFromArray:existing_reports];
  }

  NSMutableDictionary* next_entry =
      current_entry ? [[current_entry mutableCopy] autorelease] : [NSMutableDictionary dictionary];
  next_entry[@"titleId"] = title_id_string;
  next_entry[@"title"] = title ?: title_id_string;
  next_entry[@"status"] = xe_string_from_object(report[@"status"]) ?: @"nothing";
  next_entry[@"perf"] = xe_string_from_object(report[@"perf"]) ?: @"n/a";
  next_entry[@"notes"] = xe_string_from_object(report[@"notes"]) ?: @"";
  next_entry[@"updatedAt"] = xe_string_from_object(report[@"date"]) ?: xe_current_iso_date_string();
  next_entry[@"reports"] = reports;

  NSString* device = xe_string_from_object(report[@"device"]);
  NSString* device_machine = xe_string_from_object(report[@"deviceMachine"]);
  NSString* platform = xe_string_from_object(report[@"platform"]);
  NSString* os_version = xe_string_from_object(report[@"osVersion"]);
  NSString* arch = xe_string_from_object(report[@"arch"]);
  NSString* gpu_backend = xe_string_from_object(report[@"gpuBackend"]);

  if (device.length > 0) {
    next_entry[@"device"] = device;
  }
  if (device_machine.length > 0) {
    next_entry[@"deviceMachine"] = device_machine;
  }
  if (platform.length > 0) {
    next_entry[@"platform"] = platform;
  }
  if (os_version.length > 0) {
    next_entry[@"osVersion"] = os_version;
  }
  if (arch.length > 0) {
    next_entry[@"arch"] = arch;
  }
  if (gpu_backend.length > 0) {
    next_entry[@"gpuBackend"] = gpu_backend;
  }

  NSMutableDictionary* last_report = [NSMutableDictionary dictionary];
  if (device.length > 0) {
    last_report[@"device"] = device;
  }
  if (platform.length > 0) {
    last_report[@"platform"] = platform;
  }
  if (os_version.length > 0) {
    last_report[@"osVersion"] = os_version;
  }
  if (arch.length > 0) {
    last_report[@"arch"] = arch;
  }
  if (gpu_backend.length > 0) {
    last_report[@"gpuBackend"] = gpu_backend;
  }
  if (last_report.count > 0) {
    next_entry[@"lastReport"] = last_report;
  }

  NSMutableArray* platforms =
      [NSMutableArray arrayWithArray:[current_entry[@"platforms"] isKindOfClass:[NSArray class]]
                                         ? current_entry[@"platforms"]
                                         : @[]];
  if (platform.length > 0 && ![platforms containsObject:platform]) {
    [platforms addObject:platform];
  }
  if (platforms.count > 0) {
    next_entry[@"platforms"] = platforms;
  }

  NSString* next_issue_url =
      issue_url.length > 0 ? issue_url : xe_string_from_object(current_entry[@"issueUrl"]);
  if (next_issue_url.length > 0) {
    next_entry[@"issueUrl"] = next_issue_url;
    NSInteger issue_number = xe_issue_number_from_url(next_issue_url);
    if (issue_number > 0) {
      next_entry[@"issueNumber"] = @(issue_number);
    }
  }

  if (existing_index != NSNotFound) {
    entries[existing_index] = next_entry;
  } else {
    [entries addObject:next_entry];
  }

  if ([NSJSONSerialization isValidJSONObject:entries]) {
    NSError* write_error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:entries
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:&write_error];
    if (data && !write_error) {
      NSMutableData* writable = [data mutableCopy];
      static const char newline = '\n';
      [writable appendBytes:&newline length:1];
      [writable writeToFile:cache_path atomically:YES];
      [writable release];
    }
  }
  return next_entry;
}

NSDictionary* xe_cache_discussion_snapshot_for_submission(uint32_t title_id,
                                                          NSDictionary* compat_entry) {
  if (!compat_entry) {
    return nil;
  }

  NSMutableDictionary* snapshot = [NSMutableDictionary dictionary];
  NSArray* reports =
      [compat_entry[@"reports"] isKindOfClass:[NSArray class]] ? compat_entry[@"reports"] : nil;
  if (reports.count > 0) {
    snapshot[@"reports"] = reports;
  }
  NSString* issue_url = xe_string_from_object(compat_entry[@"issueUrl"]);
  if (issue_url.length > 0) {
    snapshot[@"issueUrl"] = issue_url;
    NSInteger issue_number = xe_issue_number_from_url(issue_url);
    if (issue_number > 0) {
      snapshot[@"issueNumber"] = @(issue_number);
    }
  } else if ([compat_entry[@"issueNumber"] isKindOfClass:[NSNumber class]]) {
    snapshot[@"issueNumber"] = compat_entry[@"issueNumber"];
  }

  if (snapshot.count == 0) {
    return nil;
  }

  if ([NSJSONSerialization isValidJSONObject:snapshot]) {
    NSError* write_error = nil;
    NSData* data = [NSJSONSerialization dataWithJSONObject:snapshot
                                                   options:NSJSONWritingPrettyPrinted
                                                     error:&write_error];
    if (data && !write_error) {
      NSMutableData* writable = [data mutableCopy];
      static const char newline = '\n';
      [writable appendBytes:&newline length:1];
      [writable writeToFile:xe_discussion_cache_path(title_id) atomically:YES];
      [writable release];
    }
  }
  return snapshot;
}

NSDictionary* xe_parse_compat_json(NSData* data) {
  if (!data || data.length == 0) {
    return nil;
  }
  NSError* error = nil;
  id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
  if (error || ![parsed isKindOfClass:[NSArray class]]) {
    return nil;
  }
  NSMutableDictionary* dict = [NSMutableDictionary dictionaryWithCapacity:[parsed count]];
  for (NSDictionary* game in parsed) {
    if (![game isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSString* title_id = game[@"titleId"];
    if ([title_id isKindOfClass:[NSString class]] && title_id.length > 0) {
      [dict setObject:game forKey:[title_id uppercaseString]];
    }
  }
  return dict;
}

NSDictionary* xe_load_cached_compat_data(void) {
  NSData* cached = [NSData dataWithContentsOfFile:xe_compat_cache_path()];
  if (cached.length == 0) {
    return nil;
  }
  return xe_parse_compat_json(cached);
}

NSArray* xe_parse_compat_games_array(NSData* data) {
  if (!data || data.length == 0) {
    return nil;
  }
  id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  return [parsed isKindOfClass:[NSArray class]] ? (NSArray*)parsed : nil;
}

static NSString* xe_compat_report_identity(NSDictionary* report) {
  if (![report isKindOfClass:[NSDictionary class]]) {
    return nil;
  }
  NSDictionary* build = xe_dictionary_from_object(report[@"build"]);
  return [@[
    xe_string_from_object(report[@"date"]) ?: @"",
    xe_string_from_object(report[@"platform"]) ?: @"",
    xe_string_from_object(report[@"device"]) ?: @"",
    xe_string_from_object(report[@"status"]) ?: @"", xe_string_from_object(report[@"perf"]) ?: @"",
    xe_string_from_object(report[@"notes"]) ?: @"", xe_string_from_object(build[@"buildId"]) ?: @"",
    xe_string_from_object(build[@"buildNumber"]) ?: @"",
    xe_string_from_object(build[@"commitShort"]) ?: @""
  ] componentsJoinedByString:@"|"];
}

static NSArray* xe_pending_cached_app_reports(NSDictionary* cached_entry,
                                              NSDictionary* remote_entry) {
  NSArray* cached_reports =
      [cached_entry[@"reports"] isKindOfClass:[NSArray class]] ? cached_entry[@"reports"] : nil;
  if (cached_reports.count == 0) {
    return @[];
  }

  NSMutableSet* remote_identities = [NSMutableSet set];
  NSArray* remote_reports =
      [remote_entry[@"reports"] isKindOfClass:[NSArray class]] ? remote_entry[@"reports"] : nil;
  for (id remote_report in remote_reports) {
    NSString* identity = xe_compat_report_identity(remote_report);
    if (identity.length > 0) {
      [remote_identities addObject:identity];
    }
  }

  NSMutableArray* pending_reports = [NSMutableArray array];
  for (id cached_report in cached_reports) {
    if (![cached_report isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    if (![xe_string_from_object(cached_report[@"source"]) isEqualToString:@"app"]) {
      continue;
    }
    NSString* cached_identity = xe_compat_report_identity(cached_report);
    if (cached_identity.length == 0 || [remote_identities containsObject:cached_identity]) {
      continue;
    }
    [pending_reports addObject:cached_report];
  }

  return pending_reports;
}

static BOOL xe_cached_entry_has_pending_app_report(NSDictionary* cached_entry,
                                                   NSDictionary* remote_entry) {
  return xe_pending_cached_app_reports(cached_entry, remote_entry).count > 0;
}

static NSDictionary* xe_merge_remote_compat_entry_with_cached_submission(
    NSDictionary* remote_entry, NSDictionary* cached_entry) {
  NSMutableDictionary* merged_entry = [[remote_entry mutableCopy] autorelease];

  NSArray* pending_reports = xe_pending_cached_app_reports(cached_entry, remote_entry);
  NSArray* remote_reports =
      [remote_entry[@"reports"] isKindOfClass:[NSArray class]] ? remote_entry[@"reports"] : nil;
  NSMutableArray* merged_reports =
      [NSMutableArray arrayWithCapacity:remote_reports.count + pending_reports.count];
  if (pending_reports.count > 0) {
    [merged_reports addObjectsFromArray:pending_reports];
  }
  if (remote_reports.count > 0) {
    [merged_reports addObjectsFromArray:remote_reports];
  }
  if (merged_reports.count > 0) {
    merged_entry[@"reports"] = merged_reports;
  }

  for (NSString* key in @[
         @"status", @"perf", @"notes", @"updatedAt", @"device", @"deviceMachine", @"platform",
         @"osVersion", @"arch", @"gpuBackend"
       ]) {
    id value = cached_entry[key];
    if ([value isKindOfClass:[NSString class]] && [value length] > 0) {
      merged_entry[key] = value;
    }
  }

  NSDictionary* cached_last_report = xe_dictionary_from_object(cached_entry[@"lastReport"]);
  if (cached_last_report) {
    merged_entry[@"lastReport"] = cached_last_report;
  }

  NSMutableArray* merged_platforms =
      [NSMutableArray arrayWithArray:[remote_entry[@"platforms"] isKindOfClass:[NSArray class]]
                                         ? remote_entry[@"platforms"]
                                         : @[]];
  NSArray* cached_platforms =
      [cached_entry[@"platforms"] isKindOfClass:[NSArray class]] ? cached_entry[@"platforms"] : nil;
  for (id platform in cached_platforms) {
    if ([platform isKindOfClass:[NSString class]] && ![merged_platforms containsObject:platform]) {
      [merged_platforms addObject:platform];
    }
  }
  if (merged_platforms.count > 0) {
    merged_entry[@"platforms"] = merged_platforms;
  }

  NSString* remote_title = xe_string_from_object(remote_entry[@"title"]);
  NSString* cached_title = xe_string_from_object(cached_entry[@"title"]);
  if (remote_title.length == 0 && cached_title.length > 0) {
    merged_entry[@"title"] = cached_title;
  }

  NSString* remote_issue_url = xe_string_from_object(remote_entry[@"issueUrl"]);
  NSString* cached_issue_url = xe_string_from_object(cached_entry[@"issueUrl"]);
  if (remote_issue_url.length == 0 && cached_issue_url.length > 0) {
    merged_entry[@"issueUrl"] = cached_issue_url;
  }
  if (![merged_entry[@"issueNumber"] isKindOfClass:[NSNumber class]] &&
      [cached_entry[@"issueNumber"] isKindOfClass:[NSNumber class]]) {
    merged_entry[@"issueNumber"] = cached_entry[@"issueNumber"];
  }

  return merged_entry;
}

NSArray* xe_merge_remote_compat_games_with_cached_submissions(NSArray* remote_games,
                                                            NSArray* cached_games) {
  if (remote_games.count == 0) {
    return cached_games.count > 0 ? cached_games : remote_games;
  }
  if (cached_games.count == 0) {
    return remote_games;
  }

  NSMutableArray* merged = [NSMutableArray arrayWithArray:remote_games];
  NSMutableDictionary* remote_indexes =
      [NSMutableDictionary dictionaryWithCapacity:remote_games.count];
  for (NSUInteger i = 0; i < remote_games.count; ++i) {
    id item = remote_games[i];
    if (![item isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSString* title_id = xe_string_from_object(item[@"titleId"]);
    if (title_id.length > 0) {
      remote_indexes[[title_id uppercaseString]] = @(i);
    }
  }

  for (id item in cached_games) {
    if (![item isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSDictionary* cached_entry = (NSDictionary*)item;
    NSString* title_id = [xe_string_from_object(cached_entry[@"titleId"]) uppercaseString];
    if (title_id.length == 0) {
      continue;
    }

    NSNumber* remote_index = remote_indexes[title_id];
    NSDictionary* remote_entry =
        remote_index ? xe_dictionary_from_object(merged[remote_index.unsignedIntegerValue]) : nil;
    if (!remote_entry) {
      if (xe_cached_entry_has_pending_app_report(cached_entry, nil)) {
        [merged addObject:cached_entry];
      }
      continue;
    }

    if (xe_cached_entry_has_pending_app_report(cached_entry, remote_entry)) {
      merged[remote_index.unsignedIntegerValue] =
          xe_merge_remote_compat_entry_with_cached_submission(remote_entry, cached_entry);
    }
  }

  return merged;
}
