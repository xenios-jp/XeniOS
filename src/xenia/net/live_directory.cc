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
#include <cstdlib>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "xenia/base/logging.h"

namespace xe::net::live {

namespace {

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
    a.xuid = std::strtoull(xuid->value.GetString(), nullptr, 16);
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

std::string EncodeAdvertise(const DirectoryAdvert& a, uint32_t ttl) {
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

std::optional<DirectoryReply> ParseDirectoryReply(std::string_view text) {
  rapidjson::Document doc;
  doc.Parse(text.data(), text.size());
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
  if (type == "results") {
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
  } else if (type == "error") {
    reply.kind = DirectoryReply::Kind::kError;
    auto reason = doc.FindMember("reason");
    if (reason != doc.MemberEnd() && reason->value.IsString()) {
      reply.reason = reason->value.GetString();
    }
  }
  return reply;
}

SessionDirectory::SessionDirectory(std::string lobby, uint32_t title_id)
    : url_(fmt::format("{}/v2/sessions/{:08X}", lobby, title_id)) {
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
  if (socket_ || stopping_) {
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
  for (const auto& [id, advert] : adverts_) {
    socket_->Send(EncodeAdvertise(advert, kTtl));
  }
  changed_.notify_all();
}

void SessionDirectory::OnMessage(std::string text) {
  auto reply = ParseDirectoryReply(text);
  if (!reply) {
    return;
  }
  switch (reply->kind) {
    case DirectoryReply::Kind::kResults:
      answers_[reply->seq] = std::move(reply->sessions);
      changed_.notify_all();
      break;
    case DirectoryReply::Kind::kError:
      XELOGW("Live directory: {}", text);
      break;
    default:
      break;
  }
}

void SessionDirectory::OnClosed(std::string reason) {
  XELOGI("Live directory: closed ({})", reason);
  open_ = false;
  closed_ = true;
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
      lock.unlock();
      socket.reset();
      std::this_thread::sleep_for(seconds(2));
      lock.lock();
      if (!adverts_.empty()) {
        EnsureOpenLocked();  // Advertises them again once open.
      }
      continue;
    }
    if (socket_ && open_) {
      if (!socket_->Ping()) {
        socket_->Send("ping");
      }
      if (steady_clock::now() >= next_refresh && !adverts_.empty()) {
        socket_->Send(EncodeRefresh(kTtl));
        next_refresh = steady_clock::now() + seconds(kTtl / 2);
      }
    }
  }
}

void SessionDirectory::Advertise(const DirectoryAdvert& advert) {
  std::lock_guard lock(mutex_);
  adverts_[advert.id] = advert;
  EnsureOpenLocked();
  if (socket_ && open_) {
    socket_->Send(EncodeAdvertise(advert, kTtl));
  }
}

void SessionDirectory::Withdraw(const std::string& id) {
  std::lock_guard lock(mutex_);
  if (!adverts_.erase(id)) {
    return;
  }
  if (socket_ && open_) {
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
  if (!changed_.wait_until(lock, deadline,
                           [this] { return open_ || stopping_ || !socket_; }) ||
      !open_) {
    return {};
  }
  const uint32_t seq = next_seq_++;
  socket_->Send(encode(seq));
  if (!changed_.wait_until(lock, deadline, [&] {
        return answers_.contains(seq) || stopping_ || !open_;
      })) {
    return {};
  }
  auto it = answers_.find(seq);
  if (it == answers_.end()) {
    return {};
  }
  auto sessions = std::move(it->second);
  answers_.erase(it);
  return sessions;
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
