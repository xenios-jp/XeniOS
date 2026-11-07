/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_live_stats.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_live_sessions.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"

namespace xe {
namespace kernel {
namespace xam {

namespace {

using net::live::StatsColumnUpdate;
using net::live::StatsOp;
using net::live::StatsValue;

constexpr uint8_t kContext = 0;
constexpr uint8_t kInt32 = 1;
constexpr uint8_t kInt64 = 2;
constexpr uint8_t kDouble = 3;
constexpr uint8_t kWString = 4;
constexpr uint8_t kFloat = 5;
constexpr uint8_t kDateTime = 7;

// Live answered a read within a few hundred ms; a title waits on it.
constexpr uint32_t kStatsReadTimeoutMs = 1500;

bool IsKeptType(uint8_t type) {
  return type == kContext || type == kInt32 || type == kInt64 ||
         type == kDouble || type == kFloat || type == kDateTime;
}

StatsOp OpFromAggregation(uint16_t aggregation) {
  switch (static_cast<AggregationType>(aggregation)) {
    case AggregationType::kSum:
      return StatsOp::kSum;
    case AggregationType::kMax:
      return StatsOp::kMax;
    case AggregationType::kMin:
      return StatsOp::kMin;
    default:
      return StatsOp::kLast;
  }
}

// |value| as |type|.
StatsValue Convert(const StatsValue& value, uint8_t type) {
  if (value.type == type) {
    return value;
  }
  const bool from_float = value.type == kDouble || value.type == kFloat;
  const bool to_float = type == kDouble || type == kFloat;
  StatsValue out;
  out.type = type;
  if (to_float) {
    out.d = from_float ? value.d : static_cast<double>(value.i);
    if (type == kFloat) {
      out.d = static_cast<float>(out.d);
    }
    return out;
  }
  int64_t i = from_float ? static_cast<int64_t>(value.d) : value.i;
  if (type == kInt32) {
    i = std::clamp<int64_t>(i, INT32_MIN, INT32_MAX);
  } else if (type == kContext) {
    i = std::clamp<int64_t>(i, 0, UINT32_MAX);
  } else if (type == kDateTime) {
    i = std::max<int64_t>(i, 0);
  }
  out.i = i;
  return out;
}

// System columns a title can't write.
bool IsSystemAttr(uint16_t attr) {
  return attr >= 0xFFF0 && attr != net::live::kStatsAttrRating;
}

std::optional<StatsValue> SkillDefault(uint16_t attr) {
  switch (attr) {
    case kStatsColumnSkill:
      return StatsValue{kInt64, 1, 0};
    case kStatsColumnGamesPlayed:
      return StatsValue{kInt64, 0, 0};
    case kStatsColumnMu:
      return StatsValue{kDouble, 0, 3.0};
    case kStatsColumnSigma:
      return StatsValue{kDouble, 0, 1.0};
    default:
      return std::nullopt;
  }
}

void PutValue(uint8_t* column, uint8_t type, const StatsValue& value) {
  // XUSER_STATS_COLUMN: column ID at 0, X_USER_DATA at 8 (type, then the
  // value at 0x10).
  column[8] = type;
  uint8_t* data = column + 0x10;
  const StatsValue v = Convert(value, type);
  switch (type) {
    case kContext:
    case kInt32:
      xe::store_and_swap<uint32_t>(data, static_cast<uint32_t>(v.i));
      break;
    case kInt64:
    case kDateTime:
      xe::store_and_swap<uint64_t>(data, static_cast<uint64_t>(v.i));
      break;
    case kDouble:
      xe::store_and_swap<double>(data, v.d);
      break;
    case kFloat:
      xe::store_and_swap<float>(data, static_cast<float>(v.d));
      break;
    default:
      break;
  }
}

}  // namespace

bool IsTrueSkillView(uint32_t view_id) {
  return (view_id >> 16) == 0xFFFE || (view_id >> 16) == 0xFFFF;
}

std::optional<StatsValue> StatsValueFromGuest(const X_USER_DATA& data) {
  const uint8_t type = static_cast<uint8_t>(data.type);
  StatsValue v;
  v.type = type;
  switch (type) {
    case kContext:
      v.i = data.data.u32;
      break;
    case kInt32:
      v.i = data.data.s32;
      break;
    case kInt64:
      v.i = data.data.s64;
      break;
    case kDateTime:
      v.i = static_cast<int64_t>(
          std::min<uint64_t>(data.data.filetime, INT64_MAX));
      break;
    case kDouble:
      v.d = data.data.f64;
      if (!std::isfinite(v.d)) {
        return std::nullopt;
      }
      break;
    case kFloat:
      v.d = data.data.f32;
      if (!std::isfinite(v.d)) {
        return std::nullopt;
      }
      break;
    default:
      return std::nullopt;
  }
  return v;
}

std::vector<StatsColumnUpdate> MapStatsWrite(
    const util::GameInfoDatabase::StatsView& view,
    const StatsProperties& properties) {
  std::vector<StatsColumnUpdate> out;
  for (const auto& column : view.shared_view.column_entries) {
    if (IsSystemAttr(column.attribute_id)) {
      continue;
    }
    auto it = properties.find(column.property_id);
    if (it == properties.end()) {
      continue;
    }
    uint8_t type = static_cast<uint8_t>((column.property_id >> 28) & 0xF);
    if (column.attribute_id == net::live::kStatsAttrRating) {
      type = kInt64;
    }
    if (!IsKeptType(type)) {
      continue;
    }
    if (std::ranges::any_of(out, [&](const StatsColumnUpdate& u) {
          return u.attr == column.attribute_id;
        })) {
      continue;
    }
    StatsColumnUpdate update;
    update.attr = column.attribute_id;
    update.op = OpFromAggregation(column.aggregation_type);
    update.value = Convert(it->second, type);
    out.push_back(update);
  }
  return out;
}

uint8_t StatsColumnType(
    const std::optional<util::GameInfoDatabase::StatsView>& view,
    uint32_t view_id, uint16_t attr) {
  switch (attr) {
    case net::live::kStatsAttrRank:
      return kInt32;
    case net::live::kStatsAttrRating:
      return kInt64;
    case net::live::kStatsAttrGamertag:
      return kWString;
    default:
      break;
  }
  if (IsTrueSkillView(view_id)) {
    if (auto d = SkillDefault(attr)) {
      return d->type;
    }
  }
  if (view) {
    for (const auto& column : view->shared_view.column_entries) {
      if (column.attribute_id == attr) {
        const uint8_t type =
            static_cast<uint8_t>((column.property_id >> 28) & 0xF);
        if (IsKeptType(type) || type == kWString) {
          return type;
        }
      }
    }
  }
  return kInt32;
}

uint32_t StatsResultsSize(const std::vector<StatsLayoutView>& views,
                          uint32_t rows) {
  uint32_t size = kStatsResultsHeaderSize;
  for (const auto& view : views) {
    size += kStatsViewSize +
            rows * (kStatsRowSize +
                    static_cast<uint32_t>(view.columns.size()) *
                        kStatsColumnSize);
  }
  return size;
}

void WriteStatsResults(uint8_t* buffer, uint32_t size, uint32_t guest,
                       const std::vector<StatsLayoutView>& views,
                       const std::vector<net::live::StatsViewData>& data,
                       const std::vector<uint64_t>* xuids) {
  std::memset(buffer, 0, size);
  const uint32_t views_size =
      kStatsResultsHeaderSize +
      static_cast<uint32_t>(views.size()) * kStatsViewSize;
  if (size < views_size) {
    return;
  }
  xe::store_and_swap<uint32_t>(buffer, static_cast<uint32_t>(views.size()));
  xe::store_and_swap<uint32_t>(buffer + 4, guest + kStatsResultsHeaderSize);
  uint32_t cursor = views_size;
  // Strings take room from the end, while it lasts.
  uint32_t tail = size;
  for (size_t i = 0; i < views.size(); ++i) {
    const StatsLayoutView& layout = views[i];
    uint8_t* view = buffer + kStatsResultsHeaderSize + i * kStatsViewSize;
    const net::live::StatsViewData* answer = nullptr;
    for (const auto& d : data) {
      if (d.view_id == layout.view_id) {
        answer = &d;
        break;
      }
    }
    // The rows, in order, and nothing for a player without one.
    std::vector<const net::live::StatsRowData*> rows;
    std::vector<uint64_t> row_xuids;
    if (xuids) {
      for (uint64_t xuid : *xuids) {
        const net::live::StatsRowData* found = nullptr;
        if (answer) {
          for (const auto& r : answer->rows) {
            if (r.xuid == xuid) {
              found = &r;
              break;
            }
          }
        }
        rows.push_back(found);
        row_xuids.push_back(xuid);
      }
    } else if (answer) {
      for (const auto& r : answer->rows) {
        rows.push_back(&r);
        row_xuids.push_back(r.xuid);
      }
    }
    const uint32_t ncols = static_cast<uint32_t>(layout.columns.size());
    const uint32_t per_row = kStatsRowSize + ncols * kStatsColumnSize;
    const uint32_t fit =
        std::min<uint32_t>(static_cast<uint32_t>(rows.size()),
                           tail > cursor ? (tail - cursor) / per_row : 0);
    xe::store_and_swap<uint32_t>(view, layout.view_id);
    xe::store_and_swap<uint32_t>(view + 4, answer ? answer->total : 0);
    xe::store_and_swap<uint32_t>(view + 8, fit);
    // Valid even with no rows: titles dereference it.
    xe::store_and_swap<uint32_t>(view + 0xC, guest + cursor);
    const uint32_t rows_at = cursor;
    uint32_t columns_at = rows_at + fit * kStatsRowSize;
    cursor = columns_at + fit * ncols * kStatsColumnSize;
    for (uint32_t r = 0; r < fit; ++r) {
      const net::live::StatsRowData* row = rows[r];
      uint8_t* out = buffer + rows_at + r * kStatsRowSize;
      // XUSER_STATS_ROW: XUID 0, rank 8, rating 0x10, gamertag 0x18 (16
      // chars), column count 0x28, columns 0x2C.
      xe::store_and_swap<uint64_t>(out, row_xuids[r]);
      xe::store_and_swap<uint32_t>(out + 8, row ? row->rank : 0);
      xe::store_and_swap<uint64_t>(out + 0x10,
                                   row ? static_cast<uint64_t>(row->rating) : 0);
      if (row) {
        std::memcpy(out + 0x18, row->gamertag.data(),
                    std::min<size_t>(row->gamertag.size(), 15));
      }
      xe::store_and_swap<uint32_t>(out + 0x28, ncols);
      xe::store_and_swap<uint32_t>(out + 0x2C,
                                   ncols ? guest + columns_at : 0);
      for (uint32_t c = 0; c < ncols; ++c) {
        uint8_t* column = buffer + columns_at + c * kStatsColumnSize;
        const uint16_t attr = layout.columns[c];
        const uint8_t type = layout.types[c];
        xe::store_and_swap<uint16_t>(column, attr);
        column[8] = type;
        if (attr == net::live::kStatsAttrRank) {
          PutValue(column, type, {kInt32, row ? row->rank : 0, 0});
          continue;
        }
        if (attr == net::live::kStatsAttrRating) {
          PutValue(column, type, {kInt64, row ? row->rating : 0, 0});
          continue;
        }
        if (type == kWString) {
          // Text the row has: its gamertag, for the gamertag column.
          if (attr == net::live::kStatsAttrGamertag && row &&
              !row->gamertag.empty()) {
            const uint32_t bytes =
                static_cast<uint32_t>(row->gamertag.size() + 1) * 2;
            if (tail >= cursor + bytes) {
              tail -= bytes;
              for (size_t k = 0; k < row->gamertag.size(); ++k) {
                xe::store_and_swap<uint16_t>(
                    buffer + tail + k * 2,
                    static_cast<uint8_t>(row->gamertag[k]));
              }
              xe::store_and_swap<uint32_t>(column + 0x10, bytes);
              xe::store_and_swap<uint32_t>(column + 0x14, guest + tail);
            }
          }
          continue;
        }
        std::optional<StatsValue> value;
        if (row) {
          for (const auto& cell : row->cells) {
            if (cell.attr == attr) {
              value = cell.value;
              break;
            }
          }
        }
        if (!value && IsTrueSkillView(layout.view_id)) {
          value = SkillDefault(attr);
        }
        if (value && IsKeptType(type)) {
          PutValue(column, type, *value);
        }
      }
      columns_at += ncols * kStatsColumnSize;
    }
  }
}

std::vector<StatsLayoutView> StatsLayout(
    KernelState* kernel_state,
    const std::vector<net::live::StatsSpec>& specs) {
  auto* db = kernel_state && kernel_state->emulator()
                 ? kernel_state->emulator()->game_info_database()
                 : nullptr;
  std::vector<StatsLayoutView> out;
  for (const auto& spec : specs) {
    StatsLayoutView view;
    view.view_id = spec.view_id;
    view.columns = spec.columns;
    const auto spa = db ? db->GetStatsView(spec.view_id) : std::nullopt;
    for (uint16_t attr : spec.columns) {
      view.types.push_back(StatsColumnType(spa, spec.view_id, attr));
    }
    out.push_back(std::move(view));
  }
  return out;
}

void FlushLiveStats(KernelState* kernel_state, const StatsCache& cache) {
  if (cache.empty()) {
    return;
  }
  const uint64_t me = LiveStatsAccountXuid();
  auto* db = kernel_state->emulator()
                 ? kernel_state->emulator()->game_info_database()
                 : nullptr;
  std::vector<net::live::StatsViewWrite> views;
  size_t others = 0;
  for (const auto& [xuid, by_view] : cache) {
    if (!me || xuid != me) {
      ++others;
      continue;
    }
    for (const auto& [view_id, properties] : by_view) {
      net::live::StatsViewWrite write;
      write.view_id = view_id;
      if (!IsTrueSkillView(view_id)) {
        const auto spa = db ? db->GetStatsView(view_id) : std::nullopt;
        if (!spa) {
          continue;
        }
        write.columns = MapStatsWrite(*spa, properties);
        if (write.columns.empty()) {
          continue;
        }
      }
      views.push_back(std::move(write));
    }
  }
  if (others) {
    XELOGI("Live stats: {} other player(s) left to their own consoles",
           others);
  }
  if (views.empty()) {
    return;
  }
  auto directory = LiveStatsDirectory(kernel_state);
  if (!directory) {
    XELOGI("Live stats: no lobby to keep {} view(s)", views.size());
    return;
  }
  XELOGI("Live stats: flushing {} view(s) for {:016X}", views.size(), me);
  // The lobby takes 16 views a write.
  for (size_t i = 0; i < views.size(); i += net::live::kStatsMaxWriteViews) {
    const size_t end =
        std::min(views.size(), i + net::live::kStatsMaxWriteViews);
    directory->WriteStats(std::vector<net::live::StatsViewWrite>(
        views.begin() + i, views.begin() + end));
  }
}

std::optional<std::vector<net::live::StatsViewData>> ReadLiveStats(
    KernelState* kernel_state, uint32_t title_id,
    const std::vector<uint64_t>& xuids,
    const std::vector<net::live::StatsSpec>& specs) {
  if (title_id && title_id != kernel_state->title_id()) {
    return std::nullopt;
  }
  auto directory = LiveStatsDirectory(kernel_state);
  if (!directory || xuids.empty() || specs.empty()) {
    return std::nullopt;
  }
  return directory->ReadStats(xuids, specs, kStatsReadTimeoutMs);
}

std::optional<std::vector<net::live::StatsViewData>> EnumerateLiveStats(
    KernelState* kernel_state, uint32_t title_id, net::live::StatsEnumBy by,
    uint64_t pivot, uint32_t rows,
    const std::vector<net::live::StatsSpec>& specs) {
  if (title_id && title_id != kernel_state->title_id()) {
    return std::nullopt;
  }
  auto directory = LiveStatsDirectory(kernel_state);
  if (!directory || specs.empty()) {
    return std::nullopt;
  }
  return directory->EnumerateStats(by, pivot, rows, specs,
                                   kStatsReadTimeoutMs);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
