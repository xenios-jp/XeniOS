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
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"

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
  own_writes.clear();
  own_resolves.clear();
  draw_states.clear();
  draw_own_states.clear();
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
      return false;
    }
    band_rows = rows;
    return true;
  };
  int32_t values_x[kMaxStrips], values_y[kMaxStrips];
  for (size_t j = 0; j < first.own_writes.size(); ++j) {
    uint32_t reg = first.own_writes[j].reg;
    uint32_t value_first = first.own_writes[j].value;
    bool equal = true;
    for (uint32_t k = 1; k < strip_count_; ++k) {
      const OwnWrite& write = strips_[k].own_writes[j];
      if (write.reg != reg) {
        failure_strip_ = k;
        Fail("strip-own writes differ between strips");
        return false;
      }
      equal &= write.value == value_first;
    }
    if (equal) {
      // The same in every strip, as if shared.
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
          return false;
        }
        if (reg == XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR) {
          if (window_br_first && window_br_first != value_first) {
            Fail("strips with more than one frame width");
            return false;
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
    }
  }
  if (!band_rows || !window_br_first) {
    Fail("no strip window");
    return false;
  }
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

bool StripMerge::CompareStrip(uint32_t index) {
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
  if (strip.draws != first.draws) {
    failure_strip_ = index;
    Fail("strips with different draw counts");
    return false;
  }
  // Learning sessions hash both states at every draw (BeforeDraw), others
  // none, so equal draw counts give equal sizes.
  for (size_t d = 0; d < first.draw_states.size(); ++d) {
    if (first.draw_states[d] != strip.draw_states[d]) {
      failure_strip_ = index;
      if (cvars::strip_merge_log) {
        XELOGI("Strip merge: strip {} draw {} sees different state", index, d);
      }
      Fail("a draw sees different inherited state between strips");
      return false;
    }
    if (first.draw_own_states[d] != strip.draw_own_states[d]) {
      failure_strip_ = index;
      if (cvars::strip_merge_log) {
        XELOGI("Strip merge: strip {} draw {} sees different own state", index,
               d);
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
    return;
  }
  // Masks naming another strip are shared (and compared as such at the end).
  // Without a layout yet, any other bit counts as another strip's.
  uint64_t other_strips = candidate_.IsValid()
                              ? candidate_.strip_bits_union
                              : ~kFirstTileBit;
  if (bin_mask & ~(strip.bits | kFirstTileBit) & other_strips) {
    return;
  }
  current_packet_own_ = true;
  if (opcode == xenos::PM4_SET_CONSTANT &&
      IsRedundantOwnSetConstant(strip, payload)) {
    // A repeat of the strip's own latest values is left out of the strip-own
    // sequence the strips are compared by. Halo 3 sets RB_COPY_DEST_BASE to
    // the first strip's value, then writes every strip's own value, before
    // each strip's resolve: strip k has its resolve after k + 1 such writes
    // and 3 - k more after it. BeforeDraw compares, at every draw, which
    // registers hold the strip's own values and which hold shared ones, so
    // dropping the repeats can't hide a difference.
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

bool StripMerge::IsRedundantOwnSetConstant(
    const Strip& strip, const RingBuffer::ReadRange& payload) const {
  uint32_t count = PayloadDwordCount(payload);
  if (count < 2) {
    return false;
  }
  uint32_t offset_type = PayloadDword(payload, 0);
  uint32_t index = offset_type & 0x7FF;
  uint32_t base;
  switch ((offset_type >> 16) & 0xFF) {
    case 0:
      base = XE_GPU_REG_SHADER_CONSTANT_000_X + index;
      break;
    case 4:
      base = 0x2000 + index;
      break;
    default:
      return false;
  }
  for (uint32_t i = 1; i < count; ++i) {
    uint32_t reg = base + (i - 1);
    auto it = std::find_if(strip.own_shadow.cbegin(), strip.own_shadow.cend(),
                           [reg](const OwnWrite& own) { return own.reg == reg; });
    if (it == strip.own_shadow.cend() || it->value != PayloadDword(payload, i)) {
      return false;
    }
  }
  return true;
}

uint64_t StripMerge::HashDrawOwnState(const Strip& strip) {
  // How many of the strip's own writes came before the draw; for each register
  // the strip wrote itself, whether it still holds the strip's latest own
  // value or else the value shared code put there; and the registers
  // HashDrawState leaves out that the strip hasn't written itself.
  constexpr uint32_t kExcluded[] = {
      XE_GPU_REG_PA_SC_WINDOW_OFFSET, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL,
      XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR, XE_GPU_REG_RB_COPY_DEST_BASE};
  std::vector<uint32_t>& data = draw_own_scratch_;
  data.clear();
  data.push_back(uint32_t(strip.own_writes.size()));
  for (const OwnWrite& own : strip.own_shadow) {
    uint32_t value = register_file_[own.reg];
    bool overwritten = value != own.value;
    data.push_back(own.reg);
    data.push_back(uint32_t(overwritten));
    data.push_back(overwritten ? value : 0);
  }
  for (uint32_t reg : kExcluded) {
    if (std::none_of(strip.own_shadow.cbegin(), strip.own_shadow.cend(),
                     [reg](const OwnWrite& w) { return w.reg == reg; })) {
      data.push_back(register_file_[reg]);
    }
  }
  return XXH3_64bits(data.data(), data.size() * sizeof(uint32_t));
}

void StripMerge::RecordOwnSetConstant(const RingBuffer::ReadRange& payload) {
  uint32_t count = PayloadDwordCount(payload);
  if (!count) {
    return;
  }
  uint32_t offset_type = PayloadDword(payload, 0);
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t base;
  switch (type) {
    case 0:
      base = XE_GPU_REG_SHADER_CONSTANT_000_X + index;
      break;
    case 4:
      base = 0x2000 + index;
      break;
    default:
      Fail("strip-own fetch, bool or loop constants");
      return;
  }
  Strip& strip = strips_[strip_index_];
  for (uint32_t i = 1; i < count; ++i) {
    uint32_t reg = base + (i - 1);
    uint32_t value = PayloadDword(payload, i);
    strip.own_writes.push_back({reg, value});
    auto it = std::find_if(strip.own_shadow.begin(), strip.own_shadow.end(),
                           [reg](const OwnWrite& own) { return own.reg == reg; });
    if (it != strip.own_shadow.end()) {
      it->value = value;
    } else {
      strip.own_shadow.push_back({reg, value});
    }
    if (type == 0) {
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
  strips_[strip_index_].records.push_back(
      {~uint64_t(0), HashPayload(payload, header), UINT32_MAX});
}

bool StripMerge::BeforeDraw() {
  Strip& strip = strips_[strip_index_];
  ++strip.draws;
  if (mode_ == Mode::kMerged) {
    if (strip_index_) {
      // The merged first strip drew and resolved this strip's rows.
      return false;
    }
    uint32_t& window_br =
        register_file_.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    if (window_br == layout_.window_br_first) {
      window_br = layout_.window_br_frame();
      window_br_extended_ = true;
    }
    return true;
  }
  if (mode_ == Mode::kLearn) {
    strip.draw_states.push_back(HashDrawState());
    strip.draw_own_states.push_back(HashDrawOwnState(strip));
  }
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

uint64_t StripMerge::HashDrawState() {
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
  for (uint32_t reg : strips_[strip_index_].own_float_registers) {
    exclude(reg);
  }
  return XXH3_64bits(scratch, draw_state_scratch_.size() * sizeof(uint32_t));
}

bool StripMerge::IsBanded(uint32_t pitch_tiles_at_32bpp,
                          xenos::MsaaSamples msaa_samples) const {
  const Layout& checked = layout();
  return checked.IsValid() && checked.pitch_tiles_at_32bpp &&
         pitch_tiles_at_32bpp == checked.pitch_tiles_at_32bpp &&
         msaa_samples == checked.msaa_samples;
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

void StripMerge::NoteOwnResolve(const ResolveRecord& resolve) {
  if (!learning()) {
    return;
  }
  strips_[strip_index_].own_resolves.push_back(resolve);
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
