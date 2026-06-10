/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/zar_converter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

#include "third_party/zarchive/include/zarchive/zarchivereader.h"
#include "third_party/zarchive/include/zarchive/zarchivewriter.h"

#include <zstd.h>

namespace xe {
namespace vfs {
namespace {

constexpr size_t kCopyBufferSize = 1024 * 1024;
constexpr size_t kEstimateCompressionBlockSize = 64 * 1024;
constexpr int kZarCompressionLevel = 6;

struct ZarOutputContext {
  std::filesystem::path output_path;
  std::ofstream stream;
  bool has_error = false;
  std::string error_message;
};

void SetError(ZarConversionResult* result, std::string message) {
  if (result && result->error_message.empty()) {
    result->error_message = std::move(message);
  }
}

void ReportProgress(const ZarConversionProgressCallback& progress_callback,
                    const ZarConversionResult& result,
                    const std::string& current_path, bool finalizing = false) {
  if (!progress_callback) {
    return;
  }
  ZarConversionProgress progress;
  progress.files_written = result.files_written;
  progress.bytes_written = result.bytes_written;
  progress.total_bytes = result.total_bytes;
  progress.current_path = current_path;
  progress.finalizing = finalizing;
  progress.cancelled = result.cancelled;
  progress_callback(progress);
}

bool CheckCancellation(const ZarConversionCancelCallback& cancel_callback,
                       ZarConversionResult* result) {
  if (!cancel_callback || !cancel_callback()) {
    return false;
  }
  if (result) {
    result->cancelled = true;
    SetError(result, "Conversion cancelled.");
  }
  return true;
}

std::string PathForMessage(const std::filesystem::path& path) {
  return path.string();
}

const char* BoolForLog(bool value) { return value ? "true" : "false"; }

const char* SourceKindForLog(const std::filesystem::path& source_path) {
  const std::string extension =
      xe::utf8::lower_ascii(source_path.extension().string());
  if (extension == ".iso") {
    return "iso";
  }
  const std::string filename =
      xe::utf8::lower_ascii(source_path.filename().string());
  if (filename == "default.xex") {
    return "xex_directory";
  }
  std::error_code ec;
  if (std::filesystem::is_directory(source_path, ec)) {
    return "host_directory";
  }
  if (extension.empty()) {
    return "xcontent_or_god";
  }
  return "unknown";
}

std::filesystem::path WeaklyCanonicalOrAbsolute(
    const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    return canonical;
  }
  ec.clear();
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  return ec ? path : absolute;
}

bool PathsReferToSameFile(const std::filesystem::path& left,
                          const std::filesystem::path& right) {
  std::error_code ec;
  if (std::filesystem::equivalent(left, right, ec)) {
    return true;
  }
  ec.clear();
  return WeaklyCanonicalOrAbsolute(left) == WeaklyCanonicalOrAbsolute(right);
}

void ZarOpenOutputFile(const int32_t part_index, void* context) {
  (void)part_index;
  auto* output_context = reinterpret_cast<ZarOutputContext*>(context);
  output_context->stream = std::ofstream(output_context->output_path,
                                         std::ios::binary | std::ios::trunc);
  if (!output_context->stream.is_open()) {
    output_context->has_error = true;
    output_context->error_message = "Failed to create output file: " +
                                    PathForMessage(output_context->output_path);
  }
}

void ZarWriteOutputData(const void* data, size_t length, void* context) {
  auto* output_context = reinterpret_cast<ZarOutputContext*>(context);
  if (output_context->has_error) {
    return;
  }
  output_context->stream.write(reinterpret_cast<const char*>(data), length);
  if (!output_context->stream.good()) {
    output_context->has_error = true;
    output_context->error_message = "Failed writing output file: " +
                                    PathForMessage(output_context->output_path);
  }
}

bool ShouldSkipHostPath(const std::filesystem::path& path,
                        const std::filesystem::path& output_path) {
  return PathsReferToSameFile(path, output_path);
}

uint64_t SaturatingAdd(uint64_t lhs, uint64_t rhs) {
  const uint64_t max_value = std::numeric_limits<uint64_t>::max();
  return lhs > max_value - rhs ? max_value : lhs + rhs;
}

bool IsDirectoryEntry(const Entry* entry) {
  return (entry->attributes() & kFileAttributeDirectory) != 0;
}

uint64_t EstimateHostDirectoryBytes(
    const std::filesystem::path& input_directory,
    const std::filesystem::path& output_path) {
  uint64_t total = 0;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      input_directory,
      std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return 0;
  }

  for (std::filesystem::recursive_directory_iterator end; it != end;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const std::filesystem::directory_entry& entry = *it;
    if (!entry.is_regular_file(ec) ||
        ShouldSkipHostPath(entry.path(), output_path)) {
      ec.clear();
      continue;
    }
    const uintmax_t file_size = entry.file_size(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    total = SaturatingAdd(total, static_cast<uint64_t>(file_size));
  }
  return total;
}

uint64_t EstimateVfsEntryBytes(const Entry* entry) {
  if (IsDirectoryEntry(entry)) {
    uint64_t total = 0;
    for (const auto& child : entry->children()) {
      total = SaturatingAdd(total, EstimateVfsEntryBytes(child.get()));
    }
    return total;
  }
  return static_cast<uint64_t>(entry->size());
}

uint64_t EstimateVfsDeviceBytes(Device* device) {
  Entry* root = device->ResolvePath("");
  if (!root) {
    root = device->ResolvePath("/");
  }
  if (!root) {
    return 0;
  }

  uint64_t total = 0;
  for (const auto& child : root->children()) {
    total = SaturatingAdd(total, EstimateVfsEntryBytes(child.get()));
  }
  return total;
}

bool AppendHostFileToZar(ZArchiveWriter* writer,
                         const std::filesystem::path& input_path,
                         const std::string& archive_path,
                         std::vector<uint8_t>* buffer,
                         ZarConversionResult* result,
                         const ZarConversionProgressCallback& progress_callback,
                         const ZarConversionCancelCallback& cancel_callback) {
  if (CheckCancellation(cancel_callback, result)) {
    return false;
  }
  if (!writer->StartNewFile(archive_path.c_str())) {
    SetError(result, "Failed to create archive entry: " + archive_path);
    return false;
  }

  std::ifstream input(input_path, std::ios::binary);
  if (!input.is_open()) {
    SetError(result,
             "Failed to open input file: " + PathForMessage(input_path));
    return false;
  }

  while (input.good()) {
    if (CheckCancellation(cancel_callback, result)) {
      return false;
    }
    input.read(reinterpret_cast<char*>(buffer->data()), buffer->size());
    std::streamsize read_bytes = input.gcount();
    if (read_bytes <= 0) {
      break;
    }
    if (CheckCancellation(cancel_callback, result)) {
      return false;
    }
    writer->AppendData(buffer->data(), static_cast<size_t>(read_bytes));
    result->bytes_written += static_cast<uint64_t>(read_bytes);
    ReportProgress(progress_callback, *result, archive_path);
  }

  if (input.bad()) {
    SetError(result,
             "Failed reading input file: " + PathForMessage(input_path));
    return false;
  }

  if (CheckCancellation(cancel_callback, result)) {
    return false;
  }
  ++result->files_written;
  ReportProgress(progress_callback, *result, archive_path);
  return true;
}

bool PackHostDirectoryToZar(
    ZArchiveWriter* writer, const std::filesystem::path& input_directory,
    const std::filesystem::path& output_path, ZarConversionResult* result,
    const ZarConversionProgressCallback& progress_callback,
    const ZarConversionCancelCallback& cancel_callback) {
  std::vector<uint8_t> buffer(kCopyBufferSize);
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      input_directory,
      std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    SetError(result, "Failed scanning input folder: " + ec.message());
    return false;
  }

  for (std::filesystem::recursive_directory_iterator end; it != end;
       it.increment(ec)) {
    if (CheckCancellation(cancel_callback, result)) {
      return false;
    }

    const std::filesystem::directory_entry& entry = *it;
    std::filesystem::path relative_path =
        std::filesystem::relative(entry.path(), input_directory, ec);
    if (ec) {
      ec.clear();
      relative_path = entry.path().filename();
    }
    std::string archive_path = relative_path.generic_string();

    if (entry.is_directory(ec)) {
      if (!archive_path.empty() &&
          !writer->MakeDir(archive_path.c_str(), true)) {
        SetError(result, "Failed to create archive directory: " + archive_path);
        return false;
      }
      continue;
    }
    if (ec) {
      ec.clear();
      continue;
    }
    if (!entry.is_regular_file(ec) ||
        ShouldSkipHostPath(entry.path(), output_path)) {
      ec.clear();
      continue;
    }
    if (!AppendHostFileToZar(writer, entry.path(), archive_path, &buffer,
                             result, progress_callback, cancel_callback)) {
      return false;
    }
  }
  // A failed increment() resets the iterator to end(), terminating the loop,
  // so the error must be checked here - an archive missing files must not be
  // reported as a successful conversion.
  if (ec) {
    SetError(result, "Failed scanning input folder: " + ec.message());
    return false;
  }

  return true;
}

std::string JoinArchivePath(const std::string& base, const std::string& name) {
  if (base.empty()) {
    return name;
  }
  return base + "/" + name;
}

bool AppendVfsFileToZar(ZArchiveWriter* writer, Entry* entry,
                        const std::string& archive_path,
                        std::vector<uint8_t>* buffer,
                        ZarConversionResult* result,
                        const ZarConversionProgressCallback& progress_callback,
                        const ZarConversionCancelCallback& cancel_callback) {
  if (CheckCancellation(cancel_callback, result)) {
    return false;
  }
  if (!writer->StartNewFile(archive_path.c_str())) {
    SetError(result, "Failed to create archive entry: " + archive_path);
    return false;
  }

  File* file = nullptr;
  X_STATUS status =
      entry->Open(xe::filesystem::FileAccess::kGenericRead, &file);
  if (status != X_STATUS_SUCCESS || !file) {
    SetError(result, "Failed to open virtual file: " + archive_path);
    return false;
  }

  size_t offset = 0;
  while (offset < entry->size()) {
    if (CheckCancellation(cancel_callback, result)) {
      file->Destroy();
      return false;
    }
    const size_t read_length = std::min(buffer->size(), entry->size() - offset);
    size_t bytes_read = 0;
    status = file->ReadSync(std::span<uint8_t>(buffer->data(), read_length),
                            offset, &bytes_read);
    if (status != X_STATUS_SUCCESS && status != X_STATUS_END_OF_FILE) {
      file->Destroy();
      SetError(result, "Failed reading virtual file: " + archive_path);
      return false;
    }
    if (!bytes_read) {
      break;
    }
    if (CheckCancellation(cancel_callback, result)) {
      file->Destroy();
      return false;
    }
    writer->AppendData(buffer->data(), bytes_read);
    offset += bytes_read;
    result->bytes_written += static_cast<uint64_t>(bytes_read);
    ReportProgress(progress_callback, *result, archive_path);
  }

  file->Destroy();
  if (offset != entry->size()) {
    SetError(result, "Short read from virtual file: " + archive_path);
    return false;
  }

  if (CheckCancellation(cancel_callback, result)) {
    return false;
  }
  ++result->files_written;
  ReportProgress(progress_callback, *result, archive_path);
  return true;
}

struct ZarCompressionSample {
  uint64_t input_bytes = 0;
  uint64_t output_bytes = 0;
  uint32_t files_seen = 0;
};

uint64_t EstimateZarMetadataOverhead(uint64_t input_bytes,
                                     uint32_t files_seen) {
  if (!input_bytes) {
    return 0;
  }
  const uint64_t block_count =
      (input_bytes + kEstimateCompressionBlockSize - 1) /
      kEstimateCompressionBlockSize;
  return 256ull * 1024ull + block_count * 16ull + uint64_t(files_seen) * 256ull;
}

void AppendCompressionSample(const uint8_t* data, size_t length,
                             ZarCompressionSample* sample) {
  if (!data || !length || !sample) {
    return;
  }

  const size_t bound = ZSTD_compressBound(length);
  std::vector<uint8_t> compressed(bound);
  const size_t compressed_size = ZSTD_compress(
      compressed.data(), compressed.size(), data, length, kZarCompressionLevel);
  size_t stored_size = length;
  if (!ZSTD_isError(compressed_size) && compressed_size < length) {
    stored_size = compressed_size;
  }
  sample->input_bytes += static_cast<uint64_t>(length);
  sample->output_bytes += static_cast<uint64_t>(stored_size);
}

uint64_t EstimateZarOutputBytes(uint64_t input_bytes,
                                const ZarCompressionSample& sample) {
  if (!input_bytes) {
    return 0;
  }
  if (!sample.input_bytes || !sample.output_bytes) {
    return input_bytes +
           EstimateZarMetadataOverhead(input_bytes, sample.files_seen);
  }

  const double ratio =
      std::min(1.0, static_cast<double>(sample.output_bytes) /
                        static_cast<double>(sample.input_bytes));
  const double estimated_payload =
      std::ceil(static_cast<double>(input_bytes) * ratio);
  const uint64_t payload_bytes =
      estimated_payload >=
              static_cast<double>(std::numeric_limits<uint64_t>::max())
          ? std::numeric_limits<uint64_t>::max()
          : static_cast<uint64_t>(estimated_payload);
  return SaturatingAdd(payload_bytes, EstimateZarMetadataOverhead(
                                          input_bytes, sample.files_seen));
}

void SampleHostFileForZarEstimate(const std::filesystem::path& path,
                                  uint64_t max_sample_bytes,
                                  ZarCompressionSample* sample) {
  if (!sample || sample->input_bytes >= max_sample_bytes) {
    return;
  }
  ++sample->files_seen;

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return;
  }

  std::vector<uint8_t> buffer(kEstimateCompressionBlockSize);
  const uint64_t remaining_sample = max_sample_bytes - sample->input_bytes;
  const size_t read_size =
      static_cast<size_t>(std::min<uint64_t>(buffer.size(), remaining_sample));
  input.read(reinterpret_cast<char*>(buffer.data()), read_size);
  const std::streamsize read_bytes = input.gcount();
  if (read_bytes > 0) {
    AppendCompressionSample(buffer.data(), static_cast<size_t>(read_bytes),
                            sample);
  }
}

void SampleHostDirectoryForZarEstimate(
    const std::filesystem::path& input_directory,
    const std::filesystem::path& output_path, uint64_t max_sample_bytes,
    ZarCompressionSample* sample) {
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      input_directory,
      std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return;
  }

  for (std::filesystem::recursive_directory_iterator end;
       it != end && sample && sample->input_bytes < max_sample_bytes;
       it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const std::filesystem::directory_entry& entry = *it;
    if (!entry.is_regular_file(ec) ||
        ShouldSkipHostPath(entry.path(), output_path)) {
      ec.clear();
      continue;
    }
    SampleHostFileForZarEstimate(entry.path(), max_sample_bytes, sample);
  }
}

void SampleVfsEntryForZarEstimate(Entry* entry, uint64_t max_sample_bytes,
                                  ZarCompressionSample* sample) {
  if (!entry || !sample || sample->input_bytes >= max_sample_bytes) {
    return;
  }
  if (IsDirectoryEntry(entry)) {
    for (const auto& child : entry->children()) {
      SampleVfsEntryForZarEstimate(child.get(), max_sample_bytes, sample);
      if (sample->input_bytes >= max_sample_bytes) {
        break;
      }
    }
    return;
  }

  ++sample->files_seen;
  File* file = nullptr;
  X_STATUS status =
      entry->Open(xe::filesystem::FileAccess::kGenericRead, &file);
  if (status != X_STATUS_SUCCESS || !file) {
    return;
  }

  std::vector<uint8_t> buffer(kEstimateCompressionBlockSize);
  const uint64_t remaining_sample = max_sample_bytes - sample->input_bytes;
  const size_t read_size = static_cast<size_t>(
      std::min<uint64_t>({buffer.size(), remaining_sample, entry->size()}));
  size_t bytes_read = 0;
  if (read_size) {
    status = file->ReadSync(std::span<uint8_t>(buffer.data(), read_size), 0,
                            &bytes_read);
    if ((status == X_STATUS_SUCCESS || status == X_STATUS_END_OF_FILE) &&
        bytes_read) {
      AppendCompressionSample(buffer.data(), bytes_read, sample);
    }
  }
  file->Destroy();
}

void SampleVfsDeviceForZarEstimate(Device* device, uint64_t max_sample_bytes,
                                   ZarCompressionSample* sample) {
  Entry* root = device ? device->ResolvePath("") : nullptr;
  if (!root && device) {
    root = device->ResolvePath("/");
  }
  if (!root) {
    return;
  }

  for (const auto& child : root->children()) {
    SampleVfsEntryForZarEstimate(child.get(), max_sample_bytes, sample);
    if (sample && sample->input_bytes >= max_sample_bytes) {
      break;
    }
  }
}

bool PackVfsEntryToZar(ZArchiveWriter* writer, Entry* entry,
                       const std::string& archive_path,
                       std::vector<uint8_t>* buffer,
                       ZarConversionResult* result,
                       const ZarConversionProgressCallback& progress_callback,
                       const ZarConversionCancelCallback& cancel_callback) {
  if (CheckCancellation(cancel_callback, result)) {
    return false;
  }
  if (IsDirectoryEntry(entry)) {
    if (!archive_path.empty() && !writer->MakeDir(archive_path.c_str(), true)) {
      SetError(result, "Failed to create archive directory: " + archive_path);
      return false;
    }
    for (const auto& child : entry->children()) {
      if (!PackVfsEntryToZar(
              writer, child.get(), JoinArchivePath(archive_path, child->name()),
              buffer, result, progress_callback, cancel_callback)) {
        return false;
      }
    }
    return true;
  }

  return AppendVfsFileToZar(writer, entry, archive_path, buffer, result,
                            progress_callback, cancel_callback);
}

bool PackVfsDeviceToZar(Device* device, ZArchiveWriter* writer,
                        ZarConversionResult* result,
                        const ZarConversionProgressCallback& progress_callback,
                        const ZarConversionCancelCallback& cancel_callback) {
  Entry* root = device->ResolvePath("");
  if (!root) {
    root = device->ResolvePath("/");
  }
  if (!root) {
    SetError(result, "Failed to resolve virtual filesystem root.");
    return false;
  }

  std::vector<uint8_t> buffer(kCopyBufferSize);
  for (const auto& child : root->children()) {
    if (CheckCancellation(cancel_callback, result)) {
      return false;
    }
    if (!PackVfsEntryToZar(writer, child.get(), child->name(), &buffer, result,
                           progress_callback, cancel_callback)) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<Device> CreateDeviceForSource(
    const std::filesystem::path& source_path, std::string* error_message_out) {
  const std::string extension =
      xe::utf8::lower_ascii(source_path.extension().string());
  if (extension == ".iso") {
    auto device = std::make_unique<DiscImageDevice>("", source_path);
    if (!device->Initialize()) {
      if (error_message_out) {
        *error_message_out = "Failed to read ISO filesystem.";
      }
      return nullptr;
    }
    return device;
  }

  auto content_device =
      XContentContainerDevice::CreateContentDevice("", source_path);
  if (content_device) {
    if (!content_device->Initialize()) {
      if (error_message_out) {
        *error_message_out = "Failed to read content package filesystem.";
      }
      return nullptr;
    }
    return content_device;
  }

  if (error_message_out) {
    *error_message_out = "Unsupported source format for ZAR conversion.";
  }
  return nullptr;
}

std::filesystem::path HostDirectoryForSource(
    const std::filesystem::path& source_path) {
  std::error_code ec;
  if (std::filesystem::is_directory(source_path, ec)) {
    return source_path;
  }
  const std::string filename =
      xe::utf8::lower_ascii(source_path.filename().string());
  if (filename == "default.xex") {
    return source_path.parent_path();
  }
  return {};
}

}  // namespace

ZarConversionEstimate EstimatePathToZar(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_path, uint64_t max_sample_bytes) {
  XELOGI(
      "ZAR estimate: starting source='{}' output='{}' kind={} "
      "max_sample_bytes={}",
      PathForMessage(source_path), PathForMessage(output_path),
      SourceKindForLog(source_path), max_sample_bytes);
  ZarConversionEstimate estimate;
  std::error_code ec;
  if (!std::filesystem::exists(source_path, ec)) {
    estimate.error_message = "Source path does not exist.";
    XELOGI("ZAR estimate: failed source='{}' error='{}'",
           PathForMessage(source_path), estimate.error_message);
    return estimate;
  }

  const std::filesystem::path host_directory =
      HostDirectoryForSource(source_path);
  std::unique_ptr<Device> device;
  if (host_directory.empty()) {
    std::string device_error;
    device = CreateDeviceForSource(source_path, &device_error);
    if (!device) {
      estimate.error_message = device_error;
      XELOGI(
          "ZAR estimate: failed creating source device source='{}' "
          "kind={} error='{}'",
          PathForMessage(source_path), SourceKindForLog(source_path),
          estimate.error_message);
      return estimate;
    }
  }

  XELOGI("ZAR estimate: prepared source='{}' kind={} host_directory='{}'",
         PathForMessage(source_path), SourceKindForLog(source_path),
         PathForMessage(host_directory));

  estimate.input_bytes =
      host_directory.empty()
          ? EstimateVfsDeviceBytes(device.get())
          : EstimateHostDirectoryBytes(host_directory, output_path);
  ZarCompressionSample sample;
  if (max_sample_bytes > 0 && estimate.input_bytes > 0) {
    if (host_directory.empty()) {
      SampleVfsDeviceForZarEstimate(device.get(), max_sample_bytes, &sample);
    } else {
      SampleHostDirectoryForZarEstimate(host_directory, output_path,
                                        max_sample_bytes, &sample);
    }
  }

  estimate.sampled_input_bytes = sample.input_bytes;
  estimate.sampled_output_bytes = sample.output_bytes;
  estimate.files_seen = sample.files_seen;
  estimate.estimated_output_bytes =
      EstimateZarOutputBytes(estimate.input_bytes, sample);
  estimate.success =
      estimate.input_bytes > 0 && estimate.estimated_output_bytes > 0;
  if (!estimate.success && estimate.error_message.empty()) {
    estimate.error_message = "No convertible data was found.";
  }
  XELOGI(
      "ZAR estimate: finished source='{}' success={} input_bytes={} "
      "estimated_output_bytes={} sampled_input_bytes={} "
      "sampled_output_bytes={} files_seen={} error='{}'",
      PathForMessage(source_path), BoolForLog(estimate.success),
      estimate.input_bytes, estimate.estimated_output_bytes,
      estimate.sampled_input_bytes, estimate.sampled_output_bytes,
      estimate.files_seen, estimate.error_message);
  return estimate;
}

ZarConversionResult ConvertPathToZar(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_path,
    ZarConversionProgressCallback progress_callback,
    ZarConversionCancelCallback cancel_callback,
    const ZarConversionOptions& options) {
  XELOGI("ZAR convert: starting source='{}' output='{}' kind={}",
         PathForMessage(source_path), PathForMessage(output_path),
         SourceKindForLog(source_path));
  ZarConversionResult result;
  std::error_code ec;
  if (CheckCancellation(cancel_callback, &result)) {
    XELOGI("ZAR convert: cancelled before start source='{}'",
           PathForMessage(source_path));
    return result;
  }
  if (!std::filesystem::exists(source_path, ec)) {
    result.error_message = "Source path does not exist.";
    XELOGI("ZAR convert: failed source='{}' error='{}'",
           PathForMessage(source_path), result.error_message);
    return result;
  }
  if (output_path.empty()) {
    result.error_message = "Output path is empty.";
    XELOGI("ZAR convert: failed source='{}' error='{}'",
           PathForMessage(source_path), result.error_message);
    return result;
  }
  if (PathsReferToSameFile(source_path, output_path)) {
    result.error_message =
        "Output path must be different from the source path.";
    XELOGI("ZAR convert: failed source='{}' output='{}' error='{}'",
           PathForMessage(source_path), PathForMessage(output_path),
           result.error_message);
    return result;
  }

  const std::filesystem::path host_directory =
      HostDirectoryForSource(source_path);
  std::unique_ptr<Device> device;
  if (!host_directory.empty()) {
    // Host folder sources are packed directly.
  } else {
    std::string device_error;
    device = CreateDeviceForSource(source_path, &device_error);
    if (!device) {
      result.error_message = device_error;
      XELOGI(
          "ZAR convert: failed creating source device source='{}' "
          "kind={} error='{}'",
          PathForMessage(source_path), SourceKindForLog(source_path),
          result.error_message);
      return result;
    }
  }
  result.total_bytes =
      host_directory.empty()
          ? EstimateVfsDeviceBytes(device.get())
          : EstimateHostDirectoryBytes(host_directory, output_path);
  XELOGI(
      "ZAR convert: prepared source='{}' kind={} host_directory='{}' "
      "total_bytes={}",
      PathForMessage(source_path), SourceKindForLog(source_path),
      PathForMessage(host_directory), result.total_bytes);
  ReportProgress(progress_callback, result, std::string());

  if (!output_path.parent_path().empty()) {
    std::filesystem::create_directories(output_path.parent_path(), ec);
    if (ec) {
      result.error_message = "Failed to create output folder: " + ec.message();
      XELOGI(
          "ZAR convert: failed creating output folder output='{}' "
          "error='{}'",
          PathForMessage(output_path), result.error_message);
      return result;
    }
  }

  ZarOutputContext output_context;
  output_context.output_path = output_path;
  ZArchiveWriter writer(ZarOpenOutputFile, ZarWriteOutputData, &output_context,
                        options.compression_thread_count,
                        options.compression_thread_initializer);
  if (output_context.has_error) {
    result.error_message = output_context.error_message;
    XELOGI("ZAR convert: failed opening output='{}' error='{}'",
           PathForMessage(output_path), result.error_message);
    return result;
  }

  const bool parallel_compression = options.compression_thread_count > 1;
  const unsigned compression_workers =
      parallel_compression ? options.compression_thread_count : 1u;
  XELOGI("ZAR convert: compression={} workers={} output='{}'",
         parallel_compression ? "parallel" : "synchronous", compression_workers,
         PathForMessage(output_path));
  const auto convert_start = std::chrono::steady_clock::now();

  bool packed = false;
  if (!host_directory.empty()) {
    XELOGI("ZAR convert: packing host directory='{}' output='{}'",
           PathForMessage(host_directory), PathForMessage(output_path));
    packed =
        PackHostDirectoryToZar(&writer, host_directory, output_path, &result,
                               progress_callback, cancel_callback);
  } else {
    XELOGI("ZAR convert: packing virtual device source='{}' output='{}'",
           PathForMessage(source_path), PathForMessage(output_path));
    packed = PackVfsDeviceToZar(device.get(), &writer, &result,
                                progress_callback, cancel_callback);
  }

  if (packed && !CheckCancellation(cancel_callback, &result)) {
    XELOGI(
        "ZAR convert: finalizing output='{}' files={} bytes={} "
        "total_bytes={}",
        PathForMessage(output_path), result.files_written, result.bytes_written,
        result.total_bytes);
    ReportProgress(progress_callback, result, std::string(), true);
    writer.Finalize();
    output_context.stream.flush();
    if (!output_context.stream.good()) {
      output_context.has_error = true;
      output_context.error_message =
          "Failed finalizing output file: " + PathForMessage(output_path);
      XELOGI(
          "ZAR convert: output stream finalization failed output='{}' "
          "error='{}'",
          PathForMessage(output_path), output_context.error_message);
    }
    CheckCancellation(cancel_callback, &result);
  } else {
    // Finalize() is skipped on cancel/failure, but it is what drains and joins
    // the parallel compression pool. Abort here so no serializer thread is
    // still writing to the stream we are about to close and remove.
    writer.Abort();
  }
  output_context.stream.close();

  if (result.cancelled || !packed || output_context.has_error ||
      result.files_written == 0) {
    if (result.error_message.empty()) {
      result.error_message = output_context.has_error
                                 ? output_context.error_message
                                 : "No files were converted.";
    }
    std::filesystem::remove(output_path, ec);
    XELOGI(
        "ZAR convert: failed source='{}' output='{}' packed={} "
        "cancelled={} output_error={} files={} bytes={} total_bytes={} "
        "remove_error='{}' error='{}'",
        PathForMessage(source_path), PathForMessage(output_path),
        BoolForLog(packed), BoolForLog(result.cancelled),
        BoolForLog(output_context.has_error), result.files_written,
        result.bytes_written, result.total_bytes, ec ? ec.message() : "none",
        result.error_message);
    return result;
  }

  result.success = true;
  const double elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    convert_start)
          .count();
  std::error_code output_size_ec;
  const uint64_t output_bytes =
      std::filesystem::file_size(output_path, output_size_ec);
  const double input_mib =
      static_cast<double>(result.bytes_written) / (1024.0 * 1024.0);
  const double throughput_mib_s =
      elapsed_seconds > 0.0 ? input_mib / elapsed_seconds : 0.0;
  const double ratio = (result.bytes_written > 0 && !output_size_ec)
                           ? static_cast<double>(output_bytes) /
                                 static_cast<double>(result.bytes_written)
                           : 0.0;
  XELOGI(
      "ZAR convert: success source='{}' output='{}' files={} bytes={} "
      "total_bytes={} output_bytes={} workers={} elapsed={:.2f}s "
      "throughput={:.1f} MiB/s ratio={:.3f}",
      PathForMessage(source_path), PathForMessage(output_path),
      result.files_written, result.bytes_written, result.total_bytes,
      output_size_ec ? 0ull : output_bytes, compression_workers,
      elapsed_seconds, throughput_mib_s, ratio);
  return result;
}

bool ValidateZarArchive(const std::filesystem::path& path,
                        std::string* error_message_out) {
  XELOGI("ZAR validate: starting path='{}'", PathForMessage(path));
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    if (error_message_out) {
      *error_message_out = "ZAR output file is missing.";
    }
    XELOGI(
        "ZAR validate: failed path='{}' error='{}'", PathForMessage(path),
        error_message_out ? *error_message_out : "ZAR output file is missing.");
    return false;
  }
  ec.clear();
  const uintmax_t file_size = std::filesystem::file_size(path, ec);
  if (ec || file_size == 0) {
    if (error_message_out) {
      *error_message_out =
          ec ? "Failed reading ZAR output size: " + ec.message()
             : "ZAR output file is empty.";
    }
    XELOGI("ZAR validate: failed path='{}' file_size={} error='{}'",
           PathForMessage(path), ec ? 0 : static_cast<uint64_t>(file_size),
           error_message_out ? *error_message_out
                             : (ec ? "Failed reading ZAR output size."
                                   : "ZAR output file is empty."));
    return false;
  }
  std::unique_ptr<ZArchiveReader> reader(ZArchiveReader::OpenFromFile(path));
  if (!reader) {
    if (error_message_out) {
      *error_message_out = "ZAR output is incomplete or not a valid archive.";
    }
    XELOGI("ZAR validate: failed opening archive path='{}' file_size={}",
           PathForMessage(path), static_cast<uint64_t>(file_size));
    return false;
  }
  XELOGI("ZAR validate: success path='{}' file_size={}", PathForMessage(path),
         static_cast<uint64_t>(file_size));
  return true;
}

}  // namespace vfs
}  // namespace xe
