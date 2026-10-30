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
