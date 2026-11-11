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
class Memory;
namespace gpu {

class Shader;
class TraceWriter;

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
//   row enable merging. Each tiling session of a frame (by its
//   order since the last swap) learns on its own.
// - Merged session, first strip: the window scissor bottom of the strip's
//   frame-space draws is extended to the frame bottom; render targets at the
//   strip surface pitch and MSAA are "merged" ones, N bands tall, whose EDRAM
//   ownership is the first strip's tiles; draws that address the band itself
//   (window offset 0, like the clear quads) are replicated into every band;
//   strip-own resolves cover every strip's rows into the first strip's
//   destination. Later strips: draws and resolves are skipped, every other
//   packet runs, so the register state and the guest-visible handshake
//   (interrupts, waits, memory writes, events) are those of normal execution.
// - At the end, the merged render targets stay in the EDRAM ownership, their
//   last band holding what the last strip leaves; one moves into its ordinary
//   twin only for what can't read the band (RenderTargetCache::EndStripMerge).
// - EDRAM carried across strips: a strip starts with the previous strip's
//   EDRAM contents (Halo 3: the base-0 stencil and the 608 GAMMA color are
//   loaded before being overwritten, and the color is never cleared, so the
//   pixels a strip doesn't cover are the strip before's). One pass sequence
//   can't feed band k+1 with band k's end state, so band k+1 starts with band
//   k's end state of the previous session (the carry).
//   This is exact when consecutive frames are equal, and otherwise one frame
//   late in the pixels a strip leaves to the next. A session merges only once
//   there is a carry: the unmerged session before the first merged one (the
//   last verifying one, and every relearning one) copies what each strip
//   leaves into the carry's bands. Only the tiling session of the frame that
//   left the carry merges while it's there: another one's later strips would
//   start from the other session's bands (425307D5 alternates two sessions
//   over the same tiles, in other formats), so it runs unmerged.
// - A check failing in the merged first strip recovers: its band 0 becomes the
//   EDRAM, and the later strips run normally. A failure in a later strip of a
//   merged session (only possible if the game changed what it draws between
//   strips) may leave that frame wrong, and pauses merging.
class StripMerge {
 public:
  // Also the most bands a strip-merged render target has.
  static constexpr uint32_t kMaxStrips = 4;
  static constexpr uint64_t kFirstTileBit = uint64_t(1) << 31;
  // The bin bits that name strips. Bits 32-63 are selected or not as a
  // whole for every strip (4D530919 selects bit 32 partway through the first
  // strip and for all of the second): they don't name strips.
  static constexpr uint64_t kStripBitsMask = kFirstTileBit - 1;

  struct Layout {
    uint32_t strip_count = 0;
    // Guest pixel rows of a strip.
    uint32_t band_rows = 0;
    // The bottom of the last strip's window, the frame's rows: D3D clips the
    // last strip to the frame (720 rows as 512 + 208), while every strip's
    // EDRAM band is band_rows tall.
    uint32_t frame_rows = 0;
    // The surface the strips render to, as RenderTargetCache keys have it.
    uint32_t pitch_tiles_at_32bpp = 0;
    xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
    // PA_SC_WINDOW_SCISSOR_BR of the first strip's frame-space draws.
    uint32_t window_br_first = 0;
    uint64_t strip_bits[kMaxStrips] = {};
    uint64_t strip_bits_union = 0;
    // Per strip-own resolve of a strip, in order: the frame row the merged
    // first strip's resolve reaches. Every strip but the last resolves its
    // band's rows; the last one's may stop short (at its window or the
    // frame), or not.
    std::vector<uint32_t> resolve_bottoms;
    // Per strip-own resolve: the frame row its clear reaches, 0 if it doesn't
    // clear. D3D clears the next tile's EDRAM with each tile's resolve, in
    // every strip (575207D7) or every strip but the last (425307D5): the
    // merged resolve clears the bands of the strips whose resolves clear.
    std::vector<uint32_t> resolve_clear_bottoms;
    // Indices among the first strip's draws (and resolves) of its own draws
    // the later strips don't do, ascending: D3D clears the first tile with a
    // quad masked to it alone (425307D5, 4D5307D5), the later tiles start
    // from what the previous tile's clearing resolve left. The merged first
    // strip draws them into the first band alone, as the first strip does.
    std::vector<uint32_t> first_strip_draws;

    bool IsValid() const {
      return strip_count >= 2 && band_rows && frame_rows;
    }
    uint32_t frame_bottom() const { return frame_rows; }
    uint32_t window_br_frame() const {
      return (window_br_first & 0x3FFF) | (frame_bottom() << 16);
    }
    uint32_t band_tile_rows() const {
      return band_rows / tile_height();
    }
    uint32_t tile_height() const {
      return msaa_samples >= xenos::MsaaSamples::k2X ? 8 : 16;
    }
    // The rows of the last strip's window, and whether the frame clips it.
    // The rows of the last band past them are what the strip before left
    // there: the last strip draws and resolves none of them (DeriveLayout,
    // NoteDrawTiles), and an ended merged session's last band holds the EDRAM
    // only above them (RenderTargetCache::EndStripMerge).
    uint32_t last_band_rows() const {
      return frame_rows - (strip_count - 1) * band_rows;
    }
    bool last_band_clipped() const { return last_band_rows() < band_rows; }
    bool operator==(const Layout& other) const;
    bool operator!=(const Layout& other) const { return !(*this == other); }
  };

  class Backend {
   public:
    virtual ~Backend() = default;
    // A merged session's first strip is about to run. Null to merge it, else
    // why it can't be (the session runs normally and isn't verified).
    virtual const char* StripMergeBegin(const Layout& layout) = 0;
    // Merged rendering ends. Band `band` of every merged render target becomes
    // the EDRAM; with carry, the bands are kept for the next session's later
    // strips (see the carry above). With band_tile_rows nonzero, only its
    // first band_tile_rows tile rows, the rows below them being band - 1's
    // (Layout::last_band_rows).
    virtual void StripMergeEnd(uint32_t band, bool carry,
                               uint32_t band_tile_rows) = 0;
    // Strip `band` (not the last) of an unmerged session with this layout
    // ended: what it leaves in the EDRAM of the strip surface becomes band
    // `band` of the carry, band 0 replacing the carry there was. False if
    // there is no carry now.
    virtual bool StripMergeSeedCarry(const Layout& layout, uint32_t band) = 0;
    // The seeded carry isn't to be used.
    virtual void StripMergeDropCarry() = 0;
    // Whether guest memory in a range is yet to be written by GPU work
    // already recorded, so it can't be read to verify a session.
    virtual bool StripMergeMemoryUnfinished(uint32_t address,
                                            uint32_t length) = 0;
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

  // What NoteResolve records for DeriveLayout, from draw_util::ResolveInfo.
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
    // The values it clears with (0 where it doesn't clear): the merged
    // resolve clears every clearing strip's band with the first strip's.
    uint32_t clear_color = 0;
    uint32_t clear_color_lo = 0;
    uint32_t clear_depth = 0;
  };

  StripMerge(RegisterFile& register_file, const Memory* memory)
      : register_file_(register_file), memory_(memory) {}

  void SetBackend(Backend* backend) { backend_ = backend; }

  // A session is open and recorded, the PM4 hooks below have to run.
  bool recording() const { return session_open_ && recording_; }
  // A session is open, recorded or not: BeforeDraw has to run (it counts the
  // draws of unrecorded sessions).
  bool session_open() const { return session_open_; }

  // PM4 hooks. tracing: a trace is being written (traces record the guest's
  // mask rewrites, a merged trace couldn't be replayed unmerged).
  void OnBinSelect(uint64_t bin_select, bool tracing);
  // Every type 3 packet while recording, before it executes, with its payload.
  void OnPacketType3(uint32_t header, uint32_t opcode, uint64_t bin_mask,
                     uint64_t bin_select,
                     const RingBuffer::ReadRange& payload);
  // Type 0 and 1 register writes while recording.
  void OnPacketType01(uint32_t header, const RingBuffer::ReadRange& payload);
  // Before each draw or resolve while a session is open. False to skip it.
  bool BeforeDraw();
  // An unpredicated swap: the next tiling session is the frame's first.
  void OnSwap();

  // Backend queries.
  // The first strip of a merged session is running.
  bool merging() const {
    return session_open_ && mode_ == Mode::kMerged && strip_index_ == 0;
  }
  // A learning session is running.
  bool learning() const { return recording() && mode_ == Mode::kLearn; }
  // A check failed in this session (a merged first strip keeps rendering
  // merged until it ends, then recovers).
  bool failed() const { return failed_; }
  // The layout the current session renders (merged) or checks against.
  const Layout& layout() const { return mode_ == Mode::kMerged ? layout_
                                                              : candidate_; }
  // Draw-level checks apply: the first strip, with a layout to check against.
  bool checking_draws() const {
    return recording() && strip_index_ == 0 && !failed_ &&
           (mode_ == Mode::kMerged ||
            (mode_ == Mode::kLearn && candidate_.IsValid()));
  }
  // The packet being executed is strip-own.
  bool current_packet_own() const { return current_packet_own_; }
  // The merged first strip's current draw is one the later strips don't do
  // (Layout::first_strip_draws): it goes into the first band alone.
  bool first_strip_only_draw() const { return first_strip_draw_; }
  // Render targets at this pitch and MSAA are banded (merged when merging).
  bool IsBanded(uint32_t pitch_tiles_at_32bpp,
                xenos::MsaaSamples msaa_samples) const;
  // IsBanded for the current RB_SURFACE_INFO.
  bool IsSurfaceBanded() const;
  // The current window scissor is the first strip's frame-space one (when
  // merging, after BeforeDraw extended it).
  bool IsFrameSpace() const;
  // Draw-level checks (first strip, see the class comment). Fails the session
  // for memexport, for shaders reading strip-own constants in draws that don't
  // address the frame, and for band-addressed draws that can't be replicated.
  void CheckDraw(const DrawInfo& draw);
  // A resolve while recording: learning sessions record the strip-own ones.
  // The frame row a strip-own resolve of the merged first strip extends down
  // to, covering every strip's rows, and the one its clear reaches; 0
  // otherwise.
  uint32_t NoteResolve(TraceWriter& trace_writer, uint32_t& clear_bottom_out);
  // Whether NoteDrawTiles has to be called for the render targets of draws:
  // a learning session with a layout whose last strip the frame clips.
  bool checking_draw_tiles() const {
    return learning() && !failed_ && candidate_.IsValid() &&
           candidate_.last_band_clipped();
  }
  // With checking_draw_tiles, a render target a draw claims, from EDRAM tile
  // base_tiles on, length_tiles long, as RenderTargetCache claims it for the
  // draw's rows. Fails the session if it's in the last strip and reaches the
  // rows of the strip surface past the frame.
  void NoteDrawTiles(uint32_t base_tiles, uint32_t length_tiles, bool banded);
  // The merged first strip: the frame row its next strip-own resolve reaches
  // and the one its clear reaches (0, and the session failed, if it has more
  // than the learned ones).
  uint32_t TakeMergedResolveBottom(uint32_t& clear_bottom_out);
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
    bool executed;
  };
  // The registers a register or constant write packet sets.
  struct RegisterWrites {
    uint32_t base = 0;
    uint32_t count = 0;
    // From the payload, starting at dword payload_first, or big-endian from
    // guest memory.
    const RingBuffer::ReadRange* payload = nullptr;
    uint32_t payload_first = 0;
    const uint32_t* memory = nullptr;
    uint32_t value(uint32_t index) const;
  };
  struct OwnLatest {
    uint32_t reg;
    uint32_t value;
    // Written by something other than the strip's own packets since.
    bool stale = false;
  };
  struct OwnValue {
    uint32_t reg;
    uint32_t value;
    bool operator==(const OwnValue& other) const {
      return reg == other.reg && value == other.value;
    }
  };
  // What kind of draw a draw packet is, as far as the strips' own registers
  // are concerned.
  enum DrawKind : uint8_t {
    kDrawKindOwn = 1 << 0,
    kDrawKindResolve = 1 << 1,
  };
  // RB_HIZCONTROL hiz_enable, the only bit of it Xenia reads.
  static constexpr uint32_t kHiZEnableBit = 1 << 1;
  enum DrawWindow : uint8_t {
    // A draw on the strip surface (CheckDraw).
    kDrawWindowBanded = 1 << 0,
    // PA_SU_SC_MODE_CNTL vtx_window_offset_enable.
    kDrawWindowVertexOffset = 1 << 1,
    // PA_SC_WINDOW_SCISSOR_TL window_offset_disable.
    kDrawWindowScissorOffsetDisabled = 1 << 2,
  };
  struct Strip {
    uint64_t bits = 0;
    std::vector<Record> records;
    std::vector<ResolveRecord> own_resolves;
    std::vector<uint64_t> draw_states;
    // Per draw: HashDrawOwnState.
    std::vector<uint64_t> draw_own_states;
    // Per draw: DrawKind bits.
    std::vector<uint8_t> draw_kinds;
    // Per draw: PA_SC_WINDOW_SCISSOR_BR, and DrawWindow bits (CheckDraw), for
    // DeriveLayout to check that a draw addresses the frame exactly when the
    // strips' windows differ at it.
    std::vector<uint32_t> draw_window_br;
    std::vector<uint8_t> draw_window;
    // Per draw: which of own_snapshot_starts holds the strip-own values the
    // draw sees (the registers still holding the strip's own latest value).
    std::vector<uint32_t> draw_own_snapshots;
    // Snapshot i is own_snapshot_values [starts[i], starts[i + 1]), sorted by
    // register; a draw seeing what the previous one saw reuses its snapshot.
    std::vector<uint32_t> own_snapshot_starts;
    std::vector<OwnValue> own_snapshot_values;
    // Per strip-own draw (learning): CheckDraw's hash of its vertices.
    struct OwnDrawVertices {
      // Index among the strip's draws.
      uint32_t draw;
      uint64_t hash;
      // For MatchClippedQuad: the vertex shader, the vertices (host endian)
      // if it has one binding of at most kMaxClippedQuadDwords, and whether
      // the vertex rows are the band's pixel rows with pixel centers at
      // integers (no viewport y scale or offset, no window offset, D3D pixel
      // centers).
      uint64_t vertex_shader_hash = 0;
      std::vector<uint32_t> words;
      bool rows_are_band_rows = false;
    };
    std::vector<OwnDrawVertices> own_draw_vertices;
    // The strip's latest own value of every register it wrote itself, sorted
    // by register.
    std::vector<OwnLatest> own_shadow;
    std::vector<uint32_t> own_float_registers;
    uint32_t draws = 0;
    void Clear();
  };

  void BeginSession(uint64_t bin_select);
  void BeginStrip(uint64_t bin_select);
  void EndStrip();
  // A dormant session: no learning, its slot's dormant run counts down.
  void EndDormantSession();
  void EndSession();
  // False if a packet is predicated on some strips but not all.
  bool CheckMasks();
  // Learning: the first strip's own draws the second strip doesn't do, found
  // by matching the strips' draws in order. False if they don't match that
  // way (the comparisons below then fail with the reason).
  bool FindFirstStripDraws(std::vector<uint32_t>& draws_out) const;
  // Removes the first strip's draws the later strips don't do from what is
  // compared draw by draw. False (and the session failed) if one isn't the
  // first strip's own draw.
  bool DropFirstStripDraws(const std::vector<uint32_t>& draws);
  // Compares strip `index` with the first one, of the session's layout. False
  // if they differ.
  bool CompareStrip(uint32_t index, const Layout& layout);
  // Whether the last strip's own draw `last` is the first strip's `first`
  // clipped to the last strip's rows: D3D's tiling clear quad (its vertex
  // shader), the same vertices but the bottom ones' rows, which are the band's
  // bottom in the first strip and the frame's in the last, and constant
  // depth and color. Above the frame, the first strip's quad covers the same
  // samples with the same values; below it, the last band's rows aren't the
  // EDRAM (Layout::last_band_rows).
  bool MatchClippedQuad(const Strip::OwnDrawVertices& first,
                        const Strip::OwnDrawVertices& last,
                        const Layout& layout) const;
  static constexpr uint32_t kMaxClippedQuadDwords = 64;
  // Derives the layout from the session's records. False if it isn't one.
  // Merged sessions skip the later strips' resolves, so they don't check them.
  bool DeriveLayout(Layout& layout_out, bool check_resolves);
  // Checks the values strip-own register `reg` has in every strip at a draw
  // of kind `kind` that sees them. False (and the session failed) if merging
  // can't reproduce them.
  bool CheckOwnValues(uint32_t reg, const uint32_t* values, uint8_t kind,
                      int32_t& band_rows, uint32_t& window_br_first,
                      uint32_t& frame_rows);
  uint64_t HashDrawOwnState(Strip& strip, uint8_t kind);
  // SET_CONSTANT, SET_CONSTANT2, SET_SHADER_CONSTANTS and LOAD_ALU_CONSTANT.
  // False for other packets.
  bool GetRegisterWrites(uint32_t opcode, const RingBuffer::ReadRange& payload,
                         RegisterWrites& writes_out) const;
  void MarkOwnStale(uint32_t base, uint32_t count);
  void MarkOwnStale(uint32_t opcode, const RingBuffer::ReadRange& payload);
  void RecordOwnWrites(const RegisterWrites& writes);
  bool ReadsOwnConstants(const Shader* shader, bool is_pixel_shader) const;
  // With the draw's shaders, only the float constants they read.
  uint64_t HashDrawState(const DrawInfo* draw = nullptr);
  // HashDrawState of a resolve: the registers it reads.
  uint64_t HashResolveState() const;
  void RestoreWindowScissor();

  RegisterFile& register_file_;
  const Memory* memory_;
  std::vector<uint32_t> draw_own_scratch_;
  Backend* backend_ = nullptr;

  bool session_open_ = false;
  // The open session is recorded: the PM4 hooks see its packets. A learning
  // session stops being recorded once it failed (at its next draw), a dormant
  // one isn't (SessionSlot::dormant_sessions).
  bool recording_ = false;
  bool dormant_ = false;
  Mode mode_ = Mode::kLearn;
  // The session merges from the first strip's first draw, if it has one: a
  // session that draws nothing (555308AE opens one before each drawing one)
  // runs unmerged and leaves learning and the carries as they are.
  bool merge_planned_ = false;
  uint64_t current_select_ = 0;
  // A learning session started with a layout to check the draws against.
  bool draw_checks_ran_ = false;
  uint32_t strip_index_ = 0;
  uint32_t strip_count_ = 0;
  uint64_t strip_bits_union_ = 0;
  // The strip bits of the slot's last tiling session with more than one
  // strip.
  uint64_t previous_strip_bits_union_ = 0;
  Strip strips_[kMaxStrips];
  bool failed_ = false;
  // Strip-own resolves the merged first strip has done.
  uint32_t merged_resolves_ = 0;
  std::string failure_reason_;
  uint32_t failure_strip_ = 0;
  bool current_packet_own_ = false;
  bool first_strip_draw_ = false;
  // checking_draw_tiles: the EDRAM bases of render targets the strips before
  // the last drew on the strip surface.
  std::vector<uint32_t> banded_bases_;
  bool window_br_extended_ = false;
  // An unmerged session that leaves the carry the next (merged) one starts
  // its later bands with, if it is verified: seeding_ while every strip so
  // far has been copied.
  bool seed_planned_ = false;
  bool seeding_ = false;

  // The open session's learning state, loaded from its slot when it begins
  // and stored back when it ends.
  // The layout of the slot's last session, unverified or verified.
  Layout candidate_;
  // Verified sessions in a row with the candidate layout.
  uint32_t verified_sessions_ = 0;
  // Merged sessions since the last learning one.
  uint32_t merged_since_learning_ = 0;

  // Learning state per tiling session of the frame, by its order since the
  // last swap: frames often run several tiling passes with different strips,
  // each learns from the same pass of earlier frames. Sessions past the last
  // slot share it.
  struct SessionSlot {
    Layout candidate;
    uint32_t verified_sessions = 0;
    uint32_t merged_since_learning = 0;
    uint64_t previous_strip_bits_union = 0;
    // Learning sessions in a row that failed or found another layout than
    // the last one (kDormantAfterSessions).
    uint32_t failed_sessions = 0;
    // Sessions left that run unrecorded, and how many the next dormant run
    // has (doubling, up to kDormantMaxSessions; 0 for the first).
    uint32_t dormant_sessions = 0;
    uint32_t dormant_length = 0;
  };
  static constexpr uint32_t kSessionSlots = 4;
  SessionSlot slots_[kSessionSlots];
  uint32_t slot_ = 0;
  // Tiling sessions since the last swap (up to the last slot).
  uint32_t frame_sessions_ = 0;
  // The same, uncapped, and the current session's order in its frame.
  uint32_t frame_session_order_ = 0;
  uint32_t session_order_ = 0;
  // The order in its frame of the merged session that left the carry, the
  // only one that merges while it's there, or kNoCarry.
  static constexpr uint32_t kNoCarry = UINT32_MAX;
  uint32_t carry_session_order_ = kNoCarry;

  // The layout merged sessions use, fixed by the first one.
  Layout layout_;
  // Sessions left before merging may resume after a failure.
  uint32_t cooldown_sessions_ = 0;
  uint64_t sessions_ = 0;
  // Float constant vectors (0-511) strip-own packets have written.
  std::vector<uint32_t> own_float_constants_;

  std::vector<uint32_t> draw_state_scratch_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_STRIP_MERGE_H_
