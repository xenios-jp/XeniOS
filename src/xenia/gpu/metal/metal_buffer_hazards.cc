/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_buffer_hazards.h"

#include <algorithm>

#include "xenia/base/assert.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {
// Every stage that may access the buffer, including the object and mesh
// stages of the emulated tessellation.
constexpr MTL::RenderStages kRenderStages =
    MTL::RenderStageVertex | MTL::RenderStageFragment | MTL::RenderStageObject |
    MTL::RenderStageMesh;
}  // namespace

void MetalBufferHazards::Encoder::BeginCommon(
    MetalBufferHazards& hazards, MTL::CommandBuffer* command_buffer) {
  assert_null(hazards_);
  assert_not_null(command_buffer);
  hazards_ = &hazards;
  command_buffer_ = command_buffer;
  render_ = nullptr;
  compute_ = nullptr;
  blit_ = nullptr;
  reads_.clear();
  writes_.clear();
  waited_fences_.clear();
}

void MetalBufferHazards::Encoder::Begin(MetalBufferHazards& hazards,
                                        MTL::CommandBuffer* command_buffer,
                                        MTL::RenderCommandEncoder* encoder) {
  BeginCommon(hazards, command_buffer);
  render_ = encoder;
}

void MetalBufferHazards::Encoder::Begin(MetalBufferHazards& hazards,
                                        MTL::CommandBuffer* command_buffer,
                                        MTL::ComputeCommandEncoder* encoder) {
  BeginCommon(hazards, command_buffer);
  compute_ = encoder;
}

void MetalBufferHazards::Encoder::Begin(MetalBufferHazards& hazards,
                                        MTL::CommandBuffer* command_buffer,
                                        MTL::BlitCommandEncoder* encoder) {
  BeginCommon(hazards, command_buffer);
  blit_ = encoder;
}

bool MetalBufferHazards::Encoder::Overlaps(const std::vector<Range>& ranges,
                                           uint32_t start, uint32_t end) {
  // The first range ending after the start.
  auto it = std::upper_bound(
      ranges.begin(), ranges.end(), start,
      [](uint32_t value, const Range& range) { return value < range.end; });
  return it != ranges.end() && it->start < end;
}

void MetalBufferHazards::Encoder::Insert(std::vector<Range>& ranges,
                                         uint32_t start, uint32_t end) {
  // Merge with every range overlapping or adjacent to the new one.
  auto first = std::lower_bound(
      ranges.begin(), ranges.end(), start,
      [](const Range& range, uint32_t value) { return range.end < value; });
  auto last = first;
  while (last != ranges.end() && last->start <= end) {
    start = std::min(start, last->start);
    end = std::max(end, last->end);
    ++last;
  }
  if (first == last) {
    ranges.insert(first, Range{start, end});
    return;
  }
  *first = Range{start, end};
  ranges.erase(first + 1, last);
}

void MetalBufferHazards::Encoder::Wait(MTL::Fence* fence) {
  if (std::find(waited_fences_.begin(), waited_fences_.end(), fence) !=
      waited_fences_.end()) {
    return;
  }
  waited_fences_.push_back(fence);
  if (render_) {
    render_->waitForFence(fence, kRenderStages);
  } else if (compute_) {
    compute_->waitForFence(fence);
  } else {
    blit_->waitForFence(fence);
  }
}

bool MetalBufferHazards::Encoder::Access(uint32_t start, uint32_t length,
                                         bool write) {
  assert_not_null(hazards_);
  MetalBufferHazards& hazards = *hazards_;
  if (!length || start >= hazards.buffer_size_) {
    return false;
  }
  uint32_t end = start + std::min(length, hazards.buffer_size_ - start);

  bool conflict_within =
      Overlaps(writes_, start, end) || (write && Overlaps(reads_, start, end));
  Insert(write ? writes_ : reads_, start, end);

  // A read waits for the earlier writes, a write for the earlier reads too.
  bool check_writes = AnyBlock(hazards.write_blocks_, start, end);
  bool check_reads = write && AnyBlock(hazards.read_blocks_, start, end);
  if (check_writes || check_reads) {
    for (const std::unique_ptr<Group>& group_ptr : hazards.groups_) {
      Group& group = *group_ptr;
      if (!hazards.IsVisible(group, command_buffer_)) {
        continue;
      }
      if (check_writes) {
        for (const Record& record : group.writes) {
          if (record.start < end && start < record.end) {
            Wait(record.fence);
          }
        }
      }
      if (check_reads) {
        for (const Record& record : group.reads) {
          if (record.start < end && start < record.end) {
            Wait(record.fence);
          }
        }
      }
    }
  }
  return conflict_within;
}

void MetalBufferHazards::Encoder::OrderPreviousAccesses() {
  assert_not_null(hazards_);
  for (const std::unique_ptr<Group>& group_ptr : hazards_->groups_) {
    Group& group = *group_ptr;
    if (!hazards_->IsVisible(group, command_buffer_)) {
      continue;
    }
    for (MTL::Fence* fence : group.fences) {
      Wait(fence);
    }
  }
}

void MetalBufferHazards::Encoder::End() {
  if (!hazards_) {
    return;
  }
  MetalBufferHazards& hazards = *hazards_;
  if (!reads_.empty() || !writes_.empty()) {
    // Give this encoder a separate output fence from the producers it waits on.
    MTL::Fence* fence;
    std::vector<MTL::Fence*> skipped_fences;
    while (true) {
      fence = hazards.AcquireFence();
      if (std::find(waited_fences_.begin(), waited_fences_.end(), fence) ==
          waited_fences_.end()) {
        break;
      }
      skipped_fences.push_back(fence);
    }
    hazards.free_fences_.insert(hazards.free_fences_.end(),
                                skipped_fences.begin(), skipped_fences.end());
    if (render_) {
      render_->updateFence(fence, kRenderStages);
    } else if (compute_) {
      compute_->updateFence(fence);
    } else {
      blit_->updateFence(fence);
    }
    Group& group = hazards.GetGroup(command_buffer_);
    group.fences.push_back(fence);
    for (const Range& range : reads_) {
      group.reads.push_back(Record{range.start, range.end, fence});
      hazards.CountBlocks(hazards.read_blocks_, range.start, range.end, 1);
    }
    for (const Range& range : writes_) {
      group.writes.push_back(Record{range.start, range.end, fence});
      hazards.CountBlocks(hazards.write_blocks_, range.start, range.end, 1);
    }
  }
  Deactivate();
}

void MetalBufferHazards::Encoder::Abandon() {
  if (!hazards_) {
    return;
  }
  Deactivate();
}

void MetalBufferHazards::Encoder::Deactivate() {
  hazards_ = nullptr;
  command_buffer_ = nullptr;
  render_ = nullptr;
  compute_ = nullptr;
  blit_ = nullptr;
}

void MetalBufferHazards::Encoder::assert_ended() const {
  assert_null(hazards_);
}

MetalBufferHazards::MetalBufferHazards(MTL::Device* device,
                                       uint32_t buffer_size)
    : device_(device), buffer_size_(buffer_size) {
  uint32_t block_count =
      uint32_t((uint64_t(buffer_size) + (UINT32_C(1) << kBlockSizeLog2) - 1) >>
               kBlockSizeLog2);
  read_blocks_.resize(block_count);
  write_blocks_.resize(block_count);
}

MetalBufferHazards::~MetalBufferHazards() {
  for (const std::unique_ptr<Group>& group : groups_) {
    ReleaseGroup(*group);
  }
  groups_.clear();
  for (MTL::Fence* fence : free_fences_) {
    fence->release();
  }
  free_fences_.clear();
}

MetalBufferHazards::Group& MetalBufferHazards::GetGroup(
    MTL::CommandBuffer* command_buffer) {
  if (last_group_ && last_group_->command_buffer == command_buffer) {
    return *last_group_;
  }
  for (const std::unique_ptr<Group>& group : groups_) {
    if (group->command_buffer == command_buffer) {
      last_group_ = group.get();
      return *group;
    }
  }
  auto group = std::make_unique<Group>();
  group->command_buffer = command_buffer;
  command_buffer->retain();
  last_group_ = group.get();
  groups_.push_back(std::move(group));
  return *last_group_;
}

bool MetalBufferHazards::IsVisible(Group& group,
                                   MTL::CommandBuffer* command_buffer) {
  if (group.command_buffer == command_buffer || group.committed) {
    return true;
  }
  if (group.command_buffer->status() >= MTL::CommandBufferStatusCommitted) {
    group.committed = true;
    return true;
  }
  return false;
}

bool MetalBufferHazards::AnyBlock(const std::vector<uint32_t>& blocks,
                                  uint32_t start, uint32_t end) {
  uint32_t block_last = (end - 1) >> kBlockSizeLog2;
  for (uint32_t block = start >> kBlockSizeLog2; block <= block_last; ++block) {
    if (blocks[block]) {
      return true;
    }
  }
  return false;
}

void MetalBufferHazards::CountBlocks(std::vector<uint32_t>& blocks,
                                     uint32_t start, uint32_t end,
                                     int32_t delta) {
  uint32_t block_last = (end - 1) >> kBlockSizeLog2;
  for (uint32_t block = start >> kBlockSizeLog2; block <= block_last; ++block) {
    assert_true(delta > 0 || blocks[block]);
    blocks[block] += uint32_t(delta);
  }
}

MTL::Fence* MetalBufferHazards::AcquireFence() {
  if (!free_fences_.empty()) {
    MTL::Fence* fence = free_fences_.back();
    free_fences_.pop_back();
    return fence;
  }
  return device_->newFence();
}

void MetalBufferHazards::ReleaseGroup(Group& group) {
  for (const Record& record : group.reads) {
    CountBlocks(read_blocks_, record.start, record.end, -1);
  }
  for (const Record& record : group.writes) {
    CountBlocks(write_blocks_, record.start, record.end, -1);
  }
  free_fences_.insert(free_fences_.end(), group.fences.begin(),
                      group.fences.end());
  group.fences.clear();
  group.reads.clear();
  group.writes.clear();
  if (group.command_buffer) {
    group.command_buffer->release();
    group.command_buffer = nullptr;
  }
}

void MetalBufferHazards::Retire() {
  last_group_ = nullptr;
  auto it = std::remove_if(
      groups_.begin(), groups_.end(), [this](std::unique_ptr<Group>& group) {
        MTL::CommandBufferStatus status = group->command_buffer->status();
        if (status != MTL::CommandBufferStatusCompleted &&
            status != MTL::CommandBufferStatusError) {
          return false;
        }
        ReleaseGroup(*group);
        return true;
      });
  groups_.erase(it, groups_.end());
}

void MetalBufferHazards::Discard(MTL::CommandBuffer* command_buffer) {
  last_group_ = nullptr;
  auto it = std::remove_if(groups_.begin(), groups_.end(),
                           [&](std::unique_ptr<Group>& group) {
                             if (group->command_buffer != command_buffer) {
                               return false;
                             }
                             ReleaseGroup(*group);
                             return true;
                           });
  groups_.erase(it, groups_.end());
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
