/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/d3d12/pipeline_cache.h"

#include <cmath>
#include <cstring>
#include <thread>

#include "third_party/dxbc/DXBCChecksum.h"
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/string.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/dxbc.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/pipeline_util.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_util.h"

#include "third_party/fmt/include/fmt/xchar.h"

DEFINE_bool(d3d12_dxbc_disasm, false,
            "Disassemble DXBC shaders after generation.", "D3D12");
DEFINE_bool(
    d3d12_dxbc_disasm_dxilconv, false,
    "Disassemble DXBC shaders after conversion to DXIL, if DXIL shaders are "
    "supported by the OS, and DirectX Shader Compiler DLLs available at "
    "https://github.com/microsoft/DirectXShaderCompiler/releases are present.",
    "D3D12");
DEFINE_int32(
    d3d12_pipeline_creation_threads, -1,
    "Number of threads used for graphics pipeline creation. -1 to calculate "
    "automatically (75% of logical CPU cores), a positive number to specify "
    "the number of threads explicitly (up to the number of logical CPU cores), "
    "0 to disable multithreaded pipeline creation.",
    "D3D12");
DEFINE_bool(d3d12_tessellation_wireframe, false,
            "Display tessellated surfaces as wireframe for debugging.",
            "D3D12");

namespace xe {
namespace gpu {
namespace d3d12 {

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/adaptive_quad_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/adaptive_triangle_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/continuous_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_quad_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_quad_4cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_triangle_1cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/discrete_triangle_3cp_hs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/float24_round_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/float24_truncate_ps.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/tessellation_adaptive_vs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/tessellation_indexed_vs.h"
}  // namespace shaders

PipelineCache::PipelineCache(D3D12CommandProcessor& command_processor,
                             const RegisterFile& register_file,
                             const D3D12RenderTargetCache& render_target_cache,
                             bool bindless_resources_used)
    : command_processor_(command_processor),
      register_file_(register_file),
      render_target_cache_(render_target_cache),
      bindless_resources_used_(bindless_resources_used) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();

  bool edram_rov_used = render_target_cache.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
      !(edram_rov_used ||
        render_target_cache_.gamma_render_target_as_unorm16()),
      render_target_cache_.msaa_2x_supported(),
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y(),
      provider.GetGraphicsAnalysis() != nullptr);

  if (edram_rov_used) {
    depth_only_pixel_shader_ =
        std::move(shader_translator_->CreateDepthOnlyPixelShader());
  }
}

PipelineCache::~PipelineCache() { Shutdown(); }

bool PipelineCache::Initialize() {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();

  // Initialize the command processor thread DXIL objects.
  dxbc_converter_ = nullptr;
  dxc_utils_ = nullptr;
  dxc_compiler_ = nullptr;
  if (cvars::d3d12_dxbc_disasm_dxilconv) {
    if (FAILED(provider.DxbcConverterCreateInstance(
            CLSID_DxbcConverter, IID_PPV_ARGS(&dxbc_converter_)))) {
      XELOGE(
          "Failed to create DxbcConverter, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcUtils,
                                          IID_PPV_ARGS(&dxc_utils_)))) {
      XELOGE(
          "Failed to create DxcUtils, converted DXIL disassembly for debugging "
          "will be unavailable");
    }
    if (FAILED(provider.DxcCreateInstance(CLSID_DxcCompiler,
                                          IID_PPV_ARGS(&dxc_compiler_)))) {
      XELOGE(
          "Failed to create DxcCompiler, converted DXIL disassembly for "
          "debugging will be unavailable");
    }
  }

  uint32_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    // Pick some reasonable amount if couldn't determine the number of cores.
    logical_processor_count = 6;
  }
  // Initialize creation thread synchronization data even if not using creation
  // threads because they may be used anyway to create pipelines from the
  // storage.
  creation_threads_busy_ = 0;
  creation_completion_event_ =
      xe::threading::Event::CreateManualResetEvent(true);
  assert_not_null(creation_completion_event_);
  creation_completion_set_event_ = false;
  creation_threads_shutdown_from_ = SIZE_MAX;
  if (cvars::d3d12_pipeline_creation_threads != 0) {
    size_t creation_thread_count;
    if (cvars::d3d12_pipeline_creation_threads < 0) {
      creation_thread_count =
          std::max(logical_processor_count * 3 / 4, uint32_t(1));
    } else {
      creation_thread_count =
          std::min(uint32_t(cvars::d3d12_pipeline_creation_threads),
                   logical_processor_count);
    }
    for (size_t i = 0; i < creation_thread_count; ++i) {
      std::unique_ptr<xe::threading::Thread> creation_thread =
          xe::threading::Thread::Create({}, [this, i]() { CreationThread(i); });
      assert_not_null(creation_thread);
      creation_thread->set_name("D3D12 Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }
  }
  return true;
}

void PipelineCache::Shutdown() {
  // Shut down all threads, before destroying the pipelines since they may be
  // creating them.
  if (!creation_threads_.empty()) {
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      creation_threads_shutdown_from_ = 0;
    }
    creation_request_cond_.notify_all();
    for (size_t i = 0; i < creation_threads_.size(); ++i) {
      xe::threading::Wait(creation_threads_[i].get(), false);
    }
    creation_threads_.clear();
  }
  creation_completion_event_.reset();

  // Shut down the persistent shader / pipeline storage.
  ShutdownShaderStorage();

  // Destroy all pipelines.
  current_pipeline_ = nullptr;
  for (auto it : pipelines_) {
    ID3D12PipelineState* state =
        it.second->state.load(std::memory_order_acquire);
    if (state) {
      state->Release();
    }
    delete it.second;
  }
  pipelines_.clear();
  COUNT_profile_set("gpu/pipeline_cache/pipelines", 0);

  // Destroy all shaders.
  if (bindless_resources_used_) {
    bindless_sampler_layout_map_.clear();
    bindless_sampler_layouts_.clear();
  }
  texture_binding_layout_map_.clear();
  texture_binding_layouts_.clear();
  for (auto it : shaders_) {
    delete it.second;
  }
  shaders_.clear();

  // Shut down shader translation.
  ui::d3d12::util::ReleaseAndNull(dxc_compiler_);
  ui::d3d12::util::ReleaseAndNull(dxc_utils_);
  ui::d3d12::util::ReleaseAndNull(dxbc_converter_);
}

void PipelineCache::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  ShutdownShaderStorage();

  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  ShaderStorageWriter<PipelineStoredDescription>::PipelineStorageConfig
      pipeline_config;
  pipeline_config.file_suffix =
      fmt::format(".{}.d3d12.xpso", edram_rov_used ? "rov" : "rtv");
  pipeline_config.api_magic = edram_rov_used ? 0x4F525844 : 0x54525844;
  pipeline_config.version =
      std::max(PipelineDescription::kVersion,
               DxbcShaderTranslator::Modification::kVersion);

  uint32_t storage_index = storage_writer_.storage_index() + 1;

  std::vector<PipelineStoredDescription> pipeline_stored_descriptions;
  if (!storage_writer_.InitializeShaderStorage(
          cache_root, title_id, pipeline_config,
          // Shader load callback.
          [&](xenos::ShaderType type, const uint32_t* ucode_dwords,
              uint32_t ucode_dword_count, uint64_t ucode_data_hash) {
            D3D12Shader* shader = LoadShader(
                type, ucode_dwords, ucode_dword_count, ucode_data_hash);
            if (shader->ucode_storage_index() == storage_index) {
              return true;  // Already loaded.
            }
            shader->set_ucode_storage_index(storage_index);
            return true;
          },
          // Shader translate callback.
          [this, edram_rov_used](const std::set<std::pair<uint64_t, uint64_t>>
                                     & translations_needed) {
            TranslateShadersForStorage(translations_needed, edram_rov_used);
          },
          pipeline_stored_descriptions)) {
    return;
  }
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;

  // Create the pipelines.
  if (!pipeline_stored_descriptions.empty()) {
    uint64_t pipeline_creation_start_ = xe::Clock::QueryHostTickCount();

    // Launch additional creation threads to use all cores to create
    // pipelines faster. Will also be using the main thread, so minus 1.
    size_t logical_processor_count = xe::threading::logical_processor_count();
    if (!logical_processor_count) {
      logical_processor_count = 6;
    }
    size_t creation_thread_original_count = creation_threads_.size();
    size_t creation_thread_needed_count = std::max(
        std::min(pipeline_stored_descriptions.size(), logical_processor_count) -
            size_t(1),
        creation_thread_original_count);
    while (creation_threads_.size() < creation_thread_needed_count) {
      size_t creation_thread_index = creation_threads_.size();
      std::unique_ptr<xe::threading::Thread> creation_thread =
          xe::threading::Thread::Create({}, [this, creation_thread_index]() {
            CreationThread(creation_thread_index);
          });
      assert_not_null(creation_thread);
      creation_thread->set_name("D3D12 Pipelines");
      creation_threads_.push_back(std::move(creation_thread));
    }

    size_t pipelines_created = 0;
    size_t pipelines_already_exist = 0;
    size_t pipelines_vs_not_found = 0;
    size_t pipelines_vs_translation_missing = 0;
    size_t pipelines_ps_not_found = 0;
    size_t pipelines_ps_translation_missing = 0;
    size_t pipelines_root_sig_failed = 0;
    for (const PipelineStoredDescription& pipeline_stored_description :
         pipeline_stored_descriptions) {
      const PipelineDescription& pipeline_description =
          pipeline_stored_description.description;
      // TODO(Triang3l): On Vulkan, skip pipelines requiring unsupported device
      // features (to keep the cache files mostly shareable across devices).
      // Skip already known pipelines - those have already been enqueued.
      auto found_range =
          pipelines_.equal_range(pipeline_stored_description.description_hash);
      bool pipeline_found = false;
      for (auto it = found_range.first; it != found_range.second; ++it) {
        Pipeline* found_pipeline = it->second;
        if (!std::memcmp(&found_pipeline->description.description,
                         &pipeline_description, sizeof(pipeline_description))) {
          pipeline_found = true;
          break;
        }
      }
      if (pipeline_found) {
        ++pipelines_already_exist;
        continue;
      }

      PipelineRuntimeDescription pipeline_runtime_description;
      auto vertex_shader_it =
          shaders_.find(pipeline_description.vertex_shader_hash);
      if (vertex_shader_it == shaders_.end()) {
        ++pipelines_vs_not_found;
        XELOGW("Pipeline cache: VS {:016X} not found in shader storage",
               pipeline_description.vertex_shader_hash);
        continue;
      }
      D3D12Shader* vertex_shader = vertex_shader_it->second;
      pipeline_runtime_description.vertex_shader =
          static_cast<D3D12Shader::D3D12Translation*>(
              vertex_shader->GetTranslation(
                  pipeline_description.vertex_shader_modification));
      if (!pipeline_runtime_description.vertex_shader ||
          !pipeline_runtime_description.vertex_shader->is_translated() ||
          !pipeline_runtime_description.vertex_shader->is_valid()) {
        ++pipelines_vs_translation_missing;
        XELOGW(
            "Pipeline cache: VS {:016X} mod {:016X} translation "
            "missing/invalid",
            pipeline_description.vertex_shader_hash,
            pipeline_description.vertex_shader_modification);
        continue;
      }
      D3D12Shader* pixel_shader;
      if (pipeline_description.pixel_shader_hash) {
        auto pixel_shader_it =
            shaders_.find(pipeline_description.pixel_shader_hash);
        if (pixel_shader_it == shaders_.end()) {
          ++pipelines_ps_not_found;
          XELOGW("Pipeline cache: PS {:016X} not found in shader storage",
                 pipeline_description.pixel_shader_hash);
          continue;
        }
        pixel_shader = pixel_shader_it->second;
        pipeline_runtime_description.pixel_shader =
            static_cast<D3D12Shader::D3D12Translation*>(
                pixel_shader->GetTranslation(
                    pipeline_description.pixel_shader_modification));
        if (!pipeline_runtime_description.pixel_shader ||
            !pipeline_runtime_description.pixel_shader->is_translated() ||
            !pipeline_runtime_description.pixel_shader->is_valid()) {
          ++pipelines_ps_translation_missing;
          XELOGW(
              "Pipeline cache: PS {:016X} mod {:016X} translation "
              "missing/invalid",
              pipeline_description.pixel_shader_hash,
              pipeline_description.pixel_shader_modification);
          continue;
        }
      } else {
        pixel_shader = nullptr;
        pipeline_runtime_description.pixel_shader = nullptr;
      }
      GeometryShaderKey pipeline_geometry_shader_key;
      pipeline_runtime_description.geometry_shader =
          GetGeometryShaderKey(
              pipeline_description.geometry_shader,
              DxbcShaderTranslator::Modification(
                  pipeline_description.vertex_shader_modification),
              DxbcShaderTranslator::Modification(
                  pipeline_description.pixel_shader_modification),
              pipeline_geometry_shader_key)
              ? &GetGeometryShader(pipeline_geometry_shader_key)
              : nullptr;
      pipeline_runtime_description.root_signature =
          command_processor_.GetRootSignature(
              vertex_shader, pixel_shader,
              Shader::IsHostVertexShaderTypeDomain(
                  DxbcShaderTranslator::Modification(
                      pipeline_description.vertex_shader_modification)
                      .vertex.host_vertex_shader_type));
      if (!pipeline_runtime_description.root_signature) {
        ++pipelines_root_sig_failed;
        XELOGW(
            "Pipeline cache: Root signature failed for VS {:016X} PS {:016X}",
            pipeline_description.vertex_shader_hash,
            pipeline_description.pixel_shader_hash);
        continue;
      }
      std::memcpy(&pipeline_runtime_description.description,
                  &pipeline_description, sizeof(pipeline_description));

      Pipeline* new_pipeline = new Pipeline;
      std::memcpy(&new_pipeline->description, &pipeline_runtime_description,
                  sizeof(pipeline_runtime_description));
      // Calculate priority based on whether shader writes to visible RTs.
      if (pixel_shader) {
        uint32_t bound_rts =
            (pipeline_description.render_targets[0].used ? 1 : 0) |
            (pipeline_description.render_targets[1].used ? 2 : 0) |
            (pipeline_description.render_targets[2].used ? 4 : 0) |
            (pipeline_description.render_targets[3].used ? 8 : 0);
        new_pipeline->priority = pipeline_util::CalculatePipelinePriority(
            bound_rts, pixel_shader->writes_color_targets(),
            pixel_shader->writes_depth());
      }
      pipelines_.emplace(pipeline_stored_description.description_hash,
                         new_pipeline);
      COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());
      if (!creation_threads_.empty()) {
        // Submit the pipeline for creation to any available thread.
        {
          std::lock_guard<xe_mutex> lock(creation_request_lock_);
          creation_queue_.push(new_pipeline);
        }
        creation_request_cond_.notify_one();
      } else {
        new_pipeline->state.store(
            CreateD3D12Pipeline(pipeline_runtime_description),
            std::memory_order_release);
      }
      ++pipelines_created;
    }

    if (!creation_threads_.empty()) {
      if (blocking) {
        // Blocking mode: help drain the queue on this thread, then wait for
        // background threads to finish.
        CreateQueuedPipelinesOnProcessorThread();
        if (creation_threads_.size() > creation_thread_original_count) {
          {
            std::lock_guard<xe_mutex> lock(creation_request_lock_);
            creation_threads_shutdown_from_ = creation_thread_original_count;
            // Assuming the queue is empty because of
            // CreateQueuedPipelinesOnProcessorThread.
          }
          creation_request_cond_.notify_all();
          while (creation_threads_.size() > creation_thread_original_count) {
            xe::threading::Wait(creation_threads_.back().get(), false);
            creation_threads_.pop_back();
          }
          {
            // Cleanup so additional threads can be created later again.
            std::lock_guard<xe_mutex> lock(creation_request_lock_);
            creation_threads_shutdown_from_ = SIZE_MAX;
          }
        }
        // Wait for any background threads (including original ones) to finish
        // creating pipelines they may have popped from the queue. This ensures
        // all cached pipelines are fully created before the game starts,
        // populating the driver's shader cache.
        bool await_creation_completion_event;
        {
          std::lock_guard<xe_mutex> lock(creation_request_lock_);
          await_creation_completion_event = creation_threads_busy_ != 0;
          if (await_creation_completion_event) {
            creation_completion_event_->Reset();
            creation_completion_set_event_ = true;
          }
        }
        if (await_creation_completion_event) {
          creation_request_cond_.notify_one();
          xe::threading::Wait(creation_completion_event_.get(), false);
        }
      } else {
        // Non-blocking mode: let background threads handle all pipeline
        // creation. Store completion callback to be invoked when done.
        std::lock_guard<xe_mutex> lock(creation_request_lock_);
        if (creation_queue_.empty() && creation_threads_busy_ == 0) {
          // No work pending - callback will be invoked at end of function.
        } else {
          creation_completion_callback_ = std::move(completion_callback);
          completion_callback =
              nullptr;  // Prevent invocation at end of function
        }
      }
    }

    XELOGI(
        "Pipeline cache loaded: {} created, {} already exist, {} total stored",
        pipelines_created, pipelines_already_exist,
        pipeline_stored_descriptions.size());
    if (pipelines_vs_not_found || pipelines_vs_translation_missing ||
        pipelines_ps_not_found || pipelines_ps_translation_missing ||
        pipelines_root_sig_failed) {
      XELOGI(
          "Pipeline cache skipped: {} VS not found, {} VS translation missing, "
          "{} PS not found, {} PS translation missing, {} root sig failed",
          pipelines_vs_not_found, pipelines_vs_translation_missing,
          pipelines_ps_not_found, pipelines_ps_translation_missing,
          pipelines_root_sig_failed);
    }
    XELOGI("Pipeline creation took {} milliseconds",
           (xe::Clock::QueryHostTickCount() - pipeline_creation_start_) * 1000 /
               xe::Clock::QueryHostTickFrequency());
  }

  shader_storage_title_id_ = title_id;

  // Invoke completion callback if provided (for blocking mode or when no
  // background work was needed). For non-blocking mode with background work,
  // the callback is stored and invoked by CreationThread when done.
  if (completion_callback) {
    completion_callback();
  }
}

void PipelineCache::ShutdownShaderStorage() {
  // Shut down the storage writer (closes files, stops write thread).
  storage_writer_.ShutdownShaderStorage();
  shader_storage_file_flush_needed_ = false;
  pipeline_storage_file_flush_needed_ = false;
  shader_storage_title_id_ = 0;
}

void PipelineCache::EndSubmission() {
  if (shader_storage_file_flush_needed_ ||
      pipeline_storage_file_flush_needed_) {
    storage_writer_.RequestFlush(shader_storage_file_flush_needed_,
                                 pipeline_storage_file_flush_needed_);
    shader_storage_file_flush_needed_ = false;
    pipeline_storage_file_flush_needed_ = false;
  }
  if (!creation_threads_.empty()) {
    // Don't wait for pipeline creation - let background threads work
    // asynchronously. Draws will be skipped until pipelines are ready.
    // This avoids frame-time spikes from blocking on pipeline creation.
    creation_request_cond_.notify_one();
  }
}

bool PipelineCache::IsCreatingPipelines() {
  if (creation_threads_.empty()) {
    return false;
  }
  std::lock_guard<xe_mutex> lock(creation_request_lock_);
  return !creation_queue_.empty() || creation_threads_busy_ != 0;
}

void PipelineCache::AwaitPipelineCompletion() {
  if (creation_threads_.empty()) {
    return;
  }

  bool await_creation_completion_event;
  {
    std::lock_guard<xe_mutex> lock(creation_request_lock_);
    await_creation_completion_event =
        !creation_queue_.empty() || creation_threads_busy_ != 0;
    if (await_creation_completion_event) {
      creation_completion_event_->Reset();
      creation_completion_set_event_ = true;
    }
  }

  if (await_creation_completion_event) {
    creation_request_cond_.notify_one();
    xe::threading::Wait(creation_completion_event_.get(), false);
  }
}

ID3D12PipelineState* PipelineCache::AwaitD3D12PipelineByHandle(void* handle) {
  ID3D12PipelineState* pipeline = GetD3D12PipelineByHandle(handle);
  if (pipeline != nullptr) {
    return pipeline;
  }
  AwaitPipelineCompletion();
  return GetD3D12PipelineByHandle(handle);
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                       const uint32_t* host_address,
                                       uint32_t dword_count) {
  // Hash the input memory and lookup the shader.
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(uint32_t)));
}

D3D12Shader* PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                       const uint32_t* host_address,
                                       uint32_t dword_count,
                                       uint64_t data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    // Shader has been previously loaded.
    return it->second;
  }
  // Always create the shader and stash it away.
  // We need to track it even if it fails translation so we know not to try
  // again.
  D3D12Shader* shader =
      new D3D12Shader(shader_type, data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

DxbcShaderTranslator::Modification
PipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultVertexShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
          host_vertex_shader_type));

  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes =
      pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.vertex_kill_and =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                   xenos::PrimitiveType::kPointList);

  return modification;
}

DxbcShaderTranslator::Modification
PipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification(
      shader_translator_->GetDefaultPixelShaderModification(
          shader.GetDynamicAddressableRegisterCount(
              regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));

  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask &
      ~xenos::GetInterpolatorSamplingPattern(
          regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
          regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
          regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  if (render_target_cache_.GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    using DepthStencilMode =
        DxbcShaderTranslator::Modification::DepthStencilMode;
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        normalized_depth_control.z_enable &&
        regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
            xenos::DepthRenderTargetFormat::kD24FS8) {
      modification.pixel.depth_stencil_mode =
          render_target_cache_.depth_float24_round()
              ? DepthStencilMode::kFloat24Rounding
              : DepthStencilMode::kFloat24Truncating;
    } else {
      if (shader.implicit_early_z_write_allowed() &&
          (!shader.writes_color_target(0) ||
           !draw_util::DoesCoverageDependOnAlpha(
               regs.Get<reg::RB_COLORCONTROL>()))) {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
      } else {
        modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
      }
    }

    // Check if MIN/MAX blend is used with non-trivial source factors.
    // D3D12 fixed-function blend ignores factors for MIN/MAX, but Xbox 360
    // applies them. If the destination factor is ONE (or ZERO), we can
    // pre-multiply the shader output by the source factor to emulate this.
    // Only RT0 is supported for now.
    modification.pixel.rt0_blend_rgb_factor_for_premult =
        xenos::BlendFactor::kOne;
    modification.pixel.rt0_blend_a_factor_for_premult =
        xenos::BlendFactor::kOne;

    if (shader.writes_color_target(0)) {
      auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(
          reg::RB_BLENDCONTROL::rt_register_indices[0]);

      // Pre-multiply by kSrcAlpha for MIN/MAX blend ops when dstFactor is ONE.
      if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.color_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_rgb_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }

      if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
           blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
          blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
          blend_control.alpha_destblend == xenos::BlendFactor::kOne) {
        modification.pixel.rt0_blend_a_factor_for_premult =
            xenos::BlendFactor::kSrcAlpha;
      }
    }
  }

  return modification;
}

bool PipelineCache::ConfigurePipeline(
    D3D12Shader::D3D12Translation* vertex_shader,
    D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    void** pipeline_handle_out, ID3D12RootSignature** root_signature_out) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  assert_not_null(pipeline_handle_out);
  assert_not_null(root_signature_out);

  // Ensure shaders are translated - needed now for GetCurrentStateDescription.
  // Edge flags are not supported yet (because polygon primitives are not).
  assert_true(register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdge &&
              register_file_.Get<reg::SQ_PROGRAM_CNTL>().vs_export_mode !=
                  xenos::VertexShaderExportMode::kPosition2VectorsEdgeKill);
  assert_false(register_file_.Get<reg::SQ_PROGRAM_CNTL>().gen_index_vtx);

  // Check if we should use async pipeline creation.
  // When enabled, defer shader translation and pipeline creation to background.
  // Only use async when there's a pixel shader - VS-only pipelines are fast
  // to compile and don't benefit from async (vertex shaders are small).
  bool use_async = cvars::async_shader_compilation &&
                   !creation_threads_.empty() && pixel_shader != nullptr;

  // Ensure VS ucode is analyzed (needed for description hash).
  if (!vertex_shader->shader().is_ucode_analyzed()) {
    vertex_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
  }

  // For async mode, defer VS translation to background thread.
  // For sync mode, translate VS now on main thread.
  if (!vertex_shader->is_translated() && !use_async) {
    if (!TranslateAnalyzedShader(*shader_translator_, *vertex_shader,
                                 dxbc_converter_, dxc_utils_, dxc_compiler_)) {
      XELOGE("Failed to translate the vertex shader!");
      return false;
    }
    if (storage_writer_.is_active() &&
        vertex_shader->shader().try_set_ucode_storage_index(
            storage_writer_.storage_index())) {
      shader_storage_file_flush_needed_ = true;
      storage_writer_.QueueShaderWrite(&vertex_shader->shader());
    }
  }
  if (!use_async && !vertex_shader->is_valid()) {
    // Translation attempted previously, but not valid (sync mode only).
    return false;
  }

  if (pixel_shader != nullptr && !use_async) {
    // Sync mode - must translate PS now on main thread.
    // No mutex needed - main thread translator is not shared with background
    // threads (they have their own translators).
    if (!pixel_shader->is_translated()) {
      if (!pixel_shader->shader().is_ucode_analyzed()) {
        pixel_shader->shader().AnalyzeUcode(ucode_disasm_buffer_);
      }
      if (!TranslateAnalyzedShader(*shader_translator_, *pixel_shader,
                                   dxbc_converter_, dxc_utils_,
                                   dxc_compiler_)) {
        XELOGE("Failed to translate the pixel shader!");
        return false;
      }
      if (storage_writer_.is_active() &&
          pixel_shader->shader().try_set_ucode_storage_index(
              storage_writer_.storage_index())) {
        shader_storage_file_flush_needed_ = true;
        storage_writer_.QueueShaderWrite(&pixel_shader->shader());
      }
    }
    if (!pixel_shader->is_valid()) {
      // Translation attempted previously, but not valid.
      return false;
    }
  }

  PipelineRuntimeDescription runtime_description;
  if (!GetCurrentStateDescription(
          vertex_shader, pixel_shader, primitive_processing_result,
          normalized_depth_control, normalized_color_mask,
          bound_depth_and_color_render_target_bits,
          bound_depth_and_color_render_target_formats, runtime_description,
          use_async)) {
    return false;
  }
  PipelineDescription& description = runtime_description.description;

  if (current_pipeline_ != nullptr &&
      current_pipeline_->description.description == description) {
    *pipeline_handle_out = current_pipeline_;
    *root_signature_out = current_pipeline_->description.root_signature;
    return true;
  }

  // Find an existing pipeline in the cache.
  uint64_t hash = XXH3_64bits(&description, sizeof(description));
  auto found_range = pipelines_.equal_range(hash);
  for (auto it = found_range.first; it != found_range.second; ++it) {
    Pipeline* found_pipeline = it->second;
    if (found_pipeline->description.description == description) {
      current_pipeline_ = found_pipeline;
      *pipeline_handle_out = found_pipeline;
      *root_signature_out = found_pipeline->description.root_signature;
      return true;
    }
  }

  Pipeline* new_pipeline = new Pipeline;
  std::memcpy(&new_pipeline->description, &runtime_description,
              sizeof(runtime_description));
  pipelines_.emplace(hash, new_pipeline);
  COUNT_profile_set("gpu/pipeline_cache/pipelines", pipelines_.size());

  if (use_async) {
    // Queue for background thread.
    new_pipeline->pending_vertex_shader = vertex_shader;
    new_pipeline->pending_pixel_shader = pixel_shader;
    // Calculate priority based on whether shader writes to visible RTs.
    if (pixel_shader) {
      uint32_t bound_rts = pipeline_util::GetBoundRTMaskFromNormalizedColorMask(
          normalized_color_mask);
      new_pipeline->priority = pipeline_util::CalculatePipelinePriority(
          bound_rts, pixel_shader->shader().writes_color_targets(),
          pixel_shader->shader().writes_depth());
    }
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      creation_queue_.push(new_pipeline);
    }
    creation_request_cond_.notify_one();
  } else {
    // Sync mode or no creation threads: create synchronously.
    new_pipeline->state.store(CreateD3D12Pipeline(runtime_description),
                              std::memory_order_release);
  }

  if (storage_writer_.is_active()) {
    pipeline_storage_file_flush_needed_ = true;
    PipelineStoredDescription stored_description;
    stored_description.description_hash = hash;
    std::memcpy(&stored_description.description, &description,
                sizeof(description));
    storage_writer_.QueuePipelineWrite(stored_description);
  }

  current_pipeline_ = new_pipeline;
  *pipeline_handle_out = new_pipeline;
  *root_signature_out = runtime_description.root_signature;
  return true;
}

bool PipelineCache::TranslateAnalyzedShader(
    DxbcShaderTranslator& translator,
    D3D12Shader::D3D12Translation& translation, IDxbcConverter* dxbc_converter,
    IDxcUtils* dxc_utils, IDxcCompiler* dxc_compiler) {
  D3D12Shader& shader = static_cast<D3D12Shader&>(translation.shader());

  // Perform translation.
  // If this fails the shader will be marked as invalid and ignored later.
  if (!translator.TranslateAnalyzedShader(translation)) {
    XELOGE("Shader {:016X} translation failed; marking as ignored",
           shader.ucode_data_hash());
    return false;
  }

  const char* host_shader_type;
  if (shader.type() == xenos::ShaderType::kVertex) {
    DxbcShaderTranslator::Modification modification(translation.modification());
    switch (modification.vertex.host_vertex_shader_type) {
      case Shader::HostVertexShaderType::kLineDomainCPIndexed:
        host_shader_type = "control-point-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kLineDomainPatchIndexed:
        host_shader_type = "patch-indexed line domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
        host_shader_type = "control-point-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
        host_shader_type = "patch-indexed triangle domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
        host_shader_type = "control-point-indexed quad domain";
        break;
      case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
        host_shader_type = "patch-indexed quad domain";
        break;
      default:
        assert(modification.vertex.host_vertex_shader_type ==
               Shader::HostVertexShaderType::kVertex);
        host_shader_type = "vertex";
    }
  } else {
    host_shader_type = "pixel";
  }
  XELOGGPU("Generated {} shader ({}b) - hash {:016X}:\n{}\n", host_shader_type,
           shader.ucode_dword_count() * sizeof(uint32_t),
           shader.ucode_data_hash(), shader.ucode_disassembly().c_str());

  // Set up texture and sampler binding layouts.
  if (shader.EnterBindingLayoutUserUIDSetup()) {
    const std::vector<D3D12Shader::TextureBinding>& texture_bindings =
        shader.GetTextureBindingsAfterTranslation();
    size_t texture_binding_count = texture_bindings.size();
    const std::vector<D3D12Shader::SamplerBinding>& sampler_bindings =
        shader.GetSamplerBindingsAfterTranslation();
    size_t sampler_binding_count = sampler_bindings.size();
    assert_false(bindless_resources_used_ &&
                 texture_binding_count + sampler_binding_count >
                     D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 4);
    size_t texture_binding_layout_bytes =
        texture_binding_count * sizeof(*texture_bindings.data());
    uint64_t texture_binding_layout_hash = 0;
    if (texture_binding_count) {
      texture_binding_layout_hash =
          XXH3_64bits(texture_bindings.data(), texture_binding_layout_bytes);
    }
    size_t bindless_sampler_count =
        bindless_resources_used_ ? sampler_binding_count : 0;
    uint64_t bindless_sampler_layout_hash = 0;
    if (bindless_sampler_count) {
      XXH3_state_t hash_state;
      XXH3_64bits_reset(&hash_state);
      for (size_t i = 0; i < bindless_sampler_count; ++i) {
        XXH3_64bits_update(
            &hash_state, &sampler_bindings[i].bindless_descriptor_index,
            sizeof(sampler_bindings[i].bindless_descriptor_index));
      }
      bindless_sampler_layout_hash = XXH3_64bits_digest(&hash_state);
    }
    // Obtain the unique IDs of binding layouts if there are any texture
    // bindings or bindless samplers, for invalidation in the command processor.
    size_t texture_binding_layout_uid = kLayoutUIDEmpty;
    // Use sampler count for the bindful case because it's the only thing that
    // must be the same for layouts to be compatible in this case
    // (instruction-specified parameters are used as overrides for actual
    // samplers).
    static_assert(
        kLayoutUIDEmpty == 0,
        "Empty layout UID is assumed to be 0 because for bindful samplers, the "
        "UID is their count");
    size_t sampler_binding_layout_uid =
        bindless_resources_used_ ? kLayoutUIDEmpty : sampler_binding_count;
    if (texture_binding_count || bindless_sampler_count) {
      std::lock_guard<std::mutex> layouts_lock(layouts_mutex_);
      if (texture_binding_count) {
        auto found_range = texture_binding_layout_map_.equal_range(
            texture_binding_layout_hash);
        for (auto it = found_range.first; it != found_range.second; ++it) {
          if (it->second.vector_span_length == texture_binding_count &&
              !std::memcmp(texture_binding_layouts_.data() +
                               it->second.vector_span_offset,
                           texture_bindings.data(),
                           texture_binding_layout_bytes)) {
            texture_binding_layout_uid = it->second.uid;
            break;
          }
        }
        if (texture_binding_layout_uid == kLayoutUIDEmpty) {
          static_assert(
              kLayoutUIDEmpty == 0,
              "Layout UID is size + 1 because it's assumed that 0 is the UID "
              "for an empty layout");
          texture_binding_layout_uid = texture_binding_layout_map_.size() + 1;
          LayoutUID new_uid;
          new_uid.uid = texture_binding_layout_uid;
          new_uid.vector_span_offset = texture_binding_layouts_.size();
          new_uid.vector_span_length = texture_binding_count;
          texture_binding_layouts_.resize(new_uid.vector_span_offset +
                                          texture_binding_count);
          std::memcpy(
              texture_binding_layouts_.data() + new_uid.vector_span_offset,
              texture_bindings.data(), texture_binding_layout_bytes);
          texture_binding_layout_map_.emplace(texture_binding_layout_hash,
                                              new_uid);
        }
      }
      if (bindless_sampler_count) {
        auto found_range = bindless_sampler_layout_map_.equal_range(
            sampler_binding_layout_uid);
        for (auto it = found_range.first; it != found_range.second; ++it) {
          if (it->second.vector_span_length != bindless_sampler_count) {
            continue;
          }
          sampler_binding_layout_uid = it->second.uid;
          const uint32_t* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + it->second.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            if (vector_bindless_sampler_layout[i] !=
                sampler_bindings[i].bindless_descriptor_index) {
              sampler_binding_layout_uid = kLayoutUIDEmpty;
              break;
            }
          }
          if (sampler_binding_layout_uid != kLayoutUIDEmpty) {
            break;
          }
        }
        if (sampler_binding_layout_uid == kLayoutUIDEmpty) {
          sampler_binding_layout_uid = bindless_sampler_layout_map_.size();
          LayoutUID new_uid;
          static_assert(
              kLayoutUIDEmpty == 0,
              "Layout UID is size + 1 because it's assumed that 0 is the UID "
              "for an empty layout");
          new_uid.uid = sampler_binding_layout_uid + 1;
          new_uid.vector_span_offset = bindless_sampler_layouts_.size();
          new_uid.vector_span_length = sampler_binding_count;
          bindless_sampler_layouts_.resize(new_uid.vector_span_offset +
                                           sampler_binding_count);
          uint32_t* vector_bindless_sampler_layout =
              bindless_sampler_layouts_.data() + new_uid.vector_span_offset;
          for (size_t i = 0; i < bindless_sampler_count; ++i) {
            vector_bindless_sampler_layout[i] =
                sampler_bindings[i].bindless_descriptor_index;
          }
          bindless_sampler_layout_map_.emplace(bindless_sampler_layout_hash,
                                               new_uid);
        }
      }
    }
    shader.SetTextureBindingLayoutUserUID(texture_binding_layout_uid);
    shader.SetSamplerBindingLayoutUserUID(sampler_binding_layout_uid);
  }

  // Disassemble the shader for dumping.
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  if (cvars::d3d12_dxbc_disasm_dxilconv) {
    translation.DisassembleDxbcAndDxil(provider, cvars::d3d12_dxbc_disasm,
                                       dxbc_converter, dxc_utils, dxc_compiler);
  } else {
    translation.DisassembleDxbcAndDxil(provider, cvars::d3d12_dxbc_disasm);
  }

  // Dump shader files if desired.
  if (!cvars::dump_shaders.empty()) {
    bool edram_rov_used = render_target_cache_.GetPath() ==
                          RenderTargetCache::Path::kPixelShaderInterlock;
    translation.Dump(cvars::dump_shaders,
                     (shader.type() == xenos::ShaderType::kPixel)
                         ? (edram_rov_used ? "d3d12_rov" : "d3d12_rtv")
                         : "d3d12");
  }

  return translation.is_valid();
}

void PipelineCache::TranslateShadersForStorage(
    const std::set<std::pair<uint64_t, uint64_t>>& translations_needed,
    bool edram_rov_used) {
  uint64_t translation_start = xe::Clock::QueryHostTickCount();

  std::vector<D3D12Shader*> shaders_to_translate_list;
  shaders_to_translate_list.reserve(shaders_.size());
  for (auto& shader_pair : shaders_) {
    D3D12Shader* shader = shader_pair.second;
    uint64_t ucode_data_hash = shader->ucode_data_hash();
    if (translations_needed.lower_bound(
            std::make_pair(ucode_data_hash, uint64_t(0))) !=
        translations_needed.upper_bound(
            std::make_pair(ucode_data_hash, UINT64_MAX))) {
      shaders_to_translate_list.push_back(shader);
    }
  }

  if (shaders_to_translate_list.empty()) {
    return;
  }

  std::atomic<size_t> translation_index{0};
  std::mutex shaders_failed_to_translate_mutex;
  std::vector<D3D12Shader::D3D12Translation*> shaders_failed_to_translate;

  auto translate_function = [&, this]() {
    const ui::d3d12::D3D12Provider& provider =
        command_processor_.GetD3D12Provider();
    StringBuffer ucode_disasm_buffer;
    DxbcShaderTranslator translator(
        provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
        !(edram_rov_used ||
          render_target_cache_.gamma_render_target_as_unorm16()),
        render_target_cache_.msaa_2x_supported(),
        render_target_cache_.draw_resolution_scale_x(),
        render_target_cache_.draw_resolution_scale_y(),
        provider.GetGraphicsAnalysis() != nullptr);
    // DXIL conversion objects.
    IDxbcConverter* dxbc_converter = nullptr;
    IDxcUtils* dxc_utils = nullptr;
    IDxcCompiler* dxc_compiler = nullptr;
    if (cvars::d3d12_dxbc_disasm_dxilconv && dxbc_converter_ && dxc_utils_ &&
        dxc_compiler_) {
      provider.DxbcConverterCreateInstance(CLSID_DxbcConverter,
                                           IID_PPV_ARGS(&dxbc_converter));
      provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils));
      provider.DxcCreateInstance(CLSID_DxcCompiler,
                                 IID_PPV_ARGS(&dxc_compiler));
    }

    while (true) {
      size_t index = translation_index.fetch_add(1);
      if (index >= shaders_to_translate_list.size()) {
        break;
      }
      D3D12Shader* shader = shaders_to_translate_list[index];
      if (!shader->is_ucode_analyzed()) {
        shader->AnalyzeUcode(ucode_disasm_buffer);
      }
      uint64_t ucode_data_hash = shader->ucode_data_hash();
      for (auto modification_it = translations_needed.lower_bound(
               std::make_pair(ucode_data_hash, uint64_t(0)));
           modification_it != translations_needed.end() &&
           modification_it->first == ucode_data_hash;
           ++modification_it) {
        D3D12Shader::D3D12Translation* translation =
            static_cast<D3D12Shader::D3D12Translation*>(
                shader->GetOrCreateTranslation(modification_it->second));
        if (!translation->is_translated() &&
            !TranslateAnalyzedShader(translator, *translation, dxbc_converter,
                                     dxc_utils, dxc_compiler)) {
          std::lock_guard<std::mutex> lock(shaders_failed_to_translate_mutex);
          shaders_failed_to_translate.push_back(translation);
        }
      }
    }

    if (dxc_compiler) {
      dxc_compiler->Release();
    }
    if (dxc_utils) {
      dxc_utils->Release();
    }
    if (dxbc_converter) {
      dxbc_converter->Release();
    }
  };

  size_t logical_processor_count = xe::threading::logical_processor_count();
  if (!logical_processor_count) {
    logical_processor_count = 6;
  }
  size_t thread_count = std::min(logical_processor_count - size_t(1),
                                 shaders_to_translate_list.size());
  std::vector<std::unique_ptr<xe::threading::Thread>> translation_threads;
  for (size_t i = 0; i < thread_count; ++i) {
    auto thread = xe::threading::Thread::Create({}, translate_function);
    if (thread) {
      thread->set_name("Shader Translation");
      translation_threads.push_back(std::move(thread));
    }
  }

  // Main thread also participates.
  translate_function();

  for (auto& thread : translation_threads) {
    xe::threading::Wait(thread.get(), false);
  }

  for (D3D12Shader::D3D12Translation* translation :
       shaders_failed_to_translate) {
    D3D12Shader* shader = static_cast<D3D12Shader*>(&translation->shader());
    shader->DestroyTranslation(translation->modification());
    if (shader->translations().empty()) {
      shaders_.erase(shader->ucode_data_hash());
      delete shader;
    }
  }

  XELOGI("Translated {} shaders in {} ms",
         shaders_to_translate_list.size() - shaders_failed_to_translate.size(),
         (xe::Clock::QueryHostTickCount() - translation_start) * 1000 /
             xe::Clock::QueryHostTickFrequency());
}

bool PipelineCache::GetCurrentStateDescription(
    D3D12Shader::D3D12Translation* vertex_shader,
    D3D12Shader::D3D12Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    PipelineRuntimeDescription& runtime_description_out, bool for_placeholder) {
  // Translated shaders needed at least for the root signature.
  // Exception: for_placeholder mode (async pipeline creation) allows
  // untranslated shaders - root signature uses VS bindings only initially,
  // updated after background translation.
  assert_true(for_placeholder ||
              (vertex_shader->is_translated() && vertex_shader->is_valid()));
  assert_true(!pixel_shader || for_placeholder ||
              (pixel_shader->is_translated() && pixel_shader->is_valid()));

  PipelineDescription& description_out = runtime_description_out.description;

  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  // Initialize all unused fields to zero for comparison/hashing.
  std::memset(&runtime_description_out, 0, sizeof(runtime_description_out));

  assert_true(DxbcShaderTranslator::Modification(vertex_shader->modification())
                  .vertex.host_vertex_shader_type ==
              primitive_processing_result.host_vertex_shader_type);
  bool tessellated = primitive_processing_result.IsTessellated();
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  // In Direct3D, rasterization (along with pixel counting) is disabled by
  // disabling the pixel shader and depth / stencil. However, if rasterization
  // should be disabled, the pixel shader must be disabled externally, to ensure
  // things like texture binding layout is correct for the shader actually being
  // used (don't replace anything here).
  if (!rasterization_enabled) {
    assert_null(pixel_shader);
    if (pixel_shader) {
      return false;
    }
  }

  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  // Root signature.
  // For placeholder mode, pass nullptr for pixel_shader since placeholder PS
  // has no texture/sampler bindings - root signature only needs VS bindings.
  runtime_description_out.root_signature = command_processor_.GetRootSignature(
      static_cast<const DxbcShader*>(&vertex_shader->shader()),
      (pixel_shader && !for_placeholder)
          ? static_cast<const DxbcShader*>(&pixel_shader->shader())
          : nullptr,
      tessellated);
  if (runtime_description_out.root_signature == nullptr) {
    return false;
  }

  // Vertex shader.
  runtime_description_out.vertex_shader = vertex_shader;
  description_out.vertex_shader_hash =
      vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();

  // Index buffer strip cut value.
  if (primitive_processing_result.host_primitive_reset_enabled) {
    description_out.strip_cut_index =
        primitive_processing_result.host_index_format ==
                xenos::IndexFormat::kInt16
            ? PipelineStripCutIndex::kFFFF
            : PipelineStripCutIndex::kFFFFFFFF;
  } else {
    description_out.strip_cut_index = PipelineStripCutIndex::kNone;
  }

  // Host vertex shader type and primitive topology.
  if (tessellated) {
    description_out.primitive_topology_type_or_tessellation_mode =
        uint32_t(primitive_processing_result.tessellation_mode);
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kPoint);
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      // Quads are emulated as line lists with adjacency.
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::k2DLineStrip:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kLine);
        break;
      default:
        description_out.primitive_topology_type_or_tessellation_mode =
            uint32_t(PipelinePrimitiveTopologyType::kTriangle);
        break;
    }
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.geometry_shader = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        description_out.geometry_shader =
            PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        description_out.geometry_shader = PipelineGeometryShader::kQuadList;
        break;
      default:
        description_out.geometry_shader = PipelineGeometryShader::kNone;
        break;
    }
  }
  GeometryShaderKey geometry_shader_key;
  runtime_description_out.geometry_shader =
      GetGeometryShaderKey(
          description_out.geometry_shader,
          DxbcShaderTranslator::Modification(vertex_shader->modification()),
          DxbcShaderTranslator::Modification(
              pixel_shader ? pixel_shader->modification() : 0),
          geometry_shader_key)
          ? &GetGeometryShader(geometry_shader_key)
          : nullptr;

  // The rest doesn't matter when rasterization is disabled (thus no writing to
  // anywhere from post-geometry stages and no samples are counted).
  if (!rasterization_enabled) {
    description_out.cull_mode = PipelineCullMode::kDisableRasterization;
    return true;
  }

  // Pixel shader.
  if (pixel_shader) {
    runtime_description_out.pixel_shader = pixel_shader;
    description_out.pixel_shader_hash =
        pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
  }

  // Rasterizer state.
  // Because Direct3D 12 doesn't support per-side fill mode and depth bias, the
  // values to use depends on the current culling state.
  // If front faces are culled, use the ones for back faces.
  // If back faces are culled, it's the other way around.
  // If culling is not enabled, assume the developer wanted to draw things in a
  // more special way - so if one side is wireframe or has a depth bias, then
  // that's intentional (if both sides have a depth bias, the one for the front
  // faces is used, though it's unlikely that they will ever be different -
  // SetRenderState sets the same offset for both sides).
  // Points fill mode (0) also isn't supported in Direct3D 12, but assume the
  // developer didn't want to fill the whole primitive and use wireframe (like
  // Xenos fill mode 1).
  // Here we also assume that only one side is culled - if two sides are culled,
  // rasterization will be disabled externally, or the draw call will be dropped
  // early if the vertex shader doesn't export to memory.
  bool cull_front, cull_back;
  if (primitive_polygonal) {
    description_out.front_counter_clockwise = pa_su_sc_mode_cntl.face == 0;
    cull_front = pa_su_sc_mode_cntl.cull_front != 0;
    cull_back = pa_su_sc_mode_cntl.cull_back != 0;
    if (cull_front) {
      // The case when both faces are culled should be handled by disabling
      // rasterization.
      assert_false(cull_back);
      description_out.cull_mode = PipelineCullMode::kFront;
    } else if (cull_back) {
      description_out.cull_mode = PipelineCullMode::kBack;
    } else {
      description_out.cull_mode = PipelineCullMode::kNone;
    }
    // With ROV, the depth bias is applied in the pixel shader because
    // per-sample depth is needed for MSAA.
    if (!cull_front) {
      // Front faces aren't culled.
      // Direct3D 12, unfortunately, doesn't support point fill mode.
      if (pa_su_sc_mode_cntl.polymode_front_ptype !=
          xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (!cull_back) {
      // Back faces aren't culled.
      if (pa_su_sc_mode_cntl.polymode_back_ptype !=
          xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
      description_out.fill_mode_wireframe = 0;
    }
  } else {
    // Filled front faces only, without culling.
    cull_front = false;
    cull_back = false;
  }
  if (!edram_rov_used) {
    float polygon_offset, polygon_offset_scale;
    draw_util::GetPreferredFacePolygonOffset(
        regs, primitive_polygonal, polygon_offset_scale, polygon_offset);
    description_out.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
    description_out.depth_bias_slope_scaled =
        polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
  }
  if (tessellated && cvars::d3d12_tessellation_wireframe) {
    description_out.fill_mode_wireframe = 1;
  }
  description_out.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  bool depth_stencil_bound_and_used = false;
  if (!edram_rov_used) {
    // Depth/stencil. No stencil, always passing depth test and no depth writing
    // means depth disabled.
    if (bound_depth_and_color_render_target_bits & 1) {
      if (normalized_depth_control.z_enable) {
        description_out.depth_func = normalized_depth_control.zfunc;
        description_out.depth_write = normalized_depth_control.z_write_enable;
      } else {
        description_out.depth_func = xenos::CompareFunction::kAlways;
      }
      if (normalized_depth_control.stencil_enable) {
        description_out.stencil_enable = 1;
        bool stencil_backface_enable =
            primitive_polygonal && normalized_depth_control.backface_enable;
        // Per-face masks not supported by Direct3D 12, choose the back face
        // ones only if drawing only back faces.
        Register stencil_ref_mask_reg;
        if (stencil_backface_enable && cull_front) {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
        }
        auto stencil_ref_mask =
            regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
        description_out.stencil_read_mask = stencil_ref_mask.stencilmask;
        description_out.stencil_write_mask = stencil_ref_mask.stencilwritemask;
        description_out.stencil_front_fail_op =
            normalized_depth_control.stencilfail;
        description_out.stencil_front_depth_fail_op =
            normalized_depth_control.stencilzfail;
        description_out.stencil_front_pass_op =
            normalized_depth_control.stencilzpass;
        description_out.stencil_front_func =
            normalized_depth_control.stencilfunc;
        if (stencil_backface_enable) {
          description_out.stencil_back_fail_op =
              normalized_depth_control.stencilfail_bf;
          description_out.stencil_back_depth_fail_op =
              normalized_depth_control.stencilzfail_bf;
          description_out.stencil_back_pass_op =
              normalized_depth_control.stencilzpass_bf;
          description_out.stencil_back_func =
              normalized_depth_control.stencilfunc_bf;
        } else {
          description_out.stencil_back_fail_op =
              description_out.stencil_front_fail_op;
          description_out.stencil_back_depth_fail_op =
              description_out.stencil_front_depth_fail_op;
          description_out.stencil_back_pass_op =
              description_out.stencil_front_pass_op;
          description_out.stencil_back_func =
              description_out.stencil_front_func;
        }
      }
      // If not binding the DSV, ignore the format in the hash.
      if (description_out.depth_func != xenos::CompareFunction::kAlways ||
          description_out.depth_write || description_out.stencil_enable) {
        description_out.depth_format = xenos::DepthRenderTargetFormat(
            bound_depth_and_color_render_target_formats[0]);
        depth_stencil_bound_and_used = true;
      }
    } else {
      description_out.depth_func = xenos::CompareFunction::kAlways;
    }

    // Render targets and blending state. 32 because of 0x1F mask, for safety
    // (all unknown to zero).
    static constexpr PipelineBlendFactor kBlendFactorMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcColor,
        /*  5 */ PipelineBlendFactor::kInvSrcColor,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestColor,
        /*  9 */ PipelineBlendFactor::kInvDestColor,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        // CONSTANT_COLOR
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // Like kBlendFactorMap, but with color modes changed to alpha. Some
    // pipelines aren't created in 545407E0 because a color mode is used for
    // alpha.
    static constexpr PipelineBlendFactor kBlendFactorAlphaMap[32] = {
        /*  0 */ PipelineBlendFactor::kZero,
        /*  1 */ PipelineBlendFactor::kOne,
        /*  2 */ PipelineBlendFactor::kZero,  // ?
        /*  3 */ PipelineBlendFactor::kZero,  // ?
        /*  4 */ PipelineBlendFactor::kSrcAlpha,
        /*  5 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  6 */ PipelineBlendFactor::kSrcAlpha,
        /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
        /*  8 */ PipelineBlendFactor::kDestAlpha,
        /*  9 */ PipelineBlendFactor::kInvDestAlpha,
        /* 10 */ PipelineBlendFactor::kDestAlpha,
        /* 11 */ PipelineBlendFactor::kInvDestAlpha,
        /* 12 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_COLOR
        /* 13 */ PipelineBlendFactor::kInvBlendFactor,
        // CONSTANT_ALPHA
        /* 14 */ PipelineBlendFactor::kBlendFactor,
        // ONE_MINUS_CONSTANT_ALPHA
        /* 15 */ PipelineBlendFactor::kInvBlendFactor,
        /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
    };
    // While it's okay to specify fewer render targets in the pipeline state
    // (even fewer than written by the shader) than actually bound to the
    // command list (though this kind of truncation may only happen at the end -
    // DXGI_FORMAT_UNKNOWN *requires* a null RTV descriptor to be bound), not
    // doing that because sample counts of all render targets bound via
    // OMSetRenderTargets, even those beyond NumRenderTargets, apparently must
    // have their sample count matching the one set in the pipeline - however if
    // we set NumRenderTargets to 0 and also disable depth / stencil, the sample
    // count must be set to 1 - while the command list may still have
    // multisampled render targets bound (happens in 4D5307E6 main menu).
    // TODO(Triang3l): Investigate interaction of OMSetRenderTargets with
    // non-null depth and DSVFormat DXGI_FORMAT_UNKNOWN in the same case.
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(bound_depth_and_color_render_target_bits &
            (uint32_t(1) << (1 + i)))) {
        continue;
      }
      PipelineRenderTarget& rt = description_out.render_targets[i];
      rt.used = 1;
      auto color_info = regs.Get<reg::RB_COLOR_INFO>(
          reg::RB_COLOR_INFO::rt_register_indices[i]);
      rt.format = xenos::ColorRenderTargetFormat(
          bound_depth_and_color_render_target_formats[1 + i]);
      rt.write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
      if (rt.write_mask) {
        auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
            reg::RB_BLENDCONTROL::rt_register_indices[i]);
        rt.src_blend = kBlendFactorMap[uint32_t(blendcontrol.color_srcblend)];
        rt.dest_blend = kBlendFactorMap[uint32_t(blendcontrol.color_destblend)];
        rt.blend_op = blendcontrol.color_comb_fcn;
        rt.src_blend_alpha =
            kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_srcblend)];
        rt.dest_blend_alpha =
            kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_destblend)];
        rt.blend_op_alpha = blendcontrol.alpha_comb_fcn;
      } else {
        rt.src_blend = PipelineBlendFactor::kOne;
        rt.dest_blend = PipelineBlendFactor::kZero;
        rt.blend_op = xenos::BlendOp::kAdd;
        rt.src_blend_alpha = PipelineBlendFactor::kOne;
        rt.dest_blend_alpha = PipelineBlendFactor::kZero;
        rt.blend_op_alpha = xenos::BlendOp::kAdd;
      }
    }
  }
  xenos::MsaaSamples host_msaa_samples =
      regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  if (edram_rov_used) {
    if (host_msaa_samples == xenos::MsaaSamples::k2X) {
      // 2 is not supported in ForcedSampleCount on Nvidia.
      host_msaa_samples = xenos::MsaaSamples::k4X;
    }
  } else {
    if (!(bound_depth_and_color_render_target_bits & ~uint32_t(1)) &&
        !depth_stencil_bound_and_used) {
      // Direct3D 12 requires the sample count to be 1 when no color or depth /
      // stencil render targets are bound.
      // FIXME(Triang3l): Use ForcedSampleCount or some other fallback for
      // sample counting when needed, though with 2x it will be as incorrect as
      // with 1x / 4x anyway; or bind a dummy depth / stencil buffer if really
      // needed.
      host_msaa_samples = xenos::MsaaSamples::k1X;
    }
    // TODO(Triang3l): 4x MSAA fallback when 2x isn't supported.
  }
  description_out.host_msaa_samples = host_msaa_samples;

  return true;
}

const std::vector<uint32_t>& PipelineCache::GetGeometryShader(
    GeometryShaderKey key) {
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second;
  }
  std::vector<uint32_t> shader;
  CreateDxbcGeometryShader(key, shader);
  return geometry_shaders_.emplace(key, std::move(shader)).first->second;
}

void PipelineCache::EnsurePipelineShadersTranslated(
    Pipeline* pipeline, DxbcShaderTranslator& translator,
    StringBuffer& ucode_disasm_buffer, IDxbcConverter* dxbc_converter,
    IDxcUtils* dxc_utils, IDxcCompiler* dxc_compiler, bool use_try_claim,
    bool handle_non_placeholder) {
  D3D12Shader::D3D12Translation* pending_vs = pipeline->pending_vertex_shader;
  D3D12Shader::D3D12Translation* pending_ps = pipeline->pending_pixel_shader;

  // Helper lambda to translate a shader, optionally using TryClaimTranslation.
  auto translate_shader = [&](D3D12Shader::D3D12Translation* translation,
                              const char* shader_type) {
    if (!translation->is_translated()) {
      bool should_translate = true;
      if (use_try_claim) {
        should_translate = translation->TryClaimTranslation();
        if (!should_translate) {
          // Another thread is translating - wait for it.
          while (!translation->is_translated()) {
            std::this_thread::yield();
          }
        }
      }
      if (should_translate) {
        DxbcShader& shader = static_cast<DxbcShader&>(translation->shader());
        if (!shader.is_ucode_analyzed()) {
          shader.AnalyzeUcode(ucode_disasm_buffer);
        }
        if (!TranslateAnalyzedShader(translator, *translation, dxbc_converter,
                                     dxc_utils, dxc_compiler)) {
          XELOGE("Failed to translate {} shader {:016X}", shader_type,
                 shader.ucode_data_hash());
        } else {
          if (storage_writer_.is_active() &&
              shader.try_set_ucode_storage_index(
                  storage_writer_.storage_index())) {
            shader_storage_file_flush_needed_ = true;
            storage_writer_.QueueShaderWrite(&shader);
          }
        }
      }
    }
  };

  // Translate pending VS if present.
  if (pending_vs != nullptr) {
    translate_shader(pending_vs, "vertex");
    pipeline->pending_vertex_shader = nullptr;
  }

  // Translate pending PS if present and update root signature.
  if (pending_ps != nullptr) {
    translate_shader(pending_ps, "pixel");
    // Update root signature now that PS is translated.
    if (pending_ps->is_valid()) {
      PipelineRuntimeDescription& desc = pipeline->description;
      bool tessellated = Shader::IsHostVertexShaderTypeDomain(
          DxbcShaderTranslator::Modification(desc.vertex_shader->modification())
              .vertex.host_vertex_shader_type);
      desc.root_signature = command_processor_.GetRootSignature(
          static_cast<const DxbcShader*>(&desc.vertex_shader->shader()),
          static_cast<const DxbcShader*>(&pending_ps->shader()), tessellated);
    }
    pipeline->pending_pixel_shader = nullptr;
  } else if (handle_non_placeholder && pending_vs == nullptr) {
    // Non-placeholder mode: translate desc.pixel_shader if needed (for
    // pipelines loaded from cache).
    D3D12Shader::D3D12Translation* ps = pipeline->description.pixel_shader;
    if (ps != nullptr) {
      translate_shader(ps, "pixel");
    }
  }
}

ID3D12PipelineState* PipelineCache::CreateD3D12Pipeline(
    const PipelineRuntimeDescription& runtime_description) {
  const PipelineDescription& description = runtime_description.description;

  if (runtime_description.pixel_shader != nullptr) {
    XELOGGPU("Creating graphics pipeline with VS {:016X}, PS {:016X}",
             runtime_description.vertex_shader->shader().ucode_data_hash(),
             runtime_description.pixel_shader->shader().ucode_data_hash());
  } else {
    XELOGGPU("Creating graphics pipeline with VS {:016X}",
             runtime_description.vertex_shader->shader().ucode_data_hash());
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC state_desc;
  std::memset(&state_desc, 0, sizeof(state_desc));

  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;

  // Root signature.
  state_desc.pRootSignature = runtime_description.root_signature;

  // Index buffer strip cut value.
  switch (description.strip_cut_index) {
    case PipelineStripCutIndex::kFFFF:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      break;
    case PipelineStripCutIndex::kFFFFFFFF:
      state_desc.IBStripCutValue =
          D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      break;
    default:
      state_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      break;
  }

  // Primitive topology, vertex, hull, domain and geometry shaders.
  if (!runtime_description.vertex_shader->is_translated()) {
    XELOGE("Vertex shader {:016X} not translated",
           runtime_description.vertex_shader->shader().ucode_data_hash());
    assert_always();
    return nullptr;
  }
  Shader::HostVertexShaderType host_vertex_shader_type =
      DxbcShaderTranslator::Modification(
          runtime_description.vertex_shader->modification())
          .vertex.host_vertex_shader_type;
  if (Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
    state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    xenos::TessellationMode tessellation_mode = xenos::TessellationMode(
        description.primitive_topology_type_or_tessellation_mode);
    if (tessellation_mode == xenos::TessellationMode::kAdaptive) {
      state_desc.VS.pShaderBytecode = shaders::tessellation_adaptive_vs;
      state_desc.VS.BytecodeLength = sizeof(shaders::tessellation_adaptive_vs);
    } else {
      state_desc.VS.pShaderBytecode = shaders::tessellation_indexed_vs;
      state_desc.VS.BytecodeLength = sizeof(shaders::tessellation_indexed_vs);
    }
    switch (tessellation_mode) {
      case xenos::TessellationMode::kDiscrete:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_triangle_3cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_triangle_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_quad_4cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::discrete_quad_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::discrete_quad_1cp_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kContinuous:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_triangle_3cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_triangle_3cp_hs);
            break;
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_triangle_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_triangle_1cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_quad_4cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_quad_4cp_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::continuous_quad_1cp_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::continuous_quad_1cp_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      case xenos::TessellationMode::kAdaptive:
        switch (host_vertex_shader_type) {
          case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::adaptive_triangle_hs;
            state_desc.HS.BytecodeLength =
                sizeof(shaders::adaptive_triangle_hs);
            break;
          case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
            state_desc.HS.pShaderBytecode = shaders::adaptive_quad_hs;
            state_desc.HS.BytecodeLength = sizeof(shaders::adaptive_quad_hs);
            break;
          default:
            assert_unhandled_case(host_vertex_shader_type);
            return nullptr;
        }
        break;
      default:
        assert_unhandled_case(tessellation_mode);
        return nullptr;
    }
    state_desc.DS.pShaderBytecode =
        runtime_description.vertex_shader->translated_binary().data();
    state_desc.DS.BytecodeLength =
        runtime_description.vertex_shader->translated_binary().size();
  } else {
    assert_true(host_vertex_shader_type ==
                Shader::HostVertexShaderType::kVertex);
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
      // Fallback vertex shaders are not needed on Direct3D 12.
      return nullptr;
    }
    state_desc.VS.pShaderBytecode =
        runtime_description.vertex_shader->translated_binary().data();
    state_desc.VS.BytecodeLength =
        runtime_description.vertex_shader->translated_binary().size();
    PipelinePrimitiveTopologyType primitive_topology_type =
        PipelinePrimitiveTopologyType(
            description.primitive_topology_type_or_tessellation_mode);
    switch (primitive_topology_type) {
      case PipelinePrimitiveTopologyType::kPoint:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        break;
      case PipelinePrimitiveTopologyType::kLine:
        state_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        break;
      case PipelinePrimitiveTopologyType::kTriangle:
        state_desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        break;
      default:
        assert_unhandled_case(primitive_topology_type);
        return nullptr;
    }
  }

  // Pixel shader.
  if (runtime_description.pixel_shader != nullptr) {
    if (!runtime_description.pixel_shader->is_translated()) {
      XELOGE("Pixel shader {:016X} not translated",
             runtime_description.pixel_shader->shader().ucode_data_hash());
      assert_always();
      return nullptr;
    }
    state_desc.PS.pShaderBytecode =
        runtime_description.pixel_shader->translated_binary().data();
    state_desc.PS.BytecodeLength =
        runtime_description.pixel_shader->translated_binary().size();
  } else if (edram_rov_used) {
    state_desc.PS.pShaderBytecode = depth_only_pixel_shader_.data();
    state_desc.PS.BytecodeLength = depth_only_pixel_shader_.size();
  } else {
    if (render_target_cache_.depth_float24_convert_in_pixel_shader() &&
        (description.depth_func != xenos::CompareFunction::kAlways ||
         description.depth_write) &&
        description.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      if (render_target_cache_.depth_float24_round()) {
        state_desc.PS.pShaderBytecode = shaders::float24_round_ps;
        state_desc.PS.BytecodeLength = sizeof(shaders::float24_round_ps);
      } else {
        state_desc.PS.pShaderBytecode = shaders::float24_truncate_ps;
        state_desc.PS.BytecodeLength = sizeof(shaders::float24_truncate_ps);
      }
    }
  }

  // Geometry shader.
  if (runtime_description.geometry_shader != nullptr) {
    state_desc.GS.pShaderBytecode = runtime_description.geometry_shader->data();
    state_desc.GS.BytecodeLength =
        sizeof(*runtime_description.geometry_shader->data()) *
        runtime_description.geometry_shader->size();
  }

  // Rasterizer state.
  state_desc.RasterizerState.FillMode = description.fill_mode_wireframe
                                            ? D3D12_FILL_MODE_WIREFRAME
                                            : D3D12_FILL_MODE_SOLID;
  switch (description.cull_mode) {
    case PipelineCullMode::kFront:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
      break;
    case PipelineCullMode::kBack:
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
      break;
    default:
      assert_true(description.cull_mode == PipelineCullMode::kNone ||
                  description.cull_mode ==
                      PipelineCullMode::kDisableRasterization);
      state_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      break;
  }
  state_desc.RasterizerState.FrontCounterClockwise =
      description.front_counter_clockwise ? true : false;
  state_desc.RasterizerState.DepthBias = description.depth_bias;
  state_desc.RasterizerState.DepthBiasClamp = 0.0f;
  // With non-square resolution scaling, make sure the worst-case impact is
  // reverted (slope only along the scaled axis), thus max. More bias is better
  // than less bias, because less bias means Z fighting with the background is
  // more likely.
  state_desc.RasterizerState.SlopeScaledDepthBias =
      description.depth_bias_slope_scaled *
      float(std::max(render_target_cache_.draw_resolution_scale_x(),
                     render_target_cache_.draw_resolution_scale_y()));
  state_desc.RasterizerState.DepthClipEnable =
      description.depth_clip ? true : false;
  uint32_t msaa_sample_count = uint32_t(1)
                               << uint32_t(description.host_msaa_samples);
  if (edram_rov_used) {
    // Only 1, 4, 8 and (not on all GPUs) 16 are allowed, using sample 0 as 0
    // and 3 as 1 for 2x instead (not exactly the same sample positions, but
    // still top-left and bottom-right - however, this can be adjusted with
    // programmable sample positions).
    assert_true(msaa_sample_count == 1 || msaa_sample_count == 4);
    if (msaa_sample_count != 1 && msaa_sample_count != 4) {
      return nullptr;
    }
    state_desc.RasterizerState.ForcedSampleCount =
        uint32_t(1) << uint32_t(description.host_msaa_samples);
  }

  // Sample mask and description.
  state_desc.SampleMask = UINT_MAX;
  // TODO(Triang3l): 4x MSAA fallback when 2x isn't supported without ROV.
  if (edram_rov_used) {
    state_desc.SampleDesc.Count = 1;
  } else {
    assert_true(msaa_sample_count <= 4);
    if (msaa_sample_count > 4) {
      return nullptr;
    }
    if (msaa_sample_count == 2 && !render_target_cache_.msaa_2x_supported()) {
      // Using sample 0 as 0 and 3 as 1 for 2x instead (not exactly the same
      // sample positions, but still top-left and bottom-right - however, this
      // can be adjusted with programmable sample positions).
      state_desc.SampleMask = 0b1001;
      state_desc.SampleDesc.Count = 4;
    } else {
      state_desc.SampleDesc.Count = msaa_sample_count;
    }
  }

  if (!edram_rov_used) {
    // Depth/stencil.
    if (description.depth_func != xenos::CompareFunction::kAlways ||
        description.depth_write) {
      state_desc.DepthStencilState.DepthEnable = true;
      state_desc.DepthStencilState.DepthWriteMask =
          description.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                  : D3D12_DEPTH_WRITE_MASK_ZERO;
      // Comparison functions are the same in Direct3D 12 but plus one (minus
      // one, bit 0 for less, bit 1 for equal, bit 2 for greater).
      state_desc.DepthStencilState.DepthFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.depth_func));
    }
    if (description.stencil_enable) {
      state_desc.DepthStencilState.StencilEnable = true;
      state_desc.DepthStencilState.StencilReadMask =
          description.stencil_read_mask;
      state_desc.DepthStencilState.StencilWriteMask =
          description.stencil_write_mask;
      // Stencil operations are the same in Direct3D 12 too but plus one.
      state_desc.DepthStencilState.FrontFace.StencilFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilDepthFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_depth_fail_op));
      state_desc.DepthStencilState.FrontFace.StencilPassOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_front_pass_op));
      state_desc.DepthStencilState.FrontFace.StencilFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.stencil_front_func));
      state_desc.DepthStencilState.BackFace.StencilFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_fail_op));
      state_desc.DepthStencilState.BackFace.StencilDepthFailOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_depth_fail_op));
      state_desc.DepthStencilState.BackFace.StencilPassOp =
          D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) +
                           uint32_t(description.stencil_back_pass_op));
      state_desc.DepthStencilState.BackFace.StencilFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) +
                                uint32_t(description.stencil_back_func));
    }
    if (state_desc.DepthStencilState.DepthEnable ||
        state_desc.DepthStencilState.StencilEnable) {
      state_desc.DSVFormat = D3D12RenderTargetCache::GetDepthDSVDXGIFormat(
          description.depth_format);
    }

    // Render targets and blending.
    state_desc.BlendState.IndependentBlendEnable = true;
    static constexpr D3D12_BLEND kBlendFactorMap[] = {
        D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,
        D3D12_BLEND_SRC_COLOR,     D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,     D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_COLOR,    D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
        D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
        D3D12_BLEND_SRC_ALPHA_SAT,
    };
    // 8 entries for safety since 3 bits from the guest are passed directly.
    static constexpr D3D12_BLEND_OP kBlendOpMap[] = {
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,     D3D12_BLEND_OP_MIN,
        D3D12_BLEND_OP_MAX, D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const PipelineRenderTarget& rt = description.render_targets[i];
      if (!rt.used) {
        // Null RTV descriptors can be used for slots with DXGI_FORMAT_UNKNOWN
        // in the pipeline state.
        state_desc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        continue;
      }
      state_desc.NumRenderTargets = i + 1;
      state_desc.RTVFormats[i] =
          render_target_cache_.GetColorDrawDXGIFormat(rt.format);
      if (state_desc.RTVFormats[i] == DXGI_FORMAT_UNKNOWN) {
        assert_always();
        return nullptr;
      }
      D3D12_RENDER_TARGET_BLEND_DESC& blend_desc =
          state_desc.BlendState.RenderTarget[i];
      if (rt.src_blend != PipelineBlendFactor::kOne ||
          rt.dest_blend != PipelineBlendFactor::kZero ||
          rt.blend_op != xenos::BlendOp::kAdd ||
          rt.src_blend_alpha != PipelineBlendFactor::kOne ||
          rt.dest_blend_alpha != PipelineBlendFactor::kZero ||
          rt.blend_op_alpha != xenos::BlendOp::kAdd) {
        blend_desc.BlendEnable = true;
        blend_desc.BlendOp = kBlendOpMap[uint32_t(rt.blend_op)];
        if (blend_desc.BlendOp == D3D12_BLEND_OP_MIN ||
            blend_desc.BlendOp == D3D12_BLEND_OP_MAX) {
          blend_desc.SrcBlend = D3D12_BLEND_ONE;
          blend_desc.DestBlend = D3D12_BLEND_ONE;
          blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;
          blend_desc.DestBlendAlpha = D3D12_BLEND_ONE;
        } else {
          blend_desc.SrcBlend = kBlendFactorMap[uint32_t(rt.src_blend)];
          blend_desc.DestBlend = kBlendFactorMap[uint32_t(rt.dest_blend)];
          blend_desc.SrcBlendAlpha =
              kBlendFactorMap[uint32_t(rt.src_blend_alpha)];
          blend_desc.DestBlendAlpha =
              kBlendFactorMap[uint32_t(rt.dest_blend_alpha)];
        }
        blend_desc.BlendOpAlpha = kBlendOpMap[uint32_t(rt.blend_op_alpha)];
      }
      blend_desc.RenderTargetWriteMask = rt.write_mask;
    }
  }

  // Disable rasterization if needed (parameter combinations that make no
  // difference when rasterization is disabled have already been handled in
  // GetCurrentStateDescription) the way it's disabled in Direct3D by design
  // (disabling a pixel shader and depth / stencil).
  // TODO(Triang3l): When it happens to be that a combination of parameters
  // (no host pixel shader and depth / stencil without ROV) would disable
  // rasterization when it's still needed (for occlusion query sample counting),
  // ensure rasterization happens (by binding an empty pixel shader, or maybe
  // via ForcedSampleCount when not using 2x MSAA - its requirements for
  // OMSetRenderTargets need some investigation though).
  if (description.cull_mode == PipelineCullMode::kDisableRasterization) {
    state_desc.PS.pShaderBytecode = nullptr;
    state_desc.PS.BytecodeLength = 0;
    state_desc.DepthStencilState.DepthEnable = false;
    state_desc.DepthStencilState.StencilEnable = false;
  }

  // Create the D3D12 pipeline state object.
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  ID3D12PipelineState* state;
  HRESULT hr =
      device->CreateGraphicsPipelineState(&state_desc, IID_PPV_ARGS(&state));
  if (FAILED(hr)) {
    if (runtime_description.pixel_shader != nullptr) {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X}, PS {:016X}: "
          "HRESULT 0x{:08X}",
          runtime_description.vertex_shader->shader().ucode_data_hash(),
          runtime_description.pixel_shader->shader().ucode_data_hash(), hr);
    } else {
      XELOGE(
          "Failed to create graphics pipeline with VS {:016X}: HRESULT "
          "0x{:08X}",
          runtime_description.vertex_shader->shader().ucode_data_hash(), hr);
    }
    // Log D3D12 debug messages for all pipeline failures.
    command_processor_.GetD3D12Provider().LogD3D12DebugMessages();
    return nullptr;
  }
  std::wstring name;
  if (runtime_description.pixel_shader != nullptr) {
    name = fmt::format(
        L"VS {:016X}, PS {:016X}",
        runtime_description.vertex_shader->shader().ucode_data_hash(),
        runtime_description.pixel_shader->shader().ucode_data_hash());
  } else {
    name = fmt::format(
        L"VS {:016X}",
        runtime_description.vertex_shader->shader().ucode_data_hash());
  }
  state->SetName(name.c_str());
  return state;
}

void PipelineCache::CreationThread(size_t thread_index) {
  // Create thread-local translator to avoid contention with main thread.
  // This mirrors what the shader storage loading threads do.
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  bool edram_rov_used = render_target_cache_.GetPath() ==
                        RenderTargetCache::Path::kPixelShaderInterlock;
  StringBuffer ucode_disasm_buffer;
  DxbcShaderTranslator translator(
      provider.GetAdapterVendorID(), bindless_resources_used_, edram_rov_used,
      !(edram_rov_used ||
        render_target_cache_.gamma_render_target_as_unorm16()),
      render_target_cache_.msaa_2x_supported(),
      render_target_cache_.draw_resolution_scale_x(),
      render_target_cache_.draw_resolution_scale_y(),
      provider.GetGraphicsAnalysis() != nullptr);
  // Create thread-local DXIL conversion objects if needed.
  IDxbcConverter* dxbc_converter = nullptr;
  IDxcUtils* dxc_utils = nullptr;
  IDxcCompiler* dxc_compiler = nullptr;
  if (cvars::d3d12_dxbc_disasm_dxilconv && dxbc_converter_ && dxc_utils_ &&
      dxc_compiler_) {
    provider.DxbcConverterCreateInstance(CLSID_DxbcConverter,
                                         IID_PPV_ARGS(&dxbc_converter));
    provider.DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxc_utils));
    provider.DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc_compiler));
  }

  while (true) {
    Pipeline* pipeline_to_create = nullptr;

    // Check if need to shut down or set the completion event and dequeue the
    // pipeline if there is any.
    {
      std::unique_lock<xe_mutex> lock(creation_request_lock_);
      if (thread_index >= creation_threads_shutdown_from_ ||
          creation_queue_.empty()) {
        if (creation_threads_busy_ == 0) {
          // Last pipeline in the queue created.
          if (creation_completion_set_event_) {
            // Signal the event if requested (blocking mode).
            creation_completion_set_event_ = false;
            creation_completion_event_->Set();
          }
          if (creation_completion_callback_) {
            // Invoke completion callback (non-blocking mode).
            auto callback = std::move(creation_completion_callback_);
            creation_completion_callback_ = nullptr;
            lock.unlock();
            callback();
            lock.lock();
          }
        }
        if (thread_index >= creation_threads_shutdown_from_) {
          // Cleanup thread-local resources.
          if (dxc_compiler) {
            dxc_compiler->Release();
          }
          if (dxc_utils) {
            dxc_utils->Release();
          }
          if (dxbc_converter) {
            dxbc_converter->Release();
          }
          return;
        }
        creation_request_cond_.wait(lock);
        continue;
      }
      // Take the pipeline from the queue and increment the busy thread count
      // until the pipeline is created - other threads must be able to dequeue
      // requests, but can't set the completion event until the pipelines are
      // fully created (rather than just started creating).
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
      ++creation_threads_busy_;
    }

    // Translate pending shaders and update root signature.
    EnsurePipelineShadersTranslated(pipeline_to_create, translator,
                                    ucode_disasm_buffer, dxbc_converter,
                                    dxc_utils, dxc_compiler,
                                    /*use_try_claim=*/true,
                                    /*handle_non_placeholder=*/true);

    // Create the D3D12 pipeline state object.
    ID3D12PipelineState* new_state =
        CreateD3D12Pipeline(pipeline_to_create->description);

    // Store the pipeline. If creation failed, state stays nullptr and draws
    // will be skipped.
    if (new_state != nullptr) {
      pipeline_to_create->state.store(new_state, std::memory_order_release);
    } else {
      XELOGE("Pipeline creation failed (VS {:016X}, PS {:016X})",
             pipeline_to_create->description.vertex_shader
                 ? pipeline_to_create->description.vertex_shader->shader()
                       .ucode_data_hash()
                 : 0,
             pipeline_to_create->description.pixel_shader
                 ? pipeline_to_create->description.pixel_shader->shader()
                       .ucode_data_hash()
                 : 0);
    }

    // Pipeline created - the thread is not busy anymore, safe to set the
    // completion event if needed (at the next iteration, or in some other
    // thread).
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      --creation_threads_busy_;
    }
  }
}

void PipelineCache::CreateQueuedPipelinesOnProcessorThread() {
  assert_false(creation_threads_.empty());
  while (true) {
    Pipeline* pipeline_to_create;
    {
      std::lock_guard<xe_mutex> lock(creation_request_lock_);
      if (creation_queue_.empty()) {
        break;
      }
      pipeline_to_create = creation_queue_.top();
      creation_queue_.pop();
    }

    // Translate pending shaders and update root signature.
    EnsurePipelineShadersTranslated(pipeline_to_create, *shader_translator_,
                                    ucode_disasm_buffer_, dxbc_converter_,
                                    dxc_utils_, dxc_compiler_,
                                    /*use_try_claim=*/true,
                                    /*handle_non_placeholder=*/true);

    ID3D12PipelineState* new_state =
        CreateD3D12Pipeline(pipeline_to_create->description);

    // Store the pipeline. If creation failed, state stays nullptr.
    if (new_state != nullptr) {
      pipeline_to_create->state.store(new_state, std::memory_order_release);
    } else {
      XELOGW("ProcessorThread: Pipeline creation failed");
    }
  }
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
