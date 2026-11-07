/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PIPELINE_ARCHIVE_H_
#define XENIA_GPU_METAL_METAL_PIPELINE_ARCHIVE_H_

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

// A title's compiled render pipelines, in Metal binary archives kept on disk,
// so a later launch loads them instead of compiling them. The system shader
// cache does the same for a while, but iOS empties it when the app is
// installed or updated and after a restart.
//
// Only pipelines are added. A pipeline's entry records the specialized
// functions it uses (as specialized function libraries in the archive's
// pipelines script, see metal-pipelines-script(5)), and its lookup hits with a
// function specialized again at launch. Specialized functions aren't added on
// their own: addFunctionWithDescriptor is for visible and intersection
// functions (MTLBinaryArchive.h), an archive of nothing else fails to
// serialize, and opening a part that holds them takes memory that grows with
// the square of their count (iPad M5: 256 of them, 1.4 GB for a 6 MB part;
// 707 in a Halo 3 part ended the app on an iPhone).
//
// Done as Apple describes for binary archives (WWDC20 "Build GPU binaries with
// Metal"): what's compiled is collected into a new, empty archive, which is
// serialized once and then reopened from its file only to look pipelines up;
// a reopened archive is mapped, so it doesn't count against the app's memory.
// Each save makes one more such part, and lookups search them all. An archive
// is only usable on the GPU and OS build it was made on: made elsewhere, or by
// another build of the app, the parts are discarded.
class MetalPipelineArchive {
 public:
  ~MetalPipelineArchive() { Close(false); }

  // Loads the parts in |dir|, discarding them when they were made with another
  // |fingerprint| (to which the OS build is added) or are unusable.
  void Open(MTL::Device* device, const std::filesystem::path& dir,
            const std::string& fingerprint);
  // Writes what was compiled since the last save as a new part. Slow (each
  // addition is compiled again for the archive): not on the draw thread.
  void Save();
  // Makes a save in progress stop at its next pipeline.
  void CancelSave() { cancel_save_.store(true, std::memory_order_relaxed); }
  void Close(bool save);

  // Thread-safe. Looks the pipeline up in the parts first; on a miss, compiles
  // it and keeps it for the next save. Never waits for a save.
  MTL::RenderPipelineState* NewRenderPipelineState(
      MTL::Device* device, MTL::RenderPipelineDescriptor* desc,
      NS::Error** error);

  uint32_t unsaved_additions() const {
    return added_.load(std::memory_order_relaxed);
  }

 private:
  // The parts, retained, or nullptr when there are none.
  NS::Array* GetArchives();
  // With mutex_ held.
  void UpdateArchivesArray();
  // An empty archive when |path| is empty.
  MTL::BinaryArchive* NewArchive(const std::filesystem::path& path,
                                 NS::Error** error);
  // Opens the part at |path|. Its name is kept in the opening marker while
  // Metal reads it, so a part that ends the app is dropped at the next Open
  // instead of ending every launch.
  MTL::BinaryArchive* OpenPart(const std::filesystem::path& path,
                               NS::Error** error);
  // With save_mutex_ held. Writes |additions| [begin, end) as one part, or,
  // when that fails, halves them until what fails is found and left out.
  // Returns how many were left out.
  size_t WriteParts(
      const std::vector<std::function<bool(MTL::BinaryArchive*)>>& additions,
      size_t begin, size_t end);
  static void ReleasePending(
      std::vector<MTL::RenderPipelineDescriptor*>& pipelines);

  // Held through a save, which can take seconds.
  std::mutex save_mutex_;
  // Held briefly; guards the rest.
  mutable std::mutex mutex_;
  MTL::Device* device_ = nullptr;
  std::filesystem::path dir_;
  std::vector<MTL::BinaryArchive*> parts_;
  NS::Array* archives_ = nullptr;
  uint32_t next_part_ = 0;
  // What the system archiver's staging directories held at Open.
  std::set<std::filesystem::path> staging_before_;
  // Copies of what was compiled since the last save.
  std::vector<MTL::RenderPipelineDescriptor*> pending_pipelines_;
  std::atomic<uint32_t> added_{0};
  std::atomic<bool> cancel_save_{false};
  uint32_t hits_ = 0;
  uint32_t misses_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_PIPELINE_ARCHIVE_H_
