/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
#define XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/texture_cache.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"

#include "third_party/metal-cpp/Metal/Metal.hpp"

namespace xe {
namespace gpu {
namespace metal {

class MetalCommandProcessor;
class MetalSharedMemory;
class MetalHeapPool;

class MetalTextureCache : public TextureCache {
 public:
  MetalTextureCache(MetalCommandProcessor* command_processor,
                    const RegisterFile& register_file,
                    MetalSharedMemory& shared_memory,
                    uint32_t draw_resolution_scale_x,
                    uint32_t draw_resolution_scale_y);
  ~MetalTextureCache();

  bool Initialize();
  void Shutdown();
  // Settle standalone upload tickets before the command processor joins
  // callbacks.
  void FinishPendingUploads() { AbortUploadCommandBufferBatch(); }
  void ClearCache() override;
  void CompletedSubmissionUpdated(uint64_t completed_submission_index) override;

  bool UploadTexture2D(const TextureInfo& texture_info);
  bool UploadTextureCube(const TextureInfo& texture_info);

  // Pixel format conversion
  MTL::PixelFormat ConvertXenosFormat(
      xenos::TextureFormat format,
      xenos::Endian endian = xenos::Endian::k8in32);

  // Null texture accessors for invalid bindings (following D3D12/Vulkan
  // pattern)
  MTL::Texture* GetNullTexture2D() const { return null_texture_2d_; }
  MTL::Texture* GetNullTexture3D() const { return null_texture_3d_; }
  MTL::Texture* GetNullTextureCube(bool as_array = false) const {
    return as_array ? null_texture_cube_array_ : null_texture_cube_;
  }

  // Loads the 2D views of the 3D textures the shaders sample as 1D or 2D. Call
  // while the draw requests its textures: binding would otherwise load them
  // with the draw's render pass open, in a command buffer committed ahead of
  // the submission and its earlier resolves.
  void Load3DAs2DViews(const SpirvShader& vertex_shader,
                       const SpirvShader* pixel_shader);
  MTL::Texture* GetTextureForBinding(uint32_t fetch_constant,
                                     xenos::FetchOpDimension dimension,
                                     bool is_signed,
                                     bool cube_as_array = false);

  MTL::Texture* RequestSwapTexture(uint32_t& width_scaled_out,
                                   uint32_t& height_scaled_out,
                                   xenos::TextureFormat& format_out);

  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;
      xenos::ClampMode clamp_y : 3;
      xenos::ClampMode clamp_z : 3;
      xenos::BorderColor border_color : 2;
      uint32_t mag_linear : 1;
      uint32_t min_linear : 1;
      uint32_t mip_linear : 1;
      xenos::AnisoFilter aniso_filter : 3;
      uint32_t mip_min_level : 4;
      uint32_t mip_base_map : 1;
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& other) const {
      return value == other.value;
    }
    bool operator!=(const SamplerParameters& other) const {
      return value != other.value;
    }
  };

  SamplerParameters GetSamplerParameters(
      const SpirvShader::SamplerBinding& binding) const;
  MTL::SamplerState* GetOrCreateSampler(SamplerParameters parameters);

  // TextureCache virtual method overrides
  void RequestTextures(uint32_t used_texture_mask) override;
  uint64_t binding_state_generation() const {
    return binding_state_generation_;
  }

  // Narrow producer interface for an existing, exact texture-cache image
  // whose loader copies raw 32-bit or 64-bit texels. The render-target backend
  // may write the same logical texels there while it performs the required
  // guest-memory resolve, which covers the whole texture. Publication, right
  // after the resolve marked the range GPU-written, is only accepted if the
  // range is still valid in shared memory (no CPU write since), and installs
  // the ordinary cache watch immediately, so later writes take the normal path.
  struct ResolveTextureRefreshTarget {
    MTL::Texture* write_texture = nullptr;
    void* cache_texture = nullptr;
    uint32_t length = 0;

    explicit operator bool() const {
      return write_texture && cache_texture && length;
    }
  };
  // Leaves target_out empty if there is no single exact texture.
  void PrepareDirectResolveTextureRefresh(
      xenos::ColorFormat dest_format, uint32_t address, uint32_t length,
      uint32_t base_address, uint32_t width, uint32_t height, uint32_t pitch,
      xenos::Endian endianness, ResolveTextureRefreshTarget& target_out);
  void PublishDirectResolveTextureRefresh(
      const ResolveTextureRefreshTarget& target);

  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  bool IsScaledResolveSupportedForFormat(TextureKey key) const override;
  bool EnsureScaledResolveMemoryCommitted(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2 = 0) override;
  bool MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                     uint32_t length_unscaled,
                                     uint32_t length_scaled_alignment_log2 = 0);
  bool GetCurrentScaledResolveBuffer(MTL::Buffer*& buffer_out,
                                     size_t& buffer_offset_out,
                                     size_t& buffer_length_out) const;
  uint64_t GetCurrentScaledResolveRangeStartScaled() const {
    return scaled_resolve_current_range_start_scaled_;
  }
  uint64_t GetCurrentScaledResolveRangeLengthScaled() const {
    return scaled_resolve_current_range_length_scaled_;
  }
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(
      xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(
      xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  // GPU-based texture loading entry point. Returns true on success.
  bool TryGpuLoadTexture(Texture& texture, bool load_base, bool load_mips);
  MTL::StorageMode GetCacheTextureStorageMode() const;
  bool ShouldUploadViaBlit() const;
  void BeginUploadCommandBufferBatch();
  // Creates the batch command buffer on the first upload that wants it, so a
  // request that uploads nothing costs none.
  MTL::CommandBuffer* EnsureUploadCommandBufferBatch();
  void EndUploadCommandBufferBatch();
  void AbortUploadCommandBufferBatch(bool commit_if_has_work = true);

  // Format / load shader mapping for Metal texture loading.
  bool IsDecompressionNeededForKey(TextureKey key) const;
  LoadShaderIndex GetLoadShaderIndexForKey(TextureKey key) const;
  MTL::PixelFormat GetPixelFormatForKey(TextureKey key) const;

  // Initialize GPU texture_load_* pipelines for Metal.
  bool InitializeLoadPipelines();

  struct Norm16Selection {
    bool unsigned_uses_float = false;
    bool signed_uses_float = false;
  };

  void InitializeNorm16Selection(MTL::Device* device);

  // Metal compute pipelines for texture_load_* shaders (unscaled and
  // resolution-scaled variants), indexed by TextureCache::LoadShaderIndex.
  MTL::ComputePipelineState* load_pipelines_[kLoadShaderCount] = {};
  MTL::ComputePipelineState* load_pipelines_scaled_[kLoadShaderCount] = {};
  MTL::ComputePipelineState* load_direct_raw32_pipeline_ = nullptr;
  MTL::ComputePipelineState* load_direct_raw64_pipeline_ = nullptr;

  // Metal-specific Texture implementation

  class MetalTexture : public Texture {
   public:
    MetalTexture(MetalTextureCache& texture_cache, const TextureKey& key,
                 MTL::Texture* metal_texture, bool track_usage = true);
    ~MetalTexture() override;

    MTL::Texture* metal_texture() const { return metal_texture_; }
    MTL::Texture* GetOrCreateView(uint32_t host_swizzle,
                                  xenos::FetchOpDimension dimension,
                                  bool is_signed, bool cube_as_array = false);
    MTL::Texture* GetOrCreate3DAs2DView(uint32_t host_swizzle,
                                        xenos::FetchOpDimension dimension,
                                        bool is_signed);
    void Invalidate3DAs2DView() { texture_3d_as_2d_.reset(); }
    // Integer reinterpretation of the image for the resolve refresh write.
    MTL::Texture* GetOrCreateResolveWriteView();

   private:
    friend class MetalTextureCache;
    MetalTextureCache& texture_cache_;
    MTL::Texture* metal_texture_;
    std::unique_ptr<MetalTexture> texture_3d_as_2d_;
    MTL::Texture* resolve_write_view_ = nullptr;
    std::unordered_map<uint64_t, MTL::Texture*> swizzled_view_cache_;
    MetalTexture* resolve_refresh_previous_ = nullptr;
    MetalTexture* resolve_refresh_next_ = nullptr;
    bool resolve_refresh_registered_ = false;
  };

 private:
  // Metal texture creation helpers
  MTL::Texture* CreateTexture2D(uint32_t width, uint32_t height,
                                uint32_t array_length, MTL::PixelFormat format,
                                MTL::TextureSwizzleChannels swizzle,
                                uint32_t mip_levels = 1,
                                bool shader_write = false);
  MTL::Texture* CreateTexture3D(uint32_t width, uint32_t height, uint32_t depth,
                                MTL::PixelFormat format,
                                MTL::TextureSwizzleChannels swizzle,
                                uint32_t mip_levels = 1);
  MTL::Texture* CreateTextureCube(uint32_t width, MTL::PixelFormat format,
                                  MTL::TextureSwizzleChannels swizzle,
                                  uint32_t mip_levels = 1,
                                  uint32_t cube_count = 1);
  void DumpTextureToFile(MTL::Texture* texture, const std::string& filename,
                         uint32_t width, uint32_t height);

  struct ScaledResolveBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t base_scaled = 0;
    uint64_t length_scaled = 0;
  };
  struct RetiredScaledResolveBuffer {
    MTL::Buffer* buffer = nullptr;
    uint64_t submission_id = 0;
    uint64_t length_scaled = 0;
  };

  bool GetScaledResolveRange(uint32_t start_unscaled, uint32_t length_unscaled,
                             uint32_t length_scaled_alignment_log2,
                             uint64_t& start_scaled_out,
                             uint64_t& length_scaled_out) const;
  bool IsScaledResolveRangeResident(
      uint32_t start_unscaled, uint32_t length_unscaled,
      uint32_t length_scaled_alignment_log2) const;
  bool EnsureScaledResolveBufferRange(uint64_t start_scaled,
                                      uint64_t length_scaled);
  void ClearScaledResolveBuffers();

  // Null texture factory methods (following existing CreateTexture pattern)
  MTL::Texture* CreateNullTexture2D();
  MTL::Texture* CreateNullTexture3D();
  MTL::Texture* CreateNullTextureCube();

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  MetalCommandProcessor* command_processor_;
  uint64_t binding_state_generation_ = 1;

  // Pre-created null textures for invalid bindings (following existing
  // patterns)
  MTL::Texture* null_texture_2d_ = nullptr;
  MTL::Texture* null_texture_3d_ = nullptr;
  MTL::Texture* null_texture_cube_ = nullptr;
  MTL::Texture* null_texture_cube_array_ = nullptr;

  Norm16Selection r16_selection_;
  Norm16Selection rg16_selection_;
  Norm16Selection rgba16_selection_;

  std::unordered_map<uint32_t, MTL::SamplerState*> sampler_cache_;

  class UploadBufferPool;
  mutable std::mutex upload_buffer_pool_mutex_;
  std::shared_ptr<UploadBufferPool> upload_buffer_pool_;
  MTL::CommandBuffer* upload_batch_command_buffer_ = nullptr;
  bool upload_batch_command_buffer_has_work_ = false;
  uint32_t upload_batch_depth_ = 0;
  // Intrusive inventory of the cache's existing MetalTexture objects. This is
  // intentionally not a second key/range registry: preparation scans the
  // authoritative objects and rejects ambiguous aliases.
  MetalTexture* resolve_refresh_first_ = nullptr;
  MetalTexture* resolve_refresh_last_ = nullptr;
  // Whether a texture with this key is created writable and inventoried as a
  // resolve refresh destination: an enabled format in the exact tiled 2D
  // single-level shape a 1x resolve can write.
  bool IsResolveRefreshKey(const TextureKey& key) const;
  std::unique_ptr<MetalHeapPool> texture_heap_pool_;
  bool supports_bc_texture_compression_ = false;

  std::vector<ScaledResolveBuffer> scaled_resolve_buffers_;
  std::vector<RetiredScaledResolveBuffer> scaled_resolve_retired_buffers_;
  uint64_t scaled_resolve_retired_bytes_ = 0;
  size_t scaled_resolve_current_buffer_index_ = size_t(-1);
  uint64_t scaled_resolve_current_range_start_scaled_ = 0;
  uint64_t scaled_resolve_current_range_length_scaled_ = 0;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_TEXTURE_CACHE_H_
