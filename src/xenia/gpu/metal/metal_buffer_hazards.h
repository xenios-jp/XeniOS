/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_BUFFER_HAZARDS_H_
#define XENIA_GPU_METAL_METAL_BUFFER_HAZARDS_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace xe {
namespace gpu {
namespace metal {

// Orders the GPU accesses to a buffer created without Metal hazard tracking.
// Metal tracks a whole buffer as one resource, so with tracking, any encoder
// writing any part of it waits for every earlier encoder accessing any other
// part, and the other way around. Here every encoder reports the ranges it
// accesses, and waits, with fences, only for the earlier encoders whose
// accesses overlap them.
//
// Encoders are ordered as the queue runs them: earlier encoders of the same
// command buffer, and those of command buffers already committed. An encoder
// can't wait for one in another command buffer that isn't committed yet - if
// that command buffer is committed later, it runs later, as with tracking.
class MetalBufferHazards {
 public:
  class Encoder {
   public:
    Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    ~Encoder() { assert_ended(); }

    void Begin(MetalBufferHazards& hazards, MTL::CommandBuffer* command_buffer,
               MTL::RenderCommandEncoder* encoder);
    void Begin(MetalBufferHazards& hazards, MTL::CommandBuffer* command_buffer,
               MTL::ComputeCommandEncoder* encoder);
    void Begin(MetalBufferHazards& hazards, MTL::CommandBuffer* command_buffer,
               MTL::BlitCommandEncoder* encoder);
    bool is_active() const { return hazards_ != nullptr; }
    // Accesses already encoded in this still-open encoder.
    bool HasAccess(uint32_t start, uint32_t length) const;

    // Encodes the waits for the earlier encoders whose accesses conflict with
    // this one. Returns whether it conflicts with an earlier access by this
    // encoder, which only a barrier within the encoder can order.
    bool Access(uint32_t start, uint32_t length, bool write);
    // Orders a fence publication after the preceding buffer accesses without
    // recording a fictitious whole-buffer access. Report its actual write with
    // Access afterwards so unrelated future ranges may proceed independently.
    void OrderPreviousAccesses();
    // Encodes the fence update for the encoders after this one. Call right
    // before ending the encoder.
    void End();
    // For an encoder abandoned before anything was encoded into it.
    void Abandon();

   private:
    struct Range {
      uint32_t start;
      uint32_t end;
    };
    void BeginCommon(MetalBufferHazards& hazards,
                     MTL::CommandBuffer* command_buffer);
    void Wait(MTL::Fence* fence);
    void Deactivate();
    static bool Overlaps(const std::vector<Range>& ranges, uint32_t start,
                         uint32_t end);
    static void Insert(std::vector<Range>& ranges, uint32_t start,
                       uint32_t end);
    void assert_ended() const;

    MetalBufferHazards* hazards_ = nullptr;
    MTL::CommandBuffer* command_buffer_ = nullptr;
    MTL::RenderCommandEncoder* render_ = nullptr;
    MTL::ComputeCommandEncoder* compute_ = nullptr;
    MTL::BlitCommandEncoder* blit_ = nullptr;
    // Sorted and disjoint.
    std::vector<Range> reads_;
    std::vector<Range> writes_;
    std::vector<MTL::Fence*> waited_fences_;
  };

  MetalBufferHazards(MTL::Device* device, uint32_t buffer_size);
  ~MetalBufferHazards();
  MetalBufferHazards(const MetalBufferHazards&) = delete;
  MetalBufferHazards& operator=(const MetalBufferHazards&) = delete;

  // Forgets the accesses of the command buffers the GPU has completed.
  void Retire();
  // Includes ended encoders in command buffers that have not been committed.
  bool HasPendingAccess(uint32_t start, uint32_t length) const;

  // For a command buffer released without a commit - nothing in it will run.
  void Discard(MTL::CommandBuffer* command_buffer);

 private:
  static constexpr uint32_t kBlockSizeLog2 = 16;

  struct Record {
    uint32_t start;
    uint32_t end;
    MTL::Fence* fence;
  };
  struct Group {
    // Retained.
    MTL::CommandBuffer* command_buffer;
    bool committed = false;
    std::vector<Record> reads;
    std::vector<Record> writes;
    std::vector<MTL::Fence*> fences;
  };

  Group& GetGroup(MTL::CommandBuffer* command_buffer);
  bool IsVisible(Group& group, MTL::CommandBuffer* command_buffer);
  static bool AnyBlock(const std::vector<uint32_t>& blocks, uint32_t start,
                       uint32_t end);
  void CountBlocks(std::vector<uint32_t>& blocks, uint32_t start, uint32_t end,
                   int32_t delta);
  MTL::Fence* AcquireFence();
  void ReleaseGroup(Group& group);

  MTL::Device* device_;
  uint32_t buffer_size_;
  std::vector<std::unique_ptr<Group>> groups_;
  // Group lookup for the command buffer the last encoder used.
  Group* last_group_ = nullptr;
  // Recorded accesses touching each block, to skip the scans of the records
  // for the ranges nothing has accessed recently.
  std::vector<uint32_t> read_blocks_;
  std::vector<uint32_t> write_blocks_;
  std::vector<MTL::Fence*> free_fences_;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_BUFFER_HAZARDS_H_
