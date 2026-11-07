/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xam/xam_live_stats.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using namespace xe::kernel::xam;
using xe::kernel::util::GameInfoDatabase;
using xe::net::live::StatsOp;
using xe::net::live::StatsValue;

constexpr uint32_t kGuest = 0x40002000;

GameInfoDatabase::Field Column(uint32_t property, uint16_t attr,
                               uint16_t aggregation) {
  GameInfoDatabase::Field f{};
  f.property_id = property;
  f.attribute_id = attr;
  f.aggregation_type = aggregation;
  return f;
}

// A view shaped like a shooter's (made up, not any title's SPA): the system
// rank and gamertag, two summed int32 counters, a best (max) int32, a last
// double, and a summed int64 rating.
GameInfoDatabase::StatsView SampleView() {
  GameInfoDatabase::StatsView v{};
  v.view.id = 1;
  auto& c = v.shared_view.column_entries;
  c.push_back(Column(0x10008001, 0xFFFF, 0x8001));  // rank
  c.push_back(Column(0x40008002, 0xFFFD, 0x8001));  // gamertag
  c.push_back(Column(0x1000000C, 1, 0x8003));       // kills, sum
  c.push_back(Column(0x1000000D, 2, 0x8003));       // deaths, sum
  c.push_back(Column(0x1000000C, 3, 0x800B));       // best kills, max
  c.push_back(Column(0x3000000E, 4, 0x8001));       // accuracy, last
  c.push_back(Column(0x10000010, 5, 0x8009));       // fastest, min
  c.push_back(Column(0x20000009, 0xFFFE, 0x8003));  // rating, sum
  c.push_back(Column(0x10008011, 0xFFFA, 0x8001));  // attachment size
  return v;
}

const xe::net::live::StatsColumnUpdate* Find(
    const std::vector<xe::net::live::StatsColumnUpdate>& updates,
    uint16_t attr) {
  for (const auto& u : updates) {
    if (u.attr == attr) {
      return &u;
    }
  }
  return nullptr;
}

TEST_CASE("Live stats writes become column updates from the SPA",
          "[live_stats]") {
  StatsProperties written;
  written[0x1000000C] = {1, 12, 0};      // kills
  written[0x1000000D] = {1, 3, 0};       // deaths
  written[0x3000000E] = {3, 0, 0.375};   // accuracy
  written[0x20000009] = {1, 250, 0};     // rating, written as int32
  written[0x10008001] = {1, 99, 0};      // the rank: the title can't set it
  written[0x10000099] = {1, 5, 0};       // in no column
  const auto updates = MapStatsWrite(SampleView(), written);
  REQUIRE(updates.size() == 5);
  REQUIRE(Find(updates, 1));
  CHECK(Find(updates, 1)->op == StatsOp::kSum);
  CHECK(Find(updates, 1)->value == StatsValue{1, 12, 0});
  CHECK(Find(updates, 2)->value == StatsValue{1, 3, 0});
  // One property can feed two columns with their own aggregations.
  REQUIRE(Find(updates, 3));
  CHECK(Find(updates, 3)->op == StatsOp::kMax);
  CHECK(Find(updates, 3)->value == StatsValue{1, 12, 0});
  CHECK(Find(updates, 4)->op == StatsOp::kLast);
  CHECK(Find(updates, 4)->value.d == 0.375);
  // The rating always goes as int64.
  REQUIRE(Find(updates, 0xFFFE));
  CHECK(Find(updates, 0xFFFE)->value == StatsValue{2, 250, 0});
  CHECK_FALSE(Find(updates, 0xFFFF));
  CHECK_FALSE(Find(updates, 0xFFFD));
  CHECK_FALSE(Find(updates, 5));  // not written
}

TEST_CASE("Live stats values from guest X_USER_DATA", "[live_stats]") {
  X_USER_DATA d{};
  d.type = X_USER_DATA_TYPE::INT64;
  d.data.s64 = -5;
  CHECK(StatsValueFromGuest(d) == StatsValue{2, -5, 0});
  d.type = X_USER_DATA_TYPE::FLOAT;
  d.data.f32 = 1.5f;
  CHECK(StatsValueFromGuest(d)->d == 1.5);
  d.type = X_USER_DATA_TYPE::WSTRING;
  CHECK_FALSE(StatsValueFromGuest(d));
  d.type = X_USER_DATA_TYPE::DOUBLE;
  d.data.f64 = std::numeric_limits<double>::infinity();
  CHECK_FALSE(StatsValueFromGuest(d));
}

TEST_CASE("Live stats column types", "[live_stats]") {
  const std::optional<GameInfoDatabase::StatsView> view = SampleView();
  CHECK(StatsColumnType(view, 1, 1) == 1);
  CHECK(StatsColumnType(view, 1, 4) == 3);
  CHECK(StatsColumnType(view, 1, 0xFFFE) == 2);
  CHECK(StatsColumnType(view, 1, 0xFFFF) == 1);
  CHECK(StatsColumnType(view, 1, 0xFFFD) == 4);
  CHECK(StatsColumnType(view, 1, 77) == 1);  // unknown: int32
  CHECK(StatsColumnType(std::nullopt, 0xFFFF0001, 63) == 3);
  CHECK(StatsColumnType(std::nullopt, 0xFFFE0001, 62) == 2);
  CHECK(IsTrueSkillView(0xFFFF0000));
  CHECK(IsTrueSkillView(0xFFFE0003));
  CHECK_FALSE(IsTrueSkillView(0x0000FFFF));
}

uint32_t U32(const std::vector<uint8_t>& b, uint32_t guest_or_offset) {
  const uint32_t off = guest_or_offset >= kGuest ? guest_or_offset - kGuest
                                                 : guest_or_offset;
  return xe::load_and_swap<uint32_t>(b.data() + off);
}
uint64_t U64(const std::vector<uint8_t>& b, uint32_t guest_or_offset) {
  const uint32_t off = guest_or_offset >= kGuest ? guest_or_offset - kGuest
                                                 : guest_or_offset;
  return xe::load_and_swap<uint64_t>(b.data() + off);
}

TEST_CASE("Live stats results: one row per player asked, in the title's buffer",
          "[live_stats]") {
  std::vector<StatsLayoutView> layout = {
      {1, {0xFFFF, 1, 4, 0xFFFD}, {1, 1, 3, 4}},
      {0xFFFF0000, {61, 63, 64}, {2, 3, 3}},
  };
  std::vector<xe::net::live::StatsViewData> data(1);
  data[0].view_id = 1;
  data[0].total = 40;
  xe::net::live::StatsRowData row;
  row.xuid = 0x0009000000000002ull;
  row.gamertag = "Arbiter";
  row.rank = 7;
  row.rating = 1234;
  row.cells = {{1, {1, 30, 0}}, {4, {3, 0, 0.5}}};
  data[0].rows.push_back(row);
  const std::vector<uint64_t> xuids = {0x0009000000000001ull,
                                       0x0009000000000002ull};
  // The size the title's library works out, plus text room.
  const uint32_t size = StatsResultsSize(layout, 2) + 64;
  REQUIRE(StatsResultsSize(layout, 2) ==
          0x8 + 2 * 0x10 + 2 * (0x30 + 4 * 0x18) + 2 * (0x30 + 3 * 0x18));
  std::vector<uint8_t> b(size, 0xCD);
  WriteStatsResults(b.data(), size, kGuest, layout, data, &xuids);

  CHECK(U32(b, 0) == 2);
  CHECK(U32(b, 4) == kGuest + 8);
  // View 1.
  CHECK(U32(b, 8) == 1);
  CHECK(U32(b, 0xC) == 40);
  CHECK(U32(b, 0x10) == 2);
  const uint32_t rows = U32(b, 0x14);
  CHECK(rows == kGuest + 0x28);
  // The first player has no row: rank 0, zeros, the XUID asked for.
  CHECK(U64(b, rows) == 0x0009000000000001ull);
  CHECK(U32(b, rows + 8) == 0);
  CHECK(U32(b, rows + 0x28) == 4);
  const uint32_t cols0 = U32(b, rows + 0x2C);
  CHECK(cols0 == rows + 2 * 0x30);
  CHECK(xe::load_and_swap<uint16_t>(b.data() + cols0 - kGuest + 0x18) == 1);
  CHECK(b[cols0 - kGuest + 0x18 + 8] == 1);   // int32
  CHECK(U32(b, cols0 + 0x18 + 0x10) == 0);
  // The second has the lobby's row.
  const uint32_t r1 = rows + 0x30;
  CHECK(U64(b, r1) == 0x0009000000000002ull);
  CHECK(U32(b, r1 + 8) == 7);
  CHECK(U64(b, r1 + 0x10) == 1234);
  CHECK(std::strncmp(reinterpret_cast<const char*>(b.data() + r1 - kGuest +
                                                   0x18),
                     "Arbiter", 16) == 0);
  const uint32_t cols1 = U32(b, r1 + 0x2C);
  CHECK(cols1 == cols0 + 4 * 0x18);
  CHECK(xe::load_and_swap<uint16_t>(b.data() + cols1 - kGuest) == 0xFFFF);
  CHECK(U32(b, cols1 + 0x10) == 7);                      // rank column
  CHECK(U32(b, cols1 + 0x18 + 0x10) == 30);              // kills
  CHECK(b[cols1 + 0x30 + 8 - kGuest] == 3);              // double
  CHECK(xe::load_and_swap<double>(b.data() + cols1 + 0x30 + 0x10 - kGuest) ==
        0.5);
  // The gamertag column points at its text at the end of the buffer.
  CHECK(b[cols1 + 0x48 + 8 - kGuest] == 4);
  const uint32_t text_bytes = U32(b, cols1 + 0x48 + 0x10);
  const uint32_t text = U32(b, cols1 + 0x48 + 0x14);
  CHECK(text_bytes == 16);
  CHECK(text == kGuest + size - 16);
  CHECK(xe::load_and_swap<uint16_t>(b.data() + text - kGuest) == 'A');
  CHECK(xe::load_and_swap<uint16_t>(b.data() + text - kGuest + 14) == 0);
  // The skill view: nobody has a row, so Live's defaults.
  CHECK(U32(b, 0x18) == 0xFFFF0000);
  CHECK(U32(b, 0x20) == 2);
  const uint32_t srows = U32(b, 0x24);
  CHECK(srows == cols1 + 4 * 0x18);
  const uint32_t scols = U32(b, srows + 0x2C);
  CHECK(U64(b, scols + 0x10) == 1);                      // skill
  CHECK(xe::load_and_swap<double>(b.data() + scols + 0x18 + 0x10 - kGuest) ==
        3.0);                                            // mu
  CHECK(xe::load_and_swap<double>(b.data() + scols + 0x30 + 0x10 - kGuest) ==
        1.0);                                            // sigma
}

TEST_CASE("Live stats results: pages, and rows that don't fit are left out",
          "[live_stats]") {
  std::vector<StatsLayoutView> layout = {{3, {1}, {1}}};
  std::vector<xe::net::live::StatsViewData> data(1);
  data[0].view_id = 3;
  data[0].total = 9;
  for (uint32_t i = 0; i < 3; ++i) {
    xe::net::live::StatsRowData row;
    row.xuid = 0x0009000000000010ull + i;
    row.rank = 4 + i;
    row.cells = {{1, {1, 100 - int64_t(i), 0}}};
    data[0].rows.push_back(row);
  }
  // Room for two rows only.
  const uint32_t size = StatsResultsSize(layout, 2);
  std::vector<uint8_t> b(size, 0xCD);
  WriteStatsResults(b.data(), size, kGuest, layout, data, nullptr);
  CHECK(U32(b, 0xC) == 9);
  CHECK(U32(b, 0x10) == 2);
  const uint32_t rows = U32(b, 0x14);
  CHECK(U32(b, rows + 8) == 4);
  CHECK(U32(b, rows + 0x30 + 8) == 5);
  const uint32_t cols = U32(b, rows + 0x30 + 0x2C);
  CHECK(U32(b, cols + 0x10) == 99);

  // A view the lobby sent nothing for: no rows, a valid pointer.
  std::vector<uint8_t> e(StatsResultsSize(layout, 1), 0xCD);
  WriteStatsResults(e.data(), static_cast<uint32_t>(e.size()), kGuest, layout,
                    {}, nullptr);
  CHECK(U32(e, 8) == 3);
  CHECK(U32(e, 0x10) == 0);
  CHECK(U32(e, 0x14) == kGuest + 0x18);
}

}  // namespace
