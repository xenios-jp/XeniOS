/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import "xenia/ui/ios/launcher/ios_zar_conversion_coordinator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>

#include "xenia/base/logging.h"
#include "xenia/vfs/zar_converter.h"

#import "xenia/ui/ios/launcher/ios_game_library_store.h"
#import "xenia/ui/ios/shared/ios_theme.h"
#import "xenia/ui/ios/shared/ios_view_helpers.h"

using xe::ui::IOSDiscoveredGame;

namespace {

NSString* const kXeniaPendingZarConversionDefaultsKey = @"ios_pending_zar_conversion";
NSString* const kXeniaPendingZarSourcePathKey = @"sourcePath";
NSString* const kXeniaPendingZarOutputPathKey = @"outputPath";
NSString* const kXeniaPendingZarOutputNameKey = @"outputName";
NSString* const kXeniaPendingZarSourceWasExternalKey = @"sourceWasExternal";
NSString* const kXeniaPendingZarStartedAtKey = @"startedAt";

// Number of parallel ZSTD block-compression workers for ZAR conversion. Prefer
// the physical core count, fall back to the logical count, and cap so the
// serializer + producer + UI still get scheduled. perflevel0 is read only as an
// opportunistic sanity bound, never relied on alone.
unsigned XEZarRecommendedCompressionThreads() {
  auto read_sysctl = [](const char* name) -> unsigned {
    int value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value > 0) {
      return static_cast<unsigned>(value);
    }
    return 0;
  };
  unsigned cores = read_sysctl("hw.physicalcpu");
  if (cores == 0) {
    cores = std::thread::hardware_concurrency();
  }
  if (cores == 0) {
    cores = 2;
  }
  constexpr unsigned kMaxWorkers = 6;
  if (cores > kMaxWorkers) {
    cores = kMaxWorkers;
  }
  return cores;
}

// Runs at the top of every ZAR compression pool thread. Match the conversion's
// user-initiated QoS so the foreground progress UI stays responsive, and give
// the threads a stable name for Instruments traces.
void XEZarInitCompressionThread() {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
  pthread_setname_np("xe-zar-compress");
}

enum class XeniaIOSZarConversionCleanupMode {
  kKeepOriginals,
  kAskAfterSuccess,
  kCleanupAfterSuccess,
  kDeleteOriginalsAfterSuccess,
};

const char* XEZarCleanupModeName(XeniaIOSZarConversionCleanupMode mode) {
  switch (mode) {
    case XeniaIOSZarConversionCleanupMode::kKeepOriginals:
      return "keep_originals";
    case XeniaIOSZarConversionCleanupMode::kAskAfterSuccess:
      return "ask_after_success";
    case XeniaIOSZarConversionCleanupMode::kCleanupAfterSuccess:
      return "cleanup_after_success";
    case XeniaIOSZarConversionCleanupMode::kDeleteOriginalsAfterSuccess:
      return "delete_originals_after_success";
  }
  return "unknown";
}

const char* XEZarBoolString(bool value) { return value ? "true" : "false"; }

std::filesystem::path XEZarWeaklyCanonicalOrAbsolute(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return canonical;
  }
  ec.clear();
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return ec ? path : absolute;
}

bool XEZarPathIsInDirectory(const std::filesystem::path& path,
                            const std::filesystem::path& directory) {
  auto path_it = path.begin();
  auto directory_it = directory.begin();
  for (; directory_it != directory.end(); ++directory_it, ++path_it) {
    if (path_it == path.end() || *path_it != *directory_it) {
      return false;
    }
  }
  return true;
}

bool XEZarPathMatchesAddedPath(const std::filesystem::path& path,
                               const std::filesystem::path& added_path) {
  if (path.empty() || added_path.empty()) {
    return false;
  }
  return XEZarPathIsInDirectory(XEZarWeaklyCanonicalOrAbsolute(path),
                                XEZarWeaklyCanonicalOrAbsolute(added_path));
}

const char* XEZarSourceKindForLog(const std::filesystem::path& path) {
  if (xe::ui::IsZarPath(path)) {
    return "zar";
  }
  if (xe::ui::IsISOPath(path)) {
    return "iso";
  }
  if (xe::ui::IsDefaultXexPath(path)) {
    return "xex";
  }
  if (xe::ui::IsLikelyGodContainerFile(path)) {
    return "god";
  }
  return "unknown";
}

struct XeniaIOSZarConversionRequest {
  std::filesystem::path source_path;
  std::filesystem::path output_path;
  std::string output_name;
  std::string display_name;
  uint64_t source_bytes = 0;
  uint64_t estimated_zar_bytes = 0;
  uint64_t estimated_input_bytes = 0;
  bool estimate_available = false;
  bool likely_source_external = false;
  bool output_in_place = false;
};

struct XeniaIOSZarConversionItemResult {
  XeniaIOSZarConversionRequest request;
  xe::vfs::ZarConversionResult conversion;
  uint64_t output_bytes = 0;
  bool source_was_external = false;
  bool cleanup_attempted = false;
  bool cleanup_succeeded = false;
  bool skipped_after_cancel = false;
  std::string cleanup_message;
};

std::string TrimAscii(std::string value) {
  size_t start = 0;
  while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  size_t end = value.size();
  while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string SanitizedZarBaseName(std::string value, const std::filesystem::path& fallback_path) {
  value = TrimAscii(std::move(value));
  if (value.empty()) {
    value = fallback_path.stem().string();
  }
  if (value.empty() || value == "default") {
    value = fallback_path.parent_path().filename().string();
  }

  for (char& c : value) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|') {
      c = '_';
    }
  }
  value = TrimAscii(std::move(value));
  while (!value.empty() && (value.back() == '.' || value.back() == ' ')) {
    value.pop_back();
  }
  return value.empty() ? "Game" : value;
}

bool PathIsReserved(const std::filesystem::path& path,
                    const std::vector<std::filesystem::path>& reserved_paths) {
  return std::find(reserved_paths.begin(), reserved_paths.end(), path) != reserved_paths.end();
}

std::filesystem::path UniqueZarOutputPath(
    const std::filesystem::path& directory, const std::string& base_name,
    const std::vector<std::filesystem::path>& reserved_paths = {}) {
  std::filesystem::path candidate = directory / (base_name + ".zar");
  std::error_code ec;
  if (!std::filesystem::exists(candidate, ec) && !PathIsReserved(candidate, reserved_paths)) {
    return candidate;
  }
  for (uint32_t index = 2; index < 1000; ++index) {
    candidate = directory / (base_name + " " + std::to_string(index) + ".zar");
    ec.clear();
    if (!std::filesystem::exists(candidate, ec) && !PathIsReserved(candidate, reserved_paths)) {
      return candidate;
    }
  }
  return directory / (base_name + " copy.zar");
}

NSString* XEZarFormatByteCount(uint64_t bytes) {
  return [NSByteCountFormatter stringFromByteCount:static_cast<long long>(bytes)
                                        countStyle:NSByteCountFormatterCountStyleFile];
}

// Coarse, succinct "time remaining" phrasing for the conversion progress UI.
// Returns nil when an estimate is not meaningful (not enough data, finished).
NSString* XEZarFormatTimeRemaining(double seconds) {
  if (!(seconds > 0.0) || std::isinf(seconds) || std::isnan(seconds)) {
    return nil;
  }
  if (seconds <= 5.0) {
    return @"A few seconds remaining";
  }
  if (seconds < 60.0) {
    long rounded = static_cast<long>(std::llround(seconds / 5.0)) * 5;
    if (rounded < 5) {
      rounded = 5;
    }
    return [NSString stringWithFormat:@"About %ld seconds remaining", rounded];
  }
  if (seconds < 3600.0) {
    long minutes = static_cast<long>(std::ceil(seconds / 60.0));
    return [NSString
        stringWithFormat:@"About %ld minute%@ remaining", minutes, minutes == 1 ? @"" : @"s"];
  }
  long hours = static_cast<long>(std::llround(seconds / 3600.0));
  if (hours < 1) {
    hours = 1;
  }
  return [NSString stringWithFormat:@"About %ld hour%@ remaining", hours, hours == 1 ? @"" : @"s"];
}

NSString* const kXEZarSummaryRowTitleKey = @"title";
NSString* const kXEZarSummaryRowDetailKey = @"detail";
NSString* const kXEZarSummaryRowSymbolKey = @"symbol";
NSString* const kXEZarSummaryRowStyleKey = @"style";
NSString* const kXEZarSummaryRowStyleSuccess = @"success";
NSString* const kXEZarSummaryRowStyleWarning = @"warning";
NSString* const kXEZarSummaryRowStyleError = @"error";
NSString* const kXEZarSummaryRowStyleMuted = @"muted";

UIColor* XEZarSummaryRowTint(NSString* style) {
  if ([style isEqualToString:kXEZarSummaryRowStyleSuccess]) {
    return [XeniaTheme accent];
  }
  if ([style isEqualToString:kXEZarSummaryRowStyleWarning]) {
    return [XeniaTheme statusWarning];
  }
  if ([style isEqualToString:kXEZarSummaryRowStyleError]) {
    return [XeniaTheme statusError];
  }
  return [XeniaTheme textMuted];
}

NSString* XEZarResultDisplayName(const XeniaIOSZarConversionItemResult& item) {
  if (!item.request.display_name.empty()) {
    return ToNSString(item.request.display_name);
  }
  if (!item.request.output_name.empty()) {
    return ToNSString(item.request.output_name);
  }
  return ToNSString(item.request.source_path.filename().string());
}

NSString* XEZarResultOutputName(const XeniaIOSZarConversionItemResult& item) {
  if (!item.request.output_name.empty()) {
    return ToNSString(item.request.output_name);
  }
  return ToNSString(item.request.output_path.filename().string());
}

std::filesystem::path XEZarExternalDeletePath(const std::filesystem::path& source_path);

NSString* XEZarResultSourceName(const XeniaIOSZarConversionItemResult& item) {
  std::filesystem::path source_path = XEZarExternalDeletePath(item.request.source_path);
  if (source_path.empty()) {
    source_path = item.request.source_path;
  }
  return ToNSString(source_path.filename().string());
}

void XEZarAddSummaryRow(NSMutableArray<NSDictionary<NSString*, NSString*>*>* rows, NSString* title,
                        NSString* detail, NSString* symbol, NSString* style) {
  [rows addObject:@{
    kXEZarSummaryRowTitleKey : title ?: @"",
    kXEZarSummaryRowDetailKey : detail ?: @"",
    kXEZarSummaryRowSymbolKey : symbol ?: @"circle",
    kXEZarSummaryRowStyleKey : style ?: kXEZarSummaryRowStyleMuted,
  }];
}

void XEZarAppendSkippedAfterCancelResults(const std::vector<XeniaIOSZarConversionRequest>& requests,
                                          size_t first_index,
                                          std::vector<XeniaIOSZarConversionItemResult>* results) {
  if (!results || first_index >= requests.size()) {
    return;
  }
  for (size_t index = first_index; index < requests.size(); ++index) {
    XeniaIOSZarConversionItemResult skipped_item;
    skipped_item.request = requests[index];
    skipped_item.skipped_after_cancel = true;
    skipped_item.conversion.cancelled = true;
    skipped_item.conversion.error_message = "Not started because cancellation was requested.";
    results->push_back(std::move(skipped_item));
  }
}

uint64_t XEZarPathStorageBytes(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) {
    uint64_t total = 0;
    std::filesystem::recursive_directory_iterator it(
        path, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
      return 0;
    }
    for (std::filesystem::recursive_directory_iterator end; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      const std::filesystem::directory_entry& entry = *it;
      if (!entry.is_regular_file(ec)) {
        ec.clear();
        continue;
      }
      const uintmax_t file_size = entry.file_size(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      total += static_cast<uint64_t>(file_size);
    }
    return total;
  }
  ec.clear();
  if (std::filesystem::is_regular_file(path, ec)) {
    const uintmax_t file_size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(file_size);
  }
  return 0;
}

uint64_t XEZarSourceStorageBytes(const std::filesystem::path& source_path) {
  if (xe::ui::IsDefaultXexPath(source_path) || xe::ui::IsDefaultXbePath(source_path)) {
    return XEZarPathStorageBytes(source_path.parent_path());
  }
  uint64_t total = XEZarPathStorageBytes(source_path);
  if (xe::ui::IsLikelyGodContainerFile(source_path)) {
    std::filesystem::path sidecar_path = source_path;
    sidecar_path += ".data";
    total += XEZarPathStorageBytes(sidecar_path);
  }
  return total;
}

uint64_t XEZarOutputStorageBytes(const std::filesystem::path& output_path) {
  return XEZarPathStorageBytes(output_path);
}

void XEZarRefreshConversionRequestEstimate(XeniaIOSZarConversionRequest* request) {
  if (!request || request->source_path.empty()) {
    return;
  }

  XELOGI("iOS ZAR: estimating source='{}' output='{}' kind={} external={} "
         "in_place={}",
         request->source_path.string(), request->output_path.string(),
         XEZarSourceKindForLog(request->source_path),
         XEZarBoolString(request->likely_source_external),
         XEZarBoolString(request->output_in_place));
  NSError* external_access_error = nil;
  BOOL matched_external_location = NO;
  XeniaIOSExternalLibraryAccess* external_access = [xe::ui::StartIOSExternalLibraryAccessForPath(
      request->source_path, &matched_external_location, &external_access_error) retain];
  if (matched_external_location && !external_access) {
    XELOGI("iOS ZAR: estimate failed, external access unavailable source='{}' "
           "error='{}'",
           request->source_path.string(),
           external_access_error.localizedDescription
               ? [external_access_error.localizedDescription UTF8String]
               : "unknown");
    request->source_bytes = 0;
    request->estimated_zar_bytes = 0;
    request->estimated_input_bytes = 0;
    request->estimate_available = false;
    return;
  }

  request->source_bytes = XEZarSourceStorageBytes(request->source_path);
  const xe::vfs::ZarConversionEstimate estimate =
      xe::vfs::EstimatePathToZar(request->source_path, request->output_path);
  request->estimated_input_bytes = estimate.input_bytes;
  request->estimated_zar_bytes = estimate.estimated_output_bytes;
  request->estimate_available = estimate.success && estimate.estimated_output_bytes > 0;
  XELOGI("iOS ZAR: estimate result source='{}' success={} source_bytes={} "
         "input_bytes={} estimated_output_bytes={} sampled_input_bytes={} "
         "sampled_output_bytes={} files_seen={} error='{}'",
         request->source_path.string(), XEZarBoolString(estimate.success), request->source_bytes,
         request->estimated_input_bytes, request->estimated_zar_bytes, estimate.sampled_input_bytes,
         estimate.sampled_output_bytes, estimate.files_seen, estimate.error_message);
  [external_access release];
}

NSString* XEZarSpaceDeltaSummary(uint64_t source_bytes, uint64_t output_bytes) {
  if (!source_bytes || !output_bytes) {
    return @"Space saved could not be estimated.";
  }
  if (source_bytes >= output_bytes) {
    const uint64_t saved_bytes = source_bytes - output_bytes;
    return [NSString stringWithFormat:@"Saved %@ (%@ -> %@).", XEZarFormatByteCount(saved_bytes),
                                      XEZarFormatByteCount(source_bytes),
                                      XEZarFormatByteCount(output_bytes)];
  }
  return [NSString stringWithFormat:@"Archive is %@ larger (%@ -> %@).",
                                    XEZarFormatByteCount(output_bytes - source_bytes),
                                    XEZarFormatByteCount(source_bytes),
                                    XEZarFormatByteCount(output_bytes)];
}

NSString* XEZarEstimatedSpaceDeltaSummary(uint64_t source_bytes, uint64_t output_bytes) {
  if (!source_bytes || !output_bytes) {
    return @"Estimated savings unavailable.";
  }
  if (source_bytes >= output_bytes) {
    const uint64_t saved_bytes = source_bytes - output_bytes;
    return [NSString
        stringWithFormat:@"Estimated savings: %@ (%@ -> %@).", XEZarFormatByteCount(saved_bytes),
                         XEZarFormatByteCount(source_bytes), XEZarFormatByteCount(output_bytes)];
  }
  return [NSString stringWithFormat:@"Estimated .zar is %@ larger (%@ -> %@).",
                                    XEZarFormatByteCount(output_bytes - source_bytes),
                                    XEZarFormatByteCount(source_bytes),
                                    XEZarFormatByteCount(output_bytes)];
}

// Builds the table row for one finished item. The title is the game name; the
// detail leads with the space saved (or the failure/cancel reason) so each row
// is self-explanatory both live during conversion and in the final results.
NSDictionary<NSString*, NSString*>* XEZarResultRowForItem(
    const XeniaIOSZarConversionItemResult& item, XeniaIOSZarConversionCleanupMode cleanup_mode) {
  NSString* name = XEZarResultDisplayName(item) ?: @"Game";
  NSString* source_name = XEZarResultSourceName(item) ?: @"Source";
  NSMutableArray<NSString*>* detail_parts = [NSMutableArray array];
  NSString* symbol = @"circle";
  NSString* style = kXEZarSummaryRowStyleMuted;

  if (item.skipped_after_cancel) {
    [detail_parts addObject:@"Not started. Cancelled before reaching this item."];
    [detail_parts addObject:[NSString stringWithFormat:@"Source: %@", source_name]];
    symbol = @"clock";
    style = kXEZarSummaryRowStyleMuted;
  } else if (item.conversion.cancelled) {
    [detail_parts addObject:@"Cancelled. Partial .zar output was removed."];
    if (item.conversion.files_written > 0 || item.conversion.bytes_written > 0) {
      [detail_parts
          addObject:[NSString
                        stringWithFormat:@"%u file(s), %@ packed before cancellation.",
                                         item.conversion.files_written,
                                         XEZarFormatByteCount(item.conversion.bytes_written)]];
    }
    [detail_parts addObject:[NSString stringWithFormat:@"Source: %@", source_name]];
    symbol = @"xmark.circle";
    style = kXEZarSummaryRowStyleWarning;
  } else if (!item.conversion.success) {
    NSString* error = item.conversion.error_message.empty()
                          ? @"Conversion failed."
                          : ToNSString(item.conversion.error_message);
    [detail_parts addObject:(error ?: @"Conversion failed.")];
    [detail_parts addObject:[NSString stringWithFormat:@"Source: %@", source_name]];
    symbol = @"exclamationmark.triangle";
    style = kXEZarSummaryRowStyleError;
  } else {
    if (item.request.source_bytes > 0 && item.output_bytes > 0) {
      [detail_parts addObject:XEZarSpaceDeltaSummary(item.request.source_bytes, item.output_bytes)];
    } else if (item.output_bytes > 0) {
      [detail_parts addObject:[NSString stringWithFormat:@"Output size: %@.",
                                                         XEZarFormatByteCount(item.output_bytes)]];
    } else {
      [detail_parts addObject:@"Conversion complete."];
    }
    [detail_parts addObject:[NSString stringWithFormat:@"Created %@.", XEZarResultOutputName(item)
                                                                           ?: @"archive.zar"]];
    symbol = @"checkmark.circle";
    style = kXEZarSummaryRowStyleSuccess;
    if (item.cleanup_attempted) {
      NSString* cleanup_message = item.cleanup_message.empty() ? @"Original cleanup finished."
                                                               : ToNSString(item.cleanup_message);
      if (item.cleanup_succeeded) {
        [detail_parts addObject:(cleanup_message ?: @"Original cleanup finished.")];
        symbol = cleanup_mode == XeniaIOSZarConversionCleanupMode::kDeleteOriginalsAfterSuccess
                     ? @"trash.circle"
                     : @"checkmark.circle";
      } else {
        [detail_parts addObject:[NSString stringWithFormat:@"Cleanup failed: %@",
                                                           (cleanup_message ?: @"Unknown error.")]];
        symbol = @"exclamationmark.triangle";
        style = kXEZarSummaryRowStyleWarning;
      }
    }
  }

  return @{
    kXEZarSummaryRowTitleKey : name ?: @"",
    kXEZarSummaryRowDetailKey : [detail_parts componentsJoinedByString:@"\n"] ?: @"",
    kXEZarSummaryRowSymbolKey : symbol ?: @"circle",
    kXEZarSummaryRowStyleKey : style ?: kXEZarSummaryRowStyleMuted,
  };
}

void StorePendingZarConversionRecord(const std::filesystem::path& source_path,
                                     const std::filesystem::path& output_path,
                                     NSString* output_name, BOOL source_was_external) {
  if (source_path.empty() || output_path.empty()) {
    return;
  }
  NSString* effective_output_name =
      output_name.length > 0 ? output_name : ToNSString(output_path.filename().string());
  NSDictionary* record = @{
    kXeniaPendingZarSourcePathKey : ToNSString(source_path.string()),
    kXeniaPendingZarOutputPathKey : ToNSString(output_path.string()),
    kXeniaPendingZarOutputNameKey : effective_output_name ?: @"Converted Game.zar",
    kXeniaPendingZarSourceWasExternalKey : @(source_was_external),
    kXeniaPendingZarStartedAtKey : @([[NSDate date] timeIntervalSince1970]),
  };
  [[NSUserDefaults standardUserDefaults] setObject:record
                                            forKey:kXeniaPendingZarConversionDefaultsKey];
}

NSDictionary* LoadPendingZarConversionRecord() {
  id record =
      [[NSUserDefaults standardUserDefaults] objectForKey:kXeniaPendingZarConversionDefaultsKey];
  return [record isKindOfClass:[NSDictionary class]] ? (NSDictionary*)record : nil;
}

void ClearPendingZarConversionRecord() {
  [[NSUserDefaults standardUserDefaults] removeObjectForKey:kXeniaPendingZarConversionDefaultsKey];
}

bool XEZarRequestAlreadyIncludesSource(const std::vector<XeniaIOSZarConversionRequest>& requests,
                                       const std::filesystem::path& source_path) {
  return std::any_of(requests.begin(), requests.end(),
                     [&](const XeniaIOSZarConversionRequest& request) {
                       return request.source_path == source_path;
                     });
}

std::filesystem::path XEZarExternalDeletePath(const std::filesystem::path& source_path) {
  if (xe::ui::IsDefaultXexPath(source_path) || xe::ui::IsDefaultXbePath(source_path)) {
    return source_path.parent_path();
  }
  return source_path;
}

std::filesystem::path XEZarInPlaceSourceContainerPath(const std::filesystem::path& source_path) {
  if (xe::ui::IsDefaultXexPath(source_path) || xe::ui::IsDefaultXbePath(source_path)) {
    return source_path.parent_path();
  }
  if (xe::ui::IsLikelyGodContainerFile(source_path)) {
    const std::filesystem::path content_directory = source_path.parent_path();
    if (!content_directory.empty() && !content_directory.parent_path().empty()) {
      return content_directory.parent_path();
    }
  }
  return source_path;
}

std::filesystem::path XEZarInPlaceOutputDirectory(const std::filesystem::path& source_path) {
  const std::filesystem::path container_path = XEZarInPlaceSourceContainerPath(source_path);
  return container_path.empty() ? source_path.parent_path() : container_path.parent_path();
}

bool XEZarAddConversionRequest(const std::filesystem::path& source_path,
                               const std::string& display_name, const std::string& base_name_hint,
                               bool likely_source_external, bool output_in_place,
                               std::vector<std::filesystem::path>* reserved_paths,
                               std::vector<XeniaIOSZarConversionRequest>* requests) {
  if (!requests || !reserved_paths) {
    return false;
  }
  if (source_path.empty()) {
    XELOGI("iOS ZAR: skipped conversion request with empty source");
    return false;
  }
  if (xe::ui::IsZarPath(source_path)) {
    XELOGI("iOS ZAR: skipped existing ZAR source='{}'", source_path.string());
    return false;
  }
  if (XEZarRequestAlreadyIncludesSource(*requests, source_path)) {
    XELOGI("iOS ZAR: skipped duplicate source='{}'", source_path.string());
    return false;
  }

  XeniaIOSZarConversionRequest request;
  request.source_path = source_path;
  request.display_name = display_name.empty() ? source_path.stem().string() : display_name;
  std::filesystem::path output_directory = xe::ui::IOSImportedGamesDirectory();
  if (output_in_place) {
    NSError* access_error = nil;
    BOOL matched_external_location = NO;
    XeniaIOSExternalLibraryAccess* external_access = [xe::ui::StartIOSExternalLibraryAccessForPath(
        source_path, &matched_external_location, &access_error) retain];
    if (likely_source_external && (!matched_external_location || !external_access)) {
      XELOGI("iOS ZAR: skipped in-place request source='{}' likely_external={} "
             "matched_external={} access_granted={} error='{}'",
             source_path.string(), XEZarBoolString(likely_source_external),
             XEZarBoolString(matched_external_location), XEZarBoolString(external_access != nil),
             access_error.localizedDescription ? [access_error.localizedDescription UTF8String]
                                               : "none");
      [external_access release];
      return false;
    }
    output_directory = XEZarInPlaceOutputDirectory(source_path);
    [external_access release];
  }
  request.output_path = UniqueZarOutputPath(
      output_directory, SanitizedZarBaseName(base_name_hint, source_path), *reserved_paths);
  request.output_name = request.output_path.filename().string();
  request.likely_source_external = likely_source_external;
  request.output_in_place = output_in_place;
  reserved_paths->push_back(request.output_path);
  XELOGI("iOS ZAR: planned request kind={} external={} in_place={} "
         "display='{}' source='{}' output='{}'",
         XEZarSourceKindForLog(request.source_path),
         XEZarBoolString(request.likely_source_external), XEZarBoolString(request.output_in_place),
         request.display_name, request.source_path.string(), request.output_path.string());
  requests->push_back(std::move(request));
  return true;
}

std::vector<XeniaIOSZarConversionRequest> XEZarSingleConversionRequest(
    const IOSDiscoveredGame& game, bool output_in_place = false) {
  XELOGI("iOS ZAR: planning single conversion title='{}' title_id={:08X} "
         "path='{}' kind={} external={} discs={} in_place={}",
         game.title, game.title_id, game.path.string(), XEZarSourceKindForLog(game.path),
         XEZarBoolString(game.is_external), game.discs.size(), XEZarBoolString(output_in_place));
  std::vector<XeniaIOSZarConversionRequest> requests;
  std::vector<std::filesystem::path> reserved_paths;
  const std::string display_name = game.title.empty() ? game.path.stem().string() : game.title;
  if (game.discs.empty()) {
    XEZarAddConversionRequest(game.path, display_name, display_name, game.is_external,
                              output_in_place, &reserved_paths, &requests);
    return requests;
  }

  for (const IOSDiscoveredGame::Disc& disc : game.discs) {
    std::string base_name = display_name;
    const bool multi_disc = game.discs.size() > 1 || disc.disc_count > 1;
    if (multi_disc && disc.disc_number) {
      base_name += " Disc " + std::to_string(disc.disc_number);
    } else if (multi_disc && !disc.label.empty()) {
      base_name += " " + disc.label;
    }
    XEZarAddConversionRequest(disc.path, base_name, base_name, disc.is_external, output_in_place,
                              &reserved_paths, &requests);
  }
  return requests;
}

std::vector<XeniaIOSZarConversionRequest> XEZarBulkConversionRequests(
    const std::vector<IOSDiscoveredGame>& games, bool external_only = false,
    bool output_in_place = false) {
  XELOGI("iOS ZAR: planning bulk conversion games={} external_only={} "
         "in_place={}",
         games.size(), XEZarBoolString(external_only), XEZarBoolString(output_in_place));
  std::vector<XeniaIOSZarConversionRequest> requests;
  std::vector<std::filesystem::path> reserved_paths;
  for (const IOSDiscoveredGame& game : games) {
    const std::string display_name = game.title.empty() ? game.path.stem().string() : game.title;
    if (game.discs.empty()) {
      if (external_only && !game.is_external) {
        continue;
      }
      XEZarAddConversionRequest(game.path, display_name, display_name, game.is_external,
                                output_in_place, &reserved_paths, &requests);
      continue;
    }

    for (const IOSDiscoveredGame::Disc& disc : game.discs) {
      if (external_only && !disc.is_external) {
        continue;
      }
      std::string base_name = display_name;
      const bool multi_disc = game.discs.size() > 1 || disc.disc_count > 1;
      if (multi_disc && disc.disc_number) {
        base_name += " Disc " + std::to_string(disc.disc_number);
      } else if (multi_disc && !disc.label.empty()) {
        base_name += " " + disc.label;
      }
      XEZarAddConversionRequest(disc.path, base_name, base_name, disc.is_external, output_in_place,
                                &reserved_paths, &requests);
    }
  }
  return requests;
}

std::vector<IOSDiscoveredGame> XEZarGamesMatchingAddedPath(
    const std::vector<IOSDiscoveredGame>& games, const std::filesystem::path& added_path) {
  std::vector<IOSDiscoveredGame> matching_games;
  for (const IOSDiscoveredGame& game : games) {
    if (game.discs.empty()) {
      if (XEZarPathMatchesAddedPath(game.path, added_path)) {
        matching_games.push_back(game);
      }
      continue;
    }

    IOSDiscoveredGame matching_game = game;
    matching_game.discs.clear();
    for (const IOSDiscoveredGame::Disc& disc : game.discs) {
      if (XEZarPathMatchesAddedPath(disc.path, added_path)) {
        matching_game.discs.push_back(disc);
      }
    }
    if (!matching_game.discs.empty()) {
      matching_game.path = matching_game.discs.front().path;
      matching_game.is_external = matching_game.discs.front().is_external;
      matching_games.push_back(std::move(matching_game));
    } else if (XEZarPathMatchesAddedPath(game.path, added_path)) {
      matching_games.push_back(game);
    }
  }
  return matching_games;
}

bool XEZarRequestSupportsAutoCleanup(const XeniaIOSZarConversionRequest& request) {
  return request.likely_source_external ||
         (xe::ui::IsISOPath(request.source_path) &&
          xe::ui::IsPathInIOSImportedGamesDirectory(request.source_path));
}

bool XEZarRequestSupportsExternalDelete(const XeniaIOSZarConversionRequest& request) {
  return request.likely_source_external;
}

NSString* XEZarRequestDisplayName(const XeniaIOSZarConversionRequest& request) {
  if (!request.display_name.empty()) {
    return ToNSString(request.display_name);
  }
  if (!request.output_name.empty()) {
    return ToNSString(request.output_name);
  }
  return ToNSString(request.source_path.filename().string());
}

NSString* XEZarRequestOutputName(const XeniaIOSZarConversionRequest& request) {
  if (!request.output_name.empty()) {
    return ToNSString(request.output_name);
  }
  return ToNSString(request.output_path.filename().string());
}

NSString* XEZarRequestSourceName(const XeniaIOSZarConversionRequest& request) {
  std::filesystem::path source_path = XEZarExternalDeletePath(request.source_path);
  if (source_path.empty()) {
    source_path = request.source_path;
  }
  return ToNSString(source_path.filename().string());
}

NSString* XEZarPreflightItemCountText(size_t count) {
  return count == 1 ? @"1 item" : [NSString stringWithFormat:@"%zu items", count];
}

NSArray<NSDictionary<NSString*, NSString*>*>* XEZarPreflightRowsForRequests(
    const std::vector<XeniaIOSZarConversionRequest>& requests, BOOL estimating) {
  NSMutableArray<NSDictionary<NSString*, NSString*>*>* rows = [NSMutableArray array];
  for (const XeniaIOSZarConversionRequest& request : requests) {
    NSMutableArray<NSString*>* detail_parts = [NSMutableArray array];
    NSString* output_name = XEZarRequestOutputName(request) ?: @"archive.zar";
    [detail_parts addObject:[NSString stringWithFormat:@"Output: %@", output_name]];
    if (request.estimate_available) {
      [detail_parts addObject:XEZarEstimatedSpaceDeltaSummary(request.source_bytes,
                                                              request.estimated_zar_bytes)];
    } else {
      [detail_parts
          addObject:estimating ? @"Waiting for estimate." : @"Estimated savings unavailable."];
    }
    NSString* source_name = XEZarRequestSourceName(request) ?: @"Source";
    [detail_parts addObject:[NSString stringWithFormat:@"Source: %@", source_name]];
    if (request.likely_source_external) {
      [detail_parts addObject:@"External library item."];
    }
    XEZarAddSummaryRow(
        rows, XEZarRequestDisplayName(request) ?: @"Game",
        [detail_parts componentsJoinedByString:@"\n"],
        request.estimate_available ? @"archivebox" : @"clock",
        request.estimate_available ? kXEZarSummaryRowStyleSuccess : kXEZarSummaryRowStyleMuted);
  }
  return rows;
}

}  // namespace

@interface XeniaIOSZarConversionPreflightAction : NSObject {
 @private
  NSString* title_;
  NSString* detail_;
  NSString* symbol_name_;
  NSString* style_;
  void (^handler_)(void);
}
@property(nonatomic, copy, readonly) NSString* title;
@property(nonatomic, copy, readonly) NSString* detail;
@property(nonatomic, copy, readonly) NSString* symbolName;
@property(nonatomic, copy, readonly) NSString* style;
@property(nonatomic, copy, readonly) void (^handler)(void);
+ (instancetype)actionWithTitle:(NSString*)title
                         detail:(NSString*)detail
                     symbolName:(NSString*)symbolName
                          style:(NSString*)style
                        handler:(void (^)(void))handler;
- (instancetype)initWithTitle:(NSString*)title
                       detail:(NSString*)detail
                   symbolName:(NSString*)symbolName
                        style:(NSString*)style
                      handler:(void (^)(void))handler;
@end

@implementation XeniaIOSZarConversionPreflightAction

@synthesize title = title_;
@synthesize detail = detail_;
@synthesize symbolName = symbol_name_;
@synthesize style = style_;
@synthesize handler = handler_;

+ (instancetype)actionWithTitle:(NSString*)title
                         detail:(NSString*)detail
                     symbolName:(NSString*)symbolName
                          style:(NSString*)style
                        handler:(void (^)(void))handler {
  return [[[self alloc] initWithTitle:title
                               detail:detail
                           symbolName:symbolName
                                style:style
                              handler:handler] autorelease];
}

- (instancetype)initWithTitle:(NSString*)title
                       detail:(NSString*)detail
                   symbolName:(NSString*)symbolName
                        style:(NSString*)style
                      handler:(void (^)(void))handler {
  if (!(self = [super init])) {
    return nil;
  }
  title_ = [title copy];
  detail_ = [detail copy];
  symbol_name_ = [symbolName copy];
  style_ = [style copy];
  handler_ = [handler copy];
  return self;
}

- (void)dealloc {
  [title_ release];
  [detail_ release];
  [symbol_name_ release];
  [style_ release];
  [handler_ release];
  [super dealloc];
}

@end

// The preflight sheet is a single screen that walks through every stage of a
// conversion in place: estimating savings, choosing an action, the live
// conversion progress, and the final results. Keeping all of it in one sheet
// (rather than swapping in a separate "Converting to ZAR" modal) is a
// deliberate UX requirement.
typedef NS_ENUM(NSInteger, XeniaIOSZarPreflightPhase) {
  XeniaIOSZarPreflightPhaseEstimating = 0,
  XeniaIOSZarPreflightPhaseReady,
  XeniaIOSZarPreflightPhaseConverting,
  XeniaIOSZarPreflightPhaseDone,
};

typedef NS_ENUM(NSInteger, XeniaIOSZarPreflightSection) {
  XeniaIOSZarPreflightSectionActions = 0,
  XeniaIOSZarPreflightSectionProgress,
  XeniaIOSZarPreflightSectionSummary,
  XeniaIOSZarPreflightSectionItems,
};

// One row of live conversion progress, rendered inline inside the preflight
// sheet so the whole flow stays in a single screen.
@interface XeniaIOSZarConversionInlineProgressCell : UITableViewCell
- (void)updateWithBatchText:(NSString*)batchText
                 outputName:(NSString*)outputName
                percentText:(NSString*)percentText
                   progress:(float)progress
                    etaText:(NSString*)etaText
                 detailText:(NSString*)detailText;
@end

@interface XeniaIOSZarConversionPreflightViewController
    : XESheetTableViewController <UIAdaptivePresentationControllerDelegate>
// Invoked when the user backs out before conversion starts (estimating/ready).
@property(nonatomic, copy) void (^cancelHandler)(void);
// Invoked when the user cancels an in-flight conversion.
@property(nonatomic, copy) void (^conversionCancelHandler)(void);
// Invoked when the user closes the sheet from the final results phase.
@property(nonatomic, copy) void (^dismissHandler)(void);
- (instancetype)initWithTitle:(NSString*)title requestCount:(NSUInteger)requestCount;
- (void)updateWithRequests:(const std::vector<XeniaIOSZarConversionRequest>&)requests
                estimating:(BOOL)estimating
                 completed:(NSUInteger)completed
                     total:(NSUInteger)total
                   actions:(NSArray<XeniaIOSZarConversionPreflightAction*>*)actions;
- (void)beginConversionWithItemNames:(NSArray<NSString*>*)itemNames
                          outputName:(NSString*)outputName;
- (void)updateConversionProgressWithFiles:(uint32_t)files
                                    bytes:(uint64_t)bytes
                               totalBytes:(uint64_t)totalBytes
                               batchIndex:(NSUInteger)batchIndex
                               batchCount:(NSUInteger)batchCount
                               outputName:(NSString*)outputName
                               finalizing:(BOOL)finalizing;
- (void)setConversionCancelEnabled:(BOOL)enabled;
- (void)showResultsWithTitle:(NSString*)title
                     summary:(NSString*)summary
                        rows:(NSArray<NSDictionary<NSString*, NSString*>*>*)rows
                     actions:(NSArray<XeniaIOSZarConversionPreflightAction*>*)actions;
- (void)markFinished;
@end

@implementation XeniaIOSZarConversionInlineProgressCell {
  UILabel* batch_label_;
  UILabel* output_label_;
  UILabel* percent_label_;
  UIProgressView* progress_view_;
  UILabel* eta_label_;
  UILabel* detail_label_;
  UILabel* warning_label_;
  UIImageView* warning_icon_;
}

- (instancetype)initWithStyle:(UITableViewCellStyle)style
              reuseIdentifier:(NSString*)reuseIdentifier {
  if (!(self = [super initWithStyle:style reuseIdentifier:reuseIdentifier])) {
    return nil;
  }
  self.selectionStyle = UITableViewCellSelectionStyleNone;

  batch_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  batch_label_.textColor = [XeniaTheme textSecondary];
  batch_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  batch_label_.adjustsFontForContentSizeCategory = YES;

  output_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  output_label_.textColor = [XeniaTheme textPrimary];
  output_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  output_label_.adjustsFontForContentSizeCategory = YES;
  output_label_.numberOfLines = 2;

  // The percentage is supporting detail next to the title, not a hero number:
  // match the title's text style and sit it on the same baseline (HIG favours
  // built-in text styles over oversized custom sizes).
  percent_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  percent_label_.textColor = [XeniaTheme accent];
  percent_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
  percent_label_.adjustsFontForContentSizeCategory = YES;
  percent_label_.textAlignment = NSTextAlignmentRight;
  [percent_label_ setContentHuggingPriority:UILayoutPriorityRequired
                                    forAxis:UILayoutConstraintAxisHorizontal];
  [percent_label_ setContentCompressionResistancePriority:UILayoutPriorityRequired
                                                  forAxis:UILayoutConstraintAxisHorizontal];

  UIStackView* title_row =
      [[UIStackView alloc] initWithArrangedSubviews:@[ output_label_, percent_label_ ]];
  title_row.axis = UILayoutConstraintAxisHorizontal;
  title_row.alignment = UIStackViewAlignmentLastBaseline;
  title_row.spacing = 8.0;

  progress_view_ = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
  progress_view_.progress = 0.0f;

  eta_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  eta_label_.textColor = [XeniaTheme textPrimary];
  eta_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  eta_label_.adjustsFontForContentSizeCategory = YES;
  eta_label_.numberOfLines = 0;

  detail_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  detail_label_.textColor = [XeniaTheme textSecondary];
  detail_label_.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  detail_label_.adjustsFontForContentSizeCategory = YES;
  detail_label_.numberOfLines = 0;

  // The "keep XeniOS open" guidance reads larger on iPad (Body) than on iPhone
  // (Footnote); both follow Dynamic Type so they scale with the user's setting.
  const BOOL pad = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
  UIFont* warning_font =
      [UIFont preferredFontForTextStyle:pad ? UIFontTextStyleBody : UIFontTextStyleFootnote];
  warning_icon_ = [[UIImageView alloc]
      initWithImage:[UIImage systemImageNamed:@"exclamationmark.triangle.fill"
                            withConfiguration:[UIImageSymbolConfiguration
                                                  configurationWithFont:warning_font]]];
  warning_icon_.tintColor = [XeniaTheme statusWarning];
  warning_icon_.contentMode = UIViewContentModeScaleAspectFit;
  [warning_icon_ setContentHuggingPriority:UILayoutPriorityRequired
                                   forAxis:UILayoutConstraintAxisHorizontal];

  warning_label_ = [[UILabel alloc] initWithFrame:CGRectZero];
  warning_label_.text =
      @"Keep XeniOS open until conversion finishes. Cancel removes partial output.";
  warning_label_.textColor = [XeniaTheme textSecondary];
  warning_label_.font = warning_font;
  warning_label_.adjustsFontForContentSizeCategory = YES;
  warning_label_.numberOfLines = 0;

  UIStackView* warning_row =
      [[UIStackView alloc] initWithArrangedSubviews:@[ warning_icon_, warning_label_ ]];
  warning_row.axis = UILayoutConstraintAxisHorizontal;
  warning_row.alignment = UIStackViewAlignmentFirstBaseline;
  warning_row.spacing = 8.0;

  UIStackView* content_stack = [[UIStackView alloc] initWithArrangedSubviews:@[
    batch_label_, title_row, progress_view_, eta_label_, detail_label_, warning_row
  ]];
  content_stack.axis = UILayoutConstraintAxisVertical;
  content_stack.alignment = UIStackViewAlignmentFill;
  content_stack.spacing = 8.0;
  [content_stack setCustomSpacing:14.0 afterView:detail_label_];
  content_stack.translatesAutoresizingMaskIntoConstraints = NO;
  [self.contentView addSubview:content_stack];

  UILayoutGuide* margins = self.contentView.layoutMarginsGuide;
  [NSLayoutConstraint activateConstraints:@[
    [content_stack.topAnchor constraintEqualToAnchor:margins.topAnchor],
    [content_stack.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
    [content_stack.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
    [content_stack.bottomAnchor constraintEqualToAnchor:margins.bottomAnchor],
  ]];

  [title_row release];
  [warning_row release];
  [content_stack release];
  return self;
}

- (void)dealloc {
  [batch_label_ release];
  [output_label_ release];
  [percent_label_ release];
  [progress_view_ release];
  [eta_label_ release];
  [detail_label_ release];
  [warning_label_ release];
  [warning_icon_ release];
  [super dealloc];
}

- (void)updateWithBatchText:(NSString*)batchText
                 outputName:(NSString*)outputName
                percentText:(NSString*)percentText
                   progress:(float)progress
                    etaText:(NSString*)etaText
                 detailText:(NSString*)detailText {
  batch_label_.text = batchText;
  output_label_.text = outputName;
  percent_label_.text = percentText;
  [progress_view_ setProgress:progress animated:NO];
  // Keep the ETA line always present (with a stable fallback string) so the
  // self-sizing cell height does not jump when the estimate appears/disappears.
  eta_label_.text = etaText;
  detail_label_.text = detailText;
}

@end

@implementation XeniaIOSZarConversionPreflightViewController {
  NSString* title_;
  XeniaIOSZarPreflightPhase phase_;
  NSUInteger request_count_;
  BOOL finished_;

  // Estimating / Ready: a prominent savings banner at the top, the shared item
  // list, and the convert actions (greyed out until the estimate lands).
  NSArray<NSDictionary<NSString*, NSString*>*>* item_rows_;
  NSArray<XeniaIOSZarConversionPreflightAction*>* actions_;
  NSString* summary_headline_;
  NSString* summary_detail_;
  BOOL summary_show_spinner_;

  // Converting / Done: per-item rows that fill in live as each game finishes, so
  // the list the user watches becomes the results. An entry stays NSNull until
  // the item completes; converting_item_names_ backs the not-yet-done rows.
  NSArray<NSString*>* converting_item_names_;
  NSMutableArray* item_rows_live_;
  NSString* converting_output_name_;
  uint32_t converting_files_;
  uint64_t converting_bytes_;
  uint64_t converting_total_bytes_;
  NSUInteger converting_index_;
  NSUInteger converting_count_;
  BOOL converting_finalizing_;
  BOOL converting_cancel_enabled_;
  // Throughput sampling for the "time remaining" estimate (per current item).
  NSString* converting_eta_text_;
  NSTimeInterval eta_sample_time_;
  uint64_t eta_sample_bytes_;
  NSUInteger eta_sample_index_;

  // Done.
  NSString* done_title_;
  NSString* done_summary_;

  UIBarButtonItem* cancel_button_;
}

- (instancetype)initWithTitle:(NSString*)title requestCount:(NSUInteger)requestCount {
  if (!(self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
    return nil;
  }
  title_ = [title copy];
  request_count_ = requestCount;
  phase_ = XeniaIOSZarPreflightPhaseEstimating;
  finished_ = NO;
  item_rows_ = [[NSArray alloc] init];
  actions_ = [[NSArray alloc] init];
  summary_headline_ = [@"Estimating savings..." copy];
  summary_detail_ = [[NSString stringWithFormat:@"Scanning %lu item%@...",
                                                static_cast<unsigned long>(requestCount),
                                                requestCount == 1 ? @"" : @"s"] retain];
  summary_show_spinner_ = YES;
  return self;
}

- (void)dealloc {
  [_cancelHandler release];
  [_conversionCancelHandler release];
  [_dismissHandler release];
  [title_ release];
  [item_rows_ release];
  [actions_ release];
  [summary_headline_ release];
  [summary_detail_ release];
  [converting_item_names_ release];
  [item_rows_live_ release];
  [converting_output_name_ release];
  [converting_eta_text_ release];
  [done_title_ release];
  [done_summary_ release];
  [cancel_button_ release];
  [super dealloc];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.title = title_;
  self.view.backgroundColor = [UIColor systemBackgroundColor];
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedRowHeight = 96.0;
  cancel_button_ = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel
                                                                 target:self
                                                                 action:@selector(cancelTapped:)];
  [self refreshChrome];
}

- (void)refreshChrome {
  if (!self.isViewLoaded) {
    return;
  }
  switch (phase_) {
    case XeniaIOSZarPreflightPhaseEstimating:
    case XeniaIOSZarPreflightPhaseReady:
      cancel_button_.enabled = YES;
      self.navigationItem.leftBarButtonItem = cancel_button_;
      self.navigationItem.rightBarButtonItem = nil;
      break;
    case XeniaIOSZarPreflightPhaseConverting:
      cancel_button_.enabled = converting_cancel_enabled_;
      self.navigationItem.leftBarButtonItem = cancel_button_;
      self.navigationItem.rightBarButtonItem = nil;
      break;
    case XeniaIOSZarPreflightPhaseDone:
      self.navigationItem.leftBarButtonItem = nil;
      self.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
          initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                               target:self
                               action:@selector(doneTapped:)] autorelease];
      break;
  }
}

- (NSArray<NSNumber*>*)currentSectionKinds {
  switch (phase_) {
    case XeniaIOSZarPreflightPhaseEstimating:
    case XeniaIOSZarPreflightPhaseReady:
    case XeniaIOSZarPreflightPhaseDone: {
      // Status banner first, then the convert/cleanup actions, then the items.
      NSMutableArray<NSNumber*>* kinds =
          [NSMutableArray arrayWithObject:@(XeniaIOSZarPreflightSectionSummary)];
      if (actions_.count > 0) {
        [kinds addObject:@(XeniaIOSZarPreflightSectionActions)];
      }
      [kinds addObject:@(XeniaIOSZarPreflightSectionItems)];
      return kinds;
    }
    case XeniaIOSZarPreflightPhaseConverting:
      return @[ @(XeniaIOSZarPreflightSectionProgress), @(XeniaIOSZarPreflightSectionItems) ];
  }
  return @[];
}

- (XeniaIOSZarPreflightSection)sectionKindAtIndex:(NSInteger)section {
  NSArray<NSNumber*>* kinds = [self currentSectionKinds];
  if (section < 0 || section >= static_cast<NSInteger>(kinds.count)) {
    return XeniaIOSZarPreflightSectionItems;
  }
  return static_cast<XeniaIOSZarPreflightSection>([[kinds objectAtIndex:section] integerValue]);
}

- (NSInteger)sectionIndexForKind:(XeniaIOSZarPreflightSection)kind {
  NSArray<NSNumber*>* kinds = [self currentSectionKinds];
  for (NSUInteger index = 0; index < kinds.count; ++index) {
    if ([[kinds objectAtIndex:index] integerValue] == kind) {
      return static_cast<NSInteger>(index);
    }
  }
  return -1;
}

- (void)cancelTapped:(__unused id)sender {
  if (phase_ == XeniaIOSZarPreflightPhaseConverting) {
    if (!converting_cancel_enabled_) {
      return;
    }
    [self setConversionCancelEnabled:NO];
    if (_conversionCancelHandler) {
      _conversionCancelHandler();
    }
    return;
  }
  [self markFinished];
  if (_cancelHandler) {
    _cancelHandler();
  } else {
    [self dismissViewControllerAnimated:YES completion:nil];
  }
}

- (void)doneTapped:(__unused id)sender {
  [self markFinished];
  if (_dismissHandler) {
    _dismissHandler();
  } else {
    [self dismissViewControllerAnimated:YES completion:nil];
  }
}

- (BOOL)presentationControllerShouldDismiss:
    (__unused UIPresentationController*)presentationController {
  return phase_ != XeniaIOSZarPreflightPhaseConverting;
}

- (void)presentationControllerDidDismiss:
    (__unused UIPresentationController*)presentationController {
  if (phase_ == XeniaIOSZarPreflightPhaseDone) {
    if (_dismissHandler) {
      _dismissHandler();
    }
    return;
  }
  if (finished_) {
    return;
  }
  [self markFinished];
  if (_cancelHandler) {
    _cancelHandler();
  }
}

- (void)markFinished {
  finished_ = YES;
}

- (void)updateWithRequests:(const std::vector<XeniaIOSZarConversionRequest>&)requests
                estimating:(BOOL)estimating
                 completed:(NSUInteger)completed
                     total:(NSUInteger)total
                   actions:(NSArray<XeniaIOSZarConversionPreflightAction*>*)actions {
  request_count_ = requests.size();

  [item_rows_ release];
  item_rows_ = [XEZarPreflightRowsForRequests(requests, estimating) copy];

  // The convert actions ride along even while estimating so they can be shown
  // greyed out, making it obvious why they are not tappable yet.
  [actions_ release];
  actions_ = actions ? [actions copy] : [[NSArray alloc] init];

  uint64_t source_bytes = 0;
  uint64_t estimated_zar_bytes = 0;
  BOOL any_estimate = NO;
  for (const XeniaIOSZarConversionRequest& request : requests) {
    source_bytes += request.source_bytes;
    if (request.estimate_available) {
      estimated_zar_bytes += request.estimated_zar_bytes;
      any_estimate = YES;
    }
  }

  if (estimating) {
    phase_ = XeniaIOSZarPreflightPhaseEstimating;
    summary_show_spinner_ = YES;
    [summary_headline_ release];
    summary_headline_ = [@"Estimating savings..." copy];
    const NSUInteger safe_total = total > 0 ? total : request_count_;
    double percent = 0.0;
    if (safe_total > 0) {
      percent = (static_cast<double>(completed) / static_cast<double>(safe_total)) * 100.0;
    }
    [summary_detail_ release];
    summary_detail_ = [[NSString stringWithFormat:@"Scanned %lu of %lu item%@ (%.0f%%)",
                                                  static_cast<unsigned long>(completed),
                                                  static_cast<unsigned long>(safe_total),
                                                  safe_total == 1 ? @"" : @"s", percent] retain];
  } else {
    phase_ = XeniaIOSZarPreflightPhaseReady;
    summary_show_spinner_ = NO;
    NSString* headline = nil;
    NSString* detail = nil;
    if (any_estimate && source_bytes > 0 && estimated_zar_bytes > 0) {
      if (source_bytes >= estimated_zar_bytes) {
        headline =
            [NSString stringWithFormat:@"Save about %@",
                                       XEZarFormatByteCount(source_bytes - estimated_zar_bytes)];
      } else {
        headline =
            [NSString stringWithFormat:@"Uses about %@ more",
                                       XEZarFormatByteCount(estimated_zar_bytes - source_bytes)];
      }
      detail =
          [NSString stringWithFormat:@"%@ - %@ -> %@", XEZarPreflightItemCountText(request_count_),
                                     XEZarFormatByteCount(source_bytes),
                                     XEZarFormatByteCount(estimated_zar_bytes)];
    } else {
      headline = @"Ready to convert";
      detail = [NSString stringWithFormat:@"%@ - estimated savings unavailable",
                                          XEZarPreflightItemCountText(request_count_)];
    }
    [summary_headline_ release];
    summary_headline_ = [headline copy];
    [summary_detail_ release];
    summary_detail_ = [detail copy];
  }

  if (self.isViewLoaded) {
    [self.tableView reloadData];
    [self refreshChrome];
  }
}

- (void)beginConversionWithItemNames:(NSArray<NSString*>*)itemNames
                          outputName:(NSString*)outputName {
  phase_ = XeniaIOSZarPreflightPhaseConverting;
  finished_ = NO;
  [converting_item_names_ release];
  converting_item_names_ = [itemNames copy];
  [item_rows_live_ release];
  item_rows_live_ = [[NSMutableArray alloc] initWithCapacity:itemNames.count];
  for (NSUInteger index = 0; index < itemNames.count; ++index) {
    [item_rows_live_ addObject:[NSNull null]];
  }
  [converting_output_name_ release];
  converting_output_name_ = [outputName copy];
  converting_files_ = 0;
  converting_bytes_ = 0;
  converting_total_bytes_ = 0;
  converting_count_ = itemNames.count;
  converting_index_ = itemNames.count > 0 ? 1 : 0;
  converting_finalizing_ = NO;
  converting_cancel_enabled_ = YES;
  [converting_eta_text_ release];
  converting_eta_text_ = nil;
  eta_sample_time_ = 0.0;
  eta_sample_bytes_ = 0;
  eta_sample_index_ = 0;
  if (self.isViewLoaded) {
    [self.tableView reloadData];
    [self refreshChrome];
  }
}

- (void)setConvertingItemRow:(NSDictionary<NSString*, NSString*>*)row atIndex:(NSUInteger)index {
  if (!row || index >= item_rows_live_.count) {
    return;
  }
  [item_rows_live_ replaceObjectAtIndex:index withObject:row];
  if (self.isViewLoaded && phase_ == XeniaIOSZarPreflightPhaseConverting) {
    const NSInteger items_section = [self sectionIndexForKind:XeniaIOSZarPreflightSectionItems];
    if (items_section >= 0) {
      [self.tableView
          reloadRowsAtIndexPaths:@[ [NSIndexPath indexPathForRow:static_cast<NSInteger>(index)
                                                       inSection:items_section] ]
                withRowAnimation:UITableViewRowAnimationNone];
    }
  }
}

- (void)updateConversionProgressWithFiles:(uint32_t)files
                                    bytes:(uint64_t)bytes
                               totalBytes:(uint64_t)totalBytes
                               batchIndex:(NSUInteger)batchIndex
                               batchCount:(NSUInteger)batchCount
                               outputName:(NSString*)outputName
                               finalizing:(BOOL)finalizing {
  if (phase_ != XeniaIOSZarPreflightPhaseConverting) {
    return;
  }
  const NSUInteger previous_index = converting_index_;
  const BOOL previous_finalizing = converting_finalizing_;
  converting_files_ = files;
  converting_bytes_ = bytes;
  converting_total_bytes_ = totalBytes;
  converting_index_ = batchIndex;
  converting_count_ = batchCount;
  [converting_output_name_ release];
  converting_output_name_ = [outputName copy];
  converting_finalizing_ = finalizing;

  // Estimate time remaining from this item's average throughput so far. Restart
  // sampling whenever the active item changes; keep the last estimate until a
  // fresh one is computed so the text does not flicker between updates.
  if (finalizing || totalBytes == 0 || bytes >= totalBytes) {
    [converting_eta_text_ release];
    converting_eta_text_ = nil;
  } else if (batchIndex != eta_sample_index_ || eta_sample_time_ == 0.0 ||
             bytes < eta_sample_bytes_) {
    eta_sample_index_ = batchIndex;
    eta_sample_time_ = [NSProcessInfo processInfo].systemUptime;
    eta_sample_bytes_ = bytes;
    [converting_eta_text_ release];
    converting_eta_text_ = nil;
  } else {
    const NSTimeInterval elapsed = [NSProcessInfo processInfo].systemUptime - eta_sample_time_;
    if (elapsed >= 1.5 && bytes > eta_sample_bytes_) {
      const double rate = static_cast<double>(bytes - eta_sample_bytes_) / elapsed;
      if (rate > 0.0) {
        NSString* text = XEZarFormatTimeRemaining(static_cast<double>(totalBytes - bytes) / rate);
        [converting_eta_text_ release];
        converting_eta_text_ = [text copy];
      }
    }
  }

  if (!self.isViewLoaded) {
    return;
  }
  [self refreshVisibleProgressCell];
  if (batchIndex != previous_index || finalizing != previous_finalizing) {
    const NSInteger items_section = [self sectionIndexForKind:XeniaIOSZarPreflightSectionItems];
    if (items_section >= 0) {
      [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:items_section]
                    withRowAnimation:UITableViewRowAnimationNone];
    }
  }
}

- (void)setConversionCancelEnabled:(BOOL)enabled {
  converting_cancel_enabled_ = enabled;
  cancel_button_.enabled = enabled;
  if (self.isViewLoaded) {
    [self refreshVisibleProgressCell];
  }
}

- (void)refreshVisibleProgressCell {
  const NSInteger progress_section = [self sectionIndexForKind:XeniaIOSZarPreflightSectionProgress];
  if (progress_section < 0) {
    return;
  }
  NSIndexPath* index_path = [NSIndexPath indexPathForRow:0 inSection:progress_section];
  UITableViewCell* cell = [self.tableView cellForRowAtIndexPath:index_path];
  if ([cell isKindOfClass:[XeniaIOSZarConversionInlineProgressCell class]]) {
    [self applyProgressToCell:(XeniaIOSZarConversionInlineProgressCell*)cell];
  }
}

- (void)applyProgressToCell:(XeniaIOSZarConversionInlineProgressCell*)cell {
  float progress = 0.0f;
  if (converting_total_bytes_ > 0) {
    progress = std::min(1.0f, static_cast<float>(static_cast<double>(converting_bytes_) /
                                                 static_cast<double>(converting_total_bytes_)));
  }
  if (converting_finalizing_ && converting_total_bytes_ > 0) {
    progress = std::min(progress, 0.99f);
  }

  NSString* batch_text = nil;
  if (!converting_cancel_enabled_) {
    batch_text = @"Cancelling after the current file";
  } else if (converting_count_ > 1) {
    batch_text = [NSString stringWithFormat:@"Item %lu of %lu",
                                            static_cast<unsigned long>(converting_index_),
                                            static_cast<unsigned long>(converting_count_)];
  } else {
    batch_text = @"Current item";
  }

  NSString* percent_text =
      converting_finalizing_
          ? @"Finalizing"
          : (converting_total_bytes_ > 0 ? [NSString stringWithFormat:@"%.0f%%", progress * 100.0f]
                                         : @"--");

  NSString* eta_text = nil;
  if (!converting_cancel_enabled_) {
    eta_text = @"Finishing the current file...";
  } else if (converting_finalizing_) {
    eta_text = @"Finishing up...";
  } else if (converting_eta_text_.length > 0) {
    eta_text = converting_eta_text_;
  } else {
    eta_text = @"Estimating time remaining...";
  }

  NSString* detail_text = nil;
  if (converting_finalizing_) {
    detail_text = @"Writing archive index and validating output.";
  } else if (converting_total_bytes_ > 0) {
    NSString* size_text =
        [NSString stringWithFormat:@"%@ of %@", XEZarFormatByteCount(converting_bytes_),
                                   XEZarFormatByteCount(converting_total_bytes_)];
    detail_text =
        converting_files_ > 0
            ? [NSString stringWithFormat:@"%@, %u file%@ packed", size_text, converting_files_,
                                         converting_files_ == 1 ? @"" : @"s"]
            : size_text;
  } else {
    detail_text = @"Preparing file list.";
  }

  [cell updateWithBatchText:batch_text
                 outputName:converting_output_name_.length > 0 ? converting_output_name_
                                                               : @"Preparing archive"
                percentText:percent_text
                   progress:progress
                    etaText:eta_text
                 detailText:detail_text];
}

- (void)showResultsWithTitle:(NSString*)title
                     summary:(NSString*)summary
                        rows:(NSArray<NSDictionary<NSString*, NSString*>*>*)rows
                     actions:(NSArray<XeniaIOSZarConversionPreflightAction*>*)actions {
  phase_ = XeniaIOSZarPreflightPhaseDone;
  finished_ = YES;
  [done_title_ release];
  done_title_ = [title copy];
  [done_summary_ release];
  done_summary_ = [summary copy];
  // Reuse the same item list the user watched fill in during conversion; this is
  // the authoritative final set (covers skipped/cancelled items too).
  [item_rows_live_ release];
  item_rows_live_ = rows ? [rows mutableCopy] : [[NSMutableArray alloc] init];
  [actions_ release];
  actions_ = actions ? [actions copy] : [[NSArray alloc] init];
  if (self.isViewLoaded) {
    [self.tableView reloadData];
    [self refreshChrome];
  }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)__unused tableView {
  return static_cast<NSInteger>([self currentSectionKinds].count);
}

- (NSInteger)tableView:(UITableView*)__unused tableView numberOfRowsInSection:(NSInteger)section {
  switch ([self sectionKindAtIndex:section]) {
    case XeniaIOSZarPreflightSectionActions:
      return static_cast<NSInteger>(actions_.count);
    case XeniaIOSZarPreflightSectionProgress:
      return 1;
    case XeniaIOSZarPreflightSectionSummary:
      return 1;
    case XeniaIOSZarPreflightSectionItems:
      if (phase_ == XeniaIOSZarPreflightPhaseConverting ||
          phase_ == XeniaIOSZarPreflightPhaseDone) {
        return static_cast<NSInteger>(item_rows_live_.count);
      }
      return static_cast<NSInteger>(item_rows_.count);
  }
  return 0;
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForHeaderInSection:(NSInteger)section {
  switch ([self sectionKindAtIndex:section]) {
    case XeniaIOSZarPreflightSectionActions:
      return phase_ == XeniaIOSZarPreflightPhaseDone ? @"Original Files" : @"Convert Options";
    case XeniaIOSZarPreflightSectionProgress:
      return @"Converting";
    case XeniaIOSZarPreflightSectionSummary:
      return done_title_;
    case XeniaIOSZarPreflightSectionItems:
      if (phase_ == XeniaIOSZarPreflightPhaseConverting ||
          phase_ == XeniaIOSZarPreflightPhaseDone) {
        return @"Items";
      }
      return request_count_ == 1 ? @"Item" : @"Items";
  }
  return nil;
}

- (NSString*)tableView:(UITableView*)__unused tableView titleForFooterInSection:(NSInteger)section {
  switch ([self sectionKindAtIndex:section]) {
    case XeniaIOSZarPreflightSectionActions:
      return nil;
    case XeniaIOSZarPreflightSectionProgress:
      // The "keep open" guidance lives inside the progress cell now (legible on
      // iPad), so the section footer stays empty.
      return nil;
    case XeniaIOSZarPreflightSectionItems:
      if (phase_ == XeniaIOSZarPreflightPhaseReady) {
        return @"Keep XeniOS open during conversion. Canceling removes partial output.";
      }
      return nil;
    case XeniaIOSZarPreflightSectionSummary:
      return nil;
  }
  return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
  switch ([self sectionKindAtIndex:indexPath.section]) {
    case XeniaIOSZarPreflightSectionActions:
      return [self actionCellForTableView:tableView action:[actions_ objectAtIndex:indexPath.row]];
    case XeniaIOSZarPreflightSectionProgress:
      return [self progressCellForTableView:tableView];
    case XeniaIOSZarPreflightSectionSummary:
      return [self summaryCellForTableView:tableView];
    case XeniaIOSZarPreflightSectionItems:
      if (phase_ == XeniaIOSZarPreflightPhaseConverting) {
        return [self convertingItemCellForTableView:tableView row:indexPath.row];
      }
      return [self rowCellForTableView:tableView
                                   row:[(phase_ == XeniaIOSZarPreflightPhaseDone ? item_rows_live_
                                                                                 : item_rows_)
                                           objectAtIndex:indexPath.row]];
  }
  return [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                 reuseIdentifier:nil] autorelease];
}

- (UITableViewCell*)rowCellForTableView:(UITableView*)tableView
                                    row:(NSDictionary<NSString*, NSString*>*)row {
  static NSString* const kItemCellIdentifier = @"XeniaIOSZarConversionPreflightItemCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kItemCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kItemCellIdentifier] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
  }
  NSString* style = [row objectForKey:kXEZarSummaryRowStyleKey];
  UIColor* tint = XEZarSummaryRowTint(style);
  UIImageSymbolConfiguration* symbol_config =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* symbol = [UIImage systemImageNamed:[row objectForKey:kXEZarSummaryRowSymbolKey]
                            withConfiguration:symbol_config];
  cell.imageView.image = [symbol imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = tint;
  cell.textLabel.text = [row objectForKey:kXEZarSummaryRowTitleKey];
  cell.textLabel.textColor = tint;
  cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  cell.detailTextLabel.text = [row objectForKey:kXEZarSummaryRowDetailKey];
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  cell.accessoryType = UITableViewCellAccessoryNone;
  cell.accessoryView = nil;
  return cell;
}

- (UITableViewCell*)actionCellForTableView:(UITableView*)tableView
                                    action:(XeniaIOSZarConversionPreflightAction*)action {
  static NSString* const kActionCellIdentifier = @"XeniaIOSZarConversionPreflightActionCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kActionCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kActionCellIdentifier] autorelease];
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
  }
  const BOOL estimating = (phase_ == XeniaIOSZarPreflightPhaseEstimating);
  UIColor* tint = estimating ? [XeniaTheme textMuted] : XEZarSummaryRowTint(action.style);
  UIImageSymbolConfiguration* symbol_config =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* symbol = [UIImage systemImageNamed:action.symbolName withConfiguration:symbol_config];
  cell.imageView.image = [symbol imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = tint;
  cell.textLabel.text = action.title;
  cell.textLabel.textColor = tint;
  cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  cell.detailTextLabel.text = estimating ? @"Available once the estimate finishes." : action.detail;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  cell.accessoryType =
      estimating ? UITableViewCellAccessoryNone : UITableViewCellAccessoryDisclosureIndicator;
  cell.accessoryView = nil;
  cell.selectionStyle =
      estimating ? UITableViewCellSelectionStyleNone : UITableViewCellSelectionStyleDefault;
  cell.userInteractionEnabled = !estimating;
  return cell;
}

- (UITableViewCell*)summaryCellForTableView:(UITableView*)tableView {
  if (phase_ == XeniaIOSZarPreflightPhaseDone) {
    static NSString* const kDoneSummaryCellIdentifier =
        @"XeniaIOSZarConversionPreflightDoneSummaryCell";
    UITableViewCell* cell =
        [tableView dequeueReusableCellWithIdentifier:kDoneSummaryCellIdentifier];
    if (!cell) {
      cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                     reuseIdentifier:kDoneSummaryCellIdentifier] autorelease];
      cell.selectionStyle = UITableViewCellSelectionStyleNone;
      cell.textLabel.numberOfLines = 0;
    }
    cell.textLabel.text = done_summary_;
    cell.textLabel.textColor = [XeniaTheme textPrimary];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    return cell;
  }

  // Estimating / Ready: a prominent headline (the savings) over a one-line
  // detail, with a spinner while the estimate is still running.
  static NSString* const kBannerCellIdentifier = @"XeniaIOSZarConversionPreflightBannerCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:kBannerCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kBannerCellIdentifier] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
  }
  const BOOL ready = (phase_ == XeniaIOSZarPreflightPhaseReady);
  cell.textLabel.text = summary_headline_;
  cell.textLabel.textColor = ready ? [XeniaTheme accent] : [XeniaTheme textPrimary];
  cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
  cell.textLabel.adjustsFontForContentSizeCategory = YES;
  cell.detailTextLabel.text = summary_detail_;
  cell.detailTextLabel.textColor = [XeniaTheme textSecondary];
  cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
  UIImageSymbolConfiguration* symbol_config =
      [UIImageSymbolConfiguration configurationWithPointSize:24.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* symbol = [UIImage systemImageNamed:ready ? @"internaldrive.fill" : @"hourglass"
                            withConfiguration:symbol_config];
  cell.imageView.image = [symbol imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = ready ? [XeniaTheme accent] : [XeniaTheme textMuted];
  if (summary_show_spinner_) {
    UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
    [spinner startAnimating];
    cell.accessoryView = spinner;
  } else {
    cell.accessoryView = nil;
  }
  return cell;
}

- (UITableViewCell*)progressCellForTableView:(UITableView*)tableView {
  static NSString* const kProgressCellIdentifier = @"XeniaIOSZarConversionInlineProgressCell";
  XeniaIOSZarConversionInlineProgressCell* cell =
      (XeniaIOSZarConversionInlineProgressCell*)[tableView
          dequeueReusableCellWithIdentifier:kProgressCellIdentifier];
  if (!cell) {
    cell = [[[XeniaIOSZarConversionInlineProgressCell alloc]
          initWithStyle:UITableViewCellStyleDefault
        reuseIdentifier:kProgressCellIdentifier] autorelease];
  }
  // Populate synchronously so the cell has content (and the right self-sized
  // height) the moment it appears, rather than showing blank until a scroll or
  // the next progress tick forces a refresh.
  [self applyProgressToCell:cell];
  return cell;
}

- (UITableViewCell*)convertingItemCellForTableView:(UITableView*)tableView row:(NSInteger)row {
  // Once an item finishes, its result row (with the saved amount and details) is
  // pushed into item_rows_live_; render that the same way the results list does.
  id entry = (row >= 0 && row < static_cast<NSInteger>(item_rows_live_.count))
                 ? [item_rows_live_ objectAtIndex:row]
                 : [NSNull null];
  if ([entry isKindOfClass:[NSDictionary class]]) {
    return [self rowCellForTableView:tableView row:(NSDictionary<NSString*, NSString*>*)entry];
  }

  static NSString* const kConvertingItemCellIdentifier =
      @"XeniaIOSZarConversionPreflightConvertingItemCell";
  UITableViewCell* cell =
      [tableView dequeueReusableCellWithIdentifier:kConvertingItemCellIdentifier];
  if (!cell) {
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                   reuseIdentifier:kConvertingItemCellIdentifier] autorelease];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
  }
  const NSInteger current = static_cast<NSInteger>(converting_index_) - 1;
  NSString* status = nil;
  NSString* symbol_name = nil;
  NSString* style = nil;
  BOOL show_spinner = NO;
  if (row == current) {
    if (converting_finalizing_) {
      status = @"Finalizing...";
      symbol_name = @"archivebox";
    } else {
      status = @"Converting...";
      symbol_name = @"arrow.triangle.2.circlepath";
      show_spinner = YES;
    }
    style = kXEZarSummaryRowStyleSuccess;
  } else if (row < current) {
    status = @"Finishing up...";
    symbol_name = @"checkmark.circle";
    style = kXEZarSummaryRowStyleSuccess;
  } else {
    status = @"Waiting";
    symbol_name = @"clock";
    style = kXEZarSummaryRowStyleMuted;
  }
  UIColor* tint = XEZarSummaryRowTint(style);
  UIImageSymbolConfiguration* symbol_config =
      [UIImageSymbolConfiguration configurationWithPointSize:20.0
                                                      weight:UIImageSymbolWeightSemibold];
  UIImage* symbol = [UIImage systemImageNamed:symbol_name withConfiguration:symbol_config];
  cell.imageView.image = [symbol imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
  cell.imageView.tintColor = tint;
  cell.textLabel.text = (row >= 0 && row < static_cast<NSInteger>(converting_item_names_.count))
                            ? [converting_item_names_ objectAtIndex:row]
                            : @"Game";
  cell.textLabel.textColor = [XeniaTheme textPrimary];
  cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
  cell.detailTextLabel.text = status;
  cell.detailTextLabel.textColor = tint;
  cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
  if (show_spinner) {
    UIActivityIndicatorView* spinner = [[[UIActivityIndicatorView alloc]
        initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium] autorelease];
    [spinner startAnimating];
    cell.accessoryView = spinner;
  } else {
    cell.accessoryView = nil;
  }
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  if (phase_ == XeniaIOSZarPreflightPhaseEstimating) {
    return;  // Actions are shown greyed out until the estimate finishes.
  }
  if ([self sectionKindAtIndex:indexPath.section] != XeniaIOSZarPreflightSectionActions) {
    return;
  }
  if (indexPath.row >= static_cast<NSInteger>(actions_.count)) {
    return;
  }
  XeniaIOSZarConversionPreflightAction* action = [actions_ objectAtIndex:indexPath.row];
  void (^handler)(void) = action.handler;
  if (handler) {
    handler();
  }
}

@end

namespace {

bool XEZarDeleteSourceFiles(const std::filesystem::path& source_path, std::string* message_out) {
  if (message_out) {
    message_out->clear();
  }

  const std::filesystem::path delete_path = XEZarExternalDeletePath(source_path);
  if (delete_path.empty()) {
    if (message_out) {
      *message_out = "Original source path is empty.";
    }
    return false;
  }

  std::error_code ec;
  uintmax_t removed_count = 0;
  if (std::filesystem::is_directory(delete_path, ec)) {
    removed_count = std::filesystem::remove_all(delete_path, ec);
  } else {
    const bool removed = std::filesystem::remove(delete_path, ec);
    removed_count = removed ? 1 : 0;
    if (!ec) {
      std::filesystem::path sidecar_path = delete_path;
      sidecar_path += ".data";
      std::error_code sidecar_ec;
      if (std::filesystem::exists(sidecar_path, sidecar_ec)) {
        std::filesystem::remove_all(sidecar_path, sidecar_ec);
        if (sidecar_ec) {
          if (message_out) {
            *message_out = "Deleted original source, but failed to delete sidecar data: " +
                           sidecar_ec.message();
          }
          return false;
        }
      }
    }
  }

  if (ec || removed_count == 0) {
    if (message_out) {
      *message_out = ec ? "Failed to delete original source files: " + ec.message()
                        : "Original source files were not found.";
    }
    return false;
  }

  if (message_out) {
    *message_out = "Deleted original source files.";
  }
  return true;
}

bool XEZarCleanupOriginalAfterConversion(const std::filesystem::path& source_path,
                                         bool source_was_external, bool delete_source_files,
                                         bool* attempted_out, std::string* message_out) {
  if (attempted_out) {
    *attempted_out = false;
  }
  if (message_out) {
    message_out->clear();
  }

  if (delete_source_files) {
    if (attempted_out) {
      *attempted_out = true;
    }
    return XEZarDeleteSourceFiles(source_path, message_out);
  }

  if (source_was_external) {
    if (attempted_out) {
      *attempted_out = true;
    }
    NSString* hidden_name = nil;
    NSError* error = nil;
    if (!xe::ui::HideIOSExternalLibraryGameAtPath(source_path, &hidden_name, &error)) {
      if (message_out) {
        *message_out = error.localizedDescription
                           ? std::string([error.localizedDescription UTF8String])
                           : std::string("Failed to unlink original game.");
      }
      return false;
    }
    if (message_out) {
      *message_out = hidden_name.length > 0
                         ? "Unlinked " + std::string([hidden_name UTF8String]) + "."
                         : "Unlinked original game.";
    }
    return true;
  }

  if (!xe::ui::IsISOPath(source_path) || !xe::ui::IsPathInIOSImportedGamesDirectory(source_path)) {
    if (message_out) {
      *message_out = "Original source was kept.";
    }
    return true;
  }

  if (attempted_out) {
    *attempted_out = true;
  }
  std::error_code remove_ec;
  const bool removed = std::filesystem::remove(source_path, remove_ec);
  if (remove_ec || !removed) {
    if (message_out) {
      *message_out = remove_ec ? "Failed to delete original ISO: " + remove_ec.message()
                               : "Original ISO was not found.";
    }
    return false;
  }
  if (message_out) {
    *message_out = "Deleted original ISO copy.";
  }
  return true;
}

}  // namespace

@interface XeniaIOSZarConversionCoordinator ()
- (UIViewController*)topPresentedControllerForModalPresentation;
- (void)showStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style;
- (void)showPersistentStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style;
- (void)updateStatusToast:(NSString*)message;
- (void)dismissStatusToast;
- (void)refreshImportedGamesAsync;
- (void)refreshImportedGamesAsyncWithCompletion:(void (^)(void))completion;
- (void)presentZarConversionOptionsForIndex:(size_t)game_index;
- (void)presentBulkZarConversionOptions;
- (XeniaIOSZarConversionPreflightViewController*)
    presentZarConversionPreflightWithTitle:(NSString*)title
                              requestCount:(NSUInteger)requestCount
                             cancelHandler:(void (^)(void))cancelHandler;
- (void)dismissZarConversionPreflightWithCompletion:(void (^)(void))completion;
- (NSArray<XeniaIOSZarConversionPreflightAction*>*)
    singleZarPreflightActionsForGame:(const IOSDiscoveredGame&)game
                      requestsHolder:
                          (std::shared_ptr<std::vector<XeniaIOSZarConversionRequest>>)requestsHolder
                  completionOnChoice:(void (^)(BOOL))completion;
- (NSArray<XeniaIOSZarConversionPreflightAction*>*)
    bulkZarPreflightActionsForRequestsHolder:
        (std::shared_ptr<std::vector<XeniaIOSZarConversionRequest>>)requestsHolder
                          completionOnChoice:(void (^)(BOOL))completion;
- (void)convertGameToZarForIndex:(size_t)game_index;
- (void)startZarConversionRequests:(std::vector<XeniaIOSZarConversionRequest>)requests
                       cleanupMode:(XeniaIOSZarConversionCleanupMode)cleanupMode;
- (void)checkPendingZarConversionOnLaunch;
- (void)updateZarConversionProgressWithFiles:(uint32_t)files
                                       bytes:(uint64_t)bytes
                                  totalBytes:(uint64_t)totalBytes
                                  batchIndex:(NSUInteger)batchIndex
                                  batchCount:(NSUInteger)batchCount
                                  outputName:(NSString*)outputName
                                  finalizing:(BOOL)finalizing;
- (void)updateZarConversionItemRow:(NSDictionary<NSString*, NSString*>*)row
                           atIndex:(NSUInteger)index;
- (void)presentZarConversionSummaryForResults:(std::vector<XeniaIOSZarConversionItemResult>)results
                                  cleanupMode:(XeniaIOSZarConversionCleanupMode)cleanupMode
                                       isBulk:(BOOL)isBulk;
- (NSArray<XeniaIOSZarConversionPreflightAction*>*)
    zarCleanupActionsForSourcePath:(const std::filesystem::path&)sourcePath
                 sourceWasExternal:(BOOL)sourceWasExternal;
- (void)deleteOriginalGameAfterZarConversionAtPath:(const std::filesystem::path&)sourcePath;
- (void)deleteExternalGameFilesAfterZarConversionAtPath:(const std::filesystem::path&)sourcePath;
- (void)unlinkExternalGameForConvertedSourcePath:(const std::filesystem::path&)sourcePath;
@end

@implementation XeniaIOSZarConversionCoordinator {
  id<XeniaIOSZarConversionCoordinatorHost> host_;
  std::vector<IOSDiscoveredGame> discovered_games_;
  BOOL checked_pending_zar_conversion_;
  UINavigationController* zar_conversion_preflight_navigation_;
  XeniaIOSZarConversionPreflightViewController* zar_conversion_preflight_controller_;
  std::shared_ptr<std::atomic_bool> zar_conversion_cancel_requested_;
}

- (instancetype)initWithHost:(id<XeniaIOSZarConversionCoordinatorHost>)host {
  if (!(self = [super init])) {
    return nil;
  }
  host_ = host;
  checked_pending_zar_conversion_ = NO;
  zar_conversion_preflight_navigation_ = nil;
  zar_conversion_preflight_controller_ = nil;
  zar_conversion_cancel_requested_.reset();
  return self;
}

- (void)dealloc {
  [zar_conversion_preflight_navigation_ release];
  [zar_conversion_preflight_controller_ release];
  [super dealloc];
}

- (UIViewController*)topPresentedControllerForModalPresentation {
  return [host_ zarConversionCoordinatorPresenter];
}

- (void)showStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style {
  [host_ zarConversionCoordinatorShowStatusToast:message style:style];
}

- (void)showPersistentStatusToast:(NSString*)message style:(XeniaIOSStatusToastStyle)style {
  [host_ zarConversionCoordinatorShowPersistentStatusToast:message style:style];
}

- (void)updateStatusToast:(NSString*)message {
  [host_ zarConversionCoordinatorUpdateStatusToast:message];
}

- (void)dismissStatusToast {
  [host_ zarConversionCoordinatorDismissStatusToast];
}

- (void)refreshImportedGamesAsync {
  [host_ zarConversionCoordinatorRefreshImportedGames];
}

- (void)refreshImportedGamesAsyncWithCompletion:(void (^)(void))completion {
  [host_ zarConversionCoordinatorRefreshImportedGamesWithCompletion:completion];
}

- (XeniaIOSZarConversionPreflightViewController*)
    presentZarConversionPreflightWithTitle:(NSString*)title
                              requestCount:(NSUInteger)requestCount
                             cancelHandler:(void (^)(void))cancelHandler {
  [zar_conversion_preflight_navigation_ release];
  zar_conversion_preflight_navigation_ = nil;
  [zar_conversion_preflight_controller_ release];
  zar_conversion_preflight_controller_ = nil;

  UIViewController* presenter = [self topPresentedControllerForModalPresentation];
  if (!presenter) {
    return nil;
  }

  XeniaIOSZarConversionPreflightViewController* preflight_controller =
      [[XeniaIOSZarConversionPreflightViewController alloc] initWithTitle:title
                                                             requestCount:requestCount];
  preflight_controller.cancelHandler = cancelHandler;
  UINavigationController* navigation =
      [[UINavigationController alloc] initWithRootViewController:preflight_controller];
  XEConfigureDestinationPresentation(navigation, presenter.view, CGSizeMake(720.0, 720.0), NO);
  navigation.presentationController.delegate = preflight_controller;

  zar_conversion_preflight_controller_ = [preflight_controller retain];
  zar_conversion_preflight_navigation_ = [navigation retain];
  [presenter presentViewController:navigation animated:YES completion:nil];
  [navigation release];
  [preflight_controller release];
  return zar_conversion_preflight_controller_;
}

- (void)dismissZarConversionPreflightWithCompletion:(void (^)(void))completion {
  UINavigationController* navigation = [zar_conversion_preflight_navigation_ retain];
  XeniaIOSZarConversionPreflightViewController* preflight_controller =
      [zar_conversion_preflight_controller_ retain];
  [preflight_controller markFinished];
  [zar_conversion_preflight_navigation_ release];
  zar_conversion_preflight_navigation_ = nil;
  [zar_conversion_preflight_controller_ release];
  zar_conversion_preflight_controller_ = nil;

  if (navigation.presentingViewController) {
    [navigation dismissViewControllerAnimated:YES
                                   completion:^{
                                     [preflight_controller release];
                                     [navigation release];
                                     if (completion) {
                                       completion();
                                     }
                                   }];
    return;
  }

  [preflight_controller release];
  [navigation release];
  if (completion) {
    completion();
  }
}

- (NSArray<XeniaIOSZarConversionPreflightAction*>*)
    singleZarPreflightActionsForGame:(const IOSDiscoveredGame&)game
                      requestsHolder:
                          (std::shared_ptr<std::vector<XeniaIOSZarConversionRequest>>)requestsHolder
                  completionOnChoice:(void (^)(BOOL))completion {
  uint64_t source_bytes = 0;
  uint64_t estimated_zar_bytes = 0;
  bool supports_auto_cleanup = false;
  bool supports_external_delete = false;
  for (const XeniaIOSZarConversionRequest& request : *requestsHolder) {
    source_bytes += request.source_bytes;
    if (request.estimate_available) {
      estimated_zar_bytes += request.estimated_zar_bytes;
    }
    supports_auto_cleanup = supports_auto_cleanup || XEZarRequestSupportsAutoCleanup(request);
    supports_external_delete =
        supports_external_delete || XEZarRequestSupportsExternalDelete(request);
  }

  XELOGI("iOS ZAR: presenting single preflight actions requests={} source_bytes={} "
         "estimated_zar_bytes={} supports_cleanup={} supports_external_delete={}",
         requestsHolder->size(), source_bytes, estimated_zar_bytes,
         XEZarBoolString(supports_auto_cleanup), XEZarBoolString(supports_external_delete));

  NSMutableArray<XeniaIOSZarConversionPreflightAction*>* actions = [NSMutableArray array];
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;
  void (^completion_copy)(BOOL) = [completion copy];
  [actions addObject:[XeniaIOSZarConversionPreflightAction
                         actionWithTitle:@"Convert to ZAR"
                                  detail:@"Create the archive in XeniOS, then choose what to do "
                                         @"with the original."
                              symbolName:@"archivebox"
                                   style:kXEZarSummaryRowStyleSuccess
                                 handler:^{
                                   if (completion_copy) {
                                     completion_copy(YES);
                                   }
                                   [unsafe_self
                                       startZarConversionRequests:*requestsHolder
                                                      cleanupMode:XeniaIOSZarConversionCleanupMode::
                                                                      kAskAfterSuccess];
                                 }]];

  if (supports_auto_cleanup) {
    NSString* cleanup_title = supports_external_delete ? @"Convert and Unlink Originals"
                                                       : @"Convert and Delete Original Copies";
    [actions
        addObject:[XeniaIOSZarConversionPreflightAction
                      actionWithTitle:cleanup_title
                               detail:@"Create the archive and clean up the old library entry "
                                      @"after a successful conversion."
                           symbolName:supports_external_delete ? @"link.badge.minus" : @"trash"
                                style:kXEZarSummaryRowStyleWarning
                              handler:^{
                                if (completion_copy) {
                                  completion_copy(YES);
                                }
                                [unsafe_self
                                    startZarConversionRequests:*requestsHolder
                                                   cleanupMode:XeniaIOSZarConversionCleanupMode::
                                                                   kCleanupAfterSuccess];
                              }]];
  }

  if (supports_external_delete) {
    IOSDiscoveredGame game_copy = game;
    [actions
        addObject:[XeniaIOSZarConversionPreflightAction
                      actionWithTitle:@"Convert In Place and Delete Original"
                               detail:@"Write the archive beside the external source, then "
                                      @"delete the original files."
                           symbolName:@"trash.circle"
                                style:kXEZarSummaryRowStyleError
                              handler:^{
                                if (completion_copy) {
                                  completion_copy(YES);
                                }
                                std::vector<XeniaIOSZarConversionRequest> in_place_requests =
                                    XEZarSingleConversionRequest(game_copy, true);
                                if (in_place_requests.empty()) {
                                  XEPresentOKAlert(
                                      [unsafe_self topPresentedControllerForModalPresentation],
                                      @"External Folder Unavailable",
                                      @"XeniOS could not access the linked external folder "
                                      @"for in-place conversion.");
                                  return;
                                }
                                [unsafe_self
                                    startZarConversionRequests:std::move(in_place_requests)
                                                   cleanupMode:XeniaIOSZarConversionCleanupMode::
                                                                   kDeleteOriginalsAfterSuccess];
                              }]];
  }

  [completion_copy release];
  return actions;
}

- (NSArray<XeniaIOSZarConversionPreflightAction*>*)
    bulkZarPreflightActionsForRequestsHolder:
        (std::shared_ptr<std::vector<XeniaIOSZarConversionRequest>>)requestsHolder
                          completionOnChoice:(void (^)(BOOL))completion {
  NSUInteger external_count = 0;
  uint64_t source_bytes = 0;
  uint64_t estimated_zar_bytes = 0;
  for (const XeniaIOSZarConversionRequest& request : *requestsHolder) {
    source_bytes += request.source_bytes;
    if (request.estimate_available) {
      estimated_zar_bytes += request.estimated_zar_bytes;
    }
    if (XEZarRequestSupportsExternalDelete(request)) {
      ++external_count;
    }
  }

  XELOGI("iOS ZAR: presenting bulk preflight actions requests={} external_requests={} "
         "source_bytes={} estimated_zar_bytes={}",
         requestsHolder->size(), static_cast<unsigned long>(external_count), source_bytes,
         estimated_zar_bytes);

  NSMutableArray<XeniaIOSZarConversionPreflightAction*>* actions = [NSMutableArray array];
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;
  void (^completion_copy)(BOOL) = [completion copy];
  [actions addObject:[XeniaIOSZarConversionPreflightAction
                         actionWithTitle:@"Convert in Place"
                                  detail:@"Write .zar archives beside the source games and keep "
                                         @"the originals."
                              symbolName:@"archivebox"
                                   style:kXEZarSummaryRowStyleSuccess
                                 handler:^{
                                   std::vector<XeniaIOSZarConversionRequest> in_place_requests =
                                       XEZarBulkConversionRequests(unsafe_self->discovered_games_,
                                                                   false, true);
                                   if (completion_copy) {
                                     completion_copy(YES);
                                   }
                                   if (in_place_requests.empty()) {
                                     XEPresentOKAlert(
                                         [unsafe_self topPresentedControllerForModalPresentation],
                                         @"No Games to Convert",
                                         @"No non-.zar games were available for in-place "
                                         @"conversion.");
                                     return;
                                   }
                                   [unsafe_self
                                       startZarConversionRequests:std::move(in_place_requests)
                                                      cleanupMode:XeniaIOSZarConversionCleanupMode::
                                                                      kKeepOriginals];
                                 }]];

  [actions addObject:[XeniaIOSZarConversionPreflightAction
                         actionWithTitle:@"Convert and Import"
                                  detail:@"Create .zar archives in XeniOS, then decide how to "
                                         @"handle the originals."
                              symbolName:@"square.and.arrow.down"
                                   style:kXEZarSummaryRowStyleSuccess
                                 handler:^{
                                   if (completion_copy) {
                                     completion_copy(YES);
                                   }
                                   [unsafe_self
                                       startZarConversionRequests:*requestsHolder
                                                      cleanupMode:XeniaIOSZarConversionCleanupMode::
                                                                      kAskAfterSuccess];
                                 }]];

  [actions addObject:[XeniaIOSZarConversionPreflightAction
                         actionWithTitle:@"Convert and Delete Originals"
                                  detail:@"Write archives beside the source games and delete the "
                                         @"original files after success."
                              symbolName:@"trash.circle"
                                   style:kXEZarSummaryRowStyleError
                                 handler:^{
                                   std::vector<XeniaIOSZarConversionRequest> in_place_requests =
                                       XEZarBulkConversionRequests(unsafe_self->discovered_games_,
                                                                   false, true);
                                   if (completion_copy) {
                                     completion_copy(YES);
                                   }
                                   if (in_place_requests.empty()) {
                                     XEPresentOKAlert(
                                         [unsafe_self topPresentedControllerForModalPresentation],
                                         @"No Games to Convert",
                                         @"No non-.zar games were available for in-place "
                                         @"conversion.");
                                     return;
                                   }
                                   [unsafe_self
                                       startZarConversionRequests:std::move(in_place_requests)
                                                      cleanupMode:XeniaIOSZarConversionCleanupMode::
                                                                      kDeleteOriginalsAfterSuccess];
                                 }]];

  [completion_copy release];
  return actions;
}

- (void)checkPendingConversionOnLaunch {
  if (checked_pending_zar_conversion_) {
    return;
  }
  checked_pending_zar_conversion_ = YES;
  [self checkPendingZarConversionOnLaunch];
}

- (void)presentConversionOptionsForGames:(const std::vector<IOSDiscoveredGame>&)games
                                   index:(size_t)gameIndex {
  XELOGI("iOS ZAR: present single conversion options games={} index={}", games.size(), gameIndex);
  discovered_games_ = games;
  [self presentZarConversionOptionsForIndex:gameIndex];
}

- (void)presentBulkConversionOptionsForGames:(const std::vector<IOSDiscoveredGame>&)games {
  XELOGI("iOS ZAR: present bulk conversion options games={}", games.size());
  discovered_games_ = games;
  [self presentBulkZarConversionOptions];
}

- (void)presentPostImportConversionPromptForGames:(const std::vector<IOSDiscoveredGame>&)games
                                        addedPath:(const std::filesystem::path&)addedPath
                                  externalLibrary:(BOOL)externalLibrary
                                       completion:(void (^)(BOOL conversionChosen))completion {
  discovered_games_ = XEZarGamesMatchingAddedPath(games, addedPath);
  std::vector<XeniaIOSZarConversionRequest> requests =
      XEZarBulkConversionRequests(discovered_games_);
  void (^completion_copy)(BOOL) = [completion copy];
  if (requests.empty()) {
    XELOGI("iOS ZAR: no post-import conversion requests added_path='{}' external={}",
           addedPath.string(), XEZarBoolString(externalLibrary));
    if (completion_copy) {
      completion_copy(NO);
      [completion_copy release];
    }
    return;
  }

  XELOGI("iOS ZAR: post-import conversion prompt added_path='{}' external={} "
         "games={} requests={}",
         addedPath.string(), XEZarBoolString(externalLibrary), discovered_games_.size(),
         requests.size());
  auto requests_holder =
      std::make_shared<std::vector<XeniaIOSZarConversionRequest>>(std::move(requests));
  NSString* title = externalLibrary
                        ? @"Convert External Library to ZAR"
                        : (requests_holder->size() == 1 ? @"Convert Imported Game to ZAR"
                                                        : @"Convert Added Games to ZAR");
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;
  XeniaIOSZarConversionPreflightViewController* preflight_controller = [self
      presentZarConversionPreflightWithTitle:title
                                requestCount:requests_holder->size()
                               cancelHandler:^{
                                 if (completion_copy) {
                                   completion_copy(NO);
                                 }
                                 [unsafe_self dismissZarConversionPreflightWithCompletion:nil];
                               }];
  NSArray<XeniaIOSZarConversionPreflightAction*>* preflight_actions =
      [self bulkZarPreflightActionsForRequestsHolder:requests_holder
                                  completionOnChoice:completion_copy];
  [preflight_controller updateWithRequests:*requests_holder
                                estimating:YES
                                 completed:0
                                     total:requests_holder->size()
                                   actions:preflight_actions];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    for (size_t request_index = 0; request_index < requests_holder->size(); ++request_index) {
      XEZarRefreshConversionRequestEstimate(&(*requests_holder)[request_index]);
      const size_t completed = request_index + 1;
      const size_t total = requests_holder->size();
      // The next loop iteration keeps writing into requests_holder while the
      // main queue reads, so hand the UI an immutable snapshot instead.
      auto requests_snapshot =
          std::make_shared<const std::vector<XeniaIOSZarConversionRequest>>(*requests_holder);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (unsafe_self->zar_conversion_preflight_controller_ != preflight_controller) {
          return;
        }
        [preflight_controller updateWithRequests:*requests_snapshot
                                      estimating:YES
                                       completed:completed
                                           total:total
                                         actions:preflight_actions];
      });
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (unsafe_self->zar_conversion_preflight_controller_ != preflight_controller) {
        [completion_copy release];
        return;
      }
      [preflight_controller updateWithRequests:*requests_holder
                                    estimating:NO
                                     completed:requests_holder->size()
                                         total:requests_holder->size()
                                       actions:preflight_actions];
      [completion_copy release];
    });
  });
}

- (void)convertGameToZarForGames:(const std::vector<IOSDiscoveredGame>&)games
                           index:(size_t)gameIndex {
  XELOGI("iOS ZAR: direct convert request games={} index={}", games.size(), gameIndex);
  discovered_games_ = games;
  [self convertGameToZarForIndex:gameIndex];
}

- (void)presentZarConversionOptionsForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }

  const IOSDiscoveredGame game = discovered_games_[game_index];
  std::vector<XeniaIOSZarConversionRequest> requests = XEZarSingleConversionRequest(game);
  if (requests.empty()) {
    XELOGI("iOS ZAR: no single conversion requests title='{}' path='{}'", game.title,
           game.path.string());
    XEPresentOKAlert([self topPresentedControllerForModalPresentation], @"Already ZAR",
                     @"This game is already a .zar archive and does not need conversion.");
    return;
  }

  auto requests_holder =
      std::make_shared<std::vector<XeniaIOSZarConversionRequest>>(std::move(requests));
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;
  XeniaIOSZarConversionPreflightViewController* preflight_controller = [self
      presentZarConversionPreflightWithTitle:@"Convert to ZAR"
                                requestCount:requests_holder->size()
                               cancelHandler:^{
                                 [unsafe_self dismissZarConversionPreflightWithCompletion:nil];
                               }];
  // Build the actions up front so they can be shown greyed out while estimating.
  IOSDiscoveredGame game_copy = game;
  NSArray<XeniaIOSZarConversionPreflightAction*>* preflight_actions =
      [self singleZarPreflightActionsForGame:game_copy
                              requestsHolder:requests_holder
                          completionOnChoice:nil];
  [preflight_controller updateWithRequests:*requests_holder
                                estimating:YES
                                 completed:0
                                     total:requests_holder->size()
                                   actions:preflight_actions];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    for (size_t request_index = 0; request_index < requests_holder->size(); ++request_index) {
      XEZarRefreshConversionRequestEstimate(&(*requests_holder)[request_index]);
      const size_t completed = request_index + 1;
      const size_t total = requests_holder->size();
      // The next loop iteration keeps writing into requests_holder while the
      // main queue reads, so hand the UI an immutable snapshot instead.
      auto requests_snapshot =
          std::make_shared<const std::vector<XeniaIOSZarConversionRequest>>(*requests_holder);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (unsafe_self->zar_conversion_preflight_controller_ != preflight_controller) {
          return;
        }
        [preflight_controller updateWithRequests:*requests_snapshot
                                      estimating:YES
                                       completed:completed
                                           total:total
                                         actions:preflight_actions];
      });
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (unsafe_self->zar_conversion_preflight_controller_ != preflight_controller) {
        return;
      }
      [preflight_controller updateWithRequests:*requests_holder
                                    estimating:NO
                                     completed:requests_holder->size()
                                         total:requests_holder->size()
                                       actions:preflight_actions];
    });
  });
}

- (void)presentBulkZarConversionOptions {
  std::vector<XeniaIOSZarConversionRequest> requests =
      XEZarBulkConversionRequests(discovered_games_);
  if (requests.empty()) {
    XELOGI("iOS ZAR: no bulk conversion requests");
    XEPresentOKAlert([self topPresentedControllerForModalPresentation], @"No Games to Convert",
                     @"No non-.zar games were found in the current library.");
    return;
  }

  XELOGI("iOS ZAR: bulk conversion planned {} request(s)", requests.size());
  auto requests_holder =
      std::make_shared<std::vector<XeniaIOSZarConversionRequest>>(std::move(requests));
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;
  XeniaIOSZarConversionPreflightViewController* preflight_controller = [self
      presentZarConversionPreflightWithTitle:@"Convert Library to ZAR"
                                requestCount:requests_holder->size()
                               cancelHandler:^{
                                 [unsafe_self dismissZarConversionPreflightWithCompletion:nil];
                               }];
  NSArray<XeniaIOSZarConversionPreflightAction*>* preflight_actions =
      [self bulkZarPreflightActionsForRequestsHolder:requests_holder completionOnChoice:nil];
  [preflight_controller updateWithRequests:*requests_holder
                                estimating:YES
                                 completed:0
                                     total:requests_holder->size()
                                   actions:preflight_actions];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    for (size_t request_index = 0; request_index < requests_holder->size(); ++request_index) {
      XEZarRefreshConversionRequestEstimate(&(*requests_holder)[request_index]);
      const size_t completed = request_index + 1;
      const size_t total = requests_holder->size();
      // The next loop iteration keeps writing into requests_holder while the
      // main queue reads, so hand the UI an immutable snapshot instead.
      auto requests_snapshot =
          std::make_shared<const std::vector<XeniaIOSZarConversionRequest>>(*requests_holder);
      dispatch_async(dispatch_get_main_queue(), ^{
        if (unsafe_self->zar_conversion_preflight_controller_ != preflight_controller) {
          return;
        }
        [preflight_controller updateWithRequests:*requests_snapshot
                                      estimating:YES
                                       completed:completed
                                           total:total
                                         actions:preflight_actions];
      });
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (unsafe_self->zar_conversion_preflight_controller_ != preflight_controller) {
        return;
      }
      [preflight_controller updateWithRequests:*requests_holder
                                    estimating:NO
                                     completed:requests_holder->size()
                                         total:requests_holder->size()
                                       actions:preflight_actions];
    });
  });
}

- (void)convertGameToZarForIndex:(size_t)game_index {
  if (game_index >= discovered_games_.size()) {
    return;
  }
  std::vector<XeniaIOSZarConversionRequest> requests =
      XEZarSingleConversionRequest(discovered_games_[game_index]);
  [self startZarConversionRequests:std::move(requests)
                       cleanupMode:XeniaIOSZarConversionCleanupMode::kAskAfterSuccess];
}

- (void)startZarConversionRequests:(std::vector<XeniaIOSZarConversionRequest>)requests
                       cleanupMode:(XeniaIOSZarConversionCleanupMode)cleanupMode {
  if (requests.empty()) {
    XELOGI("iOS ZAR: start requested with no conversion requests");
    XEPresentOKAlert([self topPresentedControllerForModalPresentation], @"No Games to Convert",
                     @"No non-.zar games were selected for conversion.");
    return;
  }

  XELOGI("iOS ZAR: starting batch requests={} cleanup_mode={}", requests.size(),
         XEZarCleanupModeName(cleanupMode));
  for (size_t index = 0; index < requests.size(); ++index) {
    const XeniaIOSZarConversionRequest& request = requests[index];
    XELOGI("iOS ZAR: batch request {}/{} kind={} external={} in_place={} "
           "source='{}' output='{}'",
           index + 1, requests.size(), XEZarSourceKindForLog(request.source_path),
           XEZarBoolString(request.likely_source_external),
           XEZarBoolString(request.output_in_place), request.source_path.string(),
           request.output_path.string());
  }

  auto cancel_requested = std::make_shared<std::atomic_bool>(false);
  zar_conversion_cancel_requested_ = cancel_requested;
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;

  // Drive the live conversion inside the existing preflight sheet so the whole
  // flow stays in one screen. Fall back to a fresh sheet for any direct caller
  // that did not route through the preflight options first.
  NSMutableArray<NSString*>* item_names = [NSMutableArray array];
  for (const XeniaIOSZarConversionRequest& request : requests) {
    [item_names addObject:(XEZarRequestDisplayName(request) ?: @"Game")];
  }
  NSString* batch_output_name = ToNSString(requests.front().output_name);
  if (!zar_conversion_preflight_controller_) {
    [self presentZarConversionPreflightWithTitle:@"Convert to ZAR"
                                    requestCount:requests.size()
                                   cancelHandler:nil];
  }
  XeniaIOSZarConversionPreflightViewController* preflight_controller =
      zar_conversion_preflight_controller_;
  preflight_controller.conversionCancelHandler = ^{
    if (unsafe_self->zar_conversion_cancel_requested_) {
      unsafe_self->zar_conversion_cancel_requested_->store(true);
    }
  };
  [preflight_controller beginConversionWithItemNames:item_names outputName:batch_output_name];

  auto requests_holder =
      std::make_shared<std::vector<XeniaIOSZarConversionRequest>>(std::move(requests));
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    std::vector<XeniaIOSZarConversionItemResult> results;
    results.reserve(requests_holder->size());

    const BOOL delete_original_files =
        cleanupMode == XeniaIOSZarConversionCleanupMode::kDeleteOriginalsAfterSuccess;

    xe::vfs::ZarConversionOptions conversion_options;
    conversion_options.compression_thread_count = XEZarRecommendedCompressionThreads();
    conversion_options.compression_thread_initializer = []() { XEZarInitCompressionThread(); };
    XELOGI("iOS ZAR: using {} parallel compression worker(s)",
           conversion_options.compression_thread_count);

    // Push each finished item's result row into the live list so the row the
    // user is watching flips from "Converting..." to "Saved X" as it completes.
    auto publish_item_row = [unsafe_self, cleanupMode](
                                const XeniaIOSZarConversionItemResult& finished_item,
                                size_t finished_index) {
      NSDictionary<NSString*, NSString*>* row = XEZarResultRowForItem(finished_item, cleanupMode);
      dispatch_async(dispatch_get_main_queue(), ^{
        [unsafe_self updateZarConversionItemRow:row atIndex:finished_index];
      });
    };

    for (size_t index = 0; index < requests_holder->size(); ++index) {
      if (cancel_requested->load()) {
        XELOGI("iOS ZAR: cancellation requested before item {}/{}", index + 1,
               requests_holder->size());
        XEZarAppendSkippedAfterCancelResults(*requests_holder, index, &results);
        break;
      }

      XeniaIOSZarConversionRequest request = (*requests_holder)[index];
      XeniaIOSZarConversionItemResult item;
      item.request = request;

      NSError* external_access_error = nil;
      BOOL matched_external_location = NO;
      XeniaIOSExternalLibraryAccess* external_access =
          [xe::ui::StartIOSExternalLibraryAccessForPath(
              request.source_path, &matched_external_location, &external_access_error) retain];
      item.source_was_external = matched_external_location || request.likely_source_external;
      XELOGI("iOS ZAR: item {}/{} starting kind={} source='{}' output='{}' "
             "matched_external={} source_was_external={} access_granted={}",
             index + 1, requests_holder->size(), XEZarSourceKindForLog(request.source_path),
             request.source_path.string(), request.output_path.string(),
             XEZarBoolString(matched_external_location), XEZarBoolString(item.source_was_external),
             XEZarBoolString(external_access != nil));
      if (matched_external_location && !external_access) {
        item.conversion.success = false;
        item.conversion.error_message =
            external_access_error.localizedDescription
                ? std::string([external_access_error.localizedDescription UTF8String])
                : std::string("External folder is unavailable.");
        XELOGI("iOS ZAR: item {}/{} failed before conversion error='{}'", index + 1,
               requests_holder->size(), item.conversion.error_message);
        publish_item_row(item, index);
        results.push_back(std::move(item));
        [external_access release];
        continue;
      }

      XELOGI("iOS ZAR: item {}/{} invoking converter", index + 1, requests_holder->size());
      StorePendingZarConversionRecord(request.source_path, request.output_path,
                                      ToNSString(request.output_name), item.source_was_external);
      item.conversion = xe::vfs::ConvertPathToZar(
          request.source_path, request.output_path,
          [unsafe_self, index, count = requests_holder->size(),
           output_name = request.output_name](const xe::vfs::ZarConversionProgress& progress) {
            const uint32_t files_written = progress.files_written;
            const uint64_t bytes_written = progress.bytes_written;
            const uint64_t total_bytes = progress.total_bytes;
            const BOOL finalizing = progress.finalizing ? YES : NO;
            NSString* output_name_string = [ToNSString(output_name) retain];
            dispatch_async(dispatch_get_main_queue(), ^{
              [unsafe_self updateZarConversionProgressWithFiles:files_written
                                                          bytes:bytes_written
                                                     totalBytes:total_bytes
                                                     batchIndex:index + 1
                                                     batchCount:count
                                                     outputName:output_name_string
                                                     finalizing:finalizing];
              [output_name_string release];
            });
          },
          [cancel_requested]() { return cancel_requested->load(); }, conversion_options);

      if (item.conversion.success) {
        std::string validation_error;
        if (!xe::vfs::ValidateZarArchive(request.output_path, &validation_error)) {
          item.conversion.success = false;
          item.conversion.error_message = validation_error;
          std::error_code remove_ec;
          std::filesystem::remove(request.output_path, remove_ec);
          XELOGI("iOS ZAR: item {}/{} validation failed output='{}' error='{}'", index + 1,
                 requests_holder->size(), request.output_path.string(), validation_error);
        }
      }

      if (item.conversion.success) {
        item.output_bytes = XEZarOutputStorageBytes(request.output_path);
        XELOGI("iOS ZAR: item {}/{} converted files={} bytes={} total_bytes={} "
               "output_bytes={}",
               index + 1, requests_holder->size(), item.conversion.files_written,
               item.conversion.bytes_written, item.conversion.total_bytes, item.output_bytes);
        if (cleanupMode == XeniaIOSZarConversionCleanupMode::kCleanupAfterSuccess ||
            cleanupMode == XeniaIOSZarConversionCleanupMode::kDeleteOriginalsAfterSuccess) {
          item.cleanup_succeeded = XEZarCleanupOriginalAfterConversion(
              request.source_path, item.source_was_external, delete_original_files,
              &item.cleanup_attempted, &item.cleanup_message);
          XELOGI("iOS ZAR: item {}/{} cleanup attempted={} success={} "
                 "message='{}'",
                 index + 1, requests_holder->size(), XEZarBoolString(item.cleanup_attempted),
                 XEZarBoolString(item.cleanup_succeeded), item.cleanup_message);
        }
      } else {
        XELOGI("iOS ZAR: item {}/{} conversion failed cancelled={} files={} "
               "bytes={} total_bytes={} error='{}'",
               index + 1, requests_holder->size(), XEZarBoolString(item.conversion.cancelled),
               item.conversion.files_written, item.conversion.bytes_written,
               item.conversion.total_bytes, item.conversion.error_message);
      }

      if (item.conversion.cancelled) {
        XELOGI("iOS ZAR: item {}/{} cancelled; stopping batch", index + 1, requests_holder->size());
        ClearPendingZarConversionRecord();
        publish_item_row(item, index);
        results.push_back(std::move(item));
        XEZarAppendSkippedAfterCancelResults(*requests_holder, index + 1, &results);
        [external_access release];
        break;
      }

      ClearPendingZarConversionRecord();
      publish_item_row(item, index);
      results.push_back(std::move(item));
      [external_access release];
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      [unsafe_self refreshImportedGamesAsyncWithCompletion:^{
        [unsafe_self presentZarConversionSummaryForResults:std::move(results)
                                               cleanupMode:cleanupMode
                                                    isBulk:requests_holder->size() > 1 ? YES : NO];
      }];
    });
  });
}

- (void)checkPendingZarConversionOnLaunch {
  NSDictionary* record = LoadPendingZarConversionRecord();
  if (!record) {
    return;
  }

  XELOGI("iOS ZAR: found pending conversion record on launch");
  NSString* output_path_string = [record objectForKey:kXeniaPendingZarOutputPathKey];
  NSString* output_name = [record objectForKey:kXeniaPendingZarOutputNameKey];
  if (![output_path_string isKindOfClass:[NSString class]] || output_path_string.length == 0) {
    XELOGI("iOS ZAR: clearing invalid pending conversion record");
    ClearPendingZarConversionRecord();
    return;
  }

  const std::filesystem::path output_path{std::string([output_path_string UTF8String])};
  std::string validation_error;
  if (xe::vfs::ValidateZarArchive(output_path, &validation_error)) {
    XELOGI("iOS ZAR: pending conversion output is valid output='{}'", output_path.string());
    ClearPendingZarConversionRecord();
    [self refreshImportedGamesAsync];
    XEPresentOKAlert(
        [self topPresentedControllerForModalPresentation], @"ZAR Conversion Finished",
        [NSString stringWithFormat:@"%@ is present and valid. The original source was kept because "
                                   @"XeniOS was closed before cleanup could be confirmed.",
                                   output_name ?: output_path_string.lastPathComponent]);
    return;
  }

  std::error_code remove_ec;
  std::filesystem::remove(output_path, remove_ec);
  XELOGI("iOS ZAR: removed incomplete pending output='{}' remove_error='{}' "
         "validation_error='{}'",
         output_path.string(), remove_ec ? remove_ec.message() : "none", validation_error);
  ClearPendingZarConversionRecord();
  [self refreshImportedGamesAsync];
  XEPresentOKAlert([self topPresentedControllerForModalPresentation],
                   @"Incomplete ZAR Conversion Removed",
                   @"XeniOS removed the partial .zar output from the interrupted conversion.");
}

- (void)updateZarConversionProgressWithFiles:(uint32_t)files
                                       bytes:(uint64_t)bytes
                                  totalBytes:(uint64_t)totalBytes
                                  batchIndex:(NSUInteger)batchIndex
                                  batchCount:(NSUInteger)batchCount
                                  outputName:(NSString*)outputName
                                  finalizing:(BOOL)finalizing {
  [zar_conversion_preflight_controller_ updateConversionProgressWithFiles:files
                                                                    bytes:bytes
                                                               totalBytes:totalBytes
                                                               batchIndex:batchIndex
                                                               batchCount:batchCount
                                                               outputName:outputName
                                                               finalizing:finalizing];
}

- (void)updateZarConversionItemRow:(NSDictionary<NSString*, NSString*>*)row
                           atIndex:(NSUInteger)index {
  [zar_conversion_preflight_controller_ setConvertingItemRow:row atIndex:index];
}

- (NSArray<XeniaIOSZarConversionPreflightAction*>*)
    zarCleanupActionsForSourcePath:(const std::filesystem::path&)sourcePath
                 sourceWasExternal:(BOOL)sourceWasExternal {
  NSMutableArray<XeniaIOSZarConversionPreflightAction*>* actions = [NSMutableArray array];
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;
  const std::filesystem::path source_path = sourcePath;
  if (sourceWasExternal) {
    [actions addObject:[XeniaIOSZarConversionPreflightAction
                           actionWithTitle:@"Unlink Original"
                                    detail:@"Remove the linked library entry. The source files "
                                           @"stay on the external drive."
                                symbolName:@"link.badge.minus"
                                     style:kXEZarSummaryRowStyleWarning
                                   handler:^{
                                     [unsafe_self dismissZarConversionPreflightWithCompletion:^{
                                       [unsafe_self
                                           unlinkExternalGameForConvertedSourcePath:source_path];
                                     }];
                                   }]];
    [actions
        addObject:[XeniaIOSZarConversionPreflightAction
                      actionWithTitle:@"Delete External Files"
                               detail:@"Permanently delete the original files from the "
                                      @"external drive."
                           symbolName:@"trash"
                                style:kXEZarSummaryRowStyleError
                              handler:^{
                                [unsafe_self dismissZarConversionPreflightWithCompletion:^{
                                  [unsafe_self
                                      deleteExternalGameFilesAfterZarConversionAtPath:source_path];
                                }];
                              }]];
  } else if (xe::ui::IsISOPath(sourcePath) &&
             xe::ui::IsPathInIOSImportedGamesDirectory(sourcePath)) {
    [actions addObject:[XeniaIOSZarConversionPreflightAction
                           actionWithTitle:@"Delete Original Copy"
                                    detail:@"Delete the imported source file now that the .zar "
                                           @"archive exists."
                                symbolName:@"trash"
                                     style:kXEZarSummaryRowStyleError
                                   handler:^{
                                     [unsafe_self dismissZarConversionPreflightWithCompletion:^{
                                       [unsafe_self
                                           deleteOriginalGameAfterZarConversionAtPath:source_path];
                                     }];
                                   }]];
  }
  [actions addObject:[XeniaIOSZarConversionPreflightAction
                         actionWithTitle:@"Keep Original"
                                  detail:@"Leave the original game in place alongside the new "
                                         @".zar archive."
                              symbolName:@"checkmark.circle"
                                   style:kXEZarSummaryRowStyleSuccess
                                 handler:^{
                                   [unsafe_self dismissZarConversionPreflightWithCompletion:^{
                                     [unsafe_self refreshImportedGamesAsync];
                                   }];
                                 }]];
  return actions;
}

- (void)presentZarConversionSummaryForResults:(std::vector<XeniaIOSZarConversionItemResult>)results
                                  cleanupMode:(XeniaIOSZarConversionCleanupMode)cleanupMode
                                       isBulk:(BOOL)isBulk {
  NSUInteger success_count = 0;
  NSUInteger cancelled_count = 0;
  NSUInteger skipped_count = 0;
  NSUInteger failure_count = 0;
  NSUInteger cleanup_success_count = 0;
  NSUInteger cleanup_failure_count = 0;
  uint64_t source_bytes = 0;
  uint64_t output_bytes = 0;
  NSMutableArray<NSDictionary<NSString*, NSString*>*>* item_rows = [NSMutableArray array];

  for (const XeniaIOSZarConversionItemResult& item : results) {
    if (item.skipped_after_cancel) {
      ++skipped_count;
    } else if (item.conversion.cancelled) {
      ++cancelled_count;
    } else if (!item.conversion.success) {
      ++failure_count;
    } else {
      ++success_count;
      source_bytes += item.request.source_bytes;
      output_bytes += item.output_bytes;
      if (item.cleanup_attempted && !item.cleanup_succeeded) {
        ++cleanup_failure_count;
      }
      if (item.cleanup_attempted && item.cleanup_succeeded) {
        ++cleanup_success_count;
      }
    }
    [item_rows addObject:XEZarResultRowForItem(item, cleanupMode)];
  }

  // For a single successful conversion that asked what to do with the original,
  // offer the cleanup choices as action rows in the results phase instead of a
  // separate alert, so everything stays in the one sheet.
  NSArray<XeniaIOSZarConversionPreflightAction*>* cleanup_actions = nil;
  if (!isBulk && cleanupMode == XeniaIOSZarConversionCleanupMode::kAskAfterSuccess &&
      success_count == 1 && failure_count == 0 && cancelled_count == 0 && skipped_count == 0) {
    for (const XeniaIOSZarConversionItemResult& item : results) {
      if (item.conversion.success) {
        cleanup_actions = [self zarCleanupActionsForSourcePath:item.request.source_path
                                             sourceWasExternal:item.source_was_external ? YES : NO];
        break;
      }
    }
  }

  XELOGI(
      "iOS ZAR: summary success={} cancelled={} skipped={} "
      "cleanup_success={} cleanup_failures={} failures={} source_bytes={} "
      "output_bytes={} cleanup_mode={} bulk={}",
      static_cast<unsigned long>(success_count), static_cast<unsigned long>(cancelled_count),
      static_cast<unsigned long>(skipped_count), static_cast<unsigned long>(cleanup_success_count),
      static_cast<unsigned long>(cleanup_failure_count), static_cast<unsigned long>(failure_count),
      source_bytes, output_bytes, XEZarCleanupModeName(cleanupMode), isBulk ? "true" : "false");

  NSString* title = @"ZAR Conversion Complete";
  if (cancelled_count > 0 || skipped_count > 0) {
    title = @"ZAR Conversion Cancelled";
  } else if (success_count > 0 && failure_count > 0) {
    title = @"ZAR Conversion Finished with Errors";
  } else if (success_count == 0) {
    title = @"ZAR Conversion Failed";
  }

  NSMutableArray<NSString*>* lines = [NSMutableArray array];
  [lines addObject:[NSString stringWithFormat:@"Converted: %lu of %lu.",
                                              static_cast<unsigned long>(success_count),
                                              static_cast<unsigned long>(results.size())]];
  if (failure_count > 0) {
    [lines addObject:[NSString stringWithFormat:@"Failed: %lu.",
                                                static_cast<unsigned long>(failure_count)]];
  }
  if (cancelled_count > 0) {
    [lines addObject:[NSString stringWithFormat:@"Cancelled: %lu.",
                                                static_cast<unsigned long>(cancelled_count)]];
  }
  if (skipped_count > 0) {
    [lines addObject:[NSString stringWithFormat:@"Not started: %lu.",
                                                static_cast<unsigned long>(skipped_count)]];
  }
  if (success_count > 0 && source_bytes > 0 && output_bytes > 0) {
    [lines addObject:XEZarSpaceDeltaSummary(source_bytes, output_bytes)];
  }
  if (cleanup_success_count > 0) {
    NSString* cleanup_label =
        cleanupMode == XeniaIOSZarConversionCleanupMode::kDeleteOriginalsAfterSuccess
            ? @"Deleted originals"
            : @"Cleaned originals";
    [lines addObject:[NSString stringWithFormat:@"%@: %lu.", cleanup_label,
                                                static_cast<unsigned long>(cleanup_success_count)]];
  }
  if (cleanup_failure_count > 0) {
    [lines addObject:[NSString stringWithFormat:@"%lu cleanup step(s) failed.",
                                                static_cast<unsigned long>(cleanup_failure_count)]];
  }
  if (cancelled_count > 0) {
    [lines addObject:@"Partial .zar output from the cancelled item was removed."];
  }

  NSString* summary = [lines componentsJoinedByString:@"\n"];
  __unsafe_unretained XeniaIOSZarConversionCoordinator* unsafe_self = self;
  if (!zar_conversion_preflight_controller_) {
    [self presentZarConversionPreflightWithTitle:title
                                    requestCount:results.size()
                                   cancelHandler:nil];
  }
  zar_conversion_preflight_controller_.dismissHandler = ^{
    [unsafe_self dismissZarConversionPreflightWithCompletion:nil];
  };
  [zar_conversion_preflight_controller_ showResultsWithTitle:title
                                                     summary:summary
                                                        rows:item_rows
                                                     actions:cleanup_actions];

  XeniaIOSStatusToastStyle toast_style = XeniaIOSStatusToastStyleSuccess;
  NSString* toast_message = @"ZAR conversion finished.";
  if (cancelled_count > 0 || skipped_count > 0) {
    toast_style = XeniaIOSStatusToastStyleInfo;
    toast_message = @"ZAR conversion cancelled.";
  } else if (failure_count > 0 || cleanup_failure_count > 0 || success_count == 0) {
    toast_style = success_count > 0 ? XeniaIOSStatusToastStyleInfo : XeniaIOSStatusToastStyleError;
    toast_message =
        success_count > 0 ? @"ZAR conversion finished with errors." : @"ZAR conversion failed.";
  }
  [self showStatusToast:toast_message style:toast_style];
}

- (void)deleteOriginalGameAfterZarConversionAtPath:(const std::filesystem::path&)sourcePath {
  bool attempted = false;
  std::string message;
  const bool success =
      XEZarCleanupOriginalAfterConversion(sourcePath, false, false, &attempted, &message);
  [self refreshImportedGamesAsync];
  [self showStatusToast:ToNSString(message)
                  style:success ? XeniaIOSStatusToastStyleSuccess : XeniaIOSStatusToastStyleError];
}

- (void)deleteExternalGameFilesAfterZarConversionAtPath:(const std::filesystem::path&)sourcePath {
  NSError* external_access_error = nil;
  BOOL matched_external_location = NO;
  XeniaIOSExternalLibraryAccess* external_access = [xe::ui::StartIOSExternalLibraryAccessForPath(
      sourcePath, &matched_external_location, &external_access_error) retain];
  if (matched_external_location && !external_access) {
    XEPresentOKAlert([self topPresentedControllerForModalPresentation],
                     @"External Folder Unavailable",
                     external_access_error.localizedDescription
                         ?: @"XeniOS could not access the linked external folder.");
    return;
  }
  std::string message;
  const bool success = XEZarDeleteSourceFiles(sourcePath, &message);
  [external_access release];
  [self refreshImportedGamesAsync];
  [self showStatusToast:ToNSString(message)
                  style:success ? XeniaIOSStatusToastStyleSuccess : XeniaIOSStatusToastStyleError];
}

- (void)unlinkExternalGameForConvertedSourcePath:(const std::filesystem::path&)sourcePath {
  NSString* hidden_name = nil;
  NSError* error = nil;
  BOOL success = xe::ui::HideIOSExternalLibraryGameAtPath(sourcePath, &hidden_name, &error);
  NSString* message = nil;
  if (success) {
    message = hidden_name.length > 0 ? [NSString stringWithFormat:@"Unlinked %@.", hidden_name]
                                     : @"Unlinked original game.";
  } else {
    message = error.localizedDescription ?: @"Failed to unlink original game.";
  }
  [self refreshImportedGamesAsync];
  [self showStatusToast:message
                  style:success ? XeniaIOSStatusToastStyleSuccess : XeniaIOSStatusToastStyleError];
}

@end
