/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// RenderTargetCache's side of strip merge (strip_merge.h): strip-merged render
// targets, their bands, carries and twins in the EDRAM ownership.

#include "xenia/gpu/render_target_cache.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/xenos.h"

DECLARE_bool(strip_merge_log);

namespace xe {
namespace gpu {

bool RenderTargetCache::GetStripMergedResolveRectangle(
    uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch,
    std::vector<ResolveCopyDumpRectangle>& rectangles_out) const {
  // The owner of the first tile has to be a strip-merged render target owning
  // every tile of the resolve area in its first band, which is the one in the
  // EDRAM model; its other bands are past the EDRAM size.
  base &= xenos::kEdramTileCount - 1;
  auto it = ownership_ranges_.upper_bound(base);
  if (it == ownership_ranges_.cbegin()) {
    return false;
  }
  --it;
  if (it->second.end_tiles <= base) {
    return false;
  }
  RenderTargetKey key = it->second.render_target;
  if (!key.strip_merged || pitch != key.GetPitchTiles()) {
    return false;
  }
  uint32_t band_tile_rows = strip_merge_bands_.tile_rows;
  uint32_t row_first = ((base - key.base_tiles) & (xenos::kEdramTileCount - 1)) /
                       pitch;
  if (row_first >= band_tile_rows ||
      row_first + rows > strip_merge_bands_.count * band_tile_rows) {
    return false;
  }
  // The first band's part of the resolve area.
  uint32_t band_rows_used = std::min(rows, band_tile_rows - row_first);
  uint32_t check_end = base + (band_rows_used - 1) * pitch + row_length;
  auto owned = [&](uint32_t extent_start, uint32_t extent_end) {
    uint32_t position = extent_start;
    return ForEachOwnershipRangeInExtent(
               extent_start, extent_end,
               [&](uint32_t start_tiles, const OwnershipRange& range) {
                 if (start_tiles > position || range.render_target != key) {
                   return false;
                 }
                 position = range.end_tiles;
                 return true;
               }) &&
           position >= extent_end;
  };
  if (!owned(base, std::min(check_end, xenos::kEdramTileCount)) ||
      (check_end > xenos::kEdramTileCount &&
       !owned(0, check_end - xenos::kEdramTileCount))) {
    return false;
  }
  auto rt_it = render_targets_.find(key);
  if (rt_it == render_targets_.cend() || !rt_it->second) {
    return false;
  }
  rectangles_out.emplace_back(rt_it->second, 0, rows, 0, row_length);
  return true;
}

void RenderTargetCache::AppendStripMergeTransfers(
    std::vector<Transfer>& transfers, uint32_t start_tiles,
    uint32_t end_tiles, RenderTargetKey dest, RenderTarget* source,
    RenderTarget* host_depth_source) {
  uint32_t band_count = strip_merge_bands_.count;
  // A strip-merged render target of an ended session is the EDRAM as that
  // session left it, in one band, the same for every strip as anything not
  // strip-merged (ChangeOwnership has given the source and the host depth
  // source one such band or none).
  const StripMergePending* pending = GetStripMergePending(source->key());
  if (!pending && host_depth_source) {
    pending = GetStripMergePending(host_depth_source->key());
  }
  // One band, or two by row (EndStripMerge): one transfer per band.
  StripMergePendingPiece pieces[kMaxStripMergePendingPieces];
  auto get_pieces = [&](uint32_t pieces_start_tiles,
                        uint32_t pieces_end_tiles) {
    if (pending) {
      return GetStripMergePendingPieces(*pending, pieces_start_tiles,
                                        pieces_end_tiles, pieces);
    }
    pieces[0] = {pieces_start_tiles, pieces_end_tiles, 0};
    return uint32_t(1);
  };
  bool source_merged =
      source->key().strip_merged && GetStripMergePendingBand(source->key()) < 0;
  if (source_merged &&
      (!host_depth_source || host_depth_source->key().strip_merged)) {
    // Each band from the same band.
    transfers.emplace_back(start_tiles, end_tiles, source, host_depth_source);
    return;
  }
  // Anything not strip-merged is the same for every strip, but its addressing
  // wraps around the EDRAM, so it fills one band per transfer.
  auto append_bands = [&](uint32_t band_start_tiles, uint32_t band_end_tiles) {
    uint32_t piece_count = get_pieces(band_start_tiles, band_end_tiles);
    for (uint32_t band = 0; band < band_count; ++band) {
      for (uint32_t piece_index = 0; piece_index < piece_count;
           ++piece_index) {
        const StripMergePendingPiece& piece = pieces[piece_index];
        Transfer& transfer = transfers.emplace_back(
            piece.start_tiles, piece.end_tiles, source, host_depth_source);
        transfer.band_first = uint8_t(band);
        transfer.band_count = 1;
        if (pending) {
          transfer.source_band_shift =
              int8_t(int32_t(piece.band) - int32_t(band));
        }
      }
    }
  };
  if (source_merged) {
    append_bands(start_tiles, end_tiles);
    return;
  }
  // The first claim of the tiles in a merged session takes the EDRAM as it
  // was before the session, which is what the first strip starts with. The
  // later strips start with what the strip before leaves: the carry of the
  // previous merged session, where there is one, band k - 1 of its
  // strip-merged owner, unwritten since (strip_merge_alt).
  uint32_t position = start_tiles;
  for (const StripMergeCarry& carry : strip_merge_carries_) {
    if (carry.end_tiles <= position || carry.start_tiles >= end_tiles) {
      continue;
    }
    if (carry.start_tiles > position) {
      append_bands(position, carry.start_tiles);
      position = carry.start_tiles;
    }
    uint32_t piece_end_tiles = std::min(carry.end_tiles, end_tiles);
    auto carry_it = render_targets_.find(carry.owner);
    if (carry.owner != dest && carry_it != render_targets_.end() &&
        carry_it->second && carry_it->second == carry.render_target) {
      uint32_t piece_count = get_pieces(position, piece_end_tiles);
      for (uint32_t piece_index = 0; piece_index < piece_count;
           ++piece_index) {
        const StripMergePendingPiece& piece = pieces[piece_index];
        Transfer& first_band = transfers.emplace_back(
            piece.start_tiles, piece.end_tiles, source, host_depth_source);
        first_band.band_first = 0;
        first_band.band_count = 1;
        first_band.source_band_shift = int8_t(piece.band);
      }
      // A depth carry into a depth render target is its own host depth, not
      // to lose the host precision. Into color, the guest bits are the same
      // without, and the transfer can be drawn in the destination's pass.
      RenderTarget* carry_rt = carry_it->second;
      Transfer& later_bands = transfers.emplace_back(
          position, piece_end_tiles, carry_rt,
          carry.owner.is_depth && dest.is_depth ? carry_rt : nullptr);
      later_bands.band_first = 1;
      later_bands.band_count = uint8_t(band_count - 1);
      later_bands.source_band_shift = -1;
    } else {
      append_bands(position, piece_end_tiles);
    }
    position = piece_end_tiles;
  }
  if (position < end_tiles) {
    append_bands(position, end_tiles);
  }
}

void RenderTargetCache::ConsumeStripMergeCarries(uint32_t start_tiles,
                                                 uint32_t end_tiles) {
  if (start_tiles >= end_tiles) {
    return;
  }
  std::vector<StripMergeCarry> remaining;
  remaining.reserve(strip_merge_carries_.size() + 1);
  for (const StripMergeCarry& carry : strip_merge_carries_) {
    if (carry.end_tiles <= start_tiles || carry.start_tiles >= end_tiles) {
      remaining.push_back(carry);
      continue;
    }
    if (carry.start_tiles < start_tiles) {
      remaining.push_back(
          {carry.start_tiles, start_tiles, carry.owner, carry.render_target});
    }
    if (carry.end_tiles > end_tiles) {
      remaining.push_back(
          {end_tiles, carry.end_tiles, carry.owner, carry.render_target});
    }
  }
  strip_merge_carries_ = std::move(remaining);
}

const char* RenderTargetCache::BeginStripMerge(uint32_t band_count,
                                               uint32_t band_tile_rows) {
  if (GetPath() != Path::kHostRenderTargets) {
    return "the render targets aren't host ones";
  }
  if (IsDrawResolutionScaled()) {
    return "the draw resolution is scaled";
  }
  if (band_count < 2 || !band_tile_rows) {
    return "no bands";
  }
  if (band_count > StripMerge::kMaxStrips) {
    return "more strips than merged render targets have bands";
  }
  // Strip-merged render targets are created with the band shape.
  if (strip_merge_bands_.count &&
      (strip_merge_bands_.count != band_count ||
       strip_merge_bands_.tile_rows != band_tile_rows)) {
    return "bands of another shape than the first merged session's";
  }
  strip_merge_bands_.count = band_count;
  strip_merge_bands_.tile_rows = band_tile_rows;
  // The keys whose previous render target holds a carry draw to their other
  // one in this session.
  strip_merge_alt_keys_.clear();
  for (const StripMergeCarry& carry : strip_merge_carries_) {
    RenderTargetKey session_key = carry.owner;
    session_key.strip_merge_alt ^= 1;
    if (std::find(strip_merge_alt_keys_.cbegin(), strip_merge_alt_keys_.cend(),
                  session_key) == strip_merge_alt_keys_.cend()) {
      strip_merge_alt_keys_.push_back(session_key);
    }
  }
  // A render target an ended session left in the ownership, that this one
  // would draw to, moves into its twin first.
  PruneStripMergePending();
  std::vector<RenderTargetKey> drawn_again;
  for (const StripMergePending& pending : strip_merge_pending_) {
    if (GetStripMergeSessionKey(pending.key) == pending.key) {
      drawn_again.push_back(pending.key);
    }
  }
  for (RenderTargetKey key : drawn_again) {
    MaterializeStripMergeTwin(key, "drawn again");
  }
  strip_merge_active_ = true;
  are_accumulated_render_targets_valid_ = false;
  return nullptr;
}
  return true;
}

RenderTargetCache::RenderTargetKey RenderTargetCache::GetStripMergeSessionKey(
    RenderTargetKey key) const {
  key.strip_merge_alt = 0;
  for (RenderTargetKey alt_key : strip_merge_alt_keys_) {
    RenderTargetKey alt_key_cleared = alt_key;
    alt_key_cleared.strip_merge_alt = 0;
    if (alt_key_cleared == key) {
      return alt_key;
    }
  }
  return key;
}

void RenderTargetCache::EndStripMerge(uint32_t band, bool carry,
                                      uint32_t band_tile_rows) {
  if (!strip_merge_active_) {
    return;
  }
  strip_merge_active_ = false;
  uint32_t band_count = strip_merge_bands_.count;
  carry = carry && band + 1 == band_count;
  PruneStripMergePending();
  // The ones of earlier sessions keep their bands.
  std::vector<RenderTargetKey> merged_keys;
  auto note_key = [&](RenderTargetKey key) {
    if (key.strip_merged && GetStripMergePendingBand(key) < 0 &&
        std::find(merged_keys.cbegin(), merged_keys.cend(), key) ==
            merged_keys.cend()) {
      merged_keys.push_back(key);
    }
  };
  ForEachEdramOwner(note_key);
  std::vector<StripMergeCarry> carries;
  bool replaced = false;
  for (RenderTargetKey key : merged_keys) {
    auto source_it = render_targets_.find(key);
    RenderTarget* source =
        source_it != render_targets_.end() ? source_it->second : nullptr;
    if (!source) {
      // Nothing to read.
      replaced |= ReplaceOwnershipKey(key, RenderTargetKey());
      continue;
    }
    strip_merge_pending_.push_back(
        {key, band,
         band && band_tile_rows < strip_merge_bands_.tile_rows ? band_tile_rows
                                                               : 0});
    // The ranges it owns are carried: the next session reads its bands in
    // place.
    if (!carry) {
      continue;
    }
    for (const auto& range_pair : ownership_ranges_) {
      const OwnershipRange& range = range_pair.second;
      if (range.render_target != key) {
        continue;
      }
      if (!carries.empty() && carries.back().owner == key &&
          carries.back().end_tiles == range_pair.first) {
        carries.back().end_tiles = range.end_tiles;
      } else {
        carries.push_back({range_pair.first, range.end_tiles, key, source});
      }
    }
  }
  if (replaced) {
    MergeSameOwnershipRanges();
  }
  std::sort(carries.begin(), carries.end(),
            [](const StripMergeCarry& a, const StripMergeCarry& b) {
              return a.start_tiles < b.start_tiles;
            });
  strip_merge_carries_ = std::move(carries);
  // Nothing bound may still refer to a strip-merged render target.
  are_accumulated_render_targets_valid_ = false;
  std::memset(last_update_accumulated_render_targets_, 0,
              sizeof(last_update_accumulated_render_targets_));
  std::memset(last_update_used_render_targets_, 0,
              sizeof(last_update_used_render_targets_));
}

int32_t RenderTargetCache::GetStripMergePendingBand(RenderTargetKey key) const {
  if (!key.strip_merged) {
    return -1;
  }
  for (const StripMergePending& pending : strip_merge_pending_) {
    if (pending.key == key) {
      return int32_t(pending.band);
    }
  }
  return -1;
}

const RenderTargetCache::StripMergePending*
RenderTargetCache::GetStripMergePending(RenderTargetKey key) const {
  if (!key.strip_merged) {
    return nullptr;
  }
  for (const StripMergePending& pending : strip_merge_pending_) {
    if (pending.key == key) {
      return &pending;
    }
  }
  return nullptr;
}

uint32_t RenderTargetCache::GetStripMergePendingPieces(
    const StripMergePending& pending, uint32_t start_tiles, uint32_t end_tiles,
    StripMergePendingPiece* pieces_out) const {
  uint32_t count = 0;
  auto emit = [&](uint32_t piece_start, uint32_t piece_end, uint32_t band) {
    if (piece_start >= piece_end) {
      return;
    }
    if (count && pieces_out[count - 1].band == band &&
        pieces_out[count - 1].end_tiles == piece_start) {
      pieces_out[count - 1].end_tiles = piece_end;
      return;
    }
    assert_true(count < kMaxStripMergePendingPieces);
    pieces_out[count++] = {piece_start, piece_end, band};
  };
  if (!pending.band || !pending.band_tile_rows) {
    emit(start_tiles, end_tiles, pending.band);
    return count;
  }
  // The rows below the last strip's, of the key's first band (the tiles it
  // owns), may wrap around the EDRAM end.
  uint32_t pitch_tiles = pending.key.GetPitchTiles();
  uint32_t below_start =
      (pending.key.base_tiles + pending.band_tile_rows * pitch_tiles) &
      (xenos::kEdramTileCount - 1);
  uint32_t below_length = std::min(
      (strip_merge_bands_.tile_rows - pending.band_tile_rows) * pitch_tiles,
      xenos::kEdramTileCount);
  uint32_t below_end = below_start + below_length;
  std::pair<uint32_t, uint32_t> below[2];
  uint32_t below_count = 0;
  if (below_end > xenos::kEdramTileCount) {
    below[below_count++] = {0, below_end - xenos::kEdramTileCount};
    below[below_count++] = {below_start, xenos::kEdramTileCount};
  } else {
    below[below_count++] = {below_start, below_end};
  }
  uint32_t position = start_tiles;
  for (uint32_t i = 0; i < below_count; ++i) {
    uint32_t piece_start = std::max(below[i].first, position);
    uint32_t piece_end = std::min(below[i].second, end_tiles);
    if (piece_start >= piece_end) {
      continue;
    }
    emit(position, piece_start, pending.band);
    emit(piece_start, piece_end, pending.band - 1);
    position = piece_end;
  }
  emit(position, end_tiles, pending.band);
  return count;
}

bool RenderTargetCache::AreStripMergePendingBandsSame(
    const StripMergePending& a, const StripMergePending& b) const {
  if (a.band != b.band) {
    return false;
  }
  bool a_split = a.band && a.band_tile_rows;
  bool b_split = b.band && b.band_tile_rows;
  if (!a_split && !b_split) {
    return true;
  }
  if (a_split != b_split) {
    return false;
  }
  auto below = [](const StripMergePending& pending) {
    uint32_t pitch_tiles = pending.key.GetPitchTiles();
    return std::make_pair(
        (pending.key.base_tiles + pending.band_tile_rows * pitch_tiles) &
            (xenos::kEdramTileCount - 1),
        pitch_tiles);
  };
  return a.band_tile_rows == b.band_tile_rows && below(a) == below(b);
}

void RenderTargetCache::PruneStripMergePending() {
  strip_merge_pending_.erase(
      std::remove_if(
          strip_merge_pending_.begin(), strip_merge_pending_.end(),
          [this](const StripMergePending& pending) {
            for (const auto& range_pair : ownership_ranges_) {
              const OwnershipRange& range = range_pair.second;
              if (range.render_target == pending.key ||
                  range.depth_bits_target == pending.key ||
                  range.host_depth_render_target_unorm24 == pending.key ||
                  range.host_depth_render_target_float24 == pending.key) {
                return false;
              }
            }
            return true;
          }),
      strip_merge_pending_.end());
}

void RenderTargetCache::MaterializeStripMergeTwin(RenderTargetKey key,
                                                  const char* reason) {
  auto pending_it = std::find_if(
      strip_merge_pending_.begin(), strip_merge_pending_.end(),
      [key](const StripMergePending& pending) { return pending.key == key; });
  if (pending_it == strip_merge_pending_.end()) {
    return;
  }
  StripMergePending pending = *pending_it;
  strip_merge_pending_.erase(pending_it);
  auto source_it = render_targets_.find(key);
  RenderTarget* source =
      source_it != render_targets_.end() ? source_it->second : nullptr;
  RenderTargetKey twin_key = key;
  twin_key.strip_merged = 0;
  twin_key.strip_merge_alt = 0;
  RenderTarget* twin =
      source ? GetOrCreateRenderTarget(twin_key, false) : nullptr;
  // Depth copies take the host depth from the same render target, so it
  // keeps its host precision.
  RenderTarget* host_depth_source = key.is_depth ? source : nullptr;
  // Every range the strip-merged render target is in, as the owner or as the
  // holder of depth, moves to the twin, from band `band`.
  std::vector<Transfer> transfers;
  uint32_t tile_count = 0;
  for (const auto& range_pair : ownership_ranges_) {
    const OwnershipRange& range = range_pair.second;
    if (!source || (range.render_target != key &&
                    range.depth_bits_target != key &&
                    range.host_depth_render_target_unorm24 != key &&
                    range.host_depth_render_target_float24 != key)) {
      continue;
    }
    tile_count += range.end_tiles - range_pair.first;
    // One band, or two by row (EndStripMerge).
    StripMergePendingPiece pieces[kMaxStripMergePendingPieces];
    uint32_t piece_count = GetStripMergePendingPieces(
        pending, range_pair.first, range.end_tiles, pieces);
    for (uint32_t piece_index = 0; piece_index < piece_count; ++piece_index) {
      const StripMergePendingPiece& piece = pieces[piece_index];
      if (!transfers.empty() &&
          transfers.back().end_tiles == piece.start_tiles &&
          transfers.back().source_band_shift == int8_t(piece.band)) {
        transfers.back().end_tiles = piece.end_tiles;
      } else {
        Transfer& transfer = transfers.emplace_back(
            piece.start_tiles, piece.end_tiles, source, host_depth_source);
        transfer.source_band_shift = int8_t(piece.band);
      }
    }
  }
  if (cvars::strip_merge_log) {
    XELOGI("Strip merge: {} moves into its twin for {}, {} tiles",
           key.GetDebugName(), reason, tile_count);
  }
  if (twin && !transfers.empty() &&
      !PerformStripMergeTransfers(twin, transfers)) {
    XELOGE("Strip merge: failed to move {} into its twin", key.GetDebugName());
  }
  // The twin takes the strip-merged render target's place.
  ReplaceOwnershipKey(key, twin ? twin_key : RenderTargetKey());
  MergeSameOwnershipRanges();
}

bool RenderTargetCache::ReplaceOwnershipKey(RenderTargetKey key,
                                            RenderTargetKey replacement) {
  bool replaced = false;
  for (auto& range_pair : ownership_ranges_) {
    OwnershipRange& range = range_pair.second;
    for (RenderTargetKey* range_key :
         {&range.render_target, &range.depth_bits_target,
          &range.host_depth_render_target_unorm24,
          &range.host_depth_render_target_float24}) {
      if (*range_key == key) {
        *range_key = replacement;
        replaced = true;
      }
    }
  }
  return replaced;
}

uint32_t RenderTargetCache::GetRangeRectangles(
    uint32_t start_tiles, uint32_t end_tiles, RenderTargetKey key,
    bool is_64bpp, Transfer::Rectangle* rectangles_out,
    const Transfer::Rectangle* cutout, uint32_t band_first,
    uint32_t band_count) const {
  if (!key.strip_merged || strip_merge_bands_.count < 2) {
    return Transfer::GetRangeRectangles(start_tiles, end_tiles, key, is_64bpp,
                                        rectangles_out, cutout);
  }
  // A strip-merged render target owns the first band's tiles, the same tiles
  // in each band, the cutout being in the whole render target.
  Transfer::Rectangle band_rectangles[Transfer::kMaxRectanglesWithoutCutout];
  uint32_t band_rectangle_count = Transfer::GetRangeRectangles(
      start_tiles, end_tiles, key, is_64bpp, band_rectangles);
  uint32_t band_end =
      band_count ? std::min(band_first + band_count, strip_merge_bands_.count)
                 : strip_merge_bands_.count;
  uint32_t band_rows = GetStripMergeBandRows(key);
  uint32_t rectangle_count = 0;
  for (uint32_t band = band_first; band < band_end; ++band) {
    for (uint32_t i = 0; i < band_rectangle_count; ++i) {
      Transfer::Rectangle rectangle = band_rectangles[i];
      rectangle.y_pixels += band * band_rows;
      rectangle_count += Transfer::AddRectangle(
          rectangle,
          rectangles_out ? rectangles_out + rectangle_count : nullptr, cutout);
    }
  }
  return rectangle_count;
}

void RenderTargetCache::MergeSameOwnershipRanges() {
  for (auto it = ownership_ranges_.begin(); it != ownership_ranges_.end();) {
    auto it_next = std::next(it);
    if (it_next != ownership_ranges_.end() &&
        it->second.end_tiles == it_next->first &&
        it_next->second.AreOwnersSame(it->second)) {
      it->second.end_tiles = it_next->second.end_tiles;
      ownership_ranges_.erase(it_next);
      continue;
    }
    it = it_next;
  }
}

bool RenderTargetCache::IsStripMergeTwinPendingInResolve(
    uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch) const {
  if (strip_merge_pending_.empty() || !row_length || !rows) {
    return false;
  }
  base &= xenos::kEdramTileCount - 1;
  uint32_t end = base + (rows - 1) * pitch + std::min(row_length, pitch);
  auto pending_in = [&](uint32_t extent_start, uint32_t extent_end) {
    return !ForEachOwnershipRangeInExtent(
        extent_start, extent_end, [&](uint32_t, const OwnershipRange& range) {
          return GetStripMergePendingBand(range.render_target) < 0;
        });
  };
  return pending_in(base, std::min(end, xenos::kEdramTileCount)) ||
         (end > xenos::kEdramTileCount &&
          pending_in(0, std::min(end - xenos::kEdramTileCount, base)));
}

void RenderTargetCache::MaterializeStripMergeTwinsInResolve(
    uint32_t base, uint32_t row_length, uint32_t rows, uint32_t pitch) {
  if (strip_merge_pending_.empty() || !row_length || !rows) {
    return;
  }
  base &= xenos::kEdramTileCount - 1;
  uint32_t end = base + (rows - 1) * pitch + std::min(row_length, pitch);
  std::vector<RenderTargetKey> keys;
  auto collect = [&](uint32_t extent_start, uint32_t extent_end) {
    ForEachOwnershipRangeInExtent(
        extent_start, extent_end, [&](uint32_t, const OwnershipRange& range) {
          RenderTargetKey key = range.render_target;
          if (GetStripMergePendingBand(key) >= 0 &&
              std::find(keys.cbegin(), keys.cend(), key) == keys.cend()) {
            keys.push_back(key);
          }
          return true;
        });
  };
  collect(base, std::min(end, xenos::kEdramTileCount));
  if (end > xenos::kEdramTileCount) {
    collect(0, std::min(end - xenos::kEdramTileCount, base));
  }
  for (RenderTargetKey key : keys) {
    MaterializeStripMergeTwin(key, "a resolve");
  }
}

}  // namespace gpu
}  // namespace xe
