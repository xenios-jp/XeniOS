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

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/resolve_refresh_watch_log.h"
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
  void FinishPendingUploads() {
    CommitLoadsAhead();
    FlushDeferredLoads();
    AbortUploadCommandBufferBatch();
  }
  // Commits the loads recorded to run ahead of the current command buffer in
  // a command buffer of their own. The command processor calls it right
  // before committing the current command buffer.
  void CommitLoadsAhead();
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
      // Force the border color alpha to 1.0 (only meaningful with a border
      // clamp mode).
      uint32_t force_bc_w_to_max : 1;
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
  // Changes when what the descriptor gather of a draw's textures and samplers
  // reads, other than the fetch constant words, may have changed.
  uint64_t binding_state_generation() const {
    return binding_state_generation_;
  }
  // The six words of each of the 32 texture fetch constants, as the registers
  // hold them.
  const uint32_t* texture_fetch_constant_words() const {
    return &register_file().values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
  }

  // Narrow producer interface for an existing, exact texture-cache image
  // whose loader copies raw texels or converts packed 24-bit depth. The backend
  // may write the same logical texels there while it performs the required
  // guest-memory resolve. Partial rectangles require current retained texels,
  // or proof that all writes to an outdated texture stayed within complete
  // rows being rewritten, or, of an outdated texture, that the rectangles
  // refreshed with nothing else written between them add up to all of it.
  // Publication rechecks intervening invalidations after
  // the resolve marks the range GPU-written. It requires valid shared-memory
  // pages and installs the ordinary watch immediately under the same lock,
  // so later writes take the normal invalidation path.
  struct ResolveTextureRefreshTarget {
    enum class Transform : uint8_t { kNone, kDepthUnorm24, kDepthFloat24 };
    struct Alias {
      NS::SharedPtr<MTL::Texture> write_texture;
      void* cache_texture = nullptr;
      // Where the resolve rectangle starts in the texture, in whole 32x32
      // tiles, as texels.
      uint32_t offset_x = 0;
      uint32_t offset_y = 0;
      bool partial = false;
      // The texture is outdated beyond what the rectangle rewrites: it becomes
      // current only once such rectangles cover it (MetalTexture's
      // resolve_refresh_cover_).
      bool accumulate = false;
      uint64_t invalidation_count = 0;
      bool outdated_within_band = false;
      uint64_t fire_sequence = 0;
    };
    static constexpr uint32_t kMaxAliases = 3;
    uint32_t length = 0;
    uint32_t address = 0;
    // The resolve rectangle.
    uint32_t width = 0;
    uint32_t height = 0;
    Transform transform = Transform::kNone;
    // The first destination supplies the shared shape and conversion. Keep
    // every view alive through encoding and publication even if RequestRange
    // evicts its cache wrapper.
    std::array<Alias, kMaxAliases + 1> destinations{};
    uint32_t count = 0;
    // The last raw_count destinations of a depth refresh are other
    // interpretations whose loaders copy the resolved dwords unchanged. They
    // take the packed depth and stencil texels as raw dwords through R32Uint
    // views rather than the converted depth.
    uint32_t raw_count = 0;
    // Textures whose image is a destination's: the resolve writes them through
    // that destination, and publishes them with it.
    std::array<Alias, kMaxAliases + 1> shared{};
    uint32_t shared_count = 0;

    std::span<const Alias> textures() const {
      return {destinations.data(), count};
    }
    std::span<const Alias> shared_textures() const {
      return {shared.data(), shared_count};
    }

    // The destinations, then the textures sharing their images.
    template <typename Function>
    void ForEachTexture(Function function) const {
      for (const Alias& texture : textures()) {
        function(texture);
      }
      for (const Alias& texture : shared_textures()) {
        function(texture);
      }
    }

    bool HasPartial() const {
      bool partial = false;
      ForEachTexture([&](const Alias& texture) { partial |= texture.partial; });
      return partial;
    }

    explicit operator bool() const { return count != 0; }
  };
  // Other interpretations may join only with matching write format, size,
  // conversion and offset; unmatched textures retain the normal reload. A
  // depth refresh also takes raw 32bpb interpretations of matching size and
  // offset, after the depth ones.
  void PrepareDirectResolveTextureRefresh(
      xenos::ColorFormat dest_format, uint32_t address, uint32_t length,
      uint32_t base_address, uint32_t width, uint32_t height, uint32_t pitch,
      xenos::Endian endianness, bool depth,
      ResolveTextureRefreshTarget& target_out);
  void PublishDirectResolveTextureRefresh(
      const ResolveTextureRefreshTarget& target);

  // Another image of the shape, format, swizzle and usage of a refresh
  // target's texture, with its views. A tile resolve whose pass samples the
  // texture writes this in its place, and the texture takes it as its image
  // when the pass ends, instead of having it copied in - for resolves that
  // write every texel.
  struct ResolveRefreshStorage {
    // Retained.
    MTL::Texture* texture = nullptr;
    MTL::Texture* write_view = nullptr;
    std::unordered_map<uint64_t, MTL::Texture*> views;
    ~ResolveRefreshStorage();
  };
  // For the texture whose refresh write view is write_view, or nullptr.
  std::unique_ptr<ResolveRefreshStorage> AcquireResolveRefreshStorage(
      MTL::Texture* write_view);
  // Exchanges the image of the texture whose refresh write view is
  // write_view, if it still exists, with storage, which then holds the
  // texture's former image. False if the texture is gone.
  bool SwapResolveRefreshStorage(
      MTL::Texture* write_view,
      std::unique_ptr<ResolveRefreshStorage>& storage);
  // Back for reuse. The image may still be in use by commands encoded so far.
  void ReleaseResolveRefreshStorage(
      std::unique_ptr<ResolveRefreshStorage> storage);

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

  // The GPU commands of texture loads, recorded so the loads of a whole
  // texture request (a draw's textures) share one compute encoder and one blit
  // encoder rather than taking two each.
  struct LoadDispatch {
    MTL::ComputePipelineState* pipeline;
    MTL::Buffer* constants;
    size_t constants_offset;
    // Null when the pipeline writes dest_texture instead.
    MTL::Buffer* dest;
    size_t dest_offset;
    MTL::Texture* dest_texture;
    MTL::Buffer* source;
    size_t source_offset;
    MTL::Size threadgroups;
    MTL::Size threads_per_group;
  };
  struct LoadCopy {
    MTL::Buffer* source;
    size_t source_offset;
    // To dest_texture, or, if it's null, source_bytes_per_row bytes to
    // dest_buffer.
    size_t source_bytes_per_row;
    size_t source_bytes_per_image;
    MTL::Size size;
    MTL::Texture* dest_texture;
    uint32_t dest_slice;
    uint32_t dest_level;
    MTL::Buffer* dest_buffer;
    size_t dest_offset;
  };
  struct LoadCommands {
    std::vector<LoadDispatch> dispatches;
    // Shared memory ranges (start, length) the dispatches read.
    std::vector<std::pair<uint32_t, uint32_t>> shared_memory_reads;
    std::vector<LoadCopy> copies;
    // Upload pool buffers to release once the command buffer completes.
    std::vector<MTL::Buffer*> buffers;
    // Retained until encoded.
    std::vector<MTL::Texture*> textures;
    // Texture::LoadedGuestData::load_done of the parts loaded, set when the
    // command buffer the commands are encoded into completes.
    std::vector<std::shared_ptr<std::atomic<bool>>> done_flags;
    bool empty() const { return dispatches.empty() && copies.empty(); }
  };
  bool TracksLoadCompletion() const override { return true; }
  // Encodes the dispatches in one compute encoder, then the copies in one blit
  // encoder, releases the buffers after cmd and the textures, and clears
  // commands.
  bool EncodeLoadCommands(MTL::CommandBuffer* cmd, LoadCommands& commands);
  struct DeferredLoads {
    LoadCommands commands;
    // The command buffer the loads were recorded for, retained.
    MTL::CommandBuffer* command_buffer = nullptr;
  };
  // Encodes the loads deferred during the current texture request.
  void FlushDeferredLoads(DeferredLoads& deferred);
  void FlushDeferredLoads() {
    FlushDeferredLoads(deferred_batch_loads_);
    FlushDeferredLoads(deferred_loads_);
  }
  // Encodes the commands into a command buffer of their own and commits it.
  void EncodeLoadCommandsAlone(LoadCommands& commands);
  // Loads into the current command buffer.
  DeferredLoads deferred_loads_;
  // Loads that need nothing from the current command buffer, for
  // CommitLoadsAhead. Recording them there would end the render pass.
  LoadCommands loads_ahead_;
  // Incremented when loads_ahead_ is committed.
  uint64_t loads_ahead_generation_ = 1;
  // Loads into the upload batch command buffer, committed ahead of the current
  // one.
  DeferredLoads deferred_batch_loads_;

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
    // Of metal_texture_, which never changes: asked for on every view lookup.
    MTL::PixelFormat metal_pixel_format_ = MTL::PixelFormatInvalid;
    MTL::TextureType metal_texture_type_ = MTL::TextureType2D;
    std::unique_ptr<MetalTexture> texture_3d_as_2d_;
    MTL::Texture* resolve_write_view_ = nullptr;
    std::unordered_map<uint64_t, MTL::Texture*> swizzled_view_cache_;
    MetalTexture* resolve_refresh_previous_ = nullptr;
    MetalTexture* resolve_refresh_next_ = nullptr;
    bool resolve_refresh_registered_ = false;
    // While the texture is outdated: the rectangle of it that direct resolves
    // have rewritten, in guest memory and in the image alike, with nothing
    // else written to its memory since the first of them. The watch tells of
    // the first write after the last of them, which is the next such resolve
    // itself if the rectangle is to grow. All under the global lock.
    struct ResolveRefreshCover {
      uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      SharedMemory::WatchHandle watch = nullptr;
      // The shared memory fire sequence of the write the watch saw.
      uint64_t fired_sequence = 0;
      bool active = false;
    };
    ResolveRefreshCover resolve_refresh_cover_;
    static void ResolveRefreshCoverWatchCallback(
        const global_unique_lock_type& global_lock, void* context, void* data,
        uint64_t argument, bool invalidated_by_gpu);
    void DropResolveRefreshCover(const global_unique_lock_type& global_lock);
    // The last submission whose command buffer accesses the texture.
    uint64_t used_submission_ = 0;
    // The loads_ahead_generation_ in which a load ahead was recorded.
    uint64_t loads_ahead_generation_ = 0;
  };

 private:
  // Metal texture creation helpers
  MTL::Texture* CreateTexture2D(uint32_t width, uint32_t height,
                                uint32_t array_length, MTL::PixelFormat format,
                                MTL::TextureSwizzleChannels swizzle,
                                uint32_t mip_levels = 1,
                                bool shader_write = false,
                                bool pixel_format_view = true);
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
  // The binding state generation changes only here: when a binding's
  // textures, swizzle or signs change or the bindings are reset, and when a
  // view, sampler or 3D-as-2D wrapper the gather may return is created or
  // dropped.
  void OnGatherInputsChanged() {
    if (++binding_state_generation_ == 0) {
      binding_state_generation_ = 1;
    }
  }
  void UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) override;
  uint64_t binding_state_generation_ = 1;
  // GetSamplerParameters runs for every sampler of every draw, and its result
  // is a function of the fetch constant's words and the binding's filters.
  struct SamplerParametersMemo {
    xenos::xe_gpu_texture_fetch_t fetch;
    uint32_t filters;
    SamplerParameters parameters;
    bool valid;
  };
  mutable std::array<SamplerParametersMemo, xenos::kTextureFetchConstantCount>
      sampler_parameters_memo_ = {};

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
  // Free ResolveRefreshStorage, and those released beyond the pool's size,
  // freed once the submission that may use them has completed.
  std::vector<std::unique_ptr<ResolveRefreshStorage>>
      resolve_refresh_storage_pool_;
  std::vector<std::pair<uint64_t, std::unique_ptr<ResolveRefreshStorage>>>
      resolve_refresh_storage_retired_;
  void FreeResolveRefreshStorage(bool all);
  // Whether a texture with this key is created writable and inventoried as a
  // resolve refresh destination: an enabled format in the exact tiled 2D
  // single-level shape a 1x resolve can write.
  bool IsResolveRefreshKey(const TextureKey& key) const;
  // A resolve refresh texture whose image a new texture of this key can take
  // as its own through a view: the same memory and shape, and a loader that
  // copies the same raw texels.
  MetalTexture* FindResolveAliasImageOwner(const TextureKey& key,
                                           MTL::PixelFormat pixel_format);
  // Points the textures other than except that view old_root at a view of
  // new_root instead, as their image moved.
  void MoveResolveAliasViews(MTL::Texture* old_root, MTL::Texture* new_root,
                             const MetalTexture* except);
  ResolveRefreshWatchLog refresh_fire_log_;
  SharedMemory::GlobalWatchHandle refresh_fire_watch_handle_ = nullptr;
  static void RefreshFireGlobalWatchCallbackThunk(
      const global_unique_lock_type& global_lock, void* context,
      uint32_t address_first, uint32_t address_last, bool invalidated_by_gpu);
  bool RefreshFiresWithin(const global_unique_lock_type& global_lock,
                          uint64_t first_sequence, uint32_t range_first,
                          uint32_t range_last, uint32_t cpu_first,
                          uint32_t cpu_last, uint32_t gpu_first,
                          uint32_t gpu_last) const;

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
