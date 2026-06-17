/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_PIPELINE_COMPILER_H_
#define XENIA_GPU_METAL_METAL_PIPELINE_COMPILER_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

#include "xenia/ui/metal/metal_api.h"

namespace xe {
namespace gpu {
namespace metal {

struct MetalRenderPipelineCompileRequest {
  MTL::Library* vertex_library = nullptr;
  const char* vertex_function_name = nullptr;
  MTL::Function* vertex_function = nullptr;
  MTL::Library* fragment_library = nullptr;
  const char* fragment_function_name = nullptr;
  MTL::Function* fragment_function = nullptr;
  MTL::VertexDescriptor* vertex_descriptor = nullptr;
  std::array<MTL::PixelFormat, 4> color_formats = {};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  uint32_t sample_count = 1;
  uint32_t normalized_color_mask = 0;
  std::array<uint32_t, 4> blendcontrol = {};
  bool alpha_to_coverage = false;
};

struct MetalMeshPipelineCompileRequest {
  MTL::Library* object_library = nullptr;
  const char* object_function_name = nullptr;
  MTL::Function* object_function = nullptr;
  MTL::Library* mesh_library = nullptr;
  const char* mesh_function_name = nullptr;
  MTL::Function* mesh_function = nullptr;
  MTL::Library* fragment_library = nullptr;
  const char* fragment_function_name = nullptr;
  MTL::Function* fragment_function = nullptr;
  std::array<MTL::PixelFormat, 4> color_formats = {};
  MTL::PixelFormat depth_format = MTL::PixelFormatInvalid;
  MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
  uint32_t sample_count = 1;
  uint32_t normalized_color_mask = 0;
  std::array<uint32_t, 4> blendcontrol = {};
  bool alpha_to_coverage = false;
  uint32_t max_total_threadgroups_per_mesh_grid = 0;
  uint32_t max_total_threads_per_object_threadgroup = 0;
  uint32_t max_total_threads_per_mesh_threadgroup = 0;
  MTL::Size required_threads_per_object_threadgroup = MTL::Size::Make(0, 0, 0);
  MTL::Size required_threads_per_mesh_threadgroup = MTL::Size::Make(0, 0, 0);
  uint32_t payload_memory_length = 0;
};

class MetalPipelineCompiler {
 public:
  enum class RequestedMode : uint32_t {
    kMetal3,
    kMetal4,
    kAuto,
  };

  enum class ActiveBackend : uint32_t {
    kMetal3,
    kMetal4,
  };

  enum class CacheKey : uint32_t {
    kMetal3 = 0,
    kMetal4 = 1,
    kMetal4HelperBridge = 2,
  };

  static RequestedMode ParseRequestedMode(const std::string& value,
                                          bool* valid);

  MetalPipelineCompiler(MTL::Device* device, RequestedMode requested_mode);
  ~MetalPipelineCompiler();

  MetalPipelineCompiler(const MetalPipelineCompiler&) = delete;
  MetalPipelineCompiler& operator=(const MetalPipelineCompiler&) = delete;

  bool Initialize();
  void ShutdownArchive();

  ActiveBackend active_backend() const { return active_backend_; }
  RequestedMode requested_mode() const { return requested_mode_; }
  bool using_metal4() const {
    return active_backend_ == ActiveBackend::kMetal4;
  }
  const char* active_backend_name() const;
  uint32_t direct_pipeline_cache_key() const;
  uint32_t helper_bridge_pipeline_cache_key() const;
  uint32_t native_msl_library_cache_key() const;

  bool InitializeArchive(const std::filesystem::path& archive_path);
  void SerializeArchive();
  bool ArchivePrewarmEnabled() const;

  MTL::Library* NewLibraryWithSource(const NS::String* source,
                                     const MTL::CompileOptions* options,
                                     NS::Error** error);
  MTL::RenderPipelineState* NewRenderPipelineState(
      const MetalRenderPipelineCompileRequest& request, NS::Error** error);
  MTL::RenderPipelineState* NewMeshRenderPipelineState(
      const MetalMeshPipelineCompileRequest& request, NS::Error** error);

 private:
  bool IsMetal4Available(std::string* reason) const;
  bool CreateMetal4Compiler();
  void ReleaseMetal4Compiler();
  void ReleaseArchiveObjects();
  bool InitializeMetal3Archive(const std::filesystem::path& archive_path);
  bool InitializeMetal4Archive(const std::filesystem::path& archive_path);
  MTL4::CompilerTaskOptions* NewMetal4CompilerTaskOptions();
  MTL::RenderPipelineState* NewMetal3RenderPipelineState(
      const MetalRenderPipelineCompileRequest& request, NS::Error** error);
  MTL::RenderPipelineState* NewMetal4RenderPipelineState(
      const MetalRenderPipelineCompileRequest& request, NS::Error** error);
  MTL::RenderPipelineState* NewMetal3MeshRenderPipelineState(
      const MetalMeshPipelineCompileRequest& request, NS::Error** error);
  MTL::RenderPipelineState* NewMetal4MeshRenderPipelineState(
      const MetalMeshPipelineCompileRequest& request, NS::Error** error);

  MTL::Device* device_ = nullptr;
  RequestedMode requested_mode_ = RequestedMode::kMetal3;
  ActiveBackend active_backend_ = ActiveBackend::kMetal3;

  MTL4::Compiler* metal4_compiler_ = nullptr;
  MTL4::Archive* metal4_archive_ = nullptr;
  MTL4::PipelineDataSetSerializer* metal4_serializer_ = nullptr;
  MTL::BinaryArchive* metal3_binary_archive_ = nullptr;
  std::filesystem::path archive_path_;
  std::mutex archive_mutex_;
  bool archive_enabled_ = false;
  bool archive_dirty_ = false;
};

}  // namespace metal
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_METAL_METAL_PIPELINE_COMPILER_H_
