/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_pipeline_archive.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_IOS
#include <os/proc.h>
#endif  // XE_PLATFORM_IOS

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

NS::URL* FileUrl(const std::filesystem::path& path) {
  return NS::URL::fileURLWithPath(
      NS::String::string(path.c_str(), NS::UTF8StringEncoding));
}

// Saving an archive leaves a staging copy of it (up to gigabytes) in the
// archiver's directory in the per-user cache, which nothing deletes. It's in
// the app's own directory there, or (on iOS, where the whole cache is the
// app's) at its top.
std::vector<std::filesystem::path> GpuArchiverStagingDirs() {
  std::vector<std::filesystem::path> dirs;
  char buffer[1024];
  size_t length = confstr(_CS_DARWIN_USER_CACHE_DIR, buffer, sizeof(buffer));
  if (!length || length > sizeof(buffer)) {
    return dirs;
  }
  const std::filesystem::path cache(buffer);
  if (CFStringRef identifier = CFBundleGetIdentifier(CFBundleGetMainBundle())) {
    char name[256];
    if (CFStringGetCString(identifier, name, sizeof(name),
                           kCFStringEncodingUTF8)) {
      dirs.push_back(cache / name / "com.apple.gpuarchiver");
    }
  }
#if XE_PLATFORM_IOS
  dirs.push_back(cache / "com.apple.gpuarchiver");
#endif  // XE_PLATFORM_IOS
  return dirs;
}

std::set<std::filesystem::path> ListStaging() {
  std::set<std::filesystem::path> entries;
  for (const std::filesystem::path& dir : GpuArchiverStagingDirs()) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      entries.insert(entry.path());
    }
  }
  return entries;
}

std::string ErrorText(NS::Error* error) {
  return error && error->localizedDescription()
             ? error->localizedDescription()->utf8String()
             : "unknown";
}

// An archive is only usable on the OS build it was made on.
std::string OsBuild() {
  char build[64] = {};
  size_t size = sizeof(build) - 1;
  if (sysctlbyname("kern.osversion", build, &size, nullptr, 0)) {
    return "unknown";
  }
  return build;
}

// Metal allocates about a part's size to open it and ends the app if it can't.
bool HasRoomToOpen(const std::filesystem::path& path) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return true;
  }
  const size_t needed = size_t(size) * 2 + (size_t(64) << 20);
#if XE_PLATFORM_IOS
  if (os_proc_available_memory() < needed) {
    return false;
  }
#endif  // XE_PLATFORM_IOS
  void* probe = mmap(nullptr, needed, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (probe == MAP_FAILED) {
    return false;
  }
  munmap(probe, needed);
  return true;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::string text;
  if (std::FILE* in = xe::filesystem::OpenFile(path, "rb")) {
    char buffer[4096];
    size_t length;
    while ((length = std::fread(buffer, 1, sizeof(buffer), in)) != 0) {
      text.append(buffer, length);
    }
    std::fclose(in);
  }
  return text;
}

constexpr char kPartExtension[] = ".metalarchive";
// More parts than this (a title whose pipelines keep changing) and they're
// discarded, to start over with fewer.
constexpr size_t kMaxParts = 64;

}  // namespace

MTL::BinaryArchive* MetalPipelineArchive::NewArchive(
    const std::filesystem::path& path, NS::Error** error) {
  MTL::BinaryArchiveDescriptor* desc =
      MTL::BinaryArchiveDescriptor::alloc()->init();
  if (!path.empty()) {
    desc->setUrl(FileUrl(path));
  }
  MTL::BinaryArchive* archive = device_->newBinaryArchive(desc, error);
  desc->release();
  return archive;
}

void MetalPipelineArchive::UpdateArchivesArray() {
  if (archives_) {
    archives_->release();
    archives_ = nullptr;
  }
  if (!parts_.empty()) {
    // Newest first: the parts don't overlap, but a later one is likelier to
    // hold what's being looked up.
    std::vector<const NS::Object*> objects(parts_.rbegin(), parts_.rend());
    archives_ = NS::Array::alloc()->init(objects.data(), objects.size());
  }
}

NS::Array* MetalPipelineArchive::GetArchives() {
  std::lock_guard<std::mutex> lock(mutex_);
  return archives_ ? archives_->retain() : nullptr;
}

void MetalPipelineArchive::Open(MTL::Device* device,
                                const std::filesystem::path& dir,
                                const std::string& fingerprint) {
  Close(false);
  if (!device) {
    return;
  }
  std::lock_guard<std::mutex> save_lock(save_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  device_ = device;
  dir_ = dir;
  cancel_save_ = false;

  const std::string full_fingerprint = fingerprint + " os=" + OsBuild();
  const std::filesystem::path id_path = dir / "archive.id";
  std::vector<std::pair<uint32_t, std::filesystem::path>> files;
  bool discard = ReadFile(id_path) != full_fingerprint;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::filesystem::path& path = entry.path();
    if (path.extension() == kPartExtension) {
      char* end = nullptr;
      const std::string stem = xe::path_to_utf8(path.stem());
      const unsigned long index = std::strtoul(stem.c_str(), &end, 10);
      if (end && !*end && !stem.empty()) {
        files.emplace_back(uint32_t(index), path);
        continue;
      }
    }
    if (path.filename() != "archive.id") {
      // A part a save was writing when the app stopped.
      std::filesystem::remove(path, ec);
    }
  }
  if (files.size() > kMaxParts) {
    XELOGI("Metal pipeline archive: {} parts, starting over", files.size());
    discard = true;
  }
  if (discard) {
    for (const auto& file : files) {
      std::filesystem::remove(file.second, ec);
    }
    files.clear();
    if (std::FILE* out = xe::filesystem::OpenFile(id_path, "wb")) {
      std::fwrite(full_fingerprint.data(), 1, full_fingerprint.size(), out);
      std::fclose(out);
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto& [index, path] : files) {
    next_part_ = std::max(next_part_, index + 1);
    if (!HasRoomToOpen(path)) {
      XELOGW("Metal pipeline archive: not enough memory to open {} of {} parts",
             files.size() - parts_.size(), files.size());
      break;
    }
    NS::Error* error = nullptr;
    MTL::BinaryArchive* part = NewArchive(path, &error);
    if (!part) {
      XELOGW("Metal pipeline archive: dropping an unusable part: {}",
             ErrorText(error));
      std::filesystem::remove(path, ec);
      continue;
    }
    parts_.push_back(part);
  }
  UpdateArchivesArray();

#if XE_PLATFORM_IOS
  // What runs that ended without closing left. On macOS another instance could
  // be using them.
  for (const std::filesystem::path& entry : ListStaging()) {
    std::filesystem::remove_all(entry, ec);
  }
#endif  // XE_PLATFORM_IOS
  staging_before_ = ListStaging();
  added_ = 0;
  hits_ = 0;
  misses_ = 0;
  XELOGI("Metal pipeline archive: {} saved parts loaded", parts_.size());
}

size_t MetalPipelineArchive::WriteParts(
    const std::vector<std::function<bool(MTL::BinaryArchive*)>>& additions,
    size_t begin, size_t end) {
  if (begin >= end) {
    return 0;
  }
  if (cancel_save_.load(std::memory_order_relaxed)) {
    return end - begin;
  }
  NS::Error* error = nullptr;
  MTL::BinaryArchive* archive = NewArchive({}, &error);
  if (!archive) {
    XELOGW("Metal pipeline archive: can't create one: {}", ErrorText(error));
    return end - begin;
  }
  size_t added = 0;
  for (size_t i = begin; i < end; ++i) {
    if (cancel_save_.load(std::memory_order_relaxed)) {
      archive->release();
      return end - begin;
    }
    added += additions[i](archive) ? 1 : 0;
  }
  const std::filesystem::path path =
      dir_ / fmt::format("{:05}{}", next_part_, kPartExtension);
  // Written beside it and renamed: a part is never left half written.
  const std::filesystem::path temp_path =
      dir_ / fmt::format("{:05}.{}.tmp", next_part_, getpid());
  std::error_code ec;
  error = nullptr;
  bool written = added && archive->serializeToURL(FileUrl(temp_path), &error) &&
                 (std::filesystem::rename(temp_path, path, ec), !ec);
  // The collecting archive holds what it compiled in memory; the part reopened
  // from its file is mapped instead.
  archive->release();
  if (written && !HasRoomToOpen(path)) {
    // Saved for the next launch.
    ++next_part_;
    return (end - begin) - added;
  }
  if (written) {
    MTL::BinaryArchive* part = NewArchive(path, &error);
    if (part) {
      ++next_part_;
      std::lock_guard<std::mutex> lock(mutex_);
      parts_.push_back(part);
      UpdateArchivesArray();
      return (end - begin) - added;
    }
    std::filesystem::remove(path, ec);
  } else {
    std::filesystem::remove(temp_path, ec);
  }
  if (!added) {
    return end - begin;
  }
  // Some pipelines make the archive holding them fail to serialize (seen:
  // "expecting 'fragment' stage", "missing 'vertex' stage in pipeline no. N")
  // with nothing in their descriptors to tell them apart.
  if (end - begin == 1) {
    return 1;
  }
  const size_t middle = begin + (end - begin) / 2;
  return WriteParts(additions, begin, middle) +
         WriteParts(additions, middle, end);
}

void MetalPipelineArchive::ReleasePending(
    std::vector<MTL::RenderPipelineDescriptor*>& pipelines,
    std::vector<PendingFunction>& functions) {
  for (MTL::RenderPipelineDescriptor* desc : pipelines) {
    desc->release();
  }
  pipelines.clear();
  for (PendingFunction& function : functions) {
    function.desc->release();
    function.library->release();
  }
  functions.clear();
}

void MetalPipelineArchive::Save() {
  std::lock_guard<std::mutex> save_lock(save_mutex_);
  std::vector<MTL::RenderPipelineDescriptor*> pipelines;
  std::vector<PendingFunction> functions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ ||
        (pending_pipelines_.empty() && pending_functions_.empty())) {
      return;
    }
    pipelines.swap(pending_pipelines_);
    functions.swap(pending_functions_);
    added_.store(0, std::memory_order_relaxed);
  }
  const uint64_t start_ticks = xe::Clock::QueryHostTickCount();
  std::vector<std::function<bool(MTL::BinaryArchive*)>> additions;
  for (MTL::RenderPipelineDescriptor* desc : pipelines) {
    additions.push_back([desc](MTL::BinaryArchive* to) {
      NS::Error* error = nullptr;
      return to->addRenderPipelineFunctions(desc, &error);
    });
  }
  for (const PendingFunction& function : functions) {
    additions.push_back([this, function](MTL::BinaryArchive* to) {
      NS::Error* error = nullptr;
      if (to->addFunction(function.desc, function.library, &error)) {
        return true;
      }
      if (!function_error_logged_) {
        function_error_logged_ = true;
        XELOGW("Metal pipeline archive: can't hold a specialized function: {}",
               ErrorText(error));
      }
      return false;
    });
  }
  const size_t parts_before = parts_.size();
  const size_t left_out = WriteParts(additions, 0, additions.size());
  ReleasePending(pipelines, functions);
  const uint64_t ms = (xe::Clock::QueryHostTickCount() - start_ticks) * 1000 /
                      xe::Clock::QueryHostTickFrequency();
  uintmax_t bytes = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
    if (entry.path().extension() == kPartExtension) {
      bytes += entry.file_size(ec);
    }
  }
  XELOGI(
      "Metal pipeline archive: saved {} of {} in {} part(s) in {} ms ({} "
      "parts, "
      "{} MB){}",
      additions.size() - left_out, additions.size(),
      parts_.size() - parts_before, ms, parts_.size(), uint64_t(bytes >> 20),
      left_out ? fmt::format(", {} left out", left_out) : std::string());
}

void MetalPipelineArchive::Close(bool save) {
  if (save) {
    Save();
  }
  std::lock_guard<std::mutex> save_lock(save_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  ReleasePending(pending_pipelines_, pending_functions_);
  if (device_ && (hits_ || misses_)) {
    XELOGI("Metal pipeline archive: {} loaded, {} compiled", hits_, misses_);
  }
  for (MTL::BinaryArchive* part : parts_) {
    part->release();
  }
  parts_.clear();
  UpdateArchivesArray();
  if (device_) {
    // Saving leaves the archiver's staging copies behind.
    std::error_code ec;
    for (const std::filesystem::path& entry : ListStaging()) {
      if (!staging_before_.count(entry)) {
        std::filesystem::remove_all(entry, ec);
      }
    }
  }
  device_ = nullptr;
  dir_.clear();
  next_part_ = 0;
}

MTL::RenderPipelineState* MetalPipelineArchive::NewRenderPipelineState(
    MTL::Device* device, MTL::RenderPipelineDescriptor* desc,
    NS::Error** error) {
  if (NS::Array* archives = GetArchives()) {
    desc->setBinaryArchives(archives);
    // A hit isn't saved again.
    NS::Error* miss = nullptr;
    MTL::RenderPipelineState* pipeline = device->newRenderPipelineState(
        desc, MTL::PipelineOptionFailOnBinaryArchiveMiss, nullptr, &miss);
    desc->setBinaryArchives(nullptr);
    archives->release();
    if (pipeline) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++hits_;
      return pipeline;
    }
  }
  MTL::RenderPipelineState* pipeline =
      device->newRenderPipelineState(desc, error);
  // An archive holding a pipeline without a fragment stage fails to serialize
  // ("expecting 'fragment' stage").
  if (!pipeline || !desc->fragmentFunction()) {
    return pipeline;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (device_) {
    ++misses_;
    pending_pipelines_.push_back(desc->copy());
    added_.fetch_add(1, std::memory_order_relaxed);
  }
  return pipeline;
}

MTL::Function* MetalPipelineArchive::NewFunction(MTL::Library* library,
                                                 MTL::FunctionDescriptor* desc,
                                                 NS::Error** error) {
  if (NS::Array* archives = GetArchives()) {
    desc->setBinaryArchives(archives);
    desc->setOptions(MTL::FunctionOptionFailOnBinaryArchiveMiss);
    NS::Error* miss = nullptr;
    MTL::Function* function = library->newFunction(desc, &miss);
    desc->setOptions(MTL::FunctionOptionNone);
    desc->setBinaryArchives(nullptr);
    archives->release();
    if (function) {
      return function;
    }
  }
  MTL::Function* function = library->newFunction(desc, error);
  if (!function) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (device_) {
    pending_functions_.push_back(
        PendingFunction{desc->copy(), library->retain()});
    added_.fetch_add(1, std::memory_order_relaxed);
  }
  return function;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
