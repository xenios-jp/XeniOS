/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_STRIP_MERGE_H_
#define XENIA_GPU_STRIP_MERGE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "xenia/base/ring_buffer.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

class Shader;

// Strip merge: predicated tiling (D3D9 on Xenos) renders a frame too big for
// the EDRAM as N horizontal strips. The command buffer runs once per strip
// with a bin select naming the strip; predicated packets run when their bin
// mask meets the select. Halo 3 (4D5307E6) draws 1152x640 4x MSAA as 4 strips
// of 160 rows, running every draw once per strip.
//
// Xenia's host render targets have no EDRAM size limit, so the strips can be
// drawn as one: the first strip runs over a render target as tall as the
// frame, the later strips run only for their guest-visible side effects.
//
// - A session starts at a bin select with bit 31 (first tile) and strip bits,
//   continues through selects with disjoint strip bits, and ends at any other
//   select or a swap.
// - Packets are classified by their bin mask against the strip bits: shared
//   (every strip), strip-own (one strip), first-tile-only (bit 31 alone). The
//   rendering-state packets of each strip are hashed (shared) or recorded
//   (strip-own), and compared across strips when they end.
// - Learning: sessions run normally. A session is verified when the shared
//   state of every strip is identical, the strip-own writes are the window
//   offset, window scissor, copy destination base and float constants
//   varying linearly with the strip (the strip's frame row), the strip-own
//   resolves tile the destination, and the draw-level checks pass (see
//   CheckDraw). kLearnSessions verified sessions with the same layout in a
//   row enable merging.
// - Merged session, first strip: the window scissor bottom of the strip's
//   frame-space draws is extended to the frame bottom; render targets at the
//   strip surface pitch and MSAA are "merged" ones, N bands tall, whose EDRAM
//   ownership is the first strip's tiles; draws that address the band itself
//   (window offset 0, like the clear quads) are replicated into every band;
//   strip-own resolves cover every strip's rows into the first strip's
//   destination. Later strips: draws and resolves are skipped, every other
//   packet runs, so the register state and the guest-visible handshake
//   (interrupts, waits, memory writes, events) are those of normal execution.
// - At the end, each merged render target's last band moves into its ordinary
//   twin, so the EDRAM is what the last strip leaves (RenderTargetCache::
//   EndStripMerge).
// - EDRAM carried across strips: a strip starts with the previous strip's
//   EDRAM contents (Halo 3: the base-0 stencil and the 608 GAMMA color are
//   loaded before being overwritten). One pass sequence can't feed band k+1
//   with band k's end state, so band k+1 starts with band k's end state of the
//   previous merged session (the carry). This is exact when consecutive frames
//   are equal, and otherwise one frame late in the pixels a strip leaves to the
//   next.
// - A check failing in the merged first strip recovers: its band 0 becomes the
//   EDRAM, and the later strips run normally. A failure in a later strip of a
//   merged session (only possible if the game changed what it draws between
//   strips) may leave that frame wrong, and pauses merging.
class StripMerge {
 public:
  // Also RenderTargetCache::Transfer::kMaxStripMergeBands.
  static constexpr uint32_t kMaxStrips = 4;
  static constexpr uint64_t kFirstTileBit = uint64_t(1) << 31;

  struct Layout {
    uint32_t strip_count = 0;
    // Guest pixel rows of a strip.
    uint32_t band_rows = 0;
    // The surface the strips render to, as RenderTargetCache keys have it.
    uint32_t pitch_tiles_at_32bpp = 0;
    xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
    // PA_SC_WINDOW_SCISSOR_BR of the first strip's frame-space draws.
    uint32_t window_br_first = 0;
    uint64_t strip_bits[kMaxStrips] = {};
    uint64_t strip_bits_union = 0;

    bool IsValid() const { return strip_count >= 2 && band_rows; }
    uint32_t frame_bottom() const { return strip_count * band_rows; }
    uint32_t window_br_frame() const {
      return (window_br_first & 0x3FFF) | (frame_bottom() << 16);
    }
    uint32_t band_tile_rows() const {
      return band_rows / (msaa_samples >= xenos::MsaaSamples::k2X ? 8 : 16);
    }
    bool operator==(const Layout& other) const;
    bool operator!=(const Layout& other) const { return !(*this == other); }
  };

  class Backend {
   public:
    virtual ~Backend() = default;
    // A merged session's first strip is about to run. False to run the
    // session normally.
    virtual bool StripMergeBegin(const Layout& layout) = 0;
    // Merged rendering ends. Band `band` of every merged render target becomes
    // the EDRAM; with carry, the bands are kept for the next session's later
    // strips (see the carry above).
    virtual void StripMergeEnd(uint32_t band, bool carry) = 0;
  };

  // What CheckDraw needs about the draw's shaders.
  struct DrawInfo {
    const Shader* vertex_shader = nullptr;
    const Shader* pixel_shader = nullptr;
    bool banded = false;
    bool frame_space = false;
    bool memexport = false;
    bool pixel_position = false;
    bool tessellated = false;
  };

  // What NoteOwnResolve compares across strips, from draw_util::ResolveInfo.
  struct ResolveRecord {
    uint32_t edram_info = 0;
    uint32_t coordinate_info = 0;
    uint32_t height_div_8 = 0;
    uint32_t copy_dest_info = 0;
    uint32_t copy_dest_coordinate_info = 0;
    uint32_t copy_dest_base = 0;
    uint32_t copy_control = 0;
    // From copy_dest_coordinate_info (kept there without the height, which
    // shrinks by a strip each strip) and the destination format.
    uint32_t dest_height_aligned = 0;
    uint32_t dest_pitch_aligned = 0;
    uint32_t dest_offset_y = 0;
    uint32_t dest_bytes_per_pixel = 0;
    uint32_t edram_offset_y = 0;
    uint32_t pitch_tiles_at_32bpp = 0;
    xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
    bool is_64bpp = false;
    bool clears = false;
  };

  explicit StripMerge(RegisterFile& register_file)
      : register_file_(register_file) {}

  void SetBackend(Backend* backend) { backend_ = backend; }

  // A session is open, the PM4 hooks below have to run.
  bool recording() const { return session_open_; }

  // PM4 hooks. tracing: a trace is being written (traces record the guest's
  // mask rewrites, a merged trace couldn't be replayed unmerged).
  void OnBinSelect(uint64_t bin_select, bool tracing);
  // Every type 3 packet while recording, before it executes, with its payload.
  void OnPacketType3(uint32_t header, uint32_t opcode, uint64_t bin_mask,
                     uint64_t bin_select,
                     const RingBuffer::ReadRange& payload);
  // Type 0 and 1 register writes while recording.
  void OnPacketType01(uint32_t header, const RingBuffer::ReadRange& payload);
  // Before each draw or resolve while recording. False to skip it.
  bool BeforeDraw();

  // Backend queries.
  // The first strip of a merged session is running.
  bool merging() const {
    return session_open_ && mode_ == Mode::kMerged && strip_index_ == 0;
  }
  // A learning session is running.
  bool learning() const { return session_open_ && mode_ == Mode::kLearn; }
  // A check failed in this session (a merged first strip keeps rendering
  // merged until it ends, then recovers).
  bool failed() const { return failed_; }
  // The layout the current session renders (merged) or checks against.
  const Layout& layout() const { return mode_ == Mode::kMerged ? layout_
                                                              : candidate_; }
  // Draw-level checks apply: the first strip, with a layout to check against.
  bool checking_draws() const {
    return session_open_ && strip_index_ == 0 && !failed_ &&
           (mode_ == Mode::kMerged ||
            (mode_ == Mode::kLearn && candidate_.IsValid()));
  }
  // The packet being executed is strip-own.
  bool current_packet_own() const { return current_packet_own_; }
  // Render targets at this pitch and MSAA are banded (merged when merging).
  bool IsBanded(uint32_t pitch_tiles_at_32bpp,
                xenos::MsaaSamples msaa_samples) const;
  // The current window scissor is the first strip's frame-space one (when
  // merging, after BeforeDraw extended it).
  bool IsFrameSpace() const;
  // Draw-level checks (first strip, see the class comment). Fails the session
  // for memexport, for shaders reading strip-own constants in draws that don't
  // address the frame, and for band-addressed draws that can't be replicated.
  void CheckDraw(const DrawInfo& draw);
  // Learning sessions: a strip-own resolve.
  void NoteOwnResolve(const ResolveRecord& resolve);
  // The session can't be merged (learning), or the merged first strip has to
  // recover, or a later merged strip went wrong.
  void Fail(const char* reason);

 private:
  // Verified sessions with the same layout in a row before strips are merged.
  static constexpr uint32_t kLearnSessions = 2;
  // Merged sessions between two verifying (unmerged) ones.
  static constexpr uint32_t kRelearnInterval = 300;
  // Tiling sessions run unmerged after a merged one failed a check.
  static constexpr uint32_t kCooldownSessions = 120;
  // Learning sessions of a frame's tiling pass that fail in a row (or find
  // another layout each time) before that pass runs unrecorded for a while:
  // 16 sessions the first time, twice as many after each failing relearn, at
  // most kDormantMaxSessions.
  static constexpr uint32_t kDormantAfterSessions = 4;
  static constexpr uint32_t kDormantMaxSessions = 256;

  enum class Mode {
    kLearn,
    kMerged,
    // A merged first strip failed, the later strips run normally.
    kRecovered,
  };

  struct Record {
    uint64_t mask;
    uint64_t hash;
    uint32_t opcode;
  };
  struct OwnWrite {
    uint32_t reg;
    uint32_t value;
    // Whether a draw saw this write (it was still in the register then).
    bool used = false;
  };
  struct OwnLatest {
    uint32_t reg;
    uint32_t value;
    // Into own_writes.
    uint32_t write_index;
  };
  struct Strip {
    uint64_t bits = 0;
    std::vector<Record> records;
    std::vector<uint32_t> own_opcodes;
    std::vector<OwnWrite> own_writes;
    std::vector<ResolveRecord> own_resolves;
    std::vector<uint64_t> draw_states;
    // Per draw: HashDrawOwnState.
    std::vector<uint64_t> draw_own_states;
    // The strip's latest own value of every register it wrote itself.
    std::vector<OwnLatest> own_shadow;
    std::vector<uint32_t> own_float_registers;
    uint32_t draws = 0;
    void Clear();
  };

  void BeginSession(uint64_t bin_select);
  void BeginStrip(uint64_t bin_select);
  void EndStrip();
  void EndSession();
  // Compares strip `index` with the first one. False if they differ.
  bool CompareStrip(uint32_t index);
  // Derives the layout from the session's records. False if it isn't one.
  // Merged sessions skip the later strips' resolves, so they don't check them.
  bool DeriveLayout(Layout& layout_out, bool check_resolves);
  bool IsRedundantOwnSetConstant(const Strip& strip,
                                 const RingBuffer::ReadRange& payload) const;
  uint64_t HashDrawOwnState(Strip& strip);
  void RecordOwnSetConstant(const RingBuffer::ReadRange& payload);
  bool ReadsOwnConstants(const Shader* shader, bool is_pixel_shader) const;
  uint64_t HashDrawState();
  void RestoreWindowScissor();

  RegisterFile& register_file_;
  std::vector<uint32_t> draw_own_scratch_;
  // Per strip-own write of the last verified learning session: whether a draw
  // saw it.
  std::vector<bool> learned_own_used_;
  Backend* backend_ = nullptr;

  bool session_open_ = false;
  Mode mode_ = Mode::kLearn;
  uint64_t current_select_ = 0;
  // A learning session started with a layout to check the draws against.
  bool draw_checks_ran_ = false;
  uint32_t strip_index_ = 0;
  uint32_t strip_count_ = 0;
  uint64_t strip_bits_union_ = 0;
  Strip strips_[kMaxStrips];
  bool failed_ = false;
  std::string failure_reason_;
  uint32_t failure_strip_ = 0;
  bool current_packet_own_ = false;
  bool window_br_extended_ = false;

  // The layout of the last session, unverified or verified.
  Layout candidate_;
  // Verified sessions in a row with the candidate layout.
  uint32_t verified_sessions_ = 0;
  // The layout merged sessions use, fixed by the first one.
  Layout layout_;
  // Sessions left before merging may resume after a failure.
  uint32_t cooldown_sessions_ = 0;
  // Merged sessions since the last learning one.
  uint32_t merged_since_learning_ = 0;
  uint64_t sessions_ = 0;
  // Float constant vectors (0-511) strip-own packets have written.
  std::vector<uint32_t> own_float_constants_;

  std::vector<uint32_t> draw_state_scratch_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_STRIP_MERGE_H_
