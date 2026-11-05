/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader_library_cache.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#include <cstring>
#include <system_error>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/xxhash.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

constexpr char kMagic[8] = {'X', 'E', 'M', 'T', 'L', 'L', 'B', '1'};
// Records larger than this are taken for a damaged file.
constexpr uint32_t kMaxRecordBytes = 64u << 20;

struct RecordHeader {
  uint64_t key_low;
  uint64_t key_high;
  uint32_t entry_point_length;
  uint32_t metallib_length;
};
static_assert(sizeof(RecordHeader) == 24);

// The main executable's LC_UUID: every link makes a new one.
std::string ExecutableUuid() {
  const mach_header* header = _dyld_get_image_header(0);
  if (!header || header->magic != MH_MAGIC_64) {
    return "none";
  }
  const auto* command = reinterpret_cast<const load_command*>(
      reinterpret_cast<const uint8_t*>(header) + sizeof(mach_header_64));
  for (uint32_t i = 0; i < header->ncmds; ++i) {
    if (command->cmd == LC_UUID) {
      const auto* uuid = reinterpret_cast<const uuid_command*>(command);
      std::string text;
      for (uint8_t byte : uuid->uuid) {
        text += fmt::format("{:02X}", byte);
      }
      return text;
    }
    command = reinterpret_cast<const load_command*>(
        reinterpret_cast<const uint8_t*>(command) + command->cmdsize);
  }
  return "none";
}

}  // namespace

std::string MetalShaderLibraryCache::MakeFingerprint(
    const std::string& translator) {
  std::string fingerprint = "exe=" + ExecutableUuid() + " " + translator;
  // Every GPU and Metal option, so none that changes the translation is missed.
  // Changing an unrelated one only costs one translation of the stored shaders.
  if (cvar::ConfigVars) {
    for (const auto& [name, config_var] : *cvar::ConfigVars) {
      const std::string& category = config_var->category();
      if (category == "GPU" || category == "Metal") {
        fingerprint += " " + name + "=" + config_var->config_value();
      }
    }
  }
  return fingerprint;
}

MetalShaderLibraryCache::Key MetalShaderLibraryCache::MakeKey(
    const Shader::Translation& translation) {
  const Shader& shader = translation.shader();
  XXH3_state_t state;
  XXH3_128bits_reset(&state);
  const uint64_t ucode_hash = shader.ucode_data_hash();
  const uint64_t ucode_dwords = shader.ucode_dword_count();
  const uint32_t type = uint32_t(shader.type());
  const uint64_t modification = translation.modification();
  XXH3_128bits_update(&state, &ucode_hash, sizeof(ucode_hash));
  XXH3_128bits_update(&state, &ucode_dwords, sizeof(ucode_dwords));
  XXH3_128bits_update(&state, &type, sizeof(type));
  XXH3_128bits_update(&state, &modification, sizeof(modification));
  if (const Shader::Specialization* specialization =
          translation.specialization()) {
    for (const auto& format : specialization->memexport_formats) {
      XXH3_128bits_update(&state, &format.instruction_address,
                          sizeof(format.instruction_address));
      XXH3_128bits_update(&state, &format.format_word,
                          sizeof(format.format_word));
    }
  }
  const XXH128_hash_t hash = XXH3_128bits_digest(&state);
  return Key{hash.low64, hash.high64};
}

void MetalShaderLibraryCache::Open(const std::filesystem::path& path,
                                   const std::string& fingerprint) {
  Close();
  std::lock_guard<std::mutex> lock(mutex_);

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  // Read what an earlier run of the same build and settings left.
  std::vector<uint8_t> data;
  if (std::FILE* in = xe::filesystem::OpenFile(path, "rb")) {
    std::fseek(in, 0, SEEK_END);
    const long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    if (size > 0) {
      data.resize(size_t(size));
      if (std::fread(data.data(), 1, data.size(), in) != data.size()) {
        data.clear();
      }
    }
    std::fclose(in);
  }
  const size_t header_size =
      sizeof(kMagic) + sizeof(uint32_t) + fingerprint.size();
  bool matches = data.size() >= header_size &&
                 !std::memcmp(data.data(), kMagic, sizeof(kMagic));
  if (matches) {
    uint32_t fingerprint_length;
    std::memcpy(&fingerprint_length, data.data() + sizeof(kMagic),
                sizeof(fingerprint_length));
    matches = fingerprint_length == fingerprint.size() &&
              !std::memcmp(data.data() + sizeof(kMagic) + sizeof(uint32_t),
                           fingerprint.data(), fingerprint.size());
  }

  size_t valid_end = header_size;
  if (matches) {
    size_t offset = header_size;
    while (offset + sizeof(RecordHeader) <= data.size()) {
      RecordHeader record;
      std::memcpy(&record, data.data() + offset, sizeof(record));
      if (record.entry_point_length > 256 ||
          record.metallib_length > kMaxRecordBytes) {
        break;
      }
      const size_t entry_point_offset = offset + sizeof(record);
      const size_t metallib_offset =
          entry_point_offset + record.entry_point_length;
      const size_t end = metallib_offset + record.metallib_length;
      if (end > data.size()) {
        break;
      }
      locations_[Key{record.key_low, record.key_high}] =
          Location{entry_point_offset, record.entry_point_length,
                   metallib_offset, record.metallib_length};
      offset = end;
      valid_end = end;
    }
  }

  if (matches) {
    // Drop a record cut short, then add to the file.
    if (valid_end != data.size()) {
      std::filesystem::resize_file(path, valid_end, ec);
      data.resize(valid_end);
    }
    file_ = xe::filesystem::OpenFile(path, "ab");
  } else {
    locations_.clear();
    data.clear();
    file_ = xe::filesystem::OpenFile(path, "wb");
    if (file_) {
      const uint32_t fingerprint_length = uint32_t(fingerprint.size());
      std::fwrite(kMagic, 1, sizeof(kMagic), file_);
      std::fwrite(&fingerprint_length, sizeof(fingerprint_length), 1, file_);
      std::fwrite(fingerprint.data(), 1, fingerprint.size(), file_);
      std::fflush(file_);
    }
  }
  if (!file_) {
    XELOGW("Metal shader library cache: can't open {}",
           xe::strip_pii(xe::path_to_utf8(path)));
    locations_.clear();
    return;
  }
  data_ = std::move(data);
  XELOGI("Metal shader library cache: {} translated shaders on disk{}",
         locations_.size(), matches ? "" : " (new file for this build)");
}

void MetalShaderLibraryCache::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_) {
    if (hits_ || misses_) {
      XELOGI("Metal shader library cache: {} loaded, {} translated", hits_,
             misses_);
    }
    std::fclose(file_);
    file_ = nullptr;
  }
  data_.clear();
  data_.shrink_to_fit();
  locations_.clear();
  stored_.clear();
  hits_ = 0;
  misses_ = 0;
}

bool MetalShaderLibraryCache::Find(const Key& key, Entry& entry_out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_) {
    return false;
  }
  auto it = locations_.find(key);
  if (it == locations_.end()) {
    ++misses_;
    return false;
  }
  const Location& location = it->second;
  entry_out.entry_point_name.assign(
      reinterpret_cast<const char*>(data_.data() + location.entry_point_offset),
      location.entry_point_length);
  entry_out.metallib.assign(
      data_.data() + location.metallib_offset,
      data_.data() + location.metallib_offset + location.metallib_length);
  ++hits_;
  return true;
}

void MetalShaderLibraryCache::Store(const Key& key,
                                    const std::vector<uint8_t>& metallib,
                                    const std::string& entry_point_name) {
  if (metallib.empty() || metallib.size() > kMaxRecordBytes ||
      entry_point_name.size() > 256) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_ || locations_.count(key) || !stored_.emplace(key, true).second) {
    return;
  }
  RecordHeader record{key.low, key.high, uint32_t(entry_point_name.size()),
                      uint32_t(metallib.size())};
  // One write per record, so a stop between records leaves whole ones.
  std::vector<uint8_t> bytes(sizeof(record) + entry_point_name.size() +
                             metallib.size());
  std::memcpy(bytes.data(), &record, sizeof(record));
  std::memcpy(bytes.data() + sizeof(record), entry_point_name.data(),
              entry_point_name.size());
  std::memcpy(bytes.data() + sizeof(record) + entry_point_name.size(),
              metallib.data(), metallib.size());
  std::fwrite(bytes.data(), 1, bytes.size(), file_);
  std::fflush(file_);
}

uint32_t MetalShaderLibraryCache::hits() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hits_;
}

uint32_t MetalShaderLibraryCache::misses() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return misses_;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
