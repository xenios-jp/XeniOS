/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/live_account.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/mbedtls/include/mbedtls/bignum.h"
#include "third_party/mbedtls/include/mbedtls/ecdsa.h"
#include "third_party/mbedtls/include/mbedtls/ecp.h"
#include "third_party/mbedtls/include/mbedtls/platform_util.h"
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "third_party/utfcpp/source/utf8.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/net/network_path.h"
#include "xenia/net/xnet_secure.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace xe::net::live {

namespace {

std::string ToHex(const uint8_t* data, size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    text[i * 2] = kDigits[data[i] >> 4];
    text[i * 2 + 1] = kDigits[data[i] & 0xF];
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
    const int high = digit(text[i * 2]), low = digit(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[i] = uint8_t(high << 4 | low);
  }
  return bytes;
}

int RandomBytes(void*, unsigned char* out, size_t size) {
  Random(std::span(out, size));
  return 0;
}

// The P-256 group, the private key and the public point, freed together.
struct Ecp {
  Ecp() {
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    loaded = !mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
  }
  ~Ecp() {
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&group);
  }
  mbedtls_ecp_group group;
  mbedtls_mpi d;
  mbedtls_ecp_point q;
  bool loaded = false;
};

std::array<uint8_t, 32> Sha256(std::string_view text) {
  std::array<uint8_t, 32> hash;
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(text.data()),
                 text.size(), hash.data(), 0);
  return hash;
}

// Exactly 16 hex digits: strtoull's sign, spaces and "0x" are not.
std::optional<uint64_t> ParseXuid(const rapidjson::Value& v) {
  if (!v.IsString() || v.GetStringLength() != 16) {
    return std::nullopt;
  }
  const std::string_view text(v.GetString(), 16);
  if (text.find_first_not_of("0123456789abcdefABCDEF") !=
      std::string_view::npos) {
    return std::nullopt;
  }
  return std::strtoull(v.GetString(), nullptr, 16);
}

std::string String(const rapidjson::Value& o, const char* name) {
  auto it = o.FindMember(name);
  return it != o.MemberEnd() && it->value.IsString() ? it->value.GetString()
                                                     : std::string();
}

uint32_t Title(const rapidjson::Value& o) {
  const std::string text = String(o, "title");
  return text.size() == 8 && text.find_first_not_of("0123456789abcdefABCDEF") ==
                                 std::string::npos
             ? uint32_t(std::strtoul(text.c_str(), nullptr, 16))
             : 0;
}

// A session's XSESSION_INFO, as hex: anything longer is dropped here rather
// than held (a lobby could send megabytes). Users check its size.
std::vector<uint8_t> SessionInfo(const std::string& hex) {
  if (hex.size() > 2 * kSessionInfoSize) {
    return {};
  }
  auto info = FromHex(hex);
  return info ? std::move(*info) : std::vector<uint8_t>();
}

uint64_t Xuid(const rapidjson::Value& o, const char* name) {
  auto it = o.FindMember(name);
  return it != o.MemberEnd() ? ParseXuid(it->value).value_or(0) : 0;
}

std::vector<uint64_t> ParseXuids(const rapidjson::Value& o, const char* name) {
  std::vector<uint64_t> xuids;
  auto it = o.FindMember(name);
  if (it != o.MemberEnd() && it->value.IsArray()) {
    for (const auto& v : it->value.GetArray()) {
      if (auto xuid = ParseXuid(v)) {
        xuids.push_back(*xuid);
      }
    }
  }
  return xuids;
}

uint64_t Uint64(const rapidjson::Value& o, const char* name) {
  auto it = o.FindMember(name);
  return it != o.MemberEnd() && it->value.IsUint64() ? it->value.GetUint64()
                                                     : 0;
}

std::vector<std::string> Strings(const rapidjson::Value& o, const char* name) {
  std::vector<std::string> strings;
  auto it = o.FindMember(name);
  if (it != o.MemberEnd() && it->value.IsArray()) {
    for (const auto& v : it->value.GetArray()) {
      if (v.IsString()) {
        strings.emplace_back(v.GetString());
      }
    }
  }
  return strings;
}

NameCheck ParseNameCheck(const rapidjson::Value& o, std::string_view asked) {
  NameCheck check;
  check.gamertag = o.IsObject() && o.HasMember("gamertag")
                       ? CleanGamertag(String(o, "gamertag"))
                       : std::string(asked);
  check.status = String(o, "status");
  check.suggestions = Strings(o, "suggestions");
  return check;
}

std::vector<Device> ParseDevices(const rapidjson::Value& o) {
  std::vector<Device> devices;
  auto it = o.FindMember("list");
  if (it != o.MemberEnd() && it->value.IsArray()) {
    for (const auto& v : it->value.GetArray()) {
      if (!v.IsObject()) {
        continue;
      }
      Device d;
      d.key = String(v, "key");
      d.name = CleanText(String(v, "name"), 40);
      d.platform = String(v, "platform");
      d.added_ms = Uint64(v, "added");
      d.seen_ms = Uint64(v, "seen");
      auto current = v.FindMember("current");
      d.current = current != v.MemberEnd() && current->value.IsBool() &&
                  current->value.GetBool();
      if (!d.key.empty()) {
        devices.push_back(std::move(d));
      }
    }
  }
  return devices;
}

std::optional<Friend> ParseFriend(const rapidjson::Value& o, bool& removed) {
  if (!o.IsObject()) {
    return std::nullopt;
  }
  Friend f;
  f.xuid = Xuid(o, "xuid");
  f.gamertag = CleanGamertag(String(o, "gamertag"));
  const std::string state = String(o, "state");
  removed = state == "none";
  if (state == "friend") {
    f.state = Friend::State::kFriend;
  } else if (state == "incoming") {
    f.state = Friend::State::kIncoming;
  } else if (state == "outgoing") {
    f.state = Friend::State::kOutgoing;
  } else if (!removed) {
    return std::nullopt;
  }
  if (!f.xuid) {
    return std::nullopt;
  }
  return f;
}

std::optional<Joinable> ParseJoinable(const rapidjson::Value& o) {
  auto it = o.FindMember("joinable");
  if (it == o.MemberEnd() || !it->value.IsObject()) {
    return std::nullopt;
  }
  Joinable j;
  j.room = String(it->value, "room");
  j.session_id = String(it->value, "id");
  if (j.room.size() > kMaxRemoteId || j.session_id.size() > kMaxRemoteId) {
    return std::nullopt;
  }
  j.info = SessionInfo(String(it->value, "info"));
  return j;
}

// The `users` of a `names` or `found`: [{"xuid", "gamertag"}], without
// entries missing either.
std::vector<std::pair<uint64_t, std::string>> ParseUsers(
    const rapidjson::Value& o) {
  std::vector<std::pair<uint64_t, std::string>> users;
  auto it = o.FindMember("users");
  if (it != o.MemberEnd() && it->value.IsArray()) {
    for (const auto& v : it->value.GetArray()) {
      if (!v.IsObject()) {
        continue;
      }
      const uint64_t xuid = Xuid(v, "xuid");
      std::string gamertag = CleanGamertag(String(v, "gamertag"));
      if (xuid && !gamertag.empty()) {
        users.emplace_back(xuid, std::move(gamertag));
      }
    }
  }
  return users;
}

// `requests` in a settings object: nothing for a value this client doesn't
// know.
std::optional<FriendRequests> ParseRequests(const rapidjson::Value& o) {
  const std::string requests = String(o, "requests");
  if (requests == "anyone") {
    return FriendRequests::kAnyone;
  }
  if (requests == "code") {
    return FriendRequests::kCode;
  }
  return std::nullopt;
}

std::optional<PartyState> ParseParty(const rapidjson::Value& o) {
  auto left = o.FindMember("left");
  if (left != o.MemberEnd() && left->value.IsBool() && left->value.GetBool()) {
    return std::nullopt;
  }
  PartyState p;
  p.room = String(o, "room");
  p.leader = Xuid(o, "leader");
  auto members = o.FindMember("members");
  if (members != o.MemberEnd() && members->value.IsArray()) {
    for (const auto& m : members->value.GetArray()) {
      if (!m.IsObject()) {
        continue;
      }
      p.members.emplace_back(Xuid(m, "xuid"),
                             CleanGamertag(String(m, "gamertag")));
      // Presence, when the lobby says it.
      auto online = m.FindMember("online");
      const uint32_t title_id = Title(m);
      auto joinable = ParseJoinable(m);
      if (title_id || joinable ||
          (online != m.MemberEnd() && online->value.IsBool())) {
        Presence presence;
        presence.xuid = p.members.back().first;
        presence.gamertag = p.members.back().second;
        presence.online = online == m.MemberEnd() || !online->value.IsBool() ||
                          online->value.GetBool();
        presence.title_id = title_id;
        presence.joinable = std::move(joinable);
        p.presence.push_back(std::move(presence));
      }
    }
  }
  auto event = o.FindMember("event");
  if (event != o.MemberEnd() && event->value.IsObject()) {
    p.event = String(event->value, "kind");
    p.event_xuid = Xuid(event->value, "xuid");
  }
  return p;
}

template <typename Write>
std::string Json(Write&& write) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  write(w);
  w.EndObject();
  return buffer.GetString();
}

using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

void Field(JsonWriter& w, const char* name, std::string_view value) {
  w.Key(name);
  w.String(value.data(), rapidjson::SizeType(value.size()));
}

void WriteJoinable(JsonWriter& w, const Joinable& j) {
  Field(w, "room", j.room);
  if (!j.session_id.empty()) {
    Field(w, "id", j.session_id);
  }
  if (!j.info.empty()) {
    Field(w, "info", ToHex(j.info.data(), j.info.size()));
  }
}

}  // namespace

std::string XuidHex(uint64_t xuid) { return fmt::format("{:016x}", xuid); }

DeviceKey DeviceKey::Generate() {
  for (;;) {
    std::array<uint8_t, 32> d;
    Random(d);
    if (auto key = FromSecret(d)) {
      mbedtls_platform_zeroize(d.data(), d.size());
      return *key;
    }
  }
}

std::optional<DeviceKey> DeviceKey::FromSecret(
    const std::array<uint8_t, 32>& d) {
  Ecp ecp;
  if (mbedtls_mpi_read_binary(&ecp.d, d.data(), d.size()) ||
      mbedtls_ecp_check_privkey(&ecp.group, &ecp.d) ||
      mbedtls_ecp_mul(&ecp.group, &ecp.q, &ecp.d, &ecp.group.G, RandomBytes,
                      nullptr)) {
    return std::nullopt;
  }
  DeviceKey key;
  key.secret_ = d;
  size_t size = 0;
  if (mbedtls_ecp_point_write_binary(&ecp.group, &ecp.q,
                                     MBEDTLS_ECP_PF_UNCOMPRESSED, &size,
                                     key.public_.data(), key.public_.size()) ||
      size != key.public_.size()) {
    return std::nullopt;
  }
  return key;
}

namespace {

// Writes |text| to a new file at |path|, readable by the user only, whole or
// not at all: through a temporary file, synced, then linked in place. False
// when another instance made the file meanwhile, or the write failed.
bool WriteNewFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::path temp = path;
#if defined(_WIN32)
  temp += ".tmp";
  {
    std::ofstream file(temp, std::ios::trunc);
    if (!(file << text) || !file.flush()) {
      return false;
    }
  }
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::filesystem::remove(temp, ec);
    return false;
  }
  std::filesystem::rename(temp, path, ec);
  return !ec;
#else
  temp += fmt::format(".tmp{}", getpid());
  const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  const bool written =
      write(fd, text.data(), text.size()) == ssize_t(text.size()) &&
      fsync(fd) == 0;
  close(fd);
  // link, unlike rename, never replaces a key another instance just wrote.
  const bool linked = written && link(temp.c_str(), path.c_str()) == 0;
  unlink(temp.c_str());
  return linked;
#endif
}

}  // namespace

std::optional<DeviceKey> DeviceKey::LoadOrCreate(
    const std::filesystem::path& path) {
  // Twice: when another instance makes the key first, this one reads it.
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::string text;
    bool exists = false;
    {
      std::ifstream file(path);
      exists = bool(file);
      if (exists) {
        std::getline(file, text);
      }
    }
    if (!text.empty()) {
      if (auto bytes = FromHex(text); bytes && bytes->size() == 32) {
        std::array<uint8_t, 32> d;
        std::copy(bytes->begin(), bytes->end(), d.begin());
        if (auto key = FromSecret(d)) {
          return key;
        }
      }
      // Kept as it is: it may be the only copy of an account's key.
      XELOGE("Live account: the device key in {} isn't valid",
             path.filename().string());
      return std::nullopt;
    }
    std::error_code ec;
    if (exists) {
      // Empty, from a write that never finished: it holds no key.
      std::filesystem::remove(path, ec);
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    DeviceKey key = Generate();
    if (WriteNewFile(path, ToHex(key.secret_.data(), key.secret_.size()) +
                               "\n")) {
      return key;
    }
  }
  XELOGE("Live account: can't write the device key {}",
         path.filename().string());
  return std::nullopt;
}

std::string DeviceKey::PublicHex() const {
  return ToHex(public_.data(), public_.size());
}

std::string DeviceKey::Sign(std::string_view text) const {
  Ecp ecp;
  mbedtls_mpi r, s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  const auto hash = Sha256(text);
  std::array<uint8_t, 64> signature{};
  const bool signed_ok =
      ecp.loaded &&
      !mbedtls_mpi_read_binary(&ecp.d, secret_.data(), secret_.size()) &&
      !mbedtls_ecdsa_sign(&ecp.group, &r, &s, &ecp.d, hash.data(),
                          hash.size(), RandomBytes, nullptr) &&
      !mbedtls_mpi_write_binary(&r, signature.data(), 32) &&
      !mbedtls_mpi_write_binary(&s, signature.data() + 32, 32);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  if (!signed_ok) {
    XELOGE("Live account: signing failed");
    return {};
  }
  return ToHex(signature.data(), signature.size());
}

bool VerifySignature(std::string_view public_hex, std::string_view text,
                     std::string_view signature_hex) {
  const auto point = FromHex(public_hex);
  const auto signature = FromHex(signature_hex);
  if (!point || point->size() != 65 || !signature || signature->size() != 64) {
    return false;
  }
  Ecp ecp;
  mbedtls_mpi r, s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  const auto hash = Sha256(text);
  const bool ok =
      !mbedtls_ecp_point_read_binary(&ecp.group, &ecp.q, point->data(),
                                     point->size()) &&
      !mbedtls_mpi_read_binary(&r, signature->data(), 32) &&
      !mbedtls_mpi_read_binary(&s, signature->data() + 32, 32) &&
      !mbedtls_ecdsa_verify(&ecp.group, hash.data(), hash.size(), &ecp.q, &r,
                            &s);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  return ok;
}

std::optional<AccountMessage> ParseAccountMessage(std::string_view text) {
  // The lobby's messages are a few KB; the iterative parser keeps deep
  // nesting off the stack.
  if (text.size() > kMaxLobbyMessage) {
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
  AccountMessage m;
  using Kind = AccountMessage::Kind;
  if (type == "challenge") {
    m.kind = Kind::kChallenge;
    m.nonce = String(doc, "nonce");
  } else if (type == "welcome" || type == "profile") {
    m.kind = type == "welcome" ? Kind::kWelcome : Kind::kProfile;
    m.xuid = Xuid(doc, "xuid");
    // An online XUID, or none: the profile keeps what the lobby says here.
    if (m.xuid && !IsOnlineXuid(m.xuid)) {
      return std::nullopt;
    }
    m.gamertag = CleanGamertag(String(doc, "gamertag"));
    m.tag = String(doc, "tag");
    m.code = String(doc, "code");
    auto assigned = doc.FindMember("assigned");
    m.assigned = assigned != doc.MemberEnd() && assigned->value.IsBool() &&
                 assigned->value.GetBool();
    auto friends = doc.FindMember("friends");
    if (friends != doc.MemberEnd() && friends->value.IsArray()) {
      for (const auto& f : friends->value.GetArray()) {
        bool removed = false;
        if (auto parsed = ParseFriend(f, removed); parsed && !removed) {
          m.friends.push_back(*parsed);
        }
      }
    }
    m.blocked = ParseXuids(doc, "blocked");
    m.rename = String(doc, "rename");
    m.next_rename_ms = Uint64(doc, "nextRename");
    m.recovery = String(doc, "recovery");
    auto refused = doc.FindMember("refused");
    if (refused != doc.MemberEnd() && refused->value.IsObject()) {
      Refused r;
      r.gamertag = CleanGamertag(String(refused->value, "gamertag"));
      r.reason = String(refused->value, "reason");
      r.next_ms = Uint64(refused->value, "next");
      m.refused = std::move(r);
    }
    // A lobby with settings (and names) says them in every welcome.
    auto settings = doc.FindMember("settings");
    if (settings != doc.MemberEnd() && settings->value.IsObject()) {
      m.settings = true;
      m.requests = ParseRequests(settings->value);
    }
  } else if (type == "friend") {
    m.kind = Kind::kFriend;
    auto parsed = ParseFriend(doc, m.removed);
    if (!parsed) {
      return std::nullopt;
    }
    m.friends.push_back(*parsed);
  } else if (type == "presence") {
    m.kind = Kind::kPresence;
    auto& p = m.presence;
    p.xuid = Xuid(doc, "xuid");
    p.gamertag = CleanGamertag(String(doc, "gamertag"));
    auto online = doc.FindMember("online");
    p.online = online != doc.MemberEnd() && online->value.IsBool() &&
               online->value.GetBool();
    p.title_id = Title(doc);
    auto context = doc.FindMember("context");
    if (context != doc.MemberEnd() && context->value.IsUint()) {
      p.context = context->value.GetUint();
    }
    p.rich = CleanText(String(doc, "rich"), kMaxRemoteText);
    p.joinable = ParseJoinable(doc);
    if (!p.xuid) {
      return std::nullopt;
    }
  } else if (type == "invite") {
    m.kind = Kind::kInvite;
    auto& i = m.invite;
    i.id = String(doc, "id");
    i.party = String(doc, "kind") == "party";
    i.from = Xuid(doc, "from");
    i.gamertag = CleanGamertag(String(doc, "gamertag"));
    i.title_id = Title(doc);
    i.room = String(doc, "room");
    i.info = SessionInfo(String(doc, "info"));
    i.message = CleanText(String(doc, "message"), kMaxRemoteText);
    if (i.id.empty() || i.id.size() > kMaxRemoteId ||
        i.room.size() > kMaxRemoteId || !i.from) {
      return std::nullopt;
    }
  } else if (type == "moved") {
    m.kind = Kind::kMoved;
    m.xuid = Xuid(doc, "xuid");
    m.recovery = String(doc, "recovery");
    if (!m.xuid || !IsOnlineXuid(m.xuid)) {
      return std::nullopt;
    }
  } else if (type == "reject") {
    m.kind = Kind::kReject;
    m.reason = String(doc, "reason");
    m.device_name = CleanText(String(doc, "device"), 40);
    auto deleted = doc.FindMember("deleted");
    m.deleted = deleted != doc.MemberEnd() && deleted->value.IsBool() &&
                deleted->value.GetBool();
  } else if (type == "invite_state") {
    m.kind = Kind::kInviteState;
    m.invite_id = String(doc, "id");
    m.state = String(doc, "state");
  } else if (type == "invite_answered") {
    m.kind = Kind::kInviteAnswered;
    m.invite_id = String(doc, "id");
    auto accept = doc.FindMember("accept");
    m.accept = accept != doc.MemberEnd() && accept->value.IsBool() &&
               accept->value.GetBool();
  } else if (type == "party") {
    m.kind = Kind::kParty;
    m.party = ParseParty(doc);
    m.party_room = String(doc, "room");
  } else if (type == "blocked") {
    m.kind = Kind::kBlocked;
    m.blocked = ParseXuids(doc, "xuids");
  } else if (type == "checked") {
    m.kind = Kind::kChecked;
    m.checked = ParseNameCheck(doc, {});
    if (m.checked.status.empty()) {
      return std::nullopt;
    }
  } else if (type == "recovery") {
    m.kind = Kind::kRecovery;
    m.recovery = String(doc, "code");
    if (m.recovery.empty()) {
      return std::nullopt;
    }
  } else if (type == "link_code") {
    m.kind = Kind::kLinkCode;
    m.code = String(doc, "code");
    m.link_ttl_s = uint32_t(Uint64(doc, "ttl"));
    if (m.code.empty()) {
      return std::nullopt;
    }
  } else if (type == "join_pending" || type == "join_request" ||
             type == "join_closed") {
    m.kind = type == "join_pending"   ? Kind::kJoinPending
             : type == "join_request" ? Kind::kJoinRequest
                                      : Kind::kJoinClosed;
    m.join_id = String(doc, "id");
    m.join_number = String(doc, "number");
    m.join_ttl_s = uint32_t(Uint64(doc, "ttl"));
    m.reason = String(doc, "reason");
    auto device = doc.FindMember("device");
    if (device != doc.MemberEnd() && device->value.IsObject()) {
      m.join_device.name = CleanText(String(device->value, "n"), 40);
      m.join_device.platform = CleanText(String(device->value, "p"), 40);
    }
    auto scanned = doc.FindMember("scanned");
    m.join_scanned = scanned != doc.MemberEnd() && scanned->value.IsBool() &&
                     scanned->value.GetBool();
    if (m.join_id.empty()) {
      return std::nullopt;
    }
  } else if (type == "session_token") {
    m.kind = Kind::kSessionToken;
    m.token = String(doc, "token");
    m.token_ttl_s = uint32_t(Uint64(doc, "ttl"));
    if (m.token.empty()) {
      return std::nullopt;
    }
  } else if (type == "devices") {
    m.kind = Kind::kDevices;
    m.devices = ParseDevices(doc);
  } else if (type == "reported") {
    m.kind = Kind::kReported;
    m.report_id = String(doc, "id");
    m.xuid = Xuid(doc, "xuid");
  } else if (type == "names" || type == "found") {
    m.kind = type == "names" ? Kind::kNames : Kind::kFound;
    m.users = ParseUsers(doc);
  } else if (type == "results") {
    m.kind = Kind::kResults;
    m.prefix = String(doc, "prefix");
    m.users = ParseUsers(doc);
  } else if (type == "settings") {
    m.kind = Kind::kSettings;
    m.requests = ParseRequests(doc);
    if (!m.requests) {
      return std::nullopt;
    }
  } else if (type == "error") {
    m.kind = Kind::kError;
    m.reason = String(doc, "reason");
    m.for_request = String(doc, "for");
    m.to = Xuid(doc, "to");
    // Which player, for an error answering a report.
    m.xuid = Xuid(doc, "xuid");
    m.gamertag = CleanGamertag(String(doc, "gamertag"));
    m.next_ms = Uint64(doc, "next");
  }
  return m;
}

std::string LobbyHost(std::string_view lobby) {
  bool secure = false;
  if (lobby.starts_with("wss://")) {
    secure = true;
    lobby.remove_prefix(6);
  } else if (lobby.starts_with("ws://")) {
    lobby.remove_prefix(5);
  }
  std::string host(lobby.substr(0, lobby.find_first_of("/?#")));
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  const std::string_view default_port = secure ? ":443" : ":80";
  if (host.ends_with(default_port)) {
    host.resize(host.size() - default_port.size());
  }
  return host;
}

namespace {

// The device's name as the lobby takes it: printable, up to 32 characters
// (cut here to 32 bytes, at a character).
std::string DeviceName(std::string_view name) {
  std::string clean;
  for (const char c : name) {
    if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) {
      clean.push_back(c);
    }
  }
  while (!clean.empty() && clean.front() == ' ') {
    clean.erase(clean.begin());
  }
  if (clean.size() > 32) {
    size_t size = 32;
    while (size && (static_cast<unsigned char>(clean[size]) & 0xC0) == 0x80) {
      --size;
    }
    clean.resize(size);
  }
  while (!clean.empty() && clean.back() == ' ') {
    clean.pop_back();
  }
  return clean;
}

// Up to 16 characters of a-z 0-9 . _ -.
std::string DevicePlatform(std::string_view platform) {
  std::string clean;
  for (const char c : platform) {
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
        lower == '.' || lower == '_' || lower == '-') {
      clean.push_back(lower);
    }
  }
  clean.resize(std::min<size_t>(clean.size(), 16));
  return clean;
}

void WriteDevice(JsonWriter& w, const DeviceInfo& device) {
  const std::string name = DeviceName(device.name);
  const std::string platform = DevicePlatform(device.platform);
  if (name.empty() && platform.empty()) {
    return;
  }
  w.Key("device");
  w.StartObject();
  if (!name.empty()) {
    Field(w, "name", name);
  }
  if (!platform.empty()) {
    Field(w, "platform", platform);
  }
  w.EndObject();
}

// Upper case without dashes and spaces; empty if anything else is in it.
std::string Normalize(std::string_view typed, size_t size) {
  std::string code;
  for (const char c : typed) {
    if (c == '-' || c == ' ') {
      continue;
    }
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      return {};
    }
    code.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return code.size() == size ? code : std::string();
}

}  // namespace

DeviceInfo DefaultDeviceInfo() {
  DeviceInfo info;
#if XE_PLATFORM_IOS
  info.platform = "ios";
#elif XE_PLATFORM_MAC
  info.platform = "macos";
#elif XE_PLATFORM_WIN32
  info.platform = "windows";
#else
  info.platform = "linux";
#endif
#if XE_PLATFORM_WIN32
  if (const char* host = std::getenv("COMPUTERNAME")) {
    info.name = host;
  }
#else
  char host[256] = {};
  if (gethostname(host, sizeof(host) - 1) == 0) {
    info.name = host;
    info.name = info.name.substr(0, info.name.find('.'));
  }
#endif
  if (info.name == "localhost") {
    info.name.clear();
  }
  info.name = DeviceName(info.name);
  return info;
}

std::string NormalizeRecoveryCode(std::string_view typed) {
  return Normalize(typed, 20);
}

std::string CleanGamertag(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (out.size() == 15) {
      break;
    }
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || (c == ' ' && !out.empty())) {
      out.push_back(c);
    }
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string CleanText(std::string_view text, size_t max_chars) {
  std::string out;
  size_t chars = 0;
  auto it = text.begin();
  while (it != text.end() && chars < max_chars) {
    const auto start = it;
    uint32_t cp;
    try {
      cp = ::utf8::next(it, text.end());
    } catch (const ::utf8::exception&) {
      break;
    }
    const bool hidden = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) ||
                        (cp >= 0x200B && cp <= 0x200F) ||
                        (cp >= 0x202A && cp <= 0x202E) ||
                        (cp >= 0x2060 && cp <= 0x2069) || cp == 0xFEFF;
    if (!hidden) {
      out.append(start, it);
      ++chars;
    }
  }
  return out;
}

std::string NormalizeLinkCode(std::string_view typed) {
  return Normalize(typed, 8);
}

std::string NameCheckUrl(std::string_view lobby) {
  std::string url;
  if (lobby.starts_with("wss://")) {
    url = "https://";
    lobby.remove_prefix(6);
  } else if (lobby.starts_with("ws://")) {
    url = "http://";
    lobby.remove_prefix(5);
  } else {
    url = "https://";
  }
  while (lobby.ends_with('/')) {
    lobby.remove_suffix(1);
  }
  return url + std::string(lobby) + "/v2/names/check";
}

std::string EncodeNameCheckBody(std::string_view gamertag) {
  return Json([&](JsonWriter& w) { Field(w, "gamertag", gamertag); });
}

std::optional<NameCheck> ParseNameCheckResponse(std::string_view gamertag,
                                                std::string_view body) {
  if (body.size() > kMaxLobbyMessage) {
    return std::nullopt;
  }
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseIterativeFlag>(body.data(), body.size());
  if (doc.HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }
  NameCheck check = ParseNameCheck(doc, gamertag);
  if (check.status.empty()) {
    return std::nullopt;
  }
  return check;
}

std::string LobbyWebRoot(std::string_view lobby) {
  const bool secure = !lobby.starts_with("ws://");
  return (secure ? "https://" : "http://") + LobbyHost(lobby);
}

namespace {
// The presence a sign-in starts with: only appearing offline needs one.
void WriteStartPresence(JsonWriter& w, bool appear_offline) {
  if (!appear_offline) {
    return;
  }
  w.Key("presence");
  w.StartObject();
  w.Key("offline");
  w.Bool(true);
  w.EndObject();
}
}  // namespace

std::string EncodeRegister(const DeviceKey& key, std::string_view host,
                           uint64_t xuid, std::string_view gamertag,
                           std::string_view nonce, const DeviceInfo& device,
                           bool appear_offline) {
  const std::string xuid_text = xuid ? XuidHex(xuid) : std::string();
  const std::string public_key = key.PublicHex();
  const std::string sig =
      key.Sign(fmt::format("xenios-register\n{}\n{}\n{}\n{}\n{}", host,
                           xuid_text, gamertag, public_key, nonce));
  return Json([&](JsonWriter& w) {
    Field(w, "t", "register");
    w.Key("v");
    w.Uint(2);
    Field(w, "xuid", xuid_text);
    Field(w, "gamertag", gamertag);
    Field(w, "key", public_key);
    Field(w, "sig", sig);
    WriteDevice(w, device);
    WriteStartPresence(w, appear_offline);
  });
}

std::string EncodeAuth(const DeviceKey& key, std::string_view host,
                       uint64_t xuid, std::string_view nonce,
                       const DeviceInfo& device, bool appear_offline,
                       bool passive) {
  const std::string xuid_text = XuidHex(xuid);
  const std::string sig = key.Sign(
      fmt::format("xenios-auth\n{}\n{}\n{}", host, xuid_text, nonce));
  return Json([&](JsonWriter& w) {
    Field(w, "t", "auth");
    w.Key("v");
    w.Uint(2);
    Field(w, "xuid", xuid_text);
    Field(w, "key", key.PublicHex());
    Field(w, "sig", sig);
    WriteDevice(w, device);
    WriteStartPresence(w, appear_offline);
    if (passive) {
      w.Key("passive");
      w.Bool(true);
    }
  });
}

std::string JoinSignedText(std::string_view kind, std::string_view host,
                           std::string_view code, std::string_view key,
                           std::string_view nonce) {
  return fmt::format("xenios-{}\n{}\n{}\n{}\n{}", kind, host, code, key,
                     nonce);
}

namespace {
std::string EncodeJoin(const char* kind, const DeviceKey& key,
                       std::string_view host, std::string_view code,
                       std::string_view nonce, const DeviceInfo& device) {
  const std::string public_key = key.PublicHex();
  const std::string sig =
      key.Sign(JoinSignedText(kind, host, code, public_key, nonce));
  return Json([&](JsonWriter& w) {
    Field(w, "t", kind);
    w.Key("v");
    w.Uint(2);
    Field(w, "code", code);
    Field(w, "key", public_key);
    Field(w, "sig", sig);
    WriteDevice(w, device);
  });
}
}  // namespace

std::string EncodeLink(const DeviceKey& key, std::string_view host,
                       std::string_view code, std::string_view nonce,
                       const DeviceInfo& device) {
  return EncodeJoin("link", key, host, code, nonce, device);
}

std::string EncodeRecover(const DeviceKey& key, std::string_view host,
                          std::string_view code, std::string_view nonce,
                          const DeviceInfo& device) {
  return EncodeJoin("recover", key, host, code, nonce, device);
}

std::string EncodeJoinRequest(const DeviceKey& key, std::string_view host,
                              std::string_view gamertag, std::string_view nonce,
                              const DeviceInfo& device) {
  const std::string public_key = key.PublicHex();
  const std::string sig =
      key.Sign(JoinSignedText("join", host, gamertag, public_key, nonce));
  return Json([&](JsonWriter& w) {
    Field(w, "t", "join_request");
    w.Key("v");
    w.Uint(2);
    if (!gamertag.empty()) {
      Field(w, "gamertag", gamertag);
    }
    Field(w, "key", public_key);
    Field(w, "sig", sig);
    WriteDevice(w, device);
  });
}

std::string ApproveSignedText(std::string_view host, std::string_view id,
                              std::string_view nonce) {
  return fmt::format("xenios-approve\n{}\n{}\n{}", host, id, nonce);
}

std::string EncodeJoinApprove(const DeviceKey& key, std::string_view host,
                              std::string_view id, std::string_view nonce) {
  const std::string sig = key.Sign(ApproveSignedText(host, id, nonce));
  return Json([&](JsonWriter& w) {
    Field(w, "t", "join_approve");
    Field(w, "id", id);
    Field(w, "sig", sig);
  });
}

std::string EncodeJoinDeny(std::string_view id) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "join_deny");
    Field(w, "id", id);
  });
}

std::string EncodeJoinPeek(std::string_view id) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "join_peek");
    Field(w, "id", id);
  });
}

std::string JoinQrPayload(std::string_view id) {
  return fmt::format("xenios-join:{}", id);
}

std::string ParseJoinQrPayload(std::string_view text) {
  constexpr std::string_view kPrefix = "xenios-join:";
  if (!text.starts_with(kPrefix)) {
    return {};
  }
  text.remove_prefix(kPrefix.size());
  // The lobby's IDs are 128 random bits in hex.
  if (text.size() != 32 ||
      !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
      })) {
    return {};
  }
  return std::string(text);
}

std::string EncodeCheckName(std::string_view gamertag) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "check");
    Field(w, "gamertag", gamertag);
  });
}

std::string EncodeRename(std::string_view gamertag) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "rename");
    Field(w, "gamertag", gamertag);
  });
}

std::string EncodeRecoveryNew() {
  return Json([&](JsonWriter& w) { Field(w, "t", "recovery_new"); });
}

std::string EncodeLinkCode() {
  return Json([&](JsonWriter& w) { Field(w, "t", "link_code"); });
}

std::string EncodeSessionToken() {
  return Json([&](JsonWriter& w) { Field(w, "t", "session_token"); });
}

std::string EncodeDevices() {
  return Json([&](JsonWriter& w) { Field(w, "t", "devices"); });
}

std::string EncodeRemoveDevice(std::string_view key) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "remove_device");
    Field(w, "key", key);
  });
}

std::string EncodeDeleteAccount() {
  return Json([&](JsonWriter& w) { Field(w, "t", "delete_account"); });
}

std::string EncodePresence(uint32_t title_id, uint32_t context,
                           std::string_view rich,
                           const std::optional<Joinable>& joinable,
                           bool offline) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "presence");
    if (title_id) {
      Field(w, "title", fmt::format("{:08X}", title_id));
      w.Key("context");
      w.Uint(context);
    }
    if (!rich.empty()) {
      Field(w, "rich", rich.substr(0, 64));
    }
    if (joinable) {
      w.Key("joinable");
      w.StartObject();
      WriteJoinable(w, *joinable);
      w.EndObject();
    }
    if (offline) {
      w.Key("offline");
      w.Bool(true);
    }
  });
}

std::string EncodeFriendAdd(uint64_t xuid) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "friend_add");
    Field(w, "xuid", XuidHex(xuid));
  });
}

std::string EncodeBlock(uint64_t xuid) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "block");
    Field(w, "xuid", XuidHex(xuid));
  });
}

std::string EncodeUnblock(uint64_t xuid) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "unblock");
    Field(w, "xuid", XuidHex(xuid));
  });
}

bool IsReportReason(std::string_view reason) {
  return reason == "cheating" || reason == "harassment" ||
         reason == "offensive_name" || reason == "offensive_content" ||
         reason == "spam" || reason == "other";
}

namespace {
// The lobby's UNPRINTABLE (src/common.ts): control characters, bidirectional
// controls and invisible format characters.
bool Unprintable(uint32_t cp) {
  return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD ||
         cp == 0x61C || cp == 0x180E || (cp >= 0x200B && cp <= 0x200F) ||
         (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) ||
         (cp >= 0x2066 && cp <= 0x206F) || cp == 0xFEFF ||
         (cp >= 0xFFF9 && cp <= 0xFFFB);
}
// What JavaScript's trim() takes off the ends (of what Unprintable leaves).
bool TrimmedSpace(uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xA0 ||
         cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}
}  // namespace

std::string CleanReportNote(std::string_view note) {
  std::vector<uint32_t> kept;
  auto it = note.begin();
  while (it != note.end()) {
    uint32_t cp;
    try {
      cp = ::utf8::next(it, note.end());
    } catch (const ::utf8::exception&) {
      break;
    }
    if (cp == '\n' || cp == '\t' || !Unprintable(cp)) {
      kept.push_back(cp);
    }
  }
  size_t begin = 0;
  size_t end = kept.size();
  while (begin < end && TrimmedSpace(kept[begin])) {
    ++begin;
  }
  while (end > begin && TrimmedSpace(kept[end - 1])) {
    --end;
  }
  std::string out;
  size_t units = 0;
  for (size_t i = begin; i < end; ++i) {
    // Characters past U+FFFF are two UTF-16 units.
    units += kept[i] > 0xFFFF ? 2 : 1;
    if (units > kMaxReportNote) {
      break;
    }
    ::utf8::append(kept[i], std::back_inserter(out));
  }
  // Cutting may leave spaces at the end; the lobby doesn't mind those.
  return out;
}

std::string EncodeReport(uint64_t xuid, std::string_view reason,
                         std::string_view note, uint32_t title_id,
                         std::string_view room) {
  const std::string clean_note = CleanReportNote(note);
  // The lobby's ROOM_CODE: Crockford's alphabet, 6 characters.
  const bool room_code =
      room.size() == 6 &&
      std::all_of(room.begin(), room.end(), [](char c) {
        return (c >= '0' && c <= '9') ||
               (c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O' &&
                c != 'U');
      });
  return Json([&](JsonWriter& w) {
    Field(w, "t", "report");
    Field(w, "xuid", XuidHex(xuid));
    Field(w, "reason", reason);
    if (!clean_note.empty()) {
      Field(w, "note", clean_note);
    }
    if (title_id) {
      Field(w, "title", fmt::format("{:08X}", title_id));
    }
    if (room_code) {
      Field(w, "room", room);
    }
  });
}

std::string EncodeNames(const std::vector<uint64_t>& xuids) {
  if (xuids.empty() || xuids.size() > kMaxNamesPerRequest) {
    return {};
  }
  for (uint64_t xuid : xuids) {
    // The lobby takes online XUIDs only: 0009 and 12 hex digits.
    if ((xuid >> 48) != 0x0009) {
      return {};
    }
  }
  return Json([&](JsonWriter& w) {
    Field(w, "t", "names");
    w.Key("xuids");
    w.StartArray();
    for (uint64_t xuid : xuids) {
      const std::string hex = XuidHex(xuid);
      w.String(hex.data(), rapidjson::SizeType(hex.size()));
    }
    w.EndArray();
  });
}

std::string EncodeSettings(std::optional<FriendRequests> requests) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "settings");
    if (requests) {
      Field(w, "requests",
            *requests == FriendRequests::kCode ? "code" : "anyone");
    }
  });
}

std::string EncodeFriendAddCode(std::string_view code) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "friend_add");
    Field(w, "code", code);
  });
}

std::string EncodeSearch(std::string_view prefix) {
  while (!prefix.empty() && prefix.front() == ' ') prefix.remove_prefix(1);
  while (!prefix.empty() && prefix.back() == ' ') prefix.remove_suffix(1);
  if (prefix.size() < 3 || prefix.size() > 15) {
    return {};
  }
  return Json([&](JsonWriter& w) {
    Field(w, "t", "search");
    Field(w, "prefix", prefix);
  });
}

std::string EncodeFriendEdit(std::string_view type, uint64_t xuid) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", type);
    Field(w, "xuid", XuidHex(xuid));
  });
}

std::string EncodeInvite(uint64_t to, uint32_t title_id,
                         const Joinable& joinable, std::string_view message) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "invite");
    Field(w, "kind", "game");
    Field(w, "to", XuidHex(to));
    Field(w, "title", fmt::format("{:08X}", title_id));
    WriteJoinable(w, joinable);
    if (!message.empty()) {
      Field(w, "message", message.substr(0, 100));
    }
  });
}

std::string EncodeInviteReply(std::string_view id, bool accept) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "invite_reply");
    Field(w, "id", id);
    w.Key("accept");
    w.Bool(accept);
  });
}

std::string EncodePartyCreate() {
  return Json([&](JsonWriter& w) { Field(w, "t", "party_create"); });
}

std::string EncodePartyLeave() {
  return Json([&](JsonWriter& w) { Field(w, "t", "party_leave"); });
}

std::string EncodePartyInvite(uint64_t to, uint32_t title_id) {
  return Json([&](JsonWriter& w) {
    Field(w, "t", "invite");
    Field(w, "kind", "party");
    Field(w, "to", XuidHex(to));
    // The dashboard's title when the sender is in none.
    Field(w, "title", fmt::format("{:08X}", title_id ? title_id : 0xFFFE07D1));
  });
}

AccountClient::AccountClient(std::string lobby, DeviceKey key, uint64_t xuid,
                             std::string gamertag, Events events,
                             AccountOptions options)
    : lobby_(std::move(lobby)),
      key_(std::move(key)),
      gamertag_(std::move(gamertag)),
      events_(std::move(events)),
      device_(std::move(options.device)),
      join_(std::move(options.join)),
      xuid_(xuid),
      appear_offline_(options.appear_offline),
      sign_in_only_(options.sign_in_only) {
  network_listener_ = AddNetworkPathListener([this] {
    std::lock_guard lock(mutex_);
    network_changed_ = true;
    changed_.notify_all();
  });
  thread_ = std::thread([this] { Run(); });
}

namespace {
// The Calls of the event running on this thread, if any.
thread_local const void* current_calls = nullptr;
}  // namespace

void AccountClient::Call(const std::shared_ptr<Calls>& calls,
                         const std::function<void()>& call) {
  {
    std::lock_guard lock(calls->mutex);
    if (calls->closed) {
      return;
    }
    ++calls->running;
  }
  const void* outer = current_calls;
  current_calls = calls.get();
  call();
  current_calls = outer;
  {
    std::lock_guard lock(calls->mutex);
    --calls->running;
  }
  calls->done.notify_all();
}

AccountClient::~AccountClient() {
  RemoveNetworkPathListener(network_listener_);
  {
    // No event starts from now on; wait for the running ones, except the one
    // on this thread when an event drops the last reference.
    std::unique_lock lock(calls_->mutex);
    calls_->closed = true;
    const uint32_t own = current_calls == calls_.get() ? 1 : 0;
    calls_->done.wait(lock, [&] { return calls_->running <= own; });
  }
  std::unique_ptr<secure::LobbySocket> socket;
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    socket = std::move(socket_);
    ++generation_;
  }
  changed_.notify_all();
  thread_.join();
  socket.reset();
}

bool AccountClient::signed_in() const {
  std::lock_guard lock(mutex_);
  return signed_in_;
}

std::string AccountClient::problem() const {
  std::lock_guard lock(mutex_);
  return problem_;
}

bool AccountClient::halted() const {
  std::lock_guard lock(mutex_);
  return halted_;
}

uint64_t AccountClient::xuid() const {
  std::lock_guard lock(mutex_);
  return xuid_;
}

std::string AccountClient::friend_code() const {
  std::lock_guard lock(mutex_);
  return code_;
}

std::vector<Friend> AccountClient::Friends() const {
  std::lock_guard lock(mutex_);
  return friends_;
}

bool AccountClient::IsFriend(uint64_t xuid) const {
  std::lock_guard lock(mutex_);
  return std::any_of(friends_.begin(), friends_.end(), [&](const Friend& f) {
    return f.xuid == xuid && f.state == Friend::State::kFriend;
  });
}

bool AccountClient::IsBlocked(uint64_t xuid) const {
  std::lock_guard lock(mutex_);
  return std::find(blocked_.begin(), blocked_.end(), xuid) != blocked_.end();
}

std::vector<uint64_t> AccountClient::Blocked() const {
  std::lock_guard lock(mutex_);
  return blocked_;
}

std::optional<Presence> AccountClient::FriendPresence(uint64_t xuid) const {
  std::lock_guard lock(mutex_);
  for (const auto& p : presence_) {
    if (p.xuid == xuid) {
      return p;
    }
  }
  return std::nullopt;
}

std::optional<PartyState> AccountClient::Party() const {
  std::lock_guard lock(mutex_);
  return party_;
}

std::string AccountClient::gamertag() const {
  std::lock_guard lock(mutex_);
  return account_gamertag_;
}

std::string AccountClient::rename_prompt() const {
  std::lock_guard lock(mutex_);
  return rename_;
}

uint64_t AccountClient::next_rename_ms() const {
  std::lock_guard lock(mutex_);
  return next_rename_ms_;
}

std::optional<Refused> AccountClient::refused() const {
  std::lock_guard lock(mutex_);
  return refused_;
}

std::vector<Device> AccountClient::Devices() const {
  std::lock_guard lock(mutex_);
  return devices_;
}

std::vector<Invite> AccountClient::Invites() const {
  const auto oldest = std::chrono::steady_clock::now() -
                      std::chrono::seconds(kInviteLifetimeSeconds);
  std::lock_guard lock(mutex_);
  std::vector<Invite> invites;
  for (const auto& held : invites_) {
    if (held.received >= oldest) {
      invites.push_back(held.invite);
    }
  }
  return invites;
}

namespace {
// Gamertags kept for players who aren't friends; past this many the client
// starts over (friends are named from the friend list anyway).
constexpr size_t kMaxNames = 4096;
// A player the lobby didn't name isn't asked for again before this.
constexpr std::chrono::minutes kNamesAskAgain(10);
// Players waiting to be asked for, at most.
constexpr size_t kMaxNamesQueue = 256;
}  // namespace

std::string AccountClient::GamertagOf(uint64_t xuid) const {
  std::lock_guard lock(mutex_);
  for (const auto& f : friends_) {
    if (f.xuid == xuid && !f.gamertag.empty()) {
      return f.gamertag;
    }
  }
  auto it = names_.find(xuid);
  return it != names_.end() ? it->second : std::string();
}

void AccountClient::RememberNameLocked(uint64_t xuid,
                                       const std::string& gamertag) {
  if (!xuid || gamertag.empty()) {
    return;
  }
  if (names_.size() >= kMaxNames && !names_.count(xuid)) {
    names_.clear();
  }
  names_[xuid] = gamertag;
}

void AccountClient::RequestNames(const std::vector<uint64_t>& xuids) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex_);
  std::erase_if(names_asked_, [&](const auto& asked) {
    return now - asked.second >= kNamesAskAgain;
  });
  bool queued = false;
  for (uint64_t xuid : xuids) {
    // Only online XUIDs have accounts; the lobby refuses others.
    if ((xuid >> 48) != 0x0009 || xuid == xuid_ || names_.count(xuid) ||
        names_asked_.count(xuid) ||
        std::find(names_queue_.begin(), names_queue_.end(), xuid) !=
            names_queue_.end() ||
        std::any_of(friends_.begin(), friends_.end(), [&](const Friend& f) {
          return f.xuid == xuid && !f.gamertag.empty();
        })) {
      continue;
    }
    if (names_queue_.size() >= kMaxNamesQueue) {
      break;
    }
    names_queue_.push_back(xuid);
    queued = true;
  }
  if (!queued) {
    return;
  }
  SendNamesLocked();
  if (!names_queue_.empty()) {
    // The rest go when the rate allows: Run() waits for that.
    names_wake_ = true;
    changed_.notify_all();
  }
}

void AccountClient::SendNamesLocked() {
  if (!signed_in_ || !socket_ || !lobby_has_settings_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now < names_paused_until_) {
    return;
  }
  std::erase_if(names_sent_, [&](const auto& sent) {
    return now - sent >= std::chrono::minutes(1);
  });
  while (!names_queue_.empty() &&
         names_sent_.size() < kNamesRequestsPerMinute) {
    const size_t count = std::min(names_queue_.size(), kMaxNamesPerRequest);
    std::vector<uint64_t> batch(names_queue_.begin(),
                                names_queue_.begin() + count);
    names_queue_.erase(names_queue_.begin(), names_queue_.begin() + count);
    std::string text = EncodeNames(batch);
    if (text.empty()) {
      continue;
    }
    for (uint64_t xuid : batch) {
      names_asked_[xuid] = now;
    }
    names_last_ = std::move(batch);
    names_sent_.push_back(now);
    socket_->Send(std::move(text));
  }
}

std::optional<std::chrono::steady_clock::duration>
AccountClient::NamesWaitLocked() const {
  if (names_queue_.empty() || !signed_in_ || !socket_ ||
      !lobby_has_settings_) {
    return std::nullopt;
  }
  const auto now = std::chrono::steady_clock::now();
  auto when = std::max(now, names_paused_until_);
  if (names_sent_.size() >= kNamesRequestsPerMinute) {
    when = std::max(when, names_sent_.front() + std::chrono::minutes(1));
  }
  return when - now;
}

std::optional<FriendRequests> AccountClient::FriendRequestsSetting() const {
  std::lock_guard lock(mutex_);
  return lobby_has_settings_ ? friend_requests_ : std::nullopt;
}

bool AccountClient::SetFriendRequests(FriendRequests requests) {
  std::lock_guard lock(mutex_);
  // A lobby without settings closes the socket on a message it doesn't know.
  if (!signed_in_ || !socket_ || !lobby_has_settings_) {
    return false;
  }
  socket_->Send(EncodeSettings(requests));
  return true;
}

void AccountClient::SetPresence(uint32_t title_id, uint32_t context,
                                std::string rich,
                                std::optional<Joinable> joinable) {
  std::lock_guard lock(mutex_);
  presence_title_id_ = title_id;
  presence_context_ = context;
  presence_rich_ = std::move(rich);
  presence_joinable_ = std::move(joinable);
  UpdatePresenceLocked();
}

void AccountClient::SetAppearOffline(bool appear_offline) {
  std::lock_guard lock(mutex_);
  if (appear_offline == appear_offline_) {
    return;
  }
  appear_offline_ = appear_offline;
  UpdatePresenceLocked();
}

bool AccountClient::appear_offline() const {
  std::lock_guard lock(mutex_);
  return appear_offline_;
}

void AccountClient::UpdatePresenceLocked() {
  std::string message =
      EncodePresence(presence_title_id_, presence_context_, presence_rich_,
                     presence_joinable_, appear_offline_);
  if (message == presence_message_) {
    return;
  }
  presence_message_ = std::move(message);
  if (signed_in_) {
    socket_->Send(presence_message_);
  }
}

void AccountClient::AddFriend(uint64_t xuid) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeFriendAdd(xuid));
}

void AccountClient::AddFriendByCode(std::string code) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeFriendAddCode(code));
}

void AccountClient::DeclineFriend(uint64_t xuid) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeFriendEdit("friend_decline", xuid));
}

void AccountClient::SearchPlayers(const std::string& prefix) {
  std::lock_guard lock(mutex_);
  if (!lobby_has_settings_) {
    return;
  }
  const std::string message = EncodeSearch(prefix);
  if (!message.empty()) {
    SendLocked(message);
  }
}

void AccountClient::RemoveFriend(uint64_t xuid) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeFriendEdit("friend_remove", xuid));
}

void AccountClient::Block(uint64_t xuid) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeBlock(xuid));
}

void AccountClient::Unblock(uint64_t xuid) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeUnblock(xuid));
}

bool AccountClient::Report(uint64_t xuid, std::string reason, std::string note,
                           uint32_t title_id, std::string room) {
  if (!IsReportReason(reason)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  SendLocked(EncodeReport(xuid, reason, note, title_id, room));
  return true;
}

void AccountClient::SendInvite(uint64_t to, uint32_t title_id,
                               Joinable joinable, std::string message) {
  std::lock_guard lock(mutex_);
  sent_invites_[to] = {joinable.session_id, std::chrono::steady_clock::now()};
  SendLocked(EncodeInvite(to, title_id, joinable, message));
}

std::optional<std::string> AccountClient::InvitedTo(
    uint64_t xuid, std::chrono::steady_clock::duration window) const {
  std::lock_guard lock(mutex_);
  auto found = sent_invites_.find(xuid);
  if (found == sent_invites_.end() ||
      std::chrono::steady_clock::now() - found->second.at > window) {
    return std::nullopt;
  }
  return found->second.session_id;
}

void AccountClient::ReplyInvite(std::string id, bool accept) {
  std::lock_guard lock(mutex_);
  if (id.empty()) {
    return;
  }
  auto held = std::find_if(invites_.begin(), invites_.end(),
                           [&](const HeldInvite& h) { return h.invite.id == id; });
  const bool expired =
      held != invites_.end() &&
      std::chrono::steady_clock::now() - held->received >
          std::chrono::seconds(kInviteLifetimeSeconds);
  if (held != invites_.end()) {
    invites_.erase(held);
  }
  // The lobby would answer not_found: nothing to tell it, or the player.
  if (expired || std::find(gone_invites_.begin(), gone_invites_.end(), id) !=
                     gone_invites_.end()) {
    XELOGI("Live account: invite {} was answered, replaced or expired; not "
           "answering it",
           id);
    return;
  }
  ForgetInviteLocked(id);
  SendLocked(EncodeInviteReply(id, accept));
}

void AccountClient::ForgetInviteLocked(const std::string& id) {
  if (std::find(gone_invites_.begin(), gone_invites_.end(), id) !=
      gone_invites_.end()) {
    return;
  }
  gone_invites_.push_back(id);
  if (gone_invites_.size() > kGoneInvitesKept) {
    gone_invites_.erase(gone_invites_.begin());
  }
}

void AccountClient::CreateParty() {
  std::lock_guard lock(mutex_);
  SendLocked(EncodePartyCreate());
}

void AccountClient::LeaveParty() {
  std::lock_guard lock(mutex_);
  SendLocked(EncodePartyLeave());
}

void AccountClient::InviteToParty(uint64_t to, uint32_t title_id) {
  std::lock_guard lock(mutex_);
  // Into the party the lobby has for the account, if any: it answers
  // no_party when there's none, and that starts one (PartyInviteRefusedLocked).
  // Before the lobby's party message it isn't known whether there's one: a
  // relaunched app has its old party restored right after the welcome.
  if (!party_) {
    std::erase_if(party_invites_unsure_,
                  [to](const auto& unsure) { return unsure.first == to; });
    party_invites_unsure_.emplace_back(to, title_id);
  }
  SendLocked(EncodePartyInvite(to, title_id));
}

// The lobby refused a party invite to |to| for want of a party: start one and
// invite them into it.
void AccountClient::PartyInviteRefusedLocked(uint64_t to) {
  auto unsure = std::find_if(party_invites_unsure_.begin(),
                             party_invites_unsure_.end(),
                             [to](const auto& u) { return u.first == to; });
  if (unsure == party_invites_unsure_.end()) {
    return;
  }
  const uint32_t title_id = unsure->second;
  party_invites_unsure_.erase(unsure);
  if (party_) {
    // A party came meanwhile: invite into it.
    SendLocked(EncodePartyInvite(to, title_id));
    return;
  }
  const bool creating = !party_invites_waiting_.empty();
  std::erase_if(party_invites_waiting_,
                [to](const auto& waiting) { return waiting.first == to; });
  party_invites_waiting_.emplace_back(to, title_id);
  if (!creating) {
    XELOGI("Live account: starting a party to invite {:016X} to", to);
    SendLocked(EncodePartyCreate());
  }
}

void AccountClient::RetryRegister(std::string gamertag) {
  std::lock_guard lock(mutex_);
  if (signed_in_ || !socket_ || nonce_.empty() || join_ || sign_in_only_) {
    return;
  }
  gamertag_ = std::move(gamertag);
  socket_->Send(EncodeRegister(key_, LobbyHost(lobby_), xuid_, gamertag_,
                               nonce_, device_, appear_offline_));
}

bool AccountClient::CheckName(std::string gamertag) {
  std::lock_guard lock(mutex_);
  if (!signed_in_ || !socket_) {
    return false;
  }
  socket_->Send(EncodeCheckName(gamertag));
  return true;
}

void AccountClient::Rename(std::string gamertag) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeRename(gamertag));
}

void AccountClient::RequestLinkCode() {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeLinkCode());
}

void AccountClient::ApproveJoin(std::string id) {
  std::lock_guard lock(mutex_);
  if (signed_in_ && socket_ && !nonce_.empty()) {
    socket_->Send(EncodeJoinApprove(key_, LobbyHost(lobby_), id, nonce_));
  }
}

void AccountClient::DenyJoin(std::string id) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeJoinDeny(id));
}

void AccountClient::PeekJoin(std::string id) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeJoinPeek(id));
}

void AccountClient::RequestSessionToken() {
  std::lock_guard lock(mutex_);
  // One waiting request is enough; the lobby limits them.
  const std::string request = EncodeSessionToken();
  if (!signed_in_ &&
      std::find(pending_.begin(), pending_.end(), request) != pending_.end()) {
    return;
  }
  SendLocked(request);
}

std::optional<std::string> AccountClient::SessionToken() const {
  std::lock_guard lock(mutex_);
  if (session_token_.empty() ||
      session_token_expires_ - std::chrono::steady_clock::now() <
          std::chrono::minutes(5)) {
    return std::nullopt;
  }
  return session_token_;
}

void AccountClient::NewRecoveryCode() {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeRecoveryNew());
}

void AccountClient::RequestDevices() {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeDevices());
}

void AccountClient::RemoveDevice(std::string key) {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeRemoveDevice(key));
}

void AccountClient::RemoveThisDevice() {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeRemoveDevice(key_.PublicHex()));
}

void AccountClient::DeleteAccount() {
  std::lock_guard lock(mutex_);
  SendLocked(EncodeDeleteAccount());
}

// Sends at once when signed in, else after signing in.
void AccountClient::SendLocked(std::string text) {
  if (signed_in_ && socket_) {
    socket_->Send(std::move(text));
  } else if (pending_.size() < 32) {
    pending_.push_back(std::move(text));
  }
}

void AccountClient::EnsureOpenLocked() {
  // With no network, Run opens it when one comes back.
  if (socket_ || stopping_ || halted_ || !NetworkPathAvailable()) {
    return;
  }
  auto factory = secure::GetLobbySocketFactory();
  if (!factory) {
    static bool said = false;
    if (!std::exchange(said, true)) {
      XELOGW("Live account: this platform can't reach the lobby, so no "
             "friends or invites");
    }
    return;
  }
  const uint64_t generation = ++generation_;
  secure::LobbySocket::Events events;
  events.opened = [] {};
  events.message = [this, calls = calls_, generation](std::string text) {
    Call(calls, [&] {
      {
        std::lock_guard lock(mutex_);
        if (generation != generation_) {
          return;
        }
      }
      OnMessage(std::move(text));
    });
  };
  events.closed = [this, calls = calls_, generation](std::string reason) {
    Call(calls, [&] {
      std::lock_guard lock(mutex_);
      if (generation == generation_) {
        OnClosed(std::move(reason));
      }
    });
  };
  // A profile without an online XUID gets one made up for it, and so does a
  // device that joins an account.
  const std::string url =
      xuid_ && !join_ ? fmt::format("{}/v2/account/{}", lobby_, XuidHex(xuid_))
            : lobby_ + "/v2/account";
  socket_ = factory(url, std::move(events));
  if (!socket_) {
    XELOGW("Live account: can't connect to the lobby at {}", lobby_);
  }
}

// Runs without |mutex_| held, so the events can be called. Touches nothing
// of the client after calling one: the event may drop its last reference.
void AccountClient::OnMessage(std::string text) {
  auto m = ParseAccountMessage(text);
  if (!m) {
    return;
  }
  using Kind = AccountMessage::Kind;
  std::function<void()> notify;
  {
    std::lock_guard lock(mutex_);
    if (!socket_) {
      return;
    }
    switch (m->kind) {
      case Kind::kChallenge:
        nonce_ = m->nonce;
        register_refusals_ = 0;
        if (join_ && join_->mode == AccountJoin::Mode::kRequest) {
          // Asking the account's devices: nothing is registered or signed in
          // until one of them says yes.
          socket_->Send(EncodeJoinRequest(key_, LobbyHost(lobby_), join_->code,
                                          m->nonce, device_));
        } else if (join_) {
          // A new device: the code is the credential, so nothing is registered.
          const std::string code =
              join_->mode == AccountJoin::Mode::kLink
                  ? NormalizeLinkCode(join_->code)
                  : NormalizeRecoveryCode(join_->code);
          if (code.empty()) {
            halted_ = true;
            notify = [join_failed = events_.join_failed] {
              if (join_failed) join_failed("bad_request");
            };
          } else {
            socket_->Send(
                join_->mode == AccountJoin::Mode::kLink
                    ? EncodeLink(key_, LobbyHost(lobby_), code, m->nonce,
                                 device_)
                    : EncodeRecover(key_, LobbyHost(lobby_), code, m->nonce,
                                    device_));
          }
        } else if (joined_ || sign_in_only_) {
          // The key is the account's now (or has to be): a registered name
          // could rename it, and registering could make another account.
          socket_->Send(EncodeAuth(key_, LobbyHost(lobby_), xuid_, m->nonce,
                                   device_, appear_offline_, sign_in_only_));
        } else {
          // Registering signs in when this key owns the XUID already.
          socket_->Send(EncodeRegister(key_, LobbyHost(lobby_), xuid_,
                                       gamertag_, m->nonce, device_,
                                       appear_offline_));
        }
        break;
      case Kind::kMoved: {
        // Another key owns the XUID: sign in on the new one. The lobby
        // closes this socket next. After a link or recovery, that's the
        // account the device joined.
        if (join_) {
          XELOGI("Live account: this device joined an account");
          joined_ = true;
          join_.reset();
        } else {
          XELOGW("Live account: the online XUID belongs to someone else, so "
                 "the service gave this profile another");
        }
        xuid_ = m->xuid;
        failures_ = 0;
        notify = [moved = events_.moved,
                  recovery_code = events_.recovery_code, xuid = m->xuid,
                  recovery = m->recovery] {
          if (moved) moved(xuid);
          if (recovery_code && !recovery.empty()) recovery_code(recovery);
        };
        break;
      }
      case Kind::kReject:
        XELOGW("Live account: refused ({})", m->reason);
        problem_ = m->reason;
        if (join_) {
          // The code is used up by any attempt that finds it: no second try.
          halted_ = true;
          notify = [join_failed = events_.join_failed, reason = m->reason] {
            if (join_failed) join_failed(reason);
          };
        } else if (m->reason == "removed") {
          halted_ = true;
          notify = [gone = m->deleted ? events_.account_deleted
                                      : events_.removed] {
            if (gone) gone();
          };
        } else if (m->reason == "elsewhere") {
          // Coming back on its own would end the other device's session.
          XELOGI("Live account: signed in on another device ({})",
                 m->device_name.empty() ? "unnamed" : m->device_name);
          halted_ = true;
          notify = [elsewhere = events_.signed_in_elsewhere,
                    device = m->device_name] {
            if (elsewhere) elsewhere(device);
          };
        } else if (m->reason == "limit" && register_refusals_) {
          // The 5th refused gamertag on this socket.
          halted_ = true;
          notify = [request_failed = events_.request_failed] {
            if (request_failed) request_failed("register", "limit");
          };
        } else {
          notify = [connection_changed = events_.connection_changed] {
            if (connection_changed) connection_changed();
          };
        }
        break;
      case Kind::kWelcome: {
        // Not a success yet for the wait before reconnecting: a lobby that
        // refuses what's sent next (presence, say) closes the socket.
        signed_in_ = true;
        signed_in_at_ = std::chrono::steady_clock::now();
        problem_.clear();
        if (m->xuid) {
          if (m->xuid != xuid_) {
            XELOGW("Live account: the service gave this profile another "
                   "online XUID");
          }
          xuid_ = m->xuid;
        }
        code_ = m->code;
        account_gamertag_ = m->gamertag;
        register_refusals_ = 0;
        rename_ = m->rename;
        next_rename_ms_ = m->next_rename_ms;
        refused_ = m->refused;
        devices_.clear();
        friends_ = m->friends;
        for (const auto& f : friends_) {
          RememberNameLocked(f.xuid, f.gamertag);
        }
        blocked_ = m->blocked;
        lobby_has_settings_ = m->settings;
        friend_requests_ = m->requests;
        presence_.clear();
        party_.reset();
        if (!presence_message_.empty()) {
          socket_->Send(presence_message_);
        }
        for (auto& text : pending_) {
          socket_->Send(std::move(text));
        }
        pending_.clear();
        // Names asked for before signing in, or left from the last socket.
        SendNamesLocked();
        XELOGI("Live account: signed in, {} friend(s)", friends_.size());
        notify = [signed_in = events_.signed_in,
                  connection_changed = events_.connection_changed,
                  friends_changed = events_.friends_changed,
                  blocked_changed = events_.blocked_changed,
                  settings_changed = events_.settings_changed,
                  recovery_code = events_.recovery_code, xuid = xuid_,
                  code = code_, recovery = m->recovery,
                  settings = m->settings] {
          if (signed_in) signed_in(xuid, code);
          if (connection_changed) connection_changed();
          if (friends_changed) friends_changed();
          if (blocked_changed) blocked_changed();
          if (settings_changed && settings) settings_changed();
          if (recovery_code && !recovery.empty()) recovery_code(recovery);
        };
        break;
      }
      case Kind::kFriend: {
        const Friend& f = m->friends.front();
        RememberNameLocked(f.xuid, f.gamertag);
        std::erase_if(friends_,
                      [&](const Friend& o) { return o.xuid == f.xuid; });
        if (!m->removed) {
          friends_.push_back(f);
        } else {
          std::erase_if(presence_,
                        [&](const Presence& p) { return p.xuid == f.xuid; });
        }
        notify = [friends_changed = events_.friends_changed] {
          if (friends_changed) friends_changed();
        };
        break;
      }
      case Kind::kPresence: {
        std::erase_if(presence_, [&](const Presence& p) {
          return p.xuid == m->presence.xuid;
        });
        // Friends and party members are a few hundred at most; past that the
        // oldest go, whatever the lobby sends.
        if (presence_.size() >= kMaxPresence) {
          presence_.erase(presence_.begin());
        }
        presence_.push_back(m->presence);
        RememberNameLocked(m->presence.xuid, m->presence.gamertag);
        notify = [presence_changed = events_.presence_changed,
                  p = m->presence] {
          if (presence_changed) presence_changed(p);
        };
        break;
      }
      case Kind::kInvite:
        RememberNameLocked(m->invite.from, m->invite.gamertag);
        if (m->invite.party && party_ && party_->room == m->invite.room) {
          // Into the party it's already in: nothing to offer.
          XELOGI("Live account: a party invite from {} into party {}, which "
                 "it's in; dropped",
                 m->invite.gamertag, m->invite.room);
          break;
        }
        if (!m->invite.party) {
          // The latest from each friend replaces an earlier one, on the lobby
          // too.
          std::erase_if(invites_, [&](const HeldInvite& held) {
            const bool replaced =
                held.invite.id == m->invite.id ||
                (held.invite.from == m->invite.from && !held.invite.party);
            if (replaced && held.invite.id != m->invite.id) {
              ForgetInviteLocked(held.invite.id);
            }
            return replaced;
          });
          // Expired ones go now, and the oldest past the cap.
          const auto now = std::chrono::steady_clock::now();
          std::erase_if(invites_, [&](const HeldInvite& held) {
            const bool expired = now - held.received >=
                                 std::chrono::seconds(kInviteLifetimeSeconds);
            if (expired) {
              ForgetInviteLocked(held.invite.id);
            }
            return expired;
          });
          if (invites_.size() >= kMaxHeldInvites) {
            ForgetInviteLocked(invites_.front().invite.id);
            invites_.erase(invites_.begin());
          }
          invites_.push_back({m->invite, now});
        }
        notify = [invited = events_.invited, i = m->invite] {
          if (invited) invited(i);
        };
        break;
      case Kind::kParty:
        if (!m->party && party_ && !m->party_room.empty() &&
            m->party_room != party_->room) {
          // Leaving the party it was in, told after the new one's message.
          XELOGI("Live account: left party {} (now in {})", m->party_room,
                 party_->room);
          break;
        }
        party_ = m->party;
        if (party_) {
          for (const auto& [xuid, gamertag] : party_->members) {
            RememberNameLocked(xuid, gamertag);
          }
        }
        // The party a party invite asked for.
        if (party_ && !party_invites_waiting_.empty()) {
          for (const auto& [to, title_id] : party_invites_waiting_) {
            XELOGI("Live account: party made, inviting {:016X}", to);
            SendLocked(EncodePartyInvite(to, title_id));
          }
          party_invites_waiting_.clear();
        }
        notify = [party_changed = events_.party_changed] {
          if (party_changed) party_changed();
        };
        break;
      case Kind::kInviteAnswered:
        // Another of the account's apps answered it.
        std::erase_if(invites_, [&](const HeldInvite& held) {
          return held.invite.id == m->invite_id;
        });
        ForgetInviteLocked(m->invite_id);
        notify = [invite_answered = events_.invite_answered,
                  id = m->invite_id] {
          if (invite_answered) invite_answered(id);
        };
        break;
      case Kind::kInviteState:
        notify = [invite_state = events_.invite_state, id = m->invite_id,
                  state = m->state] {
          if (invite_state) invite_state(id, state);
        };
        break;
      case Kind::kBlocked:
        blocked_ = m->blocked;
        notify = [blocked_changed = events_.blocked_changed] {
          if (blocked_changed) blocked_changed();
        };
        break;
      case Kind::kProfile:
        if (!m->gamertag.empty()) {
          account_gamertag_ = m->gamertag;
        }
        if (!m->code.empty()) {
          code_ = m->code;
        }
        rename_ = m->rename;
        next_rename_ms_ = m->next_rename_ms;
        notify = [profile_changed = events_.profile_changed] {
          if (profile_changed) profile_changed();
        };
        break;
      case Kind::kChecked:
        notify = [name_checked = events_.name_checked,
                  c = m->checked] {
          if (name_checked) name_checked(c.gamertag, c.status, c.suggestions);
        };
        break;
      case Kind::kRecovery:
        notify = [recovery_code = events_.recovery_code,
                  code = m->recovery] {
          if (recovery_code) recovery_code(code);
        };
        break;
      case Kind::kLinkCode:
        notify = [link_code = events_.link_code, code = m->code,
                  ttl = m->link_ttl_s] {
          if (link_code) link_code(code, ttl);
        };
        break;
      case Kind::kJoinPending:
        if (!join_ || join_->mode != AccountJoin::Mode::kRequest) {
          break;
        }
        notify = [join_pending = events_.join_pending, id = m->join_id,
                  number = m->join_number, ttl = m->join_ttl_s] {
          if (join_pending) {
            join_pending(id, number, ttl);
          }
        };
        break;
      case Kind::kJoinRequest:
        if (!signed_in_) {
          break;
        }
        notify = [join_requested = events_.join_requested,
                  request =
                      JoinRequest{m->join_id, m->join_device, m->join_number,
                                  m->join_ttl_s, m->join_scanned}] {
          if (join_requested) {
            join_requested(request);
          }
        };
        break;
      case Kind::kJoinClosed:
        if (!signed_in_) {
          break;
        }
        notify = [join_closed = events_.join_closed, id = m->join_id,
                  reason = m->reason] {
          if (join_closed) {
            join_closed(id, reason);
          }
        };
        break;
      case Kind::kSessionToken:
        session_token_ = m->token;
        session_token_expires_ = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(m->token_ttl_s);
        notify = [session_token = events_.session_token, token = m->token,
                  ttl = m->token_ttl_s] {
          if (session_token) session_token(token, ttl);
        };
        break;
      case Kind::kDevices:
        devices_ = m->devices;
        notify = [devices_changed = events_.devices_changed] {
          if (devices_changed) devices_changed();
        };
        break;
      case Kind::kNames:
      case Kind::kFound:
        for (const auto& [xuid, gamertag] : m->users) {
          RememberNameLocked(xuid, gamertag);
        }
        if (m->kind == Kind::kNames) {
          notify = [names = events_.names, users = m->users] {
            if (names) names(users);
          };
        }
        break;
      case Kind::kResults:
        for (const auto& [xuid, gamertag] : m->users) {
          RememberNameLocked(xuid, gamertag);
        }
        notify = [results = events_.search_results, prefix = m->prefix,
                  users = m->users] {
          if (results) results(prefix, users);
        };
        break;
      case Kind::kSettings:
        lobby_has_settings_ = true;
        friend_requests_ = m->requests;
        notify = [settings_changed = events_.settings_changed] {
          if (settings_changed) settings_changed();
        };
        break;
      case Kind::kReported:
        XELOGI("Live account: report {} of {:016X} stored", m->report_id,
               m->xuid);
        notify = [reported = events_.reported, id = m->report_id,
                  xuid = m->xuid] {
          if (reported) reported(id, xuid);
        };
        break;
      case Kind::kError:
        if (m->for_request == "invite_reply" && m->reason == "not_found") {
          // Answered or dropped elsewhere meanwhile: nothing went wrong here.
          XELOGI("Live account: the invite was already gone");
          break;
        }
        if (m->for_request == "invite" && m->reason == "no_party" && m->to) {
          const bool unsure = std::any_of(
              party_invites_unsure_.begin(), party_invites_unsure_.end(),
              [&](const auto& u) { return u.first == m->to; });
          if (unsure) {
            // Not in a party after all (the lobby keeps it across a
            // relaunch, so the client asked rather than made one).
            PartyInviteRefusedLocked(m->to);
            break;
          }
        }
        XELOGW("Live account: the service refused a request ({}: {})",
               m->for_request, m->reason);
        if (m->for_request == "party_create") {
          // Those invites have no party to go to; the app says why.
          party_invites_waiting_.clear();
        }
        if (m->for_request == "register" && !signed_in_ &&
            (m->reason == "taken" || m->reason == "invalid" ||
             m->reason == "reserved" || m->reason == "not_allowed")) {
          ++register_refusals_;
          notify = [register_refused = events_.register_refused,
                    gamertag = m->gamertag.empty() ? gamertag_ : m->gamertag,
                    reason = m->reason] {
            if (register_refused) register_refused(gamertag, reason);
          };
        } else if (m->for_request == "rename" || m->for_request == "profile") {
          notify = [rename_failed = events_.rename_failed,
                    reason = m->reason, next = m->next_ms] {
            if (rename_failed) rename_failed(reason, next);
          };
        } else if (m->for_request == "names") {
          // Only "limit" is expected (the lobby's 30 a minute, shared with
          // friend requests by gamertag): wait a minute, then ask again for
          // the last request's players. The app shows nothing for this.
          if (m->reason == "limit") {
            names_paused_until_ =
                std::chrono::steady_clock::now() + std::chrono::minutes(1);
            std::vector<uint64_t> again;
            for (uint64_t xuid : names_last_) {
              names_asked_.erase(xuid);
              if (!names_.count(xuid)) {
                again.push_back(xuid);
              }
            }
            names_queue_.insert(names_queue_.begin(), again.begin(),
                                again.end());
            names_last_.clear();
            names_wake_ = true;
            changed_.notify_all();
          }
        } else if (m->for_request == "friend_add" &&
                   m->reason == "not_allowed") {
          // The player takes requests by friend code only.
          std::string gamertag = m->gamertag;
          if (gamertag.empty() && m->xuid) {
            auto it = names_.find(m->xuid);
            if (it != names_.end()) {
              gamertag = it->second;
            }
          }
          notify = [friend_add_refused = events_.friend_add_refused,
                    xuid = m->xuid, gamertag = std::move(gamertag),
                    reason = m->reason] {
            if (friend_add_refused) friend_add_refused(xuid, gamertag, reason);
          };
        } else if (m->for_request == "report") {
          notify = [report_failed = events_.report_failed, xuid = m->xuid,
                    reason = m->reason] {
            if (report_failed) report_failed(xuid, reason);
          };
        } else {
          notify = [request_failed = events_.request_failed,
                    request = m->for_request, reason = m->reason] {
            if (request_failed) request_failed(request, reason);
          };
        }
        break;
      default:
        break;
    }
  }
  if (notify) {
    notify();
  }
}

void AccountClient::OnClosed(std::string reason) {
  XELOGI("Live account: connection closed ({})", reason);
  const bool lasted =
      signed_in_ && std::chrono::steady_clock::now() - signed_in_at_ >=
                        std::chrono::seconds(30);
  signed_in_ = false;
  problem_ = reason;
  closed_ = true;
  if (reason.find("4007") != std::string::npos ||
      reason.find("moved") != std::string::npos) {
    // At once, on the XUID the lobby moved the account to.
  } else if (lasted) {
    failures_ = 1;
  } else {
    ++failures_;
  }
  ++generation_;
  changed_.notify_all();
}

void AccountClient::Run() {
  using namespace std::chrono;
  std::unique_lock lock(mutex_);
  EnsureOpenLocked();
  last_ping_ = steady_clock::now();
  while (!stopping_) {
    // A ping every 20 s, and sooner when queued names may go.
    names_wake_ = false;
    steady_clock::duration wait = seconds(20) - (steady_clock::now() - last_ping_);
    if (auto names = NamesWaitLocked()) {
      wait = std::min<steady_clock::duration>(wait, *names);
    }
    wait = std::max<steady_clock::duration>(wait, milliseconds(10));
    changed_.wait_for(lock, wait, [this] {
      return stopping_ || closed_ || names_wake_ || network_changed_;
    });
    if (stopping_) {
      break;
    }
    if (std::exchange(network_changed_, false)) {
      if (!NetworkPathAvailable()) {
        // Closed now rather than when pings stop being answered. Not a
        // failure: it says nothing about the lobby.
        if (socket_ && !closed_) {
          XELOGI("Live account: no network, so disconnecting");
          signed_in_ = false;
          problem_.clear();
          closed_ = true;
          failures_ = 0;
          ++generation_;
        }
      } else if (!halted_) {
        // Whatever the wait had grown to, at once. A halt (refused, removed,
        // signed in elsewhere) keeps its reason.
        failures_ = 0;
        problem_.clear();
        if (!closed_) {
          EnsureOpenLocked();
        }
      }
    }
    if (closed_) {
      auto socket = std::move(socket_);
      closed_ = false;
      // 2 s after a socket that lasted, then doubling up to a minute, plus
      // up to a quarter at random so apps don't come back together; at once
      // after a move.
      thread_local std::minstd_rand rng{std::random_device{}()};
      const milliseconds base =
          seconds(std::min<uint32_t>(60, 1u << std::min(failures_, 6u)));
      const milliseconds delay =
          failures_ ? base + milliseconds(rng() % (base.count() / 4 + 1))
                    : milliseconds(0);
      lock.unlock();
      socket.reset();
      if (events_.connection_changed) events_.connection_changed();
      lock.lock();
      if (halted_) {
        continue;
      }
      changed_.wait_for(lock, delay,
                        [this] { return stopping_ || network_changed_; });
      EnsureOpenLocked();
      continue;
    }
    SendNamesLocked();
    if (steady_clock::now() - last_ping_ < seconds(20)) {
      continue;
    }
    last_ping_ = steady_clock::now();
    if (socket_ && signed_in_ && !socket_->Ping()) {
      socket_->Send("ping");
    }
  }
}

}  // namespace xe::net::live
