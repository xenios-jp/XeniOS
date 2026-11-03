/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_ZAR_CONVERTER_H_
#define XENIA_VFS_ZAR_CONVERTER_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace xe {
namespace vfs {

struct ZarConversionResult {
  bool success = false;
  bool cancelled = false;
  // The output volume ran out of space (ENOSPC/EDQUOT while writing).
  bool storage_full = false;
  // Internal: a write to the output failed; the conversion stops at the next
  // check. error_message holds the reason.
  bool output_failed = false;
  std::string error_message;
  uint32_t files_written = 0;
  uint64_t bytes_written = 0;
  uint64_t total_bytes = 0;
};

struct ZarConversionProgress {
  uint32_t files_written = 0;
  uint64_t bytes_written = 0;
  uint64_t total_bytes = 0;
  std::string current_path;
  bool finalizing = false;
  bool cancelled = false;
};

struct ZarConversionEstimate {
  bool success = false;
  std::string error_message;
  uint64_t input_bytes = 0;
  uint64_t estimated_output_bytes = 0;
  uint64_t sampled_input_bytes = 0;
  uint64_t sampled_output_bytes = 0;
  uint32_t files_seen = 0;
};

using ZarConversionProgressCallback =
    std::function<void(const ZarConversionProgress& progress)>;
using ZarConversionCancelCallback = std::function<bool()>;

ZarConversionEstimate EstimatePathToZar(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_path,
    uint64_t max_sample_bytes = 16ull * 1024ull * 1024ull);
// Writes "<output_path>.part" and renames it to output_path only after the
// archive is finalized, so a library scan never lists a half-written ZAR; the
// ".part" file is deleted on failure or cancel. Progress callbacks are
// throttled (at most every 100 ms and only on >= 1% change, plus the first,
// finalizing and last updates).
ZarConversionResult ConvertPathToZar(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_path,
    ZarConversionProgressCallback progress_callback = {},
    ZarConversionCancelCallback cancel_callback = {});
bool ValidateZarArchive(const std::filesystem::path& path,
                        std::string* error_message_out = nullptr);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_ZAR_CONVERTER_H_
