/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>
#include <vector>

#include "xenia/kernel/xenumerator.h"

#include "third_party/catch/include/catch.hpp"

namespace {

using xe::kernel::XUserStatsEnumerator;

constexpr uint32_t kGuest = 0x40001000;

TEST_CASE("Empty stats results: every view has no rows", "[xuser_stats]") {
  const std::vector<uint32_t> views = {7, 0x20, 3};
  const uint32_t needed = XUserStatsEnumerator::EmptyResultsSize(views.size());
  REQUIRE(needed == 0x8 + 3 * 0x10);

  // Room for a row after the views, as titles size their buffers.
  std::vector<uint8_t> buffer(needed + 0x30, 0xCD);
  XUserStatsEnumerator::WriteEmptyResults(
      buffer.data(), static_cast<uint32_t>(buffer.size()), kGuest, views);

  const auto* results = reinterpret_cast<
      const XUserStatsEnumerator::XUSER_STATS_READ_RESULTS*>(buffer.data());
  CHECK(results->num_views == 3);
  CHECK(results->views_ptr == kGuest + 0x8);
  const auto* out =
      reinterpret_cast<const XUserStatsEnumerator::XUSER_STATS_VIEW*>(
          results + 1);
  for (size_t i = 0; i < views.size(); ++i) {
    CHECK(out[i].view_id == views[i]);
    CHECK(out[i].total_view_rows == 0);
    CHECK(out[i].num_rows == 0);
    // Valid even with no rows: titles dereference it.
    CHECK(out[i].rows_ptr == kGuest + needed);
  }
  // Whatever follows the views reads as zero.
  for (uint32_t i = needed; i < buffer.size(); ++i) {
    CHECK(buffer[i] == 0);
  }
}

TEST_CASE("Empty stats results: exactly the size needed", "[xuser_stats]") {
  const std::vector<uint32_t> views = {1};
  const uint32_t needed = XUserStatsEnumerator::EmptyResultsSize(views.size());
  std::vector<uint8_t> buffer(needed, 0xCD);
  XUserStatsEnumerator::WriteEmptyResults(buffer.data(), needed, kGuest,
                                          views);
  const auto* results = reinterpret_cast<
      const XUserStatsEnumerator::XUSER_STATS_READ_RESULTS*>(buffer.data());
  CHECK(results->num_views == 1);
  const auto* view =
      reinterpret_cast<const XUserStatsEnumerator::XUSER_STATS_VIEW*>(
          results + 1);
  CHECK(view->view_id == 1);
  CHECK(view->num_rows == 0);
}

}  // namespace
