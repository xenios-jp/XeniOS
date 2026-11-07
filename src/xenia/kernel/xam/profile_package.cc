/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/profile_package.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <system_error>

#include <zstd.h>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

constexpr char kMagic[8] = {'X', 'E', 'N', 'I', 'O', 'S', 'P', 'F'};
constexpr size_t kHeaderSize = 16;  // Magic, version, manifest size.

// Caps, so a hostile file can't fill the disk or the memory. A saved game is at
// most a few hundred MB; a profile with every game's saves a few GB.
constexpr uint32_t kMaxManifestBytes = 32u * 1024 * 1024;
constexpr size_t kMaxFiles = 200000;
constexpr uint64_t kMaxFileBytes = 4ull * 1024 * 1024 * 1024;
constexpr uint64_t kMaxTotalBytes = 64ull * 1024 * 1024 * 1024;
constexpr size_t kMaxPathBytes = 1024;
constexpr size_t kMaxNameBytes = 255;
constexpr size_t kMaxDepth = 32;
// An Account file is 0x194 bytes.
constexpr uint64_t kMaxAccountBytes = 64 * 1024;

constexpr size_t kChunkBytes = 1024 * 1024;

constexpr std::string_view kAccountName = "Account";
constexpr std::string_view kSavesFolder = "00000001";
constexpr std::string_view kHeadersFolder = "Headers";

// Seconds between 1601 (FILETIME) and 1970 (Unix), in FILETIME's 100 ns.
constexpr uint64_t kFiletimeUnixEpoch = 116444736000000000ULL;

std::string Hex16(uint64_t value) { return fmt::format("{:016X}", value); }
std::string Hex8(uint32_t value) { return fmt::format("{:08X}", value); }

const std::string& DashboardHex() {
  static const std::string hex = Hex8(kDashboardID);
  return hex;
}
const std::string& ProfileTypeHex() {
  static const std::string hex =
      Hex8(static_cast<uint32_t>(XContentType::kProfile));
  return hex;
}

bool IsUpperHex(std::string_view text, size_t digits) {
  if (text.size() != digits) {
    return false;
  }
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
  });
}

std::optional<uint64_t> ParseHex(std::string_view text, size_t digits) {
  if (!IsUpperHex(text, digits)) {
    return std::nullopt;
  }
  uint64_t value = 0;
  for (char c : text) {
    value = (value << 4) | uint64_t(c <= '9' ? c - '0' : c - 'A' + 10);
  }
  return value;
}

std::vector<std::string_view> SplitPath(std::string_view path) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (true) {
    const size_t slash = path.find('/', start);
    parts.push_back(path.substr(start, slash == std::string_view::npos
                                           ? std::string_view::npos
                                           : slash - start));
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return parts;
}

bool IsSafeName(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameBytes || name == "." ||
      name == "..") {
    return false;
  }
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    // Separators of any host, drive letters, control characters.
    if (u < 0x20 || c == '\\' || c == ':' || c == '/' || u == 0x7F) {
      return false;
    }
  }
  return true;
}

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  for (size_t i = 0; i < suffix.size(); ++i) {
    char a = text[text.size() - suffix.size() + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') {
      a = char(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = char(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

// The part of a path inside the profile's own folder, or empty when it's a
// save.
std::string_view ProfileFolderPrefix(uint64_t xuid, std::string& storage) {
  storage = DashboardHex() + "/" + ProfileTypeHex() + "/" + Hex16(xuid) + "/";
  return storage;
}

// The same file for another profile: only the profile folder names the XUID.
std::string MapToProfile(const std::string& path, uint64_t from, uint64_t to) {
  if (from == to) {
    return path;
  }
  std::string from_prefix, to_prefix;
  ProfileFolderPrefix(from, from_prefix);
  ProfileFolderPrefix(to, to_prefix);
  if (path.compare(0, from_prefix.size(), from_prefix) == 0) {
    return to_prefix + path.substr(from_prefix.size());
  }
  return path;
}

// What moves as one: a file of the profile on its own, a saved game with its
// header.
// "<TITLE>/00000001/<name>" for both "<TITLE>/00000001/<name>/..." and
// "<TITLE>/Headers/00000001/<name>.header".
std::string UnitOf(const std::string& path) {
  const auto parts = SplitPath(path);
  if (parts.size() >= 3 && parts[1] == kSavesFolder) {
    return std::string(parts[0]) + "/" + std::string(kSavesFolder) + "/" +
           std::string(parts[2]);
  }
  if (parts.size() == 4 && parts[1] == kHeadersFolder &&
      parts[2] == kSavesFolder && EndsWithNoCase(parts[3], ".header") &&
      parts[3].size() > 7) {
    return std::string(parts[0]) + "/" + std::string(kSavesFolder) + "/" +
           std::string(parts[3].substr(0, parts[3].size() - 7));
  }
  return path;
}

// The files and folders on disk that make up |unit|, relative to
// content_root/<XUID>.
std::vector<std::string> UnitMembers(const std::string& unit) {
  const auto parts = SplitPath(unit);
  if (parts.size() == 3 && parts[1] == kSavesFolder) {
    return {unit, std::string(parts[0]) + "/" + std::string(kHeadersFolder) +
                      "/" + std::string(kSavesFolder) + "/" +
                      std::string(parts[2]) + ".header"};
  }
  return {unit};
}

std::filesystem::path Join(const std::filesystem::path& root,
                           std::string_view relative) {
  std::filesystem::path result = root;
  for (const auto part : SplitPath(relative)) {
    result /= xe::to_path(part);
  }
  return result;
}

int64_t UnixFromFiletime(uint64_t filetime) {
  if (filetime < kFiletimeUnixEpoch) {
    return 0;
  }
  return int64_t((filetime - kFiletimeUnixEpoch) / 10000000ULL);
}

uint64_t FiletimeFromUnix(int64_t unix_seconds) {
  if (unix_seconds <= 0) {
    return 0;  // Leaves the time alone.
  }
  return uint64_t(unix_seconds) * 10000000ULL + kFiletimeUnixEpoch;
}

std::optional<int64_t> FileMtime(const std::filesystem::path& path) {
  const auto info = xe::filesystem::GetInfo(path);
  if (!info) {
    return std::nullopt;
  }
  return UnixFromFiletime(info->write_timestamp);
}

// The newest file at |path|: the file itself, or any file under the folder.
std::optional<int64_t> NewestMtime(const std::filesystem::path& path) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    return std::nullopt;
  }
  if (!std::filesystem::is_directory(status)) {
    return FileMtime(path);
  }
  std::optional<int64_t> newest;
  for (auto it = std::filesystem::recursive_directory_iterator(path, ec);
       !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_symlink(ec) || !it->is_regular_file(ec)) {
      continue;
    }
    if (const auto mtime = FileMtime(it->path())) {
      newest = std::max(newest.value_or(*mtime), *mtime);
    }
  }
  // An empty save folder still counts as there.
  return newest ? newest : std::optional<int64_t>(0);
}

void PutU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(uint8_t(value));
  out.push_back(uint8_t(value >> 8));
}
void PutU32(uint8_t* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[i] = uint8_t(value >> (8 * i));
  }
}
void PutU64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(uint8_t(value >> (8 * i)));
  }
}
uint16_t GetU16(const uint8_t* in) { return uint16_t(in[0] | (in[1] << 8)); }
uint32_t GetU32(const uint8_t* in) {
  return uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) |
         (uint32_t(in[3]) << 24);
}
uint64_t GetU64(const uint8_t* in) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | in[i];
  }
  return value;
}

struct FileCloser {
  void operator()(FILE* file) const {
    if (file) {
      fclose(file);
    }
  }
};
using UniqueFile = std::unique_ptr<FILE, FileCloser>;

// Compresses into one zstd frame, written as it goes.
class ZstdWriter {
 public:
  explicit ZstdWriter(FILE* file)
      : file_(file), cctx_(ZSTD_createCCtx()), out_(ZSTD_CStreamOutSize()) {
    if (cctx_) {
      ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, 3);
      ZSTD_CCtx_setParameter(cctx_, ZSTD_c_checksumFlag, 1);
    }
  }
  ~ZstdWriter() { ZSTD_freeCCtx(cctx_); }

  bool Write(const void* data, size_t size) {
    if (!cctx_) {
      return false;
    }
    ZSTD_inBuffer in = {data, size, 0};
    while (in.pos < in.size) {
      ZSTD_outBuffer out = {out_.data(), out_.size(), 0};
      const size_t result =
          ZSTD_compressStream2(cctx_, &out, &in, ZSTD_e_continue);
      if (ZSTD_isError(result) || !Flush(out)) {
        return false;
      }
    }
    return true;
  }

  bool End() {
    if (!cctx_) {
      return false;
    }
    ZSTD_inBuffer in = {nullptr, 0, 0};
    size_t remaining = 0;
    do {
      ZSTD_outBuffer out = {out_.data(), out_.size(), 0};
      remaining = ZSTD_compressStream2(cctx_, &out, &in, ZSTD_e_end);
      if (ZSTD_isError(remaining) || !Flush(out)) {
        return false;
      }
    } while (remaining);
    return true;
  }

 private:
  bool Flush(const ZSTD_outBuffer& out) {
    return !out.pos || fwrite(out_.data(), 1, out.pos, file_) == out.pos;
  }

  FILE* file_;
  ZSTD_CCtx* cctx_;
  std::vector<uint8_t> out_;
};

// Reads exact byte counts out of one zstd frame.
class ZstdReader {
 public:
  explicit ZstdReader(FILE* file)
      : file_(file),
        dctx_(ZSTD_createDCtx()),
        in_buffer_(ZSTD_DStreamInSize()),
        out_buffer_(ZSTD_DStreamOutSize()) {
    in_ = {in_buffer_.data(), 0, 0};
  }
  ~ZstdReader() { ZSTD_freeDCtx(dctx_); }

  // False on damaged data or when the frame ends first.
  bool Read(void* data, size_t size) {
    uint8_t* out = static_cast<uint8_t*>(data);
    while (size) {
      if (out_pos_ == out_end_ && !Fill()) {
        return false;
      }
      const size_t n = std::min(size, out_end_ - out_pos_);
      if (out) {
        std::memcpy(out, out_buffer_.data() + out_pos_, n);
        out += n;
      }
      out_pos_ += n;
      size -= n;
    }
    return true;
  }

  // After the last entry: the frame has to end there, its checksum good.
  bool Finish() {
    if (out_pos_ != out_end_) {
      return false;
    }
    while (!frame_done_) {
      if (!Fill()) {
        return frame_done_;
      }
      return false;  // Data after the end.
    }
    return true;
  }

 private:
  bool Fill() {
    out_pos_ = out_end_ = 0;
    if (!dctx_ || frame_done_) {
      return false;
    }
    while (true) {
      ZSTD_outBuffer out = {out_buffer_.data(), out_buffer_.size(), 0};
      const size_t result = ZSTD_decompressStream(dctx_, &out, &in_);
      if (ZSTD_isError(result)) {
        return false;
      }
      if (result == 0) {
        frame_done_ = true;
      }
      if (out.pos) {
        out_end_ = out.pos;
        return true;
      }
      if (frame_done_) {
        return false;
      }
      if (in_.pos == in_.size) {
        const size_t got =
            fread(in_buffer_.data(), 1, in_buffer_.size(), file_);
        if (!got) {
          return false;
        }
        in_.src = in_buffer_.data();
        in_.size = got;
        in_.pos = 0;
      }
    }
  }

  FILE* file_;
  ZSTD_DCtx* dctx_;
  std::vector<uint8_t> in_buffer_;
  std::vector<uint8_t> out_buffer_;
  ZSTD_inBuffer in_;
  size_t out_pos_ = 0;
  size_t out_end_ = 0;
  bool frame_done_ = false;
};

bool ReadAccount(const std::vector<uint8_t>& data, X_XAMACCOUNTINFO* account) {
  if (data.size() < sizeof(X_XAMACCOUNTINFO) + 0x18) {
    return false;
  }
  return ProfileManager::DecryptAccountFile(data.data(), account) ||
         ProfileManager::DecryptAccountFile(data.data(), account, true);
}

// Reads the next entry's header, which has to be exactly |expected|: the
// manifest's paths and sizes are what was checked (paths allowed, disk space).
bool ReadEntryHeader(ZstdReader& reader, const ProfilePackageFile& expected) {
  uint8_t head[2];
  if (!reader.Read(head, sizeof(head)) ||
      GetU16(head) != expected.path.size()) {
    return false;
  }
  std::string path(expected.path.size(), '\0');
  uint8_t numbers[16];
  return reader.Read(path.data(), path.size()) &&
         reader.Read(numbers, sizeof(numbers)) && path == expected.path &&
         GetU64(numbers) == expected.size &&
         int64_t(GetU64(numbers + 8)) == expected.mtime;
}

// Reads the first entry, the Account, into |data|: it has to decrypt and be
// the profile the manifest names (gamertag and online account).
ProfilePackageError ReadPackageAccount(ZstdReader& reader,
                                       const ProfilePackageManifest& manifest,
                                       std::vector<uint8_t>* data) {
  const ProfilePackageFile& expected = manifest.files.front();
  data->resize(size_t(expected.size));
  X_XAMACCOUNTINFO account = {};
  if (!ReadEntryHeader(reader, expected) ||
      !reader.Read(data->data(), data->size()) ||
      !ReadAccount(*data, &account)) {
    return ProfilePackageError::kBadFile;
  }
  if (account.GetGamertagString() != manifest.gamertag ||
      account.GetOnlineXUID() != manifest.online_xuid ||
      account.IsLiveEnabled() != manifest.live_enabled) {
    return ProfilePackageError::kBadFile;
  }
  return ProfilePackageError::kNone;
}

// ---- The manifest
// ------------------------------------------------------------------

std::string WriteManifest(const ProfilePackageManifest& m) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  auto string = [&w](std::string_view text) {
    w.String(text.data(), rapidjson::SizeType(text.size()));
  };
  w.StartObject();
  w.Key("format");
  w.Uint(m.format_version);
  w.Key("gamertag");
  string(m.gamertag);
  w.Key("xuid");
  string(Hex16(m.xuid));
  if (m.online_xuid) {
    w.Key("online_xuid");
    string(Hex16(m.online_xuid));
  }
  w.Key("live");
  w.Bool(m.live_enabled);
  w.Key("app");
  string(m.app);
  w.Key("platform");
  string(m.platform);
  w.Key("created");
  w.Int64(m.created);
  w.Key("contents");
  w.StartObject();
  w.Key("saves");
  w.Bool(m.saves);
  w.Key("achievements");
  w.Bool(m.achievements);
  w.Key("settings");
  w.Bool(m.settings);
  w.EndObject();
  w.Key("sizes");
  w.StartObject();
  w.Key("saves");
  w.Uint64(m.sizes.saves);
  w.Key("achievements");
  w.Uint64(m.sizes.achievements);
  w.Key("settings");
  w.Uint64(m.sizes.settings);
  w.EndObject();
  w.Key("total_bytes");
  w.Uint64(m.total_bytes);
  w.Key("titles");
  w.StartArray();
  for (const auto& title : m.titles) {
    w.StartObject();
    w.Key("id");
    string(Hex8(title.title_id));
    w.Key("name");
    string(title.name);
    w.Key("bytes");
    w.Uint64(title.bytes);
    w.Key("saves");
    w.Uint(title.saves);
    w.EndObject();
  }
  w.EndArray();
  w.Key("files");
  w.StartArray();
  for (const auto& file : m.files) {
    w.StartObject();
    w.Key("path");
    string(file.path);
    w.Key("size");
    w.Uint64(file.size);
    w.Key("mtime");
    w.Int64(file.mtime);
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

const rapidjson::Value* Member(const rapidjson::Value& object,
                               const char* key) {
  if (!object.IsObject()) {
    return nullptr;
  }
  const auto it = object.FindMember(key);
  return it == object.MemberEnd() ? nullptr : &it->value;
}

std::string StringOf(const rapidjson::Value& object, const char* key) {
  const auto* value = Member(object, key);
  return value && value->IsString()
             ? std::string(value->GetString(), value->GetStringLength())
             : std::string();
}

bool BoolOf(const rapidjson::Value& object, const char* key) {
  const auto* value = Member(object, key);
  return value && value->IsBool() && value->GetBool();
}

uint64_t Uint64Of(const rapidjson::Value& object, const char* key) {
  const auto* value = Member(object, key);
  return value && value->IsUint64() ? value->GetUint64() : 0;
}

ProfilePackageError ParseManifest(std::string_view text,
                                  ProfilePackageManifest* out) {
  rapidjson::Document doc;
  doc.Parse(text.data(), text.size());
  if (doc.HasParseError() || !doc.IsObject()) {
    return ProfilePackageError::kBadFile;
  }
  ProfilePackageManifest m;
  const auto* format = Member(doc, "format");
  if (!format || !format->IsUint()) {
    return ProfilePackageError::kBadFile;
  }
  m.format_version = format->GetUint();
  if (m.format_version > kProfilePackageVersion) {
    return ProfilePackageError::kNewerFormat;
  }
  m.gamertag = StringOf(doc, "gamertag");
  const auto xuid = ParseHex(StringOf(doc, "xuid"), 16);
  if (!xuid || !*xuid) {
    return ProfilePackageError::kBadFile;
  }
  m.xuid = *xuid;
  const std::string online = StringOf(doc, "online_xuid");
  if (!online.empty()) {
    const auto online_xuid = ParseHex(online, 16);
    if (!online_xuid) {
      return ProfilePackageError::kBadFile;
    }
    m.online_xuid = *online_xuid;
  }
  m.live_enabled = BoolOf(doc, "live");
  m.app = StringOf(doc, "app");
  m.platform = StringOf(doc, "platform");
  if (const auto* created = Member(doc, "created");
      created && created->IsInt64()) {
    m.created = created->GetInt64();
  }
  if (const auto* contents = Member(doc, "contents")) {
    m.saves = BoolOf(*contents, "saves");
    m.achievements = BoolOf(*contents, "achievements");
    m.settings = BoolOf(*contents, "settings");
  }
  if (const auto* sizes = Member(doc, "sizes")) {
    m.sizes.saves = Uint64Of(*sizes, "saves");
    m.sizes.achievements = Uint64Of(*sizes, "achievements");
    m.sizes.settings = Uint64Of(*sizes, "settings");
  }
  m.total_bytes = Uint64Of(doc, "total_bytes");
  if (const auto* titles = Member(doc, "titles"); titles && titles->IsArray()) {
    for (const auto& entry : titles->GetArray()) {
      const auto id = ParseHex(StringOf(entry, "id"), 8);
      if (!id) {
        return ProfilePackageError::kBadFile;
      }
      ProfilePackageTitle title;
      title.title_id = uint32_t(*id);
      title.name = StringOf(entry, "name");
      title.bytes = Uint64Of(entry, "bytes");
      title.saves =
          uint32_t(std::min<uint64_t>(Uint64Of(entry, "saves"), UINT32_MAX));
      m.titles.push_back(std::move(title));
    }
  }
  const auto* files = Member(doc, "files");
  if (!files || !files->IsArray() || files->Size() == 0) {
    return ProfilePackageError::kBadFile;
  }
  if (files->Size() > kMaxFiles) {
    return ProfilePackageError::kTooLarge;
  }
  uint64_t total = 0;
  std::set<std::string> seen;
  for (const auto& entry : files->GetArray()) {
    const auto* size = Member(entry, "size");
    const auto* mtime = Member(entry, "mtime");
    if (!size || !size->IsUint64() || !mtime || !mtime->IsInt64()) {
      return ProfilePackageError::kBadFile;
    }
    ProfilePackageFile file;
    file.path = StringOf(entry, "path");
    file.size = size->GetUint64();
    file.mtime = mtime->GetInt64();
    if (!IsProfilePackagePathAllowed(file.path, m.xuid) ||
        !seen.insert(file.path).second) {
      return ProfilePackageError::kBadFile;
    }
    if (file.size > kMaxFileBytes) {
      return ProfilePackageError::kTooLarge;
    }
    total += file.size;
    if (total > kMaxTotalBytes) {
      return ProfilePackageError::kTooLarge;
    }
    m.files.push_back(std::move(file));
  }
  // The Account comes first, so it's checked before anything is written.
  std::string profile_prefix;
  ProfileFolderPrefix(m.xuid, profile_prefix);
  if (m.files.front().path != profile_prefix + std::string(kAccountName) ||
      m.files.front().size > kMaxAccountBytes) {
    return ProfilePackageError::kBadFile;
  }
  // The total is what the disk check uses, so it has to be the real one.
  if (m.total_bytes != total) {
    return ProfilePackageError::kBadFile;
  }
  *out = std::move(m);
  return ProfilePackageError::kNone;
}

// Opens a package and reads up to the end of the manifest.
ProfilePackageError OpenPackage(const std::filesystem::path& path,
                                UniqueFile* out_file,
                                ProfilePackageManifest* out_manifest) {
  UniqueFile file(xe::filesystem::OpenFile(path, "rb"));
  if (!file) {
    return ProfilePackageError::kOpenFailed;
  }
  uint8_t header[kHeaderSize];
  if (fread(header, 1, kHeaderSize, file.get()) != kHeaderSize ||
      std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
    return ProfilePackageError::kBadFile;
  }
  const uint32_t version = GetU32(header + 8);
  if (version > kProfilePackageVersion) {
    return ProfilePackageError::kNewerFormat;
  }
  if (version == 0) {
    return ProfilePackageError::kBadFile;
  }
  const uint32_t manifest_size = GetU32(header + 12);
  if (!manifest_size || manifest_size > kMaxManifestBytes) {
    return ProfilePackageError::kBadFile;
  }
  std::string manifest(manifest_size, '\0');
  if (fread(manifest.data(), 1, manifest_size, file.get()) != manifest_size) {
    return ProfilePackageError::kBadFile;
  }
  ProfilePackageError error = ParseManifest(manifest, out_manifest);
  if (error != ProfilePackageError::kNone) {
    return error;
  }
  // The manifest is only what the package says about itself: the Account in
  // it has to be the profile it names, or the package is turned away before
  // anyone is asked to import it.
  const long files_start = ftell(file.get());
  if (files_start < 0) {
    return ProfilePackageError::kBadFile;
  }
  {
    ZstdReader reader(file.get());
    std::vector<uint8_t> account;
    error = ReadPackageAccount(reader, *out_manifest, &account);
    if (error != ProfilePackageError::kNone) {
      return error;
    }
  }
  if (fseek(file.get(), files_start, SEEK_SET) != 0) {
    return ProfilePackageError::kBadFile;
  }
  if (out_file) {
    *out_file = std::move(file);
  }
  return ProfilePackageError::kNone;
}

// ---- Collecting a profile's files
// ------------------------------------------------------

enum class FileKind { kAccount, kProfileSettings, kTitleGpd, kSave };

struct Collected {
  ProfilePackageFile file;
  std::filesystem::path host_path;
  FileKind kind;
};

std::string RelativeText(const std::filesystem::path& path,
                         const std::filesystem::path& root) {
  std::string text = xe::path_to_utf8(path.lexically_relative(root));
#if XE_PLATFORM_WIN32
  std::replace(text.begin(), text.end(), '\\', '/');
#endif
  return text;
}

// Regular files under |folder| (symbolic links skipped), sorted by path.
void CollectFolder(const std::filesystem::path& folder,
                   const std::filesystem::path& profile_root, uint64_t xuid,
                   FileKind kind, std::vector<Collected>* out) {
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) {
    return;
  }
  std::vector<Collected> found;
  for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
       !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_symlink(ec) || !it->is_regular_file(ec)) {
      continue;
    }
    const auto info = xe::filesystem::GetInfo(it->path());
    if (!info) {
      continue;
    }
    Collected entry;
    entry.file.path = RelativeText(it->path(), profile_root);
    entry.file.size = info->total_size;
    entry.file.mtime = UnixFromFiletime(info->write_timestamp);
    entry.host_path = it->path();
    entry.kind = kind;
    if (!IsProfilePackagePathAllowed(entry.file.path, xuid)) {
      XELOGW("Profile export: skipping a file with a name it can't carry");
      continue;
    }
    found.push_back(std::move(entry));
  }
  std::sort(found.begin(), found.end(),
            [](const Collected& a, const Collected& b) {
              return a.file.path < b.file.path;
            });
  for (auto& entry : found) {
    out->push_back(std::move(entry));
  }
}

struct ProfileFiles {
  std::vector<Collected> files;  // Account first.
  std::map<uint32_t, ProfilePackageTitle> titles;
};

void CollectProfile(const std::filesystem::path& content_root, uint64_t xuid,
                    bool saves, bool gpds, ProfileFiles* out) {
  const std::filesystem::path profile_root = content_root / Hex16(xuid);
  const std::filesystem::path profile_folder =
      ProfilePackageProfileFolder(content_root, xuid);

  std::vector<Collected> profile;
  CollectFolder(profile_folder, profile_root, xuid, FileKind::kProfileSettings,
                &profile);
  for (auto& entry : profile) {
    const std::string name = xe::path_to_utf8(entry.host_path.filename());
    const bool top_level = entry.host_path.parent_path() == profile_folder;
    if (top_level && name == kAccountName) {
      entry.kind = FileKind::kAccount;
    } else if (EndsWithNoCase(name, ".gpd")) {
      if (!gpds) {
        continue;
      }
      entry.kind = name == DashboardHex() + ".gpd" ? FileKind::kProfileSettings
                                                   : FileKind::kTitleGpd;
    }
    if (entry.kind == FileKind::kAccount) {
      out->files.insert(out->files.begin(), std::move(entry));
    } else {
      out->files.push_back(std::move(entry));
    }
  }

  if (!saves) {
    return;
  }
  std::error_code ec;
  std::vector<std::pair<uint32_t, std::filesystem::path>> titles;
  for (auto it = std::filesystem::directory_iterator(profile_root, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (it->is_symlink(ec) || !it->is_directory(ec)) {
      continue;
    }
    const auto id = ParseHex(xe::path_to_utf8(it->path().filename()), 8);
    if (id) {
      titles.emplace_back(uint32_t(*id), it->path());
    }
  }
  std::sort(titles.begin(), titles.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [title_id, title_path] : titles) {
    const size_t before = out->files.size();
    const std::filesystem::path saves_folder =
        title_path / std::string(kSavesFolder);
    CollectFolder(saves_folder, profile_root, xuid, FileKind::kSave,
                  &out->files);
    CollectFolder(
        title_path / std::string(kHeadersFolder) / std::string(kSavesFolder),
        profile_root, xuid, FileKind::kSave, &out->files);
    if (out->files.size() == before) {
      continue;
    }
    ProfilePackageTitle& title = out->titles[title_id];
    title.title_id = title_id;
    for (size_t i = before; i < out->files.size(); ++i) {
      title.bytes += out->files[i].file.size;
    }
    for (auto it = std::filesystem::directory_iterator(saves_folder, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (!it->is_symlink(ec)) {
        ++title.saves;
      }
    }
  }
}

// What the dashboard GPD calls each game.
void NameTitles(const std::filesystem::path& profile_folder,
                std::map<uint32_t, ProfilePackageTitle>* titles) {
  if (titles->empty()) {
    return;
  }
  std::vector<uint8_t> data =
      xe::filesystem::ReadAllBytes(profile_folder / (DashboardHex() + ".gpd"));
  if (data.empty()) {
    return;
  }
  const GpdInfoProfile gpd(data);
  for (auto& [title_id, title] : *titles) {
    title.name = xe::to_utf8_lossy(gpd.GetTitleName(title_id));
  }
}

bool WithinFolder(const std::filesystem::path& path,
                  const std::filesystem::path& folder) {
  std::error_code ec;
  const auto canonical_path = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    return false;
  }
  const auto canonical_folder = std::filesystem::weakly_canonical(folder, ec);
  if (ec) {
    return false;
  }
  auto a = canonical_path.begin();
  for (auto b = canonical_folder.begin(); b != canonical_folder.end(); ++b) {
    // weakly_canonical leaves a trailing empty part for a path ending in '/'.
    if (b->empty()) {
      continue;
    }
    if (a == canonical_path.end() || *a != *b) {
      return false;
    }
    ++a;
  }
  return true;
}

// Moves |from| to |to|, copying when they're on different volumes.
bool MoveAside(const std::filesystem::path& from,
               const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::create_directories(to.parent_path(), ec);
  if (ec) {
    return false;
  }
  std::filesystem::rename(from, to, ec);
  if (!ec) {
    return true;
  }
  ec.clear();
  std::filesystem::copy(from, to,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::copy_symlinks,
                        ec);
  if (ec) {
    return false;
  }
  std::filesystem::remove_all(from, ec);
  return !ec;
}

// <prefix>-<date and time> in |root|, numbered when one from the same second
// is there.
std::filesystem::path NewDatedFolder(const std::filesystem::path& root,
                                     std::string_view prefix) {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
#if XE_PLATFORM_WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char text[64];
  std::strftime(text, sizeof(text), "%Y-%m-%d-%H%M%S", &local);
  const std::string name = fmt::format("{}-{}", prefix, text);
  std::filesystem::path folder = root / name;
  std::error_code ec;
  for (int n = 2; std::filesystem::exists(folder, ec) && n < 1000; ++n) {
    folder = root / fmt::format("{}-{}", name, n);
  }
  return folder;
}

// The header, the manifest and the files' zstd frame.
ProfilePackageError WritePackage(FILE* out, const std::string& manifest_text,
                                 const std::vector<Collected>& files,
                                 uint64_t total_bytes,
                                 const ProfilePackageProgress& progress) {
  uint8_t header[kHeaderSize];
  std::memcpy(header, kMagic, sizeof(kMagic));
  PutU32(header + 8, kProfilePackageVersion);
  PutU32(header + 12, uint32_t(manifest_text.size()));
  if (fwrite(header, 1, kHeaderSize, out) != kHeaderSize ||
      fwrite(manifest_text.data(), 1, manifest_text.size(), out) !=
          manifest_text.size()) {
    return ProfilePackageError::kWriteFailed;
  }

  ZstdWriter writer(out);
  std::vector<uint8_t> chunk(kChunkBytes);
  uint64_t done = 0;
  if (progress) {
    progress(0, total_bytes);
  }
  for (const auto& entry : files) {
    std::vector<uint8_t> head;
    PutU16(head, uint16_t(entry.file.path.size()));
    head.insert(head.end(), entry.file.path.begin(), entry.file.path.end());
    PutU64(head, entry.file.size);
    PutU64(head, uint64_t(entry.file.mtime));
    if (!writer.Write(head.data(), head.size())) {
      return ProfilePackageError::kWriteFailed;
    }
    UniqueFile in(xe::filesystem::OpenFile(entry.host_path, "rb"));
    if (!in) {
      return ProfilePackageError::kReadFailed;
    }
    uint64_t left = entry.file.size;
    while (left) {
      const size_t want = size_t(std::min<uint64_t>(left, chunk.size()));
      // A file that got shorter since it was listed.
      if (fread(chunk.data(), 1, want, in.get()) != want) {
        return ProfilePackageError::kReadFailed;
      }
      if (!writer.Write(chunk.data(), want)) {
        return ProfilePackageError::kWriteFailed;
      }
      left -= want;
      done += want;
      if (progress) {
        progress(done, total_bytes);
      }
    }
  }
  std::vector<uint8_t> end;
  PutU16(end, 0);
  if (!writer.Write(end.data(), end.size()) || !writer.End() ||
      fflush(out) != 0) {
    return ProfilePackageError::kWriteFailed;
  }
  return ProfilePackageError::kNone;
}

}  // namespace

const char* ProfilePackageErrorName(ProfilePackageError error) {
  switch (error) {
    case ProfilePackageError::kNone:
      return "none";
    case ProfilePackageError::kOpenFailed:
      return "open_failed";
    case ProfilePackageError::kBadFile:
      return "bad_file";
    case ProfilePackageError::kNewerFormat:
      return "newer_format";
    case ProfilePackageError::kNoProfile:
      return "no_profile";
    case ProfilePackageError::kProfileInUse:
      return "profile_in_use";
    case ProfilePackageError::kLowDiskSpace:
      return "low_disk_space";
    case ProfilePackageError::kTooLarge:
      return "too_large";
    case ProfilePackageError::kReadFailed:
      return "read_failed";
    case ProfilePackageError::kWriteFailed:
      return "write_failed";
  }
  return "unknown";
}

bool IsProfilePackagePathAllowed(std::string_view path, uint64_t xuid) {
  if (path.empty() || path.size() > kMaxPathBytes) {
    return false;
  }
  const auto parts = SplitPath(path);
  if (parts.size() > kMaxDepth) {
    return false;
  }
  for (const auto part : parts) {
    if (!IsSafeName(part)) {
      return false;
    }
  }
  // The profile: FFFE07D1/00010000/<XUID>/<at least one more>.
  if (parts.size() >= 4 && parts[0] == DashboardHex() &&
      parts[1] == ProfileTypeHex()) {
    return parts[2] == Hex16(xuid);
  }
  if (parts.size() < 3 || !IsUpperHex(parts[0], 8)) {
    return false;
  }
  // Saved games: <TITLE>/00000001/<at least one more>.
  if (parts[1] == kSavesFolder) {
    return true;
  }
  // Their headers: <TITLE>/Headers/00000001/<at least one more>.
  return parts.size() >= 4 && parts[1] == kHeadersFolder &&
         parts[2] == kSavesFolder;
}

std::filesystem::path ProfilePackageProfileFolder(
    const std::filesystem::path& content_root, uint64_t xuid) {
  return content_root / Hex16(xuid) / DashboardHex() / ProfileTypeHex() /
         Hex16(xuid);
}

bool ProfilePackageProfileExists(const std::filesystem::path& content_root,
                                 uint64_t xuid) {
  std::error_code ec;
  return std::filesystem::is_regular_file(
      ProfilePackageProfileFolder(content_root, xuid) /
          std::string(kAccountName),
      ec);
}

ProfileDataSizes MeasureProfileData(const std::filesystem::path& content_root,
                                    uint64_t xuid) {
  ProfileFiles files;
  CollectProfile(content_root, xuid, true, true, &files);
  ProfileDataSizes sizes;
  for (const auto& entry : files.files) {
    switch (entry.kind) {
      case FileKind::kSave:
        sizes.saves += entry.file.size;
        break;
      case FileKind::kTitleGpd:
        sizes.achievements += entry.file.size;
        break;
      case FileKind::kAccount:
      case FileKind::kProfileSettings:
        sizes.settings += entry.file.size;
        break;
    }
  }
  return sizes;
}

std::string ProfilePackageFileName(std::string_view gamertag) {
  std::string name;
  for (char c : gamertag) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || std::strchr("/\\:*?\"<>|", c)) {
      name += '_';
    } else {
      name += c;
    }
  }
  // No hidden files, and Windows drops a trailing dot or space.
  while (!name.empty() && (name.front() == '.' || name.front() == ' ')) {
    name.erase(name.begin());
  }
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
    name.pop_back();
  }
  if (name.empty()) {
    name = "Profile";
  }
  return name + std::string(kProfilePackageExtension);
}

ProfilePackageError ExportProfilePackage(
    const std::filesystem::path& content_root, uint64_t xuid,
    const ProfileExportOptions& options, const std::filesystem::path& out_path,
    const ProfilePackageProgress& progress,
    ProfilePackageManifest* out_manifest) {
  if (!ProfilePackageProfileExists(content_root, xuid)) {
    return ProfilePackageError::kNoProfile;
  }
  const std::filesystem::path profile_folder =
      ProfilePackageProfileFolder(content_root, xuid);
  X_XAMACCOUNTINFO account = {};
  if (!ReadAccount(xe::filesystem::ReadAllBytes(profile_folder /
                                                std::string(kAccountName)),
                   &account)) {
    return ProfilePackageError::kReadFailed;
  }

  ProfileFiles files;
  CollectProfile(content_root, xuid, options.saves,
                 options.achievements || options.settings, &files);
  if (files.files.empty() || files.files.front().kind != FileKind::kAccount) {
    return ProfilePackageError::kNoProfile;
  }
  if (files.files.size() > kMaxFiles) {
    return ProfilePackageError::kTooLarge;
  }
  NameTitles(profile_folder, &files.titles);

  ProfilePackageManifest manifest;
  manifest.gamertag = account.GetGamertagString();
  manifest.xuid = xuid;
  manifest.online_xuid = account.GetOnlineXUID();
  manifest.live_enabled = account.IsLiveEnabled();
  manifest.app = options.app;
  manifest.platform = options.platform;
  manifest.created = int64_t(std::time(nullptr));
  manifest.saves = options.saves;
  manifest.achievements = options.achievements;
  manifest.settings = options.settings;
  for (const auto& entry : files.files) {
    if (entry.file.size > kMaxFileBytes) {
      return ProfilePackageError::kTooLarge;
    }
    manifest.total_bytes += entry.file.size;
    switch (entry.kind) {
      case FileKind::kSave:
        manifest.sizes.saves += entry.file.size;
        break;
      case FileKind::kTitleGpd:
        manifest.sizes.achievements += entry.file.size;
        break;
      case FileKind::kAccount:
      case FileKind::kProfileSettings:
        manifest.sizes.settings += entry.file.size;
        break;
    }
    manifest.files.push_back(entry.file);
  }
  if (manifest.total_bytes > kMaxTotalBytes) {
    return ProfilePackageError::kTooLarge;
  }
  for (const auto& [title_id, title] : files.titles) {
    manifest.titles.push_back(title);
  }
  const std::string manifest_text = WriteManifest(manifest);
  if (manifest_text.size() > kMaxManifestBytes) {
    return ProfilePackageError::kTooLarge;
  }

  // Written beside the target and renamed in, so a failure leaves no half a
  // file.
  std::filesystem::path part_path = out_path;
  part_path += ".part";
  ProfilePackageError error = ProfilePackageError::kNone;
  {
    UniqueFile out(xe::filesystem::OpenFile(part_path, "wb"));
    if (!out) {
      return ProfilePackageError::kOpenFailed;
    }
    error = WritePackage(out.get(), manifest_text, files.files,
                         manifest.total_bytes, progress);
  }
  // Closed first: Windows can't remove an open file.
  auto fail = [&part_path](ProfilePackageError why) {
    std::error_code ec;
    std::filesystem::remove(part_path, ec);
    return why;
  };
  if (error != ProfilePackageError::kNone) {
    return fail(error);
  }
  std::error_code ec;
  std::filesystem::rename(part_path, out_path, ec);
  if (ec) {
    return fail(ProfilePackageError::kWriteFailed);
  }
  if (out_manifest) {
    *out_manifest = std::move(manifest);
  }
  return ProfilePackageError::kNone;
}

ProfilePackageError InspectProfilePackage(
    const std::filesystem::path& path, ProfilePackageManifest* out_manifest) {
  ProfilePackageManifest manifest;
  const ProfilePackageError error = OpenPackage(path, nullptr, &manifest);
  if (error == ProfilePackageError::kNone && out_manifest) {
    *out_manifest = std::move(manifest);
  }
  return error;
}

uint64_t FindProfileForPackage(const std::filesystem::path& content_root,
                               const ProfilePackageManifest& manifest) {
  if (ProfilePackageProfileExists(content_root, manifest.xuid)) {
    return manifest.xuid;
  }
  if (!manifest.online_xuid) {
    return 0;
  }
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(content_root, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const auto xuid = ParseHex(xe::path_to_utf8(it->path().filename()), 16);
    if (!xuid || !*xuid || !ProfilePackageProfileExists(content_root, *xuid)) {
      continue;
    }
    X_XAMACCOUNTINFO account = {};
    if (ReadAccount(xe::filesystem::ReadAllBytes(
                        ProfilePackageProfileFolder(content_root, *xuid) /
                        std::string(kAccountName)),
                    &account) &&
        account.GetOnlineXUID() == manifest.online_xuid) {
      return *xuid;
    }
  }
  return 0;
}

ProfilePackageError ImportProfilePackage(
    const std::filesystem::path& content_root,
    const std::filesystem::path& package_path,
    const ProfileImportOptions& options, const ProfilePackageProgress& progress,
    ProfileImportResult* out_result) {
  ProfileImportResult result;
  UniqueFile file;
  const ProfilePackageError error =
      OpenPackage(package_path, &file, &result.manifest);
  if (error != ProfilePackageError::kNone) {
    return error;
  }
  const ProfilePackageManifest& manifest = result.manifest;

  const uint64_t target =
      options.target_xuid ? options.target_xuid : manifest.xuid;
  result.xuid = target;
  // Another profile only when it's here already (one for the same online
  // account).
  if (target != manifest.xuid &&
      !ProfilePackageProfileExists(content_root, target)) {
    return ProfilePackageError::kNoProfile;
  }
  if (options.profile_in_use && options.profile_in_use(target)) {
    return ProfilePackageError::kProfileInUse;
  }
  result.created = !ProfilePackageProfileExists(content_root, target);

  const std::filesystem::path profile_root = content_root / Hex16(target);
  std::vector<std::string> destinations;
  destinations.reserve(manifest.files.size());
  for (const auto& entry : manifest.files) {
    destinations.push_back(MapToProfile(entry.path, manifest.xuid, target));
  }

  // Which side wins, per file of the profile and per saved game.
  struct Unit {
    int64_t imported_newest = 0;
    bool write = false;
  };
  std::map<std::string, Unit> units;
  for (size_t i = 0; i < manifest.files.size(); ++i) {
    Unit& unit = units[UnitOf(destinations[i])];
    unit.imported_newest =
        std::max(unit.imported_newest, manifest.files[i].mtime);
  }
  for (auto& [key, unit] : units) {
    std::optional<int64_t> local_newest;
    for (const auto& member : UnitMembers(key)) {
      if (const auto mtime = NewestMtime(Join(profile_root, member))) {
        local_newest = std::max(local_newest.value_or(*mtime), *mtime);
      }
    }
    if (!local_newest) {
      unit.write = true;
    } else if (options.mode == ProfileImportMode::kReplace) {
      unit.write = true;
    } else {
      unit.write = unit.imported_newest > *local_newest;
    }
    if (local_newest && !unit.write) {
      ++result.kept;
    }
  }

  uint64_t needed = 0;
  for (size_t i = 0; i < manifest.files.size(); ++i) {
    if (units[UnitOf(destinations[i])].write) {
      needed += manifest.files[i].size;
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(content_root, ec);
  const auto space = std::filesystem::space(content_root, ec);
  if (!ec && space.available < needed + options.reserve_bytes) {
    return ProfilePackageError::kLowDiskSpace;
  }

  // The files this import writes are read into a staging folder first. Only
  // once the whole package has read back right (its checksum is at the end)
  // does anything of this device's change, and then only by moving files.
  const std::filesystem::path staging =
      NewDatedFolder(content_root.parent_path(), "profile-import-staging");
  struct StagingCleanup {
    const std::filesystem::path& folder;
    ~StagingCleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(folder, ignored);
    }
  } staging_cleanup{staging};
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    return ProfilePackageError::kWriteFailed;
  }

  ZstdReader reader(file.get());
  std::vector<uint8_t> chunk(kChunkBytes);
  uint64_t done = 0;
  if (progress) {
    progress(0, manifest.total_bytes);
  }
  const auto advance = [&](uint64_t n) {
    done += n;
    if (progress) {
      progress(done, manifest.total_bytes);
    }
  };
  for (size_t i = 0; i < manifest.files.size(); ++i) {
    const ProfilePackageFile& expected = manifest.files[i];
    const bool write = units[UnitOf(destinations[i])].write;
    const std::filesystem::path staged = Join(staging, destinations[i]);
    if (write) {
      std::filesystem::create_directories(staged.parent_path(), ec);
      if (ec) {
        return ProfilePackageError::kWriteFailed;
      }
    }
    if (i == 0) {
      // The Account: has to be the profile the manifest names, whether or not
      // it's written.
      std::vector<uint8_t> account;
      const ProfilePackageError account_error =
          ReadPackageAccount(reader, manifest, &account);
      if (account_error != ProfilePackageError::kNone) {
        return account_error;
      }
      if (write && !xe::filesystem::WriteAllBytes(staged, account)) {
        return ProfilePackageError::kWriteFailed;
      }
      advance(expected.size);
    } else {
      if (!ReadEntryHeader(reader, expected)) {
        return ProfilePackageError::kBadFile;
      }
      UniqueFile out;
      if (write) {
        out.reset(xe::filesystem::OpenFile(staged, "wb"));
        if (!out) {
          return ProfilePackageError::kWriteFailed;
        }
      }
      // When this device's copy is newer, the package's is read past.
      for (uint64_t left = expected.size; left;) {
        const size_t n = size_t(std::min<uint64_t>(left, chunk.size()));
        if (!reader.Read(out ? chunk.data() : nullptr, n)) {
          return ProfilePackageError::kBadFile;
        }
        if (out && fwrite(chunk.data(), 1, n, out.get()) != n) {
          return ProfilePackageError::kWriteFailed;
        }
        left -= n;
        advance(n);
      }
      if (out && fflush(out.get()) != 0) {
        return ProfilePackageError::kWriteFailed;
      }
    }
    if (write) {
      if (auto handle = xe::filesystem::FileHandle::OpenExisting(
              staged, xe::filesystem::FileAccess::kFileWriteData |
                          xe::filesystem::FileAccess::kFileWriteAttributes)) {
        handle->SetTimes(0, 0, FiletimeFromUnix(expected.mtime));
      }
    }
  }
  uint8_t end[2];
  if (!reader.Read(end, sizeof(end)) || GetU16(end) != 0 || !reader.Finish()) {
    return ProfilePackageError::kBadFile;
  }

  // A game may have started on the profile while the package was read.
  if (options.profile_in_use && options.profile_in_use(target)) {
    return ProfilePackageError::kProfileInUse;
  }

  // Each unit to write: this device's copy goes to the backup whole, then the
  // package's takes its place, so a saved game is never half one and half the
  // other. A move that fails puts back everything moved so far.
  const std::filesystem::path backup_root =
      options.backup_root.empty() ? content_root.parent_path() / "backups"
                                  : options.backup_root;
  std::filesystem::path backup_folder;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>>
      moved_aside;  // This device's file or folder, and where it went.
  std::vector<std::filesystem::path> placed;
  const auto roll_back = [&]() {
    std::error_code ignored;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
      std::filesystem::remove(*it, ignored);
    }
    for (auto it = moved_aside.rbegin(); it != moved_aside.rend(); ++it) {
      MoveAside(it->second, it->first);
    }
    if (!backup_folder.empty()) {
      std::filesystem::remove_all(backup_folder, ignored);
    }
    return ProfilePackageError::kWriteFailed;
  };
  for (const auto& [key, unit] : units) {
    if (!unit.write) {
      continue;
    }
    bool replaced = false;
    for (const auto& member : UnitMembers(key)) {
      const std::filesystem::path local = Join(profile_root, member);
      if (!std::filesystem::exists(
              std::filesystem::symlink_status(local, ec))) {
        continue;
      }
      if (backup_folder.empty()) {
        backup_folder = NewDatedFolder(backup_root, "profile-import");
      }
      const std::filesystem::path aside =
          Join(backup_folder / Hex16(target), member);
      if (!MoveAside(local, aside)) {
        return roll_back();
      }
      moved_aside.emplace_back(local, aside);
      replaced = true;
    }
    if (replaced) {
      ++result.replaced;
    }
    for (size_t i = 0; i < destinations.size(); ++i) {
      if (UnitOf(destinations[i]) != key) {
        continue;
      }
      const std::filesystem::path destination =
          Join(profile_root, destinations[i]);
      std::filesystem::create_directories(destination.parent_path(), ec);
      // A folder on the way that's a link to somewhere else.
      if (ec || !WithinFolder(destination.parent_path(), profile_root) ||
          !MoveAside(Join(staging, destinations[i]), destination)) {
        return roll_back();
      }
      placed.push_back(destination);
      ++result.written;
    }
  }
  result.backup_folder = backup_folder;
  XELOGI(
      "Profile import: {:016X}{} from {}, {} files written, {} kept, {} "
      "replaced",
      target, result.created ? " (new)" : "", manifest.app, result.written,
      result.kept, result.replaced);
  if (out_result) {
    *out_result = std::move(result);
  }
  return ProfilePackageError::kNone;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
