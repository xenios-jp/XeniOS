/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_pipeline_compiler.h"

#include <algorithm>
#include <cctype>

#include "xenia/base/logging.h"
#include "xenia/gpu/registers.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

MTL::ColorWriteMask ToMetalColorWriteMask(uint32_t write_mask) {
  MTL::ColorWriteMask mtl_mask = MTL::ColorWriteMaskNone;
  if (write_mask & 0x1) {
    mtl_mask |= MTL::ColorWriteMaskRed;
  }
  if (write_mask & 0x2) {
    mtl_mask |= MTL::ColorWriteMaskGreen;
  }
  if (write_mask & 0x4) {
    mtl_mask |= MTL::ColorWriteMaskBlue;
  }
  if (write_mask & 0x8) {
    mtl_mask |= MTL::ColorWriteMaskAlpha;
  }
  return mtl_mask;
}

MTL::BlendOperation ToMetalBlendOperation(xenos::BlendOp blend_op) {
  static const MTL::BlendOperation kBlendOpMap[8] = {
      MTL::BlendOperationAdd,              // 0
      MTL::BlendOperationSubtract,         // 1
      MTL::BlendOperationMin,              // 2
      MTL::BlendOperationMax,              // 3
      MTL::BlendOperationReverseSubtract,  // 4
      MTL::BlendOperationAdd,              // 5
      MTL::BlendOperationAdd,              // 6
      MTL::BlendOperationAdd,              // 7
  };
  return kBlendOpMap[uint32_t(blend_op) & 0x7];
}

MTL::BlendFactor ToMetalBlendFactorRgb(xenos::BlendFactor blend_factor) {
  static const MTL::BlendFactor kBlendFactorMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,
      /*  3 */ MTL::BlendFactorZero,
      /*  4 */ MTL::BlendFactorSourceColor,
      /*  5 */ MTL::BlendFactorOneMinusSourceColor,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationColor,
      /*  9 */ MTL::BlendFactorOneMinusDestinationColor,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendColor,
      /* 13 */ MTL::BlendFactorOneMinusBlendColor,
      /* 14 */ MTL::BlendFactorBlendAlpha,
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorMap[uint32_t(blend_factor) & 0x1F];
}

MTL::BlendFactor ToMetalBlendFactorAlpha(xenos::BlendFactor blend_factor) {
  static const MTL::BlendFactor kBlendFactorAlphaMap[32] = {
      /*  0 */ MTL::BlendFactorZero,
      /*  1 */ MTL::BlendFactorOne,
      /*  2 */ MTL::BlendFactorZero,
      /*  3 */ MTL::BlendFactorZero,
      /*  4 */ MTL::BlendFactorSourceAlpha,
      /*  5 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  6 */ MTL::BlendFactorSourceAlpha,
      /*  7 */ MTL::BlendFactorOneMinusSourceAlpha,
      /*  8 */ MTL::BlendFactorDestinationAlpha,
      /*  9 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 10 */ MTL::BlendFactorDestinationAlpha,
      /* 11 */ MTL::BlendFactorOneMinusDestinationAlpha,
      /* 12 */ MTL::BlendFactorBlendAlpha,
      /* 13 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 14 */ MTL::BlendFactorBlendAlpha,
      /* 15 */ MTL::BlendFactorOneMinusBlendAlpha,
      /* 16 */ MTL::BlendFactorSourceAlphaSaturated,
  };
  return kBlendFactorAlphaMap[uint32_t(blend_factor) & 0x1F];
}

void ApplyBlendState(
    MTL::RenderPipelineColorAttachmentDescriptorArray* color_attachments,
    uint32_t normalized_color_mask,
    const std::array<uint32_t, 4>& blendcontrol) {
  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = color_attachments->object(i);
    if (color_attachment->pixelFormat() == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    uint32_t rt_write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingEnabled(false);
      continue;
    }

    reg::RB_BLENDCONTROL bc = {};
    bc.value = blendcontrol[i];
    MTL::BlendFactor src_rgb = ToMetalBlendFactorRgb(bc.color_srcblend);
    MTL::BlendFactor dst_rgb = ToMetalBlendFactorRgb(bc.color_destblend);
    MTL::BlendOperation op_rgb = ToMetalBlendOperation(bc.color_comb_fcn);
    MTL::BlendFactor src_alpha = ToMetalBlendFactorAlpha(bc.alpha_srcblend);
    MTL::BlendFactor dst_alpha = ToMetalBlendFactorAlpha(bc.alpha_destblend);
    MTL::BlendOperation op_alpha = ToMetalBlendOperation(bc.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingEnabled(blending_enabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }
}

void ApplyBlendState(
    MTL4::RenderPipelineColorAttachmentDescriptorArray* color_attachments,
    uint32_t normalized_color_mask,
    const std::array<uint32_t, 4>& blendcontrol) {
  for (uint32_t i = 0; i < 4; ++i) {
    auto* color_attachment = color_attachments->object(i);
    if (color_attachment->pixelFormat() == MTL::PixelFormatInvalid) {
      color_attachment->setWriteMask(MTL::ColorWriteMaskNone);
      color_attachment->setBlendingState(MTL4::BlendStateDisabled);
      continue;
    }

    uint32_t rt_write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
    color_attachment->setWriteMask(ToMetalColorWriteMask(rt_write_mask));
    if (!rt_write_mask) {
      color_attachment->setBlendingState(MTL4::BlendStateDisabled);
      continue;
    }

    reg::RB_BLENDCONTROL bc = {};
    bc.value = blendcontrol[i];
    MTL::BlendFactor src_rgb = ToMetalBlendFactorRgb(bc.color_srcblend);
    MTL::BlendFactor dst_rgb = ToMetalBlendFactorRgb(bc.color_destblend);
    MTL::BlendOperation op_rgb = ToMetalBlendOperation(bc.color_comb_fcn);
    MTL::BlendFactor src_alpha = ToMetalBlendFactorAlpha(bc.alpha_srcblend);
    MTL::BlendFactor dst_alpha = ToMetalBlendFactorAlpha(bc.alpha_destblend);
    MTL::BlendOperation op_alpha = ToMetalBlendOperation(bc.alpha_comb_fcn);

    bool blending_enabled =
        src_rgb != MTL::BlendFactorOne || dst_rgb != MTL::BlendFactorZero ||
        op_rgb != MTL::BlendOperationAdd || src_alpha != MTL::BlendFactorOne ||
        dst_alpha != MTL::BlendFactorZero || op_alpha != MTL::BlendOperationAdd;
    color_attachment->setBlendingState(
        blending_enabled ? MTL4::BlendStateEnabled : MTL4::BlendStateDisabled);
    if (blending_enabled) {
      color_attachment->setSourceRGBBlendFactor(src_rgb);
      color_attachment->setDestinationRGBBlendFactor(dst_rgb);
      color_attachment->setRgbBlendOperation(op_rgb);
      color_attachment->setSourceAlphaBlendFactor(src_alpha);
      color_attachment->setDestinationAlphaBlendFactor(dst_alpha);
      color_attachment->setAlphaBlendOperation(op_alpha);
    }
  }
}

MTL4::LibraryFunctionDescriptor* NewLibraryFunctionDescriptor(
    MTL::Library* library, const char* function_name) {
  if (!library || !function_name || !function_name[0]) {
    return nullptr;
  }
  MTL4::LibraryFunctionDescriptor* descriptor =
      MTL4::LibraryFunctionDescriptor::alloc()->init();
  descriptor->setLibrary(library);
  NS::String* name = NS::String::string(function_name, NS::UTF8StringEncoding);
  descriptor->setName(name);
  return descriptor;
}

}  // namespace

MetalPipelineCompiler::RequestedMode MetalPipelineCompiler::ParseRequestedMode(
    const std::string& value, bool* valid) {
  std::string lower;
  lower.reserve(value.size());
  for (char ch : value) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (valid) {
    *valid = true;
  }
  if (lower == "metal3" || lower == "mtl3" || lower == "3") {
    return RequestedMode::kMetal3;
  }
  if (lower == "metal4" || lower == "mtl4" || lower == "4") {
    return RequestedMode::kMetal4;
  }
  if (lower == "auto") {
    return RequestedMode::kAuto;
  }
  if (valid) {
    *valid = false;
  }
  return RequestedMode::kMetal3;
}

MetalPipelineCompiler::MetalPipelineCompiler(MTL::Device* device,
                                             RequestedMode requested_mode)
    : device_(device), requested_mode_(requested_mode) {}

MetalPipelineCompiler::~MetalPipelineCompiler() {
  SerializeArchive();
  {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    ReleaseArchiveObjects();
  }
  ReleaseMetal4Compiler();
}

bool MetalPipelineCompiler::Initialize() {
  active_backend_ = ActiveBackend::kMetal3;
  ReleaseMetal4Compiler();

  if (requested_mode_ == RequestedMode::kMetal3) {
    XELOGI("Metal pipeline compiler: using Metal 3 compiler backend");
    return true;
  }

  std::string unavailable_reason;
  if (!IsMetal4Available(&unavailable_reason)) {
    if (requested_mode_ == RequestedMode::kMetal4) {
      XELOGE("Metal pipeline compiler: Metal 4 requested but unavailable: {}",
             unavailable_reason);
      return false;
    }
    XELOGI(
        "Metal pipeline compiler: auto selected Metal 3; Metal 4 unavailable: "
        "{}",
        unavailable_reason);
    return true;
  }

  active_backend_ = ActiveBackend::kMetal4;
  if (!CreateMetal4Compiler()) {
    active_backend_ = ActiveBackend::kMetal3;
    if (requested_mode_ == RequestedMode::kMetal4) {
      return false;
    }
    XELOGW(
        "Metal pipeline compiler: auto fell back to Metal 3 after Metal 4 "
        "compiler creation failed");
    return true;
  }

  XELOGI("Metal pipeline compiler: using Metal 4 compiler backend");
  return true;
}

void MetalPipelineCompiler::ShutdownArchive() {
  SerializeArchive();
  const bool recreate_metal4_compiler =
      using_metal4() && metal4_compiler_ && metal4_serializer_;
  {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    ReleaseArchiveObjects();
  }
  if (recreate_metal4_compiler) {
    CreateMetal4Compiler();
  }
}

const char* MetalPipelineCompiler::active_backend_name() const {
  return using_metal4() ? "metal4" : "metal3";
}

uint32_t MetalPipelineCompiler::direct_pipeline_cache_key() const {
  return uint32_t(using_metal4() ? CacheKey::kMetal4 : CacheKey::kMetal3);
}

uint32_t MetalPipelineCompiler::helper_bridge_pipeline_cache_key() const {
  return uint32_t(using_metal4() ? CacheKey::kMetal4HelperBridge
                                 : CacheKey::kMetal3);
}

uint32_t MetalPipelineCompiler::native_msl_library_cache_key() const {
  return direct_pipeline_cache_key();
}

bool MetalPipelineCompiler::InitializeArchive(
    const std::filesystem::path& archive_path) {
  return using_metal4() ? InitializeMetal4Archive(archive_path)
                        : InitializeMetal3Archive(archive_path);
}

void MetalPipelineCompiler::SerializeArchive() {
  std::lock_guard<std::mutex> lock(archive_mutex_);
  if (!archive_enabled_ || !archive_dirty_) {
    return;
  }

  NS::String* path_string = NS::String::string(archive_path_.string().c_str(),
                                               NS::UTF8StringEncoding);
  NS::URL* url = NS::URL::fileURLWithPath(path_string);
  NS::Error* error = nullptr;
  bool serialized = true;
  if (using_metal4()) {
    if (!metal4_serializer_) {
      return;
    }
    serialized =
        metal4_serializer_->serializeAsArchiveAndFlushToURL(url, &error);
  } else {
    if (!metal3_binary_archive_) {
      return;
    }
    serialized = metal3_binary_archive_->serializeToURL(url, &error);
  }

  if (!serialized && error) {
    XELOGW("Metal {} pipeline archive serialize failed: {}",
           active_backend_name(), error->localizedDescription()->utf8String());
  }
  archive_dirty_ = false;
}

bool MetalPipelineCompiler::ArchivePrewarmEnabled() const {
  return archive_enabled_;
}

MTL::Library* MetalPipelineCompiler::NewLibraryWithSource(
    const NS::String* source, const MTL::CompileOptions* options,
    NS::Error** error) {
  if (!using_metal4()) {
    return device_ ? device_->newLibrary(source, options, error) : nullptr;
  }
  if (!metal4_compiler_) {
    if (error) {
      *error = nullptr;
    }
    return nullptr;
  }
  MTL4::LibraryDescriptor* descriptor =
      MTL4::LibraryDescriptor::alloc()->init();
  descriptor->setSource(source);
  descriptor->setOptions(options);
  MTL::Library* library = metal4_compiler_->newLibrary(descriptor, error);
  descriptor->release();
  if (library) {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    archive_dirty_ |= archive_enabled_ && metal4_serializer_ != nullptr;
  }
  return library;
}

MTL::RenderPipelineState* MetalPipelineCompiler::NewRenderPipelineState(
    const MetalRenderPipelineCompileRequest& request, NS::Error** error) {
  return using_metal4() ? NewMetal4RenderPipelineState(request, error)
                        : NewMetal3RenderPipelineState(request, error);
}

MTL::RenderPipelineState* MetalPipelineCompiler::NewMeshRenderPipelineState(
    const MetalMeshPipelineCompileRequest& request, NS::Error** error) {
  return using_metal4() ? NewMetal4MeshRenderPipelineState(request, error)
                        : NewMetal3MeshRenderPipelineState(request, error);
}

bool MetalPipelineCompiler::IsMetal4Available(std::string* reason) const {
  if (!device_) {
    if (reason) {
      *reason = "no Metal device";
    }
    return false;
  }
  if (!device_->supportsFamily(MTL::GPUFamilyMetal4)) {
    if (reason) {
      *reason = "device does not report MTLGPUFamilyMetal4";
    }
    return false;
  }
  if (!MTL::Private::Class::s_kMTL4CompilerDescriptor ||
      !MTL::Private::Class::s_kMTL4LibraryDescriptor ||
      !MTL::Private::Class::s_kMTL4LibraryFunctionDescriptor ||
      !MTL::Private::Class::s_kMTL4RenderPipelineDescriptor ||
      !MTL::Private::Class::s_kMTL4MeshRenderPipelineDescriptor ||
      !MTL::Private::Class::s_kMTL4CompilerTaskOptions ||
      !MTL::Private::Class::s_kMTL4PipelineDataSetSerializerDescriptor) {
    if (reason) {
      *reason = "Metal 4 runtime classes are unavailable";
    }
    return false;
  }
  return true;
}

bool MetalPipelineCompiler::CreateMetal4Compiler() {
  ReleaseMetal4Compiler();
  if (!device_) {
    return false;
  }

  MTL4::CompilerDescriptor* descriptor =
      MTL4::CompilerDescriptor::alloc()->init();
  NS::String* label = NS::String::string("Xenia Metal 4 Pipeline Compiler",
                                         NS::UTF8StringEncoding);
  descriptor->setLabel(label);
  if (metal4_serializer_) {
    descriptor->setPipelineDataSetSerializer(metal4_serializer_);
  }
  NS::Error* error = nullptr;
  metal4_compiler_ = device_->newCompiler(descriptor, &error);
  descriptor->release();
  if (!metal4_compiler_) {
    XELOGE(
        "Metal pipeline compiler: failed to create MTL4Compiler: {}",
        error ? error->localizedDescription()->utf8String() : "unknown error");
    return false;
  }
  return true;
}

void MetalPipelineCompiler::ReleaseMetal4Compiler() {
  if (metal4_compiler_) {
    metal4_compiler_->release();
    metal4_compiler_ = nullptr;
  }
}

void MetalPipelineCompiler::ReleaseArchiveObjects() {
  if (metal4_serializer_) {
    ReleaseMetal4Compiler();
  }
  if (metal3_binary_archive_) {
    metal3_binary_archive_->release();
    metal3_binary_archive_ = nullptr;
  }
  if (metal4_archive_) {
    metal4_archive_->release();
    metal4_archive_ = nullptr;
  }
  if (metal4_serializer_) {
    metal4_serializer_->release();
    metal4_serializer_ = nullptr;
  }
  archive_enabled_ = false;
  archive_dirty_ = false;
  archive_path_.clear();
}

bool MetalPipelineCompiler::InitializeMetal3Archive(
    const std::filesystem::path& archive_path) {
  if (!device_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(archive_mutex_);
  ReleaseArchiveObjects();

  MTL::BinaryArchiveDescriptor* descriptor =
      MTL::BinaryArchiveDescriptor::alloc()->init();
  std::error_code ec;
  bool archive_exists = std::filesystem::exists(archive_path, ec);
  if (ec) {
    XELOGW("Metal binary archive existence check failed for {}: {}",
           archive_path.string(), ec.message());
    archive_exists = false;
  }
  if (archive_exists) {
    NS::String* path_string = NS::String::string(archive_path.string().c_str(),
                                                 NS::UTF8StringEncoding);
    NS::URL* url = NS::URL::fileURLWithPath(path_string);
    descriptor->setUrl(url);
  }

  NS::Error* error = nullptr;
  metal3_binary_archive_ = device_->newBinaryArchive(descriptor, &error);
  if (!metal3_binary_archive_ && archive_exists) {
    if (error) {
      XELOGW(
          "Metal binary archive load failed for existing file {}; retrying "
          "with a fresh archive: {}",
          archive_path.string(), error->localizedDescription()->utf8String());
    }
    descriptor->setUrl(nullptr);
    error = nullptr;
    metal3_binary_archive_ = device_->newBinaryArchive(descriptor, &error);
  }
  descriptor->release();
  if (!metal3_binary_archive_) {
    if (error) {
      XELOGW("Metal binary archive init failed: {}",
             error->localizedDescription()->utf8String());
    }
    return false;
  }
  archive_enabled_ = true;
  archive_dirty_ = false;
  archive_path_ = archive_path;
  return true;
}

bool MetalPipelineCompiler::InitializeMetal4Archive(
    const std::filesystem::path& archive_path) {
  if (!device_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(archive_mutex_);
  ReleaseArchiveObjects();

  std::error_code ec;
  bool archive_exists = std::filesystem::exists(archive_path, ec);
  if (ec) {
    XELOGW("Metal 4 archive existence check failed for {}: {}",
           archive_path.string(), ec.message());
    archive_exists = false;
  }
  if (archive_exists) {
    NS::String* path_string = NS::String::string(archive_path.string().c_str(),
                                                 NS::UTF8StringEncoding);
    NS::URL* url = NS::URL::fileURLWithPath(path_string);
    NS::Error* error = nullptr;
    metal4_archive_ = device_->newArchive(url, &error);
    if (!metal4_archive_ && error) {
      XELOGW("Metal 4 archive load failed for existing file {}: {}",
             archive_path.string(),
             error->localizedDescription()->utf8String());
    }
  }

  MTL4::PipelineDataSetSerializerDescriptor* serializer_descriptor =
      MTL4::PipelineDataSetSerializerDescriptor::alloc()->init();
  serializer_descriptor->setConfiguration(
      MTL4::PipelineDataSetSerializerConfigurationCaptureDescriptors |
      MTL4::PipelineDataSetSerializerConfigurationCaptureBinaries);
  metal4_serializer_ =
      device_->newPipelineDataSetSerializer(serializer_descriptor);
  serializer_descriptor->release();
  if (!metal4_serializer_) {
    XELOGW("Metal 4 pipeline data set serializer init failed");
  }

  archive_enabled_ = true;
  archive_dirty_ = false;
  archive_path_ = archive_path;
  bool compiler_ok = CreateMetal4Compiler();
  if (!compiler_ok) {
    ReleaseArchiveObjects();
  }
  return compiler_ok;
}

MTL4::CompilerTaskOptions*
MetalPipelineCompiler::NewMetal4CompilerTaskOptions() {
  std::lock_guard<std::mutex> lock(archive_mutex_);
  if (!archive_enabled_ || !metal4_archive_) {
    return nullptr;
  }
  MTL4::CompilerTaskOptions* options =
      MTL4::CompilerTaskOptions::alloc()->init();
  NS::Array* archives = NS::Array::array(metal4_archive_);
  options->setLookupArchives(archives);
  return options;
}

MTL::RenderPipelineState* MetalPipelineCompiler::NewMetal3RenderPipelineState(
    const MetalRenderPipelineCompileRequest& request, NS::Error** error) {
  MTL::RenderPipelineDescriptor* descriptor =
      MTL::RenderPipelineDescriptor::alloc()->init();
  descriptor->setVertexFunction(request.vertex_function);
  if (request.fragment_function) {
    descriptor->setFragmentFunction(request.fragment_function);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    descriptor->colorAttachments()->object(i)->setPixelFormat(
        request.color_formats[i]);
  }
  descriptor->setDepthAttachmentPixelFormat(request.depth_format);
  descriptor->setStencilAttachmentPixelFormat(request.stencil_format);
  descriptor->setSampleCount(request.sample_count);
  descriptor->setAlphaToCoverageEnabled(request.alpha_to_coverage);
  if (request.vertex_descriptor) {
    descriptor->setVertexDescriptor(request.vertex_descriptor);
  }
  ApplyBlendState(descriptor->colorAttachments(), request.normalized_color_mask,
                  request.blendcontrol);

  {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    if (metal3_binary_archive_) {
      NS::Array* archives = NS::Array::array(metal3_binary_archive_);
      descriptor->setBinaryArchives(archives);
      NS::Error* archive_error = nullptr;
      if (metal3_binary_archive_->addRenderPipelineFunctions(descriptor,
                                                             &archive_error)) {
        archive_dirty_ = true;
      }
    }
  }

  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(descriptor, error);
  descriptor->release();
  return pipeline;
}

MTL::RenderPipelineState* MetalPipelineCompiler::NewMetal4RenderPipelineState(
    const MetalRenderPipelineCompileRequest& request, NS::Error** error) {
  if (!metal4_compiler_) {
    if (error) {
      *error = nullptr;
    }
    return nullptr;
  }
  MTL4::RenderPipelineDescriptor* descriptor =
      MTL4::RenderPipelineDescriptor::alloc()->init();
  MTL4::LibraryFunctionDescriptor* vertex_descriptor =
      NewLibraryFunctionDescriptor(request.vertex_library,
                                   request.vertex_function_name);
  MTL4::LibraryFunctionDescriptor* fragment_descriptor =
      NewLibraryFunctionDescriptor(request.fragment_library,
                                   request.fragment_function_name);
  if (!vertex_descriptor) {
    descriptor->release();
    return nullptr;
  }
  descriptor->setVertexFunctionDescriptor(vertex_descriptor);
  if (fragment_descriptor) {
    descriptor->setFragmentFunctionDescriptor(fragment_descriptor);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    descriptor->colorAttachments()->object(i)->setPixelFormat(
        request.color_formats[i]);
  }
  // Metal 4 pipeline descriptors in the current SDK do not carry the Metal 3
  // depth/stencil attachment format fields. Xenia keeps those formats in its
  // own cache key, but the MTL4 compiler path only passes color/sample state.
  descriptor->setRasterSampleCount(request.sample_count);
  descriptor->setAlphaToCoverageState(request.alpha_to_coverage
                                          ? MTL4::AlphaToCoverageStateEnabled
                                          : MTL4::AlphaToCoverageStateDisabled);
  if (request.vertex_descriptor) {
    descriptor->setVertexDescriptor(request.vertex_descriptor);
  }
  ApplyBlendState(descriptor->colorAttachments(), request.normalized_color_mask,
                  request.blendcontrol);

  MTL4::CompilerTaskOptions* task_options = NewMetal4CompilerTaskOptions();
  MTL::RenderPipelineState* pipeline =
      metal4_compiler_->newRenderPipelineState(descriptor, task_options, error);
  if (task_options) {
    task_options->release();
  }
  if (pipeline) {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    archive_dirty_ |= archive_enabled_ && metal4_serializer_ != nullptr;
  }
  if (fragment_descriptor) {
    fragment_descriptor->release();
  }
  if (vertex_descriptor) {
    vertex_descriptor->release();
  }
  descriptor->release();
  return pipeline;
}

MTL::RenderPipelineState*
MetalPipelineCompiler::NewMetal3MeshRenderPipelineState(
    const MetalMeshPipelineCompileRequest& request, NS::Error** error) {
  MTL::MeshRenderPipelineDescriptor* descriptor =
      MTL::MeshRenderPipelineDescriptor::alloc()->init();
  if (request.object_function) {
    descriptor->setObjectFunction(request.object_function);
  }
  descriptor->setMeshFunction(request.mesh_function);
  if (request.fragment_function) {
    descriptor->setFragmentFunction(request.fragment_function);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    descriptor->colorAttachments()->object(i)->setPixelFormat(
        request.color_formats[i]);
  }
  descriptor->setDepthAttachmentPixelFormat(request.depth_format);
  descriptor->setStencilAttachmentPixelFormat(request.stencil_format);
  descriptor->setRasterSampleCount(request.sample_count);
  descriptor->setAlphaToCoverageEnabled(request.alpha_to_coverage);
  if (request.max_total_threadgroups_per_mesh_grid) {
    descriptor->setMaxTotalThreadgroupsPerMeshGrid(
        request.max_total_threadgroups_per_mesh_grid);
  }
  if (request.max_total_threads_per_object_threadgroup) {
    descriptor->setMaxTotalThreadsPerObjectThreadgroup(
        request.max_total_threads_per_object_threadgroup);
  }
  if (request.max_total_threads_per_mesh_threadgroup) {
    descriptor->setMaxTotalThreadsPerMeshThreadgroup(
        request.max_total_threads_per_mesh_threadgroup);
  }
  if (request.required_threads_per_object_threadgroup.width ||
      request.required_threads_per_object_threadgroup.height ||
      request.required_threads_per_object_threadgroup.depth) {
    descriptor->setRequiredThreadsPerObjectThreadgroup(
        request.required_threads_per_object_threadgroup);
  }
  if (request.required_threads_per_mesh_threadgroup.width ||
      request.required_threads_per_mesh_threadgroup.height ||
      request.required_threads_per_mesh_threadgroup.depth) {
    descriptor->setRequiredThreadsPerMeshThreadgroup(
        request.required_threads_per_mesh_threadgroup);
  }
  if (request.payload_memory_length) {
    descriptor->setPayloadMemoryLength(request.payload_memory_length);
  }
  ApplyBlendState(descriptor->colorAttachments(), request.normalized_color_mask,
                  request.blendcontrol);

  {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    if (metal3_binary_archive_) {
      NS::Array* archives = NS::Array::array(metal3_binary_archive_);
      descriptor->setBinaryArchives(archives);
      NS::Error* archive_error = nullptr;
      if (metal3_binary_archive_->addMeshRenderPipelineFunctions(
              descriptor, &archive_error)) {
        archive_dirty_ = true;
      }
    }
  }

  MTL::RenderPipelineState* pipeline = device_->newRenderPipelineState(
      descriptor, MTL::PipelineOptionNone, nullptr, error);
  descriptor->release();
  return pipeline;
}

MTL::RenderPipelineState*
MetalPipelineCompiler::NewMetal4MeshRenderPipelineState(
    const MetalMeshPipelineCompileRequest& request, NS::Error** error) {
  if (!metal4_compiler_) {
    if (error) {
      *error = nullptr;
    }
    return nullptr;
  }
  MTL4::MeshRenderPipelineDescriptor* descriptor =
      MTL4::MeshRenderPipelineDescriptor::alloc()->init();
  MTL4::LibraryFunctionDescriptor* object_descriptor =
      NewLibraryFunctionDescriptor(request.object_library,
                                   request.object_function_name);
  MTL4::LibraryFunctionDescriptor* mesh_descriptor =
      NewLibraryFunctionDescriptor(request.mesh_library,
                                   request.mesh_function_name);
  MTL4::LibraryFunctionDescriptor* fragment_descriptor =
      NewLibraryFunctionDescriptor(request.fragment_library,
                                   request.fragment_function_name);
  if (!mesh_descriptor) {
    if (object_descriptor) {
      object_descriptor->release();
    }
    descriptor->release();
    return nullptr;
  }
  if (object_descriptor) {
    descriptor->setObjectFunctionDescriptor(object_descriptor);
  }
  descriptor->setMeshFunctionDescriptor(mesh_descriptor);
  if (fragment_descriptor) {
    descriptor->setFragmentFunctionDescriptor(fragment_descriptor);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    descriptor->colorAttachments()->object(i)->setPixelFormat(
        request.color_formats[i]);
  }
  descriptor->setRasterSampleCount(request.sample_count);
  descriptor->setAlphaToCoverageState(request.alpha_to_coverage
                                          ? MTL4::AlphaToCoverageStateEnabled
                                          : MTL4::AlphaToCoverageStateDisabled);
  if (request.max_total_threadgroups_per_mesh_grid) {
    descriptor->setMaxTotalThreadgroupsPerMeshGrid(
        request.max_total_threadgroups_per_mesh_grid);
  }
  if (request.max_total_threads_per_object_threadgroup) {
    descriptor->setMaxTotalThreadsPerObjectThreadgroup(
        request.max_total_threads_per_object_threadgroup);
  }
  if (request.max_total_threads_per_mesh_threadgroup) {
    descriptor->setMaxTotalThreadsPerMeshThreadgroup(
        request.max_total_threads_per_mesh_threadgroup);
  }
  if (request.required_threads_per_object_threadgroup.width ||
      request.required_threads_per_object_threadgroup.height ||
      request.required_threads_per_object_threadgroup.depth) {
    descriptor->setRequiredThreadsPerObjectThreadgroup(
        request.required_threads_per_object_threadgroup);
  }
  if (request.required_threads_per_mesh_threadgroup.width ||
      request.required_threads_per_mesh_threadgroup.height ||
      request.required_threads_per_mesh_threadgroup.depth) {
    descriptor->setRequiredThreadsPerMeshThreadgroup(
        request.required_threads_per_mesh_threadgroup);
  }
  if (request.payload_memory_length) {
    descriptor->setPayloadMemoryLength(request.payload_memory_length);
  }
  ApplyBlendState(descriptor->colorAttachments(), request.normalized_color_mask,
                  request.blendcontrol);

  MTL4::CompilerTaskOptions* task_options = NewMetal4CompilerTaskOptions();
  MTL::RenderPipelineState* pipeline =
      metal4_compiler_->newRenderPipelineState(descriptor, task_options, error);
  if (task_options) {
    task_options->release();
  }
  if (pipeline) {
    std::lock_guard<std::mutex> lock(archive_mutex_);
    archive_dirty_ |= archive_enabled_ && metal4_serializer_ != nullptr;
  }
  if (fragment_descriptor) {
    fragment_descriptor->release();
  }
  if (mesh_descriptor) {
    mesh_descriptor->release();
  }
  if (object_descriptor) {
    object_descriptor->release();
  }
  descriptor->release();
  return pipeline;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
