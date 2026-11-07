/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_LIVE_STATS_H_
#define XENIA_KERNEL_XAM_XAM_LIVE_STATS_H_

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "xenia/kernel/util/game_info_database.h"
#include "xenia/kernel/xam/user_data.h"
#include "xenia/net/live_directory.h"

namespace xe {
namespace kernel {
class KernelState;

namespace xam {

// Live stats and leaderboards without Live: a title's stats writes become
// column updates of the player's own rows, from the title's SPA (each column
// of a view is tied to a property and says how a new value combines with the
// stored one), and go to the lobby's store for the title with the session
// directory. Reads and the stats enumerators ask the same store. Without it
// (no lobby, a lobby without stats, not signed in) every view reads back
// with no rows, as before.

// Properties a title wrote into one view, by property ID.
using StatsProperties = std::map<uint32_t, net::live::StatsValue>;
// What a session holds until it flushes: XUID -> view -> properties.
using StatsCache = std::map<uint64_t, std::map<uint32_t, StatsProperties>>;

// TrueSkill views (0xFFFE.... standard, 0xFFFF.... ranked) aren't in the SPA;
// Live kept skill, games played, mu and sigma in columns 61 to 64.
bool IsTrueSkillView(uint32_t view_id);
constexpr uint16_t kStatsColumnSkill = 61;
constexpr uint16_t kStatsColumnGamesPlayed = 62;
constexpr uint16_t kStatsColumnMu = 63;
constexpr uint16_t kStatsColumnSigma = 64;

// An X_USER_DATA a title passed, when it's a type stats keep (context, int32,
// int64, double, float, datetime).
std::optional<net::live::StatsValue> StatsValueFromGuest(
    const X_USER_DATA& data);

// The column updates |properties| make in |view|: one for each column tied
// to a written property, with the column's aggregation. Rank, gamertag and
// other system columns are left out; the rating goes as int64.
std::vector<net::live::StatsColumnUpdate> MapStatsWrite(
    const util::GameInfoDatabase::StatsView& view,
    const StatsProperties& properties);

// The X_USER_DATA type of column |attr| of a view: from the SPA (the type of
// the property it's tied to), TrueSkill columns as Live gave them, else int32.
uint8_t StatsColumnType(
    const std::optional<util::GameInfoDatabase::StatsView>& view,
    uint32_t view_id, uint16_t attr);

// A view as a read asks for it, with the type of each column.
struct StatsLayoutView {
  uint32_t view_id = 0;
  std::vector<uint16_t> columns;
  std::vector<uint8_t> types;
};

// Guest layout sizes: XUSER_STATS_READ_RESULTS 0x8, XUSER_STATS_VIEW 0x10,
// XUSER_STATS_ROW 0x30, XUSER_STATS_COLUMN 0x18.
constexpr uint32_t kStatsResultsHeaderSize = 0x8;
constexpr uint32_t kStatsViewSize = 0x10;
constexpr uint32_t kStatsRowSize = 0x30;
constexpr uint32_t kStatsColumnSize = 0x18;

// The bytes results with |rows| rows in every view take, without strings.
uint32_t StatsResultsSize(const std::vector<StatsLayoutView>& views,
                          uint32_t rows);

// Writes XUSER_STATS_READ_RESULTS into |buffer| (|size| bytes, which the
// guest sees at |guest|): the views in the order of |views|, with the rows of
// |data| for each (matched by view ID). With |xuids| (XUserReadStats), one
// row per XUID in that order, a player without one with rank 0 and zeros (a
// TrueSkill view: Live's defaults). Every row has every asked column, typed as
// |views| says; a gamertag column (0xFFFD) gets its text after the rows while
// there's room. Rows that don't fit are left out. Zeroes the buffer first.
void WriteStatsResults(uint8_t* buffer, uint32_t size, uint32_t guest,
                       const std::vector<StatsLayoutView>& views,
                       const std::vector<net::live::StatsViewData>& data,
                       const std::vector<uint64_t>* xuids);

// The layout of |specs| (from the title) with the SPA's column types.
std::vector<StatsLayoutView> StatsLayout(
    KernelState* kernel_state,
    const std::vector<net::live::StatsSpec>& specs);

// Sends what |cache| holds for the signed-in account (the only rows it may
// write) to the lobby: one write for all its views. Other players' entries
// are dropped, as each console writes its own players. Returns at once.
void FlushLiveStats(KernelState* kernel_state, const StatsCache& cache);

// XUserReadStats and the stats enumerators against the lobby. Nothing when
// there's no store to ask, or |title_id| isn't the running title.
std::optional<std::vector<net::live::StatsViewData>> ReadLiveStats(
    KernelState* kernel_state, uint32_t title_id,
    const std::vector<uint64_t>& xuids,
    const std::vector<net::live::StatsSpec>& specs);
std::optional<std::vector<net::live::StatsViewData>> EnumerateLiveStats(
    KernelState* kernel_state, uint32_t title_id, net::live::StatsEnumBy by,
    uint64_t pivot, uint32_t rows,
    const std::vector<net::live::StatsSpec>& specs);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_LIVE_STATS_H_
