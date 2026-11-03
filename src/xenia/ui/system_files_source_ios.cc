/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/system_files_source_ios.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/memory.h"
#include "xenia/base/utf8.h"

namespace xe {
namespace ui {

namespace {

constexpr std::string_view kUpdateFolderName = "$SystemUpdate";
constexpr std::string_view kUpdateFolder = "$SystemUpdate/";
// Far above the 16 MiB of the largest file in an update.
constexpr uint64_t kMaxUpdateFileSize = 512 * 1024 * 1024;

// Whether |name| names a file in the folder it is written to.
bool IsPlainFileName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("/\\:") == std::string_view::npos;
}

// Reads the files of a zip that are stored or deflated, with zip64 sizes and
// offsets. Encrypted and split archives aren't read.
class ZipReader {
 public:
  struct Entry {
    // As the archive spells it, with '/' between folders.
    std::string name;
    uint64_t size = 0;
    uint64_t compressed_size = 0;
    uint64_t local_header_offset = 0;
    uint32_t crc32 = 0;
    uint16_t method = 0;
    uint16_t flags = 0;

    bool is_directory() const { return !name.empty() && name.back() == '/'; }
  };

  // The zip at |path|, or nothing when it can't be opened or its central
  // directory isn't a zip's.
  static std::unique_ptr<ZipReader> Open(const std::filesystem::path& path);

  const std::vector<Entry>& entries() const { return entries_; }

  // Reads all of |entry| into |data|. Fails when the entry is larger than
  // |max_size|, can't be read or unpacked, or doesn't match its CRC.
  bool Read(const Entry& entry, std::vector<uint8_t>& data,
            uint64_t max_size) const;

 private:
  static constexpr uint32_t kLocalHeaderSignature = 0x04034B50;
  static constexpr uint32_t kCentralHeaderSignature = 0x02014B50;
  static constexpr uint32_t kEndSignature = 0x06054B50;
  static constexpr uint32_t kZip64EndSignature = 0x06064B50;
  static constexpr uint32_t kZip64LocatorSignature = 0x07064B50;
  static constexpr size_t kLocalHeaderSize = 30;
  static constexpr size_t kCentralHeaderSize = 46;
  static constexpr size_t kEndSize = 22;
  static constexpr size_t kZip64EndSize = 56;
  static constexpr size_t kZip64LocatorSize = 20;
  static constexpr size_t kMaxCommentSize = 0xFFFF;
  // Far above the central directory of any update.
  static constexpr uint64_t kMaxCentralDirectorySize = 64 * 1024 * 1024;
  static constexpr uint16_t kZip64ExtraId = 0x0001;
  static constexpr uint16_t kFlagEncrypted = 0x0001;
  static constexpr uint16_t kMethodStored = 0;
  static constexpr uint16_t kMethodDeflated = 8;
  static constexpr size_t kReadChunkSize = 256 * 1024;

  ZipReader(std::unique_ptr<filesystem::FileHandle> file, uint64_t file_size)
      : file_(std::move(file)), file_size_(file_size) {}

  bool ReadAt(uint64_t offset, void* buffer, size_t length) const;
  bool ReadCentralDirectory();

  std::unique_ptr<filesystem::FileHandle> file_;
  uint64_t file_size_;
  // Where the entries' data ends, which no entry may run past.
  uint64_t central_directory_offset_ = 0;
  std::vector<Entry> entries_;
};

std::unique_ptr<ZipReader> ZipReader::Open(const std::filesystem::path& path) {
  std::error_code ec;
  const uint64_t file_size = std::filesystem::file_size(path, ec);
  if (ec) {
    return nullptr;
  }
  auto file = filesystem::FileHandle::OpenExisting(
      path, filesystem::FileAccess::kFileReadData);
  if (!file) {
    return nullptr;
  }
  std::unique_ptr<ZipReader> reader(new ZipReader(std::move(file), file_size));
  if (!reader->ReadCentralDirectory()) {
    return nullptr;
  }
  return reader;
}

bool ZipReader::ReadAt(uint64_t offset, void* buffer, size_t length) const {
  if (offset > file_size_ || length > file_size_ - offset) {
    return false;
  }
  // A read may come back short, so it goes on until it has everything.
  auto* bytes = static_cast<uint8_t*>(buffer);
  while (length) {
    size_t bytes_read = 0;
    if (!file_->Read(size_t(offset), bytes, length, &bytes_read) ||
        !bytes_read) {
      return false;
    }
    offset += bytes_read;
    bytes += bytes_read;
    length -= bytes_read;
  }
  return true;
}

bool ZipReader::ReadCentralDirectory() {
  if (file_size_ < kEndSize) {
    return false;
  }
  // The end record is last, followed only by a comment of up to 64 KiB.
  std::vector<uint8_t> tail(
      size_t(std::min<uint64_t>(file_size_, kEndSize + kMaxCommentSize)));
  const uint64_t tail_offset = file_size_ - tail.size();
  if (!ReadAt(tail_offset, tail.data(), tail.size())) {
    return false;
  }
  size_t end = tail.size() - kEndSize;
  while (xe::load<uint32_t>(&tail[end]) != kEndSignature ||
         end + kEndSize + xe::load<uint16_t>(&tail[end + 20]) > tail.size()) {
    if (!end--) {
      return false;
    }
  }
  const uint8_t* record = &tail[end];
  const uint64_t end_offset = tail_offset + end;
  uint64_t disk = xe::load<uint16_t>(record + 4);
  uint64_t directory_disk = xe::load<uint16_t>(record + 6);
  uint64_t disk_entry_count = xe::load<uint16_t>(record + 8);
  uint64_t entry_count = xe::load<uint16_t>(record + 10);
  uint64_t directory_size = xe::load<uint32_t>(record + 12);
  uint64_t directory_offset = xe::load<uint32_t>(record + 16);

  // A zip64 archive has the values that don't fit in a record of its own,
  // which a locator just before the end record points to.
  if (disk_entry_count == 0xFFFF || entry_count == 0xFFFF ||
      directory_size == 0xFFFFFFFF || directory_offset == 0xFFFFFFFF) {
    uint8_t locator[kZip64LocatorSize];
    if (end_offset < kZip64LocatorSize ||
        !ReadAt(end_offset - kZip64LocatorSize, locator, sizeof(locator)) ||
        xe::load<uint32_t>(locator) != kZip64LocatorSignature) {
      return false;
    }
    uint8_t zip64_end[kZip64EndSize];
    if (!ReadAt(xe::load<uint64_t>(locator + 8), zip64_end,
                sizeof(zip64_end)) ||
        xe::load<uint32_t>(zip64_end) != kZip64EndSignature) {
      return false;
    }
    disk = xe::load<uint32_t>(zip64_end + 16);
    directory_disk = xe::load<uint32_t>(zip64_end + 20);
    disk_entry_count = xe::load<uint64_t>(zip64_end + 24);
    entry_count = xe::load<uint64_t>(zip64_end + 32);
    directory_size = xe::load<uint64_t>(zip64_end + 40);
    directory_offset = xe::load<uint64_t>(zip64_end + 48);
  }

  // Archives split over several files aren't read.
  if (disk || directory_disk || disk_entry_count != entry_count) {
    return false;
  }
  if (directory_size > kMaxCentralDirectorySize ||
      directory_offset > end_offset ||
      directory_size > end_offset - directory_offset ||
      entry_count > directory_size / kCentralHeaderSize) {
    return false;
  }
  std::vector<uint8_t> directory(static_cast<size_t>(directory_size));
  if (!ReadAt(directory_offset, directory.data(), directory.size())) {
    return false;
  }

  std::vector<Entry> entries;
  entries.reserve(size_t(entry_count));
  size_t offset = 0;
  for (uint64_t i = 0; i < entry_count; ++i) {
    if (directory.size() - offset < kCentralHeaderSize) {
      return false;
    }
    const uint8_t* header = &directory[offset];
    if (xe::load<uint32_t>(header) != kCentralHeaderSignature) {
      return false;
    }
    const size_t name_size = xe::load<uint16_t>(header + 28);
    const size_t extra_size = xe::load<uint16_t>(header + 30);
    const size_t comment_size = xe::load<uint16_t>(header + 32);
    const size_t record_size =
        kCentralHeaderSize + name_size + extra_size + comment_size;
    if (directory.size() - offset < record_size) {
      return false;
    }
    Entry entry;
    entry.flags = xe::load<uint16_t>(header + 8);
    entry.method = xe::load<uint16_t>(header + 10);
    entry.crc32 = xe::load<uint32_t>(header + 16);
    entry.compressed_size = xe::load<uint32_t>(header + 20);
    entry.size = xe::load<uint32_t>(header + 24);
    entry.local_header_offset = xe::load<uint32_t>(header + 42);
    entry.name.assign(
        reinterpret_cast<const char*>(header + kCentralHeaderSize), name_size);

    // The zip64 extra field holds, in this order, each of these that is all
    // ones here.
    const uint8_t* extra = header + kCentralHeaderSize + name_size;
    for (size_t field = 0; field + 4 <= extra_size;) {
      const size_t field_size = xe::load<uint16_t>(extra + field + 2);
      if (field_size > extra_size - field - 4) {
        return false;
      }
      if (xe::load<uint16_t>(extra + field) == kZip64ExtraId) {
        const uint8_t* values = extra + field + 4;
        size_t used = 0;
        for (uint64_t* value : {&entry.size, &entry.compressed_size,
                                &entry.local_header_offset}) {
          if (*value != 0xFFFFFFFF) {
            continue;
          }
          if (field_size - used < sizeof(uint64_t)) {
            return false;
          }
          *value = xe::load<uint64_t>(values + used);
          used += sizeof(uint64_t);
        }
      }
      field += 4 + field_size;
    }
    if (entry.local_header_offset >= directory_offset) {
      return false;
    }
    entries.push_back(std::move(entry));
    offset += record_size;
  }
  central_directory_offset_ = directory_offset;
  entries_ = std::move(entries);
  return true;
}

bool ZipReader::Read(const Entry& entry, std::vector<uint8_t>& data,
                     uint64_t max_size) const {
  data.clear();
  if (entry.size > max_size || (entry.flags & kFlagEncrypted) ||
      (entry.method != kMethodStored && entry.method != kMethodDeflated)) {
    return false;
  }
  uint8_t header[kLocalHeaderSize];
  if (!ReadAt(entry.local_header_offset, header, sizeof(header)) ||
      xe::load<uint32_t>(header) != kLocalHeaderSignature) {
    return false;
  }
  // The data follows the local header's own name and extra field. Its sizes
  // may be left for a descriptor after the data, so the central directory's
  // are the ones used.
  const uint64_t data_offset = entry.local_header_offset + kLocalHeaderSize +
                               xe::load<uint16_t>(header + 26) +
                               xe::load<uint16_t>(header + 28);
  if (data_offset > central_directory_offset_ ||
      entry.compressed_size > central_directory_offset_ - data_offset) {
    return false;
  }
  data.resize(size_t(entry.size));

  if (entry.method == kMethodStored) {
    if (entry.compressed_size != entry.size ||
        !ReadAt(data_offset, data.data(), data.size())) {
      return false;
    }
  } else {
    zng_stream stream = {};
    if (zng_inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
      return false;
    }
    std::vector<uint8_t> chunk(
        size_t(std::min<uint64_t>(entry.compressed_size, kReadChunkSize)));
    // inflate refuses a null output even when there's nothing to write.
    uint8_t no_output;
    stream.next_out = &no_output;
    uint64_t compressed_read = 0;
    int result = Z_OK;
    // inflate stops with Z_BUF_ERROR once it can't go on, as when the data
    // runs out before the end of the stream.
    while (result == Z_OK) {
      if (!stream.avail_in && compressed_read < entry.compressed_size) {
        const size_t length = size_t(std::min<uint64_t>(
            entry.compressed_size - compressed_read, chunk.size()));
        if (!ReadAt(data_offset + compressed_read, chunk.data(), length)) {
          break;
        }
        compressed_read += length;
        stream.next_in = chunk.data();
        stream.avail_in = uint32_t(length);
      }
      if (!stream.avail_out && stream.total_out < data.size()) {
        stream.next_out = data.data() + stream.total_out;
        stream.avail_out = uint32_t(std::min<uint64_t>(
            data.size() - stream.total_out,
            std::numeric_limits<uint32_t>::max()));
      }
      result = zng_inflate(&stream, Z_NO_FLUSH);
    }
    const bool complete =
        result == Z_STREAM_END && stream.total_out == data.size();
    zng_inflateEnd(&stream);
    if (!complete) {
      return false;
    }
  }
  return zng_crc32_z(0, data.data(), data.size()) == entry.crc32;
}

// Unzips the files of the update folder in |zip_path| into |update_path|, as
// the wx installer does.
SystemFilesResult UnzipUpdate(
    const std::filesystem::path& zip_path,
    const std::filesystem::path& update_path,
    const std::function<bool(size_t done, size_t total)>& progress) {
  const auto zip = ZipReader::Open(zip_path);
  if (!zip || zip->entries().empty()) {
    return SystemFilesResult::kNotSystemUpdate;
  }
  const size_t total = zip->entries().size();
  size_t done = 0;
  std::vector<uint8_t> data;
  for (const auto& entry : zip->entries()) {
    if (!progress(done++, total)) {
      return SystemFilesResult::kCancelled;
    }
    if (entry.is_directory() ||
        !xe::utf8::starts_with_case(entry.name, kUpdateFolder)) {
      continue;
    }
    const std::string file_name = entry.name.substr(kUpdateFolder.size());
    if (!IsPlainFileName(file_name)) {
      return SystemFilesResult::kNotSystemUpdate;
    }
    if (!zip->Read(entry, data, kMaxUpdateFileSize)) {
      return SystemFilesResult::kReadFailed;
    }
    if (!xe::filesystem::WriteAllBytes(update_path / xe::to_path(file_name),
                                       data)) {
      return SystemFilesResult::kWriteFailed;
    }
  }
  return SystemFilesResult::kSuccess;
}

// Reads the whole file at |path| into |data|.
bool ReadUpdateFile(const std::filesystem::path& path,
                    std::vector<uint8_t>& data) {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxUpdateFileSize) {
    return false;
  }
  auto file = filesystem::FileHandle::OpenExisting(
      path, filesystem::FileAccess::kFileReadData);
  if (!file) {
    return false;
  }
  data.resize(size_t(size));
  size_t bytes_read = 0;
  return data.empty() ||
         (file->Read(0, data.data(), data.size(), &bytes_read) &&
          bytes_read == data.size());
}

// Copies the files of an update folder into |update_path|. |folder| is the
// update's $SystemUpdate folder or holds it.
SystemFilesResult CopyUpdateFolder(
    const std::filesystem::path& folder,
    const std::filesystem::path& update_path,
    const std::function<bool(size_t done, size_t total)>& progress) {
  std::error_code ec;
  std::filesystem::path files_folder = folder;
  for (std::filesystem::directory_iterator it(folder, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) &&
        xe::utf8::equal_case(xe::path_to_utf8(it->path().filename()),
                             kUpdateFolderName)) {
      files_folder = it->path();
      break;
    }
  }
  std::vector<std::filesystem::path> files;
  for (std::filesystem::directory_iterator it(files_folder, ec), end;
       !ec && it != end; it.increment(ec)) {
    const std::string name = xe::path_to_utf8(it->path().filename());
    // An update has no folders, and the hidden files Finder and Files leave
    // aren't the update's.
    if (it->is_directory(ec) || name.starts_with('.')) {
      continue;
    }
    if (!IsPlainFileName(name)) {
      return SystemFilesResult::kNotSystemUpdate;
    }
    files.push_back(it->path());
  }
  if (ec) {
    return SystemFilesResult::kReadFailed;
  }
  if (files.empty()) {
    return SystemFilesResult::kNotSystemUpdate;
  }
  std::vector<uint8_t> data;
  for (size_t i = 0; i < files.size(); ++i) {
    if (!progress(i, files.size())) {
      return SystemFilesResult::kCancelled;
    }
    if (!ReadUpdateFile(files[i], data)) {
      return SystemFilesResult::kReadFailed;
    }
    if (!xe::filesystem::WriteAllBytes(update_path / files[i].filename(),
                                       data)) {
      return SystemFilesResult::kWriteFailed;
    }
  }
  return SystemFilesResult::kSuccess;
}

}  // namespace

SystemFilesResult ExtractSystemUpdate(
    const std::filesystem::path& source_path,
    const std::filesystem::path& update_path,
    const std::function<bool(size_t done, size_t total)>& progress) {
  std::error_code ec;
  if (std::filesystem::is_directory(source_path, ec)) {
    return CopyUpdateFolder(source_path, update_path, progress);
  }
  if (!std::filesystem::is_regular_file(source_path, ec)) {
    return SystemFilesResult::kReadFailed;
  }
  return UnzipUpdate(source_path, update_path, progress);
}

}  // namespace ui
}  // namespace xe
