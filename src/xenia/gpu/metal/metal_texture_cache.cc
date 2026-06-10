/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_heap_pool.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/autorelease_pool_mac.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_shared_memory.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_128bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_128bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_16bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_16bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_32bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_32bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_64bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_64bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_8bpb_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_8bpb_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_bgrg8_rgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_bgrg8_rgbg8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_ctx1_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_unorm_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_depth_unorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxn_rg8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt1_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3a_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3aas1111_argb4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt3aas1111_bgra4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt5_rgba8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_dxt5a_r8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_gbgr8_grgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_gbgr8_rgb8_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r16_unorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_a4r4g4b4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_a4r4g4b4_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r4g4b4a4_b4g4r4a4_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b5a1_b5g5r5a1_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g6b5_b5g6r5_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_r5g6b5_b5g6r5_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rg16_unorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_snorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_snorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_unorm_float_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_load_rgba16_unorm_float_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/texture_upload_repack.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(metal_force_bc_decompress, false,
            "Force BC1/2/3/5/DXN decompression to RGBA8/RG8 (debug).", "Metal");
DEFINE_bool(metal_texture_cache_use_private, true,
            "Use MTLStorageModePrivate for Metal texture cache textures when "
            "GPU upload paths support it.",
            "Metal");
DEFINE_bool(metal_texture_upload_via_blit, true,
            "Upload textures via staging buffers and GPU blit copies instead "
            "of CPU replaceRegion.",
            "Metal");
DECLARE_bool(metal_use_heaps);
DECLARE_int32(metal_heap_min_bytes);

namespace xe {
namespace gpu {
namespace metal {
namespace {

#if XE_PLATFORM_IOS
constexpr uint64_t kUploadBufferPoolMaxBytes = 128ull * 1024ull * 1024ull;
constexpr uint64_t kScaledResolveRetiredMaxBytes = 64ull * 1024ull * 1024ull;
#else
constexpr uint64_t kUploadBufferPoolMaxBytes = 512ull * 1024ull * 1024ull;
constexpr uint64_t kScaledResolveRetiredMaxBytes = 256ull * 1024ull * 1024ull;
#endif
constexpr uint32_t kViewBindlessHeapPressureThreshold = 65536;

struct MetalLoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t padding0;  // Pad to 16-byte boundary for uint3 in MSL.
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
  uint32_t padding1[5];  // Pad to 64 bytes to match HLSL CB size.
};
static_assert(sizeof(MetalLoadConstants) == 64);

struct MetalTextureUploadRepackConstants {
  uint32_t source_offset;
  uint32_t dest_offset;
  uint32_t source_row_pitch;
  uint32_t dest_row_pitch;
  uint32_t source_image_pitch;
  uint32_t dest_image_pitch;
  uint32_t row_bytes;
  uint32_t row_count;
  uint32_t depth;
  uint32_t padding[3];
};
static_assert(sizeof(MetalTextureUploadRepackConstants) == 48);

class ScopedAutoreleasePool {
 public:
  ScopedAutoreleasePool() : pool_(NS::AutoreleasePool::alloc()->init()) {}
  ~ScopedAutoreleasePool() {
    if (pool_) {
      pool_->release();
    }
  }

  ScopedAutoreleasePool(const ScopedAutoreleasePool&) = delete;
  ScopedAutoreleasePool& operator=(const ScopedAutoreleasePool&) = delete;

 private:
  NS::AutoreleasePool* pool_;
};

void SetEncoderLabel(MTL::CommandEncoder* encoder, const char* label) {
  if (!encoder || !label) {
    return;
  }
  encoder->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
}

bool SupportsPixelFormat(MTL::Device* device, MTL::PixelFormat format) {
  if (!device || format == MTL::PixelFormatInvalid) {
    return false;
  }
  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType2D);
  descriptor->setPixelFormat(format);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setDepth(1);
  descriptor->setArrayLength(1);
  descriptor->setMipmapLevelCount(1);
  descriptor->setSampleCount(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);
  MTL::Texture* texture = device->newTexture(descriptor);
  descriptor->release();
  if (texture) {
    texture->release();
  }
  return texture != nullptr;
}

uint32_t GetEstimatedBytesPerPixel(MTL::PixelFormat format) {
  switch (format) {
    case MTL::PixelFormatRGBA8Unorm:
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatBGRA8Unorm:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatR32Float:
    case MTL::PixelFormatR32Uint:
    case MTL::PixelFormatR32Sint:
    case MTL::PixelFormatDepth32Float:
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatX32_Stencil8:
      return 4;
    case MTL::PixelFormatRG16Float:
    case MTL::PixelFormatRG16Uint:
    case MTL::PixelFormatRG16Sint:
      return 4;
    case MTL::PixelFormatRGBA16Float:
    case MTL::PixelFormatRGBA16Uint:
    case MTL::PixelFormatRGBA16Sint:
    case MTL::PixelFormatRG32Float:
    case MTL::PixelFormatRG32Uint:
    case MTL::PixelFormatRG32Sint:
    case MTL::PixelFormatDepth32Float_Stencil8:
      return 8;
    case MTL::PixelFormatR16Float:
    case MTL::PixelFormatR16Uint:
    case MTL::PixelFormatR16Sint:
    case MTL::PixelFormatDepth16Unorm:
      return 2;
    default:
      return 4;
  }
}

uint64_t EstimateTextureBytes(MTL::Texture* texture) {
  if (!texture) {
    return 0;
  }

  const uint32_t bytes_per_pixel =
      GetEstimatedBytesPerPixel(texture->pixelFormat());
  const uint32_t sample_count =
      std::max<uint32_t>(1, static_cast<uint32_t>(texture->sampleCount()));
  const uint32_t mip_count =
      std::max<uint32_t>(1, static_cast<uint32_t>(texture->mipmapLevelCount()));
  const uint32_t array_length =
      std::max<uint32_t>(1, static_cast<uint32_t>(texture->arrayLength()));

  uint64_t total = 0;
  for (uint32_t level = 0; level < mip_count; ++level) {
    uint32_t width =
        std::max<uint32_t>(1, static_cast<uint32_t>(texture->width() >> level));
    uint32_t height = std::max<uint32_t>(
        1, static_cast<uint32_t>(texture->height() >> level));
    uint32_t depth =
        std::max<uint32_t>(1, static_cast<uint32_t>(texture->depth() >> level));
    uint64_t level_bytes = uint64_t(width) * uint64_t(height) *
                           uint64_t(depth) * bytes_per_pixel * sample_count;
    total += level_bytes;
  }

  return total * array_length;
}

bool AreDimensionsCompatible(xenos::FetchOpDimension shader_dimension,
                             xenos::DataDimension texture_dimension) {
  switch (shader_dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D:
      return texture_dimension == xenos::DataDimension::k1D ||
             texture_dimension == xenos::DataDimension::k2DOrStacked ||
             texture_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::k3DOrStacked:
      return texture_dimension == xenos::DataDimension::k2DOrStacked ||
             texture_dimension == xenos::DataDimension::k3D;
    case xenos::FetchOpDimension::kCube:
      return texture_dimension == xenos::DataDimension::kCube;
    default:
      return false;
  }
}

MTL::TextureSwizzleChannels ToMetalTextureSwizzle(uint32_t xenos_swizzle) {
  MTL::TextureSwizzleChannels swizzle;
  // Xenos: R=0, G=1, B=2, A=3, 0=4, 1=5
  // Metal: Zero=0, One=1, Red=2, Green=3, Blue=4, Alpha=5
  static const MTL::TextureSwizzle kMap[] = {
      MTL::TextureSwizzleRed,    // 0
      MTL::TextureSwizzleGreen,  // 1
      MTL::TextureSwizzleBlue,   // 2
      MTL::TextureSwizzleAlpha,  // 3
      MTL::TextureSwizzleZero,   // 4
      MTL::TextureSwizzleOne,    // 5
      MTL::TextureSwizzleZero,   // 6 (Unused)
      MTL::TextureSwizzleZero,   // 7 (Unused)
  };
  swizzle.red = kMap[(xenos_swizzle >> 0) & 0x7];
  swizzle.green = kMap[(xenos_swizzle >> 3) & 0x7];
  swizzle.blue = kMap[(xenos_swizzle >> 6) & 0x7];
  swizzle.alpha = kMap[(xenos_swizzle >> 9) & 0x7];
  return swizzle;
}

}  // namespace

class MetalTextureCache::UploadBufferPool
    : public std::enable_shared_from_this<UploadBufferPool> {
 public:
  explicit UploadBufferPool(MTL::Device* device, uint64_t max_pooled_bytes)
      : device_(device), max_pooled_bytes_(max_pooled_bytes) {}

  MTL::Buffer* Acquire(size_t size) {
    if (!device_) {
      return nullptr;
    }
    size = xe::round_up(size, size_t(256));
    bool can_pool = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++usage_tick_;
      auto available_it = available_entries_by_size_.lower_bound(size);
      while (available_it != available_entries_by_size_.end()) {
        const size_t entry_index = available_it->second;
        Entry& entry = entries_[entry_index];
        if (entry.buffer && !entry.in_use && entry.size >= size) {
          available_entries_by_size_.erase(available_it);
          entry.in_use = true;
          entry.last_used_tick = usage_tick_;
          return entry.buffer;
        }
        // Defensive cleanup if an old entry was left in the size index.
        available_it = available_entries_by_size_.erase(available_it);
      }
      can_pool = pooled_bytes_ + size <= max_pooled_bytes_;
    }
    MTL::Buffer* buffer =
        device_->newBuffer(size, MTL::ResourceStorageModeShared);
    if (!buffer) {
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++usage_tick_;
      if (can_pool && pooled_bytes_ + size <= max_pooled_bytes_) {
        pooled_bytes_ += size;
        const size_t entry_index = entries_.size();
        entries_.push_back({buffer, size, true, true, usage_tick_});
        entry_indices_by_buffer_[buffer] = entry_index;
        return buffer;
      }
      ++transient_allocations_;
    }
    return buffer;
  }

  void ReleaseImmediate(MTL::Buffer* buffer) {
    if (!buffer) {
      return;
    }
    MTL::Buffer* buffers[] = {buffer};
    ReleaseImmediateBatch(buffers, 1);
  }

  void ReleaseImmediateBatch(MTL::Buffer* const* buffers, size_t count) {
    if (!buffers || !count) {
      return;
    }
    std::vector<MTL::Buffer*> transient_buffers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++usage_tick_;
      const uint64_t release_tick = usage_tick_;
      for (size_t buffer_index = 0; buffer_index < count; ++buffer_index) {
        MTL::Buffer* buffer = buffers[buffer_index];
        if (!buffer) {
          continue;
        }
        bool release_transient = true;
        auto entry_it = entry_indices_by_buffer_.find(buffer);
        if (entry_it != entry_indices_by_buffer_.end()) {
          Entry& entry = entries_[entry_it->second];
          if (entry.buffer == buffer) {
            if (entry.in_use) {
              entry.in_use = false;
              entry.last_used_tick = release_tick;
              available_entries_by_size_.emplace(entry.size, entry_it->second);
            }
            release_transient = false;
          }
        }
        // Compatibility with any pool state created before the index existed.
        if (release_transient) {
          for (size_t i = 0; i < entries_.size(); ++i) {
            Entry& entry = entries_[i];
            if (entry.buffer != buffer) {
              continue;
            }
            entry_indices_by_buffer_[buffer] = i;
            if (entry.in_use) {
              entry.in_use = false;
              entry.last_used_tick = release_tick;
              available_entries_by_size_.emplace(entry.size, i);
            }
            release_transient = false;
            break;
          }
        }
        if (release_transient) {
          transient_buffers.push_back(buffer);
        }
      }
    }
    for (MTL::Buffer* buffer : transient_buffers) {
      buffer->release();
    }
  }

  void ReleaseAfter(MTL::CommandBuffer* cmd, MTL::Buffer* buffer) {
    if (!buffer) {
      return;
    }
    if (!cmd) {
      ReleaseImmediate(buffer);
      return;
    }
    bool add_handler = false;
    {
      std::lock_guard<std::mutex> lock(PendingReleasesMutex());
      auto& pending = PendingReleasesMap()[cmd];
      add_handler = pending.empty();
      pending.push_back({shared_from_this(), buffer});
    }
    if (add_handler) {
      cmd->addCompletedHandler(^(MTL::CommandBuffer* completed_cmd) {
        UploadBufferPool::HandleCommandBufferCompleted(completed_cmd);
      });
    }
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
      if (!entry.buffer) {
        continue;
      }
      if (entry.in_use) {
        // Buffer is in-flight: a pending completion handler holds it in
        // PendingReleasesMap and will call ReleaseImmediateBatch when the GPU
        // finishes.  After we clear entry_indices_by_buffer_ below,
        // ReleaseImmediateBatch will treat it as a transient buffer and call
        // buffer->release() exactly once.  Skip the release here so the buffer
        // is not released twice.
        entry.buffer = nullptr;
      } else {
        entry.buffer->release();
        entry.buffer = nullptr;
      }
    }
    pooled_bytes_ = 0;
    entries_.clear();
    available_entries_by_size_.clear();
    entry_indices_by_buffer_.clear();
  }

  size_t GetEntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  uint64_t GetTotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pooled_bytes_;
  }

 private:
  struct Entry {
    MTL::Buffer* buffer = nullptr;
    size_t size = 0;
    bool pooled = false;
    bool in_use = false;
    uint64_t last_used_tick = 0;
  };

  struct PendingRelease {
    std::shared_ptr<UploadBufferPool> pool;
    MTL::Buffer* buffer = nullptr;
  };

  using PendingReleasesByCommandBuffer =
      std::unordered_map<MTL::CommandBuffer*, std::vector<PendingRelease>>;

  static std::mutex& PendingReleasesMutex() {
    // Heap-allocated and intentionally leaked to avoid static destruction order
    // issues on macOS/POSIX where threads may outlive static destructors.
    static std::mutex* pending_releases_mutex = new std::mutex();
    return *pending_releases_mutex;
  }

  static PendingReleasesByCommandBuffer& PendingReleasesMap() {
    // Heap-allocated and intentionally leaked for the same reason as the mutex.
    static PendingReleasesByCommandBuffer* pending_releases =
        new PendingReleasesByCommandBuffer();
    return *pending_releases;
  }

  static void HandleCommandBufferCompleted(MTL::CommandBuffer* cmd);

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::multimap<size_t, size_t> available_entries_by_size_;
  std::unordered_map<MTL::Buffer*, size_t> entry_indices_by_buffer_;
  MTL::Device* device_ = nullptr;
  uint64_t max_pooled_bytes_ = 0;
  uint64_t pooled_bytes_ = 0;
  uint64_t usage_tick_ = 0;
  uint64_t transient_allocations_ = 0;
};

void MetalTextureCache::UploadBufferPool::HandleCommandBufferCompleted(
    MTL::CommandBuffer* cmd) {
  std::vector<PendingRelease> releases;
  {
    std::lock_guard<std::mutex> lock(PendingReleasesMutex());
    auto& pending_releases = PendingReleasesMap();
    auto it = pending_releases.find(cmd);
    if (it == pending_releases.end()) {
      return;
    }
    releases = std::move(it->second);
    pending_releases.erase(it);
  }
  std::sort(releases.begin(), releases.end(),
            [](const PendingRelease& a, const PendingRelease& b) {
              return a.pool.get() < b.pool.get();
            });
  std::vector<MTL::Buffer*> buffers;
  buffers.reserve(releases.size());
  for (size_t i = 0; i < releases.size();) {
    std::shared_ptr<UploadBufferPool> pool = releases[i].pool;
    buffers.clear();
    size_t j = i;
    for (; j < releases.size() && releases[j].pool.get() == pool.get(); ++j) {
      if (releases[j].buffer) {
        buffers.push_back(releases[j].buffer);
      }
    }
    if (pool && !buffers.empty()) {
      pool->ReleaseImmediateBatch(buffers.data(), buffers.size());
    }
    i = j;
  }
}

MetalTextureCache::MetalTextureCache(MetalCommandProcessor* command_processor,
                                     const RegisterFile& register_file,
                                     MetalSharedMemory& shared_memory,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x,
                   draw_resolution_scale_y),
      command_processor_(command_processor) {}

MetalTextureCache::~MetalTextureCache() { Shutdown(); }

uint64_t MetalTextureCache::GetTextureLoadBytes(const Texture& texture,
                                                bool load_base,
                                                bool load_mips) {
  return (load_base ? uint64_t(xe::align(texture.GetGuestBaseSize(),
                                         UINT32_C(16)))
                    : 0) +
         (load_mips ? uint64_t(xe::align(texture.GetGuestMipsSize(),
                                         UINT32_C(16)))
                    : 0);
}

MTL::StorageMode MetalTextureCache::GetCacheTextureStorageMode() const {
  if (!::cvars::metal_texture_upload_via_blit ||
      !::cvars::metal_texture_cache_use_private) {
    return MTL::StorageModeShared;
  }
  return MTL::StorageModePrivate;
}

bool MetalTextureCache::ShouldUploadViaBlit() const {
  return ::cvars::metal_texture_upload_via_blit;
}

void MetalTextureCache::BeginUploadCommandBufferBatch() {
  ++upload_batch_depth_;
  if (upload_batch_depth_ != 1) {
    return;
  }
  if (!ShouldUploadViaBlit() || !command_processor_) {
    return;
  }
  // Avoid cross-command-buffer upload batching while a draw/copy command
  // buffer is already active in the command processor. Keeping upload work on
  // a separate command buffer in that state can reorder with in-flight render
  // setup and lead to startup rendering regressions.
  if (command_processor_->HasActiveSubmission()) {
    return;
  }
  MTL::CommandBuffer* cmd =
      command_processor_->CreateStandaloneTransferCommandBuffer(
          "XeniaCB reason=texture-upload-batch");
  if (!cmd) {
    return;
  }
  upload_batch_command_buffer_ = cmd;
  upload_batch_command_buffer_has_work_ = false;
  upload_batch_command_buffer_shared_memory_ranges_.clear();
}

void MetalTextureCache::EndUploadCommandBufferBatch() {
  if (!upload_batch_depth_) {
    return;
  }
  --upload_batch_depth_;
  if (upload_batch_depth_ != 0) {
    return;
  }
  MTL::CommandBuffer* cmd = upload_batch_command_buffer_;
  upload_batch_command_buffer_ = nullptr;
  bool has_work = upload_batch_command_buffer_has_work_;
  auto shared_memory_ranges =
      std::move(upload_batch_command_buffer_shared_memory_ranges_);
  upload_batch_command_buffer_has_work_ = false;
  upload_batch_command_buffer_shared_memory_ranges_.clear();
  if (!cmd) {
    return;
  }
  if (!has_work) {
    cmd->release();
    return;
  }
  if (command_processor_) {
    if (!shared_memory_ranges.empty()) {
      static_cast<MetalSharedMemory&>(shared_memory()).TrackStandaloneGpuAccess(
          cmd, shared_memory_ranges.data(),
          static_cast<uint32_t>(shared_memory_ranges.size()));
    }
    command_processor_->CommitStandaloneAsync(cmd);
  } else {
    cmd->release();
  }
}

void MetalTextureCache::AbortUploadCommandBufferBatch(bool commit_if_has_work) {
  MTL::CommandBuffer* cmd = upload_batch_command_buffer_;
  upload_batch_command_buffer_ = nullptr;
  bool has_work = upload_batch_command_buffer_has_work_;
  auto shared_memory_ranges =
      std::move(upload_batch_command_buffer_shared_memory_ranges_);
  upload_batch_command_buffer_has_work_ = false;
  upload_batch_command_buffer_shared_memory_ranges_.clear();
  if (!cmd) {
    return;
  }
  if (!has_work || !commit_if_has_work) {
    cmd->release();
    return;
  }
  if (command_processor_) {
    if (!shared_memory_ranges.empty()) {
      static_cast<MetalSharedMemory&>(shared_memory()).TrackStandaloneGpuAccess(
          cmd, shared_memory_ranges.data(),
          static_cast<uint32_t>(shared_memory_ranges.size()));
    }
    command_processor_->CommitStandaloneAsync(cmd);
  } else {
    cmd->release();
  }
}

void MetalTextureCache::BeginDeferredUploadEncoderBatch() {
  ++deferred_upload_batch_depth_;
}

bool MetalTextureCache::EndDeferredUploadEncoderBatch() {
  if (!deferred_upload_batch_depth_) {
    return true;
  }
  --deferred_upload_batch_depth_;
  if (deferred_upload_batch_depth_ != 0) {
    return true;
  }
  if (!deferred_upload_compute_encoder_ && deferred_upload_copies_.empty()) {
    deferred_upload_command_buffer_ = nullptr;
    return true;
  }
  return FlushDeferredUploadEncoderBatch();
}

bool MetalTextureCache::FlushDeferredUploadEncoderBatch() {
  MTL::CommandBuffer* cmd = deferred_upload_command_buffer_;
  if (!deferred_upload_compute_encoder_ && deferred_upload_copies_.empty()) {
    deferred_upload_command_buffer_ = nullptr;
    return true;
  }
  if (deferred_upload_compute_encoder_) {
    MTL::ComputeCommandEncoder* compute_encoder =
        deferred_upload_compute_encoder_;
    deferred_upload_compute_encoder_ = nullptr;
    compute_encoder->endEncoding();
    compute_encoder->release();
  }
  bool success = true;
  if (cmd && !deferred_upload_copies_.empty()) {
    if (command_processor_ &&
        cmd == command_processor_->GetCurrentCommandBuffer()) {
      command_processor_->EndSharedMemoryUploadBlitEncoder(
          MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
              kTextureDeferredBlit);
    }
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    if (!blit) {
      XELOGE("Metal texture upload: failed to create deferred blit encoder");
      success = false;
    } else {
      for (const DeferredUploadCopy& copy : deferred_upload_copies_) {
        blit->copyFromBuffer(
            copy.source_buffer, copy.source_offset_bytes, copy.source_row_pitch,
            copy.source_bytes_per_image,
            MTL::Size::Make(copy.width, copy.height, copy.depth),
            copy.destination_texture, copy.destination_slice,
            copy.destination_level, MTL::Origin::Make(0, 0, 0));
      }
      blit->endEncoding();
    }
  }
  deferred_upload_copies_.clear();
  deferred_upload_command_buffer_ = nullptr;
  return success;
}

bool MetalTextureCache::FlushPendingUploadEncodersForCommandEncoderBoundary() {
  if (!deferred_upload_compute_encoder_ && deferred_upload_copies_.empty()) {
    return true;
  }
  return FlushDeferredUploadEncoderBatch();
}

void MetalTextureCache::BeginTextureUploadBatch() {
  BeginUploadCommandBufferBatch();
  BeginDeferredUploadEncoderBatch();
}

bool MetalTextureCache::EndTextureUploadBatch() {
  bool success = EndDeferredUploadEncoderBatch();
  if (!success) {
    XELOGE("Metal texture upload: failed to flush deferred upload batch");
    ResetTextureBindings();
  }
  EndUploadCommandBufferBatch();
  return success;
}

bool MetalTextureCache::PrepareTextureDataLoadRanges(
    Texture** textures, uint32_t texture_count, uint64_t base_outdated_mask,
    uint64_t mips_outdated_mask) {
  assert_true(texture_count <= 64);
  if (!textures || !texture_count ||
      (!base_outdated_mask && !mips_outdated_mask)) {
    return true;
  }

  std::vector<SharedMemory::Range> ranges;
  ranges.reserve(texture_count * 2);
  auto add_range_if_needed = [&](uint32_t start, uint32_t length) {
    if (!shared_memory().IsRangeValid(start, length)) {
      ranges.push_back({start, length});
    }
  };
  for (uint32_t i = 0; i < texture_count; ++i) {
    Texture* texture = textures[i];
    if (!texture) {
      continue;
    }
    TextureKey texture_key = texture->key();
    if (base_outdated_mask & (UINT64_C(1) << i)) {
      add_range_if_needed(static_cast<uint32_t>(texture_key.base_page << 12),
                          xe::align(texture->GetGuestBaseSize(), UINT32_C(16)));
    }
    if (mips_outdated_mask & (UINT64_C(1) << i)) {
      add_range_if_needed(static_cast<uint32_t>(texture_key.mip_page << 12),
                          xe::align(texture->GetGuestMipsSize(), UINT32_C(16)));
    }
  }
  if (ranges.empty()) {
    return true;
  }

  // Shared-memory uploads use a Metal blit encoder. Make sure no deferred
  // texture upload encoder is still open before requesting residency.
  if ((deferred_upload_compute_encoder_ || !deferred_upload_copies_.empty()) &&
      !FlushDeferredUploadEncoderBatch()) {
    if (command_processor_) {
      command_processor_->RecordSharedMemoryRequestOutcome(
          MetalCommandProcessor::SharedMemoryRequestOutcome::
              kTextureDeferredUploadFlush);
    }
    return false;
  }

  // Request the guest memory needed by this texture upload before loading the
  // texture data for the current draw.
  const MetalCommandProcessor::SharedMemoryRequestReason reason =
      base_outdated_mask && mips_outdated_mask
          ? MetalCommandProcessor::SharedMemoryRequestReason::
                kTextureBaseAndMips
      : base_outdated_mask
          ? MetalCommandProcessor::SharedMemoryRequestReason::kTextureBase
          : MetalCommandProcessor::SharedMemoryRequestReason::kTextureMips;
  if (!command_processor_) {
    XELOGE("Metal texture data residency requires a command processor");
    return false;
  }
  return command_processor_->RequestSharedMemoryRanges(
      reason, ranges.data(), static_cast<uint32_t>(ranges.size()));
}

bool MetalTextureCache::RequestTextureDataRange(Texture&,
                                                TextureDataRangeSource source,
                                                uint32_t start,
                                                uint32_t length) {
  if (shared_memory().IsRangeValid(start, length)) {
    return true;
  }
  static bool range_not_resident_logged = false;
  if (!range_not_resident_logged) {
    range_not_resident_logged = true;
    XELOGE(
        "Metal texture load range was not resident after residency request "
        "(source={} start=0x{:08X} length={}); skipping texture load",
        source == TextureDataRangeSource::kBase ? "base" : "mips", start,
        length);
  }
  return false;
}

void MetalTextureCache::RecordTextureWatchInvalidation(
    const Texture&, bool is_mip, TextureWatchInvalidationSource source,
    uint32_t byte_count) {
  if (!command_processor_) {
    return;
  }
  using Reason = MetalCommandProcessor::TextureWatchInvalidationReason;
  Reason reason;
  switch (source) {
    case TextureWatchInvalidationSource::kCpu:
      reason = is_mip ? Reason::kCpuMips : Reason::kCpuBase;
      break;
    case TextureWatchInvalidationSource::kGpuResolve:
      reason = is_mip ? Reason::kGpuResolveMips : Reason::kGpuResolveBase;
      break;
    case TextureWatchInvalidationSource::kGpuOther:
    default:
      reason = is_mip ? Reason::kGpuOtherMips : Reason::kGpuOtherBase;
      break;
  }
  command_processor_->RecordTextureWatchInvalidation(reason, byte_count);
}

MTL::ComputeCommandEncoder* MetalTextureCache::GetDeferredUploadComputeEncoder(
    MTL::CommandBuffer* command_buffer) {
  if (!deferred_upload_batch_depth_ || !command_buffer) {
    return nullptr;
  }
  if (deferred_upload_command_buffer_ &&
      deferred_upload_command_buffer_ != command_buffer &&
      !FlushDeferredUploadEncoderBatch()) {
    return nullptr;
  }
  deferred_upload_command_buffer_ = command_buffer;
  if (!deferred_upload_compute_encoder_) {
    if (command_processor_ &&
        command_buffer == command_processor_->GetCurrentCommandBuffer()) {
      command_processor_->EndSharedMemoryUploadBlitEncoder(
          MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
              kTextureCompute);
    }
    deferred_upload_compute_encoder_ = command_buffer->computeCommandEncoder();
    if (deferred_upload_compute_encoder_) {
      SetEncoderLabel(deferred_upload_compute_encoder_,
                      "XeniaTextureUploadDeferredComputeEncoder");
      deferred_upload_compute_encoder_->retain();
    }
  }
  return deferred_upload_compute_encoder_;
}

void MetalTextureCache::QueueDeferredUploadCopy(
    MTL::Buffer* source_buffer, size_t source_offset_bytes,
    size_t source_row_pitch, size_t source_bytes_per_image,
    MTL::Texture* destination_texture, uint32_t destination_slice,
    uint32_t destination_level, uint32_t width, uint32_t height,
    uint32_t depth) {
  assert_not_zero(deferred_upload_batch_depth_);
  assert_not_null(deferred_upload_command_buffer_);
  assert_not_null(source_buffer);
  assert_not_null(destination_texture);
  assert_not_zero(width);
  assert_not_zero(height);
  assert_not_zero(depth);
  DeferredUploadCopy copy = {};
  copy.source_buffer = source_buffer;
  copy.source_offset_bytes = source_offset_bytes;
  copy.source_row_pitch = source_row_pitch;
  copy.source_bytes_per_image = source_bytes_per_image;
  copy.destination_texture = destination_texture;
  copy.destination_slice = destination_slice;
  copy.destination_level = destination_level;
  copy.width = width;
  copy.height = height;
  copy.depth = depth;
  deferred_upload_copies_.push_back(copy);
}

class MetalTextureCache::UploadBatchScope {
 public:
  explicit UploadBatchScope(MetalTextureCache& texture_cache)
      : texture_cache_(texture_cache) {
    texture_cache_.BeginTextureUploadBatch();
  }

  UploadBatchScope(const UploadBatchScope&) = delete;
  UploadBatchScope& operator=(const UploadBatchScope&) = delete;

  ~UploadBatchScope() { End(); }

  bool End() {
    if (ended_) {
      return success_;
    }
    success_ = texture_cache_.EndTextureUploadBatch();
    ended_ = true;
    return success_;
  }

 private:
  MetalTextureCache& texture_cache_;
  bool ended_ = false;
  bool success_ = true;
};

bool MetalTextureCache::PrepareTextureMaterialization(
    const RegisterFile& regs, uint32_t used_texture_mask,
    TextureMaterializationPlan& plan) {
  SCOPE_profile_cpu_f("gpu");
  plan.Reset();
  if (!used_texture_mask) {
    return true;
  }

  auto append_texture = [&](TextureKey key) {
    ++plan.request_count;
    Texture* texture = FindOrCreateTexture(key);
    bool base_outdated =
        texture ? texture->base_outdated_lockless() : false;
    bool mips_outdated =
        texture ? texture->mips_outdated_lockless() : false;
    if (!texture || (!base_outdated && !mips_outdated)) {
      return;
    }
    for (const TextureMaterializationPlan::TextureLoad& planned_load :
         plan.texture_loads_) {
      if (planned_load.texture == texture) {
        if (command_processor_) {
          command_processor_->RecordTextureUploadExecutionDetail(
              MetalCommandProcessor::TextureUploadExecutionDetail::
                  kDuplicatePlannedSamePlan);
        }
        return;
      }
    }
    const TextureKey& texture_key = texture->key();
    const uint32_t base_start = texture_key.base_page << 12;
    const uint32_t mips_start = texture_key.mip_page << 12;
    const uint32_t base_length =
        xe::align(texture->GetGuestBaseSize(), UINT32_C(16));
    const uint32_t mips_length =
        xe::align(texture->GetGuestMipsSize(), UINT32_C(16));

    enum class SourceRangeState {
      kNone,
      kInvalid,
      kValid,
      kMixed,
    };
    auto source_range_state = [&](bool needed, uint32_t start,
                                  uint32_t length) {
      if (!needed || !length) {
        return SourceRangeState::kNone;
      }
      if (shared_memory().IsRangeInvalid(start, length)) {
        return SourceRangeState::kInvalid;
      }
      if (shared_memory().IsRangeValid(start, length)) {
        return SourceRangeState::kValid;
      }
      return SourceRangeState::kMixed;
    };
    const SourceRangeState base_state =
        source_range_state(base_outdated, base_start, base_length);
    const SourceRangeState mips_state =
        source_range_state(mips_outdated, mips_start, mips_length);
    const bool base_needs_upload =
        base_state == SourceRangeState::kInvalid ||
        base_state == SourceRangeState::kMixed;
    const bool mips_needs_upload =
        mips_state == SourceRangeState::kInvalid ||
        mips_state == SourceRangeState::kMixed;
    const bool base_cpu_source =
        base_state == SourceRangeState::kInvalid;
    const bool mips_cpu_source =
        mips_state == SourceRangeState::kInvalid;

    bool use_cpu_source = !texture_key.scaled_resolve;
    bool has_cpu_source_range = false;
    if (base_outdated && base_length) {
      use_cpu_source &= base_cpu_source;
      has_cpu_source_range |= base_cpu_source;
    }
    if (mips_outdated && mips_length) {
      use_cpu_source &= mips_cpu_source;
      has_cpu_source_range |= mips_cpu_source;
    }
    use_cpu_source &= has_cpu_source_range;
    if (!use_cpu_source) {
      if (command_processor_) {
        MetalCommandProcessor::TextureUploadSourceFallbackReason
            fallback_reason =
                MetalCommandProcessor::TextureUploadSourceFallbackReason::
                    kUnknown;
        if (texture_key.scaled_resolve) {
          fallback_reason =
              MetalCommandProcessor::TextureUploadSourceFallbackReason::
                  kScaledResolve;
        } else if (base_state == SourceRangeState::kMixed ||
                   mips_state == SourceRangeState::kMixed) {
          fallback_reason =
              MetalCommandProcessor::TextureUploadSourceFallbackReason::
                  kMixedValidity;
        } else if (base_state == SourceRangeState::kValid ||
                   mips_state == SourceRangeState::kValid) {
          fallback_reason =
              MetalCommandProcessor::TextureUploadSourceFallbackReason::
                  kSourceAlreadyResident;
        }
        command_processor_->RecordTextureUploadSourceFallback(fallback_reason);
      }
      if (base_needs_upload) {
        plan.source_ranges.push_back({base_start, base_length});
      }
      if (mips_needs_upload) {
        plan.source_ranges.push_back({mips_start, mips_length});
      }
    }

    if (command_processor_) {
      const uint64_t upload_bytes =
          GetTextureLoadBytes(*texture, base_outdated, mips_outdated);
      if (base_outdated && base_state == SourceRangeState::kValid &&
          texture->last_base_watch_invalidation_source() ==
              TextureWatchInvalidationSource::kGpuResolve) {
        using ResolveReason =
            MetalCommandProcessor::TextureResolveReloadReason;
        auto record_resolve_reload = [&](ResolveReason reason) {
          command_processor_->RecordTextureResolveReload(reason, upload_bytes);
        };
        record_resolve_reload(ResolveReason::kCandidate);
        if (mips_outdated) {
          record_resolve_reload(ResolveReason::kMipsRequested);
        }
        if (texture_key.scaled_resolve) {
          record_resolve_reload(ResolveReason::kScaledResolve);
        }
        switch (texture->last_base_watch_resolve_source()) {
          case ResolveProvenanceSource::kDirectHost:
            record_resolve_reload(ResolveReason::kSourceDirectHost);
            break;
          case ResolveProvenanceSource::kRenderTarget:
            record_resolve_reload(ResolveReason::kSourceRenderTarget);
            break;
          case ResolveProvenanceSource::kUnknown:
          default:
            record_resolve_reload(ResolveReason::kSourceUnknown);
            break;
        }
        const uint32_t resolve_start =
            texture->last_base_watch_invalidation_range_start();
        const uint32_t resolve_length =
            texture->last_base_watch_invalidation_range_length();
        if (!resolve_length) {
          record_resolve_reload(ResolveReason::kNoProvenance);
        } else {
          const uint64_t texture_start = base_start;
          const uint64_t texture_end = texture_start + uint64_t(base_length);
          const uint64_t resolve_range_start = resolve_start;
          const uint64_t resolve_range_end =
              resolve_range_start + uint64_t(resolve_length);
          if (texture_start == resolve_range_start &&
              texture_end == resolve_range_end) {
            record_resolve_reload(ResolveReason::kExactRange);
          } else if (texture_start >= resolve_range_start &&
                     texture_end <= resolve_range_end) {
            record_resolve_reload(ResolveReason::kContainedRange);
          } else if (texture_start < resolve_range_end &&
                     texture_end > resolve_range_start) {
            record_resolve_reload(ResolveReason::kPartialOverlap);
          } else {
            record_resolve_reload(ResolveReason::kNoOverlap);
          }
        }
      }
      MetalCommandProcessor::TextureReloadReason range_reason =
          MetalCommandProcessor::TextureReloadReason::kPlannedBaseAndMips;
      if (base_outdated && !mips_outdated) {
        range_reason =
            MetalCommandProcessor::TextureReloadReason::kPlannedBaseOnly;
      } else if (!base_outdated && mips_outdated) {
        range_reason =
            MetalCommandProcessor::TextureReloadReason::kPlannedMipsOnly;
      }
      command_processor_->RecordTextureReloadReason(range_reason,
                                                    upload_bytes);
      command_processor_->RecordTextureReloadReason(
          use_cpu_source
              ? MetalCommandProcessor::TextureReloadReason::kPlannedCpuSource
              : MetalCommandProcessor::TextureReloadReason::
                    kPlannedResidentSource,
          upload_bytes);
      MetalTexture* metal_texture = static_cast<MetalTexture*>(texture);
      if (metal_texture->MarkMaterializationPlanned(
              command_processor_->GetCurrentTextureTelemetryFrame())) {
        command_processor_->RecordTextureReloadReason(
            MetalCommandProcessor::TextureReloadReason::
                kPlannedAgainSameFrame,
            upload_bytes);
      }
    }

    plan.texture_loads_.push_back({
        texture,
        base_outdated,
        mips_outdated,
        use_cpu_source,
    });
    ++plan.planned_load_count;
  };

  uint32_t remaining_bits = used_texture_mask;
  uint32_t index = 0;
  while (xe::bit_scan_forward(remaining_bits, &index)) {
    remaining_bits = xe::clear_lowest_bit(remaining_bits);

    TextureKey key;
    uint8_t swizzled_signs = kSwizzledSignsUnsigned;
    BindingInfoFromFetchConstant(regs.GetTextureFetch(index), key,
                                 &swizzled_signs);
    if (!key.is_valid) {
      continue;
    }

    if (IsSignedVersionSeparateForFormat(key)) {
      if (texture_util::IsAnySignNotSigned(swizzled_signs)) {
        append_texture(key);
      }
      if (texture_util::IsAnySignSigned(swizzled_signs)) {
        TextureKey signed_key = key;
        signed_key.signed_separate = 1;
        append_texture(signed_key);
      }
    } else {
      append_texture(key);
    }
  }

  return true;
}

void MetalTextureCache::RefreshTextureMaterializationPlan(
    TextureMaterializationPlan& plan) {
  TextureMaterializationPlan* plans[] = {&plan};
  RefreshTextureMaterializationPlans(plans, 1);
}

void MetalTextureCache::RefreshTextureMaterializationPlans(
    TextureMaterializationPlan* const* plans, uint32_t plan_count) {
  if (!plans || !plan_count) {
    return;
  }

  uint32_t total_load_count = 0;
  for (uint32_t i = 0; i < plan_count; ++i) {
    TextureMaterializationPlan* plan = plans[i];
    if (!plan) {
      continue;
    }
    total_load_count += static_cast<uint32_t>(plan->texture_loads_.size());
    plan->executed_load_count = 0;
  }
  if (!total_load_count) {
    return;
  }

  std::unordered_map<Texture*, TextureMaterializationPlan::TextureLoad*>
      live_loads;
  live_loads.reserve(total_load_count);
  std::vector<bool> plan_had_duplicate_prune(plan_count, false);
  uint64_t pruned_already_current = 0;
  uint64_t pruned_duplicate_same_flush = 0;

  {
    auto global_lock = AcquireGlobalLock();
    for (uint32_t i = 0; i < plan_count; ++i) {
      TextureMaterializationPlan* plan = plans[i];
      if (!plan) {
        continue;
      }
      for (TextureMaterializationPlan::TextureLoad& load :
           plan->texture_loads_) {
        Texture* texture = load.texture;
        if (!texture) {
          continue;
        }
        const bool planned_base = load.load_base;
        const bool planned_mips = load.load_mips;
        load.load_base = load.load_base && texture->base_outdated(global_lock);
        load.load_mips = load.load_mips && texture->mips_outdated(global_lock);
        if (!load.load_base && !load.load_mips) {
          if (command_processor_) {
            command_processor_->RecordTextureReloadReason(
                MetalCommandProcessor::TextureReloadReason::
                    kRefreshBecameCurrent,
                GetTextureLoadBytes(*texture, planned_base, planned_mips));
          }
          load.texture = nullptr;
          ++pruned_already_current;
          continue;
        }
        if (command_processor_) {
          command_processor_->RecordTextureReloadReason(
              MetalCommandProcessor::TextureReloadReason::kRefreshStillNeeded,
              GetTextureLoadBytes(*texture, load.load_base, load.load_mips));
        }

        auto [it, inserted] = live_loads.emplace(texture, &load);
        if (!inserted) {
          TextureMaterializationPlan::TextureLoad* existing_load = it->second;
          existing_load->load_base |= load.load_base;
          existing_load->load_mips |= load.load_mips;
          existing_load->use_cpu_source =
              existing_load->use_cpu_source && load.use_cpu_source;
          load.texture = nullptr;
          plan_had_duplicate_prune[i] = true;
          ++pruned_duplicate_same_flush;
        }
      }
    }
  }

  for (uint32_t i = 0; i < plan_count; ++i) {
    TextureMaterializationPlan* plan = plans[i];
    if (!plan) {
      continue;
    }
    auto& loads = plan->texture_loads_;
    size_t write_index = 0;
    for (size_t read_index = 0; read_index < loads.size(); ++read_index) {
      if (!loads[read_index].texture) {
        continue;
      }
      if (write_index != read_index) {
        loads[write_index] = loads[read_index];
      }
      ++write_index;
    }
    loads.resize(write_index);
    plan->planned_load_count = static_cast<uint32_t>(loads.size());
    if (loads.empty() && !plan_had_duplicate_prune[i]) {
      plan->source_ranges.clear();
    }
  }

  if (command_processor_) {
    command_processor_->RecordTextureUploadExecutionDetail(
        MetalCommandProcessor::TextureUploadExecutionDetail::
            kPrunedAlreadyCurrent,
        pruned_already_current);
    command_processor_->RecordTextureUploadExecutionDetail(
        MetalCommandProcessor::TextureUploadExecutionDetail::
            kPrunedDuplicateSameFlush,
        pruned_duplicate_same_flush);
  }
}

bool MetalTextureCache::ExecuteTextureMaterialization(
    TextureMaterializationPlan& plan) {
  if (plan.texture_loads_.empty()) {
    return true;
  }
  uint64_t load_calls_before = loaded_texture_data_count_;
  bool success = true;

  std::vector<Texture*> fallback_textures;
  fallback_textures.reserve(plan.texture_loads_.size());
  {
    UploadBatchScope upload_batch(*this);
    auto record_execution = [&](Texture& texture, bool load_base,
                                bool load_mips,
                                MetalCommandProcessor::TextureReloadReason
                                    source_reason) {
      if (!command_processor_) {
        return;
      }
      const uint64_t upload_bytes =
          GetTextureLoadBytes(texture, load_base, load_mips);
      command_processor_->RecordTextureReloadReason(source_reason,
                                                    upload_bytes);
      MetalTexture* metal_texture = static_cast<MetalTexture*>(&texture);
      if (metal_texture->MarkMaterializationExecuted(
              command_processor_->GetCurrentTextureTelemetryFrame())) {
        command_processor_->RecordTextureReloadReason(
            MetalCommandProcessor::TextureReloadReason::
                kExecuteAgainSameFrame,
            upload_bytes);
      }
    };
    for (const TextureMaterializationPlan::TextureLoad& load :
         plan.texture_loads_) {
      if (!load.texture) {
        continue;
      }
      if (!load.use_cpu_source) {
        record_execution(
            *load.texture, load.load_base, load.load_mips,
            MetalCommandProcessor::TextureReloadReason::kExecuteResidentSource);
        if (!load.texture->base_outdated_lockless() &&
            !load.texture->mips_outdated_lockless() && command_processor_) {
          command_processor_->RecordTextureUploadExecutionDetail(
              MetalCommandProcessor::TextureUploadExecutionDetail::
                  kFallbackAlreadyCurrentLockless);
        }
        fallback_textures.push_back(load.texture);
        continue;
      }
      record_execution(
          *load.texture, load.load_base, load.load_mips,
          MetalCommandProcessor::TextureReloadReason::kExecuteCpuSource);
      if (!LoadTextureDataFromCpuGuestMemory(*load.texture, load.load_base,
                                             load.load_mips)) {
        if (command_processor_) {
          command_processor_->RecordTextureUploadSourceFallback(
              MetalCommandProcessor::TextureUploadSourceFallbackReason::
                  kCpuSourceLoadFailed);
          command_processor_->RecordTextureReloadReason(
              MetalCommandProcessor::TextureReloadReason::
                  kExecuteResidentSource,
              GetTextureLoadBytes(*load.texture, load.load_base,
                                  load.load_mips));
        }
        fallback_textures.push_back(load.texture);
      }
    }

    if (!fallback_textures.empty()) {
      LoadTexturesData(fallback_textures.data(),
                       static_cast<uint32_t>(fallback_textures.size()));
    }
    success = upload_batch.End();
  }
  plan.executed_load_count =
      static_cast<uint32_t>(loaded_texture_data_count_ - load_calls_before);
  plan.texture_loads_.clear();
  if (!success) {
    ResetTextureBindings();
  }
  return success;
}

bool MetalTextureCache::IsDecompressionNeededForKey(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXN: {
      if (::cvars::metal_force_bc_decompress) {
        return true;
      }
      // BC support is GPU-family dependent on Apple GPUs. Creating BC textures
      // on a device that doesn't support them may trip Metal validation.
      if (!supports_bc_texture_compression_) {
        return true;
      }
      const FormatInfo* format_info = FormatInfo::Get(key.format);
      if (!format_info) {
        return false;
      }
      if (!(key.GetWidth() & (format_info->block_width - 1)) &&
          !(key.GetHeight() & (format_info->block_height - 1))) {
        return false;
      }
      return true;
    }
    case xenos::TextureFormat::k_CTX1:
      // CTX1 must be decompressed (no hardware support on Metal).
      return true;
    default:
      return false;
  }
}

TextureCache::LoadShaderIndex MetalTextureCache::GetLoadShaderIndexForKey(
    TextureKey key) const {
  bool decompress = IsDecompressionNeededForKey(key);
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
      return kLoadShaderIndex8bpb;
    case xenos::TextureFormat::k_8_8:
      return kLoadShaderIndex16bpb;
    case xenos::TextureFormat::k_1_5_5_5:
      return kLoadShaderIndexR5G5B5A1ToB5G5R5A1;
    case xenos::TextureFormat::k_5_6_5:
      return kLoadShaderIndexR5G6B5ToB5G6R5;
    case xenos::TextureFormat::k_6_5_5:
      return kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle;
    case xenos::TextureFormat::k_8_8_8_8:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_2_10_10_10:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_4_4_4_4:
      return kLoadShaderIndexRGBA4ToARGB4;
    case xenos::TextureFormat::k_10_11_11:
      return key.signed_separate ? kLoadShaderIndexR11G11B10ToRGBA16SNorm
                                 : kLoadShaderIndexR11G11B10ToRGBA16;
    case xenos::TextureFormat::k_11_11_10:
      return key.signed_separate ? kLoadShaderIndexR10G11B11ToRGBA16SNorm
                                 : kLoadShaderIndexR10G11B11ToRGBA16;

    case xenos::TextureFormat::k_DXT1:
      return decompress ? kLoadShaderIndexDXT1ToRGBA8 : kLoadShaderIndex64bpb;
    case xenos::TextureFormat::k_DXT2_3:
      return decompress ? kLoadShaderIndexDXT3ToRGBA8 : kLoadShaderIndex128bpb;
    case xenos::TextureFormat::k_DXT4_5:
      return decompress ? kLoadShaderIndexDXT5ToRGBA8 : kLoadShaderIndex128bpb;
    case xenos::TextureFormat::k_DXN:
      return decompress ? kLoadShaderIndexDXNToRG8 : kLoadShaderIndex128bpb;
    case xenos::TextureFormat::k_DXT3A:
      return kLoadShaderIndexDXT3A;
    case xenos::TextureFormat::k_DXT5A:
      return kLoadShaderIndexDXT5AToR8;
    case xenos::TextureFormat::k_DXT3A_AS_1_1_1_1:
      return kLoadShaderIndexDXT3AAs1111ToARGB4;
    case xenos::TextureFormat::k_CTX1:
      return kLoadShaderIndexCTX1;

    case xenos::TextureFormat::k_24_8:
      return kLoadShaderIndexDepthUnorm;
    case xenos::TextureFormat::k_24_8_FLOAT:
      return kLoadShaderIndexDepthFloat;

    case xenos::TextureFormat::k_16:
      if (key.signed_separate) {
        return r16_selection_.signed_uses_float
                   ? kLoadShaderIndexR16SNormToFloat
                   : kLoadShaderIndex16bpb;
      }
      return r16_selection_.unsigned_uses_float
                 ? kLoadShaderIndexR16UNormToFloat
                 : kLoadShaderIndex16bpb;
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
      return kLoadShaderIndex16bpb;
    case xenos::TextureFormat::k_16_16:
      if (key.signed_separate) {
        return rg16_selection_.signed_uses_float
                   ? kLoadShaderIndexRG16SNormToFloat
                   : kLoadShaderIndex32bpb;
      }
      return rg16_selection_.unsigned_uses_float
                 ? kLoadShaderIndexRG16UNormToFloat
                 : kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_16_16_16_16:
      if (key.signed_separate) {
        return rgba16_selection_.signed_uses_float
                   ? kLoadShaderIndexRGBA16SNormToFloat
                   : kLoadShaderIndex64bpb;
      }
      return rgba16_selection_.unsigned_uses_float
                 ? kLoadShaderIndexRGBA16UNormToFloat
                 : kLoadShaderIndex64bpb;
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return kLoadShaderIndex64bpb;

    case xenos::TextureFormat::k_32:
    case xenos::TextureFormat::k_32_FLOAT:
      return kLoadShaderIndex32bpb;
    case xenos::TextureFormat::k_32_32:
    case xenos::TextureFormat::k_32_32_FLOAT:
      return kLoadShaderIndex64bpb;
    case xenos::TextureFormat::k_32_32_32_32:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return kLoadShaderIndex128bpb;

    case xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP:
      // Metal has no native packed YUV format; always decompress to RGB8.
      return kLoadShaderIndexGBGR8ToRGB8;
    case xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP:
      return kLoadShaderIndexBGRG8ToRGB8;

    case xenos::TextureFormat::k_8_B:
      return kLoadShaderIndex8bpb;
    case xenos::TextureFormat::k_8_8_8_8_A:
      return kLoadShaderIndex32bpb;

    default:
      return kLoadShaderIndexUnknown;
  }
}

MTL::PixelFormat MetalTextureCache::GetPixelFormatForKey(TextureKey key) const {
  bool decompress = IsDecompressionNeededForKey(key);
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
      return MTL::PixelFormatR8Unorm;
    case xenos::TextureFormat::k_8_8:
      return MTL::PixelFormatRG8Unorm;
    case xenos::TextureFormat::k_1_5_5_5:
      // The R5G5B5A1ToB5G5R5A1 load shader outputs the layout D3D12 pairs
      // with DXGI_FORMAT_B5G5R5A1_UNORM and Vulkan with
      // VK_FORMAT_A1R5G5B5_UNORM_PACK16, which is BGR5A1Unorm, not
      // A1BGR5Unorm.
      return MTL::PixelFormatBGR5A1Unorm;
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_6_5_5:
      return MTL::PixelFormatB5G6R5Unorm;
    case xenos::TextureFormat::k_4_4_4_4:
      return MTL::PixelFormatABGR4Unorm;
    case xenos::TextureFormat::k_8_8_8_8:
      return MTL::PixelFormatRGBA8Unorm;
    case xenos::TextureFormat::k_2_10_10_10:
      return MTL::PixelFormatRGB10A2Unorm;
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      return key.signed_separate ? MTL::PixelFormatRGBA16Snorm
                                 : MTL::PixelFormatRGBA16Unorm;

    case xenos::TextureFormat::k_16:
      if (key.signed_separate) {
        return r16_selection_.signed_uses_float ? MTL::PixelFormatR16Float
                                                : MTL::PixelFormatR16Snorm;
      }
      return r16_selection_.unsigned_uses_float ? MTL::PixelFormatR16Float
                                                : MTL::PixelFormatR16Unorm;
    case xenos::TextureFormat::k_16_16:
      if (key.signed_separate) {
        return rg16_selection_.signed_uses_float ? MTL::PixelFormatRG16Float
                                                 : MTL::PixelFormatRG16Snorm;
      }
      return rg16_selection_.unsigned_uses_float ? MTL::PixelFormatRG16Float
                                                 : MTL::PixelFormatRG16Unorm;
    case xenos::TextureFormat::k_16_16_16_16:
      if (key.signed_separate) {
        return rgba16_selection_.signed_uses_float
                   ? MTL::PixelFormatRGBA16Float
                   : MTL::PixelFormatRGBA16Snorm;
      }
      return rgba16_selection_.unsigned_uses_float
                 ? MTL::PixelFormatRGBA16Float
                 : MTL::PixelFormatRGBA16Unorm;
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
      return MTL::PixelFormatR16Float;
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
      return MTL::PixelFormatRG16Float;
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return MTL::PixelFormatRGBA16Float;

    case xenos::TextureFormat::k_DXT1:
      return decompress ? MTL::PixelFormatRGBA8Unorm : MTL::PixelFormatBC1_RGBA;
    case xenos::TextureFormat::k_DXT2_3:
      return decompress ? MTL::PixelFormatRGBA8Unorm : MTL::PixelFormatBC2_RGBA;
    case xenos::TextureFormat::k_DXT4_5:
      return decompress ? MTL::PixelFormatRGBA8Unorm : MTL::PixelFormatBC3_RGBA;
    case xenos::TextureFormat::k_DXN:
      return decompress ? MTL::PixelFormatRG8Unorm
                        : MTL::PixelFormatBC5_RGUnorm;
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
      return MTL::PixelFormatR8Unorm;
    case xenos::TextureFormat::k_DXT3A_AS_1_1_1_1:
      return MTL::PixelFormatABGR4Unorm;
    case xenos::TextureFormat::k_CTX1:
      // CTX1 is always decoded via the texture load shader to RG8.
      return MTL::PixelFormatRG8Unorm;

    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return MTL::PixelFormatR32Float;

    case xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP:
    case xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP:
      // Decompressed from packed YUV to RGBA8 by the load shader.
      return MTL::PixelFormatRGBA8Unorm;

    case xenos::TextureFormat::k_8_B:
      return MTL::PixelFormatR8Unorm;
    case xenos::TextureFormat::k_8_8_8_8_A:
      return MTL::PixelFormatRGBA8Unorm;

    case xenos::TextureFormat::k_32_FLOAT:
      return MTL::PixelFormatR32Float;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return MTL::PixelFormatRG32Float;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return MTL::PixelFormatRGBA32Float;

    default:
      return MTL::PixelFormatInvalid;
  }
}

bool MetalTextureCache::TryGpuLoadTexture(Texture& texture, bool load_base,
                                          bool load_mips,
                                          TextureLoadSourceMode source_mode) {
  MetalTexture* metal_texture = static_cast<MetalTexture*>(&texture);
  if (!metal_texture || !metal_texture->metal_texture()) {
    return false;
  }
  const TextureKey& key = texture.key();
  bool texture_resolution_scaled =
      key.scaled_resolve && IsDrawResolutionScaled();
  uint32_t texture_resolution_scale_x =
      texture_resolution_scaled ? draw_resolution_scale_x() : 1;
  uint32_t texture_resolution_scale_y =
      texture_resolution_scaled ? draw_resolution_scale_y() : 1;
  uint32_t texture_resolution_scale_area =
      texture_resolution_scale_x * texture_resolution_scale_y;
  const bool use_cpu_guest_source =
      source_mode == TextureLoadSourceMode::kCpuGuestMemory;
  if (use_cpu_guest_source && texture_resolution_scaled) {
    return false;
  }

  const texture_util::TextureGuestLayout& guest_layout = texture.guest_layout();
  xenos::DataDimension dimension = key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || texture.force_load_3d_tiling();

  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;

  const FormatInfo* guest_format_info = FormatInfo::Get(key.format);
  if (!guest_format_info) {
    return false;
  }
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();

  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? key.mip_max_level : 0;
  if (level_first > level_last) {
    return false;
  }

  bool decompress = IsDecompressionNeededForKey(key);
  TextureCache::LoadShaderIndex load_shader = GetLoadShaderIndexForKey(key);
  if (load_shader == TextureCache::kLoadShaderIndexUnknown) {
    return false;
  }

  MTL::ComputePipelineState* pipeline =
      texture_resolution_scaled
          ? load_pipelines_scaled_[static_cast<size_t>(load_shader)]
          : load_pipelines_[static_cast<size_t>(load_shader)];
  if (!pipeline) {
    return false;
  }

  const TextureCache::LoadShaderInfo& load_shader_info =
      GetLoadShaderInfo(load_shader);

  bool is_block_compressed_format =
      key.format == xenos::TextureFormat::k_DXT1 ||
      key.format == xenos::TextureFormat::k_DXT2_3 ||
      key.format == xenos::TextureFormat::k_DXT4_5 ||
      key.format == xenos::TextureFormat::k_DXN;
  bool host_block_compressed = is_block_compressed_format && !decompress;
  uint32_t host_block_width = host_block_compressed ? block_width : 1;
  uint32_t host_block_height = host_block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread =
      UINT32_C(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_block_compressed) {
    host_x_blocks_per_thread *= block_width;
  }

  struct StoredLevelHostLayout {
    bool is_base;
    uint32_t level;
    uint32_t dest_offset_bytes;
    uint32_t slice_size_bytes;
    uint32_t row_pitch_bytes;
    uint32_t height_blocks;
    uint32_t depth_slices;
    uint32_t width_texels;
    uint32_t height_texels;
  };

  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);

  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  std::vector<StoredLevelHostLayout> stored_levels;
  stored_levels.reserve(loop_level_last - loop_level_first + 1);
  uint64_t dest_buffer_size = 0;

  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last;
       ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    if (!level_guest_layout.level_data_extent_bytes) {
      continue;
    }

    uint32_t level_width_unscaled, level_height_unscaled, level_depth;
    if (level == level_packed) {
      level_width_unscaled = level_guest_layout.x_extent_blocks * block_width;
      level_height_unscaled = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width_unscaled = std::max(width >> level, uint32_t(1));
      level_height_unscaled = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }

    uint32_t width_texels_scaled = xe::round_up(
        level_width_unscaled * texture_resolution_scale_x, host_block_width);
    uint32_t height_texels_scaled = xe::round_up(
        level_height_unscaled * texture_resolution_scale_y, host_block_height);
    uint32_t width_blocks = width_texels_scaled / host_block_width;
    uint32_t height_blocks = height_texels_scaled / host_block_height;

    const uint32_t row_pitch_alignment =
        ShouldUploadViaBlit() ? uint32_t(256) : uint32_t(16);
    uint32_t row_pitch_bytes =
        xe::align(xe::round_up(width_blocks, host_x_blocks_per_thread) *
                      load_shader_info.bytes_per_host_block,
                  row_pitch_alignment);
    uint32_t slice_size_bytes = xe::align(
        row_pitch_bytes * height_blocks * level_depth, row_pitch_alignment);

    StoredLevelHostLayout host_layout = {};
    host_layout.is_base = is_base;
    host_layout.level = level;
    host_layout.dest_offset_bytes = uint32_t(dest_buffer_size);
    host_layout.slice_size_bytes = slice_size_bytes;
    host_layout.row_pitch_bytes = row_pitch_bytes;
    host_layout.height_blocks = height_blocks;
    host_layout.depth_slices = level_depth;
    host_layout.width_texels = level_width_unscaled;
    host_layout.height_texels = level_height_unscaled;
    stored_levels.push_back(host_layout);

    dest_buffer_size += uint64_t(slice_size_bytes) * uint64_t(array_size);
  }

  if (stored_levels.empty()) {
    return false;
  }
  if (dest_buffer_size > SIZE_MAX) {
    return false;
  }

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    return false;
  }
  if (!command_processor_->GetMetalCommandQueue()) {
    return false;
  }

  MTL::Buffer* shared_buffer = nullptr;
  if (!use_cpu_guest_source) {
    MetalSharedMemory& metal_shared_memory =
        static_cast<MetalSharedMemory&>(shared_memory());
    shared_buffer = metal_shared_memory.GetBuffer();
    if (!shared_buffer) {
      return false;
    }
  }

  std::shared_ptr<UploadBufferPool> buffer_pool;
  {
    std::lock_guard<std::mutex> lock(upload_buffer_pool_mutex_);
    buffer_pool = upload_buffer_pool_;
  }
  auto acquire_buffer = [&](size_t size) -> MTL::Buffer* {
    if (buffer_pool) {
      return buffer_pool->Acquire(size);
    }
    MTL::Buffer* buffer =
        device->newBuffer(size, MTL::ResourceStorageModeShared);
    if (buffer) {
    }
    return buffer;
  };
  auto release_buffer_immediate = [&](MTL::Buffer* buffer, size_t size) {
    if (!buffer) {
      return;
    }
    if (buffer_pool) {
      buffer_pool->ReleaseImmediate(buffer);
      return;
    }
    buffer->release();
  };
  auto release_buffer_after = [&](MTL::CommandBuffer* cmd, MTL::Buffer* buffer,
                                  size_t size) {
    if (!buffer) {
      return;
    }
    if (buffer_pool) {
      buffer_pool->ReleaseAfter(cmd, buffer);
      return;
    }
    cmd->addCompletedHandler(^(MTL::CommandBuffer*) {
      buffer->release();
    });
  };

  MTL::Buffer* dest_buffer = acquire_buffer(size_t(dest_buffer_size));
  if (!dest_buffer) {
    return false;
  }

  uint32_t base_guest_address = key.base_page << 12;
  uint32_t mips_guest_address = key.mip_page << 12;

  size_t constants_size = xe::align(sizeof(MetalLoadConstants), size_t(16));
  size_t dispatch_count = stored_levels.size() * size_t(array_size);
  size_t constants_buffer_size = constants_size * dispatch_count;
  MTL::Buffer* constants_buffer = acquire_buffer(constants_buffer_size);
  if (!constants_buffer) {
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
    return false;
  }

  const bool use_blit_upload = ShouldUploadViaBlit();
  MetalCommandProcessor::SharedMemoryReadDependency shared_memory_dependency;
  MetalCommandProcessor::SharedMemoryRange shared_memory_ranges[2] = {};
  uint32_t shared_memory_range_count = 0;
  if (!use_cpu_guest_source && !texture_resolution_scaled &&
      command_processor_) {
    if (load_base) {
      shared_memory_ranges[shared_memory_range_count++] = {
          base_guest_address,
          xe::align(texture.GetGuestBaseSize(), UINT32_C(16)),
      };
    }
    if (load_mips) {
      shared_memory_ranges[shared_memory_range_count++] = {
          mips_guest_address,
          xe::align(texture.GetGuestMipsSize(), UINT32_C(16)),
      };
    }
    if (shared_memory_range_count &&
        !command_processor_->PrepareSharedMemoryComputeReadDependency(
            shared_memory_ranges, shared_memory_range_count, use_blit_upload,
            &shared_memory_dependency)) {
      release_buffer_immediate(constants_buffer, constants_buffer_size);
      release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
      return false;
    }
  }

  auto find_stored_level =
      [&](bool is_base_storage,
          uint32_t stored_level) -> const StoredLevelHostLayout* {
    for (const StoredLevelHostLayout& layout : stored_levels) {
      if (layout.is_base == is_base_storage && layout.level == stored_level) {
        return &layout;
      }
    }
    return nullptr;
  };

  bool has_active_submission =
      command_processor_ && command_processor_->HasActiveSubmission();
  bool can_join_active_submission =
      command_processor_ &&
      command_processor_->CanJoinActiveSubmissionForTransfer();
  bool use_current_command_buffer =
      use_blit_upload && can_join_active_submission;
  bool use_upload_batch = use_blit_upload && upload_batch_command_buffer_ &&
                          command_processor_ && !has_active_submission;
  if (use_upload_batch && texture_resolution_scaled) {
    bool needs_base_scaled_range = false;
    bool needs_mips_scaled_range = false;
    for (const StoredLevelHostLayout& stored_level : stored_levels) {
      if (stored_level.is_base) {
        needs_base_scaled_range = true;
      } else {
        needs_mips_scaled_range = true;
      }
    }
    if (needs_base_scaled_range &&
        !IsScaledResolveRangeResident(base_guest_address,
                                      texture.GetGuestBaseSize(), 4)) {
      use_upload_batch = false;
    }
    if (use_upload_batch && needs_mips_scaled_range &&
        !IsScaledResolveRangeResident(mips_guest_address,
                                      texture.GetGuestMipsSize(), 4)) {
      use_upload_batch = false;
    }
  }

  enum class UploadCommandBufferMode {
    kStandalone,
    kUploadBatch,
    kCurrentSubmission,
  };

  ScopedAutoreleasePool autorelease_pool;
  MTL::CommandBuffer* cmd = nullptr;
  UploadCommandBufferMode upload_command_buffer_mode =
      UploadCommandBufferMode::kStandalone;
  if (use_upload_batch) {
    cmd = upload_batch_command_buffer_;
    upload_command_buffer_mode = UploadCommandBufferMode::kUploadBatch;
  } else if (use_current_command_buffer) {
    cmd = command_processor_->GetCurrentCommandBuffer();
    upload_command_buffer_mode = UploadCommandBufferMode::kCurrentSubmission;
  } else {
    cmd = command_processor_->CreateStandaloneTransferCommandBuffer(
        "XeniaCB reason=texture-upload");
  }
  if (!cmd) {
    release_buffer_immediate(constants_buffer, constants_buffer_size);
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
    return false;
  }
  bool command_buffer_has_work = false;
  auto is_current_submission = [&]() {
    return upload_command_buffer_mode ==
           UploadCommandBufferMode::kCurrentSubmission;
  };
  auto is_upload_batch = [&]() {
    return upload_command_buffer_mode == UploadCommandBufferMode::kUploadBatch;
  };
  auto is_standalone = [&]() {
    return upload_command_buffer_mode == UploadCommandBufferMode::kStandalone;
  };
  if (!use_cpu_guest_source && shared_memory_range_count &&
      is_current_submission()) {
    uint64_t submission = command_processor_->GetCurrentSubmission();
    MetalSharedMemory& metal_shared_memory =
        static_cast<MetalSharedMemory&>(shared_memory());
    for (uint32_t i = 0; i < shared_memory_range_count; ++i) {
      metal_shared_memory.MarkGpuAccess(shared_memory_ranges[i].start,
                                        shared_memory_ranges[i].length,
                                        submission);
    }
  }
  struct TransientTextureSourceRange {
    bool valid = false;
    uint32_t guest_start = 0;
    uint32_t guest_length = 0;
    uint32_t copy_start = 0;
    uint32_t copy_length = 0;
    size_t buffer_length = 0;
    size_t guest_buffer_offset = 0;
    size_t buffer_offset = 0;
  };
  MTL::Buffer* transient_source_buffer = nullptr;
  size_t transient_source_size = 0;
  TransientTextureSourceRange transient_base_source;
  TransientTextureSourceRange transient_mips_source;
  bool using_deferred_upload_encoder = false;
  auto handle_upload_failure = [&](bool abort_batch) {
    if ((is_upload_batch() || is_current_submission()) &&
        command_buffer_has_work) {
      release_buffer_after(cmd, transient_source_buffer, transient_source_size);
      transient_source_buffer = nullptr;
      release_buffer_after(cmd, constants_buffer, constants_buffer_size);
      release_buffer_after(cmd, dest_buffer, size_t(dest_buffer_size));
      if (is_upload_batch()) {
        upload_batch_command_buffer_has_work_ = true;
        if (!deferred_upload_batch_depth_) {
          AbortUploadCommandBufferBatch();
        }
      }
      return;
    }
    release_buffer_immediate(transient_source_buffer, transient_source_size);
    transient_source_buffer = nullptr;
    release_buffer_immediate(constants_buffer, constants_buffer_size);
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
    if (is_upload_batch() && abort_batch && !deferred_upload_batch_depth_) {
      AbortUploadCommandBufferBatch();
    }
    if (is_standalone()) {
      cmd->release();
    }
  };

  if (use_cpu_guest_source) {
    auto append_transient_source = [&](bool needed, uint32_t guest_start,
                                       uint32_t guest_length,
                                       TransientTextureSourceRange& range) {
      if (!needed || !guest_length) {
        return true;
      }
      if (!shared_memory().IsRangeInvalid(guest_start, guest_length)) {
        return false;
      }
      constexpr uint32_t kSharedMemoryPageSize = UINT32_C(4096);
      constexpr uint32_t kSharedMemoryPageMask = kSharedMemoryPageSize - 1;
      static_assert((kSharedMemoryPageSize & kSharedMemoryPageMask) == 0);
      if (guest_start >= SharedMemory::kBufferSize) {
        return false;
      }
      uint64_t guest_end = uint64_t(guest_start) + guest_length;
      if (guest_end > SharedMemory::kBufferSize) {
        return false;
      }
      uint32_t copy_start = guest_start & ~kSharedMemoryPageMask;
      uint32_t copy_end = static_cast<uint32_t>(
          xe::align(guest_end, uint64_t(kSharedMemoryPageSize)));
      copy_end = std::min(copy_end, SharedMemory::kBufferSize);
      if (copy_end <= copy_start) {
        return false;
      }
      size_t buffer_offset = xe::align(transient_source_size, size_t(16));
      // The texture load shaders intentionally vectorize/local-X over the
      // nominal source extent. The resident path reads from the full shared
      // memory buffer; give the transient source a clean guard so those reads
      // don't observe recycled upload-buffer contents or run past the buffer.
      constexpr size_t kTextureLoadSourceGuardBytes = kSharedMemoryPageSize;
      size_t copy_length = size_t(copy_end - copy_start);
      size_t buffer_length = copy_length + kTextureLoadSourceGuardBytes;
      if (buffer_offset > SIZE_MAX - buffer_length) {
        return false;
      }
      range.valid = true;
      range.guest_start = guest_start;
      range.guest_length = guest_length;
      range.copy_start = copy_start;
      range.copy_length = static_cast<uint32_t>(copy_length);
      range.buffer_length = buffer_length;
      range.buffer_offset = buffer_offset;
      range.guest_buffer_offset =
          buffer_offset + size_t(guest_start - copy_start);
      transient_source_size = buffer_offset + buffer_length;
      return true;
    };
    if (!append_transient_source(
            load_base, base_guest_address,
            xe::align(texture.GetGuestBaseSize(), UINT32_C(16)),
            transient_base_source) ||
        !append_transient_source(
            load_mips, mips_guest_address,
            xe::align(texture.GetGuestMipsSize(), UINT32_C(16)),
            transient_mips_source) ||
        !transient_source_size) {
      handle_upload_failure(true);
      return false;
    }

    transient_source_buffer = acquire_buffer(transient_source_size);
    if (!transient_source_buffer || !transient_source_buffer->contents()) {
      handle_upload_failure(true);
      return false;
    }
    const uint8_t* xbox_ram =
        static_cast<MetalSharedMemory&>(shared_memory()).GetXboxRamBase();
    if (!xbox_ram) {
      handle_upload_failure(true);
      return false;
    }
    uint8_t* source_data =
        static_cast<uint8_t*>(transient_source_buffer->contents());
    auto copy_transient_source =
        [&](const TransientTextureSourceRange& range) {
          if (!range.valid) {
            return;
          }
          std::memcpy(source_data + range.buffer_offset,
                      xbox_ram + range.copy_start, range.copy_length);
          std::memset(source_data + range.buffer_offset + range.copy_length, 0,
                      range.buffer_length - range.copy_length);
        };
    copy_transient_source(transient_base_source);
    copy_transient_source(transient_mips_source);
  }

  struct PendingDirectUpload {
    size_t source_offset_bytes = 0;
    size_t source_row_pitch = 0;
    size_t source_bytes_per_image = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t destination_slice = 0;
    uint32_t destination_level = 0;
  };
  struct PendingRepackUpload {
    MTL::Buffer* staging_buffer = nullptr;
    size_t staging_size = 0;
    MetalTextureUploadRepackConstants constants = {};
    uint32_t blit_row_pitch = 0;
    size_t blit_bytes_per_image = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t destination_slice = 0;
    uint32_t destination_level = 0;
  };
  std::vector<PendingDirectUpload> direct_uploads;
  std::vector<PendingRepackUpload> repack_uploads;

  auto release_repack_staging_immediate = [&]() {
    for (PendingRepackUpload& upload : repack_uploads) {
      if (!upload.staging_buffer) {
        continue;
      }
      release_buffer_immediate(upload.staging_buffer, upload.staging_size);
      upload.staging_buffer = nullptr;
    }
  };
  auto release_repack_staging_after = [&]() {
    for (PendingRepackUpload& upload : repack_uploads) {
      if (!upload.staging_buffer) {
        continue;
      }
      release_buffer_after(cmd, upload.staging_buffer, upload.staging_size);
      upload.staging_buffer = nullptr;
    }
  };
  auto release_repack_staging_for_encoded_failure = [&]() {
    if ((is_upload_batch() || is_current_submission()) &&
        command_buffer_has_work) {
      release_repack_staging_after();
    } else {
      release_repack_staging_immediate();
    }
  };

  if (use_blit_upload) {
    uint32_t bytes_per_host_block = load_shader_info.bytes_per_host_block;
    const uint32_t blit_alignment = 256;

    direct_uploads.reserve((level_last - level_first + 1) * array_size);
    repack_uploads.reserve((level_last - level_first + 1) * array_size);

    auto fits_u32 = [](size_t value) {
      return value <= size_t(std::numeric_limits<uint32_t>::max());
    };

    for (uint32_t level = level_first; level <= level_last; ++level) {
      uint32_t stored_level = std::min(level, level_packed);
      bool is_base_storage =
          stored_level == 0 && (level_packed != 0 || level == 0);
      const StoredLevelHostLayout* stored_layout =
          find_stored_level(is_base_storage, stored_level);
      if (!stored_layout) {
        continue;
      }

      uint32_t level_width_unscaled = std::max(width >> level, uint32_t(1));
      uint32_t level_height_unscaled = std::max(height >> level, uint32_t(1));
      uint32_t level_depth = std::max(depth >> level, uint32_t(1));
      uint32_t level_width_scaled =
          level_width_unscaled * texture_resolution_scale_x;
      uint32_t level_height_scaled =
          level_height_unscaled * texture_resolution_scale_y;

      const uint32_t upload_width_texels = level_width_scaled;
      const uint32_t upload_height_texels = level_height_scaled;

      uint32_t packed_offset_blocks_x = 0;
      uint32_t packed_offset_blocks_y = 0;
      uint32_t packed_offset_z = 0;
      if (level >= level_packed) {
        texture_util::GetPackedMipOffset(
            width, height, depth, key.format, level, packed_offset_blocks_x,
            packed_offset_blocks_y, packed_offset_z);
      }

      uint32_t upload_blocks_x =
          xe::round_up(upload_width_texels, host_block_width) /
          host_block_width;
      uint32_t upload_blocks_y =
          xe::round_up(upload_height_texels, host_block_height) /
          host_block_height;
      uint32_t upload_row_bytes = upload_blocks_x * bytes_per_host_block;
      uint32_t upload_row_count = upload_blocks_y;
      uint32_t blit_row_pitch = xe::align(upload_row_bytes, blit_alignment);
      size_t blit_bytes_per_image = size_t(blit_row_pitch) * upload_row_count;
      size_t bytes_per_image =
          size_t(stored_layout->row_pitch_bytes) * stored_layout->height_blocks;

      for (uint32_t slice = 0; slice < array_size; ++slice) {
        size_t source_offset_bytes = stored_layout->dest_offset_bytes +
                                     slice * stored_layout->slice_size_bytes;
        if (level >= level_packed) {
          if (host_block_compressed) {
            uint32_t packed_offset_blocks_x_scaled =
                packed_offset_blocks_x * texture_resolution_scale_x;
            uint32_t packed_offset_blocks_y_scaled =
                packed_offset_blocks_y * texture_resolution_scale_y;
            source_offset_bytes += packed_offset_z * bytes_per_image;
            source_offset_bytes +=
                packed_offset_blocks_y_scaled * stored_layout->row_pitch_bytes;
            source_offset_bytes +=
                packed_offset_blocks_x_scaled * bytes_per_block;
          } else {
            uint32_t packed_offset_texels_x =
                packed_offset_blocks_x * block_width;
            uint32_t packed_offset_texels_y =
                packed_offset_blocks_y * block_height;
            packed_offset_texels_x *= texture_resolution_scale_x;
            packed_offset_texels_y *= texture_resolution_scale_y;
            source_offset_bytes += packed_offset_z * bytes_per_image;
            source_offset_bytes +=
                packed_offset_texels_y * stored_layout->row_pitch_bytes;
            source_offset_bytes +=
                packed_offset_texels_x * bytes_per_host_block;
          }
        }

        bool requires_staging = (source_offset_bytes % blit_alignment) != 0;
        if (requires_staging) {
          if (!texture_upload_repack_pipeline_ ||
              (level_depth != 0 &&
               blit_bytes_per_image >
                   std::numeric_limits<size_t>::max() / level_depth)) {
            release_repack_staging_immediate();
            handle_upload_failure(true);
            return false;
          }
          size_t staging_size = blit_bytes_per_image * level_depth;
          if (!fits_u32(source_offset_bytes) || !fits_u32(bytes_per_image) ||
              !fits_u32(blit_bytes_per_image) || !fits_u32(staging_size)) {
            release_repack_staging_immediate();
            handle_upload_failure(true);
            return false;
          }
          MTL::Buffer* staging_buffer = acquire_buffer(staging_size);
          if (!staging_buffer) {
            release_repack_staging_immediate();
            handle_upload_failure(true);
            return false;
          }

          PendingRepackUpload upload = {};
          upload.staging_buffer = staging_buffer;
          upload.staging_size = staging_size;
          upload.constants.source_offset = uint32_t(source_offset_bytes);
          upload.constants.dest_offset = 0;
          upload.constants.source_row_pitch = stored_layout->row_pitch_bytes;
          upload.constants.dest_row_pitch = blit_row_pitch;
          upload.constants.source_image_pitch = uint32_t(bytes_per_image);
          upload.constants.dest_image_pitch = uint32_t(blit_bytes_per_image);
          upload.constants.row_bytes = upload_row_bytes;
          upload.constants.row_count = upload_row_count;
          upload.constants.depth = level_depth;
          upload.blit_row_pitch = blit_row_pitch;
          upload.blit_bytes_per_image = blit_bytes_per_image;
          upload.width = upload_width_texels;
          upload.height = upload_height_texels;
          upload.depth = level_depth;
          upload.destination_slice = is_3d ? 0 : slice;
          upload.destination_level = level;
          repack_uploads.push_back(upload);
        } else {
          PendingDirectUpload upload = {};
          upload.source_offset_bytes = source_offset_bytes;
          upload.source_row_pitch = stored_layout->row_pitch_bytes;
          upload.source_bytes_per_image = bytes_per_image;
          upload.width = upload_width_texels;
          upload.height = upload_height_texels;
          upload.depth = level_depth;
          upload.destination_slice = is_3d ? 0 : slice;
          upload.destination_level = level;
          direct_uploads.push_back(upload);
        }
      }
    }
  }

  if (command_processor_) {
    using TextureUploadClass =
        MetalCommandProcessor::TextureUploadCompatibilityClass;
    using TextureUploadBlocker =
        MetalCommandProcessor::TextureUploadComputeBlocker;
    const uint64_t upload_bytes =
        (load_base ? uint64_t(xe::align(texture.GetGuestBaseSize(),
                                        UINT32_C(16)))
                   : 0) +
        (load_mips ? uint64_t(xe::align(texture.GetGuestMipsSize(),
                                        UINT32_C(16)))
                   : 0);
    auto is_raw_copy_load_shader = [](TextureCache::LoadShaderIndex shader) {
      switch (shader) {
        case TextureCache::kLoadShaderIndex8bpb:
        case TextureCache::kLoadShaderIndex16bpb:
        case TextureCache::kLoadShaderIndex32bpb:
        case TextureCache::kLoadShaderIndex64bpb:
        case TextureCache::kLoadShaderIndex128bpb:
          return true;
        default:
          return false;
      }
    };
    bool has_compute_blocker = false;
    auto record_blocker = [&](TextureUploadBlocker blocker) {
      has_compute_blocker = true;
      command_processor_->RecordTextureUploadComputeBlocker(blocker,
                                                            upload_bytes);
    };
    if (key.tiled) {
      record_blocker(TextureUploadBlocker::kTiled);
    }
    if (is_3d_tiling) {
      record_blocker(TextureUploadBlocker::kThreeDimensionalTiling);
    }
    if (key.endianness != xenos::Endian::kNone) {
      record_blocker(TextureUploadBlocker::kEndianSwap);
    }
    if (decompress) {
      record_blocker(TextureUploadBlocker::kBcDecompress);
    } else if (!is_raw_copy_load_shader(load_shader)) {
      record_blocker(TextureUploadBlocker::kFormatConversion);
    }
    if (texture_resolution_scaled) {
      record_blocker(TextureUploadBlocker::kScaledResolve);
    }
    if (key.packed_mips && load_mips) {
      record_blocker(TextureUploadBlocker::kPackedMips);
    }
    if (!repack_uploads.empty()) {
      record_blocker(TextureUploadBlocker::kRepackAlignment);
    }
    if (!has_compute_blocker && !is_raw_copy_load_shader(load_shader)) {
      record_blocker(TextureUploadBlocker::kUnknown);
    }
    command_processor_->RecordTextureUploadCompatibility(
        has_compute_blocker ? TextureUploadClass::kComputeRequired
                            : TextureUploadClass::kDirectCopyCandidate,
        upload_bytes);
  }

  const bool can_defer_upload_blits =
      use_blit_upload && (is_upload_batch() || is_current_submission()) &&
      deferred_upload_batch_depth_ != 0;
  MTL::ComputeCommandEncoder* encoder = nullptr;
  if (can_defer_upload_blits) {
    encoder = GetDeferredUploadComputeEncoder(cmd);
    using_deferred_upload_encoder = encoder != nullptr;
  }
  if (!encoder) {
    if (command_processor_ &&
        cmd == command_processor_->GetCurrentCommandBuffer()) {
      command_processor_->EndSharedMemoryUploadBlitEncoder(
          MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
              kTextureCompute);
    }
    encoder = cmd->computeCommandEncoder();
  }
  if (!encoder) {
    release_repack_staging_immediate();
    handle_upload_failure(true);
    return false;
  }
  if (!using_deferred_upload_encoder) {
    SetEncoderLabel(encoder, repack_uploads.empty()
                                 ? "XeniaTextureLoadEncoder"
                                 : "XeniaTextureLoadRepackEncoder");
  }
  bool compute_encoder_open = true;
  auto end_local_compute_encoder = [&]() {
    if (!compute_encoder_open || using_deferred_upload_encoder) {
      return;
    }
    encoder->endEncoding();
    compute_encoder_open = false;
  };
  if (command_processor_ &&
      !command_processor_->EncodeSharedMemoryComputeReadDependency(
          encoder, shared_memory_dependency, shared_memory_ranges,
          shared_memory_range_count)) {
    end_local_compute_encoder();
    release_repack_staging_immediate();
    handle_upload_failure(true);
    return false;
  }
  encoder->setComputePipelineState(pipeline);
  MTL::Buffer* shader_source_buffer =
      use_cpu_guest_source ? transient_source_buffer : shared_buffer;
  if (!texture_resolution_scaled) {
    encoder->setBuffer(shader_source_buffer, 0, 2);
  }

  uint32_t guest_x_blocks_per_group_log2 =
      load_shader_info.GetGuestXBlocksPerGroupLog2();
  MTL::Size threads_per_group =
      MTL::Size::Make(UINT32_C(1) << kLoadGuestXThreadsPerGroupLog2,
                      UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2, 1);

  bool scaled_mips_source_set_up = false;
  MTL::Buffer* source_buffer = shader_source_buffer;
  size_t source_buffer_offset = 0;
  size_t source_buffer_length = 0;

  size_t dispatch_index = 0;
  for (const StoredLevelHostLayout& stored_level : stored_levels) {
    bool is_base_storage = stored_level.is_base;
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base_storage ? guest_layout.base
                        : guest_layout.mips[stored_level.level];

    if (texture_resolution_scaled &&
        (is_base_storage || !scaled_mips_source_set_up)) {
      uint32_t guest_address =
          is_base_storage ? base_guest_address : mips_guest_address;
      uint32_t guest_size_unscaled = is_base_storage
                                         ? texture.GetGuestBaseSize()
                                         : texture.GetGuestMipsSize();
      if (!MakeScaledResolveRangeCurrent(guest_address, guest_size_unscaled,
                                         4) ||
          !GetCurrentScaledResolveBuffer(source_buffer, source_buffer_offset,
                                         source_buffer_length)) {
        end_local_compute_encoder();
        release_repack_staging_for_encoded_failure();
        handle_upload_failure(false);
        return false;
      }
      encoder->setBuffer(source_buffer, source_buffer_offset, 2);
      if (!is_base_storage) {
        scaled_mips_source_set_up = true;
      }
    }

    uint32_t level_guest_offset = 0;
    if (!texture_resolution_scaled) {
      if (use_cpu_guest_source) {
        const TransientTextureSourceRange& source_range =
            is_base_storage ? transient_base_source : transient_mips_source;
        if (!source_range.valid ||
            source_range.guest_buffer_offset >
                size_t(std::numeric_limits<uint32_t>::max())) {
          end_local_compute_encoder();
          release_repack_staging_for_encoded_failure();
          handle_upload_failure(false);
          return false;
        }
        level_guest_offset = uint32_t(source_range.guest_buffer_offset);
      } else {
        level_guest_offset =
            is_base_storage ? base_guest_address : mips_guest_address;
      }
    }
    if (!is_base_storage) {
      uint32_t mip_offset = guest_layout.mip_offsets_bytes[stored_level.level];
      if (texture_resolution_scaled) {
        mip_offset *= texture_resolution_scale_area;
      }
      if (mip_offset >
          std::numeric_limits<uint32_t>::max() - level_guest_offset) {
        end_local_compute_encoder();
        release_repack_staging_for_encoded_failure();
        handle_upload_failure(false);
        return false;
      }
      level_guest_offset += mip_offset;
    }
    // Use guest layout pitch (blocks) - new XeSL expects blocks for both tiled
    // and linear
    uint32_t guest_pitch_aligned =
        level_guest_layout.row_pitch_bytes / bytes_per_block;

    uint32_t size_blocks_x =
        (stored_level.width_texels + (block_width - 1)) / block_width;
    uint32_t size_blocks_y =
        (stored_level.height_texels + (block_height - 1)) / block_height;
    size_blocks_x *= texture_resolution_scale_x;
    size_blocks_y *= texture_resolution_scale_y;

    uint32_t group_count_x =
        (size_blocks_x +
         ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (size_blocks_y +
         ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;
    MTL::Size threadgroups = MTL::Size::Make(group_count_x, group_count_y,
                                             stored_level.depth_slices);

    for (uint32_t slice = 0; slice < array_size; ++slice) {
      MetalLoadConstants constants = {};
      constants.is_tiled_3d_endian_scale =
          uint32_t(key.tiled) | (uint32_t(is_3d_tiling) << 1) |
          (uint32_t(key.endianness) << 2) | (texture_resolution_scale_x << 4) |
          (texture_resolution_scale_y << 7);
      constants.guest_offset = level_guest_offset;
      if (!is_3d) {
        uint32_t slice_stride = level_guest_layout.array_slice_stride_bytes;
        if (texture_resolution_scaled) {
          slice_stride *= texture_resolution_scale_area;
        }
        constants.guest_offset += slice * slice_stride;
      }
      constants.guest_pitch_aligned = guest_pitch_aligned;
      constants.guest_z_stride_block_rows_aligned =
          level_guest_layout.z_slice_stride_block_rows;
      constants.size_blocks[0] = size_blocks_x;
      constants.size_blocks[1] = size_blocks_y;
      constants.size_blocks[2] = stored_level.depth_slices;
      constants.padding0 = 0;
      constants.host_offset = 0;
      constants.host_pitch = stored_level.row_pitch_bytes;
      constants.height_texels = stored_level.height_texels;

      uint8_t* constants_ptr =
          static_cast<uint8_t*>(constants_buffer->contents()) +
          dispatch_index * constants_size;
      std::memcpy(constants_ptr, &constants, sizeof(constants));

      encoder->setBuffer(constants_buffer, dispatch_index * constants_size, 0);
      encoder->setBuffer(dest_buffer,
                         stored_level.dest_offset_bytes +
                             slice * stored_level.slice_size_bytes,
                         1);
      encoder->dispatchThreadgroups(threadgroups, threads_per_group);
      command_buffer_has_work = true;
      ++dispatch_index;
    }
  }

  if (!repack_uploads.empty()) {
    encoder->memoryBarrier(MTL::BarrierScopeBuffers);
    encoder->pushDebugGroup(
        NS::String::string("XeniaTextureUploadRepack", NS::UTF8StringEncoding));
    encoder->setComputePipelineState(texture_upload_repack_pipeline_);
    encoder->setBuffer(dest_buffer, 0, 1);
    MTL::Size repack_threads_per_group = MTL::Size::Make(128, 1, 1);
    for (const PendingRepackUpload& upload : repack_uploads) {
      uint32_t dword_count = (upload.constants.row_bytes + 3) >> 2;
      uint32_t group_count_x = (dword_count + 127) >> 7;
      encoder->setBytes(&upload.constants, sizeof(upload.constants), 0);
      encoder->setBuffer(upload.staging_buffer, 0, 2);
      encoder->dispatchThreadgroups(
          MTL::Size::Make(group_count_x, upload.constants.row_count,
                          upload.constants.depth),
          repack_threads_per_group);
      command_buffer_has_work = true;
    }
    encoder->popDebugGroup();
  }

  end_local_compute_encoder();

  MTL::Texture* mtl_texture = metal_texture->metal_texture();
  if (use_blit_upload) {
    if (!direct_uploads.empty() || !repack_uploads.empty()) {
      if (using_deferred_upload_encoder) {
        for (const PendingDirectUpload& upload : direct_uploads) {
          QueueDeferredUploadCopy(
              dest_buffer, upload.source_offset_bytes, upload.source_row_pitch,
              upload.source_bytes_per_image, mtl_texture,
              upload.destination_slice, upload.destination_level, upload.width,
              upload.height, upload.depth);
          command_buffer_has_work = true;
        }
        for (const PendingRepackUpload& upload : repack_uploads) {
          QueueDeferredUploadCopy(
              upload.staging_buffer, 0, upload.blit_row_pitch,
              upload.blit_bytes_per_image, mtl_texture,
              upload.destination_slice, upload.destination_level, upload.width,
              upload.height, upload.depth);
          command_buffer_has_work = true;
        }
      } else {
        if (command_processor_ &&
            cmd == command_processor_->GetCurrentCommandBuffer()) {
          command_processor_->EndSharedMemoryUploadBlitEncoder(
              MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
                  kTextureBlit);
        }
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        if (!blit) {
          release_repack_staging_for_encoded_failure();
          handle_upload_failure(true);
          return false;
        }

        for (const PendingDirectUpload& upload : direct_uploads) {
          blit->copyFromBuffer(
              dest_buffer, upload.source_offset_bytes, upload.source_row_pitch,
              upload.source_bytes_per_image,
              MTL::Size::Make(upload.width, upload.height, upload.depth),
              mtl_texture, upload.destination_slice, upload.destination_level,
              MTL::Origin::Make(0, 0, 0));
          command_buffer_has_work = true;
        }
        for (const PendingRepackUpload& upload : repack_uploads) {
          blit->copyFromBuffer(
              upload.staging_buffer, 0, upload.blit_row_pitch,
              upload.blit_bytes_per_image,
              MTL::Size::Make(upload.width, upload.height, upload.depth),
              mtl_texture, upload.destination_slice, upload.destination_level,
              MTL::Origin::Make(0, 0, 0));
          command_buffer_has_work = true;
        }

        blit->endEncoding();
      }
    }

    release_repack_staging_after();
    release_buffer_after(cmd, transient_source_buffer, transient_source_size);
    transient_source_buffer = nullptr;
    release_buffer_after(cmd, constants_buffer, constants_buffer_size);
    release_buffer_after(cmd, dest_buffer, size_t(dest_buffer_size));
    if (is_upload_batch()) {
      if (!use_cpu_guest_source && shared_memory_range_count) {
        for (uint32_t i = 0; i < shared_memory_range_count; ++i) {
          upload_batch_command_buffer_shared_memory_ranges_.push_back(
              {shared_memory_ranges[i].start, shared_memory_ranges[i].length});
        }
      }
      upload_batch_command_buffer_has_work_ = true;
    } else if (is_standalone()) {
      if (!use_cpu_guest_source && shared_memory_range_count) {
        std::pair<uint32_t, uint32_t> standalone_ranges[2] = {};
        for (uint32_t i = 0; i < shared_memory_range_count; ++i) {
          standalone_ranges[i] = {shared_memory_ranges[i].start,
                                  shared_memory_ranges[i].length};
        }
        static_cast<MetalSharedMemory&>(shared_memory())
            .TrackStandaloneGpuAccess(cmd, standalone_ranges,
                                      shared_memory_range_count);
      }
      command_processor_->CommitStandaloneAsync(cmd);
    }
  } else {
    if (is_standalone()) {
      command_processor_->CommitStandaloneAndWait(cmd);
    } else {
      cmd->commit();
      cmd->waitUntilCompleted();
    }

    uint8_t* dest_data = static_cast<uint8_t*>(dest_buffer->contents());
    if (!dest_data) {
      release_buffer_immediate(constants_buffer, constants_buffer_size);
      release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
      return false;
    }

    auto find_stored_level =
        [&](bool is_base_storage,
            uint32_t stored_level) -> const StoredLevelHostLayout* {
      for (const StoredLevelHostLayout& layout : stored_levels) {
        if (layout.is_base == is_base_storage && layout.level == stored_level) {
          return &layout;
        }
      }
      return nullptr;
    };

    uint32_t bytes_per_host_block = load_shader_info.bytes_per_host_block;

    for (uint32_t level = level_first; level <= level_last; ++level) {
      uint32_t stored_level = std::min(level, level_packed);
      bool is_base_storage =
          stored_level == 0 && (level_packed != 0 || level == 0);
      const StoredLevelHostLayout* stored_layout =
          find_stored_level(is_base_storage, stored_level);
      if (!stored_layout) {
        continue;
      }

      uint32_t level_width_unscaled = std::max(width >> level, uint32_t(1));
      uint32_t level_height_unscaled = std::max(height >> level, uint32_t(1));
      uint32_t level_depth = std::max(depth >> level, uint32_t(1));
      uint32_t level_width_scaled =
          level_width_unscaled * texture_resolution_scale_x;
      uint32_t level_height_scaled =
          level_height_unscaled * texture_resolution_scale_y;

      uint32_t upload_width = level_width_scaled;
      uint32_t upload_height = level_height_scaled;
      if (host_block_compressed) {
        upload_width = xe::round_up(upload_width, host_block_width);
        upload_height = xe::round_up(upload_height, host_block_height);
      }

      uint32_t packed_offset_blocks_x = 0;
      uint32_t packed_offset_blocks_y = 0;
      uint32_t packed_offset_z = 0;
      if (level >= level_packed) {
        texture_util::GetPackedMipOffset(
            width, height, depth, key.format, level, packed_offset_blocks_x,
            packed_offset_blocks_y, packed_offset_z);
      }

      for (uint32_t slice = 0; slice < array_size; ++slice) {
        const uint8_t* slice_base = dest_data +
                                    stored_layout->dest_offset_bytes +
                                    slice * stored_layout->slice_size_bytes;

        const uint8_t* source_ptr = slice_base;
        size_t bytes_per_image = size_t(stored_layout->row_pitch_bytes) *
                                 stored_layout->height_blocks;
        if (level >= level_packed) {
          if (host_block_compressed) {
            uint32_t packed_offset_blocks_x_scaled =
                packed_offset_blocks_x * texture_resolution_scale_x;
            uint32_t packed_offset_blocks_y_scaled =
                packed_offset_blocks_y * texture_resolution_scale_y;
            source_ptr += packed_offset_z * bytes_per_image;
            source_ptr +=
                packed_offset_blocks_y_scaled * stored_layout->row_pitch_bytes;
            source_ptr += packed_offset_blocks_x_scaled * bytes_per_block;
          } else {
            uint32_t packed_offset_texels_x =
                packed_offset_blocks_x * block_width;
            uint32_t packed_offset_texels_y =
                packed_offset_blocks_y * block_height;
            packed_offset_texels_x *= texture_resolution_scale_x;
            packed_offset_texels_y *= texture_resolution_scale_y;
            source_ptr += packed_offset_z * bytes_per_image;
            source_ptr +=
                packed_offset_texels_y * stored_layout->row_pitch_bytes;
            source_ptr += packed_offset_texels_x * bytes_per_host_block;
          }
        }

        if (dimension == xenos::DataDimension::k3D) {
          MTL::Region region = MTL::Region::Make3D(0, 0, 0, upload_width,
                                                   upload_height, level_depth);
          mtl_texture->replaceRegion(region, level, 0, source_ptr,
                                     stored_layout->row_pitch_bytes,
                                     bytes_per_image);
        } else {
          MTL::Region region =
              MTL::Region::Make2D(0, 0, upload_width, upload_height);
          mtl_texture->replaceRegion(region, level, slice, source_ptr,
                                     stored_layout->row_pitch_bytes, 0);
        }
      }
    }
  }

  if (!use_blit_upload) {
    release_buffer_immediate(transient_source_buffer, transient_source_size);
    transient_source_buffer = nullptr;
    release_buffer_immediate(constants_buffer, constants_buffer_size);
    release_buffer_immediate(dest_buffer, size_t(dest_buffer_size));
  }

  return true;
}

bool MetalTextureCache::Initialize() {
  SCOPE_profile_cpu_f("gpu");
  XE_SCOPED_AUTORELEASE_POOL("MetalTextureCache::Initialize");

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device from command "
        "processor");
    return false;
  }
  supports_bc_texture_compression_ = device->supportsBCTextureCompression();
  if (!supports_bc_texture_compression_) {
    XELOGW(
        "Metal: BC texture compression not supported by this device; forcing "
        "BCn decompression");
  }
  if (::cvars::metal_texture_cache_use_private &&
      !::cvars::metal_texture_upload_via_blit) {
    XELOGW(
        "Metal texture cache: private textures requested but blit uploads "
        "disabled; forcing shared textures");
  }

  {
    std::lock_guard<std::mutex> lock(upload_buffer_pool_mutex_);
    upload_buffer_pool_ =
        std::make_shared<UploadBufferPool>(device, kUploadBufferPoolMaxBytes);
  }
  if (::cvars::metal_use_heaps) {
    size_t min_heap_bytes = std::max<int32_t>(0, ::cvars::metal_heap_min_bytes);
    texture_heap_pool_ = std::make_unique<MetalHeapPool>(
        device, GetCacheTextureStorageMode(), min_heap_bytes, "XeniaTex");
    texture_heap_pool_->SetHeapCreatedCallback([this](MTL::Heap* heap) {
      if (command_processor_) {
        command_processor_->AddResidencySetHeap(heap);
      }
    });
  }

  InitializeNorm16Selection(device);

  // Create null textures following existing factory pattern
  null_texture_2d_ = CreateNullTexture2D();
  null_texture_3d_ = CreateNullTexture3D();
  null_texture_cube_ = CreateNullTextureCube();

  if (!null_texture_2d_ || !null_texture_3d_ || !null_texture_cube_) {
    XELOGE("Failed to create null textures");
    return false;
  }

  // Allocate persistent bindless heap slots for null textures so that
  // bindless draws can fall back to a valid descriptor index.
  null_texture_2d_bindless_index_ =
      command_processor_->AllocateViewBindlessIndex();
  if (null_texture_2d_bindless_index_ == UINT32_MAX) {
    XELOGE("Failed to allocate bindless SRV slot for null 2D texture");
    return false;
  }
  if (auto* e = command_processor_->GetViewBindlessHeapEntry(
          null_texture_2d_bindless_index_)) {
    IRDescriptorTableSetTexture(e, null_texture_2d_, 0.0f, 0);
  }
  command_processor_->SetNativeMslViewBindlessTexture(
      null_texture_2d_bindless_index_, null_texture_2d_);
  null_texture_3d_bindless_index_ =
      command_processor_->AllocateViewBindlessIndex();
  if (null_texture_3d_bindless_index_ == UINT32_MAX) {
    XELOGE("Failed to allocate bindless SRV slot for null 3D texture");
    return false;
  }
  if (auto* e = command_processor_->GetViewBindlessHeapEntry(
          null_texture_3d_bindless_index_)) {
    IRDescriptorTableSetTexture(e, null_texture_3d_, 0.0f, 0);
  }
  command_processor_->SetNativeMslViewBindlessTexture(
      null_texture_3d_bindless_index_, null_texture_3d_);
  null_texture_cube_bindless_index_ =
      command_processor_->AllocateViewBindlessIndex();
  if (null_texture_cube_bindless_index_ == UINT32_MAX) {
    XELOGE("Failed to allocate bindless SRV slot for null cube texture");
    return false;
  }
  if (auto* e = command_processor_->GetViewBindlessHeapEntry(
          null_texture_cube_bindless_index_)) {
    IRDescriptorTableSetTexture(e, null_texture_cube_, 0.0f, 0);
  }
  command_processor_->SetNativeMslViewBindlessTexture(
      null_texture_cube_bindless_index_, null_texture_cube_);

  // Allocate a persistent bindless slot for the null sampler.
  {
    MTL::SamplerDescriptor* desc = MTL::SamplerDescriptor::alloc()->init();
    desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMipFilter(MTL::SamplerMipFilterLinear);
    desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
    desc->setSupportArgumentBuffers(true);
    null_sampler_bindless_ =
        command_processor_->GetMetalDevice()->newSamplerState(desc);
    desc->release();
    if (null_sampler_bindless_) {
      null_sampler_bindless_index_ =
          command_processor_->AllocateSamplerBindlessIndex();
      if (null_sampler_bindless_index_ == UINT32_MAX) {
        XELOGE("Failed to allocate bindless sampler slot for null sampler");
        null_sampler_bindless_->release();
        null_sampler_bindless_ = nullptr;
        return false;
      }
      if (auto* e = command_processor_->GetSamplerBindlessHeapEntry(
              null_sampler_bindless_index_)) {
        IRDescriptorTableSetSampler(e, null_sampler_bindless_, 0.0f);
      }
      command_processor_->SetNativeMslSamplerBindlessState(
          null_sampler_bindless_index_, null_sampler_bindless_);
    } else {
      XELOGE("Failed to create bindless null sampler");
      return false;
    }
  }

  if (!InitializeLoadPipelines()) {
    XELOGE("Metal texture cache: Failed to initialize texture_load pipelines");
    return false;
  }

  XELOGD(
      "Metal texture cache: Initialized successfully (null textures + GPU "
      "texture_load pipelines)");

  return true;
}

bool MetalTextureCache::InitializeLoadPipelines() {
  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    return false;
  }

  NS::Error* error = nullptr;

  auto create_pipeline_from_metallib =
      [&](const uint8_t* data, size_t size) -> MTL::ComputePipelineState* {
    if (!data || !size) {
      return nullptr;
    }
    dispatch_data_t dispatch_data = dispatch_data_create(
        data, size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    MTL::Library* lib = device->newLibrary(dispatch_data, &error);
    dispatch_release(dispatch_data);
    if (!lib) {
      XELOGE("MetalTextureCache: failed to create texture_load library: {}",
             error ? error->localizedDescription()->utf8String() : "unknown");
      return nullptr;
    }
    NS::String* fn_name =
        NS::String::string("entry_xe", NS::UTF8StringEncoding);
    MTL::Function* fn = lib->newFunction(fn_name);
    if (!fn) {
      XELOGE(
          "MetalTextureCache: texture_load metallib missing entry_xe "
          "function");
      lib->release();
      return nullptr;
    }
    MTL::ComputePipelineState* pipeline =
        device->newComputePipelineState(fn, &error);
    fn->release();
    lib->release();
    if (!pipeline) {
      XELOGE("MetalTextureCache: failed to create texture_load pipeline: {}",
             error ? error->localizedDescription()->utf8String() : "unknown");
      return nullptr;
    }
    return pipeline;
  };

  auto init_pipeline = [&](TextureCache::LoadShaderIndex index,
                           const uint8_t* data, size_t size) -> void {
    load_pipelines_[index] = create_pipeline_from_metallib(data, size);
  };
  auto init_pipeline_scaled = [&](TextureCache::LoadShaderIndex index,
                                  const uint8_t* data, size_t size) -> void {
    load_pipelines_scaled_[index] = create_pipeline_from_metallib(data, size);
  };
  texture_upload_repack_pipeline_ = create_pipeline_from_metallib(
      texture_upload_repack_metallib, sizeof(texture_upload_repack_metallib));

  init_pipeline(TextureCache::kLoadShaderIndex8bpb,
                texture_load_8bpb_cs_metallib,
                sizeof(texture_load_8bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex8bpb,
                       texture_load_8bpb_scaled_cs_metallib,
                       sizeof(texture_load_8bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex16bpb,
                texture_load_16bpb_cs_metallib,
                sizeof(texture_load_16bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex16bpb,
                       texture_load_16bpb_scaled_cs_metallib,
                       sizeof(texture_load_16bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex32bpb,
                texture_load_32bpb_cs_metallib,
                sizeof(texture_load_32bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex32bpb,
                       texture_load_32bpb_scaled_cs_metallib,
                       sizeof(texture_load_32bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex64bpb,
                texture_load_64bpb_cs_metallib,
                sizeof(texture_load_64bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex64bpb,
                       texture_load_64bpb_scaled_cs_metallib,
                       sizeof(texture_load_64bpb_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndex128bpb,
                texture_load_128bpb_cs_metallib,
                sizeof(texture_load_128bpb_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndex128bpb,
                       texture_load_128bpb_scaled_cs_metallib,
                       sizeof(texture_load_128bpb_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
                texture_load_r5g5b5a1_b5g5r5a1_cs_metallib,
                sizeof(texture_load_r5g5b5a1_b5g5r5a1_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
      texture_load_r5g5b5a1_b5g5r5a1_scaled_cs_metallib,
      sizeof(texture_load_r5g5b5a1_b5g5r5a1_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR5G6B5ToB5G6R5,
                texture_load_r5g6b5_b5g6r5_cs_metallib,
                sizeof(texture_load_r5g6b5_b5g6r5_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexR5G6B5ToB5G6R5,
                       texture_load_r5g6b5_b5g6r5_scaled_cs_metallib,
                       sizeof(texture_load_r5g6b5_b5g6r5_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
                texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs_metallib,
                sizeof(texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
      texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs_metallib,
      sizeof(texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexR10G11B11ToRGBA16,
                texture_load_r10g11b11_rgba16_cs_metallib,
                sizeof(texture_load_r10g11b11_rgba16_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR10G11B11ToRGBA16,
      texture_load_r10g11b11_rgba16_scaled_cs_metallib,
      sizeof(texture_load_r10g11b11_rgba16_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR10G11B11ToRGBA16SNorm,
                texture_load_r10g11b11_rgba16_snorm_cs_metallib,
                sizeof(texture_load_r10g11b11_rgba16_snorm_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR10G11B11ToRGBA16SNorm,
      texture_load_r10g11b11_rgba16_snorm_scaled_cs_metallib,
      sizeof(texture_load_r10g11b11_rgba16_snorm_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR11G11B10ToRGBA16,
                texture_load_r11g11b10_rgba16_cs_metallib,
                sizeof(texture_load_r11g11b10_rgba16_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR11G11B10ToRGBA16,
      texture_load_r11g11b10_rgba16_scaled_cs_metallib,
      sizeof(texture_load_r11g11b10_rgba16_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR11G11B10ToRGBA16SNorm,
                texture_load_r11g11b10_rgba16_snorm_cs_metallib,
                sizeof(texture_load_r11g11b10_rgba16_snorm_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexR11G11B10ToRGBA16SNorm,
      texture_load_r11g11b10_rgba16_snorm_scaled_cs_metallib,
      sizeof(texture_load_r11g11b10_rgba16_snorm_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexR16UNormToFloat,
                texture_load_r16_unorm_float_cs_metallib,
                sizeof(texture_load_r16_unorm_float_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexR16UNormToFloat,
                       texture_load_r16_unorm_float_scaled_cs_metallib,
                       sizeof(texture_load_r16_unorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexR16SNormToFloat,
                texture_load_r16_snorm_float_cs_metallib,
                sizeof(texture_load_r16_snorm_float_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexR16SNormToFloat,
                       texture_load_r16_snorm_float_scaled_cs_metallib,
                       sizeof(texture_load_r16_snorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRG16UNormToFloat,
                texture_load_rg16_unorm_float_cs_metallib,
                sizeof(texture_load_rg16_unorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRG16UNormToFloat,
      texture_load_rg16_unorm_float_scaled_cs_metallib,
      sizeof(texture_load_rg16_unorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRG16SNormToFloat,
                texture_load_rg16_snorm_float_cs_metallib,
                sizeof(texture_load_rg16_snorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRG16SNormToFloat,
      texture_load_rg16_snorm_float_scaled_cs_metallib,
      sizeof(texture_load_rg16_snorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRGBA16UNormToFloat,
                texture_load_rgba16_unorm_float_cs_metallib,
                sizeof(texture_load_rgba16_unorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA16UNormToFloat,
      texture_load_rgba16_unorm_float_scaled_cs_metallib,
      sizeof(texture_load_rgba16_unorm_float_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRGBA16SNormToFloat,
                texture_load_rgba16_snorm_float_cs_metallib,
                sizeof(texture_load_rgba16_snorm_float_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA16SNormToFloat,
      texture_load_rgba16_snorm_float_scaled_cs_metallib,
      sizeof(texture_load_rgba16_snorm_float_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexRGBA4ToBGRA4,
                texture_load_r4g4b4a4_b4g4r4a4_cs_metallib,
                sizeof(texture_load_r4g4b4a4_b4g4r4a4_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA4ToBGRA4,
      texture_load_r4g4b4a4_b4g4r4a4_scaled_cs_metallib,
      sizeof(texture_load_r4g4b4a4_b4g4r4a4_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexRGBA4ToARGB4,
                texture_load_r4g4b4a4_a4r4g4b4_cs_metallib,
                sizeof(texture_load_r4g4b4a4_a4r4g4b4_cs_metallib));
  init_pipeline_scaled(
      TextureCache::kLoadShaderIndexRGBA4ToARGB4,
      texture_load_r4g4b4a4_a4r4g4b4_scaled_cs_metallib,
      sizeof(texture_load_r4g4b4a4_a4r4g4b4_scaled_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexGBGR8ToGRGB8,
                texture_load_gbgr8_grgb8_cs_metallib,
                sizeof(texture_load_gbgr8_grgb8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexGBGR8ToRGB8,
                texture_load_gbgr8_rgb8_cs_metallib,
                sizeof(texture_load_gbgr8_rgb8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexBGRG8ToRGBG8,
                texture_load_bgrg8_rgbg8_cs_metallib,
                sizeof(texture_load_bgrg8_rgbg8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexBGRG8ToRGB8,
                texture_load_bgrg8_rgb8_cs_metallib,
                sizeof(texture_load_bgrg8_rgb8_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexDXT1ToRGBA8,
                texture_load_dxt1_rgba8_cs_metallib,
                sizeof(texture_load_dxt1_rgba8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3A,
                texture_load_dxt3a_cs_metallib,
                sizeof(texture_load_dxt3a_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3AAs1111ToBGRA4,
                texture_load_dxt3aas1111_bgra4_cs_metallib,
                sizeof(texture_load_dxt3aas1111_bgra4_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3AAs1111ToARGB4,
                texture_load_dxt3aas1111_argb4_cs_metallib,
                sizeof(texture_load_dxt3aas1111_argb4_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT3ToRGBA8,
                texture_load_dxt3_rgba8_cs_metallib,
                sizeof(texture_load_dxt3_rgba8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT5ToRGBA8,
                texture_load_dxt5_rgba8_cs_metallib,
                sizeof(texture_load_dxt5_rgba8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXT5AToR8,
                texture_load_dxt5a_r8_cs_metallib,
                sizeof(texture_load_dxt5a_r8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDXNToRG8,
                texture_load_dxn_rg8_cs_metallib,
                sizeof(texture_load_dxn_rg8_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexCTX1,
                texture_load_ctx1_cs_metallib,
                sizeof(texture_load_ctx1_cs_metallib));

  init_pipeline(TextureCache::kLoadShaderIndexDepthUnorm,
                texture_load_depth_unorm_cs_metallib,
                sizeof(texture_load_depth_unorm_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexDepthUnorm,
                       texture_load_depth_unorm_scaled_cs_metallib,
                       sizeof(texture_load_depth_unorm_scaled_cs_metallib));
  init_pipeline(TextureCache::kLoadShaderIndexDepthFloat,
                texture_load_depth_float_cs_metallib,
                sizeof(texture_load_depth_float_cs_metallib));
  init_pipeline_scaled(TextureCache::kLoadShaderIndexDepthFloat,
                       texture_load_depth_float_scaled_cs_metallib,
                       sizeof(texture_load_depth_float_scaled_cs_metallib));

  // Require at least the common loaders.
  return load_pipelines_[TextureCache::kLoadShaderIndex32bpb] != nullptr &&
         load_pipelines_[TextureCache::kLoadShaderIndex16bpb] != nullptr &&
         load_pipelines_[TextureCache::kLoadShaderIndex8bpb] != nullptr &&
         texture_upload_repack_pipeline_ != nullptr;
}

void MetalTextureCache::InitializeNorm16Selection(MTL::Device* device) {
  r16_selection_.unsigned_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatR16Unorm);
  r16_selection_.signed_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatR16Snorm);

  rg16_selection_.unsigned_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRG16Unorm);
  rg16_selection_.signed_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRG16Snorm);

  rgba16_selection_.unsigned_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRGBA16Unorm);
  rgba16_selection_.signed_uses_float =
      !SupportsPixelFormat(device, MTL::PixelFormatRGBA16Snorm);
}

void MetalTextureCache::Shutdown() {
  SCOPE_profile_cpu_f("gpu");

  deferred_upload_batch_depth_ = 0;
  FlushDeferredUploadEncoderBatch();
  AbortUploadCommandBufferBatch(false);

  ClearCache();

  if (texture_upload_repack_pipeline_) {
    texture_upload_repack_pipeline_->release();
    texture_upload_repack_pipeline_ = nullptr;
  }

  for (size_t i = 0; i < kLoadShaderCount; ++i) {
    if (load_pipelines_[i]) {
      load_pipelines_[i]->release();
      load_pipelines_[i] = nullptr;
    }
    if (load_pipelines_scaled_[i]) {
      load_pipelines_scaled_[i]->release();
      load_pipelines_scaled_[i] = nullptr;
    }
  }

  // Release persistent bindless heap slots for null textures/samplers.
  if (null_texture_2d_bindless_index_ != UINT32_MAX) {
    command_processor_->ReleaseViewBindlessIndex(
        null_texture_2d_bindless_index_);
    null_texture_2d_bindless_index_ = UINT32_MAX;
  }
  if (null_texture_3d_bindless_index_ != UINT32_MAX) {
    command_processor_->ReleaseViewBindlessIndex(
        null_texture_3d_bindless_index_);
    null_texture_3d_bindless_index_ = UINT32_MAX;
  }
  if (null_texture_cube_bindless_index_ != UINT32_MAX) {
    command_processor_->ReleaseViewBindlessIndex(
        null_texture_cube_bindless_index_);
    null_texture_cube_bindless_index_ = UINT32_MAX;
  }
  if (null_sampler_bindless_index_ != UINT32_MAX) {
    command_processor_->ReleaseSamplerBindlessIndex(
        null_sampler_bindless_index_);
    null_sampler_bindless_index_ = UINT32_MAX;
  }
  if (null_sampler_bindless_) {
    null_sampler_bindless_->release();
    null_sampler_bindless_ = nullptr;
  }
  for (auto& retired_sampler : retired_sampler_states_) {
    if (retired_sampler.sampler) {
      retired_sampler.sampler->release();
    }
  }
  retired_sampler_states_.clear();

  // Follow existing shutdown pattern - explicit null checks and release
  if (null_texture_2d_) {
    null_texture_2d_->release();
    null_texture_2d_ = nullptr;
  }
  if (null_texture_3d_) {
    null_texture_3d_->release();
    null_texture_3d_ = nullptr;
  }
  if (null_texture_cube_) {
    null_texture_cube_->release();
    null_texture_cube_ = nullptr;
  }

  {
    std::shared_ptr<UploadBufferPool> buffer_pool;
    {
      std::lock_guard<std::mutex> lock(upload_buffer_pool_mutex_);
      buffer_pool = std::move(upload_buffer_pool_);
    }
    if (buffer_pool) {
      buffer_pool->Shutdown();
    }
  }
  if (texture_heap_pool_) {
    texture_heap_pool_->Shutdown();
    texture_heap_pool_.reset();
  }

  XELOGD("Metal texture cache: Shutdown complete");
}

void MetalTextureCache::ClearScaledResolveBuffers() {
  for (auto& buffer : scaled_resolve_buffers_) {
    if (buffer.buffer) {
      buffer.buffer->release();
      buffer.buffer = nullptr;
    }
  }
  scaled_resolve_buffers_.clear();
  for (auto& retired : scaled_resolve_retired_buffers_) {
    if (retired.buffer) {
      retired.buffer->release();
      retired.buffer = nullptr;
    }
  }
  scaled_resolve_retired_buffers_.clear();
  scaled_resolve_retired_bytes_ = 0;
  scaled_resolve_current_buffer_index_ = size_t(-1);
  scaled_resolve_current_range_start_scaled_ = 0;
  scaled_resolve_current_range_length_scaled_ = 0;
}

void MetalTextureCache::CompletedSubmissionUpdated(
    uint64_t completed_submission_index) {
  TextureCache::CompletedSubmissionUpdated(completed_submission_index);
  if (!scaled_resolve_retired_buffers_.empty()) {
    for (auto it = scaled_resolve_retired_buffers_.begin();
         it != scaled_resolve_retired_buffers_.end();) {
      if (it->submission_id <= completed_submission_index) {
        if (it->buffer) {
          it->buffer->release();
        }
        scaled_resolve_retired_bytes_ =
            scaled_resolve_retired_bytes_ > it->length_scaled
                ? (scaled_resolve_retired_bytes_ - it->length_scaled)
                : 0;
        it = scaled_resolve_retired_buffers_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto it = retired_sampler_states_.begin();
       it != retired_sampler_states_.end();) {
    if (it->submission_id <= completed_submission_index) {
      if (it->sampler) {
        it->sampler->release();
      }
      it = retired_sampler_states_.erase(it);
    } else {
      ++it;
    }
  }
}

bool MetalTextureCache::TrimViewBindlessPressure(uint32_t needed_slot_count) {
  if (!command_processor_) {
    return false;
  }
  bool destroyed_any = false;
  uint64_t completed_submission = command_processor_->GetCompletedSubmission();
  while (command_processor_->GetViewBindlessHeapAvailableCount() <
         needed_slot_count) {
    if (!DestroyOldestTextureIfUnused(completed_submission)) {
      break;
    }
    destroyed_any = true;
  }
  return destroyed_any;
}

void MetalTextureCache::ClearCache() {
  SCOPE_profile_cpu_f("gpu");

  TextureCache::ClearCache();

  // Release persistent bindless sampler slots before destroying samplers.
  for (auto& [param_key, sampler_index] : sampler_bindless_indices_) {
    command_processor_->ReleaseSamplerBindlessIndex(sampler_index);
  }
  sampler_bindless_indices_.clear();

  for (auto& sampler_pair : sampler_cache_) {
    ReleaseOrRetireSamplerState(sampler_pair.second);
  }
  sampler_cache_.clear();
  ClearScaledResolveBuffers();

  XELOGD("Metal texture cache: Cache cleared");
}

void MetalTextureCache::ReleaseOrRetireSamplerState(
    MTL::SamplerState* sampler) {
  if (!sampler) {
    return;
  }
  uint64_t current_submission =
      command_processor_ ? command_processor_->GetLatestSubmissionStarted() : 0;
  uint64_t completed_submission =
      command_processor_ ? command_processor_->GetCompletedSubmission() : 0;
  if (!current_submission || completed_submission >= current_submission) {
    sampler->release();
    return;
  }
  retired_sampler_states_.push_back({sampler, current_submission});
}

MTL::Texture* MetalTextureCache::CreateTexture(
    MTL::TextureDescriptor* descriptor) {
  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE(
        "Metal texture cache: Failed to get Metal device from command "
        "processor");
    descriptor->release();
    return nullptr;
  }

  MTL::Texture* texture = nullptr;
  if (texture_heap_pool_ &&
      descriptor->storageMode() == MTL::StorageModePrivate) {
    texture = texture_heap_pool_->CreateTexture(descriptor);
  }
  if (!texture) {
    texture = device->newTexture(descriptor);
  }
  descriptor->release();
  return texture;
}

MTL::Texture* MetalTextureCache::CreateTexture2D(
    uint32_t width, uint32_t height, uint32_t array_length,
    MTL::PixelFormat format, MTL::TextureSwizzleChannels swizzle,
    uint32_t mip_levels) {
  // Always create 2D array textures (even with a single layer) so that the
  // Metal texture type matches the shader expectation of texture2d_array,
  // mirroring the D3D12 backend which uses TEXTURE2DARRAY SRVs for 1D/2D
  // textures.
  array_length = std::max(array_length, 1u);

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType2DArray);
  descriptor->setPixelFormat(format);
  descriptor->setWidth(width);
  descriptor->setHeight(std::max(height, 1u));
  descriptor->setDepth(1);
  descriptor->setArrayLength(array_length);
  descriptor->setMipmapLevelCount(mip_levels);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(GetCacheTextureStorageMode());
  descriptor->setSwizzle(swizzle);

  MTL::Texture* texture = CreateTexture(descriptor);
  if (!texture) {
    XELOGE(
        "Metal texture cache: Failed to create 2D array texture {}x{} (layers "
        "{})",
        width, height, array_length);
    return nullptr;
  }

  return texture;
}

MTL::Texture* MetalTextureCache::CreateTexture3D(
    uint32_t width, uint32_t height, uint32_t depth, MTL::PixelFormat format,
    MTL::TextureSwizzleChannels swizzle, uint32_t mip_levels) {
  depth = std::max(depth, 1u);

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType3D);
  descriptor->setPixelFormat(format);
  descriptor->setWidth(width);
  descriptor->setHeight(std::max(height, 1u));
  descriptor->setDepth(depth);
  descriptor->setArrayLength(1);
  descriptor->setMipmapLevelCount(mip_levels);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(GetCacheTextureStorageMode());
  descriptor->setSwizzle(swizzle);

  MTL::Texture* texture = CreateTexture(descriptor);
  if (!texture) {
    XELOGE("Metal texture cache: Failed to create 3D texture {}x{}x{}", width,
           height, depth);
    return nullptr;
  }

  return texture;
}

MTL::Texture* MetalTextureCache::CreateTextureCube(
    uint32_t width, MTL::PixelFormat format,
    MTL::TextureSwizzleChannels swizzle, uint32_t mip_levels,
    uint32_t cube_count) {
  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  // Use cube for single-cube textures to match non-array cube bindings in the
  // translated MSL, and cube-array only when multiple cubes are present.
  descriptor->setTextureType(cube_count > 1 ? MTL::TextureTypeCubeArray
                                            : MTL::TextureTypeCube);
  descriptor->setArrayLength(std::max(cube_count, 1u));
  descriptor->setPixelFormat(format);
  descriptor->setWidth(width);
  descriptor->setHeight(width);
  descriptor->setDepth(1);
  descriptor->setMipmapLevelCount(mip_levels);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(GetCacheTextureStorageMode());
  descriptor->setSwizzle(swizzle);

  MTL::Texture* texture = CreateTexture(descriptor);
  if (!texture) {
    XELOGE("Metal texture cache: Failed to create Cube texture {}x{}", width,
           width);
    return nullptr;
  }

  return texture;
}

MTL::Texture* MetalTextureCache::CreateNullTexture2D() {
  SCOPE_profile_cpu_f("gpu");

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  // Null 2D textures are created as 2D arrays with a single layer so they can
  // be bound wherever shaders expect texture2d_array.
  descriptor->setTextureType(MTL::TextureType2DArray);
  descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setArrayLength(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);

  MTL::Texture* texture = CreateTexture(descriptor);
  if (texture) {
    // Match D3D12 null-SRV semantics: missing textures should sample zero.
    uint32_t default_color = 0x00000000;
    MTL::Region region = MTL::Region::Make2D(0, 0, 1, 1);
    texture->replaceRegion(region, 0, &default_color, 4);
  } else {
    XELOGE("Failed to create null 2D texture");
  }

  return texture;  // No retain needed - newTexture returns retained object
}

MTL::Texture* MetalTextureCache::CreateNullTexture3D() {
  SCOPE_profile_cpu_f("gpu");

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType3D);
  descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setDepth(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);

  MTL::Texture* texture = CreateTexture(descriptor);
  if (texture) {
    // Match D3D12 null-SRV semantics: missing textures should sample zero.
    uint32_t default_color = 0x00000000;
    MTL::Region region = MTL::Region::Make3D(0, 0, 0, 1, 1, 1);
    texture->replaceRegion(region, 0, 0, &default_color, 4, 4);
  } else {
    XELOGE("Failed to create null 3D texture");
  }

  return texture;  // No retain needed - newTexture returns retained object
}

MTL::Texture* MetalTextureCache::CreateNullTextureCube() {
  SCOPE_profile_cpu_f("gpu");

  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  // Null cube texture must match non-array cube bindings in translated MSL.
  descriptor->setTextureType(MTL::TextureTypeCube);
  descriptor->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
  descriptor->setWidth(1);
  descriptor->setHeight(1);
  descriptor->setDepth(1);
  descriptor->setArrayLength(1);
  descriptor->setUsage(MTL::TextureUsageShaderRead |
                       MTL::TextureUsagePixelFormatView);
  descriptor->setStorageMode(MTL::StorageModeShared);

  MTL::Texture* texture = CreateTexture(descriptor);
  if (texture) {
    // Match D3D12 null-SRV semantics: missing textures should sample zero.
    uint32_t default_color = 0x00000000;
    MTL::Region region = MTL::Region::Make2D(0, 0, 1, 1);
    for (uint32_t face = 0; face < 6; ++face) {
      texture->replaceRegion(region, 0, face, &default_color, 4, 0);
    }
  } else {
    XELOGE("Failed to create null cube texture");
  }

  return texture;  // No retain needed - newTexture returns retained object
}

// RequestTextures override - integrates with standard texture binding pipeline
void MetalTextureCache::RequestTextures(uint32_t used_texture_mask) {
  SCOPE_profile_cpu_f("gpu");
  const bool may_load_data = MayRequestTexturesLoadData(used_texture_mask);
  uint64_t load_calls_before = loaded_texture_data_count_;

  if (may_load_data) {
    UploadBatchScope upload_batch(*this);
    TextureCache::RequestTextures(used_texture_mask);
    upload_batch.End();
  } else {
    TextureCache::RequestTextures(used_texture_mask);
  }
  uint64_t load_calls_delta = loaded_texture_data_count_ - load_calls_before;
  if (!may_load_data) {
    assert_zero(load_calls_delta);
  }

  // Intentionally no Metal-specific per-fetch logging here - invalid fetch
  // constants are already reported by the shared TextureCache logic.
}

void MetalTextureCache::RequestTexturesWithoutLoading(
    uint32_t used_texture_mask) {
  SCOPE_profile_cpu_f("gpu");
  uint64_t load_calls_before = loaded_texture_data_count_;
  TextureCache::RequestTextures(used_texture_mask, false);
  assert_true(loaded_texture_data_count_ == load_calls_before);
}

bool MetalTextureCache::AreActiveTextureSRVKeysUpToDate(
    const TextureSRVKey* keys,
    const DxbcShader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    const TextureSRVKey& key = keys[i];
    const TextureBinding* binding =
        GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      if (key.key.is_valid) {
        return false;
      }
      continue;
    }
    if ((key.key != binding->key) ||
        (key.host_swizzle != binding->host_swizzle) ||
        (key.swizzled_signs != binding->swizzled_signs)) {
      return false;
    }
  }
  return true;
}

void MetalTextureCache::WriteActiveTextureSRVKeys(
    TextureSRVKey* keys, const DxbcShader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    TextureSRVKey& key = keys[i];
    const TextureBinding* binding =
        GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      key.key.MakeInvalid();
      key.host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_0000;
      key.swizzled_signs = kSwizzledSignsUnsigned;
      continue;
    }
    key.key = binding->key;
    key.host_swizzle = binding->host_swizzle;
    key.swizzled_signs = binding->swizzled_signs;
  }
}

uint32_t MetalTextureCache::GetBindlessSRVIndexForBinding(
    uint32_t fetch_constant, xenos::FetchOpDimension dimension, bool is_signed,
    MTL::Texture** texture_for_encoder_out, bool* is_fallback_out,
    const char** fallback_reason_out) {
  if (is_fallback_out) {
    *is_fallback_out = false;
  }
  if (fallback_reason_out) {
    *fallback_reason_out = nullptr;
  }

  auto get_null_texture_for_dimension = [&]() -> MTL::Texture* {
    switch (dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return null_texture_2d_;
      case xenos::FetchOpDimension::k3DOrStacked:
        return null_texture_3d_;
      case xenos::FetchOpDimension::kCube:
        return null_texture_cube_;
      default:
        return null_texture_2d_;
    }
  };
  auto get_null_index_for_dimension = [&]() -> uint32_t {
    switch (dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return null_texture_2d_bindless_index_;
      case xenos::FetchOpDimension::k3DOrStacked:
        return null_texture_3d_bindless_index_;
      case xenos::FetchOpDimension::kCube:
        return null_texture_cube_bindless_index_;
      default:
        return null_texture_2d_bindless_index_;
    }
  };
  auto return_null_index_for_dimension = [&](const char* reason) -> uint32_t {
    if (texture_for_encoder_out) {
      *texture_for_encoder_out = get_null_texture_for_dimension();
    }
    if (is_fallback_out) {
      *is_fallback_out = true;
    }
    if (fallback_reason_out) {
      *fallback_reason_out = reason;
    }
    return get_null_index_for_dimension();
  };

  const TextureBinding* binding = GetValidTextureBinding(fetch_constant);
  if (!binding) {
    return return_null_index_for_dimension("fetch constant has no valid texture binding");
  }
  if (!AreDimensionsCompatible(dimension, binding->key.dimension)) {
    return return_null_index_for_dimension("shader texture dimension is incompatible with bound texture");
  }

  Texture* texture = nullptr;
  if (is_signed) {
    if (texture_util::IsAnySignSigned(binding->swizzled_signs)) {
      texture = IsSignedVersionSeparateForFormat(binding->key)
                    ? binding->texture_signed
                    : binding->texture;
    } else {
      return return_null_index_for_dimension("signed view not selected by texture signs");
    }
  } else if (texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
    texture = binding->texture;
  } else {
    return return_null_index_for_dimension("unsigned view not selected by texture signs");
  }

  if (!texture) {
    return return_null_index_for_dimension("resolved texture object is null");
  }

  texture->MarkAsUsed();
  auto* metal_texture = static_cast<MetalTexture*>(texture);
  if (!metal_texture) {
    return return_null_index_for_dimension("resolved texture is not a Metal texture");
  }
  MTL::Texture* texture_for_encoder = nullptr;
  uint32_t srv_index = metal_texture->GetOrCreateBindlessSRVIndexAndView(
      binding->host_swizzle, dimension, is_signed,
      texture_for_encoder_out ? &texture_for_encoder : nullptr);
  if (srv_index == UINT32_MAX) {
    return return_null_index_for_dimension("failed to create Metal SRV view");
  }
  if (texture_for_encoder_out) {
    *texture_for_encoder_out = texture_for_encoder
                                   ? texture_for_encoder
                                   : get_null_texture_for_dimension();
  }
  return srv_index;
}

uint32_t MetalTextureCache::GetBindlessSamplerIndexForBinding(
    const DxbcShader::SamplerBinding& binding) {
  SamplerParameters parameters = GetSamplerParameters(binding);
  // GetOrCreateSampler will create the sampler and allocate a persistent
  // bindless slot if it doesn't exist yet.
  MTL::SamplerState* sampler_state = GetOrCreateSampler(parameters);
  if (!sampler_state) {
    return null_sampler_bindless_index_;
  }
  auto it = sampler_bindless_indices_.find(parameters.value);
  if (it != sampler_bindless_indices_.end()) {
    return it->second;
  }
  return null_sampler_bindless_index_;
}

uint32_t MetalTextureCache::GetNativeMslSRVIndexForBindlessIndex(
    uint32_t index) const {
  if (!command_processor_) {
    return UINT32_MAX;
  }
  return command_processor_->GetNativeMslViewBindlessIndex(index);
}

uint32_t MetalTextureCache::GetNativeMslSamplerIndexForBindlessIndex(
    uint32_t index) const {
  if (!command_processor_) {
    return UINT32_MAX;
  }
  return command_processor_->GetNativeMslSamplerBindlessIndex(index);
}

MTL::Texture* MetalTextureCache::RequestSwapTexture(
    uint32_t& width_scaled_out, uint32_t& height_scaled_out,
    xenos::TextureFormat& format_out) {
  static bool logged_invalid = false;
  enum class SwapFailure : uint8_t {
    kCreateTexture = 0,
    kView = 1,
    kLoad = 2,
    kScaledResolve = 3,
    kInvalidPixelFormat = 4,
    kUnsupportedPixelFormat = 5,
    kMissingLoadShader = 6,
    kMissingPipeline = 7,
  };
  auto log_swap_failure_once =
      [&](SwapFailure reason, const TextureKey& log_key, const char* detail) {
        static std::unordered_set<uint64_t> logged_failures;
        uint64_t tag = (uint64_t(reason) << 56) |
                       (uint64_t(log_key.format) << 48) |
                       (uint64_t(log_key.dimension) << 40) |
                       (uint64_t(log_key.scaled_resolve) << 39) |
                       (uint64_t(log_key.endianness) << 37) |
                       (uint64_t(log_key.signed_separate) << 36);
        if (!logged_failures.insert(tag).second) {
          return;
        }
        XELOGW("MetalSwap: request failed: {}", detail);
        XELOGW(
            "MetalSwap: base=0x{:X} mip=0x{:X} {}x{} pitch={} mip_levels={} "
            "format={} dim={} scaled={} endian={} signed={}",
            log_key.base_page << 12, log_key.mip_page << 12, log_key.GetWidth(),
            log_key.GetHeight(), log_key.pitch, log_key.mip_max_level + 1,
            static_cast<uint32_t>(log_key.format),
            static_cast<uint32_t>(log_key.dimension),
            log_key.scaled_resolve ? 1 : 0,
            static_cast<uint32_t>(log_key.endianness),
            log_key.signed_separate ? 1 : 0);
      };

  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 ||
      key.dimension != xenos::DataDimension::k2DOrStacked) {
    if (!logged_invalid) {
      XELOGW("MetalSwap: fetch0 invalid (valid={}, base_page=0x{:X}, dim={})",
             key.is_valid ? 1 : 0, key.base_page,
             static_cast<uint32_t>(key.dimension));
      logged_invalid = true;
    }
    return nullptr;
  }

  auto* texture = static_cast<MetalTexture*>(FindOrCreateTexture(key));
  if (!texture) {
    log_swap_failure_once(SwapFailure::kCreateTexture, key,
                          "failed to create swap texture");
    return nullptr;
  }

  uint32_t host_swizzle =
      GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key));
  MTL::Texture* view = texture->GetOrCreateView(
      host_swizzle, xenos::FetchOpDimension::k2D, false);
  if (!view) {
    log_swap_failure_once(SwapFailure::kView, key,
                          "failed to create swap texture view");
    return nullptr;
  }

  if (!LoadTextureData(*texture)) {
    bool logged_reason = false;
    if (key.scaled_resolve && !IsScaledResolveSupportedForFormat(key)) {
      log_swap_failure_once(SwapFailure::kScaledResolve, key,
                            "scaled resolve not supported");
      logged_reason = true;
    }
    MTL::PixelFormat pixel_format = GetPixelFormatForKey(key);
    if (pixel_format == MTL::PixelFormatInvalid) {
      log_swap_failure_once(SwapFailure::kInvalidPixelFormat, key,
                            "invalid Metal pixel format");
      logged_reason = true;
    }
    MTL::Device* device =
        command_processor_ ? command_processor_->GetMetalDevice() : nullptr;
    if (device && !SupportsPixelFormat(device, pixel_format)) {
      log_swap_failure_once(SwapFailure::kUnsupportedPixelFormat, key,
                            "unsupported Metal pixel format");
      logged_reason = true;
    }
    TextureCache::LoadShaderIndex load_shader = GetLoadShaderIndexForKey(key);
    if (load_shader == TextureCache::kLoadShaderIndexUnknown) {
      log_swap_failure_once(SwapFailure::kMissingLoadShader, key,
                            "missing load shader");
      logged_reason = true;
    } else {
      MTL::ComputePipelineState* load_pipeline =
          key.scaled_resolve
              ? load_pipelines_scaled_[static_cast<size_t>(load_shader)]
              : load_pipelines_[static_cast<size_t>(load_shader)];
      if (!load_pipeline) {
        log_swap_failure_once(SwapFailure::kMissingPipeline, key,
                              "missing load pipeline");
        logged_reason = true;
      }
    }
    if (!logged_reason) {
      log_swap_failure_once(SwapFailure::kLoad, key, "LoadTextureData failed");
    }
    return nullptr;
  }

  texture->MarkAsUsed();
  key = texture->key();
  width_scaled_out =
      key.GetWidth() * (key.scaled_resolve ? draw_resolution_scale_x() : 1);
  height_scaled_out =
      key.GetHeight() * (key.scaled_resolve ? draw_resolution_scale_y() : 1);
  format_out = key.format;
  return view;
}

// Normalize clamp modes to values Metal supports.
static xenos::ClampMode NormalizeClampModeStatic(xenos::ClampMode clamp_mode) {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    return xenos::ClampMode::kMirrorClampToEdge;
  }
  return clamp_mode;
}

// Shared helper: build SamplerParameters from fetch constant + filter
// overrides.
static MetalTextureCache::SamplerParameters BuildSamplerParametersFromFetch(
    const RegisterFile& regs, uint32_t fetch_constant,
    xenos::TextureFilter req_mag_filter, xenos::TextureFilter req_min_filter,
    xenos::TextureFilter req_mip_filter, xenos::AnisoFilter req_aniso_filter) {
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(fetch_constant);

  MetalTextureCache::SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y,
                                          fetch_clamp_z);
  parameters.clamp_x = NormalizeClampModeStatic(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampModeStatic(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampModeStatic(fetch_clamp_z);

  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(
      fetch, nullptr, nullptr, nullptr, nullptr, nullptr, &mip_min_level,
      &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;

  xenos::TextureFilter mag_filter =
      req_mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter
                                                             : req_mag_filter;
  xenos::TextureFilter min_filter =
      req_min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter
                                                             : req_min_filter;
  xenos::TextureFilter mip_filter =
      req_mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter
                                                             : req_mip_filter;
  bool min_mag_linear = (mag_filter == xenos::TextureFilter::kLinear) &&
                        (min_filter == xenos::TextureFilter::kLinear);
  bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint ||
      mip_filter == xenos::TextureFilter::kLinear;
  bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;

  xenos::AnisoFilter aniso_filter =
      req_aniso_filter == xenos::AnisoFilter::kUseFetchConst
          ? fetch.aniso_filter
          : req_aniso_filter;
  // Apply anisotropic override, but only for mipmapped textures
  // that are already using bilinear/trilinear filtering.
  if (cvars::anisotropic_override > -1 && cvars::anisotropic_override < 6 &&
      has_mips && !mip_base_map && min_mag_linear &&
      mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(cvars::anisotropic_override);
  }
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  parameters.aniso_filter = aniso_filter;

  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }

  parameters.mip_base_map = mip_base_map ? 1 : 0;

  return parameters;
}

MetalTextureCache::SamplerParameters MetalTextureCache::GetSamplerParameters(
    const DxbcShader::SamplerBinding& binding) const {
  return BuildSamplerParametersFromFetch(
      register_file(), binding.fetch_constant, binding.mag_filter,
      binding.min_filter, binding.mip_filter, binding.aniso_filter);
}

MTL::SamplerState* MetalTextureCache::GetOrCreateSampler(
    SamplerParameters parameters) {
  auto it = sampler_cache_.find(parameters.value);
  if (it != sampler_cache_.end()) {
    return it->second;
  }

  MTL::SamplerDescriptor* desc = MTL::SamplerDescriptor::alloc()->init();
  if (!desc) {
    XELOGE("Failed to allocate Metal sampler descriptor");
    return nullptr;
  }

  auto convert_clamp = [](xenos::ClampMode mode) {
    switch (mode) {
      case xenos::ClampMode::kRepeat:
        return MTL::SamplerAddressModeRepeat;
      case xenos::ClampMode::kMirroredRepeat:
        return MTL::SamplerAddressModeMirrorRepeat;
      case xenos::ClampMode::kClampToEdge:
        return MTL::SamplerAddressModeClampToEdge;
      case xenos::ClampMode::kMirrorClampToEdge:
        return MTL::SamplerAddressModeMirrorClampToEdge;
      case xenos::ClampMode::kClampToBorder:
        return MTL::SamplerAddressModeClampToBorderColor;
      default:
        return MTL::SamplerAddressModeClampToEdge;
    }
  };

  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
    desc->setMipFilter(MTL::SamplerMipFilterLinear);
    desc->setMaxAnisotropy(1u << (uint32_t(parameters.aniso_filter) - 1));
  } else {
    desc->setMinFilter(parameters.min_linear ? MTL::SamplerMinMagFilterLinear
                                             : MTL::SamplerMinMagFilterNearest);
    desc->setMagFilter(parameters.mag_linear ? MTL::SamplerMinMagFilterLinear
                                             : MTL::SamplerMinMagFilterNearest);
    desc->setMipFilter(parameters.mip_linear ? MTL::SamplerMipFilterLinear
                                             : MTL::SamplerMipFilterNearest);
    desc->setMaxAnisotropy(1);
  }

  desc->setSAddressMode(convert_clamp(xenos::ClampMode(parameters.clamp_x)));
  desc->setTAddressMode(convert_clamp(xenos::ClampMode(parameters.clamp_y)));
  desc->setRAddressMode(convert_clamp(xenos::ClampMode(parameters.clamp_z)));

  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc->setBorderColor(MTL::SamplerBorderColorOpaqueWhite);
      break;
    case xenos::BorderColor::k_ABGR_Black:
    case xenos::BorderColor::k_ACBYCR_Black:
    case xenos::BorderColor::k_ACBCRY_Black:
      // Metal doesn't support custom border colors. The YUV black variants
      // still need alpha 0 to stay closer to D3D12's null/border semantics.
      desc->setBorderColor(MTL::SamplerBorderColorTransparentBlack);
      break;
    default:
      desc->setBorderColor(MTL::SamplerBorderColorOpaqueBlack);
      break;
  }

  desc->setLodMinClamp(static_cast<float>(parameters.mip_min_level));
  float max_lod = parameters.mip_base_map
                      ? static_cast<float>(parameters.mip_min_level)
                      : FLT_MAX;
  if (parameters.mip_base_map &&
      parameters.aniso_filter == xenos::AnisoFilter::kDisabled &&
      !parameters.mip_linear) {
    max_lod += 0.25f;
  }
  desc->setLodMaxClamp(max_lod);
  desc->setLodAverage(false);
  desc->setSupportArgumentBuffers(true);

  MTL::SamplerState* sampler_state =
      command_processor_->GetMetalDevice()->newSamplerState(desc);
  desc->release();

  if (!sampler_state) {
    XELOGE("Failed to create Metal sampler state");
    return nullptr;
  }

  sampler_cache_.emplace(parameters.value, sampler_state);

  // Allocate a persistent bindless sampler slot and write the descriptor.
  uint32_t sampler_index = command_processor_->AllocateSamplerBindlessIndex();
  if (sampler_index == UINT32_MAX) {
    XELOGE("Failed to allocate persistent bindless sampler slot");
    sampler_state->release();
    sampler_cache_.erase(parameters.value);
    return nullptr;
  }
  if (auto* e =
          command_processor_->GetSamplerBindlessHeapEntry(sampler_index)) {
    IRDescriptorTableSetSampler(e, sampler_state, 0.0f);
    command_processor_->SetNativeMslSamplerBindlessState(sampler_index,
                                                         sampler_state);
  } else {
    command_processor_->ReleaseSamplerBindlessIndex(sampler_index);
    sampler_state->release();
    sampler_cache_.erase(parameters.value);
    return nullptr;
  }
  sampler_bindless_indices_.emplace(parameters.value, sampler_index);

  return sampler_state;
}

xenos::ClampMode MetalTextureCache::NormalizeClampMode(
    xenos::ClampMode clamp_mode) const {
  return NormalizeClampModeStatic(clamp_mode);
}

// GetHostFormatSwizzle implementation
uint32_t MetalTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;

    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_32_32_FLOAT:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;

    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
    case xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP:
    case xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB;

    case xenos::TextureFormat::k_6_5_5:
      // On the host, green bits in blue, blue bits in green.
      return XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G);

    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_2_10_10_10:
      // Stored as BGRA after endian swap; CPU path swaps to RGBA8.
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;

    default:
      return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
}

bool MetalTextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  switch (key.format) {
    case xenos::TextureFormat::k_16:
      return r16_selection_.unsigned_uses_float ||
             r16_selection_.signed_uses_float;
    case xenos::TextureFormat::k_16_16:
      return rg16_selection_.unsigned_uses_float ||
             rg16_selection_.signed_uses_float;
    case xenos::TextureFormat::k_16_16_16_16:
      return rgba16_selection_.unsigned_uses_float ||
             rgba16_selection_.signed_uses_float;
    case xenos::TextureFormat::k_10_11_11:
    case xenos::TextureFormat::k_11_11_10:
      return true;
    default:
      return false;
  }
}

bool MetalTextureCache::IsScaledResolveSupportedForFormat(
    TextureKey key) const {
  LoadShaderIndex load_shader = GetLoadShaderIndexForKey(key);
  return load_shader != kLoadShaderIndexUnknown &&
         load_pipelines_scaled_[load_shader] != nullptr;
}

bool MetalTextureCache::EnsureScaledResolveMemoryCommitted(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }
  if (!length_unscaled) {
    return true;
  }

  uint64_t start_scaled = 0;
  uint64_t length_scaled = 0;
  if (!GetScaledResolveRange(start_unscaled, length_unscaled,
                             length_scaled_alignment_log2, start_scaled,
                             length_scaled)) {
    return false;
  }
  return EnsureScaledResolveBufferRange(start_scaled, length_scaled);
}

bool MetalTextureCache::MakeScaledResolveRangeCurrent(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) {
  if (!IsDrawResolutionScaled()) {
    return false;
  }
  if (!length_unscaled) {
    return false;
  }

  uint64_t start_scaled = 0;
  uint64_t length_scaled = 0;
  if (!GetScaledResolveRange(start_unscaled, length_unscaled,
                             length_scaled_alignment_log2, start_scaled,
                             length_scaled)) {
    return false;
  }
  if (!length_scaled) {
    return false;
  }

  uint64_t end_scaled = start_scaled + length_scaled - 1;
  for (size_t i = scaled_resolve_buffers_.size(); i-- > 0;) {
    const ScaledResolveBuffer& buffer = scaled_resolve_buffers_[i];
    uint64_t buffer_end = buffer.base_scaled + buffer.length_scaled - 1;
    if (start_scaled >= buffer.base_scaled && end_scaled <= buffer_end) {
      scaled_resolve_current_buffer_index_ = i;
      scaled_resolve_current_range_start_scaled_ = start_scaled;
      scaled_resolve_current_range_length_scaled_ = length_scaled;
      return true;
    }
  }
  return false;
}

bool MetalTextureCache::GetCurrentScaledResolveBuffer(
    MTL::Buffer*& buffer_out, size_t& buffer_offset_out,
    size_t& buffer_length_out) const {
  if (scaled_resolve_current_buffer_index_ == size_t(-1) ||
      scaled_resolve_current_buffer_index_ >= scaled_resolve_buffers_.size()) {
    return false;
  }
  const ScaledResolveBuffer& buffer =
      scaled_resolve_buffers_[scaled_resolve_current_buffer_index_];
  uint64_t offset =
      scaled_resolve_current_range_start_scaled_ - buffer.base_scaled;
  uint64_t end_offset = offset + scaled_resolve_current_range_length_scaled_;
  if (end_offset > buffer.length_scaled) {
    return false;
  }
  if (offset > std::numeric_limits<size_t>::max() ||
      scaled_resolve_current_range_length_scaled_ >
          std::numeric_limits<size_t>::max()) {
    return false;
  }
  buffer_out = buffer.buffer;
  buffer_offset_out = size_t(offset);
  buffer_length_out = size_t(scaled_resolve_current_range_length_scaled_);
  return buffer_out != nullptr;
}

bool MetalTextureCache::GetScaledResolveRange(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2, uint64_t& start_scaled_out,
    uint64_t& length_scaled_out) const {
  if (!length_unscaled) {
    start_scaled_out = 0;
    length_scaled_out = 0;
    return true;
  }
  if (start_unscaled >= SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    return false;
  }

  uint32_t scale_area = draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * scale_area;
  // The last unscaled byte maps to scale_area scaled bytes; end_scaled must
  // cover all of them (i.e. last_unscaled * scale_area + scale_area - 1),
  // matching the D3D12 backend's rounding of length_unscaled * scale_area.
  uint64_t length_scaled = uint64_t(length_unscaled) * scale_area;
  if (length_scaled_alignment_log2) {
    uint64_t alignment_mask = (uint64_t(1) << length_scaled_alignment_log2) - 1;
    length_scaled = (length_scaled + alignment_mask) & ~alignment_mask;
  }
  start_scaled_out = start_scaled;
  length_scaled_out = length_scaled;
  return true;
}

bool MetalTextureCache::IsScaledResolveRangeResident(
    uint32_t start_unscaled, uint32_t length_unscaled,
    uint32_t length_scaled_alignment_log2) const {
  if (!IsDrawResolutionScaled() || !length_unscaled) {
    return false;
  }

  uint64_t start_scaled = 0;
  uint64_t length_scaled = 0;
  if (!GetScaledResolveRange(start_unscaled, length_unscaled,
                             length_scaled_alignment_log2, start_scaled,
                             length_scaled) ||
      !length_scaled) {
    return false;
  }

  for (const ScaledResolveBuffer& buffer : scaled_resolve_buffers_) {
    if (!buffer.buffer || !buffer.length_scaled ||
        start_scaled < buffer.base_scaled) {
      continue;
    }
    uint64_t buffer_offset = start_scaled - buffer.base_scaled;
    if (buffer_offset > buffer.length_scaled) {
      continue;
    }
    uint64_t buffer_remaining = buffer.length_scaled - buffer_offset;
    if (length_scaled <= buffer_remaining) {
      return true;
    }
  }
  return false;
}

bool MetalTextureCache::EnsureScaledResolveBufferRange(uint64_t start_scaled,
                                                       uint64_t length_scaled) {
  if (!length_scaled) {
    return true;
  }
  uint64_t end_scaled = start_scaled + length_scaled - 1;

  for (const ScaledResolveBuffer& buffer : scaled_resolve_buffers_) {
    uint64_t buffer_end = buffer.base_scaled + buffer.length_scaled - 1;
    if (start_scaled >= buffer.base_scaled && end_scaled <= buffer_end) {
      return true;
    }
  }

  uint64_t new_base_scaled = start_scaled;
  uint64_t new_end_scaled = end_scaled;
  std::vector<size_t> overlap_indices;
  overlap_indices.reserve(scaled_resolve_buffers_.size());

  for (size_t i = 0; i < scaled_resolve_buffers_.size(); ++i) {
    const ScaledResolveBuffer& buffer = scaled_resolve_buffers_[i];
    uint64_t buffer_end = buffer.base_scaled + buffer.length_scaled - 1;
    if (buffer.base_scaled <= end_scaled && buffer_end >= start_scaled) {
      overlap_indices.push_back(i);
      new_base_scaled = std::min(new_base_scaled, buffer.base_scaled);
      new_end_scaled = std::max(new_end_scaled, buffer_end);
    }
  }

  uint64_t new_length_scaled = new_end_scaled - new_base_scaled + 1;
  new_length_scaled = xe::align(new_length_scaled, uint64_t(16));
  if (new_length_scaled > std::numeric_limits<size_t>::max()) {
    XELOGE("Metal scaled resolve: buffer size too large ({} bytes)",
           new_length_scaled);
    return false;
  }

  MTL::Device* device = command_processor_->GetMetalDevice();
  if (!device) {
    XELOGE("Metal scaled resolve: missing Metal device");
    return false;
  }
  if (new_length_scaled > device->maxBufferLength()) {
    XELOGE("Metal scaled resolve: requested {} bytes exceeds maxBufferLength",
           new_length_scaled);
    return false;
  }

  MTL::Buffer* new_buffer = device->newBuffer(size_t(new_length_scaled),
                                              MTL::ResourceStorageModePrivate);
  if (!new_buffer) {
    XELOGE("Metal scaled resolve: failed to allocate {} bytes",
           new_length_scaled);
    return false;
  }
  new_buffer->setLabel(
      NS::String::string("XeniaScaledResolveBuffer", NS::UTF8StringEncoding));

  uint64_t overlap_total_bytes = 0;
  for (size_t overlap_index : overlap_indices) {
    overlap_total_bytes += scaled_resolve_buffers_[overlap_index].length_scaled;
  }
  bool retain_overlaps = command_processor_->HasActiveSubmission();
  if (retain_overlaps) {
    bool exceeds_retired_budget =
        overlap_total_bytes > kScaledResolveRetiredMaxBytes ||
        scaled_resolve_retired_bytes_ >
            (kScaledResolveRetiredMaxBytes - overlap_total_bytes);
    if (exceeds_retired_budget) {
      retain_overlaps = false;
    }
  }

  if (!overlap_indices.empty()) {
    bool standalone = false;
    MTL::CommandBuffer* cmd =
        retain_overlaps ? command_processor_->GetCurrentCommandBuffer()
                        : nullptr;
    if (!cmd) {
      cmd = command_processor_->CreateStandaloneTransferCommandBuffer(
          "XeniaCB reason=scaled-resolve-blit");
      if (!cmd) {
        new_buffer->release();
        return false;
      }
      standalone = true;
    }

    if (!standalone && command_processor_ &&
        cmd == command_processor_->GetCurrentCommandBuffer()) {
      command_processor_->EndSharedMemoryUploadBlitEncoder(
          MetalCommandProcessor::SharedMemoryUploadEncoderEndReason::
              kScaledResolveBlit);
    }
    MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
    if (!blit) {
      if (standalone) {
        cmd->release();
      }
      new_buffer->release();
      return false;
    }

    for (size_t index : overlap_indices) {
      const ScaledResolveBuffer& old_buffer = scaled_resolve_buffers_[index];
      uint64_t dst_offset = old_buffer.base_scaled - new_base_scaled;
      if (dst_offset > std::numeric_limits<size_t>::max()) {
        continue;
      }
      blit->copyFromBuffer(old_buffer.buffer, 0, new_buffer, size_t(dst_offset),
                           size_t(old_buffer.length_scaled));
    }

    blit->endEncoding();
    if (standalone) {
      command_processor_->CommitStandaloneAndWait(cmd);
    }
  }

  std::vector<ScaledResolveBuffer> new_buffers;
  new_buffers.reserve(scaled_resolve_buffers_.size() - overlap_indices.size() +
                      1);
  for (size_t i = 0; i < scaled_resolve_buffers_.size(); ++i) {
    bool overlapping = false;
    for (size_t overlap_index : overlap_indices) {
      if (overlap_index == i) {
        overlapping = true;
        break;
      }
    }
    if (overlapping) {
      if (retain_overlaps) {
        RetiredScaledResolveBuffer retired;
        retired.buffer = scaled_resolve_buffers_[i].buffer;
        retired.submission_id = command_processor_->GetCurrentSubmission();
        retired.length_scaled = scaled_resolve_buffers_[i].length_scaled;
        scaled_resolve_retired_buffers_.push_back(retired);
        if (std::numeric_limits<uint64_t>::max() -
                scaled_resolve_retired_bytes_ <
            retired.length_scaled) {
          scaled_resolve_retired_bytes_ = std::numeric_limits<uint64_t>::max();
        } else {
          scaled_resolve_retired_bytes_ += retired.length_scaled;
        }
      } else if (scaled_resolve_buffers_[i].buffer) {
        scaled_resolve_buffers_[i].buffer->release();
      }
      continue;
    }
    new_buffers.push_back(scaled_resolve_buffers_[i]);
  }

  ScaledResolveBuffer new_entry;
  new_entry.buffer = new_buffer;
  new_entry.base_scaled = new_base_scaled;
  new_entry.length_scaled = new_length_scaled;
  new_buffers.push_back(new_entry);

  scaled_resolve_buffers_.swap(new_buffers);
  scaled_resolve_current_buffer_index_ = size_t(-1);
  scaled_resolve_current_range_start_scaled_ = 0;
  scaled_resolve_current_range_length_scaled_ = 0;

  return true;
}

// GetMaxHostTextureWidthHeight implementation
uint32_t MetalTextureCache::GetMaxHostTextureWidthHeight(
    xenos::DataDimension dimension) const {
  // Metal supports up to 16384x16384 for 2D textures on most devices
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return 16384;
    case xenos::DataDimension::k2DOrStacked:
      return 16384;
    case xenos::DataDimension::k3D:
      return 2048;  // 3D textures have lower limits
    case xenos::DataDimension::kCube:
      return 16384;
    default:
      return 16384;
  }
}

// GetMaxHostTextureDepthOrArraySize implementation
uint32_t MetalTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  // Metal array and 3D texture limits
  switch (dimension) {
    case xenos::DataDimension::k1D:
      return 2048;  // Array size limit
    case xenos::DataDimension::k2DOrStacked:
      return 2048;  // Array size limit
    case xenos::DataDimension::k3D:
      return 2048;  // Depth limit for 3D textures
    case xenos::DataDimension::kCube:
      return 2048;  // Array size for cube arrays
    default:
      return 2048;
  }
}

// CreateTexture implementation - creates MetalTexture from TextureKey
std::unique_ptr<TextureCache::Texture> MetalTextureCache::CreateTexture(
    TextureKey key) {
  SCOPE_profile_cpu_f("gpu");

  MTL::PixelFormat metal_format = GetPixelFormatForKey(key);
  if (metal_format == MTL::PixelFormatInvalid) {
    XELOGE("CreateTexture: Unsupported texture format {}",
           static_cast<uint32_t>(key.format));
    return nullptr;
  }

  MTL::TextureSwizzleChannels metal_swizzle =
      ToMetalTextureSwizzle(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);

  MTL::Texture* metal_texture = nullptr;
  uint32_t width = key.GetWidth();
  uint32_t height = key.GetHeight();
  if (key.scaled_resolve) {
    width *= draw_resolution_scale_x();
    height *= draw_resolution_scale_y();
  }

  // Create Metal texture based on dimension
  switch (key.dimension) {
    case xenos::DataDimension::k1D: {
      metal_texture =
          CreateTexture2D(width, height, key.GetDepthOrArraySize(),
                          metal_format, metal_swizzle, key.mip_max_level + 1);
      break;
    }
    case xenos::DataDimension::k2DOrStacked: {
      metal_texture =
          CreateTexture2D(width, height, key.GetDepthOrArraySize(),
                          metal_format, metal_swizzle, key.mip_max_level + 1);
      break;
    }
    case xenos::DataDimension::k3D: {
      metal_texture =
          CreateTexture3D(width, height, key.GetDepthOrArraySize(),
                          metal_format, metal_swizzle, key.mip_max_level + 1);
      break;
    }
    case xenos::DataDimension::kCube: {
      uint32_t array_size = key.GetDepthOrArraySize();
      if (array_size % 6 != 0) {
        XELOGW(
            "CreateTexture: Cube texture array size {} is not divisible by 6",
            array_size);
      }
      uint32_t cube_count = std::max(1u, array_size / 6);
      metal_texture = CreateTextureCube(width, metal_format, metal_swizzle,
                                        key.mip_max_level + 1, cube_count);
      break;
    }
    default: {
      XELOGE("CreateTexture: Unsupported texture dimension {}",
             static_cast<uint32_t>(key.dimension));
      return nullptr;
    }
  }

  if (!metal_texture) {
    XELOGE("CreateTexture: Failed to create Metal texture");
    return nullptr;
  }

  EnsureViewBindlessHeadroom(kViewBindlessHeapPressureThreshold);

  // Create the MetalTexture wrapper. The canonical bindless descriptor is
  // allocated lazily on first bind so unused cached textures don't pin a view
  // slot permanently.
  return std::make_unique<MetalTexture>(*this, key, metal_texture);
}

// LoadTextureDataFromResidentMemoryImpl implementation
bool MetalTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture,
                                                              bool load_base,
                                                              bool load_mips) {
  SCOPE_profile_cpu_f("gpu");
  ++loaded_texture_data_count_;

  MetalTexture* metal_texture = static_cast<MetalTexture*>(&texture);
  if (!metal_texture || !metal_texture->metal_texture()) {
    XELOGE("LoadTextureDataFromResidentMemoryImpl: Invalid Metal texture");
    return false;
  }

  // Upload work references the destination texture before normal binding does,
  // so keep it protected from LRU eviction like the D3D12 and Vulkan paths do.
  texture.MarkAsUsed();

  // GPU-based loading path for Metal texture_load_* shaders only (parity with
  // D3D12/Vulkan; no CPU untile fallback).
  if (!TryGpuLoadTexture(texture, load_base, load_mips)) {
    return false;
  }
  // If this is a 3D base texture (not a wrapper itself), drop the cached 2D
  // wrapper so it is re-built from the freshly-loaded base data on next use.
  if (!metal_texture->is_3d_as_2d_wrapper_ &&
      texture.key().dimension == xenos::DataDimension::k3D) {
    metal_texture->texture_3d_as_2d_.reset();
  }
  if (command_processor_) {
    const TextureKey& texture_key = texture.key();
    const uint64_t upload_bytes =
        (load_base ? uint64_t(xe::align(texture.GetGuestBaseSize(),
                                        UINT32_C(16)))
                   : 0) +
        (load_mips ? uint64_t(xe::align(texture.GetGuestMipsSize(),
                                        UINT32_C(16)))
                   : 0);
    command_processor_->RecordTextureUploadSourceRoute(
        texture_key.scaled_resolve
            ? MetalCommandProcessor::TextureUploadSourceRoute::kScaledResolve
            : MetalCommandProcessor::TextureUploadSourceRoute::
                  kResidentSharedMemory,
        upload_bytes);
  }
  return true;
}

bool MetalTextureCache::LoadTextureDataFromCpuGuestMemory(Texture& texture,
                                                          bool load_base,
                                                          bool load_mips) {
  SCOPE_profile_cpu_f("gpu");
  if (!load_base && !load_mips) {
    return true;
  }
  const TextureKey& texture_key = texture.key();
  if (texture_key.scaled_resolve) {
    return false;
  }

  bool base_outdated = false;
  bool mips_outdated = false;
  {
    auto global_lock = AcquireGlobalLock();
    base_outdated = load_base && texture.base_outdated(global_lock);
    mips_outdated = load_mips && texture.mips_outdated(global_lock);
  }
  if (!base_outdated && !mips_outdated) {
    if (command_processor_) {
      command_processor_->RecordTextureUploadExecutionDetail(
          MetalCommandProcessor::TextureUploadExecutionDetail::
              kCpuSourceAlreadyCurrent);
    }
    return true;
  }

  const uint32_t base_length =
      xe::align(texture.GetGuestBaseSize(), UINT32_C(16));
  const uint32_t mips_length =
      xe::align(texture.GetGuestMipsSize(), UINT32_C(16));
  auto has_cpu_source = [&](bool needed, uint32_t start, uint32_t length) {
    if (!needed || !length) {
      return true;
    }
    return shared_memory().IsRangeInvalid(start, length);
  };
  if (!has_cpu_source(base_outdated, texture_key.base_page << 12,
                      base_length) ||
      !has_cpu_source(mips_outdated, texture_key.mip_page << 12,
                      mips_length)) {
    return false;
  }
  MetalTexture* metal_texture = static_cast<MetalTexture*>(&texture);
  if (!metal_texture || !metal_texture->metal_texture()) {
    XELOGE("LoadTextureDataFromCpuGuestMemory: Invalid Metal texture");
    return false;
  }

  // Upload work references the destination texture before normal binding does,
  // so keep it protected from LRU eviction like the resident-memory path.
  texture.MarkAsUsed();
  ++loaded_texture_data_count_;

  if (!TryGpuLoadTexture(texture, base_outdated, mips_outdated,
                         TextureLoadSourceMode::kCpuGuestMemory)) {
    return false;
  }
  // Drop the cached 2D wrapper so it is re-built from the fresh base data.
  if (!metal_texture->is_3d_as_2d_wrapper_ &&
      texture_key.dimension == xenos::DataDimension::k3D) {
    metal_texture->texture_3d_as_2d_.reset();
  }

  {
    auto global_lock = AcquireGlobalLock();
    texture.MakeLoadedDataUpToDateAndWatch(global_lock, base_outdated,
                                           mips_outdated);
  }
  if (command_processor_) {
    command_processor_->RecordTextureUploadSourceRoute(
        MetalCommandProcessor::TextureUploadSourceRoute::kCpuGuestMemory,
        (base_outdated ? uint64_t(base_length) : 0) +
            (mips_outdated ? uint64_t(mips_length) : 0));
  }
  texture.LogAction("Loaded");
  return true;
}

bool MetalTextureCache::EnsureViewBindlessHeadroom(
    uint32_t target_free_slots) const {
  if (!command_processor_) {
    return false;
  }
  if (command_processor_->GetViewBindlessHeapAvailableCount() >=
      target_free_slots) {
    return true;
  }
  const_cast<MetalTextureCache*>(this)->TrimViewBindlessPressure(
      target_free_slots);
  return command_processor_->GetViewBindlessHeapAvailableCount() >=
         target_free_slots;
}

// MetalTexture implementation
MetalTextureCache::MetalTexture::MetalTexture(MetalTextureCache& texture_cache,
                                              const TextureKey& key,
                                              MTL::Texture* metal_texture,
                                              bool track_usage,
                                              bool is_3d_as_2d_wrapper)
    : Texture(texture_cache, key, track_usage),
      texture_cache_(texture_cache),
      metal_texture_(metal_texture),
      is_3d_as_2d_wrapper_(is_3d_as_2d_wrapper) {
  if (metal_texture_) {
    SetHostMemoryUsage(EstimateTextureBytes(metal_texture_));
  }
}

MetalTextureCache::MetalTexture::~MetalTexture() {
  // Release persistent bindless SRV slots before destroying the texture.
  if (bindless_srv_index_ != UINT32_MAX) {
    texture_cache_.command_processor_->ReleaseViewBindlessIndex(
        bindless_srv_index_);
    bindless_srv_index_ = UINT32_MAX;
  }
  for (auto& [key, bindless_srv_index] : swizzled_view_bindless_srv_indices_) {
    if (bindless_srv_index != UINT32_MAX) {
      texture_cache_.command_processor_->ReleaseViewBindlessIndex(
          bindless_srv_index);
    }
  }
  swizzled_view_bindless_srv_indices_.clear();

  uint64_t views_released = 0;
  for (auto& entry : swizzled_view_cache_) {
    if (entry.second) {
      ++views_released;
      entry.second->release();
    }
  }
  if (metal_texture_) {
    metal_texture_->release();
    metal_texture_ = nullptr;
  }
}

uint64_t MetalTextureCache::MetalTexture::GetViewKey(
    uint32_t host_swizzle, xenos::FetchOpDimension dimension, bool is_signed,
    MTL::PixelFormat view_format) const {
  return uint64_t(host_swizzle) | (uint64_t(dimension) << 32) |
         (uint64_t(is_signed) << 40) | (uint64_t(view_format) << 48);
}

MTL::PixelFormat MetalTextureCache::MetalTexture::GetViewPixelFormat(
    bool is_signed) const {
  if (!metal_texture_ || !is_signed) {
    return metal_texture_ ? metal_texture_->pixelFormat()
                          : MTL::PixelFormatInvalid;
  }
  switch (key().format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
      return MTL::PixelFormatR8Snorm;
    case xenos::TextureFormat::k_8_8:
      return MTL::PixelFormatRG8Snorm;
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
      return MTL::PixelFormatRGBA8Snorm;
    case xenos::TextureFormat::k_16:
      return MTL::PixelFormatR16Snorm;
    case xenos::TextureFormat::k_16_16:
      return MTL::PixelFormatRG16Snorm;
    case xenos::TextureFormat::k_16_16_16_16:
      return MTL::PixelFormatRGBA16Snorm;
    default:
      return metal_texture_->pixelFormat();
  }
}

MTL::TextureType MetalTextureCache::MetalTexture::GetViewType(
    xenos::FetchOpDimension dimension) const {
  if (!metal_texture_) {
    return MTL::TextureType2D;
  }
  switch (dimension) {
    case xenos::FetchOpDimension::kCube:
      return MTL::TextureTypeCube;
    case xenos::FetchOpDimension::k3DOrStacked:
      return key().dimension == xenos::DataDimension::k3D
                 ? MTL::TextureType3D
                 : MTL::TextureType2DArray;
    default:
      return MTL::TextureType2DArray;
  }
}

uint32_t
MetalTextureCache::MetalTexture::GetOrCreateBindlessSRVIndexForResolvedView(
    uint64_t view_key, MTL::Texture* view) {
  if (!view) {
    return UINT32_MAX;
  }

  texture_cache_.EnsureViewBindlessHeadroom(kViewBindlessHeapPressureThreshold);

  auto existing = swizzled_view_bindless_srv_indices_.find(view_key);
  if (existing != swizzled_view_bindless_srv_indices_.end()) {
    return existing->second;
  }

  uint32_t bindless_srv_index =
      texture_cache_.command_processor_->AllocateViewBindlessIndex();
  if (bindless_srv_index == UINT32_MAX) {
    return UINT32_MAX;
  }
  auto* entry = texture_cache_.command_processor_->GetViewBindlessHeapEntry(
      bindless_srv_index);
  if (!entry) {
    texture_cache_.command_processor_->ReleaseViewBindlessIndex(
        bindless_srv_index);
    return UINT32_MAX;
  }
  IRDescriptorTableSetTexture(entry, view, 0.0f, 0);
  texture_cache_.command_processor_->SetNativeMslViewBindlessTexture(
      bindless_srv_index, view);
  swizzled_view_bindless_srv_indices_.emplace(view_key, bindless_srv_index);
  return bindless_srv_index;
}

MTL::Texture* MetalTextureCache::MetalTexture::GetOrCreateView(
    uint32_t host_swizzle, xenos::FetchOpDimension dimension, bool is_signed) {
  if (!metal_texture_) {
    return nullptr;
  }

  MTL::PixelFormat view_format = GetViewPixelFormat(is_signed);
  MTL::TextureType view_type = GetViewType(dimension);

  if (host_swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA &&
      view_format == metal_texture_->pixelFormat() &&
      metal_texture_->textureType() == view_type) {
    return metal_texture_;
  }

  uint64_t view_key =
      GetViewKey(host_swizzle, dimension, is_signed, view_format);
  auto found = swizzled_view_cache_.find(view_key);
  if (found != swizzled_view_cache_.end()) {
    return found->second;
  }

  uint32_t slice_count = 1;
  switch (view_type) {
    case MTL::TextureType2DArray:
      slice_count = metal_texture_->arrayLength();
      break;
    case MTL::TextureTypeCube:
      slice_count = 6;
      break;
    case MTL::TextureTypeCubeArray:
      slice_count = metal_texture_->arrayLength() * 6;
      break;
    case MTL::TextureType3D:
      // Metal requires a single slice range for 3D texture views.
      slice_count = 1;
      break;
    default:
      slice_count = 1;
      break;
  }

  NS::Range level_range =
      NS::Range::Make(0, metal_texture_->mipmapLevelCount());
  NS::Range slice_range = NS::Range::Make(0, slice_count);
  MTL::TextureSwizzleChannels swizzle = ToMetalTextureSwizzle(host_swizzle);
  MTL::Texture* view = metal_texture_->newTextureView(
      view_format, view_type, level_range, slice_range, swizzle);
  if (!view) {
    return nullptr;
  }

  swizzled_view_cache_.emplace(view_key, view);
  return view;
}

uint32_t MetalTextureCache::MetalTexture::GetOrCreateBindlessSRVIndexAndView(
    uint32_t host_swizzle, xenos::FetchOpDimension dimension, bool is_signed,
    MTL::Texture** view_out) {
  if (view_out) {
    *view_out = nullptr;
  }
  if (!metal_texture_) {
    return UINT32_MAX;
  }

  MetalTexture* resolved_texture = this;
  MTL::Texture* view = nullptr;
  if (!is_3d_as_2d_wrapper_ && key().dimension == xenos::DataDimension::k3D &&
      dimension == xenos::FetchOpDimension::k2D) {
    view = GetOrCreate3DAs2DView(host_swizzle, dimension, is_signed);
    if (!view || !texture_3d_as_2d_) {
      return UINT32_MAX;
    }
    resolved_texture = texture_3d_as_2d_.get();
  } else {
    view = GetOrCreateView(host_swizzle, dimension, is_signed);
    if (!view) {
      return UINT32_MAX;
    }
  }

  MTL::PixelFormat view_format =
      resolved_texture->GetViewPixelFormat(is_signed);
  MTL::TextureType view_type = resolved_texture->GetViewType(dimension);
  uint32_t srv_index = UINT32_MAX;
  if (host_swizzle == xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA &&
      view_format == resolved_texture->metal_texture_->pixelFormat() &&
      resolved_texture->metal_texture_->textureType() == view_type) {
    if (resolved_texture->bindless_srv_index_ == UINT32_MAX) {
      texture_cache_.EnsureViewBindlessHeadroom(
          kViewBindlessHeapPressureThreshold);
      resolved_texture->bindless_srv_index_ =
          texture_cache_.command_processor_->AllocateViewBindlessIndex();
      if (resolved_texture->bindless_srv_index_ == UINT32_MAX) {
        XELOGE("MetalTexture: failed to allocate default bindless SRV index");
        return UINT32_MAX;
      }
      auto* entry = texture_cache_.command_processor_->GetViewBindlessHeapEntry(
          resolved_texture->bindless_srv_index_);
      if (!entry) {
        texture_cache_.command_processor_->ReleaseViewBindlessIndex(
            resolved_texture->bindless_srv_index_);
        resolved_texture->bindless_srv_index_ = UINT32_MAX;
        return UINT32_MAX;
      }
      IRDescriptorTableSetTexture(entry, resolved_texture->metal_texture_, 0.0f,
                                  0);
      texture_cache_.command_processor_->SetNativeMslViewBindlessTexture(
          resolved_texture->bindless_srv_index_,
          resolved_texture->metal_texture_);
    }
    srv_index = resolved_texture->bindless_srv_index_;
  } else {
    uint64_t view_key = resolved_texture->GetViewKey(host_swizzle, dimension,
                                                     is_signed, view_format);
    auto existing =
        resolved_texture->swizzled_view_bindless_srv_indices_.find(view_key);
    if (existing !=
        resolved_texture->swizzled_view_bindless_srv_indices_.end()) {
      srv_index = existing->second;
    } else {
      srv_index = resolved_texture->GetOrCreateBindlessSRVIndexForResolvedView(
          view_key, view);
    }
  }

  if (srv_index == UINT32_MAX) {
    return UINT32_MAX;
  }
  if (view_out) {
    *view_out = view;
  }
  return srv_index;
}

MTL::Texture* MetalTextureCache::MetalTexture::GetOrCreate3DAs2DView(
    uint32_t host_swizzle, xenos::FetchOpDimension dimension, bool is_signed) {
  if (!metal_texture_ || key().dimension != xenos::DataDimension::k3D) {
    return nullptr;
  }
  if (!::cvars::gpu_3d_to_2d_texture) {
    return nullptr;
  }

  if (!texture_3d_as_2d_) {
    TextureKey key_2d = key();
    key_2d.depth_or_array_size_minus_1 = 0;
    key_2d.mip_max_level = 0;

    uint32_t width = key_2d.GetWidth();
    uint32_t height = key_2d.GetHeight();
    if (key_2d.scaled_resolve && texture_cache_.IsDrawResolutionScaled()) {
      width *= texture_cache_.draw_resolution_scale_x();
      height *= texture_cache_.draw_resolution_scale_y();
    }

    MTL::TextureSwizzleChannels metal_swizzle =
        ToMetalTextureSwizzle(xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA);
    MTL::Texture* texture_2d = texture_cache_.CreateTexture2D(
        width, height, 1, metal_texture_->pixelFormat(), metal_swizzle, 1);
    if (!texture_2d) {
      XELOGE("MetalTexture: Failed to create 3D-as-2D wrapper texture");
      return nullptr;
    }

    texture_3d_as_2d_ = std::make_unique<MetalTexture>(texture_cache_, key_2d,
                                                       texture_2d, false, true);
    texture_3d_as_2d_->SetForceLoad3DTiling(true);
    if (!texture_cache_.LoadTextureData(*texture_3d_as_2d_)) {
      XELOGE("MetalTexture: Failed to load 3D-as-2D texture data");
      texture_3d_as_2d_.reset();
      return nullptr;
    }
  }

  return texture_3d_as_2d_->GetOrCreateView(host_swizzle, dimension, is_signed);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
