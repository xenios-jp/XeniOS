/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/relay_protocol.h"

#include <algorithm>
#include <charconv>
#include <random>

#include "third_party/fmt/include/fmt/format.h"

namespace xe::net::relay {
namespace {

constexpr std::string_view kMagic = "XESR";
constexpr size_t kHeaderSize = 6;
constexpr std::string_view kCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// Big endian, as the rest of the wire.
class Writer {
 public:
  explicit Writer(uint8_t type) {
    out_.assign(kMagic.begin(), kMagic.end());
    out_.push_back(kVersion);
    out_.push_back(type);
  }
  void U8(uint8_t value) { out_.push_back(value); }
  void U16(uint16_t value) { Unsigned(value, 2); }
  void U32(uint32_t value) { Unsigned(value, 4); }
  void U64(uint64_t value) { Unsigned(value, 8); }
  void Bytes(std::span<const uint8_t> bytes) {
    out_.insert(out_.end(), bytes.begin(), bytes.end());
  }
  std::vector<uint8_t> Take() { return std::move(out_); }

 private:
  void Unsigned(uint64_t value, int size) {
    for (int i = size - 1; i >= 0; --i) {
      out_.push_back(uint8_t(value >> (i * 8)));
    }
  }
  std::vector<uint8_t> out_;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> data) : data_(data) {}
  bool ok() const { return ok_; }
  bool done() const { return ok_ && data_.empty(); }
  uint8_t U8() { return uint8_t(Unsigned(1)); }
  uint16_t U16() { return uint16_t(Unsigned(2)); }
  uint32_t U32() { return uint32_t(Unsigned(4)); }
  uint64_t U64() { return Unsigned(8); }
  template <size_t N>
  std::array<uint8_t, N> Bytes() {
    std::array<uint8_t, N> bytes{};
    if (Need(N)) {
      std::copy_n(data_.begin(), N, bytes.begin());
      data_ = data_.subspan(N);
    }
    return bytes;
  }
  std::vector<uint8_t> Rest() {
    std::vector<uint8_t> rest(data_.begin(), data_.end());
    data_ = {};
    return rest;
  }

 private:
  bool Need(size_t size) {
    ok_ &= data_.size() >= size;
    return ok_;
  }
  uint64_t Unsigned(size_t size) {
    if (!Need(size)) {
      return 0;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) {
      value = value << 8 | data_[i];
    }
    data_ = data_.subspan(size);
    return value;
  }
  std::span<const uint8_t> data_;
  bool ok_ = true;
};

struct EncodeVisitor {
  Writer& w;
  void operator()(const Frame& m) {
    w.U64(m.key);
    w.Bytes(m.source);
    w.Bytes(m.destination);
    w.U8(m.index);
    w.U8(m.count);
    w.U16(m.id);
    w.Bytes(m.payload);
  }
  void operator()(const Probe& m) {
    w.U64(m.room_secret);
    w.Bytes(m.from);
    w.Bytes(m.to);
    w.U64(m.nonce);
    w.U8(m.reply ? 1 : 0);
  }
};

bool HasMagic(std::span<const uint8_t> datagram) {
  return datagram.size() >= kHeaderSize &&
         std::equal(kMagic.begin(), kMagic.end(), datagram.begin());
}

std::optional<Message> DecodeBody(uint8_t type, Reader& r) {
  Message message;
  switch (type) {
    case 1: {
      Frame m;
      m.key = r.U64();
      m.source = r.Bytes<6>();
      m.destination = r.Bytes<6>();
      m.index = r.U8();
      m.count = r.U8();
      m.id = r.U16();
      m.payload = r.Rest();
      if (!m.count || m.count > kMaxFragments || m.index >= m.count ||
          m.payload.empty() || m.payload.size() > kFragmentPayload) {
        return std::nullopt;
      }
      message = std::move(m);
      break;
    }
    case 2: {
      Probe m;
      m.room_secret = r.U64();
      m.from = r.Bytes<6>();
      m.to = r.Bytes<6>();
      m.nonce = r.U64();
      m.reply = r.U8() != 0;
      message = m;
      break;
    }
    default:
      return std::nullopt;
  }
  if (!r.done()) {
    return std::nullopt;
  }
  return message;
}

Room Named(std::string_view name) {
  Room room{};
  std::copy_n(name.begin(), std::min(name.size(), room.size()), room.begin());
  return room;
}

}  // namespace

std::string ToString(const Endpoint& endpoint) {
  const auto& a = endpoint.address;
  if (!endpoint.v6) {
    return fmt::format("{}.{}.{}.{}:{}", a[0], a[1], a[2], a[3], endpoint.port);
  }
  std::string text = "[";
  for (size_t i = 0; i < 16; i += 2) {
    text += fmt::format("{}{:x}", i ? ":" : "", a[i] << 8 | a[i + 1]);
  }
  return text + fmt::format("]:{}", endpoint.port);
}

uint64_t RandomId() {
  uint64_t id = 0;
  while (!id) {
    Random(std::span(reinterpret_cast<uint8_t*>(&id), sizeof(id)));
  }
  return id;
}

Room PublicLobby(uint32_t title_id) {
  return Named(fmt::format("lobby:{:08X}", title_id));
}

Room PrivateRoom(std::string_view code) {
  return Named(fmt::format("room:{}", code));
}

std::string RoomName(const Room& room) {
  auto end = std::find(room.begin(), room.end(), 0);
  return std::string(room.begin(), end);
}

std::string RoomPath(const Room& room) {
  std::string path = RoomName(room);
  std::replace(path.begin(), path.end(), ':', '/');
  return path;
}

std::optional<Endpoint> ParseEndpoint(std::string_view text) {
  Endpoint endpoint;
  std::string_view port;
  if (text.starts_with('[')) {
    const size_t close = text.find("]:");
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view address = text.substr(1, close - 1);
    port = text.substr(close + 2);
    // Up to 8 groups of hex digits, with at most one :: standing for zeros.
    std::vector<uint16_t> head, tail;
    std::vector<uint16_t>* groups = &head;
    size_t i = 0;
    if (address.starts_with("::")) {
      groups = &tail;
      i = 2;
    }
    while (i < address.size()) {
      uint32_t group = 0;
      size_t digits = 0;
      for (; i < address.size() && address[i] != ':'; ++i, ++digits) {
        const char c = address[i];
        const int value = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (value < 0 || digits == 4) {
          return std::nullopt;
        }
        group = group << 4 | uint32_t(value);
      }
      if (!digits) {
        return std::nullopt;
      }
      groups->push_back(uint16_t(group));
      if (i < address.size()) {
        ++i;
        if (i < address.size() && address[i] == ':') {
          if (groups == &tail) {
            return std::nullopt;
          }
          groups = &tail;
          ++i;
        } else if (i == address.size()) {
          return std::nullopt;
        }
      }
    }
    const size_t count = head.size() + tail.size();
    if (count > 8 || (groups == &head && count != 8)) {
      return std::nullopt;
    }
    head.resize(8 - tail.size());
    head.insert(head.end(), tail.begin(), tail.end());
    for (size_t g = 0; g < 8; ++g) {
      endpoint.address[g * 2] = uint8_t(head[g] >> 8);
      endpoint.address[g * 2 + 1] = uint8_t(head[g]);
    }
    endpoint.v6 = true;
    // ::ffff:a.b.c.d is a.b.c.d (the dual-stack socket sends it there), so the
    // loopback and private-network checks must see the IPv4 address.
    if (std::all_of(endpoint.address.begin(), endpoint.address.begin() + 10,
                    [](uint8_t b) { return !b; }) &&
        endpoint.address[10] == 0xFF && endpoint.address[11] == 0xFF) {
      std::copy_n(endpoint.address.begin() + 12, 4, endpoint.address.begin());
      std::fill(endpoint.address.begin() + 4, endpoint.address.end(), 0);
      endpoint.v6 = false;
    }
  } else {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
      return std::nullopt;
    }
    std::string_view address = text.substr(0, colon);
    port = text.substr(colon + 1);
    for (size_t b = 0; b < 4; ++b) {
      const size_t dot = b < 3 ? address.find('.') : address.size();
      if (dot == std::string_view::npos) {
        return std::nullopt;
      }
      uint32_t value = 0;
      auto [end, ec] =
          std::from_chars(address.data(), address.data() + dot, value);
      if (ec != std::errc() || end != address.data() + dot || !dot ||
          value > 255) {
        return std::nullopt;
      }
      endpoint.address[b] = uint8_t(value);
      address.remove_prefix(std::min(dot + 1, address.size()));
    }
  }
  uint32_t value = 0;
  auto [end, ec] =
      std::from_chars(port.data(), port.data() + port.size(), value);
  if (ec != std::errc() || end != port.data() + port.size() || !value ||
      value > 0xFFFF) {
    return std::nullopt;
  }
  endpoint.port = uint16_t(value);
  return endpoint;
}

std::string NewRoomCode() {
  std::random_device random;
  std::uniform_int_distribution<size_t> pick(0, kCodeAlphabet.size() - 1);
  std::string code;
  for (size_t i = 0; i < kRoomCodeLength; ++i) {
    code += kCodeAlphabet[pick(random)];
  }
  return code;
}

std::optional<std::string> NormalizeRoomCode(std::string_view text) {
  std::string code;
  for (char c : text) {
    if (c == ' ' || c == '-') {
      continue;
    }
    if (c >= 'a' && c <= 'z') {
      c = char(c - 'a' + 'A');
    }
    if (c == 'O') {
      c = '0';
    } else if (c == 'I' || c == 'L') {
      c = '1';
    }
    if (kCodeAlphabet.find(c) == std::string_view::npos ||
        code.size() == kRoomCodeLength) {
      return std::nullopt;
    }
    code += c;
  }
  if (code.size() != kRoomCodeLength) {
    return std::nullopt;
  }
  return code;
}

std::string_view ToString(Reject reason) {
  switch (reason) {
    case Reject::kFull:
      return "the room is full";
    case Reject::kDuplicateMac:
      return "another player in the room has the same profile";
    case Reject::kVersion:
      return "the lobby runs another version";
    case Reject::kBadRequest:
      return "the lobby didn't understand this player";
  }
  return "unknown reason";
}

std::optional<Reject> ParseReject(std::string_view name) {
  if (name == "full") {
    return Reject::kFull;
  }
  if (name == "duplicate_mac") {
    return Reject::kDuplicateMac;
  }
  if (name == "version") {
    return Reject::kVersion;
  }
  if (name == "bad_request") {
    return Reject::kBadRequest;
  }
  return std::nullopt;
}

std::vector<uint8_t> Encode(const Message& message) {
  Writer w(uint8_t(message.index() + 1));
  std::visit(EncodeVisitor{w}, message);
  return w.Take();
}

std::optional<Message> Decode(std::span<const uint8_t> datagram) {
  if (!HasMagic(datagram)) {
    return std::nullopt;
  }
  if (datagram[4] != kVersion) {
    return std::nullopt;
  }
  Reader r(datagram.subspan(kHeaderSize));
  return DecodeBody(datagram[5], r);
}

std::vector<Frame> Fragment(std::span<const uint8_t> ethernet, uint64_t key,
                            uint16_t id) {
  constexpr size_t kHeader = 14;
  if (ethernet.size() < kHeader ||
      ethernet.size() > kFragmentPayload * kMaxFragments) {
    return {};
  }
  const size_t count =
      (ethernet.size() + kFragmentPayload - 1) / kFragmentPayload;
  std::vector<Frame> fragments(count);
  for (size_t i = 0; i < count; ++i) {
    Frame& f = fragments[i];
    f.key = key;
    std::copy_n(ethernet.begin(), 6, f.destination.begin());
    std::copy_n(ethernet.begin() + 6, 6, f.source.begin());
    f.index = uint8_t(i);
    f.count = uint8_t(count);
    f.id = id;
    auto part = ethernet.subspan(i * kFragmentPayload);
    part = part.first(std::min(part.size(), kFragmentPayload));
    f.payload.assign(part.begin(), part.end());
  }
  return fragments;
}

std::optional<std::vector<uint8_t>> Reassembler::Add(Frame&& fragment,
                                                     uint64_t now_ms) {
  if (fragment.count == 1) {
    return std::move(fragment.payload);
  }
  std::erase_if(pending_, [&](const Pending& p) {
    return now_ms - p.started_ms > kTimeoutMs;
  });
  auto found = std::find_if(pending_.begin(), pending_.end(), [&](auto& p) {
    return p.source == fragment.source && p.id == fragment.id;
  });
  if (found == pending_.end()) {
    if (pending_.size() >= kLimit) {
      pending_.erase(pending_.begin());
    }
    Pending p;
    p.source = fragment.source;
    p.id = fragment.id;
    p.count = fragment.count;
    p.started_ms = now_ms;
    pending_.push_back(std::move(p));
    found = pending_.end() - 1;
  }
  Pending& p = *found;
  if (p.count != fragment.count || !p.parts[fragment.index].empty()) {
    return std::nullopt;
  }
  p.parts[fragment.index] = std::move(fragment.payload);
  if (++p.received < p.count) {
    return std::nullopt;
  }
  std::vector<uint8_t> whole;
  for (uint8_t i = 0; i < p.count; ++i) {
    whole.insert(whole.end(), p.parts[i].begin(), p.parts[i].end());
  }
  pending_.erase(found);
  return whole;
}

}  // namespace xe::net::relay
