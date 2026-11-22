/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_transfer_encoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_transfer_spirv_cross.h"
#include "xenia/gpu/metal/metal_util.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/metal/host_depth_store_4xmsaa_cs.h"
#include "xenia/gpu/strip_merge.h"
#include "xenia/gpu/xenos.h"

DEFINE_bool(metal_transfer_fast_divmod, true,
            "Use fast exact div/mod in Metal transfer shaders", "Metal");
DEFINE_bool(metal_transfer_sample_mask, true,
            "Draw a multisampled transfer destination one sample per draw, "
            "with the sample index in the push constants and the shader "
            "writing the sample mask, unless the source is multisampled too",
            "Metal");
DECLARE_bool(metal_defer_resolve_clears);

namespace xe {
namespace gpu {
namespace metal {

namespace {

// Draws covering a multisampled transfer destination one sample at a time need
// one push constant set and one argument buffer per sample of the widest
// destination, each holding the layout's dwords plus the sample index those
// draws append.
constexpr uint32_t kTransferSampleDrawMax = 4;
constexpr uint32_t kTransferPushConstantDwordMax =
    kEdramTransferUsedPushConstantDwordCount + 1;

}  // namespace

MetalTransferEncoder::MetalTransferEncoder(
    MetalRenderTargetCache& render_target_cache,
    MetalCommandProcessor& command_processor)
    : render_target_cache_(render_target_cache),
      command_processor_(command_processor) {}

MetalTransferEncoder::~MetalTransferEncoder() { Shutdown(); }

bool MetalTransferEncoder::Initialize(MTL::Device* device) {
  device_ = device;
  if (!device_) {
    return false;
  }

  native_stencil_output_probed_ = ProbeNativeStencilOutputSupport();
  if (!native_stencil_output_probed_) {
    XELOGW(
        "Metal: no pixel shader stencil output on this device; stencil "
        "transfers will use eight masked draws per rectangle");
  }

  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    host_depth_store_pipelines_[i] = nullptr;
  }

  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k1X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_1xmsaa_cs_metallib,
          sizeof(host_depth_store_1xmsaa_cs_metallib),
          "host_depth_store_1xmsaa");
  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k2X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_2xmsaa_cs_metallib,
          sizeof(host_depth_store_2xmsaa_cs_metallib),
          "host_depth_store_2xmsaa");
  host_depth_store_pipelines_[size_t(xenos::MsaaSamples::k4X)] =
      CreateComputePipelineFromEmbeddedLibrary(
          device_, host_depth_store_4xmsaa_cs_metallib,
          sizeof(host_depth_store_4xmsaa_cs_metallib),
          "host_depth_store_4xmsaa");

  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    if (!host_depth_store_pipelines_[i]) {
      XELOGE("Metal: failed to initialize host depth store pipelines");
      return false;
    }
  }

  return true;
}

void MetalTransferEncoder::Shutdown() {
  for (auto& dump_pipeline_pair : dump_pipelines_) {
    if (dump_pipeline_pair.second) {
      dump_pipeline_pair.second->release();
    }
  }
  dump_pipelines_.clear();
  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    if (host_depth_store_pipelines_[i]) {
      host_depth_store_pipelines_[i]->release();
      host_depth_store_pipelines_[i] = nullptr;
    }
  }
  for (auto& it : transfer_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_pipelines_.clear();
  for (auto& it : transfer_fragment_functions_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_fragment_functions_.clear();
  for (auto& it : transfer_clear_pipelines_) {
    if (it.second) {
      it.second->release();
    }
  }
  transfer_clear_pipelines_.clear();
  if (transfer_rect_vertex_function_) {
    transfer_rect_vertex_function_->release();
    transfer_rect_vertex_function_ = nullptr;
  }
  if (transfer_library_) {
    transfer_library_->release();
    transfer_library_ = nullptr;
  }
  if (transfer_depth_state_) {
    transfer_depth_state_->release();
    transfer_depth_state_ = nullptr;
  }
  if (transfer_depth_stencil_output_state_) {
    transfer_depth_stencil_output_state_->release();
    transfer_depth_stencil_output_state_ = nullptr;
  }
  if (transfer_depth_state_none_) {
    transfer_depth_state_none_->release();
    transfer_depth_state_none_ = nullptr;
  }
  if (transfer_depth_clear_state_) {
    transfer_depth_clear_state_->release();
    transfer_depth_clear_state_ = nullptr;
  }
  if (transfer_stencil_clear_state_) {
    transfer_stencil_clear_state_->release();
    transfer_stencil_clear_state_ = nullptr;
  }
  for (auto& state : transfer_stencil_bit_states_) {
    if (state) {
      state->release();
      state = nullptr;
    }
  }
  if (transfer_dummy_buffer_) {
    transfer_dummy_buffer_->release();
    transfer_dummy_buffer_ = nullptr;
  }
  transfer_invocations_.clear();
  native_stencil_output_probed_ = false;
  device_ = nullptr;
}

float MetalTransferEncoder::GetResolveClearDepth(
    xenos::DepthRenderTargetFormat format, uint64_t clear_value) {
  const uint32_t guest_depth = (uint32_t(clear_value) >> 8) & 0xFFFFFF;
  return format == xenos::DepthRenderTargetFormat::kD24S8
             ? xenos::UNorm24To32(guest_depth)
             : xenos::Float20e4To32(guest_depth) * 0.5f;
}

void MetalTransferEncoder::GetResolveClearColor8888(uint64_t clear_value,
                                                    float color_out[4]) {
  for (uint32_t j = 0; j < 4; ++j) {
    color_out[j] = ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
  }
}

bool MetalTransferEncoder::BuildTransferRectanglePlans(
    RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
    const Transfer::Rectangle* cutout, bool require_all_rectangles,
    std::vector<TransferRectanglePlan>& transfer_rectangles_out) const {
  transfer_rectangles_out.clear();
  transfer_rectangles_out.reserve(transfers.size());
  for (uint32_t transfer_index = 0; transfer_index < transfers.size();
       ++transfer_index) {
    const Transfer& transfer = transfers[transfer_index];
    TransferRectanglePlan plan;
    plan.transfer_index = transfer_index;
    plan.rectangle_count = render_target_cache_.GetTransferRectangles(
        transfer, dest_key, dest_key.Is64bpp(), plan.rectangles.data(), cutout);
    if (!plan.rectangle_count) {
      if (require_all_rectangles) {
        transfer_rectangles_out.clear();
        return false;
      }
      continue;
    }
    transfer_rectangles_out.push_back(plan);
  }
  return true;
}

// Color formats whose ownership transfer is a uint bit copy that the guest's
// float view of the destination still writes exactly when each component is
// bit-reinterpreted, so the copy can be drawn inside the guest's pass. Measured
// on an M-series GPU, 1x and 4x, every half pattern and every float32 exponent
// with NaN payloads and denormals: float32 bitcasts keep every pattern, and so
// do halves written as half, but not halves written as float, which quiets
// NaNs.
bool MetalTransferEncoder::IsColorTransferFloatBitsExact(
    xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

bool MetalTransferEncoder::GetActiveTransferAttachmentFormats(
    MTL::RenderPassDescriptor* pass_descriptor,
    TransferAttachmentFormats& attachment_formats_out) const {
  attachment_formats_out = TransferAttachmentFormats();
  if (!pass_descriptor) {
    return false;
  }

  if (auto* color_attachments = pass_descriptor->colorAttachments()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      auto* color_attachment = color_attachments->object(i);
      MTL::Texture* texture =
          color_attachment ? color_attachment->texture() : nullptr;
      if (texture) {
        attachment_formats_out.color_attachment_formats[i] =
            texture->pixelFormat();
      }
    }
  }
  if (auto* depth_attachment = pass_descriptor->depthAttachment()) {
    if (MTL::Texture* texture = depth_attachment->texture()) {
      attachment_formats_out.depth_attachment_format = texture->pixelFormat();
    }
  }
  if (auto* stencil_attachment = pass_descriptor->stencilAttachment()) {
    if (MTL::Texture* texture = stencil_attachment->texture()) {
      attachment_formats_out.stencil_attachment_format = texture->pixelFormat();
    }
  }
  return true;
}

bool MetalTransferEncoder::GetCurrentTransferAttachmentFormats(
    TransferAttachmentFormats& attachment_formats_out) const {
  attachment_formats_out = TransferAttachmentFormats();

  bool has_color_attachment = false;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MTL::Texture* texture =
        render_target_cache_.current_color_targets_[i]
            ? render_target_cache_.current_color_targets_[i]->texture()
            : nullptr;
    if (!texture) {
      continue;
    }
    attachment_formats_out.color_attachment_formats[i] = texture->pixelFormat();
    has_color_attachment = true;
  }
  MTL::Texture* depth_texture =
      render_target_cache_.current_depth_target_
          ? render_target_cache_.current_depth_target_->texture()
          : nullptr;
  // GetRenderPassDescriptor attaches a dummy 8_8_8_8 target at slot 0 when the
  // guest binds neither a color nor a depth target.
  if (!has_color_attachment && !depth_texture) {
    attachment_formats_out.color_attachment_formats[0] =
        render_target_cache_.GetColorResourcePixelFormat(
            xenos::ColorRenderTargetFormat::k_8_8_8_8);
  }
  if (depth_texture) {
    MTL::PixelFormat depth_pixel_format = depth_texture->pixelFormat();
    attachment_formats_out.depth_attachment_format = depth_pixel_format;
    attachment_formats_out.stencil_attachment_format =
        MetalRenderTargetCache::GetStencilAttachmentPixelFormat(
            depth_pixel_format);
  }
  return true;
}

MTL::ComputePipelineState* MetalTransferEncoder::GetOrCreateDumpPipeline(
    EdramDumpShaderKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  EdramDumpShaderOptions shader_options;
  // The emitter declares the EDRAM buffer with the pre-1.3 BufferBlock and
  // Uniform forms, so it has to be emitted as SPIR-V 1.0.
  shader_options.spirv_version = 0x00010000;
  shader_options.descriptor_set_dest = 0;
  shader_options.descriptor_set_source = 1;
  shader_options.resolution_scale_x =
      render_target_cache_.draw_resolution_scale_x();
  shader_options.resolution_scale_y =
      render_target_cache_.draw_resolution_scale_y();
  shader_options.msaa_2x_attachments_supported =
      render_target_cache_.msaa_2x_supported_;
  shader_options.resolve_single_store_runs = true;
  if (!key.is_depth) {
    render_target_cache_.GetColorOwnershipTransferPixelFormat(
        key.GetColorFormat(), &shader_options.source_is_uint);
  }
  shader_options.depth_float24_round = ::cvars::depth_float24_round;
  shader_options.depth_float24_convert_in_pixel_shader =
      ::cvars::depth_float24_convert_in_pixel_shader;

  MTL::ComputePipelineState* pipeline = nullptr;
  std::vector<uint32_t> spirv = BuildEdramDumpShaderSpirv(key, shader_options);
  if (spirv.empty()) {
    XELOGE("MetalRenderTargetCache: failed to emit the dump shader 0x{:08X}",
           key.key);
  } else {
    std::string error;
    MTL::Function* function =
        CompileEdramDumpFunctionMsl(device_, spirv, &error);
    if (function) {
      NS::Error* pipeline_error = nullptr;
      pipeline = device_->newComputePipelineState(function, &pipeline_error);
      function->release();
    } else {
      XELOGE("MetalRenderTargetCache: dump shader 0x{:08X} compile failed: {}",
             key.key, error);
    }
  }
  if (!pipeline) {
    XELOGE(
        "MetalRenderTargetCache: no dump pipeline for key=0x{:08X} "
        "(is_depth={}, format={}, msaa={})",
        key.key, uint32_t(key.is_depth), uint32_t(key.resource_format),
        uint32_t(key.msaa_samples));
  }
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

void MetalTransferEncoder::DumpRenderTargets(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, MTL::CommandBuffer* command_buffer) {
  SCOPE_profile_cpu_f("gpu");
  // The dump reads the backings.
  render_target_cache_.PerformDeferredClears(command_buffer);
  std::vector<MetalRenderTargetCache::ResolveCopyDumpRectangle> rectangles;
  render_target_cache_.GetResolveCopyRectanglesToDump(
      dump_base, dump_row_length_used, dump_rows, dump_pitch, rectangles);
  if (render_target_cache_.strip_merge_resolve_bottom() && dump_pitch &&
      uint64_t(dump_rows - 1) * dump_pitch + dump_row_length_used >
          xenos::kEdramTileCount) {
    // A resolve of every strip of a merged session, past the EDRAM size: the
    // EDRAM buffer can hold only the first strip's rows. The later strips
    // run normally after the failure and resolve their own rows.
    for (MetalRenderTargetCache::ResolveCopyDumpRectangle& rectangle :
         rectangles) {
      RenderTargetKey rectangle_key = rectangle.render_target->key();
      if (!rectangle_key.strip_merged) {
        continue;
      }
      uint32_t rt_row_first = ((dump_base - rectangle_key.base_tiles) &
                               (xenos::kEdramTileCount - 1)) /
                                  dump_pitch +
                              rectangle.row_first;
      uint32_t band_tile_rows =
          render_target_cache_.strip_merge_bands_.tile_rows;
      uint32_t rows_in_band =
          rt_row_first < band_tile_rows ? band_tile_rows - rt_row_first : 0;
      if (rectangle.rows > rows_in_band) {
        rectangle.rows = rows_in_band;
        rectangle.row_last_end = dump_row_length_used;
      }
    }
    rectangles.erase(
        std::remove_if(
            rectangles.begin(), rectangles.end(),
            [](const MetalRenderTargetCache::ResolveCopyDumpRectangle&
                   rectangle) { return !rectangle.rows; }),
        rectangles.end());
    render_target_cache_.strip_merge()->Fail(
        "a resolve of every strip not done directly");
  }
  // Merge adjacent rectangles of the same source to dispatch fewer times.
  // Restrict the proof to one physical EDRAM period, so combining dispatches
  // cannot turn ordered writes to the same physical tile into concurrent ones.
  if (!render_target_cache_.IsDrawResolutionScaled() &&
      dump_base < xenos::kEdramTileCount && dump_rows && dump_pitch &&
      dump_row_length_used <= dump_pitch &&
      uint64_t(dump_rows - 1) * dump_pitch + dump_row_length_used <=
          xenos::kEdramTileCount &&
      rectangles.size() > 1) {
    // The rectangles come from the planner in address order, each a valid
    // non-empty span within the checked period. Equality of the render target
    // also fixes the texture view, format, source pitch and MSAA. The dispatch
    // base stays unwrapped: source addressing needs the 12th tile bit. A merge
    // is accepted only if it strictly reduces the dispatches, not the tiles or
    // bytes written.
    auto merge_adjacent =
        [&](MetalRenderTargetCache::ResolveCopyDumpRectangle& left,
            const MetalRenderTargetCache::ResolveCopyDumpRectangle& right) {
          if (left.render_target != right.render_target) {
            return false;
          }
          const uint32_t left_last_row = left.row_first + left.rows - 1;
          const bool touches =
              left_last_row * dump_pitch + left.row_last_end ==
              right.row_first * dump_pitch + right.row_first_start;
          const bool only_pitch_padding =
              left.row_last_end == dump_row_length_used &&
              right.row_first_start == 0 &&
              right.row_first == left_last_row + 1;
          if (!touches && !only_pitch_padding) {
            return false;
          }
          MetalRenderTargetCache::ResolveCopyDumpRectangle merged = left;
          merged.rows = right.row_first + right.rows - left.row_first;
          merged.row_last_end = right.row_last_end;
          MetalRenderTargetCache::ResolveCopyDumpRectangle::Dispatch
              scratch[MetalRenderTargetCache::ResolveCopyDumpRectangle::
                          kMaxDispatches];
          if (merged.GetDispatches(dump_pitch, dump_row_length_used, scratch) >=
              left.GetDispatches(dump_pitch, dump_row_length_used, scratch) +
                  right.GetDispatches(dump_pitch, dump_row_length_used,
                                      scratch)) {
            return false;
          }
          left = merged;
          return true;
        };
    size_t output_count = 0;
    for (size_t input_index = 0; input_index < rectangles.size();
         ++input_index) {
      if (output_count && merge_adjacent(rectangles[output_count - 1],
                                         rectangles[input_index])) {
        continue;
      }
      if (output_count != input_index) {
        rectangles[output_count] = rectangles[input_index];
      }
      ++output_count;
    }
    rectangles.erase(rectangles.begin() + output_count, rectangles.end());
  }
  if (rectangles.empty()) {
    XELOGW(
        "MetalTransferEncoder::DumpRenderTargets: no rectangles for base={} "
        "row_length_used={} rows={} pitch={}",
        dump_base, dump_row_length_used, dump_rows, dump_pitch);
    return;
  }

  if (!render_target_cache_.edram_buffer_) {
    XELOGW(
        "MetalTransferEncoder::DumpRenderTargets: EDRAM buffer not "
        "initialized, skipping GPU dump");
    return;
  }

  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!queue) {
    XELOGE("MetalTransferEncoder::DumpRenderTargets: no command queue");
    return;
  }

  ScopedAutoreleasePool autorelease_pool;
  bool owns_command_buffer = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.CreateAccountedCommandBuffer(
        MetalCommandProcessor::CommandBufferKind::kRenderTargetDump);
    if (!cmd) {
      XELOGE("MetalTransferEncoder::DumpRenderTargets: no command buffer");
      return;
    }
    owns_command_buffer = true;
  }

  command_processor_.EndSharedComputeEncoder(cmd);
  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    XELOGE("MetalTransferEncoder::DumpRenderTargets: no compute encoder");
    if (owns_command_buffer) {
      command_processor_.DiscardAccountedCommandBuffer(
          cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetDump);
    }
    return;
  }

  const uint32_t scale_x = render_target_cache_.draw_resolution_scale_x();
  const uint32_t scale_y = render_target_cache_.draw_resolution_scale_y();

  EdramDumpShaderPitches pitches;
  pitches.dest_pitch = dump_pitch;

  for (const MetalRenderTargetCache::ResolveCopyDumpRectangle& rect :
       rectangles) {
    auto* rt = static_cast<MetalRenderTarget*>(rect.render_target);
    if (!rt) {
      continue;
    }
    RenderTargetKey rt_key = rt->key();

    EdramDumpShaderKey shader_key;
    shader_key.is_depth = rt_key.is_depth;
    shader_key.resource_format = rt_key.resource_format;
    shader_key.msaa_samples = rt_key.msaa_samples;
    MTL::ComputePipelineState* dump_pipeline =
        GetOrCreateDumpPipeline(shader_key);
    if (!dump_pipeline) {
      XELOGE(
          "MetalTransferEncoder::DumpRenderTargets: no dump pipeline for {}, "
          "its EDRAM tiles stay stale",
          rt_key.GetDebugName());
      continue;
    }

    // The source is the ownership transfer view, whose format the shader was
    // emitted against - an integer one for the formats where exact bits matter.
    MTL::Texture* source_texture = nullptr;
    MTL::Texture* stencil_texture = nullptr;
    if (rt_key.is_depth) {
      source_texture = rt->texture();
      stencil_texture = render_target_cache_.GetStencilTextureView(rt);
      if (!stencil_texture) {
        XELOGE(
            "MetalTransferEncoder::DumpRenderTargets: no stencil view for a "
            "depth render target");
        continue;
      }
    } else {
      bool source_is_uint = false;
      render_target_cache_.GetColorOwnershipTransferPixelFormat(
          rt_key.GetColorFormat(), &source_is_uint);
      source_texture = source_is_uint ? rt->transfer_texture() : rt->texture();
    }
    if (!source_texture) {
      XELOGE(
          "MetalTransferEncoder::DumpRenderTargets: no source texture for "
          "{}, its EDRAM tiles stay stale",
          rt_key.GetDebugName());
      continue;
    }

    encoder->setComputePipelineState(dump_pipeline);
    encoder->setTexture(source_texture, 0);
    if (stencil_texture) {
      encoder->setTexture(stencil_texture, 2);
    }
    encoder->setBuffer(render_target_cache_.edram_buffer_, 0, 1);

    pitches.source_pitch = rt_key.GetPitchTiles();

    MetalRenderTargetCache::ResolveCopyDumpRectangle::Dispatch dispatches
        [MetalRenderTargetCache::ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rect.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const MetalRenderTargetCache::ResolveCopyDumpRectangle::Dispatch&
          dispatch = dispatches[i];
      EdramDumpShaderOffsets offsets;
      offsets.dispatch_first_tile =
          rt_key.GetNonWrappedTileIndex(dump_base + dispatch.offset);
      offsets.source_base_tiles = rt_key.base_tiles;

      uint32_t push_constants[kEdramDumpShaderPushConstantCount] = {};
      push_constants[kEdramDumpShaderPushConstantPitches] = pitches.pitches;
      push_constants[kEdramDumpShaderPushConstantOffsets] = offsets.offsets;
      encoder->setBytes(push_constants, sizeof(push_constants), 0);

      encoder->dispatchThreadgroups(
          MTL::Size::Make((scale_x *
                               (xenos::kEdramTileWidthSamples >>
                                uint32_t(rt_key.Is64bpp())) *
                               dispatch.width_tiles +
                           (kEdramDumpShaderSamplesPerGroupX - 1)) /
                              kEdramDumpShaderSamplesPerGroupX,
                          (scale_y * xenos::kEdramTileHeightSamples *
                               dispatch.height_tiles +
                           (kEdramDumpShaderSamplesPerGroupY - 1)) /
                              kEdramDumpShaderSamplesPerGroupY,
                          1),
          MTL::Size::Make(kEdramDumpShaderSamplesPerGroupX,
                          kEdramDumpShaderSamplesPerGroupY, 1));
    }
  }

  encoder->endEncoding();
  if (owns_command_buffer) {
    command_processor_.CommitAndAwait(
        cmd, MetalCommandProcessor::GpuWaitReason::kDump);
  }
  // cmd is autoreleased from commandBuffer() - do not release
}

bool MetalTransferEncoder::MakeTransferAddressConstant(
    RenderTargetKey source_key, RenderTargetKey dest_key,
    const Transfer& transfer,
    EdramTransferAddressConstant& constant_out) const {
  constexpr uint32_t kTileMask = xenos::kEdramTileCount - 1;
  constant_out = EdramTransferAddressConstant();
  constant_out.dest_pitch = dest_key.GetPitchTiles();
  constant_out.source_pitch = source_key.GetPitchTiles();
  if (source_key.strip_merged) {
    // Strip merge, a source with rows past the EDRAM: the source tile index is
    // the destination one plus the offset, not wrapped, biased by
    // kEdramTileCount, or, far (the shader key bits), not biased.
    int32_t offset =
        GetStripMergeTransferOffset(source_key, dest_key, transfer);
    int32_t biased =
        offset +
        (IsStripMergeTransferFar(offset) ? 0 : int32_t(xenos::kEdramTileCount));
    if (biased < 0 || biased >= int32_t(2 * xenos::kEdramTileCount)) {
      XELOGE("Strip merge: transfer offset {} out of range", offset);
      return false;
    }
    constant_out.source_to_dest = uint32_t(biased) & kTileMask;
    constant_out.source_next_period =
        uint32_t(biased) >> xenos::kEdramBaseTilesBits;
    return true;
  }
  uint32_t band_offset_tiles = 0;
  if (dest_key.strip_merged) {
    // A source with the EDRAM's rows fills one band of a strip-merged
    // destination per transfer, the destination's tile index being past the
    // band's start.
    if (transfer.band_count != 1) {
      XELOGE("Strip merge: a multi-band transfer from {}",
             source_key.GetDebugName());
      return false;
    }
    band_offset_tiles = transfer.band_first *
                        render_target_cache_.GetStripMergeBandTiles(dest_key);
  }
  constant_out.source_to_dest =
      (uint32_t(dest_key.base_tiles) - uint32_t(source_key.base_tiles) -
       band_offset_tiles) &
      kTileMask;
  constant_out.source_next_period = source_key.next_period;
  return true;
}

int32_t MetalTransferEncoder::GetStripMergeTransferOffset(
    RenderTargetKey source_key, RenderTargetKey dest_key,
    const Transfer& transfer) const {
  // The offset between the range's tile in each, plus the band shift. The
  // destination's tile index already includes the band it writes.
  return int32_t(source_key.GetNonWrappedTileIndex(transfer.start_tiles) -
                 source_key.base_tiles) -
         int32_t(dest_key.GetNonWrappedTileIndex(transfer.start_tiles) -
                 dest_key.base_tiles) +
         int32_t(transfer.source_band_shift) *
             int32_t(render_target_cache_.GetStripMergeBandTiles(source_key));
}

bool MetalTransferEncoder::IsStripMergeAddressingSame(
    const Transfer& transfer, const Transfer& other_transfer,
    RenderTargetKey dest_key) const {
  // Only strip-merged sources have an address depending on the range.
  auto same = [&](const RenderTarget* source) {
    if (!source || !source->key().strip_merged) {
      return true;
    }
    EdramTransferAddressConstant constant, other_constant;
    return MakeTransferAddressConstant(source->key(), dest_key, transfer,
                                       constant) &&
           MakeTransferAddressConstant(source->key(), dest_key, other_transfer,
                                       other_constant) &&
           constant == other_constant;
  };
  return same(transfer.source) && same(transfer.host_depth_source);
}

EdramTransferShaderKey MetalTransferEncoder::GetTransferShaderKey(
    RenderTargetKey source_key, RenderTargetKey dest_key,
    const RenderTargetKey* host_depth_source_key,
    bool host_depth_source_is_copy, bool stencil_bit,
    uint32_t dest_color_rt_index, const Transfer& transfer) const {
  EdramTransferShaderKey shader_key;
  shader_key.source_msaa_samples = source_key.msaa_samples;
  shader_key.dest_msaa_samples = dest_key.msaa_samples;
  shader_key.source_resource_format = source_key.resource_format;
  shader_key.dest_resource_format = dest_key.resource_format;
  shader_key.host_depth_source_msaa_samples = xenos::MsaaSamples::k1X;
  shader_key.dest_color_rt_index = dest_color_rt_index;
  shader_key.dest_scale_native = dest_key.scale_native;
  shader_key.source_scale_native = source_key.scale_native;
  assert_true(!host_depth_source_key ||
              host_depth_source_key->scale_native == dest_key.scale_native);

  if (stencil_bit) {
    shader_key.mode = source_key.is_depth
                          ? EdramTransferMode::kDepthToStencilBit
                          : EdramTransferMode::kColorToStencilBit;
  } else if (dest_key.is_depth) {
    if (host_depth_source_key) {
      // Reading the host depth back out of the EDRAM buffer is a mode of its
      // own rather than a flag, so the shader declares a buffer instead of a
      // texture for it.
      if (host_depth_source_is_copy) {
        shader_key.mode =
            source_key.is_depth
                ? EdramTransferMode::kDepthAndHostDepthCopyToDepth
                : EdramTransferMode::kColorAndHostDepthCopyToDepth;
      } else {
        shader_key.mode = source_key.is_depth
                              ? EdramTransferMode::kDepthAndHostDepthToDepth
                              : EdramTransferMode::kColorAndHostDepthToDepth;
        shader_key.host_depth_source_msaa_samples =
            host_depth_source_key->msaa_samples;
      }
    } else {
      shader_key.mode = source_key.is_depth ? EdramTransferMode::kDepthToDepth
                                            : EdramTransferMode::kColorToDepth;
    }
  } else {
    shader_key.mode = source_key.is_depth ? EdramTransferMode::kDepthToColor
                                          : EdramTransferMode::kColorToColor;
  }

  shader_key.value_convert =
      render_target_cache_.IsTransferValueConverted7e3And8888(source_key,
                                                              dest_key)
          ? 1
          : 0;

  // The identity layouts address the source at the destination's own sample
  // coordinates, without the shader's relative tile wrapping. That needs the
  // same EDRAM base and pitch, and the transfer within one EDRAM period from
  // the base. GetRangeRectangles splits physical wrapping into separate
  // ranges, so a transfer range never crosses the end of EDRAM. Targets in
  // the next period use the generic addressing path.
  const uint32_t relative_start = (transfer.start_tiles - dest_key.base_tiles) &
                                  (xenos::kEdramTileCount - 1);
  // Strip-merged render targets have rows past the EDRAM, and band copies
  // move rows (see MakeTransferAddressConstant).
  const bool same_layout_no_wrap =
      !render_target_cache_.IsDrawResolutionScaled() &&
      !source_key.next_period && !dest_key.next_period &&
      !source_key.strip_merged && !dest_key.strip_merged &&
      source_key.base_tiles == dest_key.base_tiles &&
      source_key.GetPitchTiles() == dest_key.GetPitchTiles() &&
      relative_start + transfer.end_tiles - transfer.start_tiles <=
          xenos::kEdramTileCount;
  shader_key.depth_identity_layout =
      same_layout_no_wrap &&
      shader_key.mode == EdramTransferMode::kDepthToDepth &&
      source_key.resource_format == dest_key.resource_format &&
      ((source_key.msaa_samples == xenos::MsaaSamples::k1X &&
        dest_key.msaa_samples == xenos::MsaaSamples::k4X) ||
       (source_key.msaa_samples == xenos::MsaaSamples::k4X &&
        dest_key.msaa_samples == xenos::MsaaSamples::k1X));
  shader_key.color_identity_layout =
      same_layout_no_wrap &&
      shader_key.mode == EdramTransferMode::kColorToColor &&
      !source_key.Is64bpp() && !dest_key.Is64bpp() &&
      source_key.msaa_samples == dest_key.msaa_samples;
  shader_key.source_unwrapped = source_key.strip_merged;
  shader_key.host_depth_source_unwrapped = host_depth_source_key &&
                                           !host_depth_source_is_copy &&
                                           host_depth_source_key->strip_merged;
  shader_key.source_unwrapped_far =
      shader_key.source_unwrapped &&
      IsStripMergeTransferFar(
          GetStripMergeTransferOffset(source_key, dest_key, transfer));
  shader_key.host_depth_source_unwrapped_far =
      shader_key.host_depth_source_unwrapped &&
      IsStripMergeTransferFar(GetStripMergeTransferOffset(
          *host_depth_source_key, dest_key, transfer));
  // The identity layouts don't divide by the source pitch.
  shader_key.source_pitch_16 = !shader_key.depth_identity_layout &&
                               !shader_key.color_identity_layout &&
                               source_key.GetPitchTiles() == 16;
  return shader_key;
}

bool MetalTransferEncoder::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const Transfer::Rectangle* const* transfer_cutouts,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    MTL::CommandBuffer* command_buffer,
    MTL::RenderCommandEncoder* active_render_encoder,
    MTL::RenderPassDescriptor* active_render_pass_descriptor,
    bool depth_keep_dest_depth, MetalRenderTarget* pre_rotated_depth_target,
    ExecutionOutput* output) {
  DrawPassTransferEncoderMutationMask* mutations_out =
      output ? &output->mutations : nullptr;
  SCOPE_profile_cpu_f("gpu");
  if (mutations_out) {
    *mutations_out =
        MetalRenderTargetCache::kDrawPassTransferEncoderMutationNone;
  }
  if (!render_targets || !render_target_transfers) {
    return false;
  }

  bool resolve_clear_needed =
      render_target_resolve_clear_values && resolve_clear_rectangle;
  assert_false(resolve_clear_needed && transfer_cutouts);
  // What the transfers into each render target leave out.
  auto get_transfer_cutout =
      [&](uint32_t render_target_index) -> const Transfer::Rectangle* {
    return transfer_cutouts ? transfer_cutouts[render_target_index]
                            : resolve_clear_rectangle;
  };
  // Resolve clears build a pass of their own around a clear load action, which
  // the guest's already-started pass cannot provide.
  bool use_active_render_encoder = active_render_encoder != nullptr;
  if (use_active_render_encoder &&
      (resolve_clear_needed || !active_render_pass_descriptor)) {
    return false;
  }
  auto mark_encoder_mutation =
      [&](DrawPassTransferEncoderMutationMask mutations) {
        if (use_active_render_encoder && mutations_out) {
          *mutations_out |= mutations;
        }
      };
  TransferAttachmentFormats active_attachment_formats;
  if (use_active_render_encoder &&
      !GetActiveTransferAttachmentFormats(active_render_pass_descriptor,
                                          active_attachment_formats)) {
    return false;
  }
  bool any_work = false;
  bool host_depth_store_needed = false;
  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    if (resolve_clear_needed) {
      any_work = true;
    }
    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty()) {
      continue;
    }
    any_work = true;
    if (!dest_rt->key().is_depth) {
      continue;
    }
    // Rotated when queued: the previous backing is the host depth.
    if (dest_rt == pre_rotated_depth_target) {
      continue;
    }
    for (const Transfer& transfer : transfers) {
      if (transfer.host_depth_source == dest_rt) {
        host_depth_store_needed = true;
        break;
      }
    }
  }
  if (!any_work) {
    return true;
  }
  // The host depth store is a compute dispatch, which cannot be encoded into a
  // render pass.
  if (use_active_render_encoder && host_depth_store_needed) {
    return false;
  }

  // Encoding into the guest's pass touches no encoder of its own, so it needs
  // no command buffer either.
  MTL::CommandBuffer* cmd = nullptr;
  if (!use_active_render_encoder) {
    cmd = command_buffer ? command_buffer
                         : command_processor_.EnsureCommandBuffer();
    if (!cmd) {
      XELOGE(
          "MetalTransferEncoder::PerformTransfersAndResolveClears: no "
          "command buffer");
      return false;
    }
    command_processor_.EndRenderEncoder();
    // The passes below read and partially write the backings.
    render_target_cache_.PerformDeferredClears(cmd);
  }

  // Past every fallback to another path, so each transfer is logged once.
  render_target_cache_.LogTransfers(
      render_target_count, render_targets, render_target_transfers,
      resolve_clear_needed ? resolve_clear_rectangle : nullptr);

  if (resolve_clear_needed && !use_active_render_encoder) {
    const MetalRenderTargetCache::PairedClearResult result =
        render_target_cache_.TryPerformPairedResolveClear(
            render_target_count, render_targets, render_target_transfers,
            render_target_resolve_clear_values, *resolve_clear_rectangle, cmd);
    if (result != MetalRenderTargetCache::PairedClearResult::kNotApplicable) {
      return result == MetalRenderTargetCache::PairedClearResult::kEncoded;
    }
  }

  uint32_t scale_x = render_target_cache_.draw_resolution_scale_x();
  uint32_t scale_y = render_target_cache_.draw_resolution_scale_y();
  uint32_t tile_width_samples = xenos::kEdramTileWidthSamples *
                                render_target_cache_.draw_resolution_scale_x();
  uint32_t tile_height_samples = xenos::kEdramTileHeightSamples *
                                 render_target_cache_.draw_resolution_scale_y();
  uint32_t depth_round = (!::cvars::depth_float24_convert_in_pixel_shader &&
                          ::cvars::depth_float24_round)
                             ? 1u
                             : 0u;

  // Self-sourced host depth: rather than snapshotting the depth into a buffer
  // with a compute dispatch, keep the current backing as the read-only host
  // depth source and render the transfer into a spare backing that becomes
  // the active one. The generic transfer pass below then targets the new
  // backing with a don't-care load.
  MetalRenderTarget* renamed_depth_dest = pre_rotated_depth_target;
  if (host_depth_store_needed && !use_active_render_encoder) {
    auto* dest = static_cast<MetalRenderTarget*>(render_targets[0]);
    if (render_target_cache_.CanRotateHostDepth(
            render_target_count, render_targets, render_target_transfers,
            resolve_clear_needed) &&
        render_target_cache_.AcquireSpareDepthBacking(
            dest, render_target_transfers[0])) {
      dest->SwapDepthBackings();
      render_target_cache_.MarkContentChanged(dest);
      renamed_depth_dest = dest;
      host_depth_store_needed = false;
      if (output) {
        output->render_pass_descriptor_dirty = true;
      }
    }
  }

  // Host depth store pass (dest depth where host depth source == dest).
  bool host_depth_store_dispatched = false;
  if (host_depth_store_needed) {
    for (uint32_t i = 0; i < render_target_count; ++i) {
      RenderTarget* dest_rt = render_targets[i];
      if (!dest_rt) {
        continue;
      }
      RenderTargetKey dest_key = dest_rt->key();
      if (!dest_key.is_depth) {
        continue;
      }
      // The EDRAM buffer has no room for the bands of a strip-merged render
      // target past the first, the transfers drop the host depth instead.
      if (dest_key.strip_merged) {
        render_target_cache_.strip_merge()->Fail(
            "a strip-merged depth buffer is its own host depth source");
        continue;
      }
      const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
      for (const Transfer& transfer : depth_transfers) {
        if (transfer.host_depth_source != dest_rt) {
          continue;
        }
        auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
        MTL::Texture* depth_texture = dest_metal_rt->texture();
        if (!depth_texture || !render_target_cache_.edram_buffer_) {
          XELOGE(
              "MetalRenderTargetCache: host depth store skipped for {} (no "
              "texture or EDRAM buffer), the transfer reads stale host depth",
              dest_key.GetDebugName());
          continue;
        }
        size_t pipeline_index = size_t(dest_key.msaa_samples);
        if (pipeline_index >= xe::countof(host_depth_store_pipelines_) ||
            !host_depth_store_pipelines_[pipeline_index]) {
          XELOGE(
              "MetalTransferEncoder::PerformTransfersAndResolveClears: "
              "missing host depth store pipeline for msaa={}",
              uint32_t(dest_key.msaa_samples));
          continue;
        }
        Transfer::Rectangle
            rectangles[MetalRenderTargetCache::kMaxBandedRectanglesWithCutout];
        uint32_t rectangle_count = render_target_cache_.GetTransferRectangles(
            transfer, dest_key, dest_key.Is64bpp(), rectangles,
            get_transfer_cutout(i));
        if (!rectangle_count) {
          continue;
        }
        MetalRenderTargetCache::HostDepthStoreRenderTargetConstant
            render_target_constant =
                render_target_cache_.GetHostDepthStoreRenderTargetConstant(
                    dest_key.pitch_tiles_at_32bpp,
                    render_target_cache_.msaa_2x_supported_);
        command_processor_.EndSharedComputeEncoder(cmd);
        MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
        if (!encoder) {
          XELOGE(
              "MetalTransferEncoder::PerformTransfersAndResolveClears: "
              "failed to create host depth store encoder");
          continue;
        }
        encoder->setComputePipelineState(
            host_depth_store_pipelines_[pipeline_index]);
        encoder->setBuffer(render_target_cache_.edram_buffer_, 0, 1);
        encoder->setTexture(depth_texture, 0);
        encoder->useResource(render_target_cache_.edram_buffer_,
                             MTL::ResourceUsageWrite);
        encoder->useResource(depth_texture, MTL::ResourceUsageRead);
        for (uint32_t rect_index = 0; rect_index < rectangle_count;
             ++rect_index) {
          uint32_t group_count_x = 0;
          uint32_t group_count_y = 0;
          MetalRenderTargetCache::HostDepthStoreRectangleConstant
              rectangle_constant;
          render_target_cache_.GetHostDepthStoreRectangleInfo(
              rectangles[rect_index], dest_key.msaa_samples, rectangle_constant,
              group_count_x, group_count_y);
          if (!group_count_x || !group_count_y) {
            continue;
          }
          MetalRenderTargetCache::HostDepthStoreConstants constants = {};
          constants.rectangle = rectangle_constant;
          constants.render_target = render_target_constant;
          encoder->setBytes(&constants, sizeof(constants), 0);
          encoder->dispatchThreadgroups(
              MTL::Size::Make(group_count_x, group_count_y, 1),
              MTL::Size::Make(8, 8, 1));
          host_depth_store_dispatched = true;
        }
        encoder->endEncoding();
      }
      break;
    }
  }

  bool any_transfers_done = false;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& transfers = render_target_transfers[i];
    if (transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto* dest_metal_rt = static_cast<MetalRenderTarget*>(dest_rt);
    render_target_cache_.RecordPerformedTransfers(dest_metal_rt, transfers);
    const Transfer::Rectangle* const transfer_cutout = get_transfer_cutout(i);
    // What draws change from here on is tracked against what the transfers
    // wrote. A resolve clear writes more than the records say.
    if (!resolve_clear_needed && !transfers.empty() &&
        !dest_rt->key().is_depth) {
      dest_metal_rt->StartComponentTracking();
    }
    const bool dest_needed_initial_clear = dest_metal_rt->needs_initial_clear();
    if (dest_needed_initial_clear) {
      dest_metal_rt->SetNeedsInitialClear(false);
      if (output) {
        output->render_pass_descriptor_dirty = true;
      }
    }
    RenderTargetKey dest_key = dest_metal_rt->key();
    bool dest_is_depth = dest_key.is_depth;

    bool dest_is_uint = false;
    MTL::PixelFormat dest_pixel_format =
        dest_is_depth
            ? render_target_cache_.GetDepthPixelFormat(
                  dest_key.GetDepthFormat())
            : render_target_cache_.GetColorOwnershipTransferPixelFormat(
                  dest_key.GetColorFormat(), &dest_is_uint);
    // Inside the guest's pass, a bit copy the float view keeps exact is drawn
    // through that view, the attachment the pass binds.
    const bool dest_float_bits =
        use_active_render_encoder && !dest_is_depth && dest_is_uint &&
        IsColorTransferFloatBitsExact(dest_key.GetColorFormat());
    if (dest_float_bits) {
      dest_is_uint = false;
      dest_pixel_format = render_target_cache_.GetColorResourcePixelFormat(
          dest_key.GetColorFormat());
    }

    MTL::Texture* dest_texture = dest_is_depth || dest_float_bits
                                     ? dest_metal_rt->texture()
                                     : dest_metal_rt->transfer_texture();
    if (!dest_texture) {
      XELOGW(
          "MetalTransferEncoder::PerformTransfersAndResolveClears: "
          "Destination RT {} has no texture",
          i);
      continue;
    }
    if (dest_is_depth) {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer depth must use resource pixel format");
    } else {
      assert_true(dest_texture->pixelFormat() == dest_pixel_format,
                  "Transfer color must use ownership pixel format");
    }

    // The destination has to be an attachment of the pass already under way,
    // bound through the same texture the transfer draws write.
    uint32_t active_color_attachment_index = 0;
    if (use_active_render_encoder) {
      if (dest_is_depth) {
        auto* depth_attachment =
            active_render_pass_descriptor->depthAttachment();
        MTL::Texture* depth_texture =
            depth_attachment ? depth_attachment->texture() : nullptr;
        if (i != 0 || depth_texture != dest_texture ||
            active_attachment_formats.depth_attachment_format !=
                dest_pixel_format) {
          return false;
        }
        if (MTL::Texture* dest_stencil_texture =
                dest_metal_rt->stencil_attachment_texture()) {
          auto* stencil_attachment =
              active_render_pass_descriptor->stencilAttachment();
          MTL::Texture* stencil_texture =
              stencil_attachment ? stencil_attachment->texture() : nullptr;
          if (stencil_texture != dest_stencil_texture ||
              active_attachment_formats.stencil_attachment_format !=
                  dest_stencil_texture->pixelFormat()) {
            return false;
          }
        }
      } else {
        if (!i || i > xenos::kMaxColorRenderTargets) {
          return false;
        }
        active_color_attachment_index = i - 1;
        auto* color_attachments =
            active_render_pass_descriptor->colorAttachments();
        auto* color_attachment =
            color_attachments
                ? color_attachments->object(active_color_attachment_index)
                : nullptr;
        if (!color_attachment || color_attachment->texture() != dest_texture ||
            active_attachment_formats
                    .color_attachment_formats[active_color_attachment_index] !=
                dest_pixel_format) {
          return false;
        }
      }
    }
    const TransferColorAttachmentFormats* active_color_formats =
        use_active_render_encoder
            ? &active_attachment_formats.color_attachment_formats
            : nullptr;
    MTL::PixelFormat active_depth_format =
        use_active_render_encoder
            ? active_attachment_formats.depth_attachment_format
            : MTL::PixelFormatInvalid;
    MTL::PixelFormat active_stencil_format =
        use_active_render_encoder
            ? active_attachment_formats.stencil_attachment_format
            : MTL::PixelFormatInvalid;

    uint32_t dest_sample_count =
        MetalRenderTargetCache::MsaaSamplesToCount(dest_key.msaa_samples);
    uint32_t dest_width = uint32_t(dest_texture->width());
    uint32_t dest_height = uint32_t(dest_texture->height());

    auto get_scaled_rect = [&](const Transfer::Rectangle& rect,
                               uint32_t& scaled_x, uint32_t& scaled_y,
                               uint32_t& scaled_width,
                               uint32_t& scaled_height) -> bool {
      uint32_t rect_x = rect.x_pixels * scale_x;
      uint32_t rect_y = rect.y_pixels * scale_y;
      uint32_t rect_width = rect.width_pixels * scale_x;
      uint32_t rect_height = rect.height_pixels * scale_y;
      if (rect_x >= dest_width || rect_y >= dest_height) {
        return false;
      }
      rect_width = std::min(rect_width, dest_width - rect_x);
      rect_height = std::min(rect_height, dest_height - rect_y);
      if (!rect_width || !rect_height) {
        return false;
      }
      scaled_x = rect_x;
      scaled_y = rect_y;
      scaled_width = rect_width;
      scaled_height = rect_height;
      return true;
    };

    auto set_rect_viewport = [&](MTL::RenderCommandEncoder* encoder,
                                 const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      MTL::Viewport vp;
      vp.originX = double(scaled_x);
      vp.originY = double(scaled_y);
      vp.width = double(scaled_width);
      vp.height = double(scaled_height);
      vp.znear = 0.0;
      vp.zfar = 1.0;
      encoder->setViewport(vp);
      mark_encoder_mutation(
          MetalRenderTargetCache::kDrawPassTransferEncoderMutationViewport);
      MTL::ScissorRect scissor;
      scissor.x = scaled_x;
      scissor.y = scaled_y;
      scissor.width = scaled_width;
      scissor.height = scaled_height;
      encoder->setScissorRect(scissor);
      mark_encoder_mutation(
          MetalRenderTargetCache::kDrawPassTransferEncoderMutationScissor);
      return true;
    };

    std::vector<Transfer> filtered_transfers;
    bool used_blit = false;
    MTL::BlitCommandEncoder* blit_encoder = nullptr;
    auto ensure_blit_encoder = [&]() -> MTL::BlitCommandEncoder* {
      if (!blit_encoder) {
        command_processor_.EndSharedComputeEncoder(cmd);
        blit_encoder = cmd->blitCommandEncoder();
      }
      return blit_encoder;
    };

    // Fast path: when source/dest share compatible EDRAM layout and format,
    // use a blit instead of shader-based transfers. A blit needs an encoder of
    // its own, which is exactly the split the draw-pass path exists to avoid.
    if (!use_active_render_encoder && !transfers.empty()) {
      auto try_blit_transfer = [&](const Transfer& transfer) -> bool {
        // Only the transfer pass writes a rotated-in backing: it is loaded
        // with don't-care, so a blit ahead of it would be lost.
        if (dest_metal_rt == renamed_depth_dest) {
          return false;
        }
        auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
        if (!source_rt) {
          return false;
        }
        RenderTargetKey source_key = source_rt->key();
        // Strip merge: a band of a strip-merged render target into one laid
        // out like it with the EDRAM's rows (its twin), by the band shift,
        // every sample, and a depth with its stencil. Its own host depth is
        // the depth it holds. Nothing into a strip-merged destination, whose
        // bands this doesn't place.
        const bool strip_merge_source = source_key.strip_merged != 0;
        if (dest_key.strip_merged) {
          return false;
        }
        if (strip_merge_source) {
          if ((i == 0 && dest_is_depth && depth_keep_dest_depth) ||
              transfer.source_band_shift < 0 ||
              transfer.host_depth_source !=
                  (dest_is_depth ? transfer.source : nullptr)) {
            return false;
          }
        } else if (transfer.host_depth_source) {
          return false;
        }
        if (dest_is_depth != source_key.is_depth) {
          return false;
        }
        if (source_key.resource_format != dest_key.resource_format ||
            source_key.msaa_samples != dest_key.msaa_samples ||
            source_key.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp) {
          return false;
        }

        if (source_key.base_tiles != dest_key.base_tiles ||
            source_key.next_period != dest_key.next_period) {
          return false;
        }

        MTL::Texture* source_texture = dest_is_depth
                                           ? source_rt->texture()
                                           : source_rt->transfer_texture();
        if (!source_texture) {
          return false;
        }
        if (!dest_is_depth) {
          MTL::PixelFormat expected_format =
              render_target_cache_.GetColorOwnershipTransferPixelFormat(
                  source_key.GetColorFormat(), nullptr);
          assert_true(source_texture->pixelFormat() == expected_format,
                      "Transfer source must use ownership pixel format");
        }
        if (source_texture->pixelFormat() != dest_texture->pixelFormat() ||
            source_texture->sampleCount() != dest_texture->sampleCount()) {
          return false;
        }
        MTL::Texture* source_stencil = nullptr;
        MTL::Texture* dest_stencil = nullptr;
        uint32_t source_y_offset = 0;
        if (strip_merge_source) {
          if (dest_is_depth) {
            source_stencil = source_rt->stencil_view();
            dest_stencil = dest_metal_rt->stencil_view();
            if (!source_stencil || !dest_stencil ||
                source_stencil->pixelFormat() != MTL::PixelFormatStencil8 ||
                dest_stencil->pixelFormat() != MTL::PixelFormatStencil8 ||
                source_stencil->sampleCount() !=
                    source_texture->sampleCount() ||
                dest_stencil->sampleCount() != dest_texture->sampleCount() ||
                source_stencil->width() != source_texture->width() ||
                source_stencil->height() != source_texture->height() ||
                dest_stencil->width() != dest_width ||
                dest_stencil->height() != dest_height) {
              return false;
            }
          }
          source_y_offset =
              uint32_t(transfer.source_band_shift) *
              render_target_cache_.GetStripMergeBandRows(source_key) * scale_y;
        } else if (source_texture->sampleCount() != 1 ||
                   source_texture->width() != dest_width ||
                   source_texture->height() != dest_height) {
          return false;
        }

        Transfer::Rectangle
            rectangles[MetalRenderTargetCache::kMaxBandedRectanglesWithCutout];
        uint32_t rectangle_count = render_target_cache_.GetTransferRectangles(
            transfer, dest_key, dest_key.Is64bpp(), rectangles,
            transfer_cutout);
        if (!rectangle_count) {
          return false;
        }
        // All in the source, or the shaders take the whole transfer.
        struct BlitRegion {
          uint32_t x, y, width, height;
        };
        BlitRegion
            regions[MetalRenderTargetCache::kMaxBandedRectanglesWithCutout];
        uint32_t region_count = 0;
        for (uint32_t rect_index = 0; rect_index < rectangle_count;
             ++rect_index) {
          BlitRegion& region = regions[region_count];
          if (!get_scaled_rect(rectangles[rect_index], region.x, region.y,
                               region.width, region.height)) {
            continue;
          }
          if (region.x + region.width > source_texture->width() ||
              region.y + source_y_offset + region.height >
                  source_texture->height()) {
            return false;
          }
          ++region_count;
        }

        MTL::BlitCommandEncoder* blit = ensure_blit_encoder();
        if (!blit) {
          return false;
        }

        for (uint32_t region_index = 0; region_index < region_count;
             ++region_index) {
          const BlitRegion& region = regions[region_index];
          MTL::Origin source_origin =
              MTL::Origin::Make(region.x, region.y + source_y_offset, 0);
          MTL::Origin dest_origin = MTL::Origin::Make(region.x, region.y, 0);
          MTL::Size size = MTL::Size::Make(region.width, region.height, 1);
          blit->copyFromTexture(source_texture, 0, 0, source_origin, size,
                                dest_texture, 0, 0, dest_origin);
          if (source_stencil) {
            blit->copyFromTexture(source_stencil, 0, 0, source_origin, size,
                                  dest_stencil, 0, 0, dest_origin);
          }
        }

        used_blit = true;
        any_transfers_done = true;
        return true;
      };

      for (const Transfer& transfer : transfers) {
        if (!try_blit_transfer(transfer)) {
          filtered_transfers.push_back(transfer);
        }
      }
    }

    if (blit_encoder) {
      blit_encoder->endEncoding();
    }

    const bool disable_transfer_shaders = false;
    const std::vector<Transfer>& transfers_for_shaders =
        used_blit ? filtered_transfers : transfers;

    auto is_full_target_rectangle =
        [&](const Transfer::Rectangle& rect) -> bool {
      uint32_t scaled_x = 0;
      uint32_t scaled_y = 0;
      uint32_t scaled_width = 0;
      uint32_t scaled_height = 0;
      if (!get_scaled_rect(rect, scaled_x, scaled_y, scaled_width,
                           scaled_height)) {
        return false;
      }
      return !scaled_x && !scaled_y && scaled_width == dest_width &&
             scaled_height == dest_height;
    };

    auto transfers_fully_overwrite_target = [&]() -> bool {
      if (transfers_for_shaders.empty()) {
        return false;
      }
      for (const Transfer& transfer : transfers_for_shaders) {
        // A stencil-only transfer keeps the depth, which has to be loaded.
        if (i == 0 && dest_is_depth && depth_keep_dest_depth) {
          return false;
        }
        Transfer::Rectangle
            rectangles[MetalRenderTargetCache::kMaxBandedRectanglesWithCutout];
        uint32_t rectangle_count = render_target_cache_.GetTransferRectangles(
            transfer, dest_key, dest_key.Is64bpp(), rectangles,
            transfer_cutout);
        if (rectangle_count != 1 || !is_full_target_rectangle(rectangles[0])) {
          return false;
        }
      }
      return true;
    };

    bool resolve_clear_fully_overwrites_target = false;
    if (resolve_clear_needed && resolve_clear_rectangle) {
      // A render target is as tall as the EDRAM. Clearing all of it also
      // covers a smaller rectangle when nothing outside it holds EDRAM data.
      resolve_clear_fully_overwrites_target =
          is_full_target_rectangle(*resolve_clear_rectangle) ||
          (transfers_for_shaders.empty() &&
           render_target_cache_.draw_resolution_scale_x() == 1 &&
           render_target_cache_.draw_resolution_scale_y() == 1 &&
           render_target_cache_.IsRenderTargetDataOnlyInRectangle(
               dest_key, dest_key.Is64bpp(), *resolve_clear_rectangle));
    }

    bool resolve_clear_via_load_action = false;
    MTL::ClearColor resolve_clear_color = MTL::ClearColor(0.0, 0.0, 0.0, 0.0);
    double resolve_clear_depth = 1.0;
    uint32_t resolve_clear_stencil = 0;
    if (resolve_clear_needed && resolve_clear_fully_overwrites_target) {
      const uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_is_depth) {
        resolve_clear_depth =
            GetResolveClearDepth(dest_key.GetDepthFormat(), clear_value);
        resolve_clear_via_load_action = true;
        resolve_clear_stencil = uint32_t(clear_value) & 0xFF;
      } else {
        TransferClearColorFloatConstants float_constants = {};
        bool clear_via_drawing = false;
        switch (dest_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            GetResolveClearColor8888(clear_value, float_constants.color);
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            GetResolveClearColor8888(clear_value, float_constants.color);
            if (render_target_cache_.gamma_render_target_as_unorm16_) {
              for (uint32_t j = 0; j < 3; ++j) {
                float_constants.color[j] =
                    xenos::PWLGammaToLinear(float_constants.color[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            for (uint32_t j = 0; j < 2; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            float_constants.color[1] = float(uint32_t(clear_value >> 32));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
                uint64_t(float_constants.color[1]) !=
                    uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }

        bool clear_is_uint = false;
        render_target_cache_.GetColorOwnershipTransferPixelFormat(
            dest_key.GetColorFormat(), &clear_is_uint);
        if (!clear_is_uint && !clear_via_drawing) {
          resolve_clear_color = MTL::ClearColor(
              float_constants.color[0], float_constants.color[1],
              float_constants.color[2], float_constants.color[3]);
          resolve_clear_via_load_action = true;
        }
      }
    }

    // Prefer DontCare on transfer-pass loads only when destination contents are
    // provably fully overwritten by this pass.
    bool transfer_pass_load_dontcare = false;
    if (resolve_clear_fully_overwrites_target &&
        !resolve_clear_via_load_action) {
      transfer_pass_load_dontcare = true;
    }
    if (!transfer_pass_load_dontcare && !resolve_clear_needed) {
      // A rotated-in spare depth backing holds nothing worth loading: the
      // transfer overwrites every tile the destination owns.
      transfer_pass_load_dontcare = dest_metal_rt == renamed_depth_dest ||
                                    transfers_fully_overwrite_target();
    }
    MTL::LoadAction transfer_load_action = MTL::LoadActionLoad;
    if (resolve_clear_via_load_action) {
      transfer_load_action = MTL::LoadActionClear;
    } else if (transfer_pass_load_dontcare) {
      transfer_load_action = MTL::LoadActionDontCare;
    }

    // A clear the load action performs, with nothing else to draw into the
    // destination, needs no pass of its own: the next pass that binds it
    // performs it as its clear load action, and that pass doesn't load the old
    // contents either.
    if (resolve_clear_via_load_action && transfers_for_shaders.empty() &&
        !use_active_render_encoder && ::cvars::metal_defer_resolve_clears) {
      if (dest_is_depth) {
        dest_metal_rt->DeferDepthClear(float(resolve_clear_depth),
                                       resolve_clear_stencil);
      } else {
        const float color[4] = {
            float(resolve_clear_color.red), float(resolve_clear_color.green),
            float(resolve_clear_color.blue), float(resolve_clear_color.alpha)};
        dest_metal_rt->DeferColorClear(color);
      }
      dest_metal_rt->NoteDeferredClearRows(
          render_target_cache_.GetRenderTargetOwnedHeight(dest_key));
      if (output) {
        output->deferred_clear_targets.push_back(dest_key);
      }
      if (output) {
        output->render_pass_descriptor_dirty = true;
      }
      continue;
    }

    MTL::RenderCommandEncoder* transfer_encoder = nullptr;
    // The rows the transfer encoder's render area covers, 0 for all of the
    // destination.
    uint32_t transfer_encoder_area_height = 0;
    auto ensure_transfer_encoder = [&]() -> MTL::RenderCommandEncoder* {
      if (transfer_encoder) {
        return transfer_encoder;
      }
      if (use_active_render_encoder) {
        transfer_encoder = active_render_encoder;
        if (active_render_pass_descriptor) {
          transfer_encoder_area_height =
              uint32_t(active_render_pass_descriptor->renderTargetHeight());
        }
        // Whatever the guest left on the encoder must not cull, wireframe or
        // depth-bias the full-viewport transfer draws.
        transfer_encoder->setCullMode(MTL::CullModeNone);
        transfer_encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
        transfer_encoder->setDepthBias(0.0f, 0.0f, 0.0f);
        transfer_encoder->setDepthClipMode(MTL::DepthClipModeClip);
        mark_encoder_mutation(
            MetalRenderTargetCache::kDrawPassTransferEncoderMutationRasterizer);
        return transfer_encoder;
      }
      MTL::RenderPassDescriptor* rp =
          MTL::RenderPassDescriptor::renderPassDescriptor();
      if (dest_is_depth) {
        auto* da = rp->depthAttachment();
        da->setTexture(dest_texture);
        da->setLoadAction(transfer_load_action);
        da->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          da->setClearDepth(resolve_clear_depth);
        }
        if (MTL::Texture* dest_stencil_texture =
                dest_metal_rt->stencil_attachment_texture()) {
          auto* sa = rp->stencilAttachment();
          sa->setTexture(dest_stencil_texture);
          sa->setLoadAction(transfer_load_action);
          sa->setStoreAction(MTL::StoreActionStore);
          if (transfer_load_action == MTL::LoadActionClear) {
            sa->setClearStencil(resolve_clear_stencil);
          }
        }
      } else {
        auto* ca = rp->colorAttachments()->object(0);
        ca->setTexture(dest_texture);
        ca->setLoadAction(transfer_load_action);
        ca->setStoreAction(MTL::StoreActionStore);
        if (transfer_load_action == MTL::LoadActionClear) {
          ca->setClearColor(resolve_clear_color);
        }
      }
      // The transfers and the clear are inside what the destination owns.
      render_target_cache_.RestrictRenderArea(
          rp, &dest_key, 1,
          resolve_clear_needed
              ? (resolve_clear_rectangle->y_pixels +
                 resolve_clear_rectangle->height_pixels) *
                    render_target_cache_.draw_resolution_scale_y()
              : 0);
      transfer_encoder_area_height = uint32_t(rp->renderTargetHeight());
      command_processor_.EndSharedComputeEncoder(cmd);
      transfer_encoder = cmd->renderCommandEncoder(rp);
      return transfer_encoder;
    };

    // A load action is executed only if a render pass is actually encoded.
    // Full clears have no explicit clear draw, and may have no transfers at
    // all.
    if (resolve_clear_via_load_action && !ensure_transfer_encoder()) {
      dest_metal_rt->SetNeedsInitialClear(dest_needed_initial_clear);
      if (output) {
        output->render_pass_descriptor_dirty = true;
      }
      return false;
    }

    if (!transfers_for_shaders.empty() && disable_transfer_shaders) {
      static uint32_t transfer_shader_skip_log_count = 0;
      if (transfer_shader_skip_log_count < 8) {
        ++transfer_shader_skip_log_count;
        XELOGW(
            "MetalTransferEncoder::PerformTransfersAndResolveClears: "
            "transfer shaders disabled; skipping {} transfers for RT {}",
            transfers_for_shaders.size(), i);
      }
    } else if (!transfers_for_shaders.empty()) {
      // The depth draw carries the guest stencil where the device allows it,
      // which leaves nothing for the clear and the per-bit draws to do.
      bool use_native_stencil_output =
          dest_is_depth && UseNativeStencilOutputInTransfers();
      bool need_stencil_bit_draws = dest_is_depth && !use_native_stencil_output;
      bool stencil_clear_needed = need_stencil_bit_draws;

      transfer_invocations_.clear();
      transfer_invocations_.reserve(transfers_for_shaders.size() *
                                    (need_stencil_bit_draws ? 2 : 1));

      for (const Transfer& transfer : transfers_for_shaders) {
        if (transfer.source) {
          auto* source = static_cast<MetalRenderTarget*>(transfer.source);
          source->SetTemporarySortIndex(UINT32_MAX);
        }
        if (transfer.host_depth_source) {
          auto* host_depth =
              static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          host_depth->SetTemporarySortIndex(UINT32_MAX);
        }
      }

      uint32_t rt_sort_index = 0;
      auto ensure_sort_index = [&](MetalRenderTarget* rt) {
        if (rt && rt->temporary_sort_index() == UINT32_MAX) {
          rt->SetTemporarySortIndex(rt_sort_index++);
        }
      };

      for (uint32_t pass = 0; pass <= uint32_t(need_stencil_bit_draws);
           ++pass) {
        for (const Transfer& transfer : transfers_for_shaders) {
          if (!transfer.source) {
            continue;
          }
          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          auto* host_depth_rt =
              pass
                  ? nullptr
                  : static_cast<MetalRenderTarget*>(transfer.host_depth_source);
          // No host depth store for strip-merged render targets (see above).
          bool drop_host_depth = host_depth_rt && dest_key.strip_merged &&
                                 host_depth_rt == dest_metal_rt &&
                                 dest_metal_rt != renamed_depth_dest;
          if (drop_host_depth) {
            host_depth_rt = nullptr;
          }
          ensure_sort_index(source_rt);
          ensure_sort_index(host_depth_rt);

          RenderTargetKey source_key = source_rt->key();
          RenderTargetKey host_depth_key;
          if (host_depth_rt) {
            host_depth_key = host_depth_rt->key();
          }
          // A rotated destination reads its previous backing as an ordinary
          // host depth texture instead of the snapshot copy.
          EdramTransferShaderKey shader_key = GetTransferShaderKey(
              source_key, dest_key, host_depth_rt ? &host_depth_key : nullptr,
              host_depth_rt == dest_metal_rt &&
                  dest_metal_rt != renamed_depth_dest,
              pass != 0, active_color_attachment_index, transfer);
          shader_key.dest_float_bits = dest_float_bits ? 1 : 0;

          transfer_invocations_.emplace_back(transfer, shader_key);
          if (pass || drop_host_depth) {
            transfer_invocations_.back().transfer.host_depth_source = nullptr;
          }
        }
      }

      std::sort(transfer_invocations_.begin(), transfer_invocations_.end());

      if (stencil_clear_needed) {
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(
                dest_pixel_format, false, true, dest_sample_count, 0,
                active_color_formats, active_depth_format,
                active_stencil_format);
        MTL::DepthStencilState* stencil_clear_state =
            GetTransferStencilClearState();
        if (clear_pipeline && stencil_clear_state) {
          MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
          if (encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = 0.0f;
            encoder->setRenderPipelineState(clear_pipeline);
            encoder->setDepthStencilState(stencil_clear_state);
            encoder->setStencilReferenceValue(0);
            encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            mark_encoder_mutation(
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationPipeline |
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationDepthStencil |
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationStencilReference |
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationFragmentSlot0);
            for (const Transfer& transfer : transfers_for_shaders) {
              Transfer::Rectangle rectangles
                  [MetalRenderTargetCache::kMaxBandedRectanglesWithCutout];
              uint32_t rectangle_count =
                  render_target_cache_.GetTransferRectangles(
                      transfer, dest_key, dest_key.Is64bpp(), rectangles,
                      transfer_cutout);
              for (uint32_t rect_index = 0; rect_index < rectangle_count;
                   ++rect_index) {
                if (!set_rect_viewport(encoder, rectangles[rect_index])) {
                  continue;
                }
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                        NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
      MTL::RenderCommandEncoder* encoder = ensure_transfer_encoder();
      if (encoder) {
        bool transfer_viewport_full_set = false;
        MTL::ScissorRect last_transfer_scissor = {};
        bool last_transfer_scissor_valid = false;
        MTL::RenderPipelineState* last_transfer_pipeline = nullptr;
        MTL::DepthStencilState* last_transfer_depth_state = nullptr;
        bool last_transfer_stencil_reference_valid = false;
        uint32_t last_transfer_stencil_reference = 0;
        bool transfer_vertex_constants_valid = false;
        TransferVertexConstants last_transfer_vertex_constants = {};
        bool last_transfer_vertex_bytes_1_valid = false;
        TransferRectInstance last_transfer_vertex_bytes_1 = {};
        auto bind_transfer_pipeline = [&](MTL::RenderPipelineState* pipeline) {
          if (last_transfer_pipeline != pipeline) {
            encoder->setRenderPipelineState(pipeline);
            mark_encoder_mutation(MetalRenderTargetCache::
                                      kDrawPassTransferEncoderMutationPipeline);
            last_transfer_pipeline = pipeline;
          }
        };
        auto bind_transfer_depth_state = [&](MTL::DepthStencilState* state) {
          if (last_transfer_depth_state != state) {
            encoder->setDepthStencilState(state);
            mark_encoder_mutation(
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationDepthStencil);
            last_transfer_depth_state = state;
          }
        };
        MTL::Texture* set_textures[2][2] = {};
        MTL::Buffer* host_depth_buffer = nullptr;
        auto bind_transfer_source_texture = [&](uint32_t set, uint32_t binding,
                                                MTL::Texture* texture) {
          set_textures[set][binding] = texture;
        };
        auto bind_transfer_host_depth_buffer = [&](MTL::Buffer* buffer) {
          host_depth_buffer = buffer;
        };
        uint32_t transfer_push_constants[kTransferSampleDrawMax]
                                        [kTransferPushConstantDwordMax];
        uint32_t transfer_push_constant_count = 0;
        auto bind_transfer_fragment_resources =
            [&](const EdramTransferShaderKey& shader_key,
                const EdramTransferAddressConstant& address,
                const EdramTransferAddressConstant& host_depth_address,
                uint32_t stencil_mask, uint32_t sample_draw_count) -> bool {
          assert_true(sample_draw_count <= kTransferSampleDrawMax);
          const EdramTransferPipelineLayoutInfo& layout_info =
              kEdramTransferPipelineLayoutInfos[size_t(
                  kEdramTransferModes[size_t(shader_key.mode)]
                      .pipeline_layout)];

          // Only the dwords the layout declares are present, in enum order,
          // with the sample index after them when the shader takes it there.
          uint32_t
              shared_push_constants[kEdramTransferUsedPushConstantDwordCount];
          uint32_t shared_push_constant_count = 0;
          if (layout_info.used_push_constant_dwords &
              kEdramTransferUsedPushConstantDwordHostDepthAddressBit) {
            shared_push_constants[shared_push_constant_count++] =
                host_depth_address.constant;
          }
          if (layout_info.used_push_constant_dwords &
              kEdramTransferUsedPushConstantDwordAddressBit) {
            shared_push_constants[shared_push_constant_count++] =
                address.constant;
          }
          if (layout_info.used_push_constant_dwords &
              kEdramTransferUsedPushConstantDwordStencilMaskBit) {
            shared_push_constants[shared_push_constant_count++] = stencil_mask;
          }
          const bool draws_samples_separately =
              TransferDrawsSamplesSeparately(shader_key);
          transfer_push_constant_count =
              shared_push_constant_count + (draws_samples_separately ? 1 : 0);
          for (uint32_t sample_draw = 0; sample_draw < sample_draw_count;
               ++sample_draw) {
            std::memcpy(transfer_push_constants[sample_draw],
                        shared_push_constants,
                        sizeof(uint32_t) * shared_push_constant_count);
            if (draws_samples_separately) {
              // Emulating 2x MSAA as samples 0 and 3 of a 4-sample attachment
              // when there is no native 2x, the way the addressing expects.
              bool emulated_2x_upper_sample =
                  sample_draw_count == 2 &&
                  !render_target_cache_.msaa_2x_supported_ && sample_draw == 1;
              transfer_push_constants[sample_draw][shared_push_constant_count] =
                  emulated_2x_upper_sample ? 3u : sample_draw;
            }
          }

          MTL::Texture* textures[kTransferMslTextureCount] = {};
          for (uint32_t set = 0; set < kTransferDescriptorSetCount; ++set) {
            for (uint32_t binding = 0; binding < kTransferBindingsPerSet;
                 ++binding) {
              textures[TransferMslTextureIndex(set, binding)] =
                  set_textures[set][binding];
            }
          }
          encoder->setFragmentTextures(
              textures, NS::Range::Make(0, kTransferMslTextureCount));
          if (host_depth_buffer) {
            encoder->setFragmentBuffer(host_depth_buffer, 0,
                                       kTransferMslHostDepthBufferIndex);
          }
          mark_encoder_mutation(
              MetalRenderTargetCache::
                  kDrawPassTransferEncoderMutationFragmentTextures |
              MetalRenderTargetCache::
                  kDrawPassTransferEncoderMutationFragmentSlot0 |
              MetalRenderTargetCache::
                  kDrawPassTransferEncoderMutationFragmentSlot1);
          return true;
        };
        auto bind_transfer_fragment_sample = [&](uint32_t sample_draw) {
          if (transfer_push_constant_count) {
            encoder->setFragmentBytes(
                transfer_push_constants[sample_draw],
                sizeof(uint32_t) * transfer_push_constant_count,
                kTransferMslPushConstantBufferIndex);
          }
        };
        auto bind_transfer_stencil_reference = [&](uint32_t reference) {
          if (!last_transfer_stencil_reference_valid ||
              last_transfer_stencil_reference != reference) {
            encoder->setStencilReferenceValue(reference);
            mark_encoder_mutation(
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationStencilReference);
            last_transfer_stencil_reference = reference;
            last_transfer_stencil_reference_valid = true;
          }
        };
        auto bind_transfer_vertex_constants =
            [&](const TransferVertexConstants& constants) {
              if (!transfer_vertex_constants_valid ||
                  std::memcmp(&last_transfer_vertex_constants, &constants,
                              sizeof(constants)) != 0) {
                encoder->setVertexBytes(&constants, sizeof(constants), 0);
                mark_encoder_mutation(
                    MetalRenderTargetCache::
                        kDrawPassTransferEncoderMutationVertexSlot0);
                last_transfer_vertex_constants = constants;
                transfer_vertex_constants_valid = true;
              }
            };
        auto bind_transfer_scissor = [&](const MTL::ScissorRect& scissor) {
          if (!last_transfer_scissor_valid ||
              last_transfer_scissor.x != scissor.x ||
              last_transfer_scissor.y != scissor.y ||
              last_transfer_scissor.width != scissor.width ||
              last_transfer_scissor.height != scissor.height) {
            encoder->setScissorRect(scissor);
            mark_encoder_mutation(MetalRenderTargetCache::
                                      kDrawPassTransferEncoderMutationScissor);
            last_transfer_scissor = scissor;
            last_transfer_scissor_valid = true;
          }
        };
        auto bind_transfer_vertex_bytes_1 =
            [&](const TransferRectInstance& rect_instance) {
              if (!last_transfer_vertex_bytes_1_valid ||
                  std::memcmp(&last_transfer_vertex_bytes_1, &rect_instance,
                              sizeof(rect_instance)) != 0) {
                encoder->setVertexBytes(&rect_instance, sizeof(rect_instance),
                                        1);
                mark_encoder_mutation(
                    MetalRenderTargetCache::
                        kDrawPassTransferEncoderMutationVertexSlot1);
                last_transfer_vertex_bytes_1 = rect_instance;
                last_transfer_vertex_bytes_1_valid = true;
              }
            };
        auto bind_transfer_vertex_bytes_1_span =
            [&](const TransferRectInstance* rect_instances,
                uint32_t rect_instance_count) {
              if (!rect_instances || !rect_instance_count) {
                return;
              }
              encoder->setVertexBytes(
                  rect_instances,
                  size_t(rect_instance_count) * sizeof(TransferRectInstance),
                  1);
              mark_encoder_mutation(
                  MetalRenderTargetCache::
                      kDrawPassTransferEncoderMutationVertexSlot1);
              last_transfer_vertex_bytes_1_valid = false;
            };
        auto set_full_transfer_viewport_scissor = [&]() {
          if (!transfer_viewport_full_set) {
            MTL::Viewport vp;
            vp.originX = 0.0;
            vp.originY = 0.0;
            vp.width = double(dest_width);
            vp.height = double(dest_height);
            vp.znear = 0.0;
            vp.zfar = 1.0;
            encoder->setViewport(vp);
            mark_encoder_mutation(MetalRenderTargetCache::
                                      kDrawPassTransferEncoderMutationViewport);
            transfer_viewport_full_set = true;
          }
          MTL::ScissorRect scissor;
          scissor.x = 0;
          scissor.y = 0;
          scissor.width = dest_width;
          // The scissor can't leave the render area.
          scissor.height =
              transfer_encoder_area_height
                  ? std::min(dest_height, transfer_encoder_area_height)
                  : dest_height;
          bind_transfer_scissor(scissor);
        };

        std::vector<Transfer::Rectangle> merged_transfer_rectangles;
        for (size_t invocation_index = 0;
             invocation_index < transfer_invocations_.size();) {
          const auto& invocation = transfer_invocations_[invocation_index];
          size_t merged_invocation_end = invocation_index + 1;
          while (merged_invocation_end < transfer_invocations_.size() &&
                 invocation.CanBeMergedIntoOneDraw(
                     transfer_invocations_[merged_invocation_end]) &&
                 IsStripMergeAddressingSame(
                     invocation.transfer,
                     transfer_invocations_[merged_invocation_end].transfer,
                     dest_key)) {
            ++merged_invocation_end;
          }

          merged_transfer_rectangles.clear();
          merged_transfer_rectangles.reserve(
              (merged_invocation_end - invocation_index) *
              MetalRenderTargetCache::kMaxBandedRectanglesWithCutout);
          for (size_t merged_index = invocation_index;
               merged_index < merged_invocation_end; ++merged_index) {
            Transfer::Rectangle rectangles
                [MetalRenderTargetCache::kMaxBandedRectanglesWithCutout];
            uint32_t rectangle_count =
                render_target_cache_.GetTransferRectangles(
                    transfer_invocations_[merged_index].transfer, dest_key,
                    dest_key.Is64bpp(), rectangles, transfer_cutout);
            for (uint32_t rect_index = 0; rect_index < rectangle_count;
                 ++rect_index) {
              merged_transfer_rectangles.push_back(rectangles[rect_index]);
            }
          }
          invocation_index = merged_invocation_end;
          if (merged_transfer_rectangles.empty()) {
            continue;
          }

          const Transfer& transfer = invocation.transfer;
          const EdramTransferShaderKey& shader_key = invocation.shader_key;
          const EdramTransferModeInfo& mode_info =
              kEdramTransferModes[size_t(shader_key.mode)];
          bool is_stencil_bit =
              mode_info.output == EdramTransferOutput::kStencilBit;
          bool writes_stencil_with_depth =
              use_native_stencil_output &&
              mode_info.output == EdramTransferOutput::kDepth;
          bool needs_source_stencil =
              !EdramTransferSourceIsColor(shader_key.mode) &&
              (mode_info.output == EdramTransferOutput::kColor ||
               is_stencil_bit || writes_stencil_with_depth);

          auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
          if (!source_rt) {
            continue;
          }

          RenderTargetKey source_key = source_rt->key();
          bool source_is_uint = false;
          MTL::PixelFormat source_transfer_format = MTL::PixelFormatInvalid;
          if (EdramTransferSourceIsColor(shader_key.mode)) {
            source_transfer_format =
                render_target_cache_.GetColorOwnershipTransferPixelFormat(
                    source_key.GetColorFormat(), &source_is_uint);
          }

          if (is_stencil_bit) {
            // Depth/stencil state set per-bit below.
          } else if (i == 0 && dest_is_depth && depth_keep_dest_depth) {
            // The destination keeps its depth: stencil replaced from the
            // shader's reference output, depth neither tested nor written. (The
            // per-bit draws write the stencil when there is no such output.)
            bind_transfer_depth_state(writes_stencil_with_depth
                                          ? GetTransferStencilClearState()
                                          : GetTransferNoDepthStencilState());
          } else if (dest_is_depth) {
            bind_transfer_depth_state(
                writes_stencil_with_depth
                    ? GetTransferDepthAndStencilOutputState()
                    : GetTransferDepthStencilState(true));
          } else {
            MTL::DepthStencilState* no_depth_state =
                GetTransferNoDepthStencilState();
            if (!no_depth_state) {
              continue;
            }
            bind_transfer_depth_state(no_depth_state);
          }

          // The host depth set is declared before the source one, so a mode
          // that reads both puts the previous owner in set 1.
          std::memset(set_textures, 0, sizeof(set_textures));
          host_depth_buffer = nullptr;
          uint32_t source_set =
              EdramTransferUsesHostDepth(shader_key.mode) ? 1u : 0u;
          if (EdramTransferSourceIsColor(shader_key.mode)) {
            MTL::Texture* source_texture = source_rt->transfer_texture();
            if (!source_texture) {
              continue;
            }
            assert_true(source_texture->pixelFormat() == source_transfer_format,
                        "Transfer source must use ownership pixel format");
            bind_transfer_source_texture(source_set, 0, source_texture);
          } else {
            MTL::Texture* depth_texture = source_rt->texture();
            if (!depth_texture) {
              continue;
            }
            bind_transfer_source_texture(source_set, 0, depth_texture);
            if (needs_source_stencil) {
              MTL::Texture* stencil_texture =
                  render_target_cache_.GetStencilTextureView(source_rt);
              if (!stencil_texture) {
                continue;
              }
              bind_transfer_source_texture(source_set, 1, stencil_texture);
            }
          }

          if (EdramTransferUsesHostDepth(shader_key.mode)) {
            if (EdramTransferHostDepthIsCopy(shader_key.mode)) {
              // The copy modes read the previous owner back out of the EDRAM
              // buffer rather than binding it as a texture.
              MTL::Buffer* buffer = render_target_cache_.edram_buffer_
                                        ? render_target_cache_.edram_buffer_
                                        : GetTransferDummyBuffer();
              if (!buffer) {
                continue;
              }
              bind_transfer_host_depth_buffer(buffer);
            } else {
              auto* host_depth_rt =
                  static_cast<MetalRenderTarget*>(transfer.host_depth_source);
              MTL::Texture* host_depth_texture =
                  !host_depth_rt ? nullptr
                  : host_depth_rt == renamed_depth_dest
                      ? host_depth_rt->spare_depth_texture()
                      : host_depth_rt->texture();
              if (!host_depth_texture) {
                continue;
              }
              bind_transfer_source_texture(0, 0, host_depth_texture);
            }
          }

          // Everything the old MSL took as a runtime value - formats, sample
          // counts, tile dimensions - the emitter bakes into the shader, so
          // only the addressing is left to pass.
          EdramTransferAddressConstant address_constant;
          if (!MakeTransferAddressConstant(source_key, dest_key, transfer,
                                           address_constant)) {
            continue;
          }
          EdramTransferAddressConstant host_depth_address_constant;
          if (EdramTransferUsesHostDepth(shader_key.mode) &&
              !EdramTransferHostDepthIsCopy(shader_key.mode)) {
            auto* host_depth_rt =
                static_cast<MetalRenderTarget*>(transfer.host_depth_source);
            if (host_depth_rt) {
              RenderTargetKey host_depth_key = host_depth_rt->key();
              if (!MakeTransferAddressConstant(host_depth_key, dest_key,
                                               transfer,
                                               host_depth_address_constant)) {
                continue;
              }
              // The host depth address has no next period.
              if (!host_depth_key.strip_merged) {
                host_depth_address_constant.source_next_period = 0;
              }
            }
          }
          TransferVertexConstants vertex_constants;
          vertex_constants.pixel_to_ndc_x =
              dest_width ? (2.0f / float(dest_width)) : 0.0f;
          vertex_constants.pixel_to_ndc_y =
              dest_height ? (2.0f / float(dest_height)) : 0.0f;

          const uint32_t rectangle_count =
              uint32_t(merged_transfer_rectangles.size());
          std::vector<TransferRectInstance> rect_instance_list;
          rect_instance_list.reserve(rectangle_count);
          for (uint32_t rect_index = 0; rect_index < rectangle_count;
               ++rect_index) {
            uint32_t scaled_x = 0;
            uint32_t scaled_y = 0;
            uint32_t scaled_width = 0;
            uint32_t scaled_height = 0;
            if (!get_scaled_rect(merged_transfer_rectangles[rect_index],
                                 scaled_x, scaled_y, scaled_width,
                                 scaled_height) ||
                !scaled_width || !scaled_height) {
              continue;
            }
            TransferRectInstance rect_instance = {};
            rect_instance.origin_x = float(scaled_x);
            rect_instance.origin_y = float(scaled_y);
            rect_instance.size_x = float(scaled_width);
            rect_instance.size_y = float(scaled_height);
            rect_instance_list.push_back(rect_instance);
          }

          MTL::RenderPipelineState* pipeline = GetOrCreateTransferPipelines(
              shader_key, dest_pixel_format, dest_is_uint,
              writes_stencil_with_depth, active_color_attachment_index,
              active_color_formats, active_depth_format, active_stencil_format);
          if (!pipeline) {
            continue;
          }
          bind_transfer_pipeline(pipeline);
          bind_transfer_vertex_constants(vertex_constants);

          // Either the shader runs per sample and one draw covers all of them,
          // or it takes the sample index from the push constants and each
          // sample gets its own draw through the same pipeline.
          const uint32_t sample_draw_count =
              TransferDrawsSamplesSeparately(shader_key)
                  ? MetalRenderTargetCache::MsaaSamplesToCount(
                        dest_key.msaa_samples)
                  : 1;

          auto draw_transfer_rects = [&]() {
            if (!rect_instance_list.empty()) {
              set_full_transfer_viewport_scissor();
              constexpr uint32_t kTransferRectInlineBatchMax = 240;
              const TransferRectInstance* rect_instances =
                  rect_instance_list.data();
              uint32_t rect_instances_remaining =
                  uint32_t(rect_instance_list.size());
              while (rect_instances_remaining) {
                uint32_t batch_count = std::min(rect_instances_remaining,
                                                kTransferRectInlineBatchMax);
                if (batch_count == 1) {
                  bind_transfer_vertex_bytes_1(*rect_instances);
                } else {
                  bind_transfer_vertex_bytes_1_span(rect_instances,
                                                    batch_count);
                }
                encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip,
                                        NS::UInteger(0), NS::UInteger(4),
                                        NS::UInteger(batch_count));
                rect_instances += batch_count;
                rect_instances_remaining -= batch_count;
              }
            }
          };

          auto draw_transfer = [&](uint32_t stencil_mask) -> bool {
            if (!bind_transfer_fragment_resources(
                    shader_key, address_constant, host_depth_address_constant,
                    stencil_mask, sample_draw_count)) {
              return false;
            }
            for (uint32_t sample_draw = 0; sample_draw < sample_draw_count;
                 ++sample_draw) {
              bind_transfer_fragment_sample(sample_draw);
              draw_transfer_rects();
            }
            return true;
          };

          if (is_stencil_bit) {
            for (uint32_t bit = 0; bit < 8; ++bit) {
              MTL::DepthStencilState* stencil_state =
                  GetTransferStencilBitState(bit);
              if (!stencil_state) {
                continue;
              }
              bind_transfer_depth_state(stencil_state);
              bind_transfer_stencil_reference(uint32_t(1) << bit);
              if (!draw_transfer(uint32_t(1) << bit)) {
                break;
              }
            }
          } else {
            draw_transfer(0);
          }
          any_transfers_done = true;
        }
      }
    }

    if (resolve_clear_needed && !resolve_clear_via_load_action) {
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_is_depth) {
        const float depth_host_clear_value =
            GetResolveClearDepth(dest_key.GetDepthFormat(), clear_value);
        MTL::RenderPipelineState* clear_pipeline =
            GetOrCreateTransferClearPipeline(
                dest_pixel_format, false, true, dest_sample_count, 0,
                active_color_formats, active_depth_format,
                active_stencil_format);
        MTL::DepthStencilState* clear_state = GetTransferDepthClearState();
        if (clear_pipeline && clear_state) {
          MTL::RenderCommandEncoder* clear_encoder = ensure_transfer_encoder();
          if (clear_encoder) {
            TransferClearDepthConstants constants = {};
            constants.depth = depth_host_clear_value;
            clear_encoder->setRenderPipelineState(clear_pipeline);
            clear_encoder->setDepthStencilState(clear_state);
            clear_encoder->setStencilReferenceValue(uint32_t(clear_value) &
                                                    0xFF);
            clear_encoder->setFragmentBytes(&constants, sizeof(constants), 0);
            mark_encoder_mutation(
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationPipeline |
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationDepthStencil |
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationStencilReference |
                MetalRenderTargetCache::
                    kDrawPassTransferEncoderMutationFragmentSlot0);
            Transfer::Rectangle clear_rect = *resolve_clear_rectangle;
            if (set_rect_viewport(clear_encoder, clear_rect)) {
              clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                            NS::UInteger(0), NS::UInteger(3));
            }
          }
        }
      } else {
        TransferClearColorFloatConstants float_constants = {};
        TransferClearColorUintConstants uint_constants = {};
        bool clear_via_drawing = false;
        switch (dest_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8: {
            GetResolveClearColor8888(clear_value, float_constants.color);
          } break;
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            GetResolveClearColor8888(clear_value, float_constants.color);
            if (render_target_cache_.gamma_render_target_as_unorm16_) {
              for (uint32_t j = 0; j < 3; ++j) {
                float_constants.color[j] =
                    xenos::PWLGammaToLinear(float_constants.color[j]);
              }
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              float_constants.color[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            float_constants.color[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            for (uint32_t j = 0; j < 2; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            for (uint32_t j = 0; j < 4; ++j) {
              float_constants.color[j] =
                  float((clear_value >> (j * 16)) & 0xFFFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value)) {
              clear_via_drawing = true;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            float_constants.color[0] = float(uint32_t(clear_value));
            float_constants.color[1] = float(uint32_t(clear_value >> 32));
            if (uint64_t(float_constants.color[0]) != uint32_t(clear_value) ||
                uint64_t(float_constants.color[1]) !=
                    uint32_t(clear_value >> 32)) {
              clear_via_drawing = true;
            }
          } break;
        }

        bool clear_is_uint = false;
        MTL::PixelFormat clear_format =
            render_target_cache_.GetColorOwnershipTransferPixelFormat(
                dest_key.GetColorFormat(), &clear_is_uint);
        MTL::Texture* clear_texture = dest_metal_rt->transfer_texture();
        bool clear_use_uint = clear_is_uint;

        if (clear_use_uint) {
          switch (dest_key.GetColorFormat()) {
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value) & 0xFFFF;
              uint_constants.color[1] = (uint32_t(clear_value) >> 16) & 0xFFFF;
              uint_constants.color[2] = (uint32_t(clear_value >> 32)) & 0xFFFF;
              uint_constants.color[3] =
                  (uint32_t(clear_value >> 32) >> 16) & 0xFFFF;
              break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = 0;
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
              uint_constants.color[0] = uint32_t(clear_value);
              uint_constants.color[1] = uint32_t(clear_value >> 32);
              uint_constants.color[2] = 0;
              uint_constants.color[3] = 0;
              break;
            default:
              break;
          }
        }

        if (clear_via_drawing && clear_use_uint) {
          uint_constants.color[0] = uint32_t(clear_value);
          uint_constants.color[1] = uint32_t(clear_value >> 32);
          uint_constants.color[2] = 0;
          uint_constants.color[3] = 0;
        }

        if (clear_texture) {
          MTL::RenderPipelineState* clear_pipeline =
              GetOrCreateTransferClearPipeline(
                  clear_format, clear_use_uint, false, dest_sample_count,
                  active_color_attachment_index, active_color_formats,
                  active_depth_format, active_stencil_format);
          if (clear_pipeline) {
            MTL::RenderCommandEncoder* clear_encoder =
                ensure_transfer_encoder();
            if (clear_encoder) {
              MTL::DepthStencilState* no_depth_state =
                  GetTransferNoDepthStencilState();
              if (!no_depth_state) {
                continue;
              }
              clear_encoder->setRenderPipelineState(clear_pipeline);
              clear_encoder->setDepthStencilState(no_depth_state);
              if (clear_use_uint) {
                clear_encoder->setFragmentBytes(&uint_constants,
                                                sizeof(uint_constants), 0);
              } else {
                clear_encoder->setFragmentBytes(&float_constants,
                                                sizeof(float_constants), 0);
              }
              mark_encoder_mutation(
                  MetalRenderTargetCache::
                      kDrawPassTransferEncoderMutationPipeline |
                  MetalRenderTargetCache::
                      kDrawPassTransferEncoderMutationDepthStencil |
                  MetalRenderTargetCache::
                      kDrawPassTransferEncoderMutationFragmentSlot0);
              Transfer::Rectangle clear_rect = *resolve_clear_rectangle;
              if (set_rect_viewport(clear_encoder, clear_rect)) {
                clear_encoder->drawPrimitives(MTL::PrimitiveTypeTriangle,
                                              NS::UInteger(0), NS::UInteger(3));
              }
            }
          }
        }
      }
    }

    if (transfer_encoder && !use_active_render_encoder) {
      transfer_encoder->endEncoding();
    }
  }
  return true;
}

MTL::Function* MetalTransferEncoder::GetTransferRectVertexFunction() {
  if (transfer_rect_vertex_function_) {
    return transfer_rect_vertex_function_;
  }
  MTL::Library* library = GetOrCreateTransferLibrary();
  if (!library) {
    return nullptr;
  }
  transfer_rect_vertex_function_ = library->newFunction(
      NS::String::string("transfer_rect_vs", NS::UTF8StringEncoding));
  if (!transfer_rect_vertex_function_) {
    XELOGE(
        "MetalRenderTargetCache: the transfer library has no transfer_rect_vs");
  }
  return transfer_rect_vertex_function_;
}

bool MetalTransferEncoder::TransferDrawsSamplesSeparately(
    EdramTransferShaderKey key) const {
  if (!cvars::metal_transfer_sample_mask) {
    return false;
  }
  if (key.dest_msaa_samples == xenos::MsaaSamples::k1X) {
    return false;
  }
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    return false;
  }
  // Observed on Apple GPUs with the depth-and-stencil output state (not-equal
  // depth test, stencil replace on depth failure): the fragment sample mask
  // confines the depth write to the covered sample, but the stencil replace
  // still reaches the masked samples, so the last sample draw's stencil lands
  // in every sample of the pixel. Per-sample shading writes each sample's own
  // stencil.
  if (kEdramTransferModes[size_t(key.mode)].output ==
          EdramTransferOutput::kDepth &&
      UseNativeStencilOutputInTransfers()) {
    return false;
  }
  return EdramTransferHostDepthIsCopy(key.mode) ||
         key.host_depth_source_msaa_samples == xenos::MsaaSamples::k1X;
}

MTL::Function* MetalTransferEncoder::GetOrCreateTransferFragmentFunction(
    EdramTransferShaderKey key) {
  auto it = transfer_fragment_functions_.find(key);
  if (it != transfer_fragment_functions_.end()) {
    return it->second;
  }

  const EdramTransferModeInfo& mode_info =
      kEdramTransferModes[size_t(key.mode)];

  EdramTransferShaderOptions options;
  // The host depth buffer is declared with the pre-1.3 BufferBlock and Uniform
  // forms, like the dump shader's EDRAM buffer.
  options.spirv_version = 0x00010000;
  options.resolution_scale_x = render_target_cache_.draw_resolution_scale_x();
  options.resolution_scale_y = render_target_cache_.draw_resolution_scale_y();
  options.msaa_2x_attachments_supported =
      render_target_cache_.msaa_2x_supported_;
  if (EdramTransferSourceIsColor(key.mode)) {
    render_target_cache_.GetColorOwnershipTransferPixelFormat(
        xenos::ColorRenderTargetFormat(key.source_resource_format),
        &options.source_color_is_uint);
  }
  if (mode_info.output == EdramTransferOutput::kColor) {
    render_target_cache_.GetColorOwnershipTransferPixelFormat(
        xenos::ColorRenderTargetFormat(key.dest_resource_format),
        &options.dest_color_is_uint);
  }
  options.dest_color_float_bits = key.dest_float_bits;
  options.stencil_reference_output_supported =
      UseNativeStencilOutputInTransfers();
  const bool draws_samples_separately = TransferDrawsSamplesSeparately(key);
  options.sample_rate_shading_supported = !draws_samples_separately;
  // Metal has no pipeline-level sample mask, so the shader has to write the
  // one sample its draw covers, and the index rides in the push constants so
  // that one pipeline serves every sample.
  options.sample_mask_output = draws_samples_separately;
  options.sample_index_push_constant = draws_samples_separately;
  options.depth_float24_round = ::cvars::depth_float24_round;
  options.depth_float24_convert_in_pixel_shader =
      ::cvars::depth_float24_convert_in_pixel_shader;
  options.no_discard_stencil =
      ::cvars::no_discard_stencil_in_transfer_pipelines;
  options.fast_pitch_divmod = ::cvars::metal_transfer_fast_divmod;
  options.branchless_d24_host_depth = true;
  options.optimize_integer_addressing = true;

  MTL::Function* function = nullptr;
  std::vector<uint32_t> spirv = BuildEdramTransferShaderSpirv(key, options);
  if (spirv.empty()) {
    XELOGE(
        "MetalRenderTargetCache: failed to emit the transfer shader 0x{:08X}",
        key.key);
  } else {
    std::string error;
    function = CompileTransferFragmentFunctionMsl(device_, spirv, &error);
    if (!function) {
      XELOGE(
          "MetalRenderTargetCache: failed to compile the MSL transfer shader "
          "0x{:08X}: {}",
          key.key, error);
    }
  }
  // A failed key is cached as null so it is not retried on every transfer.
  transfer_fragment_functions_.emplace(key, function);
  return function;
}

MTL::RenderPipelineState* MetalTransferEncoder::GetOrCreateTransferPipelines(
    const EdramTransferShaderKey& key, MTL::PixelFormat dest_format,
    bool dest_is_uint, bool native_stencil_output,
    uint32_t color_attachment_index,
    const TransferColorAttachmentFormats* color_attachment_formats,
    MTL::PixelFormat depth_attachment_format,
    MTL::PixelFormat stencil_attachment_format) {
  const EdramTransferModeInfo& mode_info =
      kEdramTransferModes[size_t(key.mode)];
  EdramTransferOutput output = mode_info.output;
  bool source_is_color = EdramTransferSourceIsColor(key.mode);
  bool has_host_depth = EdramTransferUsesHostDepth(key.mode);
  native_stencil_output =
      native_stencil_output && output == EdramTransferOutput::kDepth;

  TransferPipelineKey pipeline_key = {};
  pipeline_key.shader_key = key;
  pipeline_key.native_stencil_output = native_stencil_output ? 1u : 0u;
  if (color_attachment_formats) {
    pipeline_key.color_attachment_formats = *color_attachment_formats;
  }
  if (output == EdramTransferOutput::kColor) {
    if (color_attachment_index >= xenos::kMaxColorRenderTargets) {
      return nullptr;
    }
    pipeline_key.color_attachment_index = color_attachment_index;
    if (!color_attachment_formats) {
      pipeline_key.color_attachment_formats[color_attachment_index] =
          dest_format;
    }
    if (pipeline_key.color_attachment_formats[color_attachment_index] !=
        dest_format) {
      return nullptr;
    }
    pipeline_key.depth_attachment_format = depth_attachment_format;
    pipeline_key.stencil_attachment_format = stencil_attachment_format;
  } else {
    pipeline_key.depth_attachment_format =
        depth_attachment_format != MTL::PixelFormatInvalid
            ? depth_attachment_format
            : dest_format;
    if (pipeline_key.depth_attachment_format != dest_format) {
      return nullptr;
    }
    const MTL::PixelFormat dest_stencil_format =
        MetalRenderTargetCache::GetStencilAttachmentPixelFormat(dest_format);
    if (dest_stencil_format != MTL::PixelFormatInvalid) {
      pipeline_key.stencil_attachment_format =
          stencil_attachment_format != MTL::PixelFormatInvalid
              ? stencil_attachment_format
              : dest_stencil_format;
      if (pipeline_key.stencil_attachment_format != dest_stencil_format) {
        return nullptr;
      }
    } else {
      pipeline_key.stencil_attachment_format = stencil_attachment_format;
    }
  }

  auto it = transfer_pipelines_.find(pipeline_key);
  if (it != transfer_pipelines_.end()) {
    return it->second;
  }

  MTL::Function* fragment_function = GetOrCreateTransferFragmentFunction(key);
  MTL::Function* vertex_function = GetTransferRectVertexFunction();
  if (!fragment_function || !vertex_function) {
    transfer_pipelines_.emplace(pipeline_key, nullptr);
    return nullptr;
  }

  NS::Error* error = nullptr;

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vertex_function);
  desc->setFragmentFunction(fragment_function);

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    color_attachment->setPixelFormat(pipeline_key.color_attachment_formats[i]);
    color_attachment->setWriteMask(
        output == EdramTransferOutput::kColor &&
                i == pipeline_key.color_attachment_index
            ? MTL::ColorWriteMaskAll
            : MTL::ColorWriteMaskNone);
  }
  desc->setDepthAttachmentPixelFormat(pipeline_key.depth_attachment_format);
  desc->setStencilAttachmentPixelFormat(pipeline_key.stencil_attachment_format);

  uint32_t sample_count = 1;
  if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
    sample_count = 2;
  } else if (key.dest_msaa_samples == xenos::MsaaSamples::k4X) {
    sample_count = 4;
  }
  desc->setSampleCount(sample_count);

  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);

  desc->release();

  if (!pipeline) {
    XELOGE(
        "GetOrCreateTransferPipelines: failed to create pipeline (mode={}): {}",
        int(key.mode),
        error && error->localizedDescription()
            ? error->localizedDescription()->utf8String()
            : "unknown error");
    transfer_pipelines_.emplace(pipeline_key, nullptr);
    return nullptr;
  }

  transfer_pipelines_.emplace(pipeline_key, pipeline);

  return pipeline;
}

MTL::Library* MetalTransferEncoder::GetOrCreateTransferLibrary() {
  if (transfer_library_) {
    return transfer_library_;
  }
  static const char kTransferLibrarySource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
  float4 position [[position]];
};

struct TransferClearColorFloatConstants {
  float4 color;
};

struct TransferClearColorUintConstants {
  uint4 color;
};

struct TransferClearDepthConstants {
  float4 depth;
};

struct TransferRectInstance {
  float2 origin;
  float2 size;
};

struct TransferVertexConstants {
  float2 pixel_to_ndc;
};

// The shared fragment shader derives everything from the fragment coordinate,
// so this only has to place the rectangle.
vertex VSOut transfer_rect_vs(uint vid [[vertex_id]], uint iid [[instance_id]],
                              constant TransferVertexConstants& constants
                                  [[buffer(0)]],
                              constant TransferRectInstance* instances
                                  [[buffer(1)]]) {
  float2 quad = float2(float(vid & 1), float(vid >> 1));
  TransferRectInstance inst = instances[iid];
  float2 pos_pixel = inst.origin + quad * inst.size;
  VSOut out;
  out.position = float4(pos_pixel.x * constants.pixel_to_ndc.x - 1.0f,
                        1.0f - pos_pixel.y * constants.pixel_to_ndc.y, 0.0f,
                        1.0f);
  return out;
}

vertex VSOut transfer_clear_vs(uint vid [[vertex_id]]) {
  float2 pt = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.position = float4(pt * 2.0f - 1.0f, 0.0f, 1.0f);
  return out;
}

fragment float4 transfer_clear_color_float_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorFloatConstants& constants [[buffer(0)]]) {
  return constants.color;
}

fragment uint4 transfer_clear_color_uint_ps(
    VSOut in [[stage_in]],
    constant TransferClearColorUintConstants& constants [[buffer(0)]]) {
  return constants.color;
}

struct TransferDepthOut {
  float depth [[depth(any)]];
};

fragment TransferDepthOut transfer_clear_depth_ps(
    VSOut in [[stage_in]],
    constant TransferClearDepthConstants& constants [[buffer(0)]]) {
  TransferDepthOut out;
  out.depth = constants.depth.x;
  return out;
}
)METAL";

  NS::Error* error = nullptr;
  auto source_str =
      NS::String::string(kTransferLibrarySource, NS::UTF8StringEncoding);
  transfer_library_ = device_->newLibrary(source_str, nullptr, &error);
  if (!transfer_library_) {
    XELOGE("GetOrCreateTransferLibrary: failed to compile transfer library: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
  }
  return transfer_library_;
}

MTL::RenderPipelineState*
MetalTransferEncoder::GetOrCreateTransferClearPipeline(
    MTL::PixelFormat dest_format, bool dest_is_uint, bool is_depth,
    uint32_t sample_count, uint32_t color_attachment_index,
    const TransferColorAttachmentFormats* color_attachment_formats,
    MTL::PixelFormat depth_attachment_format,
    MTL::PixelFormat stencil_attachment_format) {
  TransferClearPipelineKey key = {};
  key.sample_count = sample_count ? sample_count : 1;
  key.dest_is_uint = dest_is_uint ? 1u : 0u;
  key.is_depth = is_depth ? 1u : 0u;
  if (color_attachment_formats) {
    key.color_attachment_formats = *color_attachment_formats;
  }
  if (is_depth) {
    key.depth_attachment_format =
        depth_attachment_format != MTL::PixelFormatInvalid
            ? depth_attachment_format
            : dest_format;
    if (key.depth_attachment_format != dest_format) {
      return nullptr;
    }
    const MTL::PixelFormat dest_stencil_format =
        MetalRenderTargetCache::GetStencilAttachmentPixelFormat(dest_format);
    if (dest_stencil_format != MTL::PixelFormatInvalid) {
      key.stencil_attachment_format =
          stencil_attachment_format != MTL::PixelFormatInvalid
              ? stencil_attachment_format
              : dest_stencil_format;
      if (key.stencil_attachment_format != dest_stencil_format) {
        return nullptr;
      }
    } else {
      key.stencil_attachment_format = stencil_attachment_format;
    }
  } else {
    if (color_attachment_index >= xenos::kMaxColorRenderTargets) {
      return nullptr;
    }
    key.color_attachment_index = color_attachment_index;
    if (!color_attachment_formats) {
      key.color_attachment_formats[color_attachment_index] = dest_format;
    }
    if (key.color_attachment_formats[color_attachment_index] != dest_format) {
      return nullptr;
    }
    key.depth_attachment_format = depth_attachment_format;
    key.stencil_attachment_format = stencil_attachment_format;
  }
  auto it = transfer_clear_pipelines_.find(key);
  if (it != transfer_clear_pipelines_.end()) {
    return it->second;
  }

  MTL::Library* lib = GetOrCreateTransferLibrary();
  if (!lib) {
    return nullptr;
  }

  auto vs_name =
      NS::String::string("transfer_clear_vs", NS::UTF8StringEncoding);
  const char* ps_name_cstr = nullptr;
  if (is_depth) {
    ps_name_cstr = "transfer_clear_depth_ps";
  } else {
    ps_name_cstr = dest_is_uint ? "transfer_clear_color_uint_ps"
                                : "transfer_clear_color_float_ps";
  }
  auto ps_name = NS::String::string(ps_name_cstr, NS::UTF8StringEncoding);

  MTL::Function* vs = lib->newFunction(vs_name);
  MTL::Function* ps = lib->newFunction(ps_name);
  if (!vs || !ps) {
    XELOGE(
        "GetOrCreateTransferClearPipeline: missing transfer clear functions");
    if (vs) {
      vs->release();
    }
    if (ps) {
      ps->release();
    }
    return nullptr;
  }

  MTL::RenderPipelineDescriptor* desc =
      MTL::RenderPipelineDescriptor::alloc()->init();
  desc->setVertexFunction(vs);
  desc->setFragmentFunction(ps);
  desc->setSampleCount(key.sample_count);

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color_attachment = desc->colorAttachments()->object(i);
    color_attachment->setPixelFormat(key.color_attachment_formats[i]);
    color_attachment->setWriteMask(!is_depth && i == key.color_attachment_index
                                       ? MTL::ColorWriteMaskAll
                                       : MTL::ColorWriteMaskNone);
  }
  desc->setDepthAttachmentPixelFormat(key.depth_attachment_format);
  desc->setStencilAttachmentPixelFormat(key.stencil_attachment_format);

  NS::Error* error = nullptr;
  MTL::RenderPipelineState* pipeline =
      device_->newRenderPipelineState(desc, &error);

  desc->release();
  vs->release();
  ps->release();

  if (!pipeline) {
    XELOGE("GetOrCreateTransferClearPipeline: failed to create pipeline: {}",
           error && error->localizedDescription()
               ? error->localizedDescription()->utf8String()
               : "unknown error");
    return nullptr;
  }

  transfer_clear_pipelines_.emplace(key, pipeline);
  return pipeline;
}

MTL::Buffer* MetalTransferEncoder::GetTransferDummyBuffer() {
  if (!transfer_dummy_buffer_ && device_) {
    transfer_dummy_buffer_ =
        device_->newBuffer(sizeof(uint32_t), MTL::ResourceStorageModeShared);
    if (transfer_dummy_buffer_) {
    }
    if (transfer_dummy_buffer_) {
      std::memset(transfer_dummy_buffer_->contents(), 0, sizeof(uint32_t));
    }
  }
  return transfer_dummy_buffer_;
}

MTL::DepthStencilState* MetalTransferEncoder::GetTransferDepthStencilState(
    bool depth_write) {
  if (transfer_depth_state_) {
    return transfer_depth_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(::cvars::depth_transfer_not_equal_test
                                    ? MTL::CompareFunctionNotEqual
                                    : MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(depth_write);
  transfer_depth_state_ = device_->newDepthStencilState(desc);
  desc->release();
  return transfer_depth_state_;
}

MTL::DepthStencilState*
MetalTransferEncoder::GetTransferDepthAndStencilOutputState() {
  if (transfer_depth_stencil_output_state_) {
    return transfer_depth_stencil_output_state_;
  }
  bool not_equal_test = ::cvars::depth_transfer_not_equal_test;
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(not_equal_test ? MTL::CompareFunctionNotEqual
                                               : MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(true);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  // Always, not not-equal, so a differing stencil doesn't suppress the depth
  // write - and with the not-equal depth test, replacing on depth failure so
  // matching depth still gets its stencil written.
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(not_equal_test
                                        ? MTL::StencilOperationReplace
                                        : MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_depth_stencil_output_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_depth_stencil_output_state_;
}

MTL::DepthStencilState* MetalTransferEncoder::GetTransferNoDepthStencilState() {
  if (transfer_depth_state_none_) {
    return transfer_depth_state_none_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  transfer_depth_state_none_ = device_->newDepthStencilState(desc);
  desc->release();
  return transfer_depth_state_none_;
}

MTL::DepthStencilState* MetalTransferEncoder::GetTransferDepthClearState() {
  if (transfer_depth_clear_state_) {
    return transfer_depth_clear_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(true);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_depth_clear_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_depth_clear_state_;
}

MTL::DepthStencilState* MetalTransferEncoder::GetTransferStencilClearState() {
  if (transfer_stencil_clear_state_) {
    return transfer_stencil_clear_state_;
  }
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(0xFF);
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_stencil_clear_state_ = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_stencil_clear_state_;
}

bool MetalTransferEncoder::ProbeNativeStencilOutputSupport() {
  // Metal has no query for fragment stencil output, and it decides which draws
  // a stencil transfer needs, so build a pipeline that uses it up front rather
  // than discovering it half way through a batch.
  static const char kProbeSource[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

vertex float4 xe_stencil_probe_vs() {
  return float4(0.0f, 0.0f, 0.0f, 1.0f);
}

struct XeStencilProbeOut {
  uint stencil [[stencil]];
};

fragment XeStencilProbeOut xe_stencil_probe_ps() {
  XeStencilProbeOut out;
  out.stencil = 0u;
  return out;
}
)METAL";

  NS::Error* error = nullptr;
  MTL::Library* lib = device_->newLibrary(
      NS::String::string(kProbeSource, NS::UTF8StringEncoding), nullptr,
      &error);
  if (!lib) {
    return false;
  }
  MTL::Function* vs = lib->newFunction(
      NS::String::string("xe_stencil_probe_vs", NS::UTF8StringEncoding));
  MTL::Function* ps = lib->newFunction(
      NS::String::string("xe_stencil_probe_ps", NS::UTF8StringEncoding));
  MTL::RenderPipelineState* pipeline = nullptr;
  if (vs && ps) {
    MTL::PixelFormat depth_format = render_target_cache_.GetDepthPixelFormat(
        xenos::DepthRenderTargetFormat::kD24S8);
    MTL::RenderPipelineDescriptor* desc =
        MTL::RenderPipelineDescriptor::alloc()->init();
    desc->setVertexFunction(vs);
    desc->setFragmentFunction(ps);
    desc->setDepthAttachmentPixelFormat(depth_format);
    desc->setStencilAttachmentPixelFormat(
        MetalRenderTargetCache::GetStencilAttachmentPixelFormat(depth_format));
    pipeline = device_->newRenderPipelineState(desc, &error);
    desc->release();
  }
  if (vs) {
    vs->release();
  }
  if (ps) {
    ps->release();
  }
  lib->release();
  if (!pipeline) {
    return false;
  }
  pipeline->release();
  return true;
}

MTL::DepthStencilState* MetalTransferEncoder::GetTransferStencilBitState(
    uint32_t bit) {
  if (bit >= 8) {
    return nullptr;
  }
  if (transfer_stencil_bit_states_[bit]) {
    return transfer_stencil_bit_states_[bit];
  }
  uint32_t mask = uint32_t(1) << bit;
  MTL::DepthStencilDescriptor* desc =
      MTL::DepthStencilDescriptor::alloc()->init();
  desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
  desc->setDepthWriteEnabled(false);
  MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
  stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
  stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
  stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
  stencil->setReadMask(0xFF);
  stencil->setWriteMask(uint32_t(mask));
  desc->setFrontFaceStencil(stencil);
  desc->setBackFaceStencil(stencil);
  transfer_stencil_bit_states_[bit] = device_->newDepthStencilState(desc);
  stencil->release();
  desc->release();
  return transfer_stencil_bit_states_[bit];
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
