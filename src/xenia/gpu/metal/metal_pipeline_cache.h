/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_
#define XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/base/platform.h"
#include "xenia/gpu/guest_spirv_shader_cache.h"
#include "xenia/gpu/memexport_format_profile.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_pipeline_archive.h"
#include "xenia/gpu/metal/metal_shader.h"
#include "xenia/gpu/metal/metal_shader_library_cache.h"
#include "xenia/gpu/pipeline_creation_queue.h"
#include "xenia/gpu/primitive_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader_storage.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

// Owns Metal guest shader translation, SPIR-V -> AIR compilation, fetch-state
// baking/specialization, finite-multiply & draw-exactness analysis, pipeline
// state caching, on-disk shader/pipeline storage & archive persistence, and
// background shader/pipeline compilation threads.
class MetalPipelineCache : private GuestSpirvShaderCache::Host {
 public:
  enum class ShaderCompileStatus {
    kReady,
    kPending,
    kFailed,
    kNotQueued,
  };

  enum class PipelineCompileStatus {
    kReady,
    kPending,
    kFailed,
  };

  // What a queued pipeline request builds. Each kind reads a different subset
  // of the request, but all of them are captured on the draw thread.
  // Explicit values preserve on-disk .metal.xpso compatibility (1 was the
  // removed MSL tessellation kind, 2 was the removed MSC mesh tessellation
  // kind).
  enum class PipelineKind : uint32_t {
    kRender = 0,
    kPatchTessellation = 3,
  };

  // Everything that distinguishes one pipeline, in a form that survives to
  // disk. Its hash is the cache key, so replay lands where a draw looks.
  // Update kVersion if anything here changes!
  XEPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    // 0 with no pixel shader - the depth-only and placeholder pipelines.
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;

    uint32_t color_formats[4];  // MTL::PixelFormat
    uint32_t depth_format;
    uint32_t stencil_format;
    uint32_t blendcontrol[4];  // RB_BLENDCONTROL, zeroed for unwritten targets
    uint32_t normalized_color_mask;
    uint32_t sample_count;
    // Potentially non-finite sampled textures and fixed float constants.
    uint32_t vertex_finite_multiply_state[2];
    uint32_t pixel_finite_multiply_state[2];
    // Only constants the shader reads contribute to its pipeline identity.
    // Texture arithmetic other than the fetch constant decoding and system
    // flags stay dynamic.
    uint32_t vertex_fetch_endians
        [SpirvShaderTranslator::kVertexFetchEndianBindingCount * 4];
    uint32_t vertex_bool_constants[256 / 32];
    uint32_t pixel_bool_constants[256 / 32];
    uint32_t vertex_loop_constants[32];
    uint32_t pixel_loop_constants[32];
    // TextureCache::GetIntegerScaleBits of the fetch constants the shader
    // samples, selecting the fetch conversions it needs.
    uint32_t vertex_texture_scale_bits[32];
    uint32_t pixel_texture_scale_bits[32];
    // TextureCache::GetFetchDecodeBits of the fetch constants the shader
    // samples: the signednesses and the exponent bias.
    uint32_t vertex_texture_decode_bits[32];
    uint32_t pixel_texture_decode_bits[32];

    PipelineKind kind : 2;                          // 2
    xenos::TessellationMode tessellation_mode : 2;  // 4
    Shader::HostVertexShaderType host_vertex_shader_type
        : Shader::kHostVertexShaderTypeBitCount;  // 8
    uint32_t vertex_fetch_states_specialized : 1;
    uint32_t fragment_fetch_states_specialized : 1;
    uint32_t finite_multiply_enabled : 1;
    // 8_8_8_8_GAMMA, 16_16 and 16_16_16_16 color targets the pixel shader
    // blends itself, with fixed-function blending off.
    uint32_t shader_blend_mask : 4;
    // Set when a stage has finite multiply requirements and they come from
    // the one-sided analysis. Taken from the padding, so a description stored
    // by a build without it reads 0: its finite multiply state then belongs to
    // another analysis, and it's rebuilt as the general variant.
    uint32_t finite_multiply_one_sided : 1;
    uint32_t padding : 16;  // 32

    // Increment when the layout or its meaning changes.
    static constexpr uint32_t kVersion = 0x20261110;

    // Copied whole, padding included, because the hash covers every byte.
    PipelineDescription() { Reset(); }
    PipelineDescription(const PipelineDescription& other) {
      std::memcpy(this, &other, sizeof(*this));
    }
    PipelineDescription& operator=(const PipelineDescription& other) {
      std::memcpy(this, &other, sizeof(*this));
      return *this;
    }
    void Reset() { std::memset(this, 0, sizeof(*this)); }
    uint64_t GetHash() const { return XXH3_64bits(this, sizeof(*this)); }
    bool operator==(const PipelineDescription& other) const {
      return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
  });

  XEPACKEDSTRUCT(PipelineStoredDescription, {
    uint64_t description_hash;
    PipelineDescription description;
  });

  struct PipelineCompileRequest {
    PipelineDescription description;
    // description.GetHash(), the key into async_pipeline_cache_.
    uint64_t pipeline_key = 0;
    MTL::Function* vertex_function = nullptr;
    MTL::Function* fragment_function = nullptr;
    uint8_t priority = 0;
    MTL::Library* vertex_library = nullptr;
    const MetalShader::MetalTranslation* vertex_translation = nullptr;
    std::string vertex_function_name;
    bool vertex_fetch_states_specialized = false;
    bool fragment_fetch_states_specialized = false;
    MTL::Library* fragment_library = nullptr;
    const MetalShader::MetalTranslation* fragment_translation = nullptr;
    std::string fragment_function_name;
  };

  explicit MetalPipelineCache(MetalCommandProcessor& command_processor);
  ~MetalPipelineCache();

  // Lifecycle & storage.
  bool InitializeShaderTranslation();
  void InitializeAsyncCompilation();
  void ShutdownAsyncCompilation();
  void Shutdown();
  void InitializeShaderStorage(
      const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
      std::function<void()> on_storage_initialized = {});
  void FlushStorageIfNeeded();
  void MaybeSavePipelineArchive();
  void JoinPipelineArchiveSave();
  void ClearAddressShaderLoads();
  void LogCompileStages();

  // Compile-thread boost & waiting.
  void SetCompileThreadBoost(bool boost);
  void SetCompileThreadBoostAllowed(bool allowed);
  void AwaitAsyncCompiles();
  void AwaitPendingPipeline();

  // Guest shader loading.
  Shader* LoadShader(xenos::ShaderType shader_type,
                     const uint32_t* host_address, uint32_t dword_count);
  Shader* LoadShaderAtAddress(xenos::ShaderType shader_type,
                              uint32_t guest_address, uint32_t dword_count);
  SpirvShader* LoadShader(xenos::ShaderType shader_type,
                          const uint32_t* host_address, uint32_t dword_count,
                          uint64_t hash);

  // Per-draw modification, translation, exactness, and pipeline lookup.
  SpirvShaderTranslator::Modification GetCurrentSpirvVertexShaderModification(
      const Shader& shader,
      Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, bool ps_param_gen_used) const;
  SpirvShaderTranslator::Modification GetCurrentSpirvPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control,
      uint32_t normalized_color_mask) const;
  void UpdateDrawExactness(
      const Shader& vertex_shader,
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result);
  Shader::Translation* GetOrCreateHostTranslation(
      SpirvShader& shader, uint64_t modification, bool allow_async,
      ShaderCompileStatus* compile_status_out,
      std::shared_ptr<const Shader::Specialization> specialization = {});
  MTL::RenderPipelineState* GetOrCreatePipelineState(
      const Shader::Translation* vertex_translation,
      const Shader::Translation* pixel_translation, const RegisterFile& regs,
      PipelineCompileStatus* compile_status_out = nullptr);
  MTL::RenderPipelineState* GetOrCreatePlaceholderPipelineState(
      const Shader::Translation* vertex_translation, const RegisterFile& regs);

  // Rate-limited draw-skip / placeholder logging.
  void LogShaderCompilePending(const Shader::Translation* translation,
                               const char* stage_tag);
  void LogPipelineCompilePending(const Shader::Translation* vertex_translation,
                                 const Shader::Translation* pixel_translation);
  void LogPlaceholderDraw(const Shader::Translation* vertex_translation,
                          const Shader::Translation* pixel_translation);

  // Capabilities & accessors shared with MetalCommandProcessor.
  bool SupportsHostRenderTargetFeedback() const;
  const SpirvShaderTranslator::Features& spirv_translator_features() const {
    return spirv_translator_features_;
  }
  const float* draw_exact_estimates() const { return draw_exact_estimates_; }
  static constexpr size_t kDrawExactEstimatesSize =
      sizeof(float) * SpirvShaderTranslator::DrawExactAnalysis::kMaxEstimates;
  memexport_format_profile::ProfilePool& memexport_format_profiles() {
    return memexport_format_profiles_;
  }

  // Shared static helpers used by both MetalCommandProcessor and
  // MetalPipelineCache.
  static constexpr uint32_t kBlendControlMask = 0x1FFF1FFF;
  static constexpr uint32_t kBlendControlNoBlending = 0x00010001;
  static bool NeedsExactFloatDepthBias(const RegisterFile& regs);
  static bool IsFixed16Format(xenos::ColorRenderTargetFormat format);
  static bool IsFixed16BlendExact(xenos::BlendFactor src,
                                  xenos::BlendFactor dest, xenos::BlendOp op);
  static uint32_t GetShaderBlendControl(const RegisterFile& regs, uint32_t rt,
                                        uint32_t normalized_color_mask,
                                        bool gamma_fetch_blend,
                                        bool fixed16_fetch_blend);
  static uint32_t GetColorTargetsWritten(uint32_t normalized_color_mask);
  static void PopulatePipelineFormatsFromRenderPassDescriptor(
      MTL::RenderPassDescriptor* pass_descriptor,
      MTL::PixelFormat* color_formats, uint32_t color_count,
      MTL::PixelFormat* depth_format, MTL::PixelFormat* stencil_format,
      uint32_t* sample_count);

 private:
  static constexpr uint32_t kPipelineStorageAPIMagicMetal = 'MTLP';
  // Pipeline compile priorities, highest first.
  static constexpr uint8_t kPipelinePriorityStoredDepthOnly = 1;
  static constexpr uint8_t kPipelinePriorityStored = 2;
  static constexpr uint8_t kPipelinePriorityDrawDepthOnly = 3;
  static constexpr uint8_t kPipelinePriorityDraw = 4;
  static constexpr uint8_t kPipelinePriorityAwaited = 5;

  // Priority lanes in PipelineCreationQueue (highest dequeued first).
  // Matches draw pipelines > draw shaders > stored pipelines.
  enum CompileQueuePriority : size_t {
    kQueuePriorityStoredDepthOnly = 0,
    kQueuePriorityStored = 1,
    kQueuePriorityShaderPixel = 2,
    kQueuePriorityShaderVertex = 3,
    kQueuePriorityDrawDepthOnly = 4,
    kQueuePriorityDraw = 5,
    kQueuePriorityAwaited = 6,
    kQueuePriorityCount = 7,
  };
  static size_t PipelinePriorityToQueueLane(uint8_t priority) {
    switch (priority) {
      case kPipelinePriorityStoredDepthOnly:
        return kQueuePriorityStoredDepthOnly;
      case kPipelinePriorityStored:
        return kQueuePriorityStored;
      case kPipelinePriorityDrawDepthOnly:
        return kQueuePriorityDrawDepthOnly;
      case kPipelinePriorityDraw:
        return kQueuePriorityDraw;
      case kPipelinePriorityAwaited:
      default:
        return kQueuePriorityAwaited;
    }
  }

  struct ShaderCompileRequest {
    Shader::Translation* translation = nullptr;
    uint64_t shader_hash = 0;
    uint64_t modification = 0;
    uint8_t priority = 0;
    // Queued by the shader storage load, so it counts toward its progress.
    bool storage = false;
  };

  struct QueuedPipelineRequest {
    PipelineCompileRequest request;
    // Retain vertex/fragment functions for the lifetime of the queued request.
    NS::SharedPtr<MTL::Function> owned_vertex_function;
    NS::SharedPtr<MTL::Function> owned_fragment_function;
  };

  using CompileTask = std::variant<ShaderCompileRequest, QueuedPipelineRequest>;

  struct CompileResult {
    MTL::RenderPipelineState* pipeline = nullptr;
    ShaderCompileStatus shader_status = ShaderCompileStatus::kFailed;
    std::string error;
  };

  struct WorkerThreadContext {
    std::unique_ptr<SpirvShaderTranslator> translator;
  };

  struct RecentShaderLoad {
    SpirvShader* shader = nullptr;
    std::vector<uint32_t> ucode;
  };

  struct AddressShaderLoad {
    SpirvShader* shader = nullptr;
    uint32_t dword_count = 0;
    xenos::ShaderType shader_type = xenos::ShaderType::kVertex;
    uint32_t same_reloads = 0;
    bool unwatchable = false;
    bool watch_installed = false;
    SharedMemory::WatchHandle watch = nullptr;
    std::atomic<bool> valid{false};
  };

  struct FiniteMultiplyInfo {
    uint64_t required = 0;
    uint64_t direct_constants[4] = {};
  };

  struct DrawExactStableFlags {
    uint32_t flags = UINT32_MAX;
    uint32_t settled_draws = 0;
  };

  enum CompileStage : size_t {
    kCompileStageTranslate,      // Guest ucode to SPIR-V.
    kCompileStageLibraryCached,  // A metallib from shader_libraries.bin.
    kCompileStageAir,            // SPIR-V to AIR, and its MTLLibrary.
    kCompileStageSpecialize,     // newFunction with constant values.
    kCompileStageBake,           // spirv2air with the constants compiled in.
    kCompileStageBakedCached,    // A baked library from the cache.
    kCompileStagePipelineHit,    // A render pipeline found in the archive.
    kCompileStagePipelineMiss,   // A render pipeline Metal compiled.
    kCompileStageDrawWait,       // The draw thread waiting for a compile.
    kCompileStageCount,
  };
  struct CompileStageTotal {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> ns{0};
    std::atomic<uint64_t> max_ns{0};
  };

  using SpecializedFunctionKey =
      std::tuple<uintptr_t, std::string, std::vector<uint32_t>>;

  static void AddressShaderLoadWatchCallback(
      const global_unique_lock_type& global_lock, void* context, void* data,
      uint64_t argument, bool invalidated_by_gpu);

  ShaderCompileStatus GetShaderCompileStatus(Shader::Translation* translation);
  bool EnqueueShaderCompilation(Shader::Translation* translation,
                                uint8_t priority);
  void NoteShaderCompileFailed(Shader::Translation* translation);
  ShaderCompileStatus EnsureTranslationSpirv(Shader::Translation* translation,
                                             SpirvShaderTranslator& translator);
  ShaderCompileStatus TranslateClaimedSpirv(Shader::Translation* translation,
                                            SpirvShaderTranslator& translator);
  bool CompileHostShader(Shader::Translation* translation,
                         std::vector<uint8_t>* metallib_out = nullptr);
  ShaderCompileStatus TranslateAndCompileHostShader(
      Shader::Translation* translation, SpirvShaderTranslator& translator);
  // GuestSpirvShaderCache::Host overrides.
  std::unique_ptr<SpirvShaderTranslator> CreateTranslator() const override;
  bool depth_float24_round() const override;
  bool depth_float24_convert_in_pixel_shader() const override;
  bool supports_draw_scale_native() const override { return false; }
  bool allow_early_depth_stencil_hint() const override { return true; }

  static MTL::Function* GetHostShaderFunction(
      const Shader::Translation* translation);
  static bool IsHostShaderReady(const Shader::Translation* translation);
  bool EnqueuePipelineCompilation(const PipelineCompileRequest& request);

  bool IsAsyncCompileIdleLocked() const;
  void FinishAsyncCompileTaskLocked();
  void CountAsyncCompileTaskFinishedLocked();
  template <typename Done>
  void AwaitAsyncCompilesUntil(const Done& done);
  void UpdateCompileThreadLimitLocked();
  void PromoteQueuedPipelineLocked(uint64_t key, uint8_t priority);
  bool TakeQueuedPipelineLocked(uint64_t key);
  void RecordStorageRebuildStatsLocked();
  void NoteStoredPipelineDoneLocked(uint64_t pipeline_key);
  void PublishStorageRequestsLocked();
  void NoteStorageRequestDoneLocked();
  void FinishStorageRebuildLocked();

  static void ApplyColorAttachmentState(
      MTL::RenderPipelineColorAttachmentDescriptorArray* attachments,
      const PipelineCompileRequest& request);
  MTL::RenderPipelineState* CreatePipelineState(
      const PipelineCompileRequest& request, std::string* error_out);
  bool UsesVertexFetchStateFunctionConstants(
      const Shader::Translation* translation) const;
  bool UsesPixelFetchStateFunctionConstants(
      const Shader::Translation* translation) const;
  bool BakesFetchState(const Shader::Translation* translation) const;
  const FiniteMultiplyInfo& GetFiniteMultiplyInfo(const Shader& shader) const;
  uint64_t GetFiniteMultiplyRequirements(const Shader& shader) const;
  bool SetGeneralFiniteMultiplyState(const Shader::Translation* vertex,
                                     const Shader::Translation* pixel,
                                     PipelineDescription& description) const;
  bool HasFiniteMultiplyRequirements(
      const Shader::Translation* vertex, const Shader::Translation* pixel,
      const PipelineDescription& description) const;
  MTL::RenderPipelineState* FindBuiltPipelineState(uint64_t key);
  void GetFiniteMultiplyState(const Shader::Translation* translation,
                              const RegisterFile& regs, uint32_t* state) const;
  const SpirvShaderTranslator::DrawExactAnalysis& GetDrawExactAnalysis(
      const Shader& shader) const;
  void SetPipelineFunctionConstants(PipelineCompileRequest& request,
                                    const Shader::Translation* vertex,
                                    const Shader::Translation* pixel);
  MTL::Function* GetSpecializedFunction(
      const MetalShader::MetalTranslation* translation, MTL::Library* library,
      const std::string& function_name, const uint32_t* states,
      uint32_t binding_count, std::string* error_out);
  MTL::Function* GetBakedFunction(
      const MetalShader::MetalTranslation* translation, const uint32_t* states,
      uint32_t binding_count, const SpecializedFunctionKey& key);
  bool EnsureBakeSpirv(const MetalShader::MetalTranslation* translation);
  MTL::Library* GetGenericFallbackLibrary(
      const MetalShader::MetalTranslation* translation,
      std::string& entry_point_name_out);
  MTL::Function* GetPipelineVertexFunction(
      const PipelineCompileRequest& request, std::string* error_out);
  MTL::Function* GetPipelineFragmentFunction(
      const PipelineCompileRequest& request, std::string* error_out);
  CompileResult ExecuteCompileTask(const CompileTask& task,
                                   WorkerThreadContext* context);
  void StoreCompileResult(const CompileTask& task, const CompileResult& result);
  uint64_t PopulatePipelineCompileRequest(
      const RegisterFile& regs, const Shader::Translation* vertex_translation,
      const Shader::Translation* pixel_translation,
      PipelineCompileRequest& request);
  MTL::RenderPipelineState* AcquirePipelineState(
      const PipelineCompileRequest& request, bool allow_async,
      PipelineCompileStatus* compile_status_out);
  void AddCompileStageTime(CompileStage stage, int64_t start_ns);
  void TranslateShadersForStorage(
      const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
      size_t stored_count);
  void CreateStoredPipelines(
      const std::vector<PipelineStoredDescription>& stored_descriptions);
  const char* QueueStoredPipeline(const PipelineStoredDescription& stored,
                                  Shader*& vertex_shader_out);

  MetalCommandProcessor& command_processor_;

  std::unique_ptr<GuestSpirvShaderCache> guest_spirv_shader_cache_;
  SpirvShaderTranslator::Features spirv_translator_features_{false};
  bool spirv_translator_native_2x_msaa_ = false;
  uint32_t spirv_translator_resolution_scale_x_ = 1;
  uint32_t spirv_translator_resolution_scale_y_ = 1;
  std::unordered_map<uint64_t, std::unique_ptr<SpirvShader>>
      guest_shader_cache_;
  std::array<RecentShaderLoad, 8> recent_shader_loads_;
  std::unordered_map<uint32_t, std::unique_ptr<AddressShaderLoad>>
      address_shader_loads_;
  std::array<std::pair<uint32_t, AddressShaderLoad*>, 2>
      last_address_shader_loads_{};

  memexport_format_profile::ProfilePool memexport_format_profiles_;
  mutable std::mutex shader_analysis_mutex_;
  mutable std::unordered_map<const Shader*, FiniteMultiplyInfo>
      finite_multiply_requirements_;
  mutable const Shader* finite_multiply_last_shader_[2] = {};
  mutable const FiniteMultiplyInfo* finite_multiply_last_info_[2] = {};
  mutable std::unordered_map<const Shader*,
                             SpirvShaderTranslator::DrawExactAnalysis>
      draw_exact_analyses_;
  mutable const Shader* draw_exact_last_shader_ = nullptr;
  mutable const SpirvShaderTranslator::DrawExactAnalysis*
      draw_exact_last_analysis_ = nullptr;
  const Shader* draw_exact_shader_ = nullptr;
  uint32_t draw_exact_flags_ = 0;
  uint32_t draw_exact_key_flags_ = 0;
  std::unordered_map<uint64_t, DrawExactStableFlags> draw_exact_stable_flags_;
  std::mutex draw_exact_stable_flags_mutex_;
  float draw_exact_estimates_
      [SpirvShaderTranslator::DrawExactAnalysis::kMaxEstimates] = {};

  std::mutex specialized_functions_mutex_;
  std::map<SpecializedFunctionKey, MTL::Function*> specialized_functions_;
  std::vector<MTL::Library*> baked_libraries_;
  std::mutex bake_translate_mutex_;
  std::mutex generic_fallback_mutex_;
  std::map<const MetalShader::MetalTranslation*,
           std::pair<MTL::Library*, std::string>>
      generic_fallback_libraries_;

  PipelineCreationQueue<CompileTask, CompileResult, WorkerThreadContext,
                        kQueuePriorityCount>
      creation_queue_;
  std::unordered_set<Shader::Translation*> async_shader_pending_;
  std::unordered_set<Shader::Translation*> async_shader_failed_;
  std::unordered_set<uint64_t> async_pipeline_pending_;
  std::unordered_set<uint64_t> async_pipeline_failed_;
  std::mutex async_compile_mutex_;
  std::condition_variable async_compile_idle_cv_;
  size_t async_compile_base_thread_count_ = 0;
  size_t async_compile_thread_limit_ = 0;
  bool async_compile_hold_boost_ = false;
  bool async_compile_boost_allowed_ = true;
  size_t async_compile_awaiters_ = 0;
  uint64_t last_pending_pipeline_key_ = 0;
  bool async_compile_shutdown_ = false;

  std::atomic<int64_t> async_shader_failure_last_log_ns_{0};
  std::atomic<int64_t> async_pipeline_failure_last_log_ns_{0};
  std::atomic<int64_t> async_shader_pending_last_log_ns_{0};
  std::atomic<int64_t> async_pipeline_pending_last_log_ns_{0};
  std::atomic<int64_t> async_placeholder_last_log_ns_{0};
  std::atomic<int64_t> finite_multiply_general_last_log_ns_{0};
  uint64_t finite_multiply_general_draws_ = 0;
  std::atomic<int64_t> async_compile_drain_last_log_ns_{0};

  std::array<CompileStageTotal, kCompileStageCount> compile_stage_totals_;
  std::array<uint64_t, kCompileStageCount> compile_stage_logged_count_{};
  std::array<uint64_t, kCompileStageCount> compile_stage_logged_ns_{};
  std::atomic<int64_t> compile_stage_last_log_ns_{0};

  bool storage_stats_pending_ = false;
  uint64_t storage_stats_start_ticks_ = 0;
  uint64_t storage_stats_stored_ = 0;
  uint32_t storage_stats_version_ = 0;
  bool storage_stats_boosted_ = false;
  std::unordered_set<uint64_t> storage_stats_keys_;
  bool storage_stats_queued_all_ = false;
  uint64_t storage_shader_requests_ = 0;
  uint64_t storage_pipeline_requests_ = 0;
  uint64_t storage_requests_done_ = 0;
  uint64_t storage_stats_expected_pipelines_ = 0;
  std::filesystem::path storage_stats_path_;

  ShaderStorageWriter<PipelineStoredDescription> storage_writer_;
  MetalShaderLibraryCache shader_library_cache_;
  MetalPipelineArchive pipeline_archive_;
  std::thread archive_save_thread_;
  std::atomic<bool> archive_save_running_{false};
  uint64_t archive_saved_ticks_ = 0;
  static constexpr uint64_t kPipelineArchiveSaveSeconds = 20;
  bool shader_storage_flush_needed_ = false;
  bool pipeline_storage_flush_needed_ = false;
  bool replaying_stored_pipelines_ = false;
  bool queueing_storage_requests_ = false;

  std::unordered_map<uint64_t, MTL::RenderPipelineState*> async_pipeline_cache_;
  MTL::Library* exact_float_depth_bias_library_ = nullptr;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_PIPELINE_CACHE_H_
