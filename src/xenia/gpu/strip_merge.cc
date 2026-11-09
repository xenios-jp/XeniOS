/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/strip_merge.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/memory.h"

// TODO(xenios-jp): on by default; a title whose sessions fail verification
// stays unmerged. Open: coverage (only Halo 3 merges), exactness (merged Halo 3
// frames aren't bit-identical to unmerged ones) and side-by-side tile layouts.
DEFINE_bool(strip_merge, true,
            "Render the strips of predicated tiling (Halo 3) as one "
            "full-height pass sequence when every strip is verified to draw "
            "the same thing (Metal only, see xenia/gpu/strip_merge.h).",
            "GPU");
// TODO(xenios-jp): the clipped last strip is not byte-exact yet.
            "their dormancy ends (256 sessions at most).",
DEFINE_bool(strip_merge_log, false,
            "Log every predicated tiling session strip merge sees.", "GPU");

namespace xe {
namespace gpu {

namespace {

uint32_t PayloadDwordCount(const RingBuffer::ReadRange& payload) {
  return uint32_t((payload.first_length + payload.second_length) /
                  sizeof(uint32_t));
}

uint32_t PayloadDword(const RingBuffer::ReadRange& payload, uint32_t index) {
  size_t offset = size_t(index) * sizeof(uint32_t);
  const uint8_t* source =
      offset < payload.first_length
          ? payload.first + offset
          : payload.second + (offset - payload.first_length);
  uint32_t value;
  std::memcpy(&value, source, sizeof(value));
  return xe::byte_swap(value);
}

uint64_t HashPayload(const RingBuffer::ReadRange& payload, uint64_t seed) {
  uint64_t hash = XXH3_64bits_withSeed(payload.first, payload.first_length,
                                       seed);
  if (payload.second_length) {
    hash = XXH3_64bits_withSeed(payload.second, payload.second_length, hash);
  }
  return hash;
}

enum class PacketPolicy {
  // No rendering state: handshakes, events, memory writes, indirect buffers,
  // the bin select and mask (patched by the guest between strips).
  kIgnore,
  // Rendering state or a draw: hashed (shared) or recorded (strip-own).
  kRecord,
  // Results that would differ per strip: the session can't be merged.
  kViolation,
};

PacketPolicy GetPacketPolicy(uint32_t opcode) {
  switch (opcode) {
    case xenos::PM4_SET_CONSTANT:
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS:
    case xenos::PM4_LOAD_ALU_CONSTANT:
    case xenos::PM4_LOAD_CONSTANT_CONTEXT:
    case xenos::PM4_IM_LOAD:
    case xenos::PM4_IM_LOAD_IMMEDIATE:
    case xenos::PM4_REG_RMW:
    case xenos::PM4_CONTEXT_UPDATE:
    case xenos::PM4_INVALIDATE_STATE:
    case xenos::PM4_DRAW_INDX:
    case xenos::PM4_DRAW_INDX_2:
      return PacketPolicy::kRecord;
    case xenos::PM4_VIZ_QUERY:
    case xenos::PM4_EVENT_WRITE_ZPD:
    case xenos::PM4_DRAW_INDX_BIN:
    case xenos::PM4_DRAW_INDX_2_BIN:
      return PacketPolicy::kViolation;
    default:
      return PacketPolicy::kIgnore;
  }
}

// Whether values[k] == values[0] + k * slope for every k, with the slope from
// the first two.
bool IsLinear(const int32_t* values, uint32_t count, int32_t& slope_out) {
  slope_out = values[1] - values[0];
  for (uint32_t k = 2; k < count; ++k) {
    if (values[k] != values[0] + int32_t(k) * slope_out) {
      return false;
    }
  }
  return true;
}

int32_t WindowOffsetX(uint32_t value) { return int32_t(value << 17) >> 17; }
int32_t WindowOffsetY(uint32_t value) { return int32_t(value << 1) >> 17; }

}  // namespace

bool StripMerge::Layout::operator==(const Layout& other) const {
  if (strip_count != other.strip_count || band_rows != other.band_rows ||
      pitch_tiles_at_32bpp != other.pitch_tiles_at_32bpp ||
      msaa_samples != other.msaa_samples ||
      window_br_first != other.window_br_first ||
      strip_bits_union != other.strip_bits_union) {
    return false;
  }
  for (uint32_t i = 0; i < kMaxStrips; ++i) {
    if (strip_bits[i] != other.strip_bits[i]) {
      return false;
    }
  }
  return true;
}

void StripMerge::Strip::Clear() {
  bits = 0;
  records.clear();
  own_opcodes.clear();
  own_resolves.clear();
  draw_states.clear();
  draw_own_states.clear();
  draw_kinds.clear();
  draw_own_snapshots.clear();
  own_snapshot_starts.clear();
  own_snapshot_values.clear();
  own_draw_vertices.clear();
  own_shadow.clear();
  own_float_registers.clear();
  draws = 0;
}

void StripMerge::OnBinSelect(uint64_t bin_select, bool tracing) {
  if (session_open_) {
    if (bin_select == current_select_) {
      return;
    }
    uint64_t strip_bits = bin_select & ~kFirstTileBit;
    if (!(bin_select & kFirstTileBit) && strip_bits &&
        !(strip_bits & strip_bits_union_)) {
      if (strip_count_ < kMaxStrips) {
        EndStrip();
        BeginStrip(bin_select);
        return;
      }
      Fail("more strips than can be merged");
    }
    EndSession();
  }
  if (!cvars::strip_merge || !backend_ || tracing) {
    return;
  }
  uint64_t strip_bits = bin_select & ~kFirstTileBit;
  if ((bin_select & kFirstTileBit) && strip_bits && !(bin_select >> 32) &&
      strip_bits != (kFirstTileBit - 1)) {
    BeginSession(bin_select);
  }
}

void StripMerge::BeginSession(uint64_t bin_select) {
  ++sessions_;
  session_open_ = true;
  mode_ = Mode::kLearn;
  strip_index_ = 0;
  strip_count_ = 0;
  strip_bits_union_ = 0;
  failed_ = false;
  failure_reason_.clear();
  failure_strip_ = 0;
  window_br_extended_ = false;
  draw_checks_ran_ = candidate_.IsValid();
  uint64_t strip_bits = bin_select & ~kFirstTileBit;
  bool may_merge = !cooldown_sessions_ && candidate_.IsValid() &&
                   verified_sessions_ >= kLearnSessions &&
                   candidate_.strip_bits[0] == strip_bits &&
                   (!layout_.IsValid() || layout_ == candidate_);
  if (may_merge && merged_since_learning_ >= kRelearnInterval) {
    // Verify the game still draws the strips the same way.
    may_merge = false;
    merged_since_learning_ = 0;
  }
  if (may_merge && backend_->StripMergeBegin(candidate_)) {
    mode_ = Mode::kMerged;
    layout_ = candidate_;
    ++merged_since_learning_;
  }
  BeginStrip(bin_select);
}

void StripMerge::BeginStrip(uint64_t bin_select) {
  Strip& strip = strips_[strip_count_];
  strip.Clear();
  strip.bits = bin_select & ~kFirstTileBit;
  strip_index_ = strip_count_++;
  strip_bits_union_ |= strip.bits;
  current_select_ = bin_select;
  current_packet_own_ = false;
}

void StripMerge::EndStrip() {
  current_packet_own_ = false;
  if (strip_index_ != 0) {
    return;
  }
  RestoreWindowScissor();
  if (mode_ == Mode::kMerged && failed_) {
    // The later strips run normally from the first strip's band, which is what
    // the first strip leaves in EDRAM.
    backend_->StripMergeEnd(0, false);
    mode_ = Mode::kRecovered;
  }
}

void StripMerge::EndSession() {
  if (!session_open_) {
    return;
  }
  EndStrip();
  Layout derived;
  bool verified = !failed_ && DeriveLayout(derived, mode_ == Mode::kLearn);
  for (uint32_t k = 1; verified && k < strip_count_; ++k) {
    verified = CompareStrip(k);
  }
  const char* outcome;
  if (mode_ == Mode::kMerged) {
    backend_->StripMergeEnd(strip_count_ - 1, true);
    if (!verified || derived != layout_) {
      if (!failed_) {
        failed_ = true;
        failure_reason_ = "the strips differ from the verified layout";
        failure_strip_ = strip_count_ - 1;
      }
      XELOGW(
          "Strip merge: {} (strip {}) in a merged session, the frame may be "
          "wrong; merging paused",
          failure_reason_, failure_strip_);
      cooldown_sessions_ = kCooldownSessions + 1;
      verified_sessions_ = 0;
      outcome = "merged, failed";
    } else {
      outcome = "merged";
    }
  } else if (mode_ == Mode::kRecovered) {
    XELOGD("Strip merge: {} in the merged first strip, recovered; merging "
           "paused",
           failure_reason_);
    cooldown_sessions_ = kCooldownSessions + 1;
    verified_sessions_ = 0;
    outcome = "recovered";
  } else {
    if (verified && draw_checks_ran_ && derived == candidate_) {
      ++verified_sessions_;
      outcome = "verified";
    } else {
      verified_sessions_ = 0;
      outcome = verified ? "candidate" : "not mergeable";
    }
    candidate_ = verified ? derived : Layout();
    learned_own_used_.clear();
    if (verified) {
      for (const OwnWrite& write : strips_[0].own_writes) {
        learned_own_used_.push_back(write.used);
      }
    }
  }
  if (cvars::strip_merge_log) {
    XELOGI(
        "Strip merge: session {} {} strips {} band {} draws {} records {}{}",
        sessions_, outcome, strip_count_, derived.band_rows,
        strip_count_ ? strips_[0].draws : 0,
        strip_count_ ? uint32_t(strips_[0].records.size()) : 0,
        failure_reason_.empty()
            ? std::string()
            : fmt::format(": {} (strip {})", failure_reason_, failure_strip_));
  }
  if (cooldown_sessions_) {
    --cooldown_sessions_;
  }
  session_open_ = false;
  mode_ = Mode::kLearn;
  current_select_ = 0;
  current_packet_own_ = false;
}

  session_open_ = false;
  mode_ = Mode::kLearn;
  current_select_ = 0;
  current_packet_own_ = false;
}

bool StripMerge::DeriveLayout(Layout& layout_out, bool check_resolves) {
  if (strip_count_ < 2) {
    Fail("a single strip");
    return false;
  }
  const Strip& first = strips_[0];
  // The strips are compared draw by draw: every strip draws as many times
  // (CompareStrip), and what matters of the strips' own register writes is
  // what each draw sees of them. D3D's predicated tiling sets every strip's
  // copy destination before each strip's resolve, so with more than one
  // resolve surface strip k sees its own writes in another order around its
  // resolves than strip 0 (575207D7: strip 0 resolves after its first and
  // second own writes, strip 1 after its third and fourth).
  size_t draw_count = first.draw_own_snapshots.size();
  for (uint32_t k = 1; k < strip_count_; ++k) {
    const Strip& strip = strips_[k];
    if (strip.own_opcodes != first.own_opcodes ||
        strip.own_writes.size() != first.own_writes.size()) {
      failure_strip_ = k;
      Fail("strip-own packets differ between strips");
      return false;
    }
  }
  int32_t band_rows = 0;
  uint32_t window_br_first = 0;
  auto set_band_rows = [&](int32_t rows) {
    if (rows <= 0 || (band_rows && band_rows != rows)) {
      Fail("strip-own draws differ between strips");
      return false;
    }
  }
  uint32_t checked[kMaxStrips];
  std::fill(std::begin(checked), std::end(checked), UINT32_MAX);
  uint32_t values[kMaxStrips];
  for (size_t d = 0; d < draw_count; ++d) {
    bool same = true;
    for (uint32_t k = 0; k < strip_count_; ++k) {
      same &= strips_[k].draw_own_snapshots[d] == checked[k];
    }
    if (same) {
      // The previous draw saw the same.
      continue;
    }
    int32_t slope_x, slope_y;
    switch (reg) {
      case XE_GPU_REG_PA_SC_WINDOW_OFFSET:
        for (uint32_t k = 0; k < strip_count_; ++k) {
          values_x[k] = WindowOffsetX(strips_[k].own_writes[j].value);
          values_y[k] = WindowOffsetY(strips_[k].own_writes[j].value);
        }
        if (!IsLinear(values_x, strip_count_, slope_x) || slope_x ||
            !IsLinear(values_y, strip_count_, slope_y) || values_y[0] ||
            !set_band_rows(-slope_y)) {
          Fail("the window offset isn't the strip's frame row");
          return false;
        }
        break;
      case XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL:
      case XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR:
        for (uint32_t k = 0; k < strip_count_; ++k) {
          uint32_t value = strips_[k].own_writes[j].value;
          // The window offset disable bit of TL goes with x.
          values_x[k] = int32_t(value & 0x8000FFFF);
          values_y[k] = int32_t((value >> 16) & 0x3FFF);
        }
        if (!IsLinear(values_x, strip_count_, slope_x) || slope_x ||
            !IsLinear(values_y, strip_count_, slope_y) ||
            !set_band_rows(slope_y) ||
            values_y[0] !=
                (reg == XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL ? 0 : slope_y)) {
          Fail("the window scissor isn't the strip's rows");
    uint32_t first_begin = 0, first_end = 0;
    for (uint32_t k = 0; k < strip_count_; ++k) {
      const Strip& strip = strips_[k];
      checked[k] = strip.draw_own_snapshots[d];
      uint32_t begin = strip.own_snapshot_starts[checked[k]];
      uint32_t end = checked[k] + 1 < strip.own_snapshot_starts.size()
                         ? strip.own_snapshot_starts[checked[k] + 1]
                         : uint32_t(strip.own_snapshot_values.size());
      if (!k) {
        first_begin = begin;
        first_end = end;
        continue;
      }
      if (end - begin != first_end - first_begin) {
        failure_strip_ = k;
        Fail("a draw sees strip-own values of other registers between strips");
        return false;
      }
      for (uint32_t i = 0; i < end - begin; ++i) {
        if (strip.own_snapshot_values[begin + i].reg !=
            first.own_snapshot_values[first_begin + i].reg) {
          failure_strip_ = k;
          Fail(
              "a draw sees strip-own values of other registers between "
              "strips");
          return false;
        }
      }
    }
    for (uint32_t i = 0; i < first_end - first_begin; ++i) {
      for (uint32_t k = 0; k < strip_count_; ++k) {
        const Strip& strip = strips_[k];
        values[k] =
            strip.own_snapshot_values[strip.own_snapshot_starts[checked[k]] + i]
                .value;
      }
          }
          window_br_first = value_first;
        }
        break;
      case XE_GPU_REG_RB_COPY_DEST_BASE:
        // Checked with the resolves.
        break;
      default: {
        if (reg < XE_GPU_REG_SHADER_CONSTANT_000_X ||
            reg >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) {
          Fail("a strip-own write to a register merging can't vary");
          return false;
        }
        // A float constant component that is the strip's frame row (or its
        // negation), 0 in the first strip, so that the merged first strip's
        // frame-space pixels get the value of every strip's.
        float floats[kMaxStrips];
        for (uint32_t k = 0; k < strip_count_; ++k) {
          uint32_t value = strips_[k].own_writes[j].value;
          std::memcpy(&floats[k], &value, sizeof(float));
        }
        float slope = floats[1] - floats[0];
        bool linear = floats[0] == 0.0f && slope != 0.0f &&
                      float(int32_t(slope)) == slope;
        for (uint32_t k = 2; linear && k < strip_count_; ++k) {
          linear = floats[k] == slope * float(k);
        }
        if (!linear || !set_band_rows(std::abs(int32_t(slope)))) {
          Fail("a strip-own constant isn't the strip's frame row");
          return false;
        }
      } break;
        return false;
      }
    }
  }
  if (!band_rows || !window_br_first) {
    Fail("no strip window");
    return false;
  }
  for (size_t d = 0; d < draw_count; ++d) {
  layout_out = Layout();
  layout_out.strip_count = strip_count_;
  layout_out.band_rows = uint32_t(band_rows);
  layout_out.window_br_first = window_br_first;
  for (uint32_t k = 0; k < strip_count_; ++k) {
    layout_out.strip_bits[k] = strips_[k].bits;
  }
  layout_out.strip_bits_union = strip_bits_union_;
  if (!check_resolves) {
    // Merged sessions skip the later strips' resolves, the surface is the
    // verified one.
    layout_out.pitch_tiles_at_32bpp = layout_.pitch_tiles_at_32bpp;
    layout_out.msaa_samples = layout_.msaa_samples;
    return true;
  }
  if (first.own_resolves.empty()) {
    Fail("no strip-own resolve");
    return false;
  }
  for (uint32_t k = 1; k < strip_count_; ++k) {
    if (strips_[k].own_resolves.size() != first.own_resolves.size()) {
      failure_strip_ = k;
      Fail("strip-own resolves differ between strips");
      return false;
    }
  }
  // The destination rows of the strips must follow each other, which also
  // needs whole 32-row texture tiles per strip.
  if (band_rows % 32) {
    Fail("strips not a multiple of 32 rows");
    return false;
  }
  for (size_t j = 0; j < first.own_resolves.size(); ++j) {
    const ResolveRecord& resolve = first.own_resolves[j];
    if (j == 0) {
      layout_out.pitch_tiles_at_32bpp = resolve.pitch_tiles_at_32bpp;
      layout_out.msaa_samples = resolve.msaa_samples;
    }
    // The merged first strip resolves every strip's rows into its
    // destination, which has to reach the frame bottom.
    if (resolve.is_64bpp || resolve.clears || resolve.edram_offset_y ||
        resolve.dest_offset_y ||
    for (uint32_t k = 0; k < strip_count_; ++k) {
      const ResolveRecord& other = strips_[k].own_resolves[j];
      uint32_t bits = other.copy_control & kClearBits;
        if (other.clear_color != resolve.clear_color ||
            other.clear_color_lo != resolve.clear_color_lo ||
            other.clear_depth != resolve.clear_depth) {
          failure_strip_ = k;
          Fail("strip-own resolves that clear with other values");
          return false;
        }
    }
    if (resolve.is_64bpp || resolve.edram_offset_y || resolve.dest_offset_y ||
        resolve.height_div_8 * 8 != uint32_t(band_rows) ||
        resolve.dest_height_aligned < strip_count_ * uint32_t(band_rows) ||
        resolve.pitch_tiles_at_32bpp != layout_out.pitch_tiles_at_32bpp ||
        resolve.msaa_samples != layout_out.msaa_samples) {
      Fail("a strip-own resolve that isn't the strip's rows");
      return false;
    }
    uint32_t stride = uint32_t(band_rows) * resolve.dest_pitch_aligned *
                      resolve.dest_bytes_per_pixel;
    for (uint32_t k = 1; k < strip_count_; ++k) {
      const ResolveRecord& other = strips_[k].own_resolves[j];
      if (other.edram_info != resolve.edram_info ||
          other.coordinate_info != resolve.coordinate_info ||
          other.height_div_8 != resolve.height_div_8 ||
          other.copy_dest_info != resolve.copy_dest_info ||
          other.copy_dest_coordinate_info !=
              resolve.copy_dest_coordinate_info ||
          other.copy_control != resolve.copy_control ||
          other.copy_dest_base != resolve.copy_dest_base + k * stride ||
          other.dest_height_aligned + k * uint32_t(band_rows) !=
              resolve.dest_height_aligned) {
        failure_strip_ = k;
        Fail("strip-own resolve destinations don't follow each other");
        return false;
      }
    }
  }
  // A whole number of EDRAM tile rows follows from whole 32-row texture tiles.
  if (!layout_out.band_tile_rows()) {
    Fail("strips not a whole number of EDRAM tile rows");
    return false;
  }
  return true;
}

bool StripMerge::CheckOwnValues(uint32_t reg, const uint32_t* values,
                                uint8_t kind, int32_t& band_rows,
  // What's read of hi-Z control is the enable bit, by draws (HashDrawState):
  // 4D5307D5 sets other hi-Z bases per tile.
  uint32_t mask = UINT32_MAX;
  if (reg == XE_GPU_REG_RB_HIZCONTROL) {
    mask = (kind & kDrawKindResolve) ? 0 : kHiZEnableBit;
  }
  bool equal = true;
  for (uint32_t k = 1; k < strip_count_; ++k) {
    equal &= ((values[k] ^ values[0]) & mask) == 0;
  }
  if (equal) {
    // The same in every strip, as if shared.
    return true;
  }
  auto set_band_rows = [&](int32_t rows) {
    if (rows <= 0 || (band_rows && band_rows != rows)) {
      return false;
    }
    band_rows = rows;
    return true;
  };
  bool resolve = (kind & kDrawKindResolve) != 0;
  bool own = (kind & kDrawKindOwn) != 0;
  if (reg == XE_GPU_REG_RB_COPY_DEST_BASE) {
    // Only resolves read it. The strip-own ones are compared with their
    // destinations in DeriveLayout; a resolve of every strip runs once when
    // merging, so it must resolve to the same place in every strip.
    if (resolve && !own) {
      Fail("a resolve in every strip has a strip-own destination");
      return false;
    }
    return true;
  }
  if (resolve && !own) {
    // A resolve of every strip runs once when merging, with the first strip's
    // registers.
    if (reg == XE_GPU_REG_PA_SC_WINDOW_OFFSET ||
        reg == XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL ||
        reg == XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR) {
      Fail("a resolve in every strip sees the strip's own window");
      return false;
    }
  }
  int32_t values_x[kMaxStrips], values_y[kMaxStrips];
  int32_t slope_x, slope_y;
  switch (reg) {
    case XE_GPU_REG_PA_SC_WINDOW_OFFSET:
      for (uint32_t k = 0; k < strip_count_; ++k) {
        values_x[k] = WindowOffsetX(values[k]);
        values_y[k] = WindowOffsetY(values[k]);
      }
          !set_band_rows(-slope_y)) {
        Fail("the window offset isn't the strip's frame row");
        return false;
      }
      return true;
    case XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL:
      for (uint32_t k = 0; k < strip_count_; ++k) {
        // The window offset disable bit of TL goes with x.
        values_x[k] = int32_t(values[k] & 0x8000FFFF);
        values_y[k] = int32_t((values[k] >> 16) & 0x3FFF);
      }
        Fail("the window scissor isn't the strip's rows");
        return false;
      }
          return false;
        }
        window_br_first = values[0];
      }
      return true;
    default:
      break;
  }
  if (reg < XE_GPU_REG_SHADER_CONSTANT_000_X ||
      reg >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) {
    Fail("a strip-own write to a register merging can't vary");
    return false;
  }
  if (resolve) {
    // Resolves run no shaders.
    return true;
  }
  // A float constant component that is the strip's frame row (or its
  // negation), 0 in the first strip, so that the merged first strip's
  // frame-space pixels get the value of every strip's.
  float floats[kMaxStrips];
  for (uint32_t k = 0; k < strip_count_; ++k) {
    std::memcpy(&floats[k], &values[k], sizeof(float));
  }
  float slope = floats[1] - floats[0];
  bool linear = floats[0] == 0.0f && slope != 0.0f &&
                float(int32_t(slope)) == slope;
  for (uint32_t k = 2; linear && k < strip_count_; ++k) {
    linear = floats[k] == slope * float(k);
  }
  if (!linear || !set_band_rows(std::abs(int32_t(slope)))) {
    Fail("a strip-own constant isn't the strip's frame row");
    return false;
  }
  return true;
}

  // So a vertex is 7 dwords: x, y, z, then the interpolated color.
      first.vertex_shader_hash != kD3DTilingClearVertexShader ||
      last.vertex_shader_hash != kD3DTilingClearVertexShader ||
  const Strip& first = strips_[0];
  const Strip& strip = strips_[index];
  auto classify = [this](const Record& record, uint64_t bits) {
    uint64_t mask = record.mask & strip_bits_union_;
    if (mask == strip_bits_union_) {
      return 0;  // Shared.
    }
    if (mask == bits) {
      return 1;  // Own, compared through own_opcodes and own_writes.
      return 1;  // Own, compared through what the draws see of them.
    }
    if (!(mask & bits)) {
      return 1;  // Another strip's, or first tile only.
    }
    return 2;  // Partial.
  };
  size_t i = 0, j = 0;
  for (;;) {
    while (i < first.records.size()) {
      int cls = classify(first.records[i], first.bits);
      if (cls == 2) {
        failure_strip_ = 0;
        Fail("a bin mask covering some strips but not all");
        return false;
      }
      if (!cls) {
        break;
      }
      ++i;
    }
    while (j < strip.records.size()) {
      int cls = classify(strip.records[j], strip.bits);
      if (cls == 2) {
        failure_strip_ = index;
        Fail("a bin mask covering some strips but not all");
        return false;
      }
      if (!cls) {
        break;
      }
      ++j;
    }
    bool first_end = i >= first.records.size();
    bool strip_end = j >= strip.records.size();
    if (first_end || strip_end) {
      if (first_end != strip_end) {
        failure_strip_ = index;
        Fail("strips with different shared packet counts");
        return false;
      }
      break;
    }
    if (first.records[i].hash != strip.records[j].hash ||
        first.records[i].opcode != strip.records[j].opcode) {
      failure_strip_ = index;
      if (cvars::strip_merge_log) {
        XELOGI("Strip merge: strip {} shared packet {} (opcode {:02X}) differs",
               index, j, strip.records[j].opcode);
      }
      Fail("shared rendering state differs between strips");
      return false;
    }
    ++i;
    ++j;
  }
  if (first.own_draw_vertices != strip.own_draw_vertices) {
    failure_strip_ = index;
    failure_strip_ = index;
    Fail("a strip-own draw's vertices differ between strips");
    return false;
  }
  if (strip.draws != first.draws) {
    failure_strip_ = index;
    Fail("strips with different draw counts");
    return false;
  }
  // Learning sessions hash every draw's state; merged sessions only what it
  // sees of the strips' own registers (the later strips skip their draws).
  if (first.draw_states.size() != strip.draw_states.size() ||
      first.draw_own_states.size() != strip.draw_own_states.size()) {
    failure_strip_ = index;
    Fail("strips with different draw counts");
    return false;
  }
  for (size_t d = 0; d < first.draw_states.size(); ++d) {
    if (first.draw_states[d] != strip.draw_states[d]) {
      failure_strip_ = index;
      if (cvars::strip_merge_log) {
        XELOGI("Strip merge: strip {} draw {} sees different state", index,
               d);
      }
      Fail("a draw sees different inherited state between strips");
      return false;
    }
  }
  for (size_t d = 0; d < first.draw_own_states.size(); ++d) {
    if (first.draw_own_states[d] != strip.draw_own_states[d]) {
      failure_strip_ = index;
      if (cvars::strip_merge_log) {
        XELOGI("Strip merge: strip {} draw {} sees different own state",
               index, d);
      }
      Fail("a draw sees different strip-own state between strips");
      return false;
    }
  }
  return true;
}

void StripMerge::OnPacketType3(uint32_t header, uint32_t opcode,
                               uint64_t bin_mask, uint64_t bin_select,
                               const RingBuffer::ReadRange& payload) {
  current_packet_own_ = false;
  RestoreWindowScissor();
  bool predicated = (header & 1) != 0;
  if (opcode == xenos::PM4_XE_SWAP) {
    if (!predicated) {
      EndSession();
    }
    return;
  }
  PacketPolicy policy = GetPacketPolicy(opcode);
  if (policy == PacketPolicy::kIgnore) {
    return;
  }
  bool executed = !predicated || (bin_select & bin_mask) != 0;
  if (policy == PacketPolicy::kViolation) {
    if (executed) {
      Fail("a visibility query or bin draw");
    }
    return;
  }
  Strip& strip = strips_[strip_index_];
  strip.records.push_back(
      {predicated ? bin_mask : ~uint64_t(0), HashPayload(payload, header),
       opcode});
  if (!executed) {
    return;
  }
  if (opcode == xenos::PM4_DRAW_INDX && PayloadDwordCount(payload) &&
      (PayloadDword(payload, 0) & (uint32_t(1) << 8))) {
    Fail("a draw conditional on a visibility query");
  }
  if (!predicated || !(bin_mask & strip.bits)) {
    // Shared, or executed for the first tile only.
    MarkOwnStale(opcode, payload);
    return;
  }
  // Masks naming another strip are shared (and compared as such at the end).
  // Without a layout yet, any other bit counts as another strip's.
  uint64_t other_strips = candidate_.IsValid()
                              ? candidate_.strip_bits_union
                              : ~kFirstTileBit;
  if (bin_mask & ~(strip.bits | kFirstTileBit) & other_strips) {
    MarkOwnStale(opcode, payload);
    return;
  }
  current_packet_own_ = true;
  // The strips are compared by what their draws see of these (BeforeDraw), so
  // own writes may come in a different order or number in each strip (Halo 3
  // sets RB_COPY_DEST_BASE to the first strip's value, then writes every
  // strip's own value, before each strip's resolve).
    return;
  }
  strip.own_opcodes.push_back(opcode);
  switch (opcode) {
    case xenos::PM4_SET_CONSTANT:
      RecordOwnSetConstant(payload);
      break;
    case xenos::PM4_DRAW_INDX:
    case xenos::PM4_DRAW_INDX_2:
      break;
    default:
      Fail("a strip-own packet other than SET_CONSTANT or a draw");
      break;
  }
}

bool StripMerge::SetConstantRange(const RingBuffer::ReadRange& payload,
                                  uint32_t& base_out, uint32_t& count_out) {
  uint32_t count = PayloadDwordCount(payload);
  if (!count) {
    return false;
  }
  uint32_t offset_type = PayloadDword(payload, 0);
  uint32_t index = offset_type & 0x7FF;
  switch ((offset_type >> 16) & 0xFF) {
    case 0:
      base_out = XE_GPU_REG_SHADER_CONSTANT_000_X + index;
      break;
    case 4:
      base_out = 0x2000 + index;
      break;
    default:
      return false;
  }
  count_out = count - 1;
  return true;
}

void StripMerge::MarkOwnStale(uint32_t base, uint32_t count) {
  for (OwnLatest& own : strips_[strip_index_].own_shadow) {
    if (own.reg >= base && own.reg - base < count) {
      own.stale = true;
    }
  }
}

void StripMerge::MarkOwnStale(uint32_t opcode,
                              const RingBuffer::ReadRange& payload) {
  uint32_t base, count;
  if (opcode == xenos::PM4_SET_CONSTANT &&
      SetConstantRange(payload, base, count)) {
    MarkOwnStale(base, count);
  }
}

void StripMerge::MarkOwnRestored(Strip& strip,
                                 const RingBuffer::ReadRange& payload) {
  uint32_t base, count;
  if (!SetConstantRange(payload, base, count)) {
    return;
  }
  for (OwnLatest& own : strip.own_shadow) {
    if (own.reg >= base && own.reg - base < count) {
      own.stale = false;
    }
  }
}

uint64_t StripMerge::HashDrawOwnState(Strip& strip, uint8_t kind) {
  // The kind of draw; for each register the strip wrote itself, whether it
  // still holds the strip's latest own value or else the value shared code
  // put there; and the registers HashDrawState leaves out that the strip
  // hasn't written itself. The own values the draw sees go to a snapshot,
  // compared across the strips in DeriveLayout.
  constexpr uint32_t kExcluded[] = {
      XE_GPU_REG_PA_SC_WINDOW_OFFSET, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL,
      XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, XE_GPU_REG_RB_COPY_DEST_BASE};
  std::vector<uint32_t>& data = draw_own_scratch_;
  data.clear();
  data.push_back(kind);
  uint32_t snapshot_start = uint32_t(strip.own_snapshot_values.size());
  for (const OwnLatest& own : strip.own_shadow) {
    uint32_t value = register_file_[own.reg];
    bool overwritten = own.stale || value != own.value;
    if (!overwritten) {
      strip.own_snapshot_values.push_back({own.reg, value});
    }
    data.push_back(own.reg);
    data.push_back(uint32_t(overwritten));
    data.push_back(overwritten ? value : 0);
  }
  for (uint32_t reg : kExcluded) {
    // A strip-own resolve's destination is compared with the other strips'
    // in DeriveLayout (own_resolves), wherever it was written: 434307D2 sets
    // it with type-0 writes in per-tile blocks, every strip running all the
    // blocks' writes and its own block's resolve.
    if (reg == XE_GPU_REG_RB_COPY_DEST_BASE &&
        kind == (kDrawKindOwn | kDrawKindResolve)) {
      continue;
    }
    if (std::none_of(strip.own_shadow.cbegin(), strip.own_shadow.cend(),
                     [reg](const OwnLatest& w) { return w.reg == reg; })) {
      data.push_back(register_file_[reg]);
    }
  }
  // Reuse the previous draw's snapshot if this one sees the same.
  uint32_t snapshot_count = uint32_t(strip.own_snapshot_starts.size());
  if (snapshot_count) {
    uint32_t previous_start = strip.own_snapshot_starts.back();
    uint32_t snapshot_size =
        uint32_t(strip.own_snapshot_values.size()) - snapshot_start;
    if (snapshot_start - previous_start == snapshot_size &&
        std::equal(strip.own_snapshot_values.cbegin() + previous_start,
                   strip.own_snapshot_values.cbegin() + snapshot_start,
                   strip.own_snapshot_values.cbegin() + snapshot_start)) {
      strip.own_snapshot_values.resize(snapshot_start);
      strip.draw_own_snapshots.push_back(snapshot_count - 1);
      return XXH3_64bits(data.data(), data.size() * sizeof(uint32_t));
    }
  }
  strip.own_snapshot_starts.push_back(snapshot_start);
  strip.draw_own_snapshots.push_back(snapshot_count);
  return XXH3_64bits(data.data(), data.size() * sizeof(uint32_t));
}

void StripMerge::RecordOwnSetConstant(const RingBuffer::ReadRange& payload) {
  uint32_t base, count;
  if (!SetConstantRange(payload, base, count)) {
    if (PayloadDwordCount(payload)) {
      Fail("strip-own fetch, bool or loop constants");
    }
    return;
  }
  Strip& strip = strips_[strip_index_];
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t reg = base + i;
    uint32_t value = PayloadDword(payload, 1 + i);
    uint32_t write_index = uint32_t(strip.own_writes.size());
    strip.own_writes.push_back({reg, value});
    auto it = std::find_if(strip.own_shadow.begin(), strip.own_shadow.end(),
                           [reg](const OwnLatest& own) { return own.reg == reg; });
    if (it != strip.own_shadow.end()) {
    auto it = std::lower_bound(
        strip.own_shadow.begin(), strip.own_shadow.end(), reg,
        [](const OwnLatest& own, uint32_t reg) { return own.reg < reg; });
    if (it != strip.own_shadow.end() && it->reg == reg) {
      it->value = value;
      it->stale = false;
      continue;
    }
    strip.own_shadow.insert(it, {reg, value});
      strip.own_float_registers.push_back(reg);
      uint32_t vector_index = (reg - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (std::find(own_float_constants_.cbegin(), own_float_constants_.cend(),
                    vector_index) == own_float_constants_.cend()) {
        own_float_constants_.push_back(vector_index);
      }
    }
  }
}

void StripMerge::OnPacketType01(uint32_t header,
                                const RingBuffer::ReadRange& payload) {
  // Writes below the rendering context (CP, scratch and handshake registers)
  // aren't rendering state.
  if (!(header >> 30) && (header & 0x7FFF) < 0x2000) {
    return;
  }
  if (!(header >> 30)) {
    uint32_t base = header & 0x7FFF;
    uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    MarkOwnStale(base, (header & (uint32_t(1) << 15)) ? 1 : count);
  }
  strips_[strip_index_].records.push_back(
      {~uint64_t(0), HashPayload(payload, header), UINT32_MAX});
}

bool StripMerge::BeforeDraw() {
  Strip& strip = strips_[strip_index_];
  ++strip.draws;
  if (mode_ == Mode::kRecovered) {
    return true;
  }
  // What the draw sees of the strips' own registers, compared in every
  // session (merged ones too: the later strips' skipped draws still see their
  // registers). The window scissor is extended below, after this.
  uint8_t kind = current_packet_own_ ? kDrawKindOwn : 0;
  if (register_file_.Get<reg::RB_MODECONTROL>().edram_mode ==
      xenos::EdramMode::kCopy) {
    kind |= kDrawKindResolve;
  }
  strip.draw_kinds.push_back(kind);
  strip.draw_own_states.push_back(HashDrawOwnState(strip, kind));
  if (mode_ == Mode::kMerged) {
    if (strip_index_) {
      // The merged first strip drew and resolved this strip's rows.
      return false;
    }
    // Extended for this draw only: the next packet restores it.
    uint32_t& window_br =
        register_file_.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    if (window_br == layout_.window_br_first) {
      window_br = layout_.window_br_frame();
      window_br_extended_ = true;
    }
    return true;
  }
  strip.draw_states.push_back(HashDrawState());
  return true;
}

void StripMerge::RestoreWindowScissor() {
  if (!window_br_extended_) {
    return;
  }
  window_br_extended_ = false;
  uint32_t& window_br =
      register_file_.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  if (window_br == layout_.window_br_frame()) {
    window_br = layout_.window_br_first;
  }
}

uint64_t StripMerge::HashDrawState(const DrawInfo* draw) {
  if (register_file_.Get<reg::RB_MODECONTROL>().edram_mode ==
      xenos::EdramMode::kCopy) {
    return HashResolveState();
  }
  // The rendering context registers and the shader constants, without the
  // strip-own ones.
  constexpr uint32_t kContextFirst = 0x2000, kContextCount = 0x400;
  constexpr uint32_t kConstantsFirst = XE_GPU_REG_SHADER_CONSTANT_000_X;
  constexpr uint32_t kConstantsCount =
      XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1 - kConstantsFirst;
  draw_state_scratch_.resize(kContextCount + kConstantsCount);
  uint32_t* scratch = draw_state_scratch_.data();
  const uint32_t* values = register_file_.values;
  std::memcpy(scratch, values + kContextFirst,
              kContextCount * sizeof(uint32_t));
  std::memcpy(scratch + kContextCount, values + kConstantsFirst,
              kConstantsCount * sizeof(uint32_t));
  auto exclude = [&](uint32_t reg) {
    if (reg >= kContextFirst && reg < kContextFirst + kContextCount) {
      scratch[reg - kContextFirst] = 0;
    } else if (reg >= kConstantsFirst &&
               reg < kConstantsFirst + kConstantsCount) {
      scratch[kContextCount + (reg - kConstantsFirst)] = 0;
    }
  };
  exclude(XE_GPU_REG_PA_SC_WINDOW_OFFSET);
  exclude(XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
  exclude(XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
  exclude(XE_GPU_REG_RB_COPY_DEST_BASE);
  // Written by events, not read by draws.
  exclude(XE_GPU_REG_VGT_EVENT_INITIATOR);
  // The copy destination height: Halo 3 sets it, shrinking by a strip, for
  // each strip's resolve. Strip-own resolves are compared with it
  // (DeriveLayout, dest_height_aligned); draws don't read it.
  scratch[XE_GPU_REG_RB_COPY_DEST_PITCH - kContextFirst] &= 0x3FFF;
  // Whether a resolve clears: draws don't read it. Draws after a clearing
  // strip-own resolve see its value: 545407F2 clears in every strip but the
  // last, so the second strip's first draw sees the first strip's clear and
  // the first strip's sees the frame's.
  scratch[XE_GPU_REG_RB_COPY_CONTROL - kContextFirst] &= ~uint32_t(0x300);
  // Hi-Z control: only visibility survey draws read it, its enable bit
  // (GetNormalizedDepthControl). 425307D5's last tile turns hi-Z off before
  // its resolve, 4D53082D's later tiles don't write hi-Z.
  scratch[XE_GPU_REG_RB_HIZCONTROL - kContextFirst] &= kHiZEnableBit;
  // The index buffer of the last indexed draw: a draw packet sets it when it
  // has indices in memory, and other draws don't read it (Halo 3's first draw
  // in every strip is auto-indexed after the previous strip's indexed ones).
  if (register_file_.Get<reg::VGT_DRAW_INITIATOR>().source_select !=
      xenos::SourceSelect::kDMA) {
    exclude(XE_GPU_REG_VGT_DMA_BASE);
    exclude(XE_GPU_REG_VGT_DMA_SIZE);
  }
  for (uint32_t reg : strips_[strip_index_].own_float_registers) {
    exclude(reg);
  }
  if (current_packet_own_) {
    // A strip's own draw may take its vertices from its own copy of the same
    // data (Halo 3's clear quads: one 84-byte copy per strip): CheckDraw
    // compares the vertices the shader fetches, so the vertex fetch addresses
    // are left out here. A 6-dword fetch constant is a texture or three
    // vertex fetches.
    uint32_t* fetch = scratch + kContextCount +
                      (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 - kConstantsFirst);
    for (uint32_t group = 0; group < 32; ++group) {
      uint32_t* words = fetch + group * 6;
      if ((words[0] & 3) == uint32_t(xenos::FetchConstantType::kTexture)) {
        continue;
      }
      for (uint32_t pair = 0; pair < 3; ++pair) {
        uint32_t& dword_0 = words[pair * 2];
        if ((dword_0 & 3) == uint32_t(xenos::FetchConstantType::kVertex)) {
          dword_0 = 3;
        }
      }
    }
  }
  if (draw) {
    // The constants the draw's shaders don't read don't matter to it:
    // 545407F2's tiles start with constants and fetch constants the previous
    // tile's draws left, which the tile's first draw (D3D's clear quad, whose
    // vertex shader reads c0 only in a `max r0._, c0, c0` writing nothing)
    // doesn't read.
    auto keep_read = [&](const Shader* shader, uint32_t first_vector) {
      uint32_t* floats = scratch + kContextCount +
                         (XE_GPU_REG_SHADER_CONSTANT_000_X - kConstantsFirst) +
                         first_vector * 4;
      if (!shader) {
        std::memset(floats, 0, 256 * 4 * sizeof(uint32_t));
        return;
      }
      const Shader::ConstantRegisterMap& map = shader->constant_register_map();
      if (map.float_dynamic_addressing) {
        return;
      }
      const uint64_t* read = shader->float_constants_with_effect();
      for (uint32_t i = 0; i < 256; ++i) {
        if (!((read[i >> 6] >> (i & 63)) & 1)) {
          std::memset(floats + i * 4, 0, 4 * sizeof(uint32_t));
        }
      }
    };
    keep_read(draw->vertex_shader, 0);
    keep_read(draw->pixel_shader, 256);
    // Fetch constants: a texture fetch reads its 6 dwords, a vertex fetch its
    // 2 (three vertex fetch constants share a texture one's place).
    constexpr uint32_t kFetchDwords = 32 * 6;
    uint64_t fetch_read[(kFetchDwords + 63) / 64] = {};
    auto mark_fetch = [&fetch_read](uint32_t first_dword, uint32_t count) {
      for (uint32_t i = first_dword; i < first_dword + count; ++i) {
        fetch_read[i >> 6] |= uint64_t(1) << (i & 63);
      }
    };
    for (const Shader* shader : {draw->vertex_shader, draw->pixel_shader}) {
      if (!shader) {
        continue;
      }
      const Shader::ConstantRegisterMap& map = shader->constant_register_map();
      for (uint32_t i = 0; i < 96; ++i) {
        if ((map.vertex_fetch_bitmap[i >> 5] >> (i & 31)) & 1) {
          mark_fetch(i * 2, 2);
        }
      }
      for (const Shader::VertexBinding& binding : shader->vertex_bindings()) {
        mark_fetch(binding.fetch_constant * 2, 2);
      }
      for (const Shader::TextureBinding& binding :
           shader->texture_bindings()) {
        mark_fetch(binding.fetch_constant * 6, 6);
      }
    }
    uint32_t* fetch = scratch + kContextCount +
                      (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 - kConstantsFirst);
    for (uint32_t i = 0; i < kFetchDwords; ++i) {
      if (!((fetch_read[i >> 6] >> (i & 63)) & 1)) {
        fetch[i] = 0;
      }
    }
    // Bool and loop constants: the bits and loops their control flow reads
    // (545407F2's tiles start with the loop constants the previous tile
    // left).
    uint32_t bool_read[8] = {};
    uint32_t loop_read = 0;
    for (const Shader* shader : {draw->vertex_shader, draw->pixel_shader}) {
      if (!shader) {
        continue;
      }
      const Shader::ConstantRegisterMap& map = shader->constant_register_map();
      for (uint32_t i = 0; i < 8; ++i) {
        bool_read[i] |= map.bool_bitmap[i];
      }
      loop_read |= map.loop_bitmap;
    }
    uint32_t* bools =
        scratch + kContextCount +
        (XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 - kConstantsFirst);
    for (uint32_t i = 0; i < 8; ++i) {
      bools[i] &= bool_read[i];
    }
    uint32_t* loops = scratch + kContextCount +
                      (XE_GPU_REG_SHADER_CONSTANT_LOOP_00 - kConstantsFirst);
    for (uint32_t i = 0; i < 32; ++i) {
      if (!((loop_read >> i) & 1)) {
        loops[i] = 0;
      }
    }
  }
  return XXH3_64bits(scratch, draw_state_scratch_.size() * sizeof(uint32_t));
}

uint64_t StripMerge::HashResolveState() const {
  // A resolve runs no shaders: it reads what draw_util::GetResolveInfo does.
  // D3D's predicated tiling runs each tile's resolves in a block of its own,
  // then restores the constants and interpolator control the block's copy
  // draw changed: the first tile's resolves see the state of before the
  // blocks, the others the restored state (545407F2).
  constexpr uint32_t kRegisters[] = {
      XE_GPU_REG_RB_MODECONTROL,       XE_GPU_REG_RB_COPY_CONTROL,
      XE_GPU_REG_RB_COPY_DEST_PITCH,   XE_GPU_REG_RB_COPY_DEST_INFO,
      XE_GPU_REG_RB_COPY_FUNC,         XE_GPU_REG_RB_COPY_REF,
      XE_GPU_REG_RB_COPY_MASK,         XE_GPU_REG_RB_COPY_SURFACE_SLICE,
      XE_GPU_REG_RB_SURFACE_INFO,      XE_GPU_REG_RB_COLOR_INFO,
      XE_GPU_REG_RB_COLOR1_INFO,       XE_GPU_REG_RB_COLOR2_INFO,
      XE_GPU_REG_RB_COLOR3_INFO,       XE_GPU_REG_RB_DEPTH_INFO,
      XE_GPU_REG_PA_SU_SC_MODE_CNTL,   XE_GPU_REG_PA_SU_VTX_CNTL,
      XE_GPU_REG_RB_COLOR_CLEAR,       XE_GPU_REG_RB_COLOR_CLEAR_LO,
      XE_GPU_REG_RB_DEPTH_CLEAR,       XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
      XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1,
  };
  uint32_t data[std::size(kRegisters)];
  const uint32_t* values = register_file_.values;
  for (size_t i = 0; i < std::size(kRegisters); ++i) {
    data[i] = values[kRegisters[i]];
  }
  auto set = [&](uint32_t reg, uint32_t value) {
    for (size_t i = 0; i < std::size(kRegisters); ++i) {
      if (kRegisters[i] == reg) {
        data[i] = value;
      }
    }
  };
  // The copy destination height shrinks by a strip each strip (see
  // HashDrawState).
  set(XE_GPU_REG_RB_COPY_DEST_PITCH,
      values[XE_GPU_REG_RB_COPY_DEST_PITCH] & 0x3FFF);
  constexpr uint32_t kClearBits = 0x300;
  if (current_packet_own_) {
    // A strip-own resolve: whether it clears and with what is compared in
    // DeriveLayout (the last strip's may not clear), and its rectangle's
    // vertices are each strip's own copy, compared as coordinates there.
    set(XE_GPU_REG_RB_COPY_CONTROL,
        values[XE_GPU_REG_RB_COPY_CONTROL] & ~kClearBits);
    set(XE_GPU_REG_RB_COLOR_CLEAR, 0);
    set(XE_GPU_REG_RB_COLOR_CLEAR_LO, 0);
    set(XE_GPU_REG_RB_DEPTH_CLEAR, 0);
    set(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 3);
  } else if (!(values[XE_GPU_REG_RB_COPY_CONTROL] & kClearBits)) {
    // The clear values only matter to resolves that clear.
    set(XE_GPU_REG_RB_COLOR_CLEAR, 0);
    set(XE_GPU_REG_RB_COLOR_CLEAR_LO, 0);
    set(XE_GPU_REG_RB_DEPTH_CLEAR, 0);
  }
  // The strips' own registers among these are compared by what the resolve
  // sees of them (HashDrawOwnState).
  for (const OwnLatest& own : strips_[strip_index_].own_shadow) {
    set(own.reg, 0);
  }
  return XXH3_64bits(data, sizeof(data));
}

bool StripMerge::IsBanded(uint32_t pitch_tiles_at_32bpp,
                          xenos::MsaaSamples msaa_samples) const {
  const Layout& checked = layout();
  return checked.IsValid() && checked.pitch_tiles_at_32bpp &&
         pitch_tiles_at_32bpp == checked.pitch_tiles_at_32bpp &&
         msaa_samples == checked.msaa_samples;
}

bool StripMerge::IsSurfaceBanded() const {
  auto rb_surface_info = register_file_.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch_tiles_at_32bpp =
      ((rb_surface_info.surface_pitch
        << uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X)) +
       (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  return IsBanded(pitch_tiles_at_32bpp, rb_surface_info.msaa_samples);
}

bool StripMerge::IsFrameSpace() const {
  uint32_t window_br =
      register_file_.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  return mode_ == Mode::kMerged ? window_br == layout_.window_br_frame()
                                : window_br == candidate_.window_br_first;
}

bool StripMerge::ReadsOwnConstants(const Shader* shader,
                                   bool is_pixel_shader) const {
  if (!shader) {
    return false;
  }
  const Shader::ConstantRegisterMap& map = shader->constant_register_map();
  for (uint32_t vector_index : own_float_constants_) {
    // Vertex shaders read c0-c255, pixel shaders c256-c511.
    if ((vector_index >= 256) != is_pixel_shader) {
      continue;
    }
    uint32_t local_index = vector_index & 255;
    if (map.float_dynamic_addressing ||
        (map.float_bitmap[local_index >> 6] >> (local_index & 63)) & 1) {
      return true;
    }
  }
  return false;
}

void StripMerge::CheckDraw(const DrawInfo& draw) {
  if (mode_ == Mode::kLearn && session_open_) {
    // The draw's state as its shaders read it (BeforeDraw hashed every
    // constant before the shaders were known).
    if (strip.draws && strip.draw_states.size() == strip.draws) {
      strip.draw_states.back() = HashDrawState(&draw);
    }
  }
  if (mode_ == Mode::kLearn && current_packet_own_ && session_open_) {
    // The vertices of a strip-own draw (HashDrawState leaves out where they
    // are): the contents of every vertex fetch range the vertex shader uses.
    uint64_t vertices = 0;
    if (draw.vertex_shader) {
      constexpr uint32_t kMaxHashedBytes = 64 * 1024;
      for (const Shader::VertexBinding& binding :
           draw.vertex_shader->vertex_bindings()) {
        uint32_t reg = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                       binding.fetch_constant * 2;
        uint32_t dword_0 = register_file_[reg];
        uint32_t dword_1 = register_file_[reg + 1];
        uint32_t bytes = ((dword_1 >> 2) & 0xFFFFFF) * 4;
        uint64_t range[3] = {binding.fetch_constant, dword_1, 0};
        if (bytes && bytes <= kMaxHashedBytes) {
          range[2] = XXH3_64bits(memory_->TranslatePhysical<const uint8_t*>(
                                     dword_0 & ~uint32_t(3)),
                                 bytes);
        } else {
          range[2] = dword_0;
        }
        vertices = XXH3_64bits_withSeed(range, sizeof(range), vertices);
      }
    }
    strips_[strip_index_].own_draw_vertices.push_back(vertices);
  }
  if (!checking_draws()) {
    return;
  }
  if (draw.memexport) {
    Fail("memexport");
    return;
  }
  bool band_addressed = draw.banded && !draw.frame_space;
  if (band_addressed) {
    if (draw.tessellated) {
      Fail("a tessellated draw addressing the strip's band");
      return;
    }
    if (draw.pixel_position) {
      Fail("a draw addressing the strip's band reads the pixel position");
      return;
    }
  }
  if ((!draw.banded || band_addressed) &&
      (ReadsOwnConstants(draw.vertex_shader, false) ||
       ReadsOwnConstants(draw.pixel_shader, true))) {
    Fail("a draw that doesn't address the frame reads strip-own constants");
  }
}

uint32_t StripMerge::NoteResolve(TraceWriter& trace_writer) {
  bool banded = IsSurfaceBanded();
  bool own = current_packet_own();
  if (merging()) {
    if (failed()) {
      return 0;
    }
    if (banded && own) {
      return layout().frame_bottom();
    }
    if (banded) {
      // Every strip resolving its band: the later strips' resolves are
      // skipped, so their rows would be missing.
      Fail("a resolve of the strip's band in every strip");
    }
    return 0;
  }
  if (!learning()) {
    return 0;
  }
  if (!own) {
    if (banded) {
      Fail("a resolve of the strip's band in every strip");
    }
    return 0;
  }
  // Learning: what DeriveLayout compares across the strips. No trace is being
  // written while recording, so this logs no memory reads.
  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(register_file_, *memory_, trace_writer, 1, 1,
                                 false, false, resolve_info)) {
    Fail("a strip-own resolve that can't be decoded");
    return 0;
  }
  const draw_util::ResolveEdramInfo& edram_info =
      resolve_info.IsCopyingDepth() ? resolve_info.depth_edram_info
                                    : resolve_info.color_edram_info;
  ResolveRecord record;
  record.edram_info = edram_info.packed;
  record.coordinate_info = resolve_info.coordinate_info.packed;
  record.height_div_8 = resolve_info.height_div_8;
  record.copy_dest_info = resolve_info.copy_dest_info.value;
  draw_util::ResolveCopyDestCoordinateInfo dest_coordinates =
      resolve_info.copy_dest_coordinate_info;
  record.dest_height_aligned = dest_coordinates.height_aligned_div_32 << 5;
  dest_coordinates.height_aligned_div_32 = 0;
  record.copy_dest_coordinate_info = dest_coordinates.packed;
  record.copy_dest_base = resolve_info.copy_dest_base;
  record.copy_control = resolve_info.rb_copy_control.value;
  record.dest_pitch_aligned =
      resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32 << 5;
  record.dest_offset_y =
      resolve_info.copy_dest_coordinate_info.offset_y_div_8 << 3;
  record.dest_bytes_per_pixel =
      FormatInfo::Get(
          xenos::TextureFormat(resolve_info.copy_dest_info.copy_dest_format))
          ->bits_per_pixel >>
      3;
  record.edram_offset_y = resolve_info.coordinate_info.edram_offset_y_div_8
                          << 3;
  record.pitch_tiles_at_32bpp = edram_info.pitch_tiles;
  record.msaa_samples = edram_info.msaa_samples;
  record.is_64bpp = edram_info.format_is_64bpp != 0;
  record.clears =
      resolve_info.IsClearingDepth() || resolve_info.IsClearingColor();
  strips_[strip_index_].own_resolves.push_back(record);
  return 0;
}

void StripMerge::Fail(const char* reason) {
  if (!session_open_ || failed_) {
    return;
  }
  failed_ = true;
  failure_reason_ = reason;
  failure_strip_ = strip_index_;
  // EndSession warns once for the session.
  if (mode_ == Mode::kMerged && cvars::strip_merge_log) {
    XELOGI("Strip merge: {} in strip {} of a merged session{}", reason,
           strip_index_,
           strip_index_ ? ", the frame may be wrong" : ", recovering");
  }
}

}  // namespace gpu
}  // namespace xe
