/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Delivers resolve output into guest RAM when the CPU accesses it, shared by
// the D3D12 and Vulkan command processors. A resolve leaves its output in the
// shared memory buffer, where the GPU reads it, and arms a read watch over its
// destination. The first CPU access of a watched page copies the output of the
// resolve that last wrote it into guest RAM, from the faulting thread, before
// the access proceeds.
//
// Like command_processor_memexport.inc, this is #included into each backend's
// class body so each gets its own inlinable copy with no inheritance or virtual
// dispatch. The include site must already provide SharedMemory, the base
// memory_ (Memory*), texture_cache_, xenia/memory.h (VirtualHeap,
// IsWritableProtect), xenia/base/assert.h, xenia/base/logging.h,
// xenia/gpu/draw_util.h, xenia/gpu/registers.h, and <algorithm> <atomic>
// <cstring> <memory> <mutex> <unordered_map> <unordered_set> <vector>. No
// include guard: injected into a class body.
//
// It must also define, before including this:
//   bool EndResolveSubmission()
//     Submits what has been recorded, without waiting for it.
// And call OnResolveSubmissionEnded on the GPU thread whenever a submission is
// handed to the GPU, and SubmitRequestedResolve once IssueCopy is done.
//   uint64_t GetResolveSubmittedThrough() const
//     The last submission handed to the GPU, from any thread.
//   bool FaultCopyResolveToGuestRam(
//       const std::vector<std::pair<uint32_t, uint32_t>>& ranges)
//     Copies ranges of the shared memory buffer into guest RAM from the
//     faulting thread and waits for it. Without the guest RAM host buffer the
//     copy lands in a CPU-readable buffer, for CopyPackedRangesToGuestRam. With
//     zero-copy the resolve wrote guest RAM itself, so it only waits. Called
//     under resolve_read_mutex_, so it may neither take the global lock nor
//     need the GPU thread.
//   bool EnsureMemexportRangeInDeviceBuffer(uint32_t base, uint32_t size)
//     Copies memexport output in the host buffer into the shared memory buffer.
// And include command_processor_readback_staging.inc.

// How much of a guest range is committed and writable, so copying GPU output
// back into it is safe. Writing past that corrupts memory the guest never
// committed.
uint32_t WritableGuestRangeLength(uint32_t address, uint32_t length) const {
  VirtualHeap* physical_heap = memory_->GetPhysicalHeap();
  if (!physical_heap || !length) {
    return 0;
  }
  HeapAllocationInfo alloc_info;
  // The region is counted in whole pages from the one the address is in, and
  // only as far as the range, not to the end of a region that may be a large
  // pool, as this runs for every resolve.
  uint32_t page_offset = address & (physical_heap->page_size() - 1);
  if (!physical_heap->QueryRegionInfo(address, &alloc_info,
                                      length + page_offset) ||
      !(alloc_info.state & kMemoryAllocationCommit) ||
      !IsWritableProtect(alloc_info.protect)) {
    return 0;
  }
  uint64_t region_end =
      uint64_t(address - page_offset) + alloc_info.region_size;
  if (uint64_t(address) >= region_end) {
    return 0;
  }
  return uint32_t(std::min(uint64_t(length), region_end - uint64_t(address)));
}

// Whether a resolve's whole destination is writable - it is copied back whole
// or not at all. An empty range still has to name writable memory.
bool IsResolveDestinationResident(uint32_t address, uint32_t length) const {
  return length ? WritableGuestRangeLength(address, length) == length
                : WritableGuestRangeLength(address, 1) != 0;
}

// From the read callback on the faulting thread, with or without the global
// lock: records which resolves' output the CPU read, for the next resolve of
// the same destinations, and provides it.
void MarkResolvePagesRead(uint32_t physical_address, uint32_t length) {
  if (!resolve_page_owners_ || !length) {
    return;
  }
  const uint32_t page_count = SharedMemory::kBufferSize >> 12;
  uint32_t first_page = physical_address >> 12;
  if (first_page >= page_count) {
    return;
  }
  uint32_t last_page =
      std::min((physical_address + length - 1) >> 12, page_count - 1);
  uint64_t last_owner = 0;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    uint64_t owner = resolve_page_owners_[page].load(std::memory_order_relaxed);
    if (owner && owner != last_owner) {
      std::lock_guard<std::mutex> lock(resolve_read_mutex_);
      resolve_read_keys_.insert(owner);
      last_owner = owner;
    }
  }
  ProvideResolveOutput(physical_address, length, false);
}

// Before a write drops the read watch: the write has to land over what the GPU
// wrote there, not under it.
void PrepareResolvePagesForWrite(uint32_t address, uint32_t length) {
  ProvideResolveOutput(address, length, true);
}

// Before recording a draw that writes guest RAM directly, so a later fault copy
// of older resolve output can't land over it. May wait for the GPU.
void ProvideResolveOutputForGpuWrite(uint32_t address, uint32_t length) {
  if (!resolve_page_owners_ || !length ||
      address >= SharedMemory::kBufferSize) {
    return;
  }
  length = std::min(length, SharedMemory::kBufferSize - address);
  // Only submitted resolves can be copied.
  if (resolve_unsubmitted_owner_ &&
      HasUnsubmittedResolveOutput(address, length)) {
    EndResolveSubmission();
  }
  ProvideResolveOutput(address, length, false);
}

// Whether an unsubmitted resolve owns uncopied output in these pages.
bool HasUnsubmittedResolveOutput(uint32_t address, uint32_t length) {
  const uint32_t page_count = SharedMemory::kBufferSize >> 12;
  uint32_t first_page = address >> 12;
  uint32_t last_page = std::min((address + length - 1) >> 12, page_count - 1);
  uint64_t submitted = GetResolveSubmittedThrough();
  std::lock_guard<std::mutex> lock(resolve_read_mutex_);
  uint64_t last_owner_key = 0;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    uint64_t owner_key =
        resolve_page_owners_[page].load(std::memory_order_relaxed);
    if (!owner_key || owner_key == last_owner_key) {
      continue;
    }
    last_owner_key = owner_key;
    auto owner_it = resolve_owners_.find(owner_key);
    if (owner_it != resolve_owners_.end() && owner_it->second.copyable &&
        !owner_it->second.in_guest_ram &&
        owner_it->second.submission > submitted) {
      return true;
    }
  }
  return false;
}

// When the host is about to make these pages' contents irrelevant, or already
// has: nothing a resolve wrote there needs copying, and later accesses are of
// something else.
void DiscardResolvePages(uint32_t address, uint32_t length) {
  // Waits out a copy a fault is making without the global lock, which would
  // otherwise land over what the pages hold next.
  std::lock_guard<std::mutex> lock(resolve_read_mutex_);
  SetResolvePageOwner(address, length, 0);
}

// Copies into guest RAM the output of the resolves owning these pages that
// nothing has copied yet, so the access sees what the GPU wrote. Only the pages
// each resolve still owns, as the CPU may have written pages a later resolve
// took over since. The resolve has to be submitted already, as a fault can't
// make the GPU thread submit without deadlocking, holding the global lock or
// resolve_read_mutex_, so a read racing its submission gets the previous
// contents, as it would on the console, and the watch this access used up is
// armed again once it is. A write the copy can't precede lands in guest RAM, so
// the later copy of an edge page it hit takes only the resolve's own bytes.
void ProvideResolveOutput(uint32_t address, uint32_t length, bool write) {
  if (!resolve_page_owners_ || !length) {
    return;
  }
  const uint32_t page_count = SharedMemory::kBufferSize >> 12;
  uint32_t first_page = address >> 12;
  if (first_page >= page_count) {
    return;
  }
  uint32_t last_page = std::min((address + length - 1) >> 12, page_count - 1);
  std::lock_guard<std::mutex> lock(resolve_read_mutex_);
  uint64_t submitted = GetResolveSubmittedThrough();
  uint64_t last_owner_key = 0;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    uint64_t owner_key =
        resolve_page_owners_[page].load(std::memory_order_relaxed);
    if (!owner_key || owner_key == last_owner_key) {
      continue;
    }
    last_owner_key = owner_key;
    auto owner_it = resolve_owners_.find(owner_key);
    if (owner_it == resolve_owners_.end()) {
      continue;
    }
    ResolveOwner& owner = owner_it->second;
    if (owner.in_guest_ram || !owner.copyable) {
      continue;
    }
    uint32_t owner_end = owner.address + owner.length;
    uint32_t owner_first_page = owner.address >> 12;
    uint32_t owner_last_page = std::min((owner_end - 1) >> 12, page_count - 1);
    if (owner.submission > submitted) {
      if (write) {
        ClipResolveEdgePagesWritten(owner, owner_first_page, owner_last_page,
                                    first_page, last_page);
      }
      RearmResolveAfterSubmission(owner);
      continue;
    }
    resolve_fault_copy_ranges_.clear();
    for (uint32_t owner_page = owner_first_page; owner_page <= owner_last_page;
         ++owner_page) {
      if (resolve_page_owners_[owner_page].load(std::memory_order_relaxed) !=
          owner_key) {
        continue;
      }
      uint32_t page_address = owner_page << 12;
      if (!resolve_fault_copy_ranges_.empty() &&
          resolve_fault_copy_ranges_.back().first +
                  resolve_fault_copy_ranges_.back().second ==
              page_address) {
        resolve_fault_copy_ranges_.back().second += 4096;
      } else {
        resolve_fault_copy_ranges_.emplace_back(page_address, 4096);
      }
    }
    // The first and last pages may be partly someone else's, but the shared
    // memory buffer holds the latest GPU data for all of them, unless the CPU
    // wrote them before this could copy it.
    if (!resolve_fault_copy_ranges_.empty()) {
      uint32_t first_page_base = owner_first_page << 12;
      uint32_t last_page_end = (owner_last_page + 1) << 12;
      auto& first_range = resolve_fault_copy_ranges_.front();
      if (owner.clip_first_page && first_range.first == first_page_base) {
        first_range.second -= owner.address - first_page_base;
        first_range.first = owner.address;
      }
      auto& last_range = resolve_fault_copy_ranges_.back();
      if (owner.clip_last_page &&
          last_range.first + last_range.second == last_page_end) {
        last_range.second -= last_page_end - owner_end;
      }
    }
    if (FaultCopyResolveToGuestRam(resolve_fault_copy_ranges_)) {
      owner.in_guest_ram = true;
    } else {
      if (write) {
        ClipResolveEdgePagesWritten(owner, owner_first_page, owner_last_page,
                                    first_page, last_page);
      }
      RearmResolveAfterSubmission(owner);
    }
  }
}

// Copies ranges packed back to back in source into guest RAM, through the
// physical view, which the read watch does not protect.
void CopyPackedRangesToGuestRam(
    const void* source,
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  const uint8_t* source_bytes = static_cast<const uint8_t*>(source);
  for (const auto& range : ranges) {
    std::memcpy(memory_->TranslatePhysical(range.first), source_bytes,
                range.second);
    source_bytes += range.second;
  }
}

// Records a resolve of [address, length), already recorded into the current
// submission, for ProvideResolveOutput. is_scaled means its output is not in
// the shared memory buffer, so it can't be copied.
void NoteResolveForReadback(uint32_t address, uint32_t length, bool is_scaled) {
  if (!is_scaled) {
    DropStagedMemexportOutput(address, length);
    // Before this resolve takes the pages over.
    uint32_t first_page_base = address & ~uint32_t(4095);
    uint32_t end = address + length;
    uint32_t last_page_end = (end + 4095) & ~uint32_t(4095);
    bool first_pulled = PullMemexportEdgeBytes(first_page_base, address);
    bool last_pulled = PullMemexportEdgeBytes(end, last_page_end);
    // Pulled pages are current in the shared memory buffer, not the host one.
    if (first_pulled) {
      ClearMemexportPages(first_page_base, 4096);
    }
    if (last_pulled) {
      ClearMemexportPages(last_page_end - 4096, 4096);
    }
  }
  uint64_t resolve_key = MakeReadbackResolveKey(address, length);
  bool read_before = TakeResolveRead(resolve_key);
  // A later read is of this resolve's output, not what it replaces.
  SetResolvePageOwner(address, length, resolve_key);
  {
    std::lock_guard<std::mutex> lock(resolve_read_mutex_);
    if (resolve_owners_.size() >= kMaxResolveOwners) {
      PruneResolveOwners();
    }
    ResolveOwner& owner = resolve_owners_[resolve_key];
    owner.address = address;
    owner.length = length;
    owner.submission = GetCurrentSubmission();
    owner.frame = resolve_frame_index_;
    owner.copyable = !is_scaled;
    owner.in_guest_ram = false;
    owner.rearm_pending = false;
    owner.clip_first_page = false;
    owner.clip_last_page = false;
  }
  // The watch is one-shot, so it is re-armed on every resolve. Only once the
  // owner is published: a fault still on the previous arm would otherwise see
  // the previous output as copied and drop the watch.
  memory_->EnablePhysicalMemoryAccessCallbacks(address, length, false, true);
  if (is_scaled) {
    return;
  }
  resolve_unsubmitted_owner_ = true;
  // A destination the CPU has read recently will likely be read again, and
  // possibly before the next guest sync point submits it, so it is submitted as
  // soon as IssueCopy is done, outside its debug marker scope.
  if (ResolveReadRecently(resolve_key, read_before)) {
    resolve_submit_requested_ = true;
  }
}

// A fault copy takes whole pages from the shared memory buffer, so export
// output in the host buffer sharing a resolve's edge page is copied there
// first, unless an uncopied resolve owns the page and that buffer is current.
// Returns whether it copied.
bool PullMemexportEdgeBytes(uint32_t start, uint32_t end) {
  if (start >= end || !IsMemexportRange(start, end - start)) {
    return false;
  }
  if (resolve_page_owners_) {
    uint64_t owner_key =
        resolve_page_owners_[start >> 12].load(std::memory_order_relaxed);
    if (owner_key) {
      std::lock_guard<std::mutex> lock(resolve_read_mutex_);
      auto owner_it = resolve_owners_.find(owner_key);
      if (owner_it != resolve_owners_.end() && owner_it->second.copyable &&
          !owner_it->second.in_guest_ram) {
        return false;
      }
    }
  }
  return EnsureMemexportRangeInDeviceBuffer(start, end - start);
}

// Under resolve_read_mutex_, with the table full. Output already copied, or
// overwritten by later resolves at both ends, goes first. What is still full of
// output nothing has copied loses its oldest quarter, whose later reads get
// what guest RAM has.
void PruneResolveOwners() {
  for (auto it = resolve_owners_.begin(); it != resolve_owners_.end();) {
    const ResolveOwner& owner = it->second;
    uint32_t last_page = std::min((owner.address + owner.length - 1) >> 12,
                                  (SharedMemory::kBufferSize >> 12) - 1);
    bool owns_first = resolve_page_owners_[owner.address >> 12].load(
                          std::memory_order_relaxed) == it->first;
    bool owns_last = resolve_page_owners_[last_page].load(
                         std::memory_order_relaxed) == it->first;
    if (owner.in_guest_ram || (!owns_first && !owns_last)) {
      it = resolve_owners_.erase(it);
    } else {
      ++it;
    }
  }
  if (resolve_owners_.size() < kMaxResolveOwners) {
    return;
  }
  std::vector<uint32_t> ages;
  ages.reserve(resolve_owners_.size());
  for (const auto& owner : resolve_owners_) {
    ages.push_back(resolve_frame_index_ - owner.second.frame);
  }
  auto oldest = ages.begin() + ages.size() * 3 / 4;
  std::nth_element(ages.begin(), oldest, ages.end());
  uint32_t min_age = *oldest;
  for (auto it = resolve_owners_.begin(); it != resolve_owners_.end();) {
    if (resolve_frame_index_ - it->second.frame >= min_age) {
      it = resolve_owners_.erase(it);
    } else {
      ++it;
    }
  }
}

// On the GPU thread, once IssueCopy is done.
void SubmitRequestedResolve() {
  if (!resolve_submit_requested_) {
    return;
  }
  resolve_submit_requested_ = false;
  EndResolveSubmission();
}

// On the GPU thread when a submission has been handed to the GPU: resolves
// recorded into it can now be copied from a fault, so the watches reads racing
// them used up are armed again.
void OnResolveSubmissionEnded() {
  resolve_unsubmitted_owner_ = false;
  {
    std::lock_guard<std::mutex> lock(resolve_read_mutex_);
    if (resolve_rearm_ranges_.empty()) {
      return;
    }
    resolve_rearm_ranges_.swap(resolve_rearming_ranges_);
    for (auto& owner : resolve_owners_) {
      owner.second.rearm_pending = false;
    }
  }
  // Outside resolve_read_mutex_, as this takes the global lock.
  for (const auto& range : resolve_rearming_ranges_) {
    memory_->EnablePhysicalMemoryAccessCallbacks(range.first, range.second,
                                                 false, true);
  }
  resolve_rearming_ranges_.clear();
}

// Before the guest can learn the GPU got past a resolve, the resolve has to be
// submitted, so ProvideResolveOutput can copy its output from the fault.
void SubmitResolvesForGuestSync() override {
  OnResolveGuestSync();
  if (resolve_unsubmitted_owner_) {
    EndResolveSubmission();
  }
}

// Whether the CPU has read the output of the last resolve of this destination,
// consuming that. Only pages no later resolve has overwritten count, so a read
// of a smaller resolve landing inside it is not a read of it.
bool TakeResolveRead(uint64_t resolve_key) {
  std::lock_guard<std::mutex> lock(resolve_read_mutex_);
  return resolve_read_keys_.erase(resolve_key) != 0;
}

// Whether the CPU has read this destination's output within the last
// kResolveReadRecentFrames. Not only since its last resolve: one resolved twice
// between two reads would have its second output unread by then.
bool ResolveReadRecently(uint64_t resolve_key, bool read_before) {
  if (read_before) {
    if (resolve_read_frames_.size() >= kMaxResolveReadFrames) {
      for (auto it = resolve_read_frames_.begin();
           it != resolve_read_frames_.end();) {
        if (resolve_frame_index_ - it->second > kResolveReadRecentFrames) {
          it = resolve_read_frames_.erase(it);
        } else {
          ++it;
        }
      }
      if (resolve_read_frames_.size() >= kMaxResolveReadFrames) {
        resolve_read_frames_.clear();
      }
    }
    resolve_read_frames_[resolve_key] = resolve_frame_index_;
    return true;
  }
  auto it = resolve_read_frames_.find(resolve_key);
  return it != resolve_read_frames_.end() &&
         resolve_frame_index_ - it->second <= kResolveReadRecentFrames;
}

// Makes a resolve the owner of the pages it writes, so later reads of them are
// attributed to it.
void SetResolvePageOwner(uint32_t base, uint32_t length, uint64_t resolve_key) {
  const uint32_t page_count = SharedMemory::kBufferSize >> 12;
  uint32_t first_page = base >> 12;
  if (!resolve_page_owners_ || !length || first_page >= page_count) {
    return;
  }
  uint32_t last_page = std::min((base + length - 1) >> 12, page_count - 1);
  for (uint32_t page = first_page; page <= last_page; ++page) {
    resolve_page_owners_[page].store(resolve_key, std::memory_order_relaxed);
  }
}

// Ends the resolve frame.
void NoteResolveFrame() { ++resolve_frame_index_; }

// What a scaled resolve's readback needs before it can touch the GPU. False
// means it cannot be read back at all, with the reason already logged.
struct ScaledResolveReadbackInfo {
  uint32_t pixel_size_log2;
  uint32_t tile_count;
  // Whole 32x32 tiles only, a partial tail is truncated rather than copying
  // stale bytes to the guest.
  uint32_t readback_length;
  uint32_t scale_x;
  uint32_t scale_y;
  uint64_t scaled_start;
  uint64_t scaled_readback_length;
};
bool GetScaledResolveReadbackInfo(uint32_t address, uint32_t length,
                                  reg::RB_COPY_DEST_INFO copy_dest_info,
                                  ScaledResolveReadbackInfo& info_out) {
  // Texel size from the normalized copy_dest_info, not a re-read of
  // RB_COPY_DEST_INFO, for depth the register can hold a different size.
  uint32_t pixel_size_log2 =
      draw_util::GetResolveDownscalePixelSizeLog2(copy_dest_info);
  if (pixel_size_log2 > 3) {
    // 128bpp, which the downscale shader's tiled addressing reversal cannot
    // handle.
    XELOGGPU(
        "Skipping readback of a resolution-scaled resolve to a 128bpp "
        "destination - not supported by the downscale shader");
    return false;
  }
  // The scaled addressing is periodic per guest group, so the written extent
  // must be group-aligned for the per-tile reversal to be valid.
  uint32_t group_bytes_log2 = pixel_size_log2 <= 2 ? 7 : 6;
  if (address & ((uint32_t(1) << group_bytes_log2) - 1)) {
    XELOGGPU(
        "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
        "destination is not aligned to the scaled addressing group size",
        address);
    return false;
  }
  uint32_t tile_size_1x = (32u * 32u) << pixel_size_log2;
  uint32_t tile_count = length / tile_size_1x;
  if (!tile_count) {
    return false;
  }
  uint32_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t scale_y = texture_cache_->draw_resolution_scale_y();
  assert_true(scale_x >= 1 &&
              scale_x <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
  assert_true(scale_y >= 1 &&
              scale_y <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
  assert_true(scale_x > 1 || scale_y > 1);
  uint32_t readback_length = tile_count * tile_size_1x;
  uint32_t scale_area = scale_x * scale_y;
  uint64_t scaled_start = uint64_t(address) * scale_area;
  uint64_t scaled_readback_length = uint64_t(readback_length) * scale_area;
  // The source is bound at the written extent, which starts later than the
  // destination base, so the extent has to be inside the current range.
  uint64_t range_start_scaled =
      texture_cache_->GetCurrentScaledResolveRangeStartScaled();
  uint64_t range_length_scaled =
      texture_cache_->GetCurrentScaledResolveRangeLengthScaled();
  if (!range_length_scaled || scaled_start < range_start_scaled ||
      scaled_start + scaled_readback_length >
          range_start_scaled + range_length_scaled) {
    XELOGGPU(
        "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
        "written extent is not within the current scaled resolve range",
        address);
    return false;
  }
  info_out.pixel_size_log2 = pixel_size_log2;
  info_out.tile_count = tile_count;
  info_out.readback_length = readback_length;
  info_out.scale_x = scale_x;
  info_out.scale_y = scale_y;
  info_out.scaled_start = scaled_start;
  info_out.scaled_readback_length = scaled_readback_length;
  return true;
}

// Allocates the page owners. Called from each backend's SetupContext, which
// then registers the per-backend read callback thunk.
void InitResolveReadWatch() {
  resolve_page_owners_ = std::make_unique<std::atomic<uint64_t>[]>(
      SharedMemory::kBufferSize >> 12);
}

// Unregisters the read callback and drops all tracking. Called from each
// backend's ShutdownContext.
// TODO(xenios-jp): a fault callback on another thread may still be inside
// MarkResolvePagesRead when the owner array is freed, unless unregistering
// waits for callbacks in flight (not checked).
void ResetResolveReadWatch() {
  if (resolve_read_callback_) {
    memory_->UnregisterPhysicalMemoryReadCallback(resolve_read_callback_);
    resolve_read_callback_ = nullptr;
  }
  resolve_page_owners_.reset();
  {
    std::lock_guard<std::mutex> lock(resolve_read_mutex_);
    resolve_read_keys_.clear();
    resolve_owners_.clear();
  }
  resolve_unsubmitted_owner_ = false;
  resolve_submit_requested_ = false;
  resolve_rearm_ranges_.clear();
  resolve_read_frames_.clear();
}

// Per guest page, the key of the resolve that last wrote it, or 0. Written on
// the GPU thread and by discards, read on faulting threads, so atomic.
std::unique_ptr<std::atomic<uint64_t>[]> resolve_page_owners_;
// Destinations whose last output the CPU has read, until resolved again. Taken
// by the fault handler both with and without the global lock, and held across
// FaultCopyResolveToGuestRam, so nothing may take the global lock while holding
// it.
std::mutex resolve_read_mutex_;
std::unordered_set<uint64_t> resolve_read_keys_;
// Resolve output by destination key, for ProvideResolveOutput. Under
// resolve_read_mutex_.
struct ResolveOwner {
  uint32_t address = 0;
  uint32_t length = 0;
  // Submission the resolve was recorded in.
  uint64_t submission = 0;
  uint32_t frame = 0;
  bool copyable = false;
  bool in_guest_ram = false;
  // In resolve_rearm_ranges_ already.
  bool rearm_pending = false;
  // The CPU wrote this edge page before the output could be copied, so only
  // the resolve's bytes in it are copied.
  bool clip_first_page = false;
  bool clip_last_page = false;
};
std::unordered_map<uint64_t, ResolveOwner> resolve_owners_;
static constexpr size_t kMaxResolveOwners = 4096;
// Under resolve_read_mutex_.
void RearmResolveAfterSubmission(ResolveOwner& owner) {
  if (!owner.rearm_pending) {
    owner.rearm_pending = true;
    resolve_rearm_ranges_.emplace_back(owner.address, owner.length);
  }
}
// Under resolve_read_mutex_, when a write to pages [first_page, last_page] goes
// ahead of the copy of this resolve's output: an edge page it hits partly holds
// what the CPU writes there, not only what the GPU wrote.
void ClipResolveEdgePagesWritten(ResolveOwner& owner, uint32_t owner_first_page,
                                 uint32_t owner_last_page, uint32_t first_page,
                                 uint32_t last_page) {
  if ((owner.address & 4095) && first_page <= owner_first_page &&
      last_page >= owner_first_page) {
    owner.clip_first_page = true;
  }
  if (((owner.address + owner.length) & 4095) &&
      first_page <= owner_last_page && last_page >= owner_last_page) {
    owner.clip_last_page = true;
  }
}
std::vector<std::pair<uint32_t, uint32_t>> resolve_fault_copy_ranges_;
// Owners whose watch a read used up before they could be copied, to arm again
// when the next submission ends. Under resolve_read_mutex_, and the GPU
// thread's copy of them.
std::vector<std::pair<uint32_t, uint32_t>> resolve_rearm_ranges_;
std::vector<std::pair<uint32_t, uint32_t>> resolve_rearming_ranges_;
// Frame the CPU last read each destination's output, by resolve key. GPU thread
// only.
std::unordered_map<uint64_t, uint32_t> resolve_read_frames_;
static constexpr size_t kMaxResolveReadFrames = 4096;
// About a second, chosen, not measured.
static constexpr uint32_t kResolveReadRecentFrames = 60;
// A resolve not copied into guest RAM is recorded since the last submission,
// and one is to be submitted after IssueCopy. GPU thread only.
bool resolve_unsubmitted_owner_ = false;
bool resolve_submit_requested_ = false;
uint32_t resolve_frame_index_ = 0;
// Handle from RegisterPhysicalMemoryReadCallback, or null when not armed.
void* resolve_read_callback_ = nullptr;
