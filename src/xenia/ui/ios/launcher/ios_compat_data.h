/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IOS_COMPAT_DATA_H_
#define XENIA_UI_IOS_COMPAT_DATA_H_

#import <UIKit/UIKit.h>

#include <cstdint>

NS_ASSUME_NONNULL_BEGIN

// Compatibility data layer for the iOS launcher.
//
// Loads and caches the public games.json from xenios.jp / the Cloudflare
// worker, merges in reports sent from this device, and exposes formatting helpers
// for status / perf badges, build labels, hero heights and platform strings
// used by the compatibility hero card and report sheets.
//
// On-disk caches:
//   Library/Caches/compat-data.json
//   Library/Caches/discussion-{title_id_hex_upper}.json
//
// Posts kXeniaCompatDataDidUpdateNotification when the cached games list is
// updated, and kXeniaDiscussionDidUpdateNotification after a discussion
// snapshot is written.

extern NSString* const kXeniaCompatDataDidUpdateNotification;
extern NSString* const kXeniaDiscussionDidUpdateNotification;

// JSON utility helpers (used widely across the compat layer).
NSString* xe_string_from_object(id value);
NSDictionary* xe_dictionary_from_object(id value);

// Status / perf appearance + display.
UIColor* xe_compat_status_color(NSString* status);
NSString* xe_compat_status_label(NSString* status);
UIColor* xe_compat_perf_color(NSString* perf);
NSString* xe_compat_perf_label(NSString* perf);
NSArray<NSString*>* xe_compat_statuses(void);
NSArray<NSString*>* xe_compat_status_labels(void);
NSArray<NSString*>* xe_compat_perfs(void);
NSArray<NSString*>* xe_compat_perf_labels(void);

// The capsule beside Play on a game's page: a dot in the status's colour, the verdict, and how
// well it runs under it. Tapping it shows what the reports say and opens them all.
UIButton* xe_new_compat_capsule(void);
void xe_update_compat_capsule(UIButton* capsule, NSString* _Nullable status,
                              NSString* _Nullable perf, NSString* _Nullable notes,
                              void (^reports)(void));

// Channel labels used in the build pill.

// Build labels.
NSString* xe_compat_build_label(NSDictionary* build_info);
NSString* xe_user_facing_build_label(NSDictionary* build_info);
NSDictionary* xe_compat_build_info_from_entry(NSDictionary* entry);
NSDictionary* xe_current_compat_report_build_info(void);

// Date / platform formatting.
NSString* xe_format_iso_date(NSString* iso);
NSString* xe_platform_display_text(NSString* platform, NSString* os_version);

// Layout helper for the compatibility hero card.
CGFloat xe_compat_hero_height_for_width(CGFloat width, UIImage* background_art);

// Summary derivation – one verdict from every iOS report, as xenios.jp does.
NSDictionary* xe_preferred_summary_from_compat_info(NSDictionary* compat_info);

// Cache + discussion paths.
NSString* xe_discussion_cache_path(uint32_t title_id);

// Building + persisting reports for compatibility submissions.
NSDictionary* xe_build_local_compat_report(NSString* device, NSString* device_machine,
                                           NSString* os_version, NSString* status, NSString* perf,
                                           NSString* notes, NSDictionary* build_info);
NSDictionary* xe_update_cached_compat_entry_for_submission(uint32_t title_id, NSString* title,
                                                           NSDictionary* report,
                                                           NSString* issue_url);
NSDictionary* xe_cache_discussion_snapshot_for_submission(uint32_t title_id,
                                                          NSDictionary* compat_entry);

// Cache load / fetch.
NSDictionary* xe_load_cached_compat_data(void);
void xe_fetch_compat_data(void (^completion)(NSDictionary* _Nullable data));

NS_ASSUME_NONNULL_END

#endif  // XENIA_UI_IOS_COMPAT_DATA_H_
