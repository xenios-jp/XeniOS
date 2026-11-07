/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/live_directory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>
#include <set>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia/base/logging.h"
#include "xenia/net/live_account.h"

namespace xe::net::live {

namespace {

// 16 hex digits, or 0.
uint64_t StrictXuid(std::string_view text) {
  if (text.size() != 16 || text.find_first_not_of("0123456789abcdefABCDEF") !=
                               std::string_view::npos) {
    return 0;
  }
  return std::strtoull(std::string(text).c_str(), nullptr, 16);
}

using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

std::string ToHex(const uint8_t* data, size_t size) {
  std::string text;
  text.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    text += fmt::format("{:02x}", data[i]);
  }
  return text;
}

std::optional<std::vector<uint8_t>> FromHex(std::string_view text) {
  if (text.size() % 2) {
    return std::nullopt;
  }
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<uint8_t> bytes(text.size() / 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    const int high = digit(text[i * 2]);
    const int low = digit(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[i] = uint8_t(high << 4 | low);
  }
  return bytes;
}

void WritePairs(Writer& w, const char* key,
                const std::vector<std::pair<uint32_t, uint32_t>>& pairs) {
  w.Key(key);
  w.StartArray();
  for (const auto& [id, value] : pairs) {
    w.StartArray();
    w.Uint(id);
    w.Uint(value);
    w.EndArray();
  }
  w.EndArray();
}

bool ReadUint(const rapidjson::Value& object, const char* key,
              uint32_t& out) {
  auto it = object.FindMember(key);
  if (it == object.MemberEnd()) {
    return true;  // Missing ones are 0.
  }
  if (!it->value.IsUint()) {
    return false;
  }
  out = it->value.GetUint();
  return true;
}

std::optional<DirectoryAdvert> ReadAdvert(const rapidjson::Value& v) {
  if (!v.IsObject()) {
    return std::nullopt;
  }
  DirectoryAdvert a;
  auto id = v.FindMember("id");
  auto info = v.FindMember("info");
  auto room = v.FindMember("room");
  if (id == v.MemberEnd() || !id->value.IsString() ||
      info == v.MemberEnd() || !info->value.IsString() ||
      room == v.MemberEnd() || !room->value.IsString()) {
    return std::nullopt;
  }
  a.id = id->value.GetString();
  // No more than an XSESSION_INFO, and a room code's length.
  if (info->value.GetStringLength() > 2 * 0x3C ||
      room->value.GetStringLength() > 64) {
    return std::nullopt;
  }
  auto bytes = FromHex(info->value.GetString());
  if (!bytes || a.id.size() != 16) {
    return std::nullopt;
  }
  a.info = std::move(*bytes);
  a.room = room->value.GetString();
  if (!ReadUint(v, "flags", a.flags)) {
    return std::nullopt;
  }
  if (auto slots = v.FindMember("slots"); slots != v.MemberEnd()) {
    if (!slots->value.IsObject() ||
        !ReadUint(slots->value, "public", a.public_slots) ||
        !ReadUint(slots->value, "private", a.private_slots) ||
        !ReadUint(slots->value, "usedPublic", a.used_public) ||
        !ReadUint(slots->value, "usedPrivate", a.used_private)) {
      return std::nullopt;
    }
  }
  if (auto state = v.FindMember("state");
      state != v.MemberEnd() && state->value.IsString()) {
    a.playing = std::string_view(state->value.GetString()) == "playing";
  }
  if (auto jip = v.FindMember("jip");
      jip != v.MemberEnd() && jip->value.IsBool()) {
    a.join_in_progress = jip->value.GetBool();
  }
  if (auto xuid = v.FindMember("xuid");
      xuid != v.MemberEnd() && xuid->value.IsString()) {
    a.xuid = StrictXuid(xuid->value.GetString());
  }
  if (auto mode = v.FindMember("mode");
      mode != v.MemberEnd() && mode->value.IsString()) {
    a.mode = mode->value.GetString();
  }
  if (auto map = v.FindMember("map");
      map != v.MemberEnd() && map->value.IsString()) {
    a.map = map->value.GetString();
  }
  if (auto contexts = v.FindMember("contexts"); contexts != v.MemberEnd()) {
    if (!contexts->value.IsArray()) {
      return std::nullopt;
    }
    for (const auto& pair : contexts->value.GetArray()) {
      if (!pair.IsArray() || pair.Size() != 2 || !pair[0].IsUint() ||
          !pair[1].IsUint()) {
        return std::nullopt;
      }
      a.contexts.emplace_back(pair[0].GetUint(), pair[1].GetUint());
    }
  }
  if (auto props = v.FindMember("props"); props != v.MemberEnd()) {
    if (!props->value.IsArray()) {
      return std::nullopt;
    }
    for (const auto& pair : props->value.GetArray()) {
      if (!pair.IsArray() || pair.Size() != 2 || !pair[0].IsUint() ||
          !pair[1].IsString()) {
        return std::nullopt;
      }
      auto data = FromHex(pair[1].GetString());
      if (!data || data->size() > 16) {
        return std::nullopt;
      }
      a.props.emplace_back(pair[0].GetUint(), std::move(*data));
    }
  }
  if (auto strings = v.FindMember("strings"); strings != v.MemberEnd()) {
    if (!strings->value.IsArray()) {
      return std::nullopt;
    }
    for (const auto& pair : strings->value.GetArray()) {
      if (!pair.IsArray() || pair.Size() != 2 || !pair[0].IsUint() ||
          !pair[1].IsString()) {
        return std::nullopt;
      }
      a.strings.emplace_back(pair[0].GetUint(), pair[1].GetString());
    }
  }
  return a;
}

}  // namespace

std::string HexId(const uint8_t (&id)[8]) { return ToHex(id, 8); }

namespace {

// |text| cut to 64 bytes, not inside a UTF-8 sequence.
std::string CutText(std::string text) {
  if (text.size() > 64) {
    size_t n = 64;
    while (n && (uint8_t(text[n]) & 0xC0) == 0x80) {
      --n;
    }
    text.resize(n);
  }
  return text;
}

}  // namespace

DirectoryAdvert FitDirectoryLimits(DirectoryAdvert a) {
  for (uint32_t* slots : {&a.public_slots, &a.private_slots, &a.used_public,
                          &a.used_private}) {
    *slots = std::min(*slots, kDirectoryMaxSlots);
  }
  // The first of each id, up to the limit.
  auto fit = [](auto& pairs) {
    std::set<uint32_t> seen;
    std::erase_if(pairs, [&](const auto& pair) {
      return seen.size() >= kDirectoryMaxPairs ||
             !seen.insert(pair.first).second;
    });
  };
  fit(a.contexts);
  fit(a.props);
  if (a.strings.size() > kDirectoryMaxStrings) {
    a.strings.resize(kDirectoryMaxStrings);
  }
  return a;
}

std::string EncodeAdvertise(const DirectoryAdvert& advert, uint32_t ttl) {
  const DirectoryAdvert a = FitDirectoryLimits(advert);
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  w.StartObject();
  w.Key("t");
  w.String("advertise");
  w.Key("id");
  w.String(a.id.c_str());
  w.Key("info");
  w.String(ToHex(a.info.data(), a.info.size()).c_str());
  w.Key("room");
  w.String(a.room.c_str());
  w.Key("flags");
  w.Uint(a.flags);
  w.Key("slots");
  w.StartObject();
  w.Key("public");
  w.Uint(a.public_slots);
  w.Key("private");
  w.Uint(a.private_slots);
  w.Key("usedPublic");
  w.Uint(a.used_public);
  w.Key("usedPrivate");
  w.Uint(a.used_private);
  w.EndObject();
  w.Key("state");
  w.String(a.playing ? "playing" : "lobby");
  w.Key("jip");
  w.Bool(a.join_in_progress);
  if (a.xuid) {
    w.Key("xuid");
    w.String(fmt::format("{:016x}", a.xuid).c_str());
  }
  if (!a.mode.empty()) {
    w.Key("mode");
    w.String(CutText(a.mode).c_str());
  }
  if (!a.map.empty()) {
    w.Key("map");
    w.String(CutText(a.map).c_str());
  }
  WritePairs(w, "contexts", a.contexts);
  w.Key("props");
  w.StartArray();
  for (const auto& [id, data] : a.props) {
    w.StartArray();
    w.Uint(id);
    w.String(ToHex(data.data(), data.size()).c_str());
    w.EndArray();
  }
  w.EndArray();
  w.Key("strings");
  w.StartArray();
  for (const auto& [id, text] : a.strings) {
    w.StartArray();
    w.Uint(id);
    w.String(text.c_str());
    w.EndArray();
  }
  w.EndArray();
  w.Key("ttl");
  w.Uint(ttl);
  w.EndObject();
  return buffer.GetString();
}

std::string EncodeRefresh(uint32_t ttl) {
  return fmt::format(R"({{"t":"refresh","ttl":{}}})", ttl);
}

std::string EncodeUpdate(std::string_view id, uint32_t used_public,
                         uint32_t used_private, uint32_t ttl) {
  return fmt::format(
      R"({{"t":"update","id":"{}","slots":{{"usedPublic":{},"usedPrivate":{}}},"ttl":{}}})",
      id, std::min(used_public, kDirectoryMaxSlots),
      std::min(used_private, kDirectoryMaxSlots), ttl);
}

std::string EncodeDirectoryAuth(uint64_t xuid, std::string_view token) {
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  w.StartObject();
  w.Key("t");
  w.String("auth");
  w.Key("xuid");
  w.String(fmt::format("{:016x}", xuid).c_str());
  w.Key("token");
  w.String(token.data(), rapidjson::SizeType(token.size()));
  w.EndObject();
  return buffer.GetString();
}

std::string EncodeWithdraw(std::string_view id) {
  return fmt::format(R"({{"t":"withdraw","id":"{}"}})", id);
}

std::string EncodeSearch(
    const std::vector<std::pair<uint32_t, uint32_t>>& contexts, uint32_t max,
    bool in_progress, uint32_t seq) {
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  w.StartObject();
  w.Key("t");
  w.String("search");
  WritePairs(w, "contexts", contexts);
  w.Key("inProgress");
  w.Bool(in_progress);
  w.Key("max");
  w.Uint(std::clamp<uint32_t>(max, 1, 50));
  w.Key("seq");
  w.Uint(seq);
  w.EndObject();
  return buffer.GetString();
}

std::string EncodeLookup(const std::vector<std::string>& ids, uint32_t seq) {
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  w.StartObject();
  w.Key("t");
  w.String("lookup");
  w.Key("ids");
  w.StartArray();
  for (size_t i = 0; i < ids.size() && i < 16; ++i) {
    w.String(ids[i].c_str());
  }
  w.EndArray();
  w.Key("seq");
  w.Uint(seq);
  w.EndObject();
  return buffer.GetString();
}

namespace {

const char* StatsOpName(StatsOp op) {
  switch (op) {
    case StatsOp::kSum:
      return "sum";
    case StatsOp::kMax:
      return "max";
    case StatsOp::kMin:
      return "min";
    default:
      return "last";
  }
}

bool IsStatsType(uint8_t type) {
  return type == 0 || type == 1 || type == 2 || type == 3 || type == 5 ||
         type == 7;
}

bool IsSkillViewId(uint32_t view_id) {
  return (view_id >> 16) == 0xFFFE || (view_id >> 16) == 0xFFFF;
}

void WriteStatsValue(Writer& w, const StatsValue& v) {
  switch (v.type) {
    case 0:
      w.Uint(static_cast<uint32_t>(v.i));
      break;
    case 1:
      w.Int(static_cast<int32_t>(v.i));
      break;
    case 3:
    case 5:
      w.Double(std::isfinite(v.d) ? v.d : 0.0);
      break;
    default:  // int64, datetime: decimal strings
      w.String(fmt::format("{}", v.i).c_str());
      break;
  }
}

std::optional<StatsValue> ReadStatsValue(uint8_t type,
                                         const rapidjson::Value& v) {
  StatsValue value;
  value.type = type;
  switch (type) {
    case 0:
    case 1:
      if (!v.IsInt64()) return std::nullopt;
      value.i = v.GetInt64();
      break;
    case 3:
    case 5:
      if (!v.IsNumber()) return std::nullopt;
      value.d = v.GetDouble();
      break;
    case 2:
    case 7: {
      if (!v.IsString()) return std::nullopt;
      char* end = nullptr;
      value.i = std::strtoll(v.GetString(), &end, 10);
      if (!end || *end) return std::nullopt;
      break;
    }
    default:
      return std::nullopt;
  }
  return value;
}

void WriteSpecs(Writer& w, const std::vector<StatsSpec>& specs) {
  w.Key("views");
  w.StartArray();
  for (size_t i = 0; i < specs.size() && i < kStatsMaxReadViews; ++i) {
    w.StartObject();
    w.Key("id");
    w.Uint(specs[i].view_id);
    w.Key("cols");
    w.StartArray();
    for (size_t c = 0; c < specs[i].columns.size() && c < kStatsMaxColumns;
         ++c) {
      w.Uint(specs[i].columns[c]);
    }
    w.EndArray();
    w.EndObject();
  }
  w.EndArray();
}

std::vector<StatsViewData> ReadStatsViews(const rapidjson::Value& views) {
  std::vector<StatsViewData> out;
  if (!views.IsArray()) return out;
  for (const auto& v : views.GetArray()) {
    if (!v.IsObject()) continue;
    StatsViewData view;
    ReadUint(v, "id", view.view_id);
    ReadUint(v, "total", view.total);
    auto rows = v.FindMember("rows");
    if (rows != v.MemberEnd() && rows->value.IsArray()) {
      for (const auto& r : rows->value.GetArray()) {
        if (!r.IsObject()) continue;
        StatsRowData row;
        auto xuid = r.FindMember("xuid");
        if (xuid == r.MemberEnd() || !xuid->value.IsString()) continue;
        row.xuid = std::strtoull(xuid->value.GetString(), nullptr, 16);
        if (auto g = r.FindMember("gamertag");
            g != r.MemberEnd() && g->value.IsString()) {
          row.gamertag = CleanGamertag(g->value.GetString());
        }
        ReadUint(r, "rank", row.rank);
        if (auto rating = r.FindMember("rating");
            rating != r.MemberEnd() && rating->value.IsString()) {
          row.rating = std::strtoll(rating->value.GetString(), nullptr, 10);
        }
        auto cols = r.FindMember("cols");
        if (cols != r.MemberEnd() && cols->value.IsArray()) {
          for (const auto& c : cols->value.GetArray()) {
            if (!c.IsArray() || c.Size() != 3 || !c[0].IsUint() ||
                !c[1].IsUint() || c[0].GetUint() > 0xFFFF ||
                !IsStatsType(static_cast<uint8_t>(c[1].GetUint())) ||
                c[1].GetUint() > 7) {
              continue;
            }
            auto value =
                ReadStatsValue(static_cast<uint8_t>(c[1].GetUint()), c[2]);
            if (!value) continue;
            row.cells.push_back(
                {static_cast<uint16_t>(c[0].GetUint()), *value});
          }
        }
        view.rows.push_back(std::move(row));
      }
    }
    out.push_back(std::move(view));
  }
  return out;
}

}  // namespace

std::string EncodeStatsWrite(const std::vector<StatsViewWrite>& views,
                             uint32_t seq) {
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  w.StartObject();
  w.Key("t");
  w.String("stats_write");
  w.Key("seq");
  w.Uint(seq);
  w.Key("views");
  w.StartArray();
  for (size_t i = 0; i < views.size() && i < kStatsMaxWriteViews; ++i) {
    const auto& view = views[i];
    w.StartObject();
    w.Key("id");
    w.Uint(view.view_id);
    if (!IsSkillViewId(view.view_id)) {
      w.Key("cols");
      w.StartArray();
      std::set<uint16_t> seen;
      for (const auto& c : view.columns) {
        if (seen.size() >= kStatsMaxColumns || !seen.insert(c.attr).second ||
            !IsStatsType(c.value.type) || c.attr == kStatsAttrRank ||
            c.attr == kStatsAttrGamertag ||
            (c.attr == kStatsAttrRating && c.value.type != 2)) {
          continue;
        }
        w.StartArray();
        w.Uint(c.attr);
        w.Uint(c.value.type);
        w.String(StatsOpName(c.op));
        WriteStatsValue(w, c.value);
        w.EndArray();
      }
      w.EndArray();
    }
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return buffer.GetString();
}

std::string EncodeStatsRead(const std::vector<uint64_t>& xuids,
                            const std::vector<StatsSpec>& specs,
                            uint32_t seq) {
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  w.StartObject();
  w.Key("t");
  w.String("stats_read");
  w.Key("seq");
  w.Uint(seq);
  w.Key("xuids");
  w.StartArray();
  for (size_t i = 0; i < xuids.size() && i < kStatsMaxReadXuids; ++i) {
    w.String(fmt::format("{:016x}", xuids[i]).c_str());
  }
  w.EndArray();
  WriteSpecs(w, specs);
  w.EndObject();
  return buffer.GetString();
}

std::string EncodeStatsEnum(StatsEnumBy by, uint64_t pivot, uint32_t rows,
                            const std::vector<StatsSpec>& specs,
                            uint32_t seq) {
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  w.StartObject();
  w.Key("t");
  w.String("stats_enum");
  w.Key("seq");
  w.Uint(seq);
  w.Key("by");
  w.String(by == StatsEnumBy::kRank    ? "rank"
           : by == StatsEnumBy::kXuid ? "xuid"
                                      : "rating");
  w.Key("pivot");
  if (by == StatsEnumBy::kRank) {
    w.String(fmt::format("{}", std::clamp<uint64_t>(pivot, 1, 0xFFFFFFFF))
                 .c_str());
  } else if (by == StatsEnumBy::kXuid) {
    w.String(fmt::format("{:016x}", pivot).c_str());
  } else {
    w.String(fmt::format("{}", static_cast<int64_t>(pivot)).c_str());
  }
  w.Key("rows");
  w.Uint(std::clamp<uint32_t>(rows, 1, kStatsMaxEnumRows));
  WriteSpecs(w, specs);
  w.EndObject();
  return buffer.GetString();
}

std::optional<DirectoryReply> ParseDirectoryReply(std::string_view text) {
  // Results are tens of KB at most; the iterative parser keeps deep nesting
  // off the stack.
  if (text.size() > (1u << 20)) {
    return std::nullopt;
  }
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseIterativeFlag>(text.data(), text.size());
  if (doc.HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }
  auto t = doc.FindMember("t");
  if (t == doc.MemberEnd() || !t->value.IsString()) {
    return std::nullopt;
  }
  const std::string_view type = t->value.GetString();
  DirectoryReply reply;
  ReadUint(doc, "seq", reply.seq);
  if (type == "authed") {
    reply.kind = DirectoryReply::Kind::kAuthed;
    if (auto xuid = doc.FindMember("xuid");
        xuid != doc.MemberEnd() && xuid->value.IsString()) {
      reply.xuid = StrictXuid(xuid->value.GetString());
    }
    if (auto name = doc.FindMember("gamertag");
        name != doc.MemberEnd() && name->value.IsString()) {
      reply.gamertag = CleanGamertag(name->value.GetString());
    }
    ReadUint(doc, "stats", reply.stats);
  } else if (type == "stats") {
    reply.kind = DirectoryReply::Kind::kStats;
    if (auto views = doc.FindMember("views"); views != doc.MemberEnd()) {
      reply.stats_views = ReadStatsViews(views->value);
    }
  } else if (type == "stats_written") {
    reply.kind = DirectoryReply::Kind::kStatsWritten;
  } else if (type == "updated") {
    reply.kind = DirectoryReply::Kind::kUpdated;
    auto id = doc.FindMember("id");
    if (id != doc.MemberEnd() && id->value.IsString()) {
      reply.ids.emplace_back(id->value.GetString());
    }
  } else if (type == "results") {
    reply.kind = DirectoryReply::Kind::kResults;
    auto sessions = doc.FindMember("sessions");
    if (sessions != doc.MemberEnd() && sessions->value.IsArray()) {
      for (const auto& s : sessions->value.GetArray()) {
        if (auto advert = ReadAdvert(s)) {
          reply.sessions.push_back(std::move(*advert));
        }
      }
    }
  } else if (type == "advertised") {
    reply.kind = DirectoryReply::Kind::kAdvertised;
    auto id = doc.FindMember("id");
    if (id != doc.MemberEnd() && id->value.IsString()) {
      reply.ids.emplace_back(id->value.GetString());
    }
  } else if (type == "refreshed") {
    reply.kind = DirectoryReply::Kind::kRefreshed;
    auto ids = doc.FindMember("ids");
    if (ids != doc.MemberEnd() && ids->value.IsArray()) {
      for (const auto& id : ids->value.GetArray()) {
        if (id.IsString()) {
          reply.ids.emplace_back(id.GetString());
        }
      }
    }
  } else if (type == "error" || type == "reject") {
    reply.kind = DirectoryReply::Kind::kError;
    reply.rejected = type == "reject";
    auto reason = doc.FindMember("reason");
    if (reason != doc.MemberEnd() && reason->value.IsString()) {
      reply.reason = reason->value.GetString();
    }
    if (auto request = doc.FindMember("for");
        request != doc.MemberEnd() && request->value.IsString()) {
      reply.for_request = request->value.GetString();
    }
    if (auto id = doc.FindMember("id");
        id != doc.MemberEnd() && id->value.IsString()) {
      reply.ids.emplace_back(id->value.GetString());
    }
    auto appeal = doc.FindMember("appeal");
    if (appeal != doc.MemberEnd() && appeal->value.IsString()) {
      reply.appeal = appeal->value.GetString();
    }
  }
  return reply;
}

SessionDirectory::SessionDirectory(std::string lobby, uint32_t title_id,
                                   DirectoryAuth auth)
    : url_(fmt::format("{}/v2/sessions/{:08X}", lobby, title_id)),
      auth_(std::move(auth)) {
  thread_ = std::thread([this] { Run(); });
}

SessionDirectory::~SessionDirectory() {
  std::unique_ptr<secure::LobbySocket> socket;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    socket = std::move(socket_);
  }
  changed_.notify_all();
  thread_.join();
  // Destroying the socket ends its events; closing it takes the adverts away.
  socket.reset();
}

void SessionDirectory::EnsureOpenLocked() {
  if (socket_ || stopping_ || halted_) {
    return;
  }
  auto factory = secure::GetLobbySocketFactory();
  if (!factory) {
    return;
  }
  const uint64_t generation = ++generation_;
  secure::LobbySocket::Events events;
  events.opened = [this, generation] {
    std::lock_guard lock(mutex_);
    if (generation == generation_) {
      OnOpened();
    }
  };
  events.message = [this, generation](std::string text) {
    std::lock_guard lock(mutex_);
    if (generation == generation_) {
      OnMessage(std::move(text));
    }
  };
  events.closed = [this, generation](std::string reason) {
    std::lock_guard lock(mutex_);
    if (generation == generation_) {
      OnClosed(std::move(reason));
    }
  };
  socket_ = factory(url_, std::move(events));
}

// The handlers run with |mutex_| held.
void SessionDirectory::OnOpened() {
  open_ = true;
  authed_ = false;
  auth_sent_ = false;
  opened_at_ = std::chrono::steady_clock::now();
  // The adverts go out once the lobby answers the sign-in.
  TryAuthLocked();
  changed_.notify_all();
}

void SessionDirectory::TryAuthLocked() {
  using namespace std::chrono;
  if (!open_ || !socket_ || authed_ || auth_sent_ || halted_) {
    return;
  }
  const uint64_t xuid = auth_.xuid ? auth_.xuid() : 0;
  if (!xuid) {
    return;  // No account yet; TokenReady() comes when it signs in.
  }
  auto token = auth_.token ? auth_.token() : std::nullopt;
  if (token && *token != denied_token_) {
    sent_token_ = *token;
    auth_sent_ = true;
    socket_->Send(EncodeDirectoryAuth(xuid, *token));
    return;
  }
  // Ask once, and again after 3 minutes (the lobby gives 20 an hour).
  const auto now = steady_clock::now();
  if (auth_.request_token &&
      (!token_requested_ || now - *token_requested_ >= minutes(3))) {
    token_requested_ = now;
    auth_.request_token();
  }
}

void SessionDirectory::TokenReady() {
  std::lock_guard lock(mutex_);
  TryAuthLocked();
}

void SessionDirectory::OnMessage(std::string text) {
  auto reply = ParseDirectoryReply(text);
  if (!reply) {
    return;
  }
  switch (reply->kind) {
    case DirectoryReply::Kind::kAuthed:
      authed_ = true;
      denials_ = 0;
      denied_token_.clear();
      stats_version_ = reply->stats;
      XELOGI("Live directory: signed in as {}{}", reply->gamertag,
             stats_version_ ? ", stats on" : "");
      for (const auto& [id, advert] : adverts_) {
        socket_->Send(EncodeAdvertise(advert, kTtl));
      }
      if (!stats_version_ && !stats_writes_.empty()) {
        XELOGW("Live directory: this lobby keeps no stats; dropping {} "
               "flush(es)",
               stats_writes_.size());
        stats_writes_.clear();
      }
      SendStatsWritesLocked();
      changed_.notify_all();
      break;
    case DirectoryReply::Kind::kResults:
      // Late answers, after Ask gave up, are dropped.
      if (waiting_.contains(reply->seq)) {
        answers_[reply->seq] = std::move(reply->sessions);
        changed_.notify_all();
      }
      break;
    case DirectoryReply::Kind::kStats:
      if (waiting_.contains(reply->seq)) {
        stats_answers_[reply->seq] = std::move(reply->stats_views);
        changed_.notify_all();
      }
      break;
    case DirectoryReply::Kind::kStatsWritten:
      std::erase_if(stats_writes_, [&](const PendingWrite& w) {
        return w.seq && w.seq == reply->seq;
      });
      XELOGI("Live directory: stats written");
      changed_.notify_all();
      break;
    case DirectoryReply::Kind::kRefreshed:
      // The lobby drops adverts that expired, or past its limits: advertise
      // those again.
      for (const auto& [id, advert] : adverts_) {
        if (std::find(reply->ids.begin(), reply->ids.end(), id) ==
            reply->ids.end()) {
          XELOGI("Live directory: the lobby dropped session {}, advertising "
                 "it again",
                 id);
          socket_->Send(EncodeAdvertise(advert, kTtl));
        }
      }
      break;
    case DirectoryReply::Kind::kError:
      if (reply->rejected && reply->reason == "banned") {
        halted_ = true;
        XELOGE("Live directory: this account is banned from the lobby, so "
               "sessions are off{}",
               reply->appeal.empty()
                   ? std::string()
                   : fmt::format(" (appeal: {})", reply->appeal));
        changed_.notify_all();
      } else if (reply->rejected && reply->reason == "denied") {
        // A wrong or old token: one new token, then give up.
        auth_sent_ = false;
        if (++denials_ > 1) {
          halted_ = true;
          XELOGE("Live directory: the lobby denied the new session token "
                 "too, so sessions are off");
          changed_.notify_all();
        } else {
          XELOGW("Live directory: the session token was denied, asking for "
                 "a new one");
          denied_token_ = sent_token_;
          token_requested_ = std::chrono::steady_clock::now();
          if (auth_.request_token) {
            auth_.request_token();
          }
        }
      } else if (reply->for_request == "stats_write") {
        // Not kept: a refused flush won't be taken later either.
        XELOGW("Live directory: the lobby refused stats ({})", reply->reason);
        std::erase_if(stats_writes_, [&](const PendingWrite& w) {
          return w.seq && w.seq == reply->seq;
        });
        changed_.notify_all();
      } else if (reply->for_request == "stats_read" ||
                 reply->for_request == "stats_enum") {
        XELOGW("Live directory: the lobby refused a stats read ({})",
               reply->reason);
        if (waiting_.contains(reply->seq)) {
          stats_answers_[reply->seq] = std::nullopt;
          changed_.notify_all();
        }
      } else if (reply->for_request == "update" &&
                 reply->reason == "not_found" && !reply->ids.empty() &&
                 adverts_.contains(reply->ids[0])) {
        // The lobby dropped the advert: advertise it whole again.
        XELOGI("Live directory: the lobby dropped session {}, advertising "
               "it again",
               reply->ids[0]);
        socket_->Send(EncodeAdvertise(adverts_[reply->ids[0]], kTtl));
      } else {
        XELOGW("Live directory: refused ({})", reply->reason);
      }
      break;
    default:
      break;
  }
}

void SessionDirectory::OnClosed(std::string reason) {
  XELOGI("Live directory: closed ({})", reason);
  // A socket that lasted a minute was fine; one that didn't (no lobby, or it
  // refused what was sent) makes the next wait longer.
  if (open_ && std::chrono::steady_clock::now() - opened_at_ >=
                   std::chrono::minutes(1)) {
    failures_ = 0;
  } else {
    ++failures_;
  }
  open_ = false;
  authed_ = false;
  auth_sent_ = false;
  closed_ = true;
  // Flushes the lobby didn't answer go again on the next socket.
  for (auto& w : stats_writes_) {
    w.seq = 0;
  }
  ++generation_;  // Ignores anything more from this socket.
  changed_.notify_all();
}

void SessionDirectory::Run() {
  using namespace std::chrono;
  auto next_refresh = steady_clock::now() + seconds(kTtl / 2);
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    changed_.wait_for(lock, seconds(20), [this] { return stopping_ || closed_; });
    if (stopping_) {
      break;
    }
    if (closed_) {
      // A socket is destroyed here, not inside its own event, and without the
      // lock its events take.
      closed_ = false;
      auto socket = std::move(socket_);
      // Ask waits while a socket is there.
      changed_.notify_all();
      const auto delay = ReconnectDelayLocked();
      lock.unlock();
      socket.reset();
      lock.lock();
      changed_.wait_for(lock, delay, [this] { return stopping_; });
      if (stopping_) {
        break;
      }
      if (!adverts_.empty() || !stats_writes_.empty()) {
        // Advertises them again, and sends the flushes, once signed in.
        EnsureOpenLocked();
      }
      continue;
    }
    if (socket_ && open_) {
      if (!socket_->Ping()) {
        socket_->Send("ping");
      }
      TryAuthLocked();  // Asks again when no token came.
      if (authed_ && steady_clock::now() >= next_refresh &&
          !adverts_.empty()) {
        socket_->Send(EncodeRefresh(kTtl));
        next_refresh = steady_clock::now() + seconds(kTtl / 2);
      }
    }
  }
}

std::chrono::milliseconds SessionDirectory::ReconnectDelayLocked() const {
  using namespace std::chrono;
  // 2 s, then doubling after each socket that didn't last, up to 5 minutes,
  // and up to a quarter more at random so hosts don't come back together.
  const milliseconds delay = std::min<milliseconds>(
      minutes(5), seconds(2) * (1u << std::min(failures_ ? failures_ - 1 : 0,
                                                 8u)));
  thread_local std::minstd_rand rng{std::random_device{}()};
  return delay + milliseconds(rng() % (delay.count() / 4 + 1));
}

void SessionDirectory::Advertise(const DirectoryAdvert& advert) {
  std::lock_guard lock(mutex_);
  adverts_[advert.id] = advert;
  EnsureOpenLocked();
  if (socket_ && authed_) {
    socket_->Send(EncodeAdvertise(advert, kTtl));
  }
}

bool SessionDirectory::Update(const std::string& id, uint32_t used_public,
                              uint32_t used_private) {
  std::lock_guard lock(mutex_);
  auto it = adverts_.find(id);
  if (it == adverts_.end()) {
    return false;
  }
  it->second.used_public = used_public;
  it->second.used_private = used_private;
  if (socket_ && authed_) {
    socket_->Send(EncodeUpdate(id, used_public, used_private, kTtl));
  }
  return true;
}

void SessionDirectory::Withdraw(const std::string& id) {
  std::lock_guard lock(mutex_);
  if (!adverts_.erase(id)) {
    return;
  }
  if (socket_ && authed_) {
    socket_->Send(EncodeWithdraw(id));
  }
}

std::vector<DirectoryAdvert> SessionDirectory::Ask(
    const std::function<std::string(uint32_t seq)>& encode,
    uint32_t timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock lock(mutex_);
  EnsureOpenLocked();
  // Waits for the sign-in too: nothing is asked before it.
  if (!changed_.wait_until(lock, deadline,
                           [this] {
                             return authed_ || stopping_ || halted_ || !socket_;
                           }) ||
      !authed_) {
    return {};
  }
  const uint32_t seq = next_seq_++;
  waiting_.insert(seq);
  socket_->Send(encode(seq));
  changed_.wait_until(lock, deadline, [&] {
    return answers_.contains(seq) || stopping_ || !authed_;
  });
  waiting_.erase(seq);
  auto it = answers_.find(seq);
  if (it == answers_.end()) {
    return {};
  }
  auto sessions = std::move(it->second);
  answers_.erase(it);
  return sessions;
}

std::optional<std::vector<StatsViewData>> SessionDirectory::AskStats(
    const std::function<std::string(uint32_t seq)>& encode,
    uint32_t timeout_ms) {
  // No account: nothing to wait for.
  if (!auth_.xuid || !auth_.xuid()) {
    return std::nullopt;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock lock(mutex_);
  EnsureOpenLocked();
  if (!changed_.wait_until(lock, deadline,
                           [this] {
                             return authed_ || stopping_ || halted_ || !socket_;
                           }) ||
      !authed_ || !stats_version_) {
    return std::nullopt;
  }
  const uint32_t seq = next_seq_++;
  waiting_.insert(seq);
  socket_->Send(encode(seq));
  changed_.wait_until(lock, deadline, [&] {
    return stats_answers_.contains(seq) || stopping_ || !authed_;
  });
  waiting_.erase(seq);
  auto it = stats_answers_.find(seq);
  if (it == stats_answers_.end()) {
    return std::nullopt;
  }
  auto views = std::move(it->second);
  stats_answers_.erase(it);
  return views;
}

void SessionDirectory::SendStatsWritesLocked() {
  if (!socket_ || !authed_ || !stats_version_) {
    return;
  }
  for (auto& w : stats_writes_) {
    if (!w.seq) {
      w.seq = next_seq_++;
      socket_->Send(EncodeStatsWrite(w.views, w.seq));
    }
  }
}

void SessionDirectory::WriteStats(std::vector<StatsViewWrite> views) {
  if (views.empty()) {
    return;
  }
  if (views.size() > kStatsMaxWriteViews) {
    views.resize(kStatsMaxWriteViews);
  }
  std::lock_guard lock(mutex_);
  stats_cache_.clear();
  // A title that flushes and flushes again while the lobby can't be reached
  // keeps its 8 newest.
  constexpr size_t kMaxPending = 8;
  if (stats_writes_.size() >= kMaxPending) {
    stats_writes_.erase(stats_writes_.begin());
  }
  stats_writes_.push_back({std::move(views), 0});
  EnsureOpenLocked();
  SendStatsWritesLocked();
}

size_t SessionDirectory::PendingStatsWrites() {
  std::lock_guard lock(mutex_);
  return stats_writes_.size();
}

std::optional<std::vector<StatsViewData>> SessionDirectory::ReadStats(
    const std::vector<uint64_t>& xuids, const std::vector<StatsSpec>& specs,
    uint32_t timeout_ms) {
  const std::string key = EncodeStatsRead(xuids, specs, 0);
  constexpr auto kCacheFor = std::chrono::seconds(30);
  {
    std::lock_guard lock(mutex_);
    auto it = stats_cache_.find(key);
    if (it != stats_cache_.end() &&
        std::chrono::steady_clock::now() - it->second.at < kCacheFor) {
      return it->second.views;
    }
  }
  auto views = AskStats(
      [&](uint32_t seq) { return EncodeStatsRead(xuids, specs, seq); },
      timeout_ms);
  if (views) {
    std::lock_guard lock(mutex_);
    if (stats_cache_.size() >= 64) {
      stats_cache_.clear();
    }
    stats_cache_[key] = {std::chrono::steady_clock::now(), *views};
  }
  return views;
}

std::optional<std::vector<StatsViewData>> SessionDirectory::EnumerateStats(
    StatsEnumBy by, uint64_t pivot, uint32_t rows,
    const std::vector<StatsSpec>& specs, uint32_t timeout_ms) {
  const std::string key = EncodeStatsEnum(by, pivot, rows, specs, 0);
  constexpr auto kCacheFor = std::chrono::seconds(30);
  {
    std::lock_guard lock(mutex_);
    auto it = stats_cache_.find(key);
    if (it != stats_cache_.end() &&
        std::chrono::steady_clock::now() - it->second.at < kCacheFor) {
      return it->second.views;
    }
  }
  auto views = AskStats(
      [&](uint32_t seq) {
        return EncodeStatsEnum(by, pivot, rows, specs, seq);
      },
      timeout_ms);
  if (views) {
    std::lock_guard lock(mutex_);
    if (stats_cache_.size() >= 64) {
      stats_cache_.clear();
    }
    stats_cache_[key] = {std::chrono::steady_clock::now(), *views};
  }
  return views;
}

std::vector<DirectoryAdvert> SessionDirectory::Search(
    const std::vector<std::pair<uint32_t, uint32_t>>& contexts, uint32_t max,
    bool in_progress, uint32_t timeout_ms) {
  return Ask(
      [&](uint32_t seq) {
        return EncodeSearch(contexts, max, in_progress, seq);
      },
      timeout_ms);
}

std::vector<DirectoryAdvert> SessionDirectory::Lookup(
    const std::vector<std::string>& ids, uint32_t timeout_ms) {
  return Ask([&](uint32_t seq) { return EncodeLookup(ids, seq); },
             timeout_ms);
}

}  // namespace xe::net::live
