/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */


#import "xenia/ui/ios/launcher/ios_compat_summary.h"

#import "xenia/ui/ios/launcher/ios_compat_formatting.h"

static BOOL xe_compat_entry_has_summary_fields(NSDictionary* entry) {
  if (!entry) {
    return NO;
  }
  return xe_string_from_object(entry[@"status"]).length > 0 ||
         xe_string_from_object(entry[@"perf"]).length > 0 ||
         xe_string_from_object(entry[@"notes"]).length > 0;
}

static NSDictionary* xe_compat_summary_named(NSDictionary* compat_info, NSString* key) {
  NSDictionary* summaries = xe_dictionary_from_object(compat_info[@"summaries"]);
  NSDictionary* summary = summaries ? xe_dictionary_from_object(summaries[key]) : nil;
  return xe_compat_entry_has_summary_fields(summary) ? summary : nil;
}

NSDictionary* xe_release_summary_from_compat_info(NSDictionary* compat_info) {
  return xe_compat_summary_named(compat_info, @"release");
}


static BOOL xe_compat_date_is_newer(NSString* candidate, NSString* baseline) {
  if (candidate.length == 0) {
    return NO;
  }
  if (baseline.length == 0) {
    return YES;
  }
  return [candidate compare:baseline options:NSCaseInsensitiveSearch] == NSOrderedDescending;
}

static NSDictionary* xe_compat_newest_report(NSArray<NSDictionary*>* reports) {
  NSDictionary* newest = nil;
  NSString* newest_date = @"";
  NSInteger newest_rank = -1;
  for (NSDictionary* report in reports) {
    NSString* date = xe_string_from_object(report[@"date"]) ?: @"";
    NSString* status = xe_string_from_object(report[@"status"]) ?: @"";
    NSInteger rank = xe_compat_status_rank(status);
    if (!newest || xe_compat_date_is_newer(date, newest_date) ||
        ([date isEqualToString:newest_date] && rank > newest_rank)) {
      newest = report;
      newest_date = date;
      newest_rank = rank;
    }
  }
  return newest;
}

// Strict "newest by date" with no rank tiebreak — mirrors the worker's
// latestReportForReports (data/compatibility.json's summaries.* are produced
// by this rule).
static NSDictionary* xe_compat_latest_report(NSArray<NSDictionary*>* reports) {
  NSDictionary* latest = nil;
  NSString* latest_date = @"";
  for (NSDictionary* report in reports) {
    NSString* date = xe_string_from_object(report[@"date"]) ?: @"";
    if (!latest || xe_compat_date_is_newer(date, latest_date)) {
      latest = report;
      latest_date = date;
    }
  }
  return latest;
}

static NSDictionary* xe_compat_best_report(NSArray<NSDictionary*>* reports) {
  NSDictionary* best = nil;
  NSInteger best_rank = -1;
  NSString* best_date = @"";
  for (NSDictionary* report in reports) {
    NSString* status = xe_string_from_object(report[@"status"]) ?: @"";
    NSInteger rank = xe_compat_status_rank(status);
    NSString* date = xe_string_from_object(report[@"date"]) ?: @"";
    if (!best || rank > best_rank ||
        (rank == best_rank && xe_compat_date_is_newer(date, best_date))) {
      best = report;
      best_rank = rank;
      best_date = date;
    }
  }
  return best;
}

// Mirrors the compat worker's reportMatchesSummaryChannel for "release":
// strict buildId match against the currently-published release build, with a
// legacy fallback that accepts reports missing build metadata or carrying
// channel="release". See worker/src/index.ts (xenios-jp/xenios.jp).
static BOOL xe_compat_release_filter_matches(NSDictionary* report,
                                             NSDictionary* current_release_build) {
  NSDictionary* build = xe_dictionary_from_object(report[@"build"]);
  NSString* current_build_id = xe_string_from_object(current_release_build[@"buildId"]);
  if (current_build_id.length > 0) {
    NSString* report_channel = xe_string_from_object(build[@"channel"]);
    NSString* report_build_id = xe_string_from_object(build[@"buildId"]);
    return [report_channel isEqualToString:@"release"] &&
           [report_build_id isEqualToString:current_build_id];
  }
  if (!build) {
    return YES;
  }
  NSString* channel = xe_string_from_object(build[@"channel"]);
  if (channel.length == 0) {
    return YES;
  }
  return [channel isEqualToString:@"release"];
}

// Mirrors the compat worker's buildSummaryForChannel — produces summaries in
// the exact shape that data/compatibility.json's summaries.* fields use
// (channel, status, perf, notes, updatedAt, reportCount, latestReport,
// bestReport). Pass `current_release_build` from the release-builds manifest
// when summarising the release channel; pass nil for "all".
static NSDictionary* xe_compat_summarize_reports_for_channel(NSArray<NSDictionary*>* reports,
                                                             NSDictionary* current_release_build,
                                                             NSString* channel) {
  NSMutableArray<NSDictionary*>* matching = [NSMutableArray arrayWithCapacity:reports.count];
  if ([channel isEqualToString:@"all"]) {
    for (id raw in reports) {
      NSDictionary* report = xe_dictionary_from_object(raw);
      if (report) {
        [matching addObject:report];
      }
    }
  } else {
    for (id raw in reports) {
      NSDictionary* report = xe_dictionary_from_object(raw);
      if (report && xe_compat_release_filter_matches(report, current_release_build)) {
        [matching addObject:report];
      }
    }
  }

  NSDictionary* best_report = xe_compat_best_report(matching);
  NSDictionary* latest_report = xe_compat_latest_report(matching);

  NSMutableDictionary* summary = [NSMutableDictionary dictionary];
  summary[@"channel"] = channel ?: @"all";
  summary[@"status"] =
      best_report ? (xe_string_from_object(best_report[@"status"]) ?: @"untested") : @"untested";
  NSString* best_perf = xe_string_from_object(best_report[@"perf"]);
  summary[@"perf"] = best_perf.length > 0 ? best_perf : (id)[NSNull null];
  summary[@"notes"] = xe_string_from_object(latest_report[@"notes"]) ?: @"";
  NSString* latest_date = xe_string_from_object(latest_report[@"date"]);
  summary[@"updatedAt"] = latest_date.length > 0 ? latest_date : (id)[NSNull null];
  summary[@"date"] = summary[@"updatedAt"];
  summary[@"reportCount"] = @(matching.count);
  summary[@"latestReport"] = latest_report ?: (id)[NSNull null];
  summary[@"bestReport"] = best_report ?: (id)[NSNull null];

  if (best_report) {
    for (NSString* key in @[ @"device", @"platform", @"osVersion", @"arch", @"gpuBackend" ]) {
      NSString* value = xe_string_from_object(best_report[key]);
      if (value.length > 0) {
        summary[key] = value;
      }
    }
    NSDictionary* build_info = xe_dictionary_from_object(best_report[@"build"]);
    if (build_info) {
      summary[@"build"] = build_info;
    }
  }
  return summary;
}

// Worker-built summaries leave device / platform / osVersion / arch /
// gpuBackend / build inside the nested bestReport (and latestReport) so the
// summary stays compact. The iOS hero card reads those at the top level —
// flatten them when handing the dict to UI code so older locally-derived
// summaries and worker-built summaries look the same to callers.
static NSDictionary* xe_compat_flatten_summary_detail_fields(NSDictionary* summary) {
  if (!summary) {
    return nil;
  }
  NSDictionary* best = xe_dictionary_from_object(summary[@"bestReport"]);
  NSDictionary* latest = xe_dictionary_from_object(summary[@"latestReport"]);
  NSDictionary* detail = best ?: latest;
  if (!detail) {
    return summary;
  }
  NSMutableDictionary* flattened = [[summary mutableCopy] autorelease];
  for (NSString* key in
       @[ @"device", @"deviceMachine", @"platform", @"osVersion", @"arch", @"gpuBackend" ]) {
    if (xe_string_from_object(flattened[key]).length > 0) {
      continue;
    }
    NSString* value = xe_string_from_object(detail[key]);
    if (value.length > 0) {
      flattened[key] = value;
    }
  }
  if (!xe_dictionary_from_object(flattened[@"build"])) {
    NSDictionary* build = xe_dictionary_from_object(detail[@"build"]);
    if (build) {
      flattened[@"build"] = build;
    }
  }
  return flattened;
}

// Status presence check: "untested" passes xe_compat_entry_has_summary_fields
// (it's a non-empty string), but it carries no useful information. The
// preferred-summary picker uses this to skip past untested release summaries
// when a populated all-channel summary is available.
static BOOL xe_compat_summary_has_status_data(NSDictionary* summary) {
  if (!summary) {
    return NO;
  }
  NSString* status = xe_string_from_object(summary[@"status"]);
  return status.length > 0 && ![status isEqualToString:@"untested"];
}

static BOOL xe_compat_summary_is_untested(NSDictionary* summary) {
  return [xe_string_from_object(summary[@"status"]) isEqualToString:@"untested"];
}

static BOOL xe_compat_platform_is_ios(NSString* platform) {
  return platform.length > 0 && [platform caseInsensitiveCompare:@"ios"] == NSOrderedSame;
}

static BOOL xe_compat_report_is_ios(NSDictionary* report) {
  return xe_compat_platform_is_ios(xe_string_from_object(report[@"platform"]));
}

static BOOL xe_compat_summary_is_ios_report_backed(NSDictionary* summary) {
  if (!summary) {
    return NO;
  }

  BOOL has_nested_report = NO;
  NSDictionary* best = xe_dictionary_from_object(summary[@"bestReport"]);
  if (best) {
    if (!xe_compat_report_is_ios(best)) {
      return NO;
    }
    has_nested_report = YES;
  }
  NSDictionary* latest = xe_dictionary_from_object(summary[@"latestReport"]);
  if (latest) {
    if (!xe_compat_report_is_ios(latest)) {
      return NO;
    }
    has_nested_report = YES;
  }
  if (has_nested_report) {
    return YES;
  }

  return xe_compat_platform_is_ios(xe_string_from_object(summary[@"platform"]));
}

static NSArray<NSDictionary*>* xe_compat_ios_reports_from_compat_info(NSDictionary* compat_info) {
  NSArray* raw_reports = compat_info[@"reports"];
  if (![raw_reports isKindOfClass:[NSArray class]]) {
    return nil;
  }

  NSMutableArray<NSDictionary*>* ios_reports = [NSMutableArray arrayWithCapacity:raw_reports.count];
  for (id raw in raw_reports) {
    NSDictionary* report = xe_dictionary_from_object(raw);
    if (report && xe_compat_report_is_ios(report)) {
      [ios_reports addObject:report];
    }
  }
  return ios_reports;
}

static NSDictionary* xe_compat_summary_from_ios_reports_for_channel(NSDictionary* compat_info,
                                                                    NSString* channel) {
  NSArray<NSDictionary*>* ios_reports = xe_compat_ios_reports_from_compat_info(compat_info);
  if (ios_reports.count == 0) {
    return nil;
  }

  NSDictionary* derived = xe_compat_summarize_reports_for_channel(ios_reports, nil, channel);
  return xe_compat_entry_has_summary_fields(derived) ? derived : nil;
}

static NSDictionary* xe_compat_untested_summary(NSString* channel) {
  NSMutableDictionary* summary = [NSMutableDictionary dictionary];
  summary[@"channel"] = channel ?: @"release";
  summary[@"status"] = @"untested";
  summary[@"perf"] = (id)[NSNull null];
  summary[@"notes"] = @"";
  summary[@"reportCount"] = @0;
  summary[@"latestReport"] = (id)[NSNull null];
  summary[@"bestReport"] = (id)[NSNull null];
  return summary;
}

NSDictionary* xe_ios_public_release_summary_from_compat_info(NSDictionary* compat_info) {
  // Keep iOS launcher compatibility scoped to iOS reports. The website feed's
  // prebuilt summaries may be global across Apple platforms, so only trust
  // them here when the report payload is iOS-backed or explicitly untested.
  // Otherwise derive the release summary from raw iOS reports using the same
  // worker algorithm.
  NSDictionary* summaries = xe_dictionary_from_object(compat_info[@"summaries"]);
  NSDictionary* prebuilt_release = xe_dictionary_from_object(summaries[@"release"]);
  if (xe_compat_entry_has_summary_fields(prebuilt_release) &&
      (xe_compat_summary_is_untested(prebuilt_release) ||
       xe_compat_summary_is_ios_report_backed(prebuilt_release))) {
    return xe_compat_flatten_summary_detail_fields(prebuilt_release);
  }

  NSDictionary* derived = xe_compat_summary_from_ios_reports_for_channel(compat_info, @"release");
  if (derived) {
    return derived;
  }

  return xe_compat_entry_has_summary_fields(prebuilt_release)
             ? xe_compat_untested_summary(@"release")
             : nil;
}

NSDictionary* xe_preferred_summary_from_compat_info(NSDictionary* compat_info) {
  // Match the release verdict when it has actual iOS data, but fall back to
  // iOS all-channel reports when release is untested. Never use populated
  // macOS/global summaries for iOS badges or details.
  NSDictionary* summaries = xe_dictionary_from_object(compat_info[@"summaries"]);
  NSDictionary* release_summary = xe_dictionary_from_object(summaries[@"release"]);
  NSDictionary* all_summary = xe_dictionary_from_object(summaries[@"all"]);
  BOOL has_prebuilt_summaries = summaries.count > 0;
  if (xe_compat_summary_has_status_data(release_summary) &&
      xe_compat_summary_is_ios_report_backed(release_summary)) {
    return xe_compat_flatten_summary_detail_fields(release_summary);
  }

  NSDictionary* ios_release_summary =
      xe_compat_summary_from_ios_reports_for_channel(compat_info, @"release");
  if (xe_compat_summary_has_status_data(ios_release_summary)) {
    return ios_release_summary;
  }

  NSDictionary* ios_all_summary =
      xe_compat_summary_from_ios_reports_for_channel(compat_info, @"all");
  if (xe_compat_summary_has_status_data(ios_all_summary)) {
    return ios_all_summary;
  }

  if (xe_compat_summary_has_status_data(all_summary) &&
      xe_compat_summary_is_ios_report_backed(all_summary)) {
    return xe_compat_flatten_summary_detail_fields(all_summary);
  }

  // No populated iOS summary anywhere. Surface an untested release summary
  // rather than leaking a populated macOS/global verdict into the iOS UI.
  if (xe_compat_entry_has_summary_fields(release_summary) &&
      (xe_compat_summary_is_untested(release_summary) ||
       xe_compat_summary_is_ios_report_backed(release_summary))) {
    return xe_compat_flatten_summary_detail_fields(release_summary);
  }
  if (ios_release_summary) {
    return ios_release_summary;
  }
  if (xe_compat_entry_has_summary_fields(all_summary) &&
      (xe_compat_summary_is_untested(all_summary) ||
       xe_compat_summary_is_ios_report_backed(all_summary))) {
    return xe_compat_flatten_summary_detail_fields(all_summary);
  }

  if (has_prebuilt_summaries) {
    return xe_compat_untested_summary(@"release");
  }

  // Older feeds carry a top-level status/perf/notes block with no nested
  // summaries dictionary; treat the entry itself as the summary in that case.
  NSString* legacy_platform = xe_string_from_object(compat_info[@"platform"]);
  if (xe_compat_entry_has_summary_fields(compat_info) &&
      (legacy_platform.length == 0 || xe_compat_platform_is_ios(legacy_platform))) {
    return compat_info;
  }

  NSDictionary* derived = xe_ios_public_release_summary_from_compat_info(compat_info);
  if (derived) {
    return derived;
  }
  return xe_compat_summary_named(compat_info, @"preview");
}
