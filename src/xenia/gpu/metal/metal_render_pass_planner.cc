/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_render_target_cache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/metal/metal_command_processor.h"
#include "xenia/gpu/metal/metal_texture_cache.h"
#include "xenia/gpu/metal/metal_transfer_encoder.h"
#include "xenia/gpu/metal/metal_util.h"
#include "xenia/gpu/strip_merge.h"
#include "xenia/gpu/xenos.h"

DECLARE_bool(metal_transfer_in_draw_pass);
DECLARE_bool(metal_defer_resolve_clears);
DECLARE_bool(metal_resolve_store_dontcare);
DECLARE_int32(metal_memory_log_rate);

namespace xe {
namespace gpu {
namespace metal {

namespace {
// Erases the transfers matching the predicate in place, calling it once per
// transfer in order.
template <typename T, typename Predicate>
void EraseTransfersIf(std::vector<T>& transfers, Predicate predicate) {
  size_t kept = 0;
  for (size_t i = 0; i < transfers.size(); ++i) {
    if (predicate(transfers[i])) {
      continue;
    }
    if (kept != i) {
      transfers[kept] = transfers[i];
    }
    ++kept;
  }
  transfers.erase(transfers.begin() + kept, transfers.end());
}
}  // namespace

void MetalRenderTargetCache::RecordTileTransfer(
    uint32_t start_tiles, uint32_t end_tiles, const MetalRenderTarget* dest,
    const MetalRenderTarget* source) {
  const TileTransferRecord record = {
      dest->key(), source ? source->key() : RenderTargetKey(),
      source ? source->content_generation() : 0, dest->content_generation()};
  std::fill(tile_transfer_records_.begin() + start_tiles,
            tile_transfer_records_.begin() + end_tiles, record);
}

void MetalRenderTargetCache::ElideRedundantTransfers() {
  RenderTarget* const* targets = last_update_accumulated_render_targets();
  std::vector<Transfer>* transfer_lists = last_update_transfers();
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    auto* dest = static_cast<MetalRenderTarget*>(targets[i]);
    std::vector<Transfer>& transfers = transfer_lists[i];
    if (!dest) {
      continue;
    }
    const RenderTargetKey dest_key = dest->key();
    // A transfer straight back whose source and destination are both
    // unchanged since the transfer that last wrote every tile it covers would
    // rewrite exactly what the destination's backing already holds.
    auto round_trip = [&](const Transfer& transfer) {
      auto* source = static_cast<MetalRenderTarget*>(transfer.source);
      // The host depth must be the destination's own, which is what its
      // backing already holds; a third render target's host depth is another
      // result.
      // The records don't tell the bands of a strip-merged source.
      if (!source || dest->needs_initial_clear() ||
          source->needs_initial_clear() ||
          (transfer.host_depth_source && transfer.host_depth_source != dest) ||
          (source->key().strip_merged && !dest_key.strip_merged)) {
        return false;
      }
      for (uint32_t tile = transfer.start_tiles; tile < transfer.end_tiles;
           ++tile) {
        const TileTransferRecord& record = tile_transfer_records_[tile];
        if (record.dest.key != source->key().key ||
            record.source.key != dest_key.key ||
            record.dest_generation != source->content_generation() ||
            record.source_generation != dest->content_generation()) {
          return false;
        }
      }
      return true;
    };
    EraseTransfersIf(transfers, [&](const Transfer& transfer) {
      if (!round_trip(transfer)) {
        return false;
      }
      RecordTileTransfer(transfer.start_tiles, transfer.end_tiles, dest,
                         static_cast<MetalRenderTarget*>(transfer.source));
      ++pass_stats_.transfers_elided;
      return true;
    });
  }
}

void MetalRenderTargetCache::KeepDepthOfStencilRoundTrips() {
  update_depth_keep_dest_depth_ = false;
  if (!transfer_encoder_->UseNativeStencilOutputInTransfers() ||
      IsDrawResolutionScaled()) {
    return;
  }
  auto* dest = static_cast<MetalRenderTarget*>(
      last_update_accumulated_render_targets()[0]);
  std::vector<Transfer>& transfers = last_update_transfers()[0];
  if (!dest || transfers.empty()) {
    return;
  }
  const RenderTargetKey dest_key = dest->key();
  if (!dest_key.is_depth || dest->needs_initial_clear() ||
      AnyDeferredClear(dest)) {
    return;
  }
  // Red is the low byte of the 8_8_8_8 dword, where both depth formats keep
  // the stencil; green, blue and alpha hold the 24 depth bits.
  constexpr uint32_t kStencilComponents = 0b0001;
  // All or none: a transfer left with host depth from the destination itself
  // renders into a rotated-in backing, which would lose a kept depth.
  for (const Transfer& transfer : transfers) {
    auto* source = static_cast<MetalRenderTarget*>(transfer.source);
    // Only a transfer that takes the depth back from the destination itself
    // wherever the source's depth bits still match it would write, there,
    // exactly the depth the destination holds. The transfer out of the
    // destination packed those bits with the same conversion the transfer back
    // compares them with, so they match everywhere draws left them alone.
    if (!source || source == dest || transfer.host_depth_source != dest) {
      return;
    }
    const RenderTargetKey source_key = source->key();
    // The records don't tell the bands of a strip-merged source.
    if (source_key.is_depth ||
        (source_key.strip_merged && !dest_key.strip_merged) ||
        source_key.GetColorFormat() !=
            xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
        source_key.base_tiles != dest_key.base_tiles ||
        source_key.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp ||
        source_key.msaa_samples != dest_key.msaa_samples ||
        source_key.next_period != dest_key.next_period ||
        source_key.scale_native != dest_key.scale_native ||
        source->needs_initial_clear() || AnyDeferredClear(source) ||
        (source->components_written_since_transfer() & ~kStencilComponents)) {
      return;
    }
    // Transfers still queued into the source would change it after the
    // records below.
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      if (pending_draw_pass_render_targets_[i] == source &&
          !pending_draw_pass_transfers_[i].empty()) {
        return;
      }
    }
    // Every tile last written by the transfer from the destination that
    // started the source's tracking, with the destination unchanged since.
    for (uint32_t tile = transfer.start_tiles; tile < transfer.end_tiles;
         ++tile) {
      const TileTransferRecord& record = tile_transfer_records_[tile];
      if (record.dest != source_key || record.source != dest_key ||
          record.source_generation != dest->content_generation() ||
          record.dest_generation != source->transfer_generation()) {
        return;
      }
    }
  }
  for (Transfer& transfer : transfers) {
    transfer.host_depth_source = nullptr;
  }
  update_depth_keep_dest_depth_ = true;
  ++pass_stats_.stencil_round_trips_kept_depth;
}

void MetalRenderTargetCache::RecordPerformedTransfers(
    MetalRenderTarget* dest, const std::vector<Transfer>& transfers) {
  MarkContentChanged(dest);
  for (const Transfer& transfer : transfers) {
    RecordTileTransfer(transfer.start_tiles, transfer.end_tiles, dest,
                       static_cast<MetalRenderTarget*>(transfer.source));
  }
}

const MetalRenderTargetCache::ConstantContent*
MetalRenderTargetCache::FindConstantContent(const RenderTarget* rt) const {
  for (const ConstantContent& content : constant_contents_) {
    if (content.render_target == rt) {
      return content.generation == content.render_target->content_generation()
                 ? &content
                 : nullptr;
    }
  }
  return nullptr;
}

void MetalRenderTargetCache::SetConstantContent(
    MetalRenderTarget* rt, uint32_t value,
    const Transfer::Rectangle& rectangle) {
  ForgetConstantContent(rt);
  constant_contents_.push_back(
      {rt, value, rt->content_generation(), rectangle});
}

void MetalRenderTargetCache::ForgetConstantContent(const RenderTarget* rt) {
  constant_contents_.erase(
      std::remove_if(constant_contents_.begin(), constant_contents_.end(),
                     [&](const ConstantContent& content) {
                       return content.render_target == rt;
                     }),
      constant_contents_.end());
}

namespace {
// Whether two pixel rectangles together cover exactly their bounding box,
// which is returned.
template <typename Rectangle>
bool UnionIsRectangle(const Rectangle& a, const Rectangle& b,
                      Rectangle& union_out) {
  const uint64_t l = std::min(a.x_pixels, b.x_pixels);
  const uint64_t t = std::min(a.y_pixels, b.y_pixels);
  const uint64_t r =
      std::max(a.x_pixels + a.width_pixels, b.x_pixels + b.width_pixels);
  const uint64_t bt =
      std::max(a.y_pixels + a.height_pixels, b.y_pixels + b.height_pixels);
  const uint64_t il = std::max(a.x_pixels, b.x_pixels);
  const uint64_t it = std::max(a.y_pixels, b.y_pixels);
  const uint64_t ir =
      std::min(a.x_pixels + a.width_pixels, b.x_pixels + b.width_pixels);
  const uint64_t ib =
      std::min(a.y_pixels + a.height_pixels, b.y_pixels + b.height_pixels);
  const uint64_t overlap = (ir > il && ib > it) ? (ir - il) * (ib - it) : 0;
  if ((r - l) * (bt - t) != uint64_t(a.width_pixels) * a.height_pixels +
                                uint64_t(b.width_pixels) * b.height_pixels -
                                overlap) {
    return false;
  }
  union_out.x_pixels = uint32_t(l);
  union_out.y_pixels = uint32_t(t);
  union_out.width_pixels = uint32_t(r - l);
  union_out.height_pixels = uint32_t(bt - t);
  return true;
}

}  // namespace

void MetalRenderTargetCache::ApplyClearQuadRule(
    RenderTarget* const* render_targets,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    const Shader& vertex_shader) {
  const ClearQuadDraw draw = clear_quad_draw_;
  if (!draw.candidate) {
    return;
  }
  const RegisterFile& regs = register_file();
  const reg::RB_DEPTHCONTROL dc = normalized_depth_control;
  std::vector<Transfer>* transfer_lists = last_update_transfers();
  // Window-space positions, nothing scaled or offset by a viewport, depth
  // clamped (not clipped) to the host viewport range.
  auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const bool window_space = clip_cntl.clip_disable && !vte.vport_x_scale_ena &&
                            !vte.vport_x_offset_ena && !vte.vport_y_scale_ena &&
                            !vte.vport_y_offset_ena && !vte.vport_z_scale_ena &&
                            !vte.vport_z_offset_ena && vte.vtx_xy_fmt &&
                            vte.vtx_z_fmt;
  // A quad that writes only color may also come from clip space through the
  // viewport. It can only drop transfers it overwrites whole.
  const bool through_viewport =
      !window_space && draw_color_write_mask_ && !clip_cntl.ucp_ena &&
      draw_resolution_scale_x() == 1 && draw_resolution_scale_y() == 1;
  if (!window_space && !through_viewport) {
    return;
  }
  auto sc_mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  // Filled, and not culled. Rectangle lists are drawn as triangle strips that
  // the host culls by facing; the facing of the quad is not worked out, so any
  // culling skips the rule.
  if (sc_mode.poly_offset_para_enable ||
      sc_mode.poly_mode == xenos::PolygonModeEnable::kDualMode ||
      sc_mode.cull_front || sc_mode.cull_back) {
    return;
  }
  // Every sample covered.
  auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  if ((regs[XE_GPU_REG_PA_SC_AA_MASK] & 0xF) != 0xF ||
      color_control.alpha_to_mask_enable ||
      (color_control.alpha_test_enable &&
       color_control.alpha_func != xenos::CompareFunction::kAlways)) {
    return;
  }
  DrawExtentEstimator::LastVertexPositions positions =
      draw_extent_estimator().last_vertex_positions();
  if (!positions.valid && through_viewport &&
      regs.Get<reg::VGT_DRAW_INITIATOR>().num_indices <=
          DrawExtentEstimator::kMaxLastVertices) {
    // The extent estimate only runs the vertex shader for unclipped draws.
    // Run it for a quad that has transfers it may overwrite.
    bool color_transfers = false;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      color_transfers |= ((draw_color_write_mask_ >> (i * 4)) & 0xF) &&
                         !transfer_lists[1 + i].empty();
    }
    // Or one that may begin a pass, which then needn't load what it covers.
    if (color_transfers || draw.pass_begin || update_targets_changed_) {
      draw_extent_estimator().EstimateVertexMaxY(vertex_shader, false);
      positions = draw_extent_estimator().last_vertex_positions();
    }
  }
  if (!positions.valid || positions.count != 3) {
    return;
  }
  // The guest viewport, applied to quads that come through it.
  const float viewport_x_scale =
      vte.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE)
                            : 1.0f;
  const float viewport_x_offset =
      vte.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET)
                             : 0.0f;
  const float viewport_y_scale =
      vte.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE)
                            : 1.0f;
  const float viewport_y_offset =
      vte.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET)
                             : 0.0f;
  if (through_viewport) {
    // W 1 leaves the perspective divide, in either W format, out of it. With
    // clipping, Z must be inside the clip volume; X and Y are clipped at the
    // host viewport, which the covered rectangle is cut to below.
    const float z_near = clip_cntl.dx_clip_space_def ? 0.0f : -1.0f;
    // The fourth corner is not given, so its Z is only known when all three
    // are equal.
    const float z = positions.z[0];
    if (!clip_cntl.clip_disable &&
        !(z >= z_near && z <= 1.0f && positions.z[1] == z &&
          positions.z[2] == z)) {
      return;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      if (!(positions.w[i] == 1.0f)) {
        return;
      }
      positions.x[i] = positions.x[i] * viewport_x_scale + viewport_x_offset;
      positions.y[i] = positions.y[i] * viewport_y_scale + viewport_y_offset;
    }
  }
  const float x_lo = std::min({positions.x[0], positions.x[1], positions.x[2]});
  const float x_hi = std::max({positions.x[0], positions.x[1], positions.x[2]});
  const float y_lo = std::min({positions.y[0], positions.y[1], positions.y[2]});
  const float y_hi = std::max({positions.y[0], positions.y[1], positions.y[2]});
  if (!(x_lo < x_hi) || !(y_lo < y_hi)) {
    return;
  }
  // Three distinct corners of an axis-aligned box: the rectangle is the box.
  bool corners[4] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    const float x = positions.x[i], y = positions.y[i];
    if ((x != x_lo && x != x_hi) || (y != y_lo && y != y_hi)) {
      return;
    }
    const uint32_t corner = uint32_t(x == x_hi) | (uint32_t(y == y_hi) << 1);
    if (corners[corner]) {
      return;
    }
    corners[corner] = true;
  }

  // The render targets the quad writes, all of every sample it covers. More
  // than one only drop transfers they overwrite whole.
  uint32_t slot = UINT32_MAX;
  uint32_t written_slots = 0;
  if (draw_color_write_mask_) {
    if (!draw.has_pixel_shader || draw.pixel_shader_kills) {
      return;
    }
    // Nothing may fail the depth or stencil test.
    if (render_targets[0] &&
        ((dc.z_enable && dc.zfunc != xenos::CompareFunction::kAlways) ||
         (dc.stencil_enable &&
          (dc.stencilfunc != xenos::CompareFunction::kAlways ||
           (dc.backface_enable &&
            dc.stencilfunc_bf != xenos::CompareFunction::kAlways))))) {
      return;
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      const uint32_t mask = (draw_color_write_mask_ >> (i * 4)) & 0xF;
      if (!mask) {
        continue;
      }
      if (mask != 0xF || !render_targets[1 + i]) {
        return;
      }
      auto blend = regs.Get<reg::RB_BLENDCONTROL>(
          reg::RB_BLENDCONTROL::rt_register_indices[i]);
      if (blend.color_srcblend != xenos::BlendFactor::kOne ||
          blend.color_destblend != xenos::BlendFactor::kZero ||
          blend.color_comb_fcn != xenos::BlendOp::kAdd ||
          blend.alpha_srcblend != xenos::BlendFactor::kOne ||
          blend.alpha_destblend != xenos::BlendFactor::kZero ||
          blend.alpha_comb_fcn != xenos::BlendOp::kAdd) {
        return;
      }
      slot = 1 + i;
      written_slots |= uint32_t(1) << slot;
    }
    // A color quad that also stores depth and every stencil bit overwrites
    // all of the depth target's samples it covers as well, whatever depth the
    // shader or the vertices give: the transfers into the depth target leave
    // the quad out like the ones into the colors.
    if (written_slots && render_targets[0]) {
      auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
      auto stencil_ref_mask_bf =
          regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
      if (dc.z_enable && dc.z_write_enable &&
          dc.zfunc == xenos::CompareFunction::kAlways && dc.stencil_enable &&
          dc.stencilfunc == xenos::CompareFunction::kAlways &&
          dc.stencilzpass == xenos::StencilOp::kReplace &&
          stencil_ref_mask.stencilwritemask == 0xFF &&
          (!dc.backface_enable ||
           (dc.stencilfunc_bf == xenos::CompareFunction::kAlways &&
            dc.stencilzpass_bf == xenos::StencilOp::kReplace &&
            stencil_ref_mask_bf.stencilwritemask == 0xFF &&
            stencil_ref_mask_bf.stencilref == stencil_ref_mask.stencilref))) {
        written_slots |= 1;
      }
    }
  } else {
    if (!render_targets[0] || draw.has_pixel_shader) {
      return;
    }
    auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
    auto stencil_ref_mask_bf =
        regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
    if (!dc.z_enable || !dc.z_write_enable ||
        dc.zfunc != xenos::CompareFunction::kAlways || !dc.stencil_enable ||
        dc.stencilfunc != xenos::CompareFunction::kAlways ||
        dc.stencilzpass != xenos::StencilOp::kReplace ||
        stencil_ref_mask.stencilwritemask != 0xFF ||
        (dc.backface_enable &&
         (dc.stencilfunc_bf != xenos::CompareFunction::kAlways ||
          dc.stencilzpass_bf != xenos::StencilOp::kReplace ||
          stencil_ref_mask_bf.stencilwritemask != 0xFF ||
          stencil_ref_mask_bf.stencilref != stencil_ref_mask.stencilref))) {
      return;
    }
    slot = 0;
  }
  // The pixels every sample of which lies in the closed box (sample positions
  // are inside the pixel), clipped to the scissor.
  float offset_x = 0.0f, offset_y = 0.0f;
  if (sc_mode.vtx_window_offset_enable) {
    auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    offset_x += float(window_offset.window_x_offset);
    offset_y += float(window_offset.window_y_offset);
  }
  if (::cvars::half_pixel_offset &&
      regs.Get<reg::PA_SU_VTX_CNTL>().pix_center ==
          xenos::PixelCenter::kD3DZero) {
    offset_x += 0.5f;
    offset_y += 0.5f;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor, true, false);
  // Through the viewport, the corners carry its float rounding. Every sample
  // is at least 1/8 of a pixel inside it, so a pixel whose edges are within
  // 1/16 of the box still has all of its samples in it.
  const float box_margin = through_viewport ? 1.0f / 16.0f : 0.0f;
  const float box_left =
      std::max(std::ceil(x_lo + offset_x - box_margin), 0.0f);
  const float box_top = std::max(std::ceil(y_lo + offset_y - box_margin), 0.0f);
  const float box_right = std::floor(x_hi + offset_x + box_margin);
  const float box_bottom = std::floor(y_hi + offset_y + box_margin);
  int64_t left = std::max(int64_t(box_left), int64_t(scissor.offset[0]));
  int64_t top = std::max(int64_t(box_top), int64_t(scissor.offset[1]));
  int64_t right = std::min(int64_t(box_right), int64_t(scissor.offset[0]) +
                                                   int64_t(scissor.extent[0]));
  int64_t bottom =
      std::min(int64_t(box_bottom),
               int64_t(scissor.offset[1]) + int64_t(scissor.extent[1]));
  if (through_viewport && !clip_cntl.clip_disable) {
    // The host viewport is the guest one with the window and half-pixel
    // offsets, snapped to whole pixels (draw_util::GetHostViewportInfo, with
    // the 32767 bounds Metal passes).
    const float viewport_x = viewport_x_offset + offset_x;
    const float viewport_y = viewport_y_offset + offset_y;
    const float viewport_x_extent = std::abs(viewport_x_scale);
    const float viewport_y_extent = std::abs(viewport_y_scale);
    if (!(viewport_x_extent < 8192.0f && viewport_y_extent < 8192.0f &&
          std::abs(viewport_x) < 8192.0f && std::abs(viewport_y) < 8192.0f)) {
      return;
    }
    left = std::max(left, int64_t(std::floor(viewport_x - viewport_x_extent)));
    top = std::max(top, int64_t(std::floor(viewport_y - viewport_y_extent)));
    right =
        std::min(right, int64_t(std::floor(viewport_x + viewport_x_extent)));
    bottom =
        std::min(bottom, int64_t(std::floor(viewport_y + viewport_y_extent)));
  }
  if (right <= left || bottom <= top || right > 16384 || bottom > 16384) {
    return;
  }
  if (draw.band_count) {
    // Drawn in every band of strip-merged render targets, each copy clamped
    // to its band: one covering the whole band covers every band's rows,
    // ending at the band edges. Anything less isn't one rectangle.
    const uint32_t written_mask = slot == 0 ? uint32_t(1) : written_slots;
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      if ((written_mask & (uint32_t(1) << i)) &&
          !render_targets[i]->key().strip_merged) {
        return;
      }
    }
    if (top || bottom < int64_t(draw.band_rows)) {
      return;
    }
    bottom = int64_t(draw.band_rows) * draw.band_count;
  }
  Transfer::Rectangle covered = {};
  covered.x_pixels = uint32_t(left);
  covered.y_pixels = uint32_t(top);
  covered.width_pixels = uint32_t(right - left);
  covered.height_pixels = uint32_t(bottom - top);
  // Everything below withholds a transfer or a load on the strength of the
  // quad being drawn, so the draw waits for its own pipeline
  // (update_relies_on_quad_draw()).
  update_relies_on_quad_draw_ = true;
  // Unless culled, the quad overwrites every sample of `covered` in the
  // targets it writes, so a pass it begins needn't load them if it covers the
  // pass's whole render area.
  if (!sc_mode.cull_front && !sc_mode.cull_back) {
    const uint32_t written_mask = slot == 0 ? uint32_t(1) : written_slots;
    bool textures_valid = true;
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      if (written_mask & (uint32_t(1) << i)) {
        clear_quad_textures_[i] =
            static_cast<MetalRenderTarget*>(render_targets[i])->texture();
        textures_valid &= clear_quad_textures_[i] != nullptr;
      }
    }
    if (textures_valid) {
      clear_quad_written_mask_ = written_mask;
      clear_quad_rectangle_ = covered;
    }
  }

  if (through_viewport || (written_slots & (written_slots - 1))) {
    // What the transfers into the targets it writes would write inside the
    // quad it overwrites: drop the transfers it covers whole, and leave it out
    // of the rest. The transfers into other targets stay whole.
    if (quad_hold_slot_ != UINT32_MAX) {
      return;
    }
    for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
      std::vector<Transfer>& slot_transfers = transfer_lists[i];
      if (slot_transfers.empty() || !(written_slots & (uint32_t(1) << i))) {
        continue;
      }
      const RenderTargetKey key = render_targets[i]->key();
      Transfer::Rectangle rectangles[kMaxBandedRectanglesWithCutout];
      slot_transfers.erase(
          std::remove_if(slot_transfers.begin(), slot_transfers.end(),
                         [&](const Transfer& transfer) {
                           return !GetTransferRectangles(transfer, key,
                                                         key.Is64bpp(),
                                                         rectangles, &covered);
                         }),
          slot_transfers.end());
      if (!slot_transfers.empty()) {
        update_cutout_mask_ |= uint32_t(1) << i;
        update_cutouts_[i] = covered;
        // The quad is the pass's draw after the transfers.
        if (::cvars::metal_transfer_in_draw_pass &&
            PendingDrawPassTransfersFullyOverwriteTarget(i, render_targets[i],
                                                         slot_transfers)) {
          update_cutout_full_overwrite_mask_ |= uint32_t(1) << i;
        }
      }
    }
    return;
  }
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (i != slot && !transfer_lists[i].empty()) {
      return;
    }
  }
  std::vector<Transfer>& transfers = transfer_lists[slot];
  const bool constant_depth_candidate = slot == 0;
  const bool hold = quad_hold_slot_ != UINT32_MAX;
  if (transfers.empty() && !constant_depth_candidate && !hold) {
    return;
  }

  auto* rt = static_cast<MetalRenderTarget*>(render_targets[slot]);
  const RenderTargetKey key = rt->key();
  if (hold && !quad_hold_chain_update_) {
    // Another quad of the same clear: the held transfers leave it out too if
    // it writes the same target and, with the quads drawn before it, still
    // covers a rectangle.
    Transfer::Rectangle merged;
    if (slot == quad_hold_slot_ && transfers.empty() &&
        render_targets[slot] == pending_draw_pass_render_targets_[slot] &&
        quad_hold_drawn_valid_ &&
        UnionIsRectangle(quad_hold_drawn_, covered, merged)) {
      quad_hold_candidate_ = covered;
      quad_hold_candidate_valid_ = true;
    }
  } else if (hold && !transfers.empty() && draw_resolution_scale_x() == 1 &&
             draw_resolution_scale_y() == 1) {
    // This quad's transfers read the held destination, which loses every
    // tile the held transfers wrote to them, at the same pixel positions.
    // What they leave out of it nothing reads again: a later owner gets it
    // back through a transfer of its own.
    auto* held_dest = static_cast<MetalRenderTarget*>(
        pending_draw_pass_render_targets_[quad_hold_slot_]);
    bool chain = held_dest && held_dest != rt;
    if (chain) {
      const RenderTargetKey held_key = held_dest->key();
      chain = held_key.base_tiles == key.base_tiles &&
              held_key.pitch_tiles_at_32bpp == key.pitch_tiles_at_32bpp &&
              held_key.msaa_samples == key.msaa_samples;
    }
    for (const Transfer& transfer : transfers) {
      chain = chain && transfer.source == held_dest &&
              transfer.host_depth_source != held_dest;
    }
    for (const Transfer& held : pending_draw_pass_transfers_[quad_hold_slot_]) {
      bool held_covered = false;
      for (const Transfer& transfer : transfers) {
        held_covered |= transfer.start_tiles <= held.start_tiles &&
                        held.end_tiles <= transfer.end_tiles;
      }
      chain = chain && held_covered;
    }
    if (chain) {
      quad_hold_chain_ = covered;
      quad_hold_chain_valid_ = true;
    }
  }
  if (!transfers.empty() && (!hold || quad_hold_chain_update_)) {
    // The quad overwrites what the transfers would have written inside it:
    // they only fill the rest. Whether the pass may skip loading the target
    // is as if they covered all of it, since the quad is the pass's first
    // draw.
    update_cutout_mask_ |= uint32_t(1) << slot;
    update_cutouts_[slot] = covered;
    update_cutout_quad_slot_ = slot;
    if (::cvars::metal_transfer_in_draw_pass &&
        PendingDrawPassTransfersFullyOverwriteTarget(slot, rt, transfers)) {
      update_cutout_full_overwrite_mask_ |= uint32_t(1) << slot;
    }
  }
  // Depth 0 and 1 are exact in both depth formats, and kept by every
  // transfer conversion.
  if (constant_depth_candidate) {
    const bool z_zero = positions.z[0] <= 0.0f && positions.z[1] <= 0.0f &&
                        positions.z[2] <= 0.0f;
    const bool z_one = positions.z[0] == 1.0f && positions.z[1] == 1.0f &&
                       positions.z[2] == 1.0f;
    // Any constant Z in D24S8: with W 1, the host depth is Z itself (Z * W
    // / W, scale 1, offset 0, depth range 0...1), which a transfer converts
    // with round-to-nearest-even of Z * 0xFFFFFF in float32.
    const bool d24s8 =
        key.GetDepthFormat() == xenos::DepthRenderTargetFormat::kD24S8;
    const bool z_d24s8 = d24s8 && positions.z[0] == positions.z[1] &&
                         positions.z[1] == positions.z[2] &&
                         positions.z[0] >= 0.0f && positions.z[0] <= 1.0f &&
                         positions.w[0] == 1.0f && positions.w[1] == 1.0f &&
                         positions.w[2] == 1.0f;
    if ((z_zero || z_one || z_d24s8) && draw_resolution_scale_x() == 1 &&
        draw_resolution_scale_y() == 1) {
      uint32_t guest_depth = 0;
      if (z_one) {
        guest_depth = d24s8 ? 0xFFFFFF : xenos::Float32To20e4(1.0f, true);
      } else if (!z_zero) {
        const float depth_scaled = positions.z[0] * float(0xFFFFFF);
        guest_depth = uint32_t(std::nearbyint(depth_scaled));
      }
      pending_constant_.render_target = rt;
      pending_constant_.value =
          (guest_depth << 8) |
          (regs.Get<reg::RB_STENCILREFMASK>().stencilref & 0xFF);
      pending_constant_.rectangle = covered;
    }
  }
}

void MetalRenderTargetCache::ReplaceConstantSourceTransfers(
    RenderTarget* const* render_targets) {
  if (constant_contents_.empty() || draw_resolution_scale_x() != 1 ||
      draw_resolution_scale_y() != 1) {
    return;
  }
  // Each attachment's transfers on their own: the clear and the cutout are the
  // destination's.
  const std::vector<Transfer>* transfer_lists = last_update_transfers();
  for (uint32_t i = 1; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!transfer_lists[i].empty() && render_targets[i] &&
        !(update_cutout_mask_ & (uint32_t(1) << i))) {
      ReplaceConstantSourceTransfers(
          i, static_cast<MetalRenderTarget*>(render_targets[i]));
    }
  }
}

void MetalRenderTargetCache::ReplaceConstantSourceTransfers(
    uint32_t slot, MetalRenderTarget* dest) {
  const std::vector<Transfer>& transfers = last_update_transfers()[slot];
  const RenderTargetKey dest_key = dest->key();
  const xenos::ColorRenderTargetFormat format = dest_key.GetColorFormat();
  bool is_uint = false;
  GetColorOwnershipTransferPixelFormat(format, &is_uint);
  if (is_uint || dest_key.Is64bpp() ||
      (format != xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
       format != xenos::ColorRenderTargetFormat::k_2_10_10_10 &&
       format != xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10)) {
    return;
  }
  // One constant behind every transfer, from sources laid out like the
  // destination (so the constant's rectangle is the same pixels), over whole
  // rows of the destination that are all it holds.
  const ConstantContent* constant = nullptr;
  uint32_t start_tiles = UINT32_MAX, end_tiles = 0, tile_count = 0;
  for (const Transfer& transfer : transfers) {
    auto* source = static_cast<MetalRenderTarget*>(transfer.source);
    const ConstantContent* source_constant =
        source ? FindConstantContent(source) : nullptr;
    if (!source_constant || transfer.host_depth_source) {
      return;
    }
    const RenderTargetKey source_key = source->key();
    if (source_key.base_tiles != dest_key.base_tiles ||
        source_key.pitch_tiles_at_32bpp != dest_key.pitch_tiles_at_32bpp ||
        source_key.msaa_samples != dest_key.msaa_samples ||
        (!source_key.is_depth && source_key.Is64bpp())) {
      return;
    }
    // Strip merge: the same pixels only between two strip-merged render
    // targets, each band from the same band.
    if ((source_key.strip_merged || dest_key.strip_merged) &&
        (!source_key.strip_merged || !dest_key.strip_merged ||
         transfer.band_count || transfer.source_band_shift)) {
      return;
    }
    if (constant &&
        (constant->value != source_constant->value ||
         std::memcmp(&constant->rectangle, &source_constant->rectangle,
                     sizeof(Transfer::Rectangle)))) {
      return;
    }
    constant = source_constant;
    start_tiles = std::min(start_tiles, transfer.start_tiles);
    end_tiles = std::max(end_tiles, transfer.end_tiles);
    tile_count += transfer.end_tiles - transfer.start_tiles;
  }
  const uint32_t pitch_tiles = dest_key.GetPitchTiles();
  if (!pitch_tiles || tile_count != end_tiles - start_tiles ||
      start_tiles < dest_key.base_tiles ||
      (start_tiles - dest_key.base_tiles) % pitch_tiles ||
      (end_tiles - start_tiles) % pitch_tiles) {
    return;
  }
  const uint32_t tile_width =
      xenos::kEdramTileWidthSamples >>
      uint32_t(dest_key.msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t tile_height =
      xenos::kEdramTileHeightSamples >>
      uint32_t(dest_key.msaa_samples >= xenos::MsaaSamples::k2X);
  Transfer::Rectangle rows = {};
  rows.y_pixels =
      (start_tiles - dest_key.base_tiles) / pitch_tiles * tile_height;
  rows.width_pixels = pitch_tiles * tile_width;
  rows.height_pixels = (end_tiles - start_tiles) / pitch_tiles * tile_height;
  if (dest_key.strip_merged) {
    // Every band's rows, so whole bands only, to be one rectangle.
    const uint32_t band_rows = GetStripMergeBandRows(dest_key);
    if (rows.y_pixels || rows.height_pixels != band_rows) {
      return;
    }
    rows.height_pixels = band_rows * strip_merge_bands_.count;
  }
  if (!IsRenderTargetDataOnlyInRectangle(dest_key, false, rows)) {
    return;
  }
  const uint32_t value = constant->value;
  float color[4];
  if (format == xenos::ColorRenderTargetFormat::k_8_8_8_8) {
    MetalTransferEncoder::GetResolveClearColor8888(value, color);
  } else {
    for (uint32_t j = 0; j < 3; ++j) {
      color[j] = ((value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
    }
    color[3] = ((value >> 30) & 0x3) * (1.0f / 0x3);
  }
  // The pass binding the destination clears all of it to the constant as its
  // load action, and the transfers fill only what lies outside the
  // constant's rectangle.
  dest->DeferColorClear(color);
  dest->NoteDeferredClearRows(GetRenderTargetOwnedHeight(dest_key));
  deferred_clear_targets_.push_back(dest_key);
  render_pass_descriptor_dirty_ = true;
  update_cutout_mask_ |= uint32_t(1) << slot;
  update_cutouts_[slot] = constant->rectangle;
}

void MetalRenderTargetCache::NoteDrawWrites() {
  // A held quad is drawn: the transfers it ran ahead of leave it out.
  if (quad_hold_slot_ != UINT32_MAX && quad_hold_candidate_valid_) {
    if (quad_hold_drawn_valid_) {
      // Checked to be a rectangle when the candidate was taken.
      Transfer::Rectangle merged;
      UnionIsRectangle(quad_hold_drawn_, quad_hold_candidate_, merged);
      quad_hold_drawn_ = merged;
    } else {
      quad_hold_drawn_ = quad_hold_candidate_;
    }
    quad_hold_drawn_valid_ = true;
    quad_hold_candidate_valid_ = false;
    quad_hold_in_encoder_ = true;
  }
  if (current_depth_target_ &&
      (draw_may_write_depth_ || draw_may_write_stencil_)) {
    MarkContentChanged(current_depth_target_);
  }
  if (pending_constant_.render_target &&
      pending_constant_.render_target == current_depth_target_) {
    SetConstantContent(pending_constant_.render_target, pending_constant_.value,
                       pending_constant_.rectangle);
  }
  pending_constant_ = PendingConstant();
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    const uint32_t components = (draw_color_write_mask_ >> (i * 4)) & 0b1111;
    if (current_color_targets_[i] && components) {
      MarkContentChangedByDraw(current_color_targets_[i], components);
    }
  }
}

bool MetalRenderTargetCache::CanRotateHostDepth(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* transfers, bool resolve_clear) const {
  // The rotation is a transfer draw with native stencil output from color
  // sources into an unscaled depth destination that is a host depth source of
  // its own transfers.
  if (!transfer_encoder_->UseNativeStencilOutputInTransfers() ||
      IsDrawResolutionScaled()) {
    return false;
  }
  if (!render_target_count || !render_targets || !transfers ||
      !render_targets[0] || transfers[0].empty()) {
    return false;
  }
  if (resolve_clear) {
    return false;
  }
  const auto* dest = static_cast<const MetalRenderTarget*>(render_targets[0]);
  const RenderTargetKey dest_key = dest->key();
  if (dest->needs_initial_clear() || !dest_key.is_depth) {
    return false;
  }
  // Every transfer renders into the spare backing, so together they have to
  // reach every tile the destination owns without a gap.
  bool self_sourced = false;
  uint32_t covered_end_tiles = dest_key.base_tiles;
  for (const Transfer& transfer : transfers[0]) {
    if (!transfer.source) {
      return false;
    }
    if (transfer.start_tiles != covered_end_tiles) {
      return false;
    }
    covered_end_tiles = transfer.end_tiles;
    if (transfer.host_depth_source == dest) {
      self_sourced = true;
    }
  }
  if (!self_sourced) {
    return false;
  }
  // Nothing else in this update may read the destination, whether it runs
  // standalone now or inside the draw pass later: after the rotation its
  // texture() is the new backing.
  auto reads_dest = [dest](const std::vector<Transfer>& others) {
    return std::any_of(
        others.cbegin(), others.cend(), [dest](const Transfer& other) {
          return other.source == dest || other.host_depth_source == dest;
        });
  };
  for (uint32_t i = 1; i < render_target_count; ++i) {
    if (reads_dest(transfers[i])) {
      return false;
    }
  }
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (reads_dest(pending_draw_pass_transfers_[i])) {
      return false;
    }
  }
  if (!IsRenderTargetOwnershipFullyCoveredByTransfer(
          dest_key, dest_key.base_tiles, covered_end_tiles)) {
    return false;
  }
  return true;
}

bool MetalRenderTargetCache::AcquireSpareDepthBacking(
    MetalRenderTarget* dest, const std::vector<Transfer>& transfers) {
  MTL::Texture* old_depth = dest->texture();
  if (!old_depth ||
      !transfer_encoder_->GetTransferDepthAndStencilOutputState()) {
    return false;
  }
  const RenderTargetKey dest_key = dest->key();
  const MTL::PixelFormat depth_format =
      GetDepthPixelFormat(dest_key.GetDepthFormat());
  // Nothing may fail once the backings are swapped, so every transfer's
  // pipelines and extents are checked first.
  std::vector<MetalTransferEncoder::TransferRectanglePlan> plans;
  if (!transfer_encoder_->BuildTransferRectanglePlans(dest_key, transfers,
                                                      nullptr, true, plans)) {
    return false;
  }
  for (const MetalTransferEncoder::TransferRectanglePlan& plan : plans) {
    for (uint32_t i = 0; i < plan.rectangle_count; ++i) {
      const Transfer::Rectangle& rectangle = plan.rectangles[i];
      if (!rectangle.width_pixels || !rectangle.height_pixels ||
          uint64_t(rectangle.x_pixels) + rectangle.width_pixels >
              old_depth->width() ||
          uint64_t(rectangle.y_pixels) + rectangle.height_pixels >
              old_depth->height()) {
        return false;
      }
    }
    const Transfer& transfer = transfers[plan.transfer_index];
    if (!transfer.source) {
      return false;
    }
    RenderTargetKey host_depth_key;
    if (transfer.host_depth_source) {
      host_depth_key = transfer.host_depth_source->key();
    }
    const EdramTransferShaderKey depth_shader =
        transfer_encoder_->GetTransferShaderKey(
            transfer.source->key(), dest_key,
            transfer.host_depth_source ? &host_depth_key : nullptr, false,
            false, 0, transfer);
    if (!transfer_encoder_->GetOrCreateTransferPipelines(
            depth_shader, depth_format, false, true, 0, nullptr, depth_format,
            GetStencilAttachmentPixelFormat(depth_format))) {
      return false;
    }
  }
  MTL::Texture* spare = dest->spare_depth_texture();
  if (spare) {
    return spare->pixelFormat() == old_depth->pixelFormat() &&
           spare->textureType() == old_depth->textureType() &&
           spare->width() == old_depth->width() &&
           spare->height() == old_depth->height() &&
           spare->sampleCount() == old_depth->sampleCount();
  }
  MTL::Texture* spare_stencil = nullptr;
  spare = CreateDepthTexture(
      uint32_t(old_depth->width()), uint32_t(old_depth->height()),
      dest_key.GetDepthFormat(), MsaaSamplesToCount(dest_key.msaa_samples),
      &spare_stencil);
  if (!spare) {
    return false;
  }
  dest->SetSpareDepthTexture(spare, spare_stencil);
  return true;
}

bool MetalRenderTargetCache::RotateHostDepthForDrawPass(
    RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers) {
  if (pending_draw_pass_rotated_depth_ || !render_targets ||
      !render_targets[0] || !render_target_transfers) {
    return false;
  }
  auto* dest = static_cast<MetalRenderTarget*>(render_targets[0]);
  // Not for strip-merged destinations: drawn at the head of a merged pass,
  // they slowed the vertex stage on a tile GPU, so they keep their own pass.
  if (dest->key().strip_merged) {
    return false;
  }
  // A deferred clear has to land in the previous backing first, which the
  // standalone path encodes before it rotates.
  if (AnyDeferredClear(dest) ||
      !CanRotateHostDepth(1 + xenos::kMaxColorRenderTargets, render_targets,
                          render_target_transfers, false)) {
    return false;
  }
  // A pass still open with the current backing attached would go on writing
  // what the transfers are to read.
  MTL::RenderPassDescriptor* open_descriptor = nullptr;
  uint32_t open_width = 0, open_height = 0;
  MetalBufferHazards::Encoder* open_hazards = nullptr;
  if (command_processor_.GetOpenRenderPass(open_descriptor, open_width,
                                           open_height, open_hazards)) {
    MTL::RenderPassDepthAttachmentDescriptor* depth_attachment =
        open_descriptor->depthAttachment();
    MTL::RenderPassStencilAttachmentDescriptor* stencil_attachment =
        open_descriptor->stencilAttachment();
    if ((depth_attachment && depth_attachment->texture() &&
         depth_attachment->texture() == dest->texture()) ||
        (stencil_attachment && stencil_attachment->texture() &&
         stencil_attachment->texture() == dest->stencil_attachment_texture())) {
      return false;
    }
  }
  if (!AcquireSpareDepthBacking(dest, render_target_transfers[0])) {
    return false;
  }
  dest->SwapDepthBackings();
  MarkContentChanged(dest);
  render_pass_descriptor_dirty_ = true;
  pending_draw_pass_rotated_depth_ = dest;
  return true;
}

bool MetalRenderTargetCache::PrepareUpdateQuadHoldAndTransfers(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  // Transfers held behind a clear quad wait for this draw only if it may be
  // another quad of the same clear into the same targets. Otherwise they go
  // into the open pass now, after the quads, leaving out what those covered.
  if (quad_hold_slot_ != UINT32_MAX) {
    // A quad that never reached NoteDrawWrites was not drawn.
    quad_hold_candidate_valid_ = false;
    quad_hold_chain_update_ = false;
    quad_hold_chain_valid_ = false;
    if (!quad_hold_in_encoder_ || !quad_hold_drawn_valid_ ||
        !clear_quad_draw_.candidate) {
      if (!command_processor_.EncodeHeldDrawPassTransfers()) {
        return false;
      }
    } else if (GetQuadHoldState(is_rasterization_done, normalized_depth_control,
                                normalized_color_mask) != quad_hold_state_) {
      // A quad into other targets, which may read the held one through
      // transfers of its own. Still held while the rule looks at them; the
      // pass that drew the held quads stays open until this draw's begins.
      quad_hold_chain_update_ = true;
    }
  }
  // Reaching another update means the command processor never got to encode
  // the queued transfers into a pass. Their ownership is already transferred,
  // so run them standalone before the base update reshuffles ownership again.
  if (quad_hold_slot_ == UINT32_MAX && !FlushPendingDrawPassTransfers()) {
    return false;
  }
  return true;
}

bool MetalRenderTargetCache::SyncCurrentTargetsFromBaseUpdate(
    RenderTarget* const* accumulated_targets) {
  bool targets_changed = false;

  MetalRenderTarget* new_depth_target =
      accumulated_targets[0]
          ? static_cast<MetalRenderTarget*>(accumulated_targets[0])
          : nullptr;
  if (new_depth_target != current_depth_target_) {
    targets_changed = true;
    current_depth_target_ = new_depth_target;
    if (current_depth_target_) {
      XELOGD(
          "MetalRenderTargetCache::Update - Depth target changed: key={:08X}",
          current_depth_target_->key().key);
    }
  }

  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    MetalRenderTarget* new_color_target =
        accumulated_targets[i + 1]
            ? static_cast<MetalRenderTarget*>(accumulated_targets[i + 1])
            : nullptr;
    if (new_color_target != current_color_targets_[i]) {
      targets_changed = true;
      current_color_targets_[i] = new_color_target;
      if (current_color_targets_[i]) {
        XELOGD(
            "MetalRenderTargetCache::Update - Color target {} changed: "
            "key={:08X}",
            i, current_color_targets_[i]->key().key);
      }
    }
  }

  return targets_changed;
}

bool MetalRenderTargetCache::OptimizeUpdateTransfers(
    RenderTarget* const* accumulated_targets,
    reg::RB_DEPTHCONTROL normalized_depth_control, const Shader& vertex_shader,
    bool targets_changed) {
  // Transfers whose result the destination's backing already holds are
  // dropped before anything queues or draws them.
  ElideRedundantTransfers();
  // Round trips through a color copy that changed only the stencil keep the
  // depth instead of rebuilding it (no host depth copy or backing rotation).
  KeepDepthOfStencilRoundTrips();
  // A held queue can't wait past an update that changes the targets or
  // transfers anything itself.
  if (quad_hold_slot_ != UINT32_MAX && !quad_hold_chain_update_) {
    bool update_transfers_any = targets_changed;
    const std::vector<Transfer>* held_check_transfers = last_update_transfers();
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      update_transfers_any |= !held_check_transfers[i].empty();
    }
    if (update_transfers_any &&
        !command_processor_.EncodeHeldDrawPassTransfers()) {
      return false;
    }
  }
  // Full-target clear quads, and transfers that would only copy a constant.
  pending_constant_ = PendingConstant();
  update_cutout_mask_ = 0;
  update_relies_on_quad_draw_ = false;
  update_cutout_full_overwrite_mask_ = 0;
  update_cutout_quad_slot_ = UINT32_MAX;
  update_targets_changed_ = targets_changed;
  ApplyClearQuadRule(accumulated_targets, normalized_depth_control,
                     vertex_shader);
  // Not another quad of the held clear. A quad whose transfers read the held
  // destination also kills what they leave out of it.
  if (quad_hold_slot_ != UINT32_MAX && !quad_hold_candidate_valid_) {
    Transfer::Rectangle merged;
    if (quad_hold_chain_valid_ &&
        UnionIsRectangle(quad_hold_drawn_, quad_hold_chain_, merged)) {
      quad_hold_drawn_ = merged;
    }
    if (!command_processor_.EncodeHeldDrawPassTransfers()) {
      return false;
    }
  }
  ReplaceConstantSourceTransfers(accumulated_targets);
  clear_quad_draw_ = ClearQuadDraw();
  return true;
}

bool MetalRenderTargetCache::QueueOrPerformUpdateTransfers(
    RenderTarget* const* accumulated_targets, bool is_rasterization_done,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
  const std::vector<Transfer>* update_transfers = last_update_transfers();
  const auto update_cutouts =
      GetTransferCutouts(update_cutout_mask_, update_cutouts_);
  if (!::cvars::metal_transfer_in_draw_pass) {
    PerformTransfersAndResolveClears(
        1 + xenos::kMaxColorRenderTargets, accumulated_targets,
        update_transfers, update_cutouts.data(), nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, update_depth_keep_dest_depth_);
    return true;
  }

  std::array<std::vector<Transfer>, 1 + xenos::kMaxColorRenderTargets>
      fallback_transfers;
  bool fallback_transfer_work = false;
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    const std::vector<Transfer>& transfers = update_transfers[i];
    if (transfers.empty()) {
      continue;
    }
    bool queue_rotated = false;
    bool can_queue =
        CanQueueDrawPassTransfers(i, accumulated_targets, transfers);
    if (!can_queue && !i &&
        CanQueueDrawPassTransfers(i, accumulated_targets, transfers, true) &&
        RotateHostDepthForDrawPass(accumulated_targets, update_transfers)) {
      can_queue = true;
      queue_rotated = true;
    }
    if (!can_queue) {
      fallback_transfers[i] = transfers;
      fallback_transfer_work = true;
      continue;
    }
    pending_draw_pass_render_targets_[i] = accumulated_targets[i];
    pending_draw_pass_transfers_[i] = transfers;
    pending_draw_pass_transfer_mask_ |= uint32_t(1) << i;
    if (i == 0) {
      pending_draw_pass_depth_keep_dest_depth_ = update_depth_keep_dest_depth_;
    }
    if (update_cutout_mask_ & (uint32_t(1) << i)) {
      if (i == update_cutout_quad_slot_) {
        // Held until the quad is drawn, and the quads after it that
        // continue the clear, then encoded with all of them left out.
        quad_hold_slot_ = i;
        // The transfers are encoded after later updates, with the value
        // conversion this draw's blending asks for, not theirs.
        RenderTargetKey no_blend_reading[xenos::kMaxColorRenderTargets];
        for (uint32_t j = 0; j < xenos::kMaxColorRenderTargets; ++j) {
          quad_hold_blend_reading_rts_[j] = RenderTargetKey();
        }
        SwapLastUpdateBlendReadingColorRTs(quad_hold_blend_reading_rts_);
        for (uint32_t j = 0; j < xenos::kMaxColorRenderTargets; ++j) {
          no_blend_reading[j] = quad_hold_blend_reading_rts_[j];
        }
        SwapLastUpdateBlendReadingColorRTs(no_blend_reading);
        quad_hold_in_encoder_ = false;
        quad_hold_candidate_ = update_cutouts_[i];
        quad_hold_candidate_valid_ = true;
        quad_hold_drawn_valid_ = false;
        quad_hold_state_ =
            GetQuadHoldState(is_rasterization_done, normalized_depth_control,
                             normalized_color_mask);
      } else {
        pending_draw_pass_cutout_mask_ |= uint32_t(1) << i;
        pending_draw_pass_cutouts_[i] = update_cutouts_[i];
      }
      if (update_cutout_full_overwrite_mask_ & (uint32_t(1) << i)) {
        pending_draw_pass_full_overwrite_mask_ |= uint32_t(1) << i;
      }
    } else if (PendingDrawPassTransfersFullyOverwriteTarget(
                   i, accumulated_targets[i], transfers,
                   i == 0 && update_depth_keep_dest_depth_)) {
      pending_draw_pass_full_overwrite_mask_ |= uint32_t(1) << i;
    }
    // A rotated-in backing holds nothing: the transfers reach every tile
    // the destination owns (CanRotateHostDepth).
    if (queue_rotated) {
      pending_draw_pass_full_overwrite_mask_ |= uint32_t(1) << i;
    }
    // The transfer draws supersede the clear the descriptor would have done,
    // matching what the standalone path does with the flag. A deferred
    // resolve clear still covers what they don't, so the pass keeps it as
    // its load action and draws the transfers over it.
    auto* dest_metal_rt =
        static_cast<MetalRenderTarget*>(accumulated_targets[i]);
    if (dest_metal_rt->needs_initial_clear() &&
        !dest_metal_rt->clear_deferred()) {
      dest_metal_rt->SetNeedsInitialClear(false);
      render_pass_descriptor_dirty_ = true;
    }
  }
  if (fallback_transfer_work) {
    PerformTransfersAndResolveClears(
        1 + xenos::kMaxColorRenderTargets, accumulated_targets,
        fallback_transfers.data(), update_cutouts.data(), nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, update_depth_keep_dest_depth_);
  }
  // Queue only what the pass the descriptor will describe can actually
  // encode, so eligibility is not re-decided at encode time.
  if (HasPendingDrawPassTransfers()) {
    TransferAttachmentFormats attachment_formats;
    if (!transfer_encoder_->GetCurrentTransferAttachmentFormats(
            attachment_formats) ||
        !PreflightPendingDrawPassTransfers(attachment_formats)) {
      if (!FlushPendingDrawPassTransfers()) {
        return false;
      }
    }
  }
  return true;
}

bool MetalRenderTargetCache::Update(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader,
    int32_t window_offset_tiles) {
  SCOPE_profile_cpu_f("gpu");
  // Record what this draw may write, for NoteDrawWrites after encoding.
  const RegisterFile& regs = register_file();
  draw_may_write_depth_ = normalized_depth_control.z_enable &&
                          normalized_depth_control.z_write_enable;
  draw_may_write_stencil_ =
      normalized_depth_control.stencil_enable &&
      (regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask ||
       (normalized_depth_control.backface_enable &&
        regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF)
            .stencilwritemask));
  draw_color_write_mask_ = normalized_color_mask;
  clear_quad_written_mask_ = 0;

  if (!PrepareUpdateQuadHoldAndTransfers(is_rasterization_done,
                                         normalized_depth_control,
                                         normalized_color_mask)) {
    return false;
  }

  // The clear quad rule reads what this update's extent estimate saw.
  draw_extent_estimator().ResetLastDraw();

  if (!RenderTargetCache::Update(
          is_rasterization_done, normalized_depth_control,
          normalized_color_mask, vertex_shader, window_offset_tiles)) {
    XELOGE("MetalRenderTargetCache::Update - Base class Update failed");
    return false;
  }
  if (last_update_depth_clear_retargeted()) {
    // The draw's depth is in the tiles' owner (PerformDepthClearRetarget) and
    // the draw is dropped: nothing of this update is bound or transferred.
    clear_quad_draw_ = ClearQuadDraw();
    draw_may_write_depth_ = false;
    draw_may_write_stencil_ = false;
    draw_color_write_mask_ = 0;
    return true;
  }

  if (::cvars::metal_memory_log_rate > 0) {
    static uint64_t memory_log_counter = 0;
    if ((++memory_log_counter % uint64_t(::cvars::metal_memory_log_rate)) ==
        0) {
      XELOGI(
          "Metal mem: frame={} rt={} map={} dummy={} pipelines={} "
          "transfer_shaders={}",
          frame_id_, render_target_map_.size(), render_target_map_.size(),
          dummy_color_targets_.size(),
          transfer_encoder_->transfer_pipeline_count(),
          transfer_encoder_->transfer_fragment_function_count());
    }
  }

  RenderTarget* const* accumulated_targets =
      last_update_accumulated_render_targets();
  const bool targets_changed =
      SyncCurrentTargetsFromBaseUpdate(accumulated_targets);

  if (!OptimizeUpdateTransfers(accumulated_targets, normalized_depth_control,
                               vertex_shader, targets_changed)) {
    return false;
  }
  if (!QueueOrPerformUpdateTransfers(accumulated_targets, is_rasterization_done,
                                     normalized_depth_control,
                                     normalized_color_mask)) {
    return false;
  }

  if (targets_changed) {
    render_pass_descriptor_dirty_ = true;
    // A different attachment set starts a pass of its own, with nothing an
    // earlier encoder left in these attachments to preserve.
    render_pass_encoder_created_since_targets_changed_ = false;
  }

  return true;
}

void MetalRenderTargetCache::SetCachedRenderPassLoadActions(
    uint32_t attachment_mask, MTL::LoadAction load_action) {
  if (!attachment_mask || !cached_render_pass_descriptor_) {
    return;
  }
  if (attachment_mask & 1) {
    if (auto* depth_attachment =
            cached_render_pass_descriptor_->depthAttachment()) {
      depth_attachment->setLoadAction(load_action);
    }
    if (auto* stencil_attachment =
            cached_render_pass_descriptor_->stencilAttachment()) {
      stencil_attachment->setLoadAction(load_action);
    }
  }
  if (auto* color_attachments =
          cached_render_pass_descriptor_->colorAttachments()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(attachment_mask & (uint32_t(2) << i))) {
        continue;
      }
      if (auto* color_attachment = color_attachments->object(i)) {
        color_attachment->setLoadAction(load_action);
      }
    }
  }
}

uint32_t MetalRenderTargetCache::GetPendingDrawPassLoadDontCareMask() {
  // An encoder already wrote these attachments, so a reopen has to load them.
  if (render_pass_encoder_created_since_targets_changed_ ||
      !HasPendingDrawPassTransfers()) {
    return 0;
  }
  TransferAttachmentFormats attachment_formats;
  if (!transfer_encoder_->GetCurrentTransferAttachmentFormats(
          attachment_formats) ||
      !PreflightPendingDrawPassTransfers(attachment_formats)) {
    return 0;
  }
  return pending_draw_pass_transfer_mask_ &
         pending_draw_pass_full_overwrite_mask_;
}

void MetalRenderTargetCache::ApplyPendingDrawPassLoadActions() {
  uint32_t dontcare_mask = GetPendingDrawPassLoadDontCareMask();
  SetCachedRenderPassLoadActions(
      pending_draw_pass_load_dontcare_mask_ & ~dontcare_mask,
      MTL::LoadActionLoad);
  SetCachedRenderPassLoadActions(
      dontcare_mask & ~pending_draw_pass_load_dontcare_mask_,
      MTL::LoadActionDontCare);
  pending_draw_pass_load_dontcare_mask_ = dontcare_mask;
}

void MetalRenderTargetCache::ClearPendingDrawPassTransfers() {
  // A DontCare load action only holds for the one pass that also encodes the
  // transfers. Patch the descriptor back in place rather than dirtying it: a
  // rebuild would hand the command processor a new descriptor and cost the
  // encoder restart this whole path exists to avoid, and an encoder already
  // created from it has its own copy of the load actions.
  SetCachedRenderPassLoadActions(pending_draw_pass_load_dontcare_mask_,
                                 MTL::LoadActionLoad);

  for (auto& transfers : pending_draw_pass_transfers_) {
    transfers.clear();
  }
  pending_draw_pass_render_targets_.fill(nullptr);
  pending_draw_pass_transfer_mask_ = 0;
  pending_draw_pass_full_overwrite_mask_ = 0;
  pending_draw_pass_rotated_depth_ = nullptr;
  pending_draw_pass_load_dontcare_mask_ = 0;
  pending_draw_pass_cutout_mask_ = 0;
  pending_draw_pass_depth_keep_dest_depth_ = false;
  ResetQuadHold();
  released_quad_dest_ = nullptr;
  released_quad_constant_valid_ = false;
}

bool MetalRenderTargetCache::CanQueueDrawPassTransfers(
    uint32_t render_target_index, RenderTarget* const* render_targets,
    const std::vector<Transfer>& transfers, bool dest_rotated) const {
  if (!render_targets || transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return false;
  }
  auto* dest_metal_rt =
      static_cast<MetalRenderTarget*>(render_targets[render_target_index]);
  if (!dest_metal_rt) {
    return false;
  }
  RenderTargetKey dest_key = dest_metal_rt->key();
  if (dest_key.is_depth != (render_target_index == 0)) {
    return false;
  }

  // The transfer draws have to write through the very texture the pass binds.
  MTL::Texture* dest_draw_texture = dest_metal_rt->texture();
  bool dest_is_uint = false;
  if (dest_key.is_depth) {
    if (!dest_draw_texture ||
        dest_draw_texture->pixelFormat() !=
            GetDepthPixelFormat(dest_key.GetDepthFormat())) {
      return false;
    }
  } else {
    MTL::PixelFormat transfer_format = GetColorOwnershipTransferPixelFormat(
        dest_key.GetColorFormat(), &dest_is_uint);
    if (dest_is_uint && MetalTransferEncoder::IsColorTransferFloatBitsExact(
                            dest_key.GetColorFormat())) {
      // Written as bits through the float view the pass binds.
      if (!dest_draw_texture ||
          dest_draw_texture->pixelFormat() !=
              GetColorResourcePixelFormat(dest_key.GetColorFormat())) {
        return false;
      }
    } else if (dest_is_uint || !dest_draw_texture ||
               dest_draw_texture != dest_metal_rt->transfer_texture() ||
               GetColorResourcePixelFormat(dest_key.GetColorFormat()) !=
                   transfer_format ||
               dest_draw_texture->pixelFormat() != transfer_format) {
      return false;
    }
  }

  // Sampling a texture the same pass has bound as an attachment is undefined,
  // so anything the draw pass will hold has to stay out of the sources.
  auto is_active_draw_pass_texture = [&](const MetalRenderTarget* rt,
                                         MTL::Texture* texture) -> bool {
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      auto* active_rt = static_cast<MetalRenderTarget*>(render_targets[i]);
      if (!active_rt) {
        continue;
      }
      MTL::Texture* active_draw_texture = active_rt->texture();
      if (rt == active_rt || texture == active_draw_texture ||
          (rt && rt->texture() == active_draw_texture)) {
        return true;
      }
    }
    return false;
  };

  for (const Transfer& transfer : transfers) {
    if (!transfer.source) {
      return false;
    }
    auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
    if (source_rt == dest_metal_rt) {
      return false;
    }
    // A deferred clear of a source has to be encoded before the pass that
    // reads it, which the standalone path does.
    if (AnyDeferredClear(source_rt) ||
        AnyDeferredClear(transfer.host_depth_source)) {
      return false;
    }
    RenderTargetKey source_key = source_rt->key();
    // Halo 3 (and possibly other games) drops 8x4 pixel blocks stochastically
    // when depth-to-color transfers run at the head of a draw pass targeting
    // color attachment 1+ (render_target_index >= 2). Keep the exact narrow
    // workaround so color attachment 0 doesn't pay extra standalone passes
    // until root-caused.
    if (source_key.is_depth && !dest_key.is_depth && render_target_index >= 2) {
      return false;
    }
    if (transfer.host_depth_source) {
      if (!dest_key.is_depth) {
        return false;
      }
      auto* host_depth_rt =
          static_cast<MetalRenderTarget*>(transfer.host_depth_source);
      if (!host_depth_rt) {
        return false;
      }
      // A host depth source that is the destination itself goes through the
      // EDRAM round trip, which needs a compute pass of its own, unless the
      // destination is rotated to its spare backing first, leaving the
      // previous one to read as an ordinary texture no pass binds.
      if (host_depth_rt == dest_metal_rt && !dest_rotated) {
        return false;
      }
      RenderTargetKey host_depth_key = host_depth_rt->key();
      // The destination's texture() is the backing the pass will bind; the
      // previous one, read as host depth, is checked when rotated.
      if (host_depth_rt == dest_metal_rt) {
        if (!host_depth_key.is_depth) {
          return false;
        }
      } else {
        MTL::Texture* host_depth_texture = host_depth_rt->texture();
        if (!host_depth_key.is_depth || !host_depth_texture ||
            host_depth_texture->pixelFormat() !=
                GetDepthPixelFormat(host_depth_key.GetDepthFormat()) ||
            is_active_draw_pass_texture(host_depth_rt, host_depth_texture)) {
          return false;
        }
      }
    }

    MTL::Texture* source_texture = source_key.is_depth
                                       ? source_rt->texture()
                                       : source_rt->transfer_texture();
    if (!source_texture) {
      return false;
    }
    MTL::PixelFormat source_format =
        source_key.is_depth ? GetDepthPixelFormat(source_key.GetDepthFormat())
                            : GetColorOwnershipTransferPixelFormat(
                                  source_key.GetColorFormat(), nullptr);
    if (source_texture->pixelFormat() != source_format ||
        is_active_draw_pass_texture(source_rt, source_texture)) {
      return false;
    }
  }

  std::vector<MetalTransferEncoder::TransferRectanglePlan> transfer_rectangles;
  return transfer_encoder_->BuildTransferRectanglePlans(
      dest_key, transfers, nullptr, true, transfer_rectangles);
}

bool MetalRenderTargetCache::PendingDrawPassTransfersFullyOverwriteTarget(
    uint32_t render_target_index, RenderTarget* render_target,
    const std::vector<Transfer>& transfers, bool keep_dest_depth) const {
  if (!render_target || transfers.empty() ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return false;
  }

  auto* dest_metal_rt = static_cast<MetalRenderTarget*>(render_target);
  RenderTargetKey dest_key = dest_metal_rt->key();
  MTL::Texture* dest_texture = dest_metal_rt->texture();
  if (!dest_texture) {
    return false;
  }
  // A stencil-only transfer keeps the depth, which has to be loaded.
  if (keep_dest_depth) {
    return false;
  }
  uint32_t dest_width = uint32_t(dest_texture->width());
  uint32_t dest_height = uint32_t(dest_texture->height());
  if (!dest_width || !dest_height) {
    return false;
  }

  std::vector<MetalTransferEncoder::TransferRectanglePlan> transfer_rectangles;
  if (!transfer_encoder_->BuildTransferRectanglePlans(
          dest_key, transfers, nullptr, true, transfer_rectangles) ||
      transfer_rectangles.size() != transfers.size()) {
    return false;
  }
  for (const MetalTransferEncoder::TransferRectanglePlan& transfer_plan :
       transfer_rectangles) {
    if (transfer_plan.rectangle_count != 1) {
      return false;
    }
    const Transfer::Rectangle& rect = transfer_plan.rectangles[0];
    if (rect.x_pixels || rect.y_pixels ||
        rect.width_pixels * draw_resolution_scale_x() < dest_width ||
        rect.height_pixels * draw_resolution_scale_y() < dest_height) {
      return false;
    }
  }
  return true;
}

bool MetalRenderTargetCache::PreflightPendingDrawPassTransfers(
    const TransferAttachmentFormats& attachment_formats) {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }

  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_transfer_mask_ & (uint32_t(1) << i))) {
      continue;
    }
    auto* dest_metal_rt =
        static_cast<MetalRenderTarget*>(pending_draw_pass_render_targets_[i]);
    if (!dest_metal_rt || pending_draw_pass_transfers_[i].empty()) {
      return false;
    }

    RenderTargetKey dest_key = dest_metal_rt->key();
    uint32_t dest_sample_count = MsaaSamplesToCount(dest_key.msaa_samples);
    bool dest_is_uint = false;
    bool dest_float_bits = false;
    uint32_t color_attachment_index = 0;
    MTL::PixelFormat dest_format = MTL::PixelFormatInvalid;
    if (dest_key.is_depth) {
      dest_format = GetDepthPixelFormat(dest_key.GetDepthFormat());
      // Every depth destination also runs the stencil clear and the stencil
      // draws, which may fall back to the per-bit states at encode time.
      if (i != 0 || attachment_formats.depth_attachment_format != dest_format ||
          !transfer_encoder_->GetTransferDepthStencilState(true) ||
          !transfer_encoder_->GetTransferStencilClearState() ||
          !transfer_encoder_->GetOrCreateTransferClearPipeline(
              dest_format, false, true, dest_sample_count, 0,
              &attachment_formats.color_attachment_formats,
              attachment_formats.depth_attachment_format,
              attachment_formats.stencil_attachment_format)) {
        return false;
      }
      for (uint32_t bit = 0; bit < 8; ++bit) {
        if (!transfer_encoder_->GetTransferStencilBitState(bit)) {
          return false;
        }
      }
      if (attachment_formats.stencil_attachment_format !=
          GetStencilAttachmentPixelFormat(dest_format)) {
        return false;
      }
    } else {
      if (!i) {
        return false;
      }
      color_attachment_index = i - 1;
      dest_format = GetColorOwnershipTransferPixelFormat(
          dest_key.GetColorFormat(), &dest_is_uint);
      if (dest_is_uint && MetalTransferEncoder::IsColorTransferFloatBitsExact(
                              dest_key.GetColorFormat())) {
        dest_float_bits = true;
        dest_is_uint = false;
        dest_format = GetColorResourcePixelFormat(dest_key.GetColorFormat());
      }
      if (dest_is_uint ||
          attachment_formats.color_attachment_formats[color_attachment_index] !=
              dest_format ||
          !transfer_encoder_->GetTransferNoDepthStencilState()) {
        return false;
      }
    }

    for (const Transfer& transfer : pending_draw_pass_transfers_[i]) {
      auto* source_rt = static_cast<MetalRenderTarget*>(transfer.source);
      if (!source_rt) {
        return false;
      }
      RenderTargetKey source_key = source_rt->key();
      if (source_key.is_depth && !GetStencilTextureView(source_rt)) {
        return false;
      }
      RenderTargetKey host_depth_key;
      bool has_host_depth = transfer.host_depth_source != nullptr;
      if (has_host_depth) {
        host_depth_key =
            static_cast<MetalRenderTarget*>(transfer.host_depth_source)->key();
      }
      // CanQueueDrawPassTransfers rejected a host depth source that is the
      // destination unless rotated, which reads its previous backing as a
      // texture, so the copy-through-EDRAM mode never appears here.
      // Mirror the encode side: with the stencil folded into the depth draw
      // there is no stencil-bit pass to build pipelines for.
      bool native_stencil_output =
          dest_key.is_depth &&
          transfer_encoder_->UseNativeStencilOutputInTransfers();
      uint32_t stencil_bit_passes =
          (dest_key.is_depth && !native_stencil_output) ? 1u : 0u;
      for (uint32_t stencil_bit = 0; stencil_bit <= stencil_bit_passes;
           ++stencil_bit) {
        EdramTransferShaderKey shader_key =
            transfer_encoder_->GetTransferShaderKey(
                source_key, dest_key,
                (has_host_depth && !stencil_bit) ? &host_depth_key : nullptr,
                false, stencil_bit != 0, color_attachment_index, transfer);
        shader_key.dest_float_bits = dest_float_bits ? 1 : 0;
        if (!transfer_encoder_->GetOrCreateTransferPipelines(
                shader_key, dest_format, dest_is_uint,
                native_stencil_output && !stencil_bit, color_attachment_index,
                &attachment_formats.color_attachment_formats,
                attachment_formats.depth_attachment_format,
                attachment_formats.stencil_attachment_format)) {
          return false;
        }
      }
    }
  }

  return true;
}

bool MetalRenderTargetCache::PreflightPendingDrawPassTransfers(
    MTL::RenderPassDescriptor* pass_descriptor) {
  TransferAttachmentFormats attachment_formats;
  if (!transfer_encoder_->GetActiveTransferAttachmentFormats(
          pass_descriptor, attachment_formats)) {
    return false;
  }
  return PreflightPendingDrawPassTransfers(attachment_formats);
}

bool MetalRenderTargetCache::PendingDrawPassTransfersPrepareable() const {
  // Attachment zero is depth/stencil; color attachments start at one. A queue
  // into depth alone is admitted whatever its sources.
  if (pending_draw_pass_transfer_mask_ <= 1) {
    return true;
  }
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_transfer_mask_ & (1u << i))) {
      continue;
    }
    const auto* target = pending_draw_pass_render_targets_[i];
    if (!target || target->key().is_depth ||
        pending_draw_pass_transfers_[i].empty()) {
      return false;
    }
    for (const Transfer& transfer : pending_draw_pass_transfers_[i]) {
      if (!transfer.source || transfer.source->key().is_depth ||
          transfer.host_depth_source) {
        return false;
      }
    }
  }
  return true;
}

bool MetalRenderTargetCache::EncodePendingDrawPassTransfers(
    MTL::RenderCommandEncoder* encoder,
    MTL::RenderPassDescriptor* pass_descriptor,
    DrawPassTransferEncoderMutationMask* mutations_out) {
  if (mutations_out) {
    *mutations_out = kDrawPassTransferEncoderMutationNone;
  }
  ReleaseHeldDrawPassTransfers();
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!encoder || !PreflightPendingDrawPassTransfers(pass_descriptor)) {
    return false;
  }
  if (!PerformTransfersAndResolveClears(
          1 + xenos::kMaxColorRenderTargets,
          pending_draw_pass_render_targets_.data(),
          pending_draw_pass_transfers_.data(),
          GetTransferCutouts(pending_draw_pass_cutout_mask_,
                             pending_draw_pass_cutouts_)
              .data(),
          nullptr, nullptr, nullptr, encoder, pass_descriptor, mutations_out,
          pending_draw_pass_depth_keep_dest_depth_)) {
    return false;
  }
  ++pass_stats_.in_pass_transfer_flushes;
  NoteReleasedQuadWrites();
  ClearPendingDrawPassTransfers();
  return true;
}

bool MetalRenderTargetCache::FlushPendingDrawPassTransfers() {
  // Standalone, after whatever quads the open pass already holds.
  ReleaseHeldDrawPassTransfers();
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!PerformTransfersAndResolveClears(
          1 + xenos::kMaxColorRenderTargets,
          pending_draw_pass_render_targets_.data(),
          pending_draw_pass_transfers_.data(),
          GetTransferCutouts(pending_draw_pass_cutout_mask_,
                             pending_draw_pass_cutouts_)
              .data(),
          nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
          pending_draw_pass_depth_keep_dest_depth_)) {
    return false;
  }
  ++pass_stats_.standalone_transfer_flushes;
  NoteReleasedQuadWrites();
  ClearPendingDrawPassTransfers();
  return true;
}

bool MetalRenderTargetCache::HoldDrawPassTransfersForQuad() {
  if (quad_hold_slot_ == UINT32_MAX || !quad_hold_candidate_valid_ ||
      !HasPendingDrawPassTransfers()) {
    return false;
  }
  quad_hold_in_encoder_ = true;
  return true;
}

bool MetalRenderTargetCache::ReleaseHeldDrawPassTransfers() {
  if (quad_hold_slot_ == UINT32_MAX) {
    return false;
  }
  const bool in_encoder = quad_hold_in_encoder_;
  // The quads leave their union out of the held target's transfers.
  const uint32_t hold_bit = uint32_t(1) << quad_hold_slot_;
  if (quad_hold_drawn_valid_) {
    pending_draw_pass_cutout_mask_ |= hold_bit;
    pending_draw_pass_cutouts_[quad_hold_slot_] = quad_hold_drawn_;
  } else {
    pending_draw_pass_cutout_mask_ &= ~hold_bit;
  }
  released_quad_dest_ = nullptr;
  released_quad_constant_valid_ = false;
  if (quad_hold_drawn_valid_) {
    released_quad_dest_ = static_cast<MetalRenderTarget*>(
        pending_draw_pass_render_targets_[quad_hold_slot_]);
    if (const ConstantContent* constant =
            released_quad_dest_ ? FindConstantContent(released_quad_dest_)
                                : nullptr) {
      released_quad_constant_valid_ = true;
      released_quad_constant_value_ = constant->value;
      released_quad_constant_rectangle_ = constant->rectangle;
    }
  }
  ResetQuadHold();
  return in_encoder;
}

void MetalRenderTargetCache::NoteReleasedQuadWrites() {
  if (!released_quad_dest_) {
    return;
  }
  // The transfers left the quads' pixels alone, so a constant they wrote
  // still holds.
  MarkContentChanged(released_quad_dest_);
  if (released_quad_constant_valid_) {
    SetConstantContent(released_quad_dest_, released_quad_constant_value_,
                       released_quad_constant_rectangle_);
  }
  released_quad_dest_ = nullptr;
  released_quad_constant_valid_ = false;
}

bool MetalRenderTargetCache::BeginHeldTransferEncode() {
  if (quad_hold_slot_ == UINT32_MAX) {
    return false;
  }
  SwapLastUpdateBlendReadingColorRTs(quad_hold_blend_reading_rts_);
  return true;
}

void MetalRenderTargetCache::EndHeldTransferEncode() {
  SwapLastUpdateBlendReadingColorRTs(quad_hold_blend_reading_rts_);
}

void MetalRenderTargetCache::ResetQuadHold() {
  quad_hold_chain_update_ = false;
  quad_hold_chain_valid_ = false;
  quad_hold_slot_ = UINT32_MAX;
  quad_hold_in_encoder_ = false;
  quad_hold_candidate_valid_ = false;
  quad_hold_drawn_valid_ = false;
}

std::array<uint32_t, 9> MetalRenderTargetCache::GetQuadHoldState(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) const {
  const RegisterFile& regs = register_file();
  return {regs[XE_GPU_REG_RB_SURFACE_INFO],
          regs[XE_GPU_REG_RB_COLOR_INFO],
          regs[XE_GPU_REG_RB_COLOR1_INFO],
          regs[XE_GPU_REG_RB_COLOR2_INFO],
          regs[XE_GPU_REG_RB_COLOR3_INFO],
          regs[XE_GPU_REG_RB_DEPTH_INFO],
          regs[XE_GPU_REG_RB_MODECONTROL],
          normalized_color_mask,
          uint32_t(normalized_depth_control.z_enable) |
              (uint32_t(normalized_depth_control.stencil_enable) << 1) |
              (uint32_t(is_rasterization_done) << 2)};
}

uint32_t MetalRenderTargetCache::SetClearQuadLoadActions(
    MTL::RenderPassDescriptor* pass_descriptor) {
  const uint32_t written_mask = clear_quad_written_mask_;
  // Queued transfers encoded into the pass may fail and drop it before the
  // quad is drawn.
  if (!clear_quad_pass_begin_ || !written_mask || !pass_descriptor ||
      pass_descriptor != cached_render_pass_descriptor_ ||
      HasPendingDrawPassTransfers()) {
    return 0;
  }
  const Transfer::Rectangle& rectangle = clear_quad_rectangle_;
  if (rectangle.x_pixels || rectangle.y_pixels) {
    return 0;
  }
  // Load and store actions apply to the whole render area.
  uint32_t area_width = uint32_t(pass_descriptor->renderTargetWidth());
  uint32_t area_height = uint32_t(pass_descriptor->renderTargetHeight());
  auto constrain = [&](MTL::RenderPassAttachmentDescriptor* attachment) {
    if (MTL::Texture* texture = attachment->texture()) {
      area_width = area_width ? std::min(area_width, uint32_t(texture->width()))
                              : uint32_t(texture->width());
      area_height = area_height
                        ? std::min(area_height, uint32_t(texture->height()))
                        : uint32_t(texture->height());
    }
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    constrain(pass_descriptor->colorAttachments()->object(i));
  }
  constrain(pass_descriptor->depthAttachment());
  constrain(pass_descriptor->stencilAttachment());
  const uint64_t covered_width =
      uint64_t(rectangle.width_pixels) * draw_resolution_scale_x();
  const uint64_t covered_height =
      uint64_t(rectangle.height_pixels) * draw_resolution_scale_y();
  if (!area_width || !area_height) {
    return 0;
  }
  // The pass may only skip the load when the rectangle covers the whole
  // render area: an attachment that is not loaded may be discarded whole
  // (MTLLoadActionDontCare), and constraining the render target to the
  // rectangle keeps nothing, as the GPU stores the tiles the rectangle
  // touches with arbitrary data in their pixels outside it.
  if (covered_width < area_width || covered_height < area_height) {
    return 0;
  }
  // Apple GPUs load and store whole tiles. Where the render area ends inside a
  // tile, a DontCare load leaves the tile's samples beyond the area undefined,
  // and the store writes them over what the attachment held there. So the
  // area has to end on tile boundaries, multiples of 32 pixels (the largest
  // tile edge), or at the edges of the attachments.
  const uint64_t pass_width = area_width;
  const uint64_t pass_height = area_height;
  uint64_t attachments_width = 0, attachments_height = 0;
  auto extend = [&](MTL::RenderPassAttachmentDescriptor* attachment) {
    if (MTL::Texture* texture = attachment->texture()) {
      attachments_width =
          std::max<uint64_t>(attachments_width, texture->width());
      attachments_height =
          std::max<uint64_t>(attachments_height, texture->height());
    }
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    extend(pass_descriptor->colorAttachments()->object(i));
  }
  extend(pass_descriptor->depthAttachment());
  extend(pass_descriptor->stencilAttachment());
  constexpr uint64_t kTileEdgeAlignment = 32;
  if ((pass_width % kTileEdgeAlignment && pass_width < attachments_width) ||
      (pass_height % kTileEdgeAlignment && pass_height < attachments_height)) {
    return 0;
  }
  uint32_t mask = 0;
  for (uint32_t i = 0; i <= xenos::kMaxColorRenderTargets; ++i) {
    if (!(written_mask & (uint32_t(1) << i))) {
      continue;
    }
    MTL::RenderPassAttachmentDescriptor* attachment =
        i ? static_cast<MTL::RenderPassAttachmentDescriptor*>(
                pass_descriptor->colorAttachments()->object(i - 1))
          : pass_descriptor->depthAttachment();
    MTL::Texture* texture = attachment ? attachment->texture() : nullptr;
    if (!texture || texture != clear_quad_textures_[i] ||
        attachment->loadAction() != MTL::LoadActionLoad) {
      continue;
    }
    if (!i) {
      // Stencil, attached along with depth (in its own texture), has to be
      // written in full too, which the rule demands of a depth quad, and is
      // switched along with depth.
      MTL::RenderPassStencilAttachmentDescriptor* stencil_attachment =
          pass_descriptor->stencilAttachment();
      if (stencil_attachment->texture() &&
          stencil_attachment->loadAction() != MTL::LoadActionLoad) {
        continue;
      }
    }
    mask |= uint32_t(1) << i;
  }
  if (!mask) {
    return 0;
  }
  clear_quad_saved_area_width_ = uint32_t(pass_descriptor->renderTargetWidth());
  clear_quad_saved_area_height_ =
      uint32_t(pass_descriptor->renderTargetHeight());
  SetCachedRenderPassLoadActions(mask, MTL::LoadActionDontCare);
  return mask;
}

void MetalRenderTargetCache::RestoreClearQuadLoadActions(
    uint32_t attachment_mask) {
  if (!attachment_mask || !cached_render_pass_descriptor_) {
    return;
  }
  SetCachedRenderPassLoadActions(attachment_mask, MTL::LoadActionLoad);
  cached_render_pass_descriptor_->setRenderTargetWidth(
      clear_quad_saved_area_width_);
  cached_render_pass_descriptor_->setRenderTargetHeight(
      clear_quad_saved_area_height_);
}

bool MetalRenderTargetCache::Resolve(Memory& memory, uint32_t& written_address,
                                     uint32_t& written_length,
                                     MTL::CommandBuffer* command_buffer) {
  SCOPE_profile_cpu_f("gpu");
  written_address = 0;
  written_length = 0;
  const RegisterFile& regs = register_file();
  draw_util::ResolveInfo resolve_info;

  // Fixed16 formats may be truncated to -1..1 when backed by SNORM.
  bool fixed_rg16_trunc = IsFixedRG16TruncatedToMinus1To1();
  bool fixed_rgba16_trunc = IsFixedRGBA16TruncatedToMinus1To1();

  bool resolve_info_valid =
      trace_writer_ &&
      draw_util::GetResolveInfo(
          regs, memory, *trace_writer_, draw_resolution_scale_x(),
          draw_resolution_scale_y(), fixed_rg16_trunc, fixed_rgba16_trunc,
          resolve_info, strip_merge_resolve_bottom(),
          strip_merge_resolve_clear_bottom());
  return ResolveWithInfo(resolve_info, resolve_info_valid, written_address,
                         written_length, command_buffer);
}

bool MetalRenderTargetCache::TryResolveDirectly(
    const draw_util::ResolveInfo& resolve_info,
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_constants,
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, uint32_t resolve_width, uint32_t resolve_height,
    uint32_t group_count_x, uint32_t group_count_y, bool is_depth,
    bool draw_resolution_scaled, bool dest_rows_alias,
    MTL::CommandBuffer* command_buffer, bool& tile_pass_open,
    bool transfers_flushed, bool& clear_left_to_pass_end,
    MetalTextureCache::ResolveTextureRefreshTarget& refresh_target) {
  auto end_pass = [&]() {
    if (post_pass_resolves_.empty()) {
      PlanResolveStoreActions(resolve_info);
    } else {
      KeepStoresOfResolveReads(resolve_info);
    }
    command_processor_.EndRenderEncoder();
    dead_store_render_targets_.clear();
    if (transfers_flushed) {
      PerformDeferredClears(command_buffer);
    }
  };
  auto end_tile_pass = [&](const char* reason) {
    if (!tile_pass_open) {
      return;
    }
    ++pass_stats_.tile_resolve_rejections[reason ? reason : "unknown"];
    tile_pass_open = false;
    end_pass();
  };

  if (!::cvars::direct_host_resolve || draw_resolution_scaled ||
      dest_rows_alias ||
      GetDirectResolveEligibility(resolve_info, copy_shader, true,
                                  /*allow_cross_depth_color=*/true,
                                  /*allow_full32_rgb10f_1x=*/true,
                                  /*allow_full64_rgba16_1x=*/true,
                                  /*allow_full64_rgb10f_rgba16_1x=*/true,
                                  /*allow_samples_as_pixels=*/true,
                                  /*allow_full64_rgb10f_rgba16_average=*/true,
                                  /*allow_full32_1x_conversion=*/true) !=
          DirectResolveEligibility::kEligible) {
    end_tile_pass(draw_resolution_scaled ? "scaled" : "not_direct");
    return false;
  }

  const auto& dest_info = resolve_info.copy_dest_info;
  const auto& dest_coordinates = resolve_info.copy_dest_coordinate_info;
  auto* texture_cache = command_processor_.texture_cache();
  // The converting copies that apply the exponent bias themselves, before
  // packing what the refresh writes.
  const bool refresh_exp_bias =
      MetalResolveEncoder::SupportsDirectResolveRefreshExpBias(copy_shader,
                                                               copy_constants);
  if (texture_cache &&
      // Four-sample cache reuse exposes helper-lane derivative differences
      // unless guest fragments keep their helpers within each primitive.
      (!MetalResolveEncoder::IsFull32RGB10FourSampleAverage(copy_shader,
                                                            copy_constants) ||
       command_processor_.primitive_fragment_helpers_enabled()) &&
      (!dest_info.copy_dest_exp_bias || refresh_exp_bias) &&
      uint32_t(dest_info.copy_dest_endian) <=
          uint32_t(xenos::Endian::k16in32) &&
      !dest_coordinates.offset_x_div_8 && !dest_coordinates.offset_y_div_8) {
    texture_cache->PrepareDirectResolveTextureRefresh(
        dest_info.copy_dest_format, resolve_info.copy_dest_extent_start,
        resolve_info.copy_dest_extent_length, resolve_info.copy_dest_base,
        resolve_width, resolve_height,
        dest_coordinates.pitch_aligned_div_32 << 5,
        xenos::Endian(dest_info.copy_dest_endian), is_depth, refresh_target);
  }
  std::array<MTL::Texture*,
             MetalTextureCache::ResolveTextureRefreshTarget::kMaxAliases + 1>
      refresh_views{};
  for (uint32_t i = 0; i < refresh_target.count; ++i) {
    refresh_views[i] = refresh_target.destinations[i].write_texture.get();
  }

  bool resolved_directly = false;
  if (tile_pass_open) {
    PostPassRefreshCopy refresh_copy;
    bool has_refresh_copy = false;
    const char* tile_fallback =
        refresh_target.transform !=
                MetalTextureCache::ResolveTextureRefreshTarget::Transform::kNone
            ? "refresh_depth"
            : resolve_encoder_.TileResolveInOpenPass(
                  resolve_info, copy_shader, copy_constants, dump_base,
                  dump_row_length_used, dump_rows, dump_pitch,
                  {refresh_views.data(), refresh_target.count},
                  refresh_target.destinations[0].offset_x,
                  refresh_target.destinations[0].offset_y,
                  post_pass_resolves_.size(), &refresh_copy, &has_refresh_copy);
    if (tile_fallback) {
      end_tile_pass(tile_fallback);
    } else {
      ++pass_stats_.tile_resolves;
      if (has_refresh_copy) {
        post_pass_refresh_copies_.push_back(std::move(refresh_copy));
      }
      tile_pass_open = false;
      // Later draws of the pass would sample refreshed textures without a
      // pass end between, and a resolve clear needs a pass of its own after
      // this one, so the pass stays open for further resolves only, and the
      // clear waits for its end.
      const bool clears =
          resolve_info.IsClearingDepth() || resolve_info.IsClearingColor();
      if (!command_processor_.CanKeepRenderPassAfterTileResolve() ||
          (clears && !LeaveResolveClearToPassEnd(resolve_info))) {
        end_pass();
      } else {
        command_processor_.NoteTileResolveWrite(
            resolve_info.copy_dest_extent_start,
            resolve_info.copy_dest_extent_length);
        last_resolve_kept_pass_ = true;
        ++pass_stats_.tile_resolves_kept_pass;
        if (refresh_target || clears) {
          command_processor_.SetRenderEncoderResolveOnly();
          clear_left_to_pass_end = clears;
        }
      }
      resolved_directly = true;
    }
  }
  if (!resolved_directly) {
    resolved_directly = resolve_encoder_.DirectResolveRenderTargets(
        resolve_info, copy_shader, copy_constants, dump_base,
        dump_row_length_used, dump_rows, dump_pitch, group_count_x,
        group_count_y, command_buffer,
        {refresh_views.data(), refresh_target.count},
        refresh_target.destinations[0].offset_x,
        refresh_target.destinations[0].offset_y,
        refresh_target.transform ==
            MetalTextureCache::ResolveTextureRefreshTarget::Transform::
                kDepthFloat24,
        refresh_target.raw_count);
    if (resolved_directly) {
      ++pass_stats_.direct_resolves;
    }
  }
  end_tile_pass(draw_resolution_scaled ? "scaled" : "not_direct");
  return resolved_directly;
}

bool MetalRenderTargetCache::EncodeEdramComputeResolve(
    const draw_util::ResolveInfo& resolve_info,
    draw_util::ResolveCopyShaderIndex copy_shader,
    const draw_util::ResolveCopyShaderConstants& copy_constants,
    uint32_t resolve_height, uint32_t group_count_x, uint32_t group_count_y,
    bool draw_resolution_scaled, bool dest_rows_alias,
    MTL::CommandBuffer* command_buffer) {
  if (!edram_buffer_) {
    return false;
  }
  MTL::ComputePipelineState* pipeline =
      resolve_encoder_.GetEdramResolvePipeline(copy_shader, copy_constants,
                                               draw_resolution_scaled);
  if (draw_resolution_scaled && !pipeline) {
    static uint32_t missing_scaled_pipeline_log_count = 0;
    if (missing_scaled_pipeline_log_count < 8) {
      ++missing_scaled_pipeline_log_count;
      XELOGW("MetalResolve: scaled resolve pipeline missing for shader {}",
             int(copy_shader));
    }
  }
  if (!pipeline || !group_count_x || !group_count_y) {
    return false;
  }

  auto* shared = command_processor_.shared_memory();
  auto* texture_cache = command_processor_.texture_cache();
  MTL::Buffer* dest_buffer = nullptr;
  size_t dest_buffer_offset = 0;
  size_t dest_buffer_length = 0;
  if (draw_resolution_scaled) {
    auto* metal_texture_cache =
        texture_cache ? static_cast<MetalTextureCache*>(texture_cache)
                      : nullptr;
    if (!metal_texture_cache) {
      XELOGE("MetalResolve: missing MetalTextureCache for scaled resolve");
      return false;
    }
    uint32_t range_length = resolve_info.copy_dest_extent_start -
                            resolve_info.copy_dest_base +
                            resolve_info.copy_dest_extent_length;
    if (!metal_texture_cache->EnsureScaledResolveMemoryCommitted(
            resolve_info.copy_dest_extent_start,
            resolve_info.copy_dest_extent_length) ||
        !metal_texture_cache->MakeScaledResolveRangeCurrent(
            resolve_info.copy_dest_base, range_length) ||
        !metal_texture_cache->GetCurrentScaledResolveBuffer(
            dest_buffer, dest_buffer_offset, dest_buffer_length)) {
      XELOGE("MetalResolve: failed to select scaled resolve buffer");
      return false;
    }
    (void)dest_buffer_length;
  } else {
    dest_buffer = shared ? shared->GetBuffer() : nullptr;
    if (!dest_buffer) {
      XELOGE("MetalResolve: missing shared memory buffer");
      return false;
    }
    if (!shared->RequestRange(resolve_info.copy_dest_extent_start,
                              resolve_info.copy_dest_extent_length)) {
      XELOGE(
          "MetalRenderTargetCache::Resolve: RequestRange failed for "
          "0x{:08X} len {}",
          resolve_info.copy_dest_extent_start,
          resolve_info.copy_dest_extent_length);
      return false;
    }
  }

  MTL::CommandQueue* queue = command_processor_.GetMetalCommandQueue();
  if (!queue) {
    XELOGE("MetalRenderTargetCache::Resolve: no command queue for GPU path");
    return false;
  }

  ScopedAutoreleasePool autorelease_pool;
  bool owns_command_buffer = false;
  MTL::CommandBuffer* cmd = command_buffer;
  if (!cmd) {
    cmd = command_processor_.CreateAccountedCommandBuffer(
        MetalCommandProcessor::CommandBufferKind::kRenderTargetResolve);
    if (!cmd) {
      XELOGE(
          "MetalRenderTargetCache::Resolve: failed to get command buffer for "
          "GPU path");
      return false;
    }
    owns_command_buffer = true;
  }
  command_processor_.EndSharedComputeEncoder(cmd);
  MTL::ComputeCommandEncoder* encoder = cmd->computeCommandEncoder();
  if (!encoder) {
    XELOGE(
        "MetalRenderTargetCache::Resolve: failed to get compute encoder for "
        "GPU path");
    if (owns_command_buffer) {
      command_processor_.DiscardAccountedCommandBuffer(
          cmd, MetalCommandProcessor::CommandBufferKind::kRenderTargetResolve);
    }
    return false;
  }

  MetalBufferHazards::Encoder hazards;
  if (shared && shared->hazards() && dest_buffer == shared->GetBuffer()) {
    hazards.Begin(*shared->hazards(), cmd, encoder);
    hazards.Access(resolve_info.copy_dest_extent_start,
                   resolve_info.copy_dest_extent_length, true);
  }
  encoder->setComputePipelineState(pipeline);
  encoder->setBuffer(dest_buffer, dest_buffer_offset, 1);
  encoder->setBuffer(edram_buffer_, 0, 2);
  encoder->useResource(dest_buffer, MTL::ResourceUsageWrite);
  encoder->useResource(edram_buffer_, MTL::ResourceUsageRead);

  if (dest_rows_alias && !draw_resolution_scaled) {
    // The copy writes some destination bytes more than once
    // (CopyDestRowsAlias), and the console's rows below win there, as it
    // resolves rows in order.
    const uint32_t row_height = resolve_info.GetCopyDestMacroTileRowHeight();
    const uint32_t dest_offset_y =
        resolve_info.copy_dest_coordinate_info.offset_y_div_8
        << xenos::kResolveAlignmentPixelsLog2;
    uint32_t row_start = 0;
    while (row_start < resolve_height) {
      uint32_t row_end =
          std::min(resolve_height,
                   (dest_offset_y + row_start) / row_height * row_height +
                       row_height - dest_offset_y);
      draw_util::ResolveCopyShaderConstants band_constants;
      resolve_info.GetCopyShaderConstantsForRows(copy_constants, row_start,
                                                 band_constants);
      if (row_start) {
        encoder->memoryBarrier(MTL::BarrierScopeBuffers);
      }
      encoder->setBytes(&band_constants, sizeof(band_constants), 0);
      encoder->dispatchThreadgroups(
          MTL::Size::Make(
              group_count_x,
              (row_end - row_start) >> xenos::kResolveAlignmentPixelsLog2, 1),
          MTL::Size::Make(8, 8, 1));
      row_start = row_end;
    }
  } else {
    if (draw_resolution_scaled) {
      encoder->setBytes(&copy_constants.dest_relative,
                        sizeof(copy_constants.dest_relative), 0);
    } else {
      encoder->setBytes(&copy_constants, sizeof(copy_constants), 0);
    }
    encoder->dispatchThreadgroups(
        MTL::Size::Make(group_count_x, group_count_y, 1),
        MTL::Size::Make(8, 8, 1));
  }

  hazards.End();
  encoder->endEncoding();
  if (owns_command_buffer) {
    command_processor_.CommitAndAwait(
        cmd, MetalCommandProcessor::GpuWaitReason::kResolve);
  }
  ++pass_stats_.edram_resolves;
  return true;
}

bool MetalRenderTargetCache::ResolveWithInfo(
    draw_util::ResolveInfo resolve_info, bool resolve_info_valid,
    uint32_t& written_address, uint32_t& written_length,
    MTL::CommandBuffer* command_buffer) {
  written_address = 0;
  written_length = 0;
  // Transfers held behind clear quads go into the pass that drew the quads,
  // before the resolve plans its stores.
  if (DrawPassTransfersHeldInPass() &&
      !command_processor_.EncodeHeldDrawPassTransfers()) {
    return false;
  }
  // IssueCopy leaves the guest's pass before the resolve open, so the resolve
  // can read its attachments with a tile dispatch. Every other path ends it
  // first. With what the resolve will read and clear known, the end can drop
  // stores nothing will read.
  bool tile_pass_open = command_processor_.HasActiveRenderEncoder();
  bool transfers_flushed = false;
  last_resolve_kept_pass_ = false;
  auto end_pass = [&]() {
    if (resolve_info_valid) {
      if (post_pass_resolves_.empty()) {
        PlanResolveStoreActions(resolve_info);
      } else {
        // The store plan of the work left to the pass end stands, as the
        // work runs first, except for what this resolve reads after it.
        KeepStoresOfResolveReads(resolve_info);
      }
    }
    command_processor_.EndRenderEncoder();
    // With no pass open, the plan applies to none.
    dead_store_render_targets_.clear();
    if (transfers_flushed) {
      PerformDeferredClears(command_buffer);
    }
  };
  auto end_tile_pass = [&](const char* reason) {
    if (!tile_pass_open) {
      return;
    }
    ++pass_stats_.tile_resolve_rejections[reason ? reason : "unknown"];
    tile_pass_open = false;
    end_pass();
  };
  if (!tile_pass_open) {
    end_pass();
  }
  if (!trace_writer_) {
    XELOGE("MetalRenderTargetCache::Resolve: trace_writer_ is null");
    end_tile_pass("error");
    return false;
  }
  if (!resolve_info_valid) {
    XELOGE("MetalRenderTargetCache::Resolve: GetResolveInfo failed");
    end_tile_pass("error");
    return false;
  }

  // Resolving reads the render targets, so anything still queued for a draw
  // pass has to have been applied to them first.
  if (HasPendingDrawPassTransfers()) {
    end_tile_pass("pending_transfers");
  }
  if (!FlushPendingDrawPassTransfers()) {
    end_tile_pass("error");
    return false;
  }
  transfers_flushed = true;
  // So do the clears deferred to the passes that would bind them. An open
  // pass has done those of its own attachments as its clear load actions, and
  // the tile dispatch reads only those.
  if (!tile_pass_open) {
    PerformDeferredClears(command_buffer);
  }
  // A resolve may run in the open pass ahead of the work left to its end only
  // if it reads nothing that work changes.
  if (tile_pass_open && !post_pass_resolves_.empty()) {
    if (const char* conflict = GetPostPassResolveConflict(resolve_info)) {
      end_tile_pass(conflict);
    }
  }
  // Strip-merged render targets an ended session left owning tiles it reads
  // move into their twins first.
  {
    uint32_t span_base, span_row_length, span_rows, span_pitch;
    resolve_info.GetCopyEdramTileSpan(span_base, span_row_length, span_rows,
                                      span_pitch);
    if (IsStripMergeTwinPendingInResolve(span_base, span_row_length, span_rows,
                                         span_pitch)) {
      end_tile_pass("strip_merge_twin");
      MaterializeStripMergeTwinsInResolve(span_base, span_row_length, span_rows,
                                          span_pitch);
    }
  }

  // Nothing to do.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    end_tile_pass("empty");
    return true;
  }

  if (resolve_info.copy_dest_extent_length &&
      resolve_info.rb_copy_control.copy_command ==
          xenos::CopyCommand::kConvertTo1111) {
    // TODO(xenios-jp): D3D12 performs it (resolve_dxt3aas1111). Not ported: no
    // trace or log seen uses it.
    XELOGW("MetalResolve: resolve copy command 2 is not supported");
    resolve_info.copy_dest_extent_length = 0;
  }

  if (!resolve_info.copy_dest_extent_length) {
    // A malformed or entirely clipped copy destination drops only the copy.
    // The guest's post-resolve EDRAM clear is independent and must still run.
    end_tile_pass("no_copy");
    return PerformResolveClear(resolve_info, command_buffer);
  }

  const bool is_depth = resolve_info.IsCopyingDepth();
  const bool draw_resolution_scaled = IsDrawResolutionScaled();
  const uint32_t resolve_width = resolve_info.coordinate_info.width_div_8 * 8;
  const uint32_t resolve_height = resolve_info.height_div_8 * 8;

  uint32_t dump_base, dump_row_length_used, dump_rows, dump_pitch;
  resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used, dump_rows,
                                    dump_pitch);

  draw_util::ResolveCopyShaderConstants copy_constants;
  uint32_t group_count_x = 0, group_count_y = 0;
  draw_util::ResolveCopyShaderIndex copy_shader = resolve_info.GetCopyShader(
      draw_resolution_scale_x(), draw_resolution_scale_y(), copy_constants,
      group_count_x, group_count_y);
  if (!is_depth) {
    copy_constants.dest_relative.edram_info.exact_unorm8_average = 1;
  }

  // A depth resolve can't read the pass's tiles, so defer it to pass end when
  // possible so color resolves after it can still tile-resolve in the pass.
  if (tile_pass_open && is_depth && !draw_resolution_scaled &&
      LeaveDepthResolveToPassEnd(resolve_info, dump_base, dump_row_length_used,
                                 dump_rows, dump_pitch)) {
    written_address = resolve_info.copy_dest_extent_start;
    written_length = resolve_info.copy_dest_extent_length;
    last_resolve_kept_pass_ = true;
    ++pass_stats_.depth_resolves_deferred;
    return true;
  }

  bool clear_left_to_pass_end = false;
  MetalTextureCache::ResolveTextureRefreshTarget refresh_target;
  const bool dest_rows_alias = resolve_info.CopyDestRowsAlias();
  const bool resolved_directly = TryResolveDirectly(
      resolve_info, copy_shader, copy_constants, dump_base,
      dump_row_length_used, dump_rows, dump_pitch, resolve_width,
      resolve_height, group_count_x, group_count_y, is_depth,
      draw_resolution_scaled, dest_rows_alias, command_buffer, tile_pass_open,
      transfers_flushed, clear_left_to_pass_end, refresh_target);

  if (!resolved_directly) {
    DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch,
                      command_buffer);
  }

  command_processor_.SetSwapDestSwap(
      resolve_info.copy_dest_base, resolve_info.copy_dest_info.copy_dest_swap);

  auto finish_resolve = [&]() {
    written_address = resolve_info.copy_dest_extent_start;
    written_length = resolve_info.copy_dest_extent_length;

    if (auto* tex_cache = command_processor_.texture_cache()) {
      auto refresh_lock = xe::global_critical_region::AcquireDeferred();
      if (resolved_directly && refresh_target && refresh_target.HasPartial()) {
        refresh_lock.lock();
      }
      tex_cache->MarkRangeAsResolved(written_address, written_length,
                                     draw_resolution_scaled);
      if (resolved_directly && refresh_target) {
        tex_cache->PublishDirectResolveTextureRefresh(refresh_target);
      }
    }

    if (clear_left_to_pass_end) {
      return true;
    }
    return PerformResolveClear(resolve_info, command_buffer);
  };

  if (resolved_directly) {
    return finish_resolve();
  }

  if (EncodeEdramComputeResolve(resolve_info, copy_shader, copy_constants,
                                resolve_height, group_count_x, group_count_y,
                                draw_resolution_scaled, dest_rows_alias,
                                command_buffer)) {
    return finish_resolve();
  }

  XELOGE(
      "MetalRenderTargetCache::Resolve: copy to 0x{:08X} ({} bytes) not "
      "performed, the destination keeps its old contents",
      resolve_info.copy_dest_extent_start,
      resolve_info.copy_dest_extent_length);
  if (!clear_left_to_pass_end) {
    PerformResolveClear(resolve_info, command_buffer);
  }
  return false;
}

MetalRenderTargetCache::PairedClearResult
MetalRenderTargetCache::TryPerformPairedResolveClear(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* transfers, const uint64_t* clear_values,
    const Transfer::Rectangle& rectangle, MTL::CommandBuffer* cmd) {
  // Only pair the two targets already prepared by the normal ownership logic.
  // This is not deferred clearing, and never moves a clear before its copy.
  if (render_target_count != 2 || !render_targets[0] || !render_targets[1] ||
      !transfers[0].empty() || !transfers[1].empty() || !cmd ||
      draw_resolution_scale_x() != 1 || draw_resolution_scale_y() != 1) {
    return PairedClearResult::kNotApplicable;
  }
  auto* depth = static_cast<MetalRenderTarget*>(render_targets[0]);
  auto* color = static_cast<MetalRenderTarget*>(render_targets[1]);
  const RenderTargetKey depth_key = depth->key();
  const RenderTargetKey color_key = color->key();
  constexpr uint32_t sample_count = 4;
  if (!depth_key.is_depth || color_key.is_depth ||
      color_key.GetColorFormat() != xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      depth_key.msaa_samples != xenos::MsaaSamples::k4X ||
      color_key.msaa_samples != xenos::MsaaSamples::k4X ||
      depth_key.pitch_tiles_at_32bpp != color_key.pitch_tiles_at_32bpp ||
      depth_key.scale_native != color_key.scale_native) {
    return PairedClearResult::kNotApplicable;
  }
  MTL::Texture* dt = depth->texture();
  MTL::Texture* st = depth->stencil_attachment_texture();
  MTL::Texture* ct = color->transfer_texture();
  const MTL::TextureType texture_type = MTL::TextureType2DMultisample;
  // A color target sharing another key's allocation through a view keeps the
  // generic clear.
  if (!dt || !st || !ct || ct != color->texture() || ct->parentTexture() ||
      dt->textureType() != texture_type ||
      ct->textureType() != dt->textureType() ||
      dt->pixelFormat() != MTL::PixelFormatDepth32Float ||
      st->pixelFormat() != MTL::PixelFormatStencil8 ||
      ct->pixelFormat() != MTL::PixelFormatRGBA8Unorm ||
      dt->sampleCount() != sample_count || ct->sampleCount() != sample_count ||
      dt->width() != ct->width() || dt->height() != ct->height()) {
    return PairedClearResult::kNotApplicable;
  }
  // Clip the clear rectangle to the attachments.
  const uint32_t target_width = uint32_t(dt->width());
  const uint32_t target_height = uint32_t(dt->height());
  if (rectangle.x_pixels >= target_width ||
      rectangle.y_pixels >= target_height) {
    return PairedClearResult::kNotApplicable;
  }
  const uint32_t clear_x = rectangle.x_pixels, clear_y = rectangle.y_pixels;
  const uint32_t clear_width =
      std::min(rectangle.width_pixels, target_width - clear_x);
  const uint32_t clear_height =
      std::min(rectangle.height_pixels, target_height - clear_y);
  if (!clear_width || !clear_height) {
    return PairedClearResult::kNotApplicable;
  }
  // Clearing the whole attachments also covers a smaller rectangle outside
  // which neither holds EDRAM data.
  const bool full_clear =
      (!clear_x && !clear_y && clear_width == target_width &&
       clear_height == target_height) ||
      (IsRenderTargetDataOnlyInRectangle(depth_key, false, rectangle) &&
       IsRenderTargetDataOnlyInRectangle(color_key, false, rectangle));

  // A partial clear preserves pixels outside the rectangle. Do not pair a
  // first-use target whose preserved contents have not been initialized.
  if (!full_clear &&
      (depth->needs_initial_clear() || color->needs_initial_clear())) {
    return PairedClearResult::kNotApplicable;
  }

  // Use exactly the existing conversions, including the float intermediate
  // before ClearColor's double. No new rounding or format packing is
  // introduced.
  const float clear_depth = MetalTransferEncoder::GetResolveClearDepth(
      depth_key.GetDepthFormat(), clear_values[0]);
  const uint32_t stencil_value = uint32_t(clear_values[0]) & 0xFFu;
  TransferClearColorFloatConstants color_constants = {};
  MetalTransferEncoder::GetResolveClearColor8888(clear_values[1],
                                                 color_constants.color);

  // A whole-attachment clear needs no pass of its own: the next pass that
  // binds the targets performs it as its clear load action, and that pass
  // doesn't load the old contents either.
  if (full_clear && ::cvars::metal_defer_resolve_clears) {
    depth->DeferDepthClear(clear_depth, stencil_value);
    color->DeferColorClear(color_constants.color);
    depth->NoteDeferredClearRows(GetRenderTargetOwnedHeight(depth_key));
    color->NoteDeferredClearRows(GetRenderTargetOwnedHeight(color_key));
    deferred_clear_targets_.push_back(depth_key);
    deferred_clear_targets_.push_back(color_key);
    pass_stats_.deferred_resolve_clears += 2;
    MarkContentChanged(depth);
    MarkContentChanged(color);
    render_pass_descriptor_dirty_ = true;
    return PairedClearResult::kEncoded;
  }

  MTL::RenderPipelineState* clear_pipeline = nullptr;
  MTL::DepthStencilState* depth_state = nullptr;
  if (!full_clear) {
    TransferColorAttachmentFormats formats = {};
    formats[0] = MTL::PixelFormatRGBA8Unorm;
    clear_pipeline = transfer_encoder_->GetOrCreateTransferClearPipeline(
        MTL::PixelFormatRGBA8Unorm, false, false, sample_count, 0, &formats,
        MTL::PixelFormatDepth32Float, MTL::PixelFormatStencil8);
    depth_state = transfer_encoder_->GetTransferDepthClearState();
    // No work or initial-clear state has been changed, so generic fallback is
    // safe even if the combined-attachment pipeline could not be created.
    if (!clear_pipeline || !depth_state) {
      return PairedClearResult::kNotApplicable;
    }
  }

  auto* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
  const MTL::LoadAction load =
      full_clear ? MTL::LoadActionClear : MTL::LoadActionLoad;
  auto* ca = rp->colorAttachments()->object(0);
  ca->setTexture(ct);
  ca->setLoadAction(load);
  ca->setStoreAction(MTL::StoreActionStore);
  ca->setClearColor(
      MTL::ClearColor(color_constants.color[0], color_constants.color[1],
                      color_constants.color[2], color_constants.color[3]));
  auto* da = rp->depthAttachment();
  da->setTexture(dt);
  da->setLoadAction(load);
  da->setStoreAction(MTL::StoreActionStore);
  da->setClearDepth(clear_depth);
  auto* sa = rp->stencilAttachment();
  sa->setTexture(st);
  sa->setLoadAction(load);
  sa->setStoreAction(MTL::StoreActionStore);
  sa->setClearStencil(stencil_value);
  const RenderTargetKey paired_keys[] = {depth_key, color_key};
  RestrictRenderArea(rp, paired_keys, 2,
                     (rectangle.y_pixels + rectangle.height_pixels) *
                         draw_resolution_scale_y());

  command_processor_.EndSharedComputeEncoder(cmd);
  auto* encoder = cmd->renderCommandEncoder(rp);
  if (!encoder) {
    return PairedClearResult::kFailed;
  }
  if (!full_clear) {
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
    encoder->setDepthBias(0.0f, 0.0f, 0.0f);
    encoder->setDepthClipMode(MTL::DepthClipModeClip);
    // Constant viewport depth lets the color draw write depth as well.
    const MTL::Viewport viewport = {double(clear_x),     double(clear_y),
                                    double(clear_width), double(clear_height),
                                    double(clear_depth), double(clear_depth)};
    const MTL::ScissorRect scissor = {clear_x, clear_y, clear_width,
                                      clear_height};
    encoder->setViewport(viewport);
    encoder->setScissorRect(scissor);
    encoder->setRenderPipelineState(clear_pipeline);
    encoder->setDepthStencilState(depth_state);
    encoder->setStencilReferenceValue(stencil_value);
    encoder->setFragmentBytes(&color_constants, sizeof(color_constants), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                            NS::UInteger(3));
  }
  encoder->endEncoding();
  // Clear writes invalidate retained round-trip transfer records.
  MarkContentChanged(depth);
  MarkContentChanged(color);
  depth->SetNeedsInitialClear(false);
  color->SetNeedsInitialClear(false);
  render_pass_descriptor_dirty_ = true;
  return PairedClearResult::kEncoded;
}

MTL::DepthStencilState* MetalRenderTargetCache::GetOrCreateRetargetClearState(
    bool write_depth, bool write_stencil, uint8_t stencil_write_mask) {
  const uint32_t state_key =
      (uint32_t(write_depth) << 9) |
      (write_stencil ? 0x100u | uint32_t(stencil_write_mask) : 0u);
  MTL::DepthStencilState*& clear_state = retarget_clear_states_[state_key];
  if (!clear_state) {
    MTL::DepthStencilDescriptor* desc =
        MTL::DepthStencilDescriptor::alloc()->init();
    desc->setDepthCompareFunction(MTL::CompareFunctionAlways);
    desc->setDepthWriteEnabled(write_depth);
    if (write_stencil) {
      MTL::StencilDescriptor* stencil = MTL::StencilDescriptor::alloc()->init();
      stencil->setStencilCompareFunction(MTL::CompareFunctionAlways);
      stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
      stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
      stencil->setDepthStencilPassOperation(MTL::StencilOperationReplace);
      stencil->setReadMask(0xFF);
      stencil->setWriteMask(stencil_write_mask);
      desc->setFrontFaceStencil(stencil);
      desc->setBackFaceStencil(stencil);
      stencil->release();
    }
    clear_state = device_->newDepthStencilState(desc);
    desc->release();
  }
  return clear_state;
}

bool MetalRenderTargetCache::PerformDepthClearRetarget(
    const DepthClearRetarget& retarget) {
  auto* owner = static_cast<MetalRenderTarget*>(retarget.owner);
  // Transfers held behind a clear quad are still to be encoded, maybe into
  // the owner.
  if (!owner || quad_hold_slot_ != UINT32_MAX ||
      HasPendingDrawPassTransfers()) {
    return false;
  }
  const RenderTargetKey key = owner->key();
  MTL::Texture* depth_texture = owner->texture();
  MTL::Texture* stencil_texture = owner->stencil_attachment_texture();
  if (!depth_texture || !stencil_texture || owner->needs_initial_clear() ||
      owner->clear_deferred() ||
      depth_texture->pixelFormat() != MTL::PixelFormatDepth32Float ||
      stencil_texture->pixelFormat() != MTL::PixelFormatStencil8) {
    return false;
  }
  // One host sample for each guest sample: the draw covers whole pixels, so
  // it writes every sample of a 2x owner.
  const uint32_t sample_count = uint32_t(depth_texture->sampleCount());
  if (sample_count != (key.msaa_samples == xenos::MsaaSamples::k2X ? 2u : 1u) ||
      stencil_texture->sampleCount() != sample_count) {
    return false;
  }
  // TODO(xenios-jp): a clear an occlusion report counts could be drawn into the
  // open pass, inside the host query segment counting there, if the samples
  // counted came out the same as through the 4x view.
  if (command_processor_.IsDrawCountedByQuery()) {
    return false;
  }
  const Transfer::Rectangle& rectangle = retarget.rectangle;
  if (!rectangle.width_pixels || !rectangle.height_pixels ||
      rectangle.x_pixels + rectangle.width_pixels > depth_texture->width() ||
      rectangle.y_pixels + rectangle.height_pixels > depth_texture->height()) {
    return false;
  }
  MTL::DepthStencilState* clear_state = GetOrCreateRetargetClearState(
      retarget.write_depth, retarget.write_stencil,
      retarget.stencil_write_mask);
  if (!clear_state) {
    return false;
  }
  TransferClearDepthConstants constants = {};
  constants.depth = MetalTransferEncoder::GetResolveClearDepth(
      key.GetDepthFormat(), uint64_t(retarget.guest_depth) << 8);
  const MTL::Viewport viewport = {double(rectangle.x_pixels),
                                  double(rectangle.y_pixels),
                                  double(rectangle.width_pixels),
                                  double(rectangle.height_pixels),
                                  0.0,
                                  1.0};
  const MTL::ScissorRect scissor = {rectangle.x_pixels, rectangle.y_pixels,
                                    rectangle.width_pixels,
                                    rectangle.height_pixels};
  auto draw_clear = [&](MTL::RenderCommandEncoder* encoder,
                        MTL::RenderPipelineState* pipeline) {
    encoder->setCullMode(MTL::CullModeNone);
    encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
    encoder->setDepthBias(0.0f, 0.0f, 0.0f);
    encoder->setDepthClipMode(MTL::DepthClipModeClip);
    encoder->setViewport(viewport);
    encoder->setScissorRect(scissor);
    encoder->setRenderPipelineState(pipeline);
    encoder->setDepthStencilState(clear_state);
    if (retarget.write_stencil) {
      encoder->setStencilReferenceValue(retarget.stencil_value);
    }
    encoder->setFragmentBytes(&constants, sizeof(constants), 0);
    encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0),
                            NS::UInteger(3));
  };

  // In the open pass if the owner is its depth attachment, the rectangle is
  // inside its render area, and nothing left to the pass's end reads or drops
  // what the pass holds: the clear is then one more draw of that pass.
  MTL::RenderPassDescriptor* open_descriptor = nullptr;
  MTL::RenderCommandEncoder* open_encoder =
      !HasPostPassResolves() && dead_store_render_targets_.empty()
          ? command_processor_.GetRenderEncoderForDepthClear(
                depth_texture, stencil_texture, &open_descriptor)
          : nullptr;
  if (open_encoder) {
    const uint64_t area_width = open_descriptor->renderTargetWidth();
    const uint64_t area_height = open_descriptor->renderTargetHeight();
    TransferAttachmentFormats formats;
    MTL::RenderPipelineState* pipeline = nullptr;
    if ((!area_width ||
         rectangle.x_pixels + rectangle.width_pixels <= area_width) &&
        (!area_height ||
         rectangle.y_pixels + rectangle.height_pixels <= area_height) &&
        transfer_encoder_->GetActiveTransferAttachmentFormats(open_descriptor,
                                                              formats)) {
      pipeline = transfer_encoder_->GetOrCreateTransferClearPipeline(
          MTL::PixelFormatDepth32Float, false, true, sample_count, 0,
          &formats.color_attachment_formats, formats.depth_attachment_format,
          formats.stencil_attachment_format);
    }
    if (pipeline) {
      draw_clear(open_encoder, pipeline);
      command_processor_.NoteDepthClearInRenderEncoder();
      MarkContentChanged(owner);
      return true;
    }
  }

  MTL::RenderPipelineState* pipeline =
      transfer_encoder_->GetOrCreateTransferClearPipeline(
          MTL::PixelFormatDepth32Float, false, true, sample_count, 0, nullptr,
          MTL::PixelFormatDepth32Float, MTL::PixelFormatStencil8);
  MTL::CommandBuffer* cmd = command_processor_.EnsureCommandBuffer();
  if (!pipeline || !cmd) {
    return false;
  }
  command_processor_.EndRenderEncoder();
  auto* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
  auto* da = rp->depthAttachment();
  da->setTexture(depth_texture);
  da->setLoadAction(MTL::LoadActionLoad);
  da->setStoreAction(MTL::StoreActionStore);
  auto* sa = rp->stencilAttachment();
  sa->setTexture(stencil_texture);
  sa->setLoadAction(MTL::LoadActionLoad);
  sa->setStoreAction(MTL::StoreActionStore);
  RestrictRenderArea(rp, &key, 1, rectangle.y_pixels + rectangle.height_pixels);
  command_processor_.EndSharedComputeEncoder(cmd);
  auto* encoder = cmd->renderCommandEncoder(rp);
  if (!encoder) {
    return false;
  }
  draw_clear(encoder, pipeline);
  encoder->endEncoding();
  // As a draw writing depth to the owner.
  MarkContentChanged(owner);
  render_pass_descriptor_dirty_ = true;
  return true;
}

void MetalRenderTargetCache::PerformDeferredClears(MTL::CommandBuffer* cmd) {
  if (deferred_clear_targets_.empty()) {
    return;
  }
  std::vector<RenderTargetKey> keys;
  keys.swap(deferred_clear_targets_);
  for (RenderTargetKey key : keys) {
    auto it = render_target_map_.find(key.key);
    if (it == render_target_map_.end() || !it->second) {
      continue;
    }
    MetalRenderTarget* rt = it->second;
    // Already performed by a pass that bound it, or listed twice.
    if (!rt->clear_deferred()) {
      continue;
    }
    MTL::Texture* texture = rt->texture();
    if (!texture) {
      continue;
    }
    if (!cmd) {
      cmd = command_processor_.EnsureCommandBuffer();
      if (!cmd) {
        XELOGE(
            "MetalRenderTargetCache::PerformDeferredClears: no command buffer");
        return;
      }
    }
    command_processor_.EndRenderEncoder();
    auto* rp = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (key.is_depth) {
      auto* da = rp->depthAttachment();
      da->setTexture(texture);
      da->setLoadAction(MTL::LoadActionClear);
      da->setStoreAction(MTL::StoreActionStore);
      da->setClearDepth(rt->clear_depth());
      if (MTL::Texture* stencil_texture = rt->stencil_attachment_texture()) {
        auto* sa = rp->stencilAttachment();
        sa->setTexture(stencil_texture);
        sa->setLoadAction(MTL::LoadActionClear);
        sa->setStoreAction(MTL::StoreActionStore);
        sa->setClearStencil(rt->clear_stencil());
      }
    } else {
      auto* ca = rp->colorAttachments()->object(0);
      ca->setTexture(texture);
      ca->setLoadAction(MTL::LoadActionClear);
      ca->setStoreAction(MTL::StoreActionStore);
      ca->setClearColor(rt->clear_color());
    }
    // The clear covers the rows the target owned when the guest cleared it.
    // Rows it has lost since are about to be transferred out of it, so the
    // rows it owns now are not enough.
    RestrictRenderArea(
        rp, &key, 1,
        uint32_t(std::min<uint64_t>(
            uint64_t(rt->deferred_clear_rows()) *
                std::max<uint32_t>(1u, draw_resolution_scale_y()),
            UINT32_MAX)));
    auto* encoder = cmd->renderCommandEncoder(rp);
    if (!encoder) {
      XELOGE(
          "MetalRenderTargetCache::PerformDeferredClears: failed to create "
          "the clear encoder");
      continue;
    }
    encoder->endEncoding();
    rt->SetNeedsInitialClear(false);
    render_pass_descriptor_dirty_ = true;
  }
}

void MetalRenderTargetCache::PlanResolveStoreActions(
    const draw_util::ResolveInfo& resolve_info) {
  dead_store_render_targets_.clear();
  if (!::cvars::metal_resolve_store_dontcare || IsDrawResolutionScaled() ||
      (!resolve_info.IsClearingDepth() && !resolve_info.IsClearingColor()) ||
      HasPendingDrawPassTransfers()) {
    return;
  }
  ResolveClearPlan plan;
  if (!GetHostRenderTargetsResolveClearPlan(resolve_info, plan)) {
    return;
  }
  // The render targets the copy reads, through the dump or directly.
  std::vector<ResolveCopyDumpRectangle> read_rectangles;
  if (resolve_info.copy_dest_extent_length &&
      resolve_info.rb_copy_control.copy_command !=
          xenos::CopyCommand::kConvertTo1111) {
    uint32_t base, row_length_used, rows, pitch;
    resolve_info.GetCopyEdramTileSpan(base, row_length_used, rows, pitch);
    GetResolveCopyRectanglesToDump(base, row_length_used, rows, pitch,
                                   read_rectangles);
  }
  const Transfer::Rectangle& rect = plan.clear_rectangle;
  // The clear has to rewrite everything in the backing that holds EDRAM data,
  // what the target owns now and what the clear gives it, and nothing before
  // it may read the old contents.
  auto plan_dead = [&](MetalRenderTarget* rt, RenderTargetKey cleared_key,
                       uint32_t cleared_start_tiles_base_relative,
                       uint32_t cleared_length_tiles) {
    if (!rt || !cleared_length_tiles || rt->key() != cleared_key ||
        rt->clear_deferred() || !rt->texture()) {
      return;
    }
    const bool is_64bpp = cleared_key.Is64bpp();
    if (!IsRenderTargetDataOnlyInRectangle(cleared_key, is_64bpp, rect)) {
      return;
    }
    // The cleared range, split where the EDRAM addressing wraps.
    Transfer::Rectangle outside[kMaxBandedRectanglesWithCutout];
    const uint32_t start_tiles =
        cleared_key.base_tiles + cleared_start_tiles_base_relative;
    const uint32_t first_start_tiles = start_tiles % xenos::kEdramTileCount;
    const uint32_t first_end_tiles = std::min(
        first_start_tiles + cleared_length_tiles, xenos::kEdramTileCount);
    const uint32_t wrapped_length_tiles =
        cleared_length_tiles - (first_end_tiles - first_start_tiles);
    if (GetRangeRectangles(first_start_tiles, first_end_tiles, cleared_key,
                           is_64bpp, outside, &rect) ||
        (wrapped_length_tiles &&
         GetRangeRectangles(0, wrapped_length_tiles, cleared_key, is_64bpp,
                            outside, &rect))) {
      return;
    }
    for (const ResolveCopyDumpRectangle& read : read_rectangles) {
      if (read.render_target == rt) {
        return;
      }
    }
    dead_store_render_targets_.push_back(rt);
  };
  plan_dead(current_depth_target_, plan.depth_render_target_key,
            plan.depth_clear_start_tiles_base_relative,
            plan.depth_clear_length_tiles);
  for (MetalRenderTarget* color : current_color_targets_) {
    plan_dead(color, plan.color_render_target_key,
              plan.color_clear_start_tiles_base_relative,
              plan.color_clear_length_tiles);
  }
}

bool MetalRenderTargetCache::PerformResolveClear(
    const draw_util::ResolveInfo& resolve_info,
    MTL::CommandBuffer* command_buffer) {
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (!clear_depth && !clear_color) {
    return true;
  }
  Transfer::Rectangle clear_rectangle;
  RenderTarget* clear_targets[2] = {};
  std::vector<Transfer> clear_transfers[2];
  if (PrepareHostRenderTargetsResolveClear(
          resolve_info, clear_rectangle, clear_targets[0], clear_transfers[0],
          clear_targets[1], clear_transfers[1])) {
    uint64_t clear_values[2];
    clear_values[0] = resolve_info.rb_depth_clear;
    // For 64bpp formats, RB_COLOR_CLEAR_LO is the lower 32 bits of the packed
    // clear value. RB_COLOR_CLEAR is the upper 32 bits and, for 32bpp formats,
    // the whole value.
    clear_values[1] = resolve_info.color_edram_info.format_is_64bpp
                          ? resolve_info.rb_color_clear_lo |
                                (uint64_t(resolve_info.rb_color_clear) << 32)
                          : resolve_info.rb_color_clear;
    return PerformTransfersAndResolveClears(2, clear_targets, clear_transfers,
                                            nullptr, clear_values,
                                            &clear_rectangle, command_buffer);
  }
  return true;
}

namespace {

// Whether two ranges of EDRAM tiles overlap, either of them wrapping at
// kEdramTileCount.
bool EdramTileRangesOverlap(uint32_t a_start, uint32_t a_length,
                            uint32_t b_start, uint32_t b_length) {
  static_assert(!(xenos::kEdramTileCount & (xenos::kEdramTileCount - 1)));
  if (!a_length || !b_length) {
    return false;
  }
  if (a_length >= xenos::kEdramTileCount ||
      b_length >= xenos::kEdramTileCount) {
    return true;
  }
  // Two arcs of a circle overlap where either starts inside the other.
  constexpr uint32_t kTileMask = xenos::kEdramTileCount - 1;
  return ((b_start - a_start) & kTileMask) < a_length ||
         ((a_start - b_start) & kTileMask) < b_length;
}

}  // namespace

bool MetalRenderTargetCache::CanLeaveResolveToPassEnd() const {
  // Without zero-copy, an upload into shared memory ends the pass, possibly
  // in the middle of a later resolve. A trace being recorded reads guest
  // memory right away.
  auto* shared = command_processor_.shared_memory();
  return !flushing_post_pass_resolves_ &&
         command_processor_.HasActiveRenderEncoder() && shared &&
         shared->is_zero_copy() && !(trace_writer_ && trace_writer_->is_open());
}

bool MetalRenderTargetCache::GetResolveClearTileRanges(
    const draw_util::ResolveInfo& resolve_info, uint32_t start_tiles_out[2],
    uint32_t length_tiles_out[2]) const {
  for (uint32_t i = 0; i < 2; ++i) {
    start_tiles_out[i] = 0;
    length_tiles_out[i] = 0;
  }
  if (!resolve_info.IsClearingDepth() && !resolve_info.IsClearingColor()) {
    return true;
  }
  ResolveClearPlan plan;
  if (!GetHostRenderTargetsResolveClearPlan(resolve_info, plan)) {
    return false;
  }
  start_tiles_out[0] = uint32_t(plan.depth_render_target_key.base_tiles) +
                       plan.depth_clear_start_tiles_base_relative;
  length_tiles_out[0] = plan.depth_clear_length_tiles;
  start_tiles_out[1] = uint32_t(plan.color_render_target_key.base_tiles) +
                       plan.color_clear_start_tiles_base_relative;
  length_tiles_out[1] = plan.color_clear_length_tiles;
  return true;
}

const char* MetalRenderTargetCache::GetPostPassResolveConflict(
    const draw_util::ResolveInfo& resolve_info) const {
  // The tiles the copy reads, and the guest memory it writes.
  const bool copies = resolve_info.copy_dest_extent_length &&
                      resolve_info.rb_copy_control.copy_command !=
                          xenos::CopyCommand::kConvertTo1111;
  uint32_t read_start = 0, read_length = 0;
  if (copies) {
    uint32_t row_length_used, rows, pitch;
    resolve_info.GetCopyEdramTileSpan(read_start, row_length_used, rows, pitch);
    read_length = rows ? (rows - 1) * pitch + row_length_used : 0;
  }
  if (copies && PostPassResolvesWrite(resolve_info.copy_dest_extent_start,
                                      resolve_info.copy_dest_extent_length)) {
    return "queued_dest";
  }
  for (const PostPassResolve& queued : post_pass_resolves_) {
    for (uint32_t i = 0; i < 2; ++i) {
      if (EdramTileRangesOverlap(read_start, read_length,
                                 queued.clear_start_tiles[i],
                                 queued.clear_length_tiles[i])) {
        return "queued_clear";
      }
    }
  }
  return nullptr;
}

bool MetalRenderTargetCache::LeaveResolveClearToPassEnd(
    const draw_util::ResolveInfo& resolve_info) {
  PostPassResolve queued;
  if (!CanLeaveResolveToPassEnd() ||
      !GetResolveClearTileRanges(resolve_info, queued.clear_start_tiles,
                                 queued.clear_length_tiles)) {
    return false;
  }
  queued.resolve_info = resolve_info;
  queued.strip_merge_resolve_bottom = strip_merge_resolve_bottom();
  // What the clear makes dead joins the store plan of the work already left to
  // the pass end. Each clear is planned before those ahead of it change
  // ownership, which moves data only into the tiles they rewrite.
  std::vector<MetalRenderTarget*> dead;
  if (!post_pass_resolves_.empty()) {
    dead.swap(dead_store_render_targets_);
  }
  PlanResolveStoreActions(resolve_info);
  for (MetalRenderTarget* rt : dead_store_render_targets_) {
    if (std::find(dead.begin(), dead.end(), rt) != dead.end()) {
      continue;
    }
    // A depth resolve left to the pass end, ahead of the clear, reads it.
    bool read = false;
    for (const PostPassResolve& queued : post_pass_resolves_) {
      if (std::find(queued.read_targets.begin(), queued.read_targets.end(),
                    rt) != queued.read_targets.end()) {
        read = true;
        break;
      }
    }
    if (!read) {
      dead.push_back(rt);
    }
  }
  dead_store_render_targets_.swap(dead);
  post_pass_resolves_.push_back(queued);
  return true;
}

bool MetalRenderTargetCache::LeaveDepthResolveToPassEnd(
    const draw_util::ResolveInfo& resolve_info, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch) {
  if (!CanLeaveResolveToPassEnd() ||
      !command_processor_.CanKeepRenderPassAfterTileResolve()) {
    return false;
  }
  // Only worth it with color resolves to follow in the pass's tiles.
  bool color_attached = false;
  for (MetalRenderTarget* color : current_color_targets_) {
    color_attached |= color != nullptr;
  }
  if (!color_attached) {
    return false;
  }
  // When unsure, end the pass: not after a clear left to the pass end, which
  // would change what the resolve reads by the time it runs.
  for (const PostPassResolve& queued : post_pass_resolves_) {
    if (queued.clear_length_tiles[0] || queued.clear_length_tiles[1]) {
      return false;
    }
  }
  PostPassResolve queued;
  if (!GetResolveClearTileRanges(resolve_info, queued.clear_start_tiles,
                                 queued.clear_length_tiles)) {
    return false;
  }
  queued.resolve_info = resolve_info;
  queued.strip_merge_resolve_bottom = strip_merge_resolve_bottom();
  queued.copy = true;
  queued.dest_start = resolve_info.copy_dest_extent_start;
  queued.dest_length = resolve_info.copy_dest_extent_length;
  std::vector<ResolveCopyDumpRectangle> read_rectangles;
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, read_rectangles);
  for (const ResolveCopyDumpRectangle& read : read_rectangles) {
    auto* rt = static_cast<MetalRenderTarget*>(read.render_target);
    if (rt && std::find(queued.read_targets.begin(), queued.read_targets.end(),
                        rt) == queued.read_targets.end()) {
      queued.read_targets.push_back(rt);
    }
  }
  // The pass has to store what it reads.
  KeepStoresOfResolveReads(resolve_info);
  post_pass_resolves_.push_back(std::move(queued));
  command_processor_.SetRenderEncoderResolveOnly();
  return true;
}

bool MetalRenderTargetCache::PostPassResolvesWrite(uint32_t start,
                                                   uint32_t length) const {
  for (const PostPassResolve& queued : post_pass_resolves_) {
    if (queued.copy && queued.dest_length && length &&
        uint64_t(start) < uint64_t(queued.dest_start) + queued.dest_length &&
        uint64_t(queued.dest_start) < uint64_t(start) + length) {
      return true;
    }
  }
  return false;
}

void MetalRenderTargetCache::KeepStoresOfResolveReads(
    const draw_util::ResolveInfo& resolve_info) {
  if (dead_store_render_targets_.empty() ||
      !resolve_info.copy_dest_extent_length ||
      resolve_info.rb_copy_control.copy_command ==
          xenos::CopyCommand::kConvertTo1111) {
    return;
  }
  uint32_t base, row_length_used, rows, pitch;
  resolve_info.GetCopyEdramTileSpan(base, row_length_used, rows, pitch);
  std::vector<ResolveCopyDumpRectangle> read_rectangles;
  GetResolveCopyRectanglesToDump(base, row_length_used, rows, pitch,
                                 read_rectangles);
  for (const ResolveCopyDumpRectangle& read : read_rectangles) {
    dead_store_render_targets_.erase(
        std::remove(dead_store_render_targets_.begin(),
                    dead_store_render_targets_.end(),
                    static_cast<MetalRenderTarget*>(read.render_target)),
        dead_store_render_targets_.end());
  }
}

MTL::Texture* MetalRenderTargetCache::AcquireRefreshScratch(
    MTL::Texture* like) {
  for (auto it = refresh_scratch_pool_.begin();
       it != refresh_scratch_pool_.end(); ++it) {
    MTL::Texture* scratch = *it;
    if (scratch->pixelFormat() == like->pixelFormat() &&
        scratch->width() == like->width() &&
        scratch->height() == like->height()) {
      refresh_scratch_pool_.erase(it);
      return scratch;
    }
  }
  MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
  descriptor->setTextureType(MTL::TextureType2DArray);
  descriptor->setPixelFormat(like->pixelFormat());
  descriptor->setWidth(like->width());
  descriptor->setHeight(like->height());
  descriptor->setDepth(1);
  descriptor->setArrayLength(1);
  descriptor->setMipmapLevelCount(1);
  descriptor->setSampleCount(1);
  descriptor->setStorageMode(MTL::StorageModePrivate);
  descriptor->setUsage(MTL::TextureUsageShaderWrite);
  MTL::Texture* scratch = device_->newTexture(descriptor);
  descriptor->release();
  return scratch;
}

void MetalRenderTargetCache::ReleaseRefreshScratch(MTL::Texture* scratch) {
  // Commands encoded later reuse it after the copy reading it, which Metal
  // orders (tracked).
  constexpr size_t kMaxFreeScratch = 8;
  if (refresh_scratch_pool_.size() < kMaxFreeScratch) {
    refresh_scratch_pool_.push_back(scratch);
  } else {
    scratch->release();
  }
}

void MetalRenderTargetCache::ClearPostPassRefreshCopies() {
  MetalTextureCache* texture_cache = command_processor_.texture_cache();
  for (PostPassRefreshCopy& copy : post_pass_refresh_copies_) {
    if (copy.scratch) {
      ReleaseRefreshScratch(copy.scratch);
    }
    for (PostPassRefreshCopy::Swap& swap : copy.swaps) {
      if (texture_cache) {
        texture_cache->ReleaseResolveRefreshStorage(std::move(swap.storage));
      }
    }
  }
  post_pass_refresh_copies_.clear();
}

void MetalRenderTargetCache::EncodePostPassRefreshCopies(
    MTL::CommandBuffer* command_buffer, std::span<PostPassRefreshCopy> copies) {
  if (copies.empty()) {
    return;
  }
  MetalTextureCache* texture_cache = command_processor_.texture_cache();
  bool any_scratch = false;
  for (PostPassRefreshCopy& copy : copies) {
    any_scratch |= copy.scratch != nullptr;
    // The commands encoded from here on use the new images. If a texture is
    // gone, nothing reads what was written for it.
    for (PostPassRefreshCopy::Swap& swap : copy.swaps) {
      if (texture_cache) {
        texture_cache->SwapResolveRefreshStorage(swap.write_view.get(),
                                                 swap.storage);
        texture_cache->ReleaseResolveRefreshStorage(std::move(swap.storage));
      }
    }
  }
  if (!any_scratch) {
    return;
  }
  command_processor_.EndSharedComputeEncoder(command_buffer);
  MTL::BlitCommandEncoder* blit = command_buffer->blitCommandEncoder();
  if (!blit) {
    XELOGE(
        "MetalRenderTargetCache: no blit encoder for the refresh of sampled "
        "textures left to the pass end");
  }
  for (PostPassRefreshCopy& copy : copies) {
    if (!copy.scratch) {
      continue;
    }
    if (blit && copy.width && copy.height) {
      for (const NS::SharedPtr<MTL::Texture>& destination : copy.destinations) {
        const MTL::Origin origin =
            MTL::Origin::Make(copy.origin_x, copy.origin_y, 0);
        blit->copyFromTexture(copy.scratch, 0, 0, origin,
                              MTL::Size::Make(copy.width, copy.height, 1),
                              destination.get(), 0, 0, origin);
      }
    }
    ReleaseRefreshScratch(copy.scratch);
  }
  if (blit) {
    blit->endEncoding();
  }
}

void MetalRenderTargetCache::FlushPostPassResolves(
    MTL::CommandBuffer* command_buffer) {
  if (flushing_post_pass_resolves_ ||
      (post_pass_resolves_.empty() && post_pass_refresh_copies_.empty())) {
    return;
  }
  assert_false(command_processor_.HasActiveRenderEncoder());
  flushing_post_pass_resolves_ = true;
  std::vector<PostPassResolve> resolves;
  resolves.swap(post_pass_resolves_);
  std::vector<PostPassRefreshCopy> copies;
  copies.swap(post_pass_refresh_copies_);
  if (!command_buffer) {
    command_buffer = command_processor_.EnsureCommandBuffer();
  }
  // In the guest's order: the copies of tile resolves come after the resolves
  // left to the pass end before them.
  size_t copies_done = 0;
  auto encode_copies_before = [&](size_t resolve_index) {
    size_t copies_end = copies_done;
    while (copies_end < copies.size() &&
           copies[copies_end].post_pass_resolves_before <= resolve_index) {
      ++copies_end;
    }
    EncodePostPassRefreshCopies(command_buffer, std::span<PostPassRefreshCopy>(
                                                    copies.data() + copies_done,
                                                    copies_end - copies_done));
    copies_done = copies_end;
  };
  // Running the depth resolves resets what the last resolve, the one before
  // the pass end, did.
  const bool kept_pass = last_resolve_kept_pass_;
  const uint32_t strip_merge_bottom = strip_merge_resolve_bottom();
  for (size_t resolve_index = 0; resolve_index < resolves.size();
       ++resolve_index) {
    encode_copies_before(resolve_index);
    const PostPassResolve& resolve = resolves[resolve_index];
    SetStripMergeResolveBottom(resolve.strip_merge_resolve_bottom);
    if (resolve.copy) {
      uint32_t written_address, written_length;
      if (!ResolveWithInfo(resolve.resolve_info, true, written_address,
                           written_length, command_buffer)) {
        XELOGE(
            "MetalRenderTargetCache: a depth resolve left to the end of the "
            "pass failed");
      }
      continue;
    }
    if (!PerformResolveClear(resolve.resolve_info, command_buffer)) {
      XELOGE(
          "MetalRenderTargetCache: a resolve clear left to the end of the "
          "pass failed");
    }
  }
  encode_copies_before(SIZE_MAX);
  SetStripMergeResolveBottom(strip_merge_bottom);
  last_resolve_kept_pass_ = kept_pass;
  flushing_post_pass_resolves_ = false;
}

void MetalRenderTargetCache::LogPassStats() const {
  const PassStats& s = pass_stats_;
  XELOGI(
      "MetalTBDR: passes={} load(clear={}, dontcare={}, load={}) "
      "store(dontcare={}, store={}) resolves(tile={}, kept_pass={}, "
      "direct={}, edram={}, depth_deferred={}) transfers(in_pass={}, "
      "standalone={}, elided={}, stencil_keep_depth={}) deferred_clears={}",
      s.draw_passes, s.load_action_clear, s.load_action_dontcare,
      s.load_action_load, s.store_action_dontcare, s.store_action_store,
      s.tile_resolves, s.tile_resolves_kept_pass, s.direct_resolves,
      s.edram_resolves, s.depth_resolves_deferred, s.in_pass_transfer_flushes,
      s.standalone_transfer_flushes, s.transfers_elided,
      s.stencil_round_trips_kept_depth, s.deferred_resolve_clears);
  if (!s.tile_resolve_rejections.empty()) {
    std::vector<std::pair<std::string_view, uint64_t>> sorted(
        s.tile_resolve_rejections.begin(), s.tile_resolve_rejections.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::string summary;
    for (size_t i = 0; i < sorted.size(); ++i) {
      if (i) {
        summary += ", ";
      }
      summary.append(sorted[i].first.data(), sorted[i].first.size());
      summary += "=";
      summary += std::to_string(sorted[i].second);
    }
    XELOGI("MetalTBDR: tile_resolve_rejections: {}", summary);
  }
}

void MetalRenderTargetCache::FinalizeRenderPassStoreActions(
    MTL::RenderCommandEncoder* encoder,
    MTL::RenderPassDescriptor* pass_descriptor) {
  if (!encoder || !pass_descriptor) {
    dead_store_render_targets_.clear();
    return;
  }
  ++pass_stats_.draw_passes;
  auto count_load = [&](MTL::LoadAction action) {
    switch (action) {
      case MTL::LoadActionClear:
        ++pass_stats_.load_action_clear;
        break;
      case MTL::LoadActionDontCare:
        ++pass_stats_.load_action_dontcare;
        break;
      case MTL::LoadActionLoad:
        ++pass_stats_.load_action_load;
        break;
      default:
        break;
    }
  };
  auto count_store = [&](MTL::StoreAction action) {
    if (action == MTL::StoreActionDontCare) {
      ++pass_stats_.store_action_dontcare;
    } else {
      ++pass_stats_.store_action_store;
    }
  };
  // Returns whether the contents of the texture are dead, and if so, notes
  // that the backing no longer holds what the round-trip records say.
  auto is_dead = [&](MTL::Texture* texture) {
    for (MetalRenderTarget* rt : dead_store_render_targets_) {
      if (texture && (rt->texture() == texture ||
                      rt->stencil_attachment_texture() == texture)) {
        return true;
      }
    }
    return false;
  };
  auto* depth_attachment = pass_descriptor->depthAttachment();
  if (depth_attachment && depth_attachment->texture()) {
    count_load(depth_attachment->loadAction());
    MTL::StoreAction resolved_store = depth_attachment->storeAction();
    if (resolved_store == MTL::StoreActionUnknown) {
      resolved_store = is_dead(depth_attachment->texture())
                           ? MTL::StoreActionDontCare
                           : MTL::StoreActionStore;
      encoder->setDepthStoreAction(resolved_store);
    }
    count_store(resolved_store);
  }
  auto* stencil_attachment = pass_descriptor->stencilAttachment();
  if (stencil_attachment && stencil_attachment->texture()) {
    count_load(stencil_attachment->loadAction());
    MTL::StoreAction resolved_store = stencil_attachment->storeAction();
    if (resolved_store == MTL::StoreActionUnknown) {
      resolved_store = is_dead(stencil_attachment->texture())
                           ? MTL::StoreActionDontCare
                           : MTL::StoreActionStore;
      encoder->setStencilStoreAction(resolved_store);
    }
    count_store(resolved_store);
  }
  auto* color_attachments = pass_descriptor->colorAttachments();
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto* color_attachment =
        color_attachments ? color_attachments->object(i) : nullptr;
    if (color_attachment && color_attachment->texture()) {
      count_load(color_attachment->loadAction());
      MTL::StoreAction resolved_store = color_attachment->storeAction();
      if (resolved_store == MTL::StoreActionUnknown) {
        resolved_store = is_dead(color_attachment->texture())
                             ? MTL::StoreActionDontCare
                             : MTL::StoreActionStore;
        encoder->setColorStoreAction(resolved_store, i);
      }
      count_store(resolved_store);
    }
  }
  for (MetalRenderTarget* rt : dead_store_render_targets_) {
    MarkContentChanged(rt);
  }
  dead_store_render_targets_.clear();
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
