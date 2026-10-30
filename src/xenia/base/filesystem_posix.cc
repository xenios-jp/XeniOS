/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstring>
#if XE_PLATFORM_APPLE
#include <limits.h>
#include <mach-o/dyld.h>
#endif

namespace xe {

Utf8Path path_to_utf8(const std::filesystem::path& path) {
  return Utf8Path(path.string());
}

std::u16string path_to_utf16(const std::filesystem::path& path) {
  return xe::to_utf16(path.string());
}

std::filesystem::path to_path(const std::string_view source) { return source; }

std::filesystem::path to_path(const std::u16string_view source) {
  return xe::to_utf8(source);
}

namespace filesystem {

std::filesystem::path GetExecutablePath() {
#if XE_PLATFORM_APPLE
  char path[PATH_MAX];
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    char real_path[PATH_MAX];
    if (realpath(path, real_path)) {
      return std::string(real_path);
    }
    return std::string(path);
  }
  return std::string();
#else
  char buff[FILENAME_MAX] = "";
  ssize_t len = readlink("/proc/self/exe", buff, sizeof(buff) - 1);
  if (len != -1) {
    buff[len] = '\0';
    return std::string(buff);
  }
  return std::string();
#endif
}

std::filesystem::path GetExecutableFolder() {
  // When running from an AppImage use the AppImage directory instead of
  // the temporary mount point
  if (const char* appimage_path = std::getenv("APPIMAGE")) {
    return std::filesystem::path(appimage_path).parent_path();
  }
  return GetExecutablePath().parent_path();
}

std::filesystem::path GetUserFolder() {
  // get preferred data home
  char* home = std::getenv("XDG_DATA_HOME");
  if (home) {
    return std::string(home);
  }

  // if XDG_DATA_HOME not set, fallback to HOME directory
  home = std::getenv("HOME");

  // if HOME not set, fall back to this
  if (home == NULL) {
    struct passwd pw1;
    struct passwd* pw = nullptr;
    char buf[4096];  // could potentionally lower this
    // getpwuid_r returns 0 with a null result for "no entry".
    if (getpwuid_r(getuid(), &pw1, buf, sizeof(buf), &pw) != 0 || !pw) {
      XELOGW(
          "GetUserFolder: no HOME and no passwd entry; using the current "
          "directory");
      return std::filesystem::current_path() / ".local" / "share";
    }
    home = pw->pw_dir;
  }

  return std::filesystem::path(home) / ".local" / "share";
}

FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  return fopen(path.c_str(), std::string(mode).c_str());
}

bool Seek(FILE* file, int64_t offset, int origin) {
  return fseeko(file, offset, origin) == 0;
}

int64_t Tell(FILE* file) { return int64_t(ftello(file)); }

bool TruncateStdioFile(FILE* file, uint64_t length) {
  if (fflush(file)) {
    return false;
  }
  int64_t position = Tell(file);
  if (position < 0) {
    return false;
  }
  if (ftruncate(fileno(file), length)) {
    return false;
  }
  if (uint64_t(position) > length) {
    if (!Seek(file, 0, SEEK_END)) {
      return false;
    }
  }
  return true;
}

static uint64_t convertUnixtimeToWinFiletime(time_t unixtime) {
  // Unix uses seconds since 1/1/1970, Windows uses 100ns intervals since
  // 1/1/1601. Convert and add the epoch difference.
  // See https://msdn.microsoft.com/en-us/library/ms724228
  uint64_t filetime = (uint64_t(unixtime) * 10000000) + 116444736000000000ULL;
  return filetime;
}

// A 0 FILETIME leaves the time alone.
static timespec convertWinFiletimeToTimespec(uint64_t filetime) {
  if (!filetime) {
    return {0, UTIME_OMIT};
  }
  int64_t ticks = int64_t(filetime) - 116444736000000000LL;
  int64_t seconds = ticks / 10000000;
  int64_t remainder = ticks % 10000000;
  // tv_nsec can't be negative, so a time before 1970 borrows a second.
  if (remainder < 0) {
    seconds -= 1;
    remainder += 10000000;
  }
  return {time_t(seconds), long(remainder * 100)};
}

bool CreateEmptyFile(const std::filesystem::path& path) {
  int file = creat(path.c_str(), 0774);
  if (file >= 0) {
    close(file);
    return true;
  }
  return false;
}

class PosixFileHandle : public FileHandle {
 public:
  PosixFileHandle(std::filesystem::path path, int handle, bool append_only)
      : FileHandle(std::move(path)),
        handle_(handle),
        append_only_(append_only) {}
  ~PosixFileHandle() override {
    close(handle_);
    handle_ = -1;
  }
  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    ssize_t out = pread(handle_, buffer, buffer_length, file_offset);
    if (out >= 0) {
      *out_bytes_read = out;
      return true;
    } else {
      *out_bytes_read = 0;
      return false;
    }
  }
  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    ssize_t out = append_only_
                      ? write(handle_, buffer, buffer_length)
                      : pwrite(handle_, buffer, buffer_length, file_offset);
    if (out >= 0) {
      *out_bytes_written = out;
      return true;
    } else {
      *out_bytes_written = 0;
      return false;
    }
  }
  bool SetLength(size_t length) override {
    return ftruncate(handle_, length) >= 0 ? true : false;
  }
  bool SetTimes(uint64_t create_timestamp, uint64_t access_timestamp,
                uint64_t write_timestamp) override {
    // There's no creation time to set.
    const timespec times[2] = {convertWinFiletimeToTimespec(access_timestamp),
                               convertWinFiletimeToTimespec(write_timestamp)};
    return futimens(handle_, times) == 0;
  }
  void Flush() override { fsync(handle_); }

 private:
  int handle_ = -1;
  bool append_only_ = false;
};

std::unique_ptr<FileHandle> FileHandle::OpenExisting(
    const std::filesystem::path& path, uint32_t desired_access) {
  // O_RDONLY/O_WRONLY/O_RDWR are a 2-bit access mode, not OR-able flags.
  // Reduce to read/write intent, then pick the mode. kGenericExecute has no
  // POSIX open equivalent and falls back to read.
  const bool wants_read =
      desired_access & (FileAccess::kGenericRead | FileAccess::kFileReadData |
                        FileAccess::kGenericExecute | FileAccess::kGenericAll);
  const bool wants_write =
      desired_access & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                        FileAccess::kFileAppendData | FileAccess::kGenericAll);
  int open_access;
  if (wants_read && wants_write) {
    open_access = O_RDWR;
  } else if (wants_write) {
    open_access = O_WRONLY;
  } else {
    open_access = O_RDONLY;
  }
  // pwrite(2) ignores the offset on an O_APPEND descriptor.
  bool append_only = (desired_access & FileAccess::kFileAppendData) &&
                     !(desired_access &
                       (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                        FileAccess::kGenericAll));
  if (append_only) {
    open_access |= O_APPEND;
  }
  int handle = open(path.c_str(), open_access);
  if (handle == -1 && errno == EISDIR) {
    // Win32 hands out a directory handle for any access through
    // FILE_FLAG_BACKUP_SEMANTICS, and neither platform can write file data
    // through one, so drop the write intent rather than fail.
    append_only = false;
    handle = open(path.c_str(), O_RDONLY | O_DIRECTORY);
  }
  if (handle == -1) {
    // TODO(benvanik): pick correct response.
    return nullptr;
  }
  return std::make_unique<PosixFileHandle>(path, handle, append_only);
}

std::optional<FileInfo> GetInfo(const std::filesystem::path& path) {
  FileInfo info{};
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      info.type = FileInfo::Type::kDirectory;
      // On Linux st.st_size can have non-zero size (generally 4096) so make 0
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = st.st_size;
    }
    info.path = path.parent_path();
    info.name = path.filename();
    info.create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    info.access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    info.write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    return std::move(info);
  }
  return {};
}

namespace internal {

std::vector<FileInfo> ListFilesUnsorted(const std::filesystem::path& path) {
  std::vector<FileInfo> result;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }

  while (auto ent = readdir(dir)) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    FileInfo info;

    info.name = ent->d_name;
    const auto child_path = path / info.name;
    struct stat st;
    if (stat(child_path.c_str(), &st) != 0 &&
        lstat(child_path.c_str(), &st) != 0) {
      std::memset(&st, 0, sizeof(st));
    }
    info.create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    info.access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    info.write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    info.path = path;
    // d_type is unreliable: DT_LNK for a symlinked directory, and DT_UNKNOWN
    // on filesystems that do not populate it. Classify from the stat.
    if (S_ISDIR(st.st_mode)) {
      info.type = FileInfo::Type::kDirectory;
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = st.st_size;
    }
    result.push_back(info);
  }
  closedir(dir);
  return std::move(result);
}

}  // namespace internal

}  // namespace filesystem
}  // namespace xe
